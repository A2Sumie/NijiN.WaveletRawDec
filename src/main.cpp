// SPDX-License-Identifier: Apache-2.0
#include "../include/nijinWVDC.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

bool path_is_dir(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string parent_dir(const std::string& p) {
    size_t pos = p.find_last_of('/');
    if (pos == std::string::npos) return "";
    if (pos == 0) return "/";
    return p.substr(0, pos);
}

bool mkdir_p(const std::string& path, std::string& err) {
    if (path.empty() || path == "." || path == "/") return true;
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur.push_back(path[i]);
        if (path[i] == '/' || i + 1 == path.size()) {
            if (cur == "/") continue;
            struct stat st{};
            if (stat(cur.c_str(), &st) == 0) {
                if (!S_ISDIR(st.st_mode)) {
                    err = cur + " exists and is not a directory";
                    return false;
                }
            } else if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
                err = std::strerror(errno);
                return false;
            }
        }
    }
    return true;
}

std::string join_path(const std::string& dir, const char* file) {
    if (dir.empty()) return file;
    return (dir.back() == '/') ? (dir + file) : (dir + "/" + file);
}

bool ends_with_ci(const std::string& s, const char* suf) {
    size_t n = std::strlen(suf);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = s[s.size() - n + i];
        char b = suf[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        } else if (c == '\n') {
            out += "\\n";
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

bool parse_frame_spec(const std::string& spec, size_t total_frames, std::vector<uint32_t>& out, std::string& err) {
    out.clear();
    if (total_frames == 0) {
        err = "Clip has 0 video frames";
        return false;
    }
    if (spec == "all") {
        out.reserve(total_frames);
        for (size_t i = 0; i < total_frames; ++i) out.push_back(static_cast<uint32_t>(i));
        return true;
    }
    size_t dash = spec.find('-');
    if (dash == std::string::npos) {
        char* end = nullptr;
        long idx = std::strtol(spec.c_str(), &end, 10);
        if (spec.empty() || !end || *end != '\0' || idx < 0 || static_cast<size_t>(idx) >= total_frames) {
            err = "Frame index out of range: " + spec + " (valid: 0.." + std::to_string(total_frames - 1) + ")";
            return false;
        }
        out.push_back(static_cast<uint32_t>(idx));
        return true;
    }
    std::string a_str = spec.substr(0, dash);
    std::string b_str = spec.substr(dash + 1);
    char* end_a = nullptr;
    char* end_b = nullptr;
    long a = std::strtol(a_str.c_str(), &end_a, 10);
    long b = std::strtol(b_str.c_str(), &end_b, 10);
    if (a_str.empty() || b_str.empty() || !end_a || *end_a != '\0' || !end_b || *end_b != '\0' ||
        a < 0 || b < a || static_cast<size_t>(b) >= total_frames) {
        err = "Invalid frame range: " + spec + " (valid: 0.." + std::to_string(total_frames - 1) + ")";
        return false;
    }
    for (long i = a; i <= b; ++i) {
        out.push_back(static_cast<uint32_t>(i));
    }
    return true;
}

bool write_raw_file(const std::string& path, const void* data, size_t bytes, std::string& err) {
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) {
        err = "Cannot open output for writing: " + path;
        return false;
    }
    bool ok = (bytes == 0) || (std::fwrite(data, 1, bytes, fp) == bytes);
    if (std::fclose(fp) != 0) ok = false;
    if (!ok) {
        err = "Failed writing file: " + path;
        return false;
    }
    return true;
}

bool read_file_bytes(const std::string& path, std::vector<uint8_t>& out, std::string& err) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        err = "Cannot open input file: " + path;
        return false;
    }
    std::fseek(fp, 0, SEEK_END);
    long sz = std::ftell(fp);
    if (sz <= 0) {
        std::fclose(fp);
        err = "Input file is empty: " + path;
        return false;
    }
    std::fseek(fp, 0, SEEK_SET);
    out.resize(static_cast<size_t>(sz));
    size_t rd = std::fread(out.data(), 1, out.size(), fp);
    std::fclose(fp);
    if (rd != out.size()) {
        err = "Short read on input file: " + path;
        return false;
    }
    return true;
}

void print_info_common_header(const nijinwvdc::NevClipInfo& clip, const nijinwvdc::NevFrameMeta& m0) {
    std::printf("File:          %s (%.2f MB)\n", clip.file_path.c_str(), clip.file_size / (1024.0 * 1024.0));
    std::printf("Brand:         %s\n", clip.major_brand.c_str());
    std::printf("Camera:        %s / %s\n", clip.make.c_str(), clip.model.c_str());
    std::printf("Firmware:      %s\n", clip.firmware_version.c_str());
    std::printf("Lens:          %s\n", clip.lens_model.c_str());
    std::printf("DateTime:      %s\n", clip.datetime.c_str());
    (void)m0;
}

void print_info_common_tail(const nijinwvdc::NevFrameMeta& m0) {
    if (m0.has_crop) {
        std::printf("Active Crop:   [%u, %u, %u, %u]\n", m0.crop[0], m0.crop[1], m0.crop[2], m0.crop[3]);
    }
    std::printf("Orientation:   %u\n", m0.orientation);
    std::printf("Exposure:      %.4g s, f/%.2f, ISO %u, %.1f mm\n",
                m0.exposure_time_s, m0.fnumber, m0.iso, m0.focal_length_mm);
    if (!m0.picture_control_base.empty() || m0.nef_compression != 0) {
        std::printf("PictureCtrl:   base='%s' name='%s' NEFCompression=%u\n",
                    m0.picture_control_base.c_str(), m0.picture_control_name.c_str(), m0.nef_compression);
    }
    std::printf("BlackLevel:    [%u, %u, %u, %u]\n", m0.black[0], m0.black[1], m0.black[2], m0.black[3]);
    if (m0.white_level > 0) {
        std::printf("WhiteLevel:    %u\n", m0.white_level);
    }
    std::printf("WB (R,G0,G1,B): [%.6f, %.6f, %.6f, %.6f] (mode=%s, %u K)\n",
                m0.wb_rggb[0], m0.wb_rggb[1], m0.wb_rggb[2], m0.wb_rggb[3], m0.wb_mode.c_str(), m0.color_temp);
}

void print_info_human(const nijinwvdc::NevClipInfo& clip, const nijinwvdc::NevFrameMeta& m0, const nijinwvdc::TicoRawInfo& jxs) {
    print_info_common_header(clip, m0);
    if (clip.major_brand == "NEF") {
        const char* fmt_str = (!clip.tracks.empty() && !clip.tracks[0].format.empty())
                                  ? clip.tracks[0].format.c_str()
                                  : "HE/HE*";
        std::printf("Image Track:   Nikon %s (TIFF Compression=34713), codestream=%ux%u\n",
                    fmt_str, jxs.width, jxs.height);
    } else {
        std::printf("Video Track:   %zu frames @ %.4f fps, codestream=%ux%u\n",
                    clip.video_samples.size(), clip.fps, jxs.width, jxs.height);
    }
    print_info_common_tail(m0);
    std::printf("TicoRAW:       Lcod=%u B, in_bd=%d out_bd=%d cdt_depth=%d gamma=%d(type=%d, off16=%d, r_flip=%d)\n",
                jxs.lcod, jxs.in_bitdepth, jxs.out_bitdepth, jxs.cdt_depth,
                jxs.gamma_enabled ? 1 : 0, jxs.gamma_type, jxs.offset_16, jxs.r_flip);
    std::printf("Wavelet/Tile:  5/3 DWT H=%u V=%u, Yh skip=%d, %d slices (%d precincts), line_stride=%d\n",
                jxs.ndecomp_h, jxs.ndecomp_v, (jxs.yh_hor_ndecomp == 0 && jxs.yh_ver_ndecomp == 0) ? 1 : 0,
                jxs.num_slices, jxs.num_precincts, jxs.line_stride);
    for (size_t i = 0; i < clip.tracks.size(); ++i) {
        const auto& tr = clip.tracks[i];
        if (tr.handler_type == "soun") {
            std::printf("Audio Track:   %s %.0f Hz, %u ch, %u-bit (%u samples)\n",
                        tr.format.c_str(), tr.audio_sample_rate, tr.audio_channels,
                        tr.audio_bits_per_channel, tr.sample_count);
        } else if (tr.handler_type == "tmcd") {
            std::printf("Timecode:      %s ts=%u dur=%u nf=%u\n",
                        tr.format.c_str(), tr.tmcd_timescale, tr.tmcd_frame_duration, tr.tmcd_num_frames);
        }
    }
    if (clip.ncvw_size > 0) {
        std::printf("Preview JPEG:  NCVW %u B @ 0x%llx, NCTH %u B @ 0x%llx\n",
                    clip.ncvw_size, static_cast<unsigned long long>(clip.ncvw_offset),
                    clip.ncth_size, static_cast<unsigned long long>(clip.ncth_offset));
    }
}

void print_info_arw6_human(const nijinwvdc::NevClipInfo& clip, const nijinwvdc::NevFrameMeta& m0, const nijinwvdc::Arw6Info& arw6) {
    print_info_common_header(clip, m0);
    std::printf("Image Track:   Sony ARW6 (Compressed HQ / Compressed, TIFF Compression=32766, SonyRawFileType=%u), sensor=%ux%u\n",
                m0.sony_raw_file_type, arw6.width, arw6.height);
    print_info_common_tail(m0);
    std::printf("ARW6 Codec:    A000 %ux%u, strip=%u B, tiles=%zu, decomp=%u, sub_mask=0x%04x, nlt=%u->15->%u-bit\n",
                arw6.width, arw6.height, arw6.codestream_size, arw6.tiles.size(),
                arw6.decomp_levels, arw6.subsampling_mask, arw6.internal_depth, arw6.bit_depth);
    if (clip.ncvw_size > 0 || clip.ncth_size > 0) {
        std::printf("Preview JPEG:  Preview %u B @ 0x%llx, Thumb %u B @ 0x%llx\n",
                    clip.ncvw_size, static_cast<unsigned long long>(clip.ncvw_offset),
                    clip.ncth_size, static_cast<unsigned long long>(clip.ncth_offset));
    }
}

void print_info_json_prefix(const nijinwvdc::NevClipInfo& clip, const nijinwvdc::NevFrameMeta& m0, uint32_t cs_w, uint32_t cs_h) {
    std::printf("{\n");
    std::printf("  \"file\": \"%s\",\n", json_escape(clip.file_path).c_str());
    std::printf("  \"file_size\": %llu,\n", static_cast<unsigned long long>(clip.file_size));
    std::printf("  \"major_brand\": \"%s\",\n", json_escape(clip.major_brand).c_str());
    std::printf("  \"make\": \"%s\",\n", json_escape(clip.make).c_str());
    std::printf("  \"model\": \"%s\",\n", json_escape(clip.model).c_str());
    std::printf("  \"firmware\": \"%s\",\n", json_escape(clip.firmware_version).c_str());
    std::printf("  \"lens\": \"%s\",\n", json_escape(clip.lens_model).c_str());
    std::printf("  \"frames\": %zu,\n", clip.video_samples.size());
    std::printf("  \"fps\": %.6f,\n", clip.fps);
    std::printf("  \"codestream_width\": %u,\n", cs_w);
    std::printf("  \"codestream_height\": %u,\n", cs_h);
    std::printf("  \"active_crop\": [%u, %u, %u, %u],\n", m0.crop[0], m0.crop[1], m0.crop[2], m0.crop[3]);
    std::printf("  \"orientation\": %u,\n", m0.orientation);
    std::printf("  \"iso\": %u,\n", m0.iso);
    std::printf("  \"exposure_time_s\": %.6f,\n", m0.exposure_time_s);
    std::printf("  \"fnumber\": %.2f,\n", m0.fnumber);
    std::printf("  \"black_level\": [%u, %u, %u, %u],\n", m0.black[0], m0.black[1], m0.black[2], m0.black[3]);
    std::printf("  \"wb_rggb\": [%.8f, %.8f, %.8f, %.8f],\n", m0.wb_rggb[0], m0.wb_rggb[1], m0.wb_rggb[2], m0.wb_rggb[3]);
    std::printf("  \"picture_control_base\": \"%s\",\n", json_escape(m0.picture_control_base).c_str());
    std::printf("  \"nef_compression\": %u,\n", m0.nef_compression);
}

void print_info_json(const nijinwvdc::NevClipInfo& clip, const nijinwvdc::NevFrameMeta& m0, const nijinwvdc::TicoRawInfo& jxs) {
    print_info_json_prefix(clip, m0, jxs.width, jxs.height);
    std::printf("  \"ticoraw\": {\n");
    std::printf("    \"lcod\": %u,\n", jxs.lcod);
    std::printf("    \"in_bitdepth\": %d,\n", jxs.in_bitdepth);
    std::printf("    \"out_bitdepth\": %d,\n", jxs.out_bitdepth);
    std::printf("    \"cdt_depth\": %d,\n", jxs.cdt_depth);
    std::printf("    \"gamma_enabled\": %s,\n", jxs.gamma_enabled ? "true" : "false");
    std::printf("    \"gamma_type\": %d,\n", jxs.gamma_type);
    std::printf("    \"offset_16\": %d,\n", jxs.offset_16);
    std::printf("    \"r_flip\": %d,\n", jxs.r_flip);
    std::printf("    \"slices\": %d,\n", jxs.num_slices);
    std::printf("    \"precincts\": %d\n", jxs.num_precincts);
    std::printf("  }\n");
    std::printf("}\n");
}

void print_info_arw6_json(const nijinwvdc::NevClipInfo& clip, const nijinwvdc::NevFrameMeta& m0, const nijinwvdc::Arw6Info& arw6) {
    print_info_json_prefix(clip, m0, arw6.width, arw6.height);
    std::printf("  \"arw6\": {\n");
    std::printf("    \"strip_bytes\": %u,\n", arw6.codestream_size);
    std::printf("    \"tiles\": %zu,\n", arw6.tiles.size());
    std::printf("    \"bit_depth\": %u,\n", arw6.bit_depth);
    std::printf("    \"internal_depth\": %u,\n", arw6.internal_depth);
    std::printf("    \"decomp_levels\": %u,\n", arw6.decomp_levels);
    std::printf("    \"subsampling_mask\": \"0x%04x\",\n", arw6.subsampling_mask);
    std::printf("    \"sony_raw_file_type\": %u\n", m0.sony_raw_file_type);
    std::printf("  }\n");
    std::printf("}\n");
}

void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage:\n"
        "  %s --info [--json] <in.NEV | in.NEF | in.ARW | in.jxs>\n"
        "  %s <in.NEV | in.NEF | in.ARW> [--frame <N|start-end|all>] -o <out.dng|out.raw|dir/> [options]\n"
        "  %s --raw-jxs <in.jxs> -o <out.raw|out.dng> [options]\n"
        "  %s --extract-jxs <in.NEV | in.NEF> [--frame <N>] -o <out.jxs>\n"
        "  %s --extract-preview <in.NEV | in.NEF | in.ARW> [--thumb] -o <out.jpg>\n"
        "  %s --bench <in.NEV | in.ARW> [--frames N] [--threads N]\n\n"
        "Options:\n"
        "  --format dng|raw        Force output format when writing to a directory\n"
        "  --active-crop           Crop sensor margins to camera active crop area\n"
        "  --orientation <1..8>    Override TIFF Orientation\n"
        "  --threads <N>           Worker threads (default: hardware concurrency)\n",
        prog, prog, prog, prog, prog, prog);
}

} // namespace

int main(int argc, char** argv) {
    bool info_mode = false;
    bool json_mode = false;
    bool raw_jxs_mode = false;
    bool extract_jxs_mode = false;
    bool extract_preview_mode = false;
    bool extract_thumb = false;
    bool bench_mode = false;
    bool active_crop = false;

    std::string input_path;
    std::string output_path;
    std::string frame_spec = "0";
    std::string format_override;
    uint16_t override_orientation = 0;
    int threads = 0;
    int bench_frames = 20;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto need_val = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Missing value after %s\n", flag);
                std::exit(2);
            }
            return argv[++i];
        };

        if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (std::strcmp(a, "--info") == 0) {
            info_mode = true;
        } else if (std::strcmp(a, "--json") == 0) {
            json_mode = true;
        } else if (std::strcmp(a, "--raw-jxs") == 0) {
            raw_jxs_mode = true;
        } else if (std::strcmp(a, "--extract-jxs") == 0) {
            extract_jxs_mode = true;
        } else if (std::strcmp(a, "--extract-preview") == 0) {
            extract_preview_mode = true;
        } else if (std::strcmp(a, "--thumb") == 0) {
            extract_thumb = true;
        } else if (std::strcmp(a, "--bench") == 0) {
            bench_mode = true;
        } else if (std::strcmp(a, "--active-crop") == 0) {
            active_crop = true;
        } else if (std::strcmp(a, "-i") == 0 || std::strcmp(a, "--input") == 0) {
            input_path = need_val(a);
        } else if (std::strcmp(a, "-o") == 0 || std::strcmp(a, "--output") == 0) {
            output_path = need_val(a);
        } else if (std::strcmp(a, "--frame") == 0) {
            frame_spec = need_val(a);
        } else if (std::strcmp(a, "--frames") == 0) {
            bench_frames = std::atoi(need_val(a));
        } else if (std::strcmp(a, "--format") == 0) {
            format_override = need_val(a);
        } else if (std::strcmp(a, "--orientation") == 0) {
            override_orientation = static_cast<uint16_t>(std::atoi(need_val(a)));
        } else if (std::strcmp(a, "--threads") == 0) {
            threads = std::atoi(need_val(a));
        } else if (a[0] != '-' && input_path.empty()) {
            input_path = a;
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", a);
            print_usage(argv[0]);
            return 2;
        }
    }

    if (input_path.empty()) {
        print_usage(argv[0]);
        return 2;
    }

    std::string err;

    // Raw .jxs mode (either --raw-jxs or --info on .jxs)
    if (raw_jxs_mode || (info_mode && ends_with_ci(input_path, ".jxs"))) {
        std::vector<uint8_t> cs;
        if (!read_file_bytes(input_path, cs, err)) {
            std::fprintf(stderr, "Error: %s\n", err.c_str());
            return 1;
        }
        nijinwvdc::TicoRawInfo jxs;
        if (info_mode) {
            if (!nijinwvdc::probe_ticoraw_header(cs.data(), cs.size(), jxs, err)) {
                std::fprintf(stderr, "Error: %s\n", err.c_str());
                return 1;
            }
            if (json_mode) {
                std::printf("{\"width\":%u,\"height\":%u,\"lcod\":%u,\"cdt_depth\":%d,\"in_bitdepth\":%d,\"out_bitdepth\":%d,\"gamma_type\":%d,\"slices\":%d,\"precincts\":%d}\n",
                            jxs.width, jxs.height, jxs.lcod, jxs.cdt_depth, jxs.in_bitdepth, jxs.out_bitdepth, jxs.gamma_type, jxs.num_slices, jxs.num_precincts);
            } else {
                std::printf("Raw TicoRAW: %ux%u, Lcod=%u B, cdt_depth=%d, slices=%d, precincts=%d\n",
                            jxs.width, jxs.height, jxs.lcod, jxs.cdt_depth, jxs.num_slices, jxs.num_precincts);
            }
            return 0;
        }
        if (output_path.empty()) {
            std::fprintf(stderr, "Error: --raw-jxs requires -o <output>\n");
            return 2;
        }
        std::vector<uint16_t> bayer;
        if (!nijinwvdc::decode_ticoraw_bayer(cs.data(), cs.size(), threads, bayer, jxs, err)) {
            std::fprintf(stderr, "Decode failed: %s\n", err.c_str());
            return 1;
        }
        if (ends_with_ci(output_path, ".raw") || format_override == "raw") {
            if (!write_raw_file(output_path, bayer.data(), bayer.size() * sizeof(uint16_t), err)) {
                std::fprintf(stderr, "Error: %s\n", err.c_str());
                return 1;
            }
        } else {
            nijinwvdc::NevClipInfo dummy_clip;
            nijinwvdc::NevFrameMeta dummy_meta;
            nijinwvdc::DngWriteOptions opts;
            nijinwvdc::build_dng_options_from_clip_and_frame(dummy_clip, dummy_meta, jxs, false, override_orientation, opts);
            if (!nijinwvdc::write_cfa_dng(output_path, bayer.data(), opts, err)) {
                std::fprintf(stderr, "Error: %s\n", err.c_str());
                return 1;
            }
        }
        return 0;
    }

    // NEV clip modes
    nijinwvdc::NevClipInfo clip;
    if (!nijinwvdc::parse_nev_clip(input_path, clip, err)) {
        std::fprintf(stderr, "Error: %s\n", err.c_str());
        return 1;
    }

    if (info_mode) {
        nijinwvdc::NevFrameMeta m0;
        std::vector<uint8_t> cs0;
        if (!nijinwvdc::read_nev_frame_meta_and_codestream(clip, 0, m0, &cs0, err)) {
            std::fprintf(stderr, "Error: %s\n", err.c_str());
            return 1;
        }
        if (clip.major_brand == "ARW") {
            nijinwvdc::Arw6Info arw6;
            if (!nijinwvdc::probe_arw6_strip(cs0.data(), cs0.size(), arw6, err)) {
                std::fprintf(stderr, "Error: %s\n", err.c_str());
                return 1;
            }
            if (json_mode) {
                print_info_arw6_json(clip, m0, arw6);
            } else {
                print_info_arw6_human(clip, m0, arw6);
            }
            return 0;
        }
        nijinwvdc::TicoRawInfo jxs;
        if (!nijinwvdc::probe_ticoraw_header(cs0.data(), cs0.size(), jxs, err)) {
            std::fprintf(stderr, "Error: %s\n", err.c_str());
            return 1;
        }
        if (json_mode) {
            print_info_json(clip, m0, jxs);
        } else {
            print_info_human(clip, m0, jxs);
        }
        return 0;
    }

    if (extract_preview_mode) {
        if (output_path.empty()) {
            std::fprintf(stderr, "Error: --extract-preview requires -o <out.jpg>\n");
            return 2;
        }
        uint64_t off = extract_thumb ? clip.ncth_offset : clip.ncvw_offset;
        uint32_t sz = extract_thumb ? clip.ncth_size : clip.ncvw_size;
        std::vector<uint8_t> blob;
        if (!nijinwvdc::extract_nev_blob(clip.file_path, off, sz, blob, err) ||
            !write_raw_file(output_path, blob.data(), blob.size(), err)) {
            std::fprintf(stderr, "Error: %s\n", err.c_str());
            return 1;
        }
        std::printf("Extracted %u bytes to %s\n", sz, output_path.c_str());
        return 0;
    }

    if (extract_jxs_mode) {
        if (clip.major_brand == "ARW") {
            std::fprintf(stderr, "Error: --extract-jxs applies to Nikon TicoRAW (.NEV/.NEF), not Sony ARW6\n");
            return 2;
        }
        if (output_path.empty()) {
            std::fprintf(stderr, "Error: --extract-jxs requires -o <out.jxs>\n");
            return 2;
        }
        std::vector<uint32_t> frames;
        if (!parse_frame_spec(frame_spec, clip.video_samples.size(), frames, err) || frames.size() != 1) {
            std::fprintf(stderr, "Error: --extract-jxs requires a single valid --frame index (%s)\n", err.c_str());
            return 2;
        }
        nijinwvdc::NevFrameMeta meta;
        std::vector<uint8_t> cs;
        if (!nijinwvdc::read_nev_frame_meta_and_codestream(clip, frames[0], meta, &cs, err) ||
            !write_raw_file(output_path, cs.data(), cs.size(), err)) {
            std::fprintf(stderr, "Error: %s\n", err.c_str());
            return 1;
        }
        std::printf("Extracted frame %u (%zu bytes) to %s\n", frames[0], cs.size(), output_path.c_str());
        return 0;
    }

    auto decode_clip_sample = [&](const nijinwvdc::NevFrameMeta& meta,
                                  const std::vector<uint8_t>& cs_bytes,
                                  std::vector<uint16_t>& bayer_pix,
                                  nijinwvdc::DngWriteOptions& dopts,
                                  uint32_t& out_w,
                                  uint32_t& out_h) -> bool {
        if (clip.major_brand == "ARW") {
            nijinwvdc::Arw6Info arw6;
            if (!nijinwvdc::decode_arw6_bayer(cs_bytes.data(), cs_bytes.size(), threads, bayer_pix, arw6, err)) {
                return false;
            }
            nijinwvdc::build_dng_options_from_clip_and_frame(clip, meta, arw6, active_crop, override_orientation, dopts);
            out_w = arw6.width;
            out_h = arw6.height;
            return true;
        }
        nijinwvdc::TicoRawInfo jxs;
        if (!nijinwvdc::decode_ticoraw_bayer(cs_bytes.data(), cs_bytes.size(), threads, bayer_pix, jxs, err)) {
            return false;
        }
        nijinwvdc::build_dng_options_from_clip_and_frame(clip, meta, jxs, active_crop, override_orientation, dopts);
        out_w = jxs.width;
        out_h = jxs.height;
        return true;
    };

    if (bench_mode) {
        size_t count = (clip.video_samples.size() == 1)
                           ? static_cast<size_t>(std::max(1, bench_frames))
                           : static_cast<size_t>(std::min<long>(std::max(1, bench_frames), static_cast<long>(clip.video_samples.size())));
        std::vector<uint8_t> cs;
        std::vector<uint16_t> bayer;
        nijinwvdc::NevFrameMeta meta;
        nijinwvdc::DngWriteOptions dopts;
        uint32_t bw = 0, bh = 0;

        // Warmup frame 0
        if (!nijinwvdc::read_nev_frame_meta_and_codestream(clip, 0, meta, &cs, err) ||
            !decode_clip_sample(meta, cs, bayer, dopts, bw, bh)) {
            std::fprintf(stderr, "Benchmark warmup failed: %s\n", err.c_str());
            return 1;
        }

        double dec_ms_total = 0.0;
        auto t_e2e_start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < count; ++i) {
            uint32_t fidx = (clip.video_samples.size() == 1) ? 0u : static_cast<uint32_t>(i);
            if (!nijinwvdc::read_nev_frame_meta_and_codestream(clip, fidx, meta, &cs, err)) {
                std::fprintf(stderr, "Read failed at iter %zu: %s\n", i, err.c_str());
                return 1;
            }
            auto t0 = std::chrono::steady_clock::now();
            if (!decode_clip_sample(meta, cs, bayer, dopts, bw, bh)) {
                std::fprintf(stderr, "Decode failed at iter %zu: %s\n", i, err.c_str());
                return 1;
            }
            auto t1 = std::chrono::steady_clock::now();
            dec_ms_total += std::chrono::duration<double, std::milli>(t1 - t0).count();
        }
        auto t_e2e_end = std::chrono::steady_clock::now();
        double e2e_ms_total = std::chrono::duration<double, std::milli>(t_e2e_end - t_e2e_start).count();
        double avg_dec = dec_ms_total / static_cast<double>(count);
        double avg_e2e = e2e_ms_total / static_cast<double>(count);
        std::printf("Bench (%zu frames, threads=%d, %ux%u): decode=%.2f ms/frame (%.2f fps), read+decode=%.2f ms/frame (%.2f fps)\n",
                    count, threads, bw, bh, avg_dec, 1000.0 / avg_dec, avg_e2e, 1000.0 / avg_e2e);
        return 0;
    }

    if (output_path.empty()) {
        std::fprintf(stderr, "Error: missing -o <output>\n");
        print_usage(argv[0]);
        return 2;
    }

    std::vector<uint32_t> frames;
    if (!parse_frame_spec(frame_spec, clip.video_samples.size(), frames, err)) {
        std::fprintf(stderr, "Error: %s\n", err.c_str());
        return 2;
    }

    bool out_is_dir = (frames.size() > 1) || output_path.back() == '/' || path_is_dir(output_path);
    bool use_raw = (format_override == "raw") || (!out_is_dir && ends_with_ci(output_path, ".raw"));

    if (out_is_dir) {
        if (!mkdir_p(output_path, err)) {
            std::fprintf(stderr, "Error creating output directory %s: %s\n", output_path.c_str(), err.c_str());
            return 1;
        }
    } else {
        std::string parent = parent_dir(output_path);
        if (!parent.empty()) {
            std::string dir_err;
            mkdir_p(parent, dir_err);
        }
    }

    std::vector<uint8_t> cs;
    std::vector<uint16_t> bayer;
    std::vector<uint16_t> cropped;
    for (uint32_t fidx : frames) {
        nijinwvdc::NevFrameMeta meta;
        if (!nijinwvdc::read_nev_frame_meta_and_codestream(clip, fidx, meta, &cs, err)) {
            std::fprintf(stderr, "Error reading frame %u: %s\n", fidx, err.c_str());
            return 1;
        }
        nijinwvdc::DngWriteOptions opts;
        uint32_t fw = 0, fh = 0;
        if (!decode_clip_sample(meta, cs, bayer, opts, fw, fh)) {
            std::fprintf(stderr, "Error decoding frame %u: %s\n", fidx, err.c_str());
            return 1;
        }

        const uint16_t* out_pix = bayer.data();
        if (active_crop && meta.has_crop && meta.crop[2] > 0 && meta.crop[3] > 0) {
            uint8_t new_cfa[4];
            if (!nijinwvdc::apply_bayer_crop(bayer.data(), fw, fh, meta.crop, opts.cfa_pattern, cropped, new_cfa)) {
                std::fprintf(stderr, "Error applying active crop [%u,%u,%u,%u]\n",
                             meta.crop[0], meta.crop[1], meta.crop[2], meta.crop[3]);
                return 1;
            }
            out_pix = cropped.data();
            opts.width = meta.crop[2];
            opts.height = meta.crop[3];
            std::memcpy(opts.cfa_pattern, new_cfa, 4);
            opts.has_crop = false;
        }

        std::string target = output_path;
        if (out_is_dir) {
            char fname[64];
            std::snprintf(fname, sizeof(fname), "frame_%04u.%s", fidx, use_raw ? "raw" : "dng");
            target = join_path(output_path, fname);
        }

        if (use_raw) {
            size_t nbytes = static_cast<size_t>(opts.width) * static_cast<size_t>(opts.height) * sizeof(uint16_t);
            if (!write_raw_file(target, out_pix, nbytes, err)) {
                std::fprintf(stderr, "Error: %s\n", err.c_str());
                return 1;
            }
        } else {
            if (!nijinwvdc::write_cfa_dng(target, out_pix, opts, err)) {
                std::fprintf(stderr, "Error: %s\n", err.c_str());
                return 1;
            }
        }
        std::printf("Wrote frame %u (%ux%u, %s) -> %s\n",
                    fidx, opts.width, opts.height, use_raw ? "raw" : "dng", target.c_str());
    }
    return 0;
}
