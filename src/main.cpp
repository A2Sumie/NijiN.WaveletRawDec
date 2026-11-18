// SPDX-License-Identifier: Apache-2.0
#include "../include/nijinWVDC.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <direct.h>
#include <windows.h>
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#endif

namespace {

inline bool is_path_sep(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

#ifdef _WIN32
std::wstring utf8_to_wide_cli(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) return std::wstring(utf8.begin(), utf8.end());
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &w[0], n);
    return w;
}
#endif

bool path_is_dir(const std::string& p) {
#ifdef _WIN32
    std::wstring w = utf8_to_wide_cli(p);
    while (w.size() > 1 && (w.back() == L'/' || w.back() == L'\\')) {
        if (w.size() == 3 && w[1] == L':') break;
        w.pop_back();
    }
    struct _stat64 st{};
    return _wstat64(w.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#else
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

std::string parent_dir(const std::string& p) {
    size_t pos = p.find_last_of("/\\");
    if (pos == std::string::npos) return "";
    if (pos == 0) return "/";
#ifdef _WIN32
    if (pos == 2 && p.size() >= 3 && p[1] == ':') return p.substr(0, 3);
#endif
    return p.substr(0, pos);
}

bool mkdir_p(const std::string& path, std::string& err) {
    if (path.empty() || path == "." || path == "/") return true;
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur.push_back(path[i]);
        if (is_path_sep(path[i]) || i + 1 == path.size()) {
            if (cur == "/" || cur == "\\") continue;
#ifdef _WIN32
            if ((cur.size() == 2 && cur[1] == ':') ||
                (cur.size() == 3 && cur[1] == ':' && is_path_sep(cur[2]))) {
                continue;
            }
            std::wstring wcur = utf8_to_wide_cli(cur);
            while (wcur.size() > 1 && (wcur.back() == L'/' || wcur.back() == L'\\')) {
                wcur.pop_back();
            }
            struct _stat64 st{};
            if (_wstat64(wcur.c_str(), &st) == 0) {
                if (!S_ISDIR(st.st_mode)) {
                    err = cur + " exists and is not a directory";
                    return false;
                }
            } else if (_wmkdir(wcur.c_str()) != 0 && errno != EEXIST) {
                err = std::strerror(errno);
                return false;
            }
#else
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
#endif
        }
    }
    return true;
}

std::string join_path(const std::string& dir, const char* file) {
    if (dir.empty()) return file;
    return is_path_sep(dir.back()) ? (dir + file) : (dir + "/" + file);
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
#ifdef _WIN32
    FILE* fp = _wfopen(utf8_to_wide_cli(path).c_str(), L"wb");
#else
    FILE* fp = std::fopen(path.c_str(), "wb");
#endif
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
#ifdef _WIN32
    FILE* fp = _wfopen(utf8_to_wide_cli(path).c_str(), L"rb");
#else
    FILE* fp = std::fopen(path.c_str(), "rb");
#endif
    if (!fp) {
        err = "Cannot open input file: " + path;
        return false;
    }
#ifdef _WIN32
    _fseeki64(fp, 0, SEEK_END);
    __int64 sz = _ftelli64(fp);
    if (sz <= 0) {
        std::fclose(fp);
        err = "Input file is empty: " + path;
        return false;
    }
    _fseeki64(fp, 0, SEEK_SET);
#else
    std::fseek(fp, 0, SEEK_END);
    long sz = std::ftell(fp);
    if (sz <= 0) {
        std::fclose(fp);
        err = "Input file is empty: " + path;
        return false;
    }
    std::fseek(fp, 0, SEEK_SET);
#endif
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
    if (m0.focus_is_infinity || m0.focus_distance_m > 0.0f) {
        if (m0.focus_is_infinity || m0.focus_distance_m >= 999999.0f) {
            std::printf("Focus:         inf (pos=%u)\n", m0.focus_position);
        } else {
            std::printf("Focus:         %.4f m [%.4f, %.4f] (pos=%u, abs=%d, f_opt=%.2f mm)\n",
                        m0.focus_distance_m,
                        m0.focus_distance_low_m,
                        m0.focus_distance_high_m,
                        m0.focus_position,
                        m0.has_lens_position_abs ? m0.lens_position_abs : 0,
                        m0.focal_length_opt_mm);
        }
    }
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

void print_info_human(
    const nijinwvdc::NevClipInfo& clip,
    const nijinwvdc::NevFrameMeta& m0,
    const nijinwvdc::VideoFrameTiming& t0,
    const nijinwvdc::TicoRawInfo& jxs) {
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
            if (t0.has_timecode) {
                std::printf("Timecode:      %s ts=%u dur=%u nf=%u, start=%s\n",
                            tr.format.c_str(), tr.tmcd_timescale, tr.tmcd_frame_duration,
                            tr.tmcd_num_frames, t0.timecode_str);
            } else {
                std::printf("Timecode:      %s ts=%u dur=%u nf=%u\n",
                            tr.format.c_str(), tr.tmcd_timescale, tr.tmcd_frame_duration, tr.tmcd_num_frames);
            }
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

void print_info_json_prefix(
    const nijinwvdc::NevClipInfo& clip,
    const nijinwvdc::NevFrameMeta& m0,
    const nijinwvdc::VideoFrameTiming& t0,
    uint32_t cs_w,
    uint32_t cs_h) {
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
    if (t0.has_timecode) {
        std::printf("  \"timecode\": \"%s\",\n", json_escape(t0.timecode_str).c_str());
    }
    std::printf("  \"codestream_width\": %u,\n", cs_w);
    std::printf("  \"codestream_height\": %u,\n", cs_h);
    std::printf("  \"active_crop\": [%u, %u, %u, %u],\n", m0.crop[0], m0.crop[1], m0.crop[2], m0.crop[3]);
    std::printf("  \"orientation\": %u,\n", m0.orientation);
    std::printf("  \"iso\": %u,\n", m0.iso);
    std::printf("  \"exposure_time_s\": %.6f,\n", m0.exposure_time_s);
    std::printf("  \"fnumber\": %.2f,\n", m0.fnumber);
    std::printf("  \"focal_length_mm\": %.4f,\n", m0.focal_length_mm);
    std::printf("  \"focal_length_opt_mm\": %.4f,\n", m0.focal_length_opt_mm);
    if (m0.focus_is_infinity) {
        std::printf("  \"focus_distance_m\": 1000000.0000,\n");
    } else if (m0.focus_distance_m > 0.0f) {
        std::printf("  \"focus_distance_m\": %.4f,\n", m0.focus_distance_m);
    } else {
        std::printf("  \"focus_distance_m\": null,\n");
    }
    if (m0.focus_distance_low_m > 0.0f && m0.focus_distance_high_m > 0.0f) {
        std::printf("  \"focus_distance_low_m\": %.4f,\n", m0.focus_distance_low_m);
        std::printf("  \"focus_distance_high_m\": %.4f,\n", m0.focus_distance_high_m);
    } else {
        std::printf("  \"focus_distance_low_m\": null,\n");
        std::printf("  \"focus_distance_high_m\": null,\n");
    }
    std::printf("  \"focus_is_infinity\": %s,\n", m0.focus_is_infinity ? "true" : "false");
    std::printf("  \"focus_position\": %u,\n", m0.focus_position);
    std::printf("  \"black_level\": [%u, %u, %u, %u],\n", m0.black[0], m0.black[1], m0.black[2], m0.black[3]);
    std::printf("  \"wb_rggb\": [%.8f, %.8f, %.8f, %.8f],\n", m0.wb_rggb[0], m0.wb_rggb[1], m0.wb_rggb[2], m0.wb_rggb[3]);
    std::printf("  \"picture_control_base\": \"%s\",\n", json_escape(m0.picture_control_base).c_str());
    std::printf("  \"nef_compression\": %u,\n", m0.nef_compression);
}

void print_quant_json_block(const nijinwvdc::WaveletQuantProfile& q) {
    if (!q.available) {
        return;
    }
    const char* cname = (q.codec == nijinwvdc::CodecFamily::TicoRaw) ? "ticoraw"
                      : (q.codec == nijinwvdc::CodecFamily::Arw6)    ? "arw6"
                                                                     : "unknown";
    std::printf(",\n  \"quant_profile\": {\n");
    std::printf("    \"codec\": \"%s\",\n", cname);
    std::printf("    \"nlt_enabled\": %s,\n", q.nlt_enabled ? "true" : "false");
    std::printf("    \"row_step\": %u,\n", q.row_step);
    std::printf("    \"num_rows\": %u,\n", q.num_rows);
    std::printf("    \"black_ref\": %.2f,\n", q.black_ref);
    std::printf("    \"white_ref\": %.2f,\n", q.white_ref);
    std::printf("    \"mean_luma_code_std\": %.8f,\n", q.mean_luma_code_std);
    std::printf("    \"mean_chroma_code_std\": %.8f,\n", q.mean_chroma_code_std);
    std::printf("    \"max_luma_code_std\": %.8f,\n", q.max_luma_code_std);
    std::printf("    \"max_chroma_code_std\": %.8f,\n", q.max_chroma_code_std);
    std::printf("    \"nlt_slope_shadow\": %.6f,\n", q.nlt_slope_lut[4]);
    std::printf("    \"nlt_slope_mid\": %.6f,\n", q.nlt_slope_lut[128]);
    std::printf("    \"nlt_slope_high\": %.6f,\n", q.nlt_slope_lut[240]);
    std::printf("    \"luma_scale_psd\": [%.6f, %.6f, %.6f, %.6f, %.6f, %.6f],\n",
                q.luma_scale_psd[0], q.luma_scale_psd[1], q.luma_scale_psd[2],
                q.luma_scale_psd[3], q.luma_scale_psd[4], q.luma_scale_psd[5]);
    std::printf("    \"chroma_scale_psd\": [%.6f, %.6f, %.6f, %.6f, %.6f, %.6f],\n",
                q.chroma_scale_psd[0], q.chroma_scale_psd[1], q.chroma_scale_psd[2],
                q.chroma_scale_psd[3], q.chroma_scale_psd[4], q.chroma_scale_psd[5]);
    std::printf("    \"walsh16_luma_norm\": [");
    for (int k = 0; k < 16; ++k) {
        std::printf("%.5f%s", q.walsh16_luma_norm[k], (k < 15) ? ", " : "");
    }
    std::printf("],\n    \"walsh16_chroma_norm\": [");
    for (int k = 0; k < 16; ++k) {
        std::printf("%.5f%s", q.walsh16_chroma_norm[k], (k < 15) ? ", " : "");
    }
    std::printf("]\n  }");
}

void print_quant_human_line(const nijinwvdc::WaveletQuantProfile& q) {
    if (!q.available) {
        return;
    }
    std::printf("QuantProfile:  rows=%ux%u, mean_code_std=(Y=%.6f, C=%.6f), nlt_slope=(sh=%.3f, mid=%.3f, hi=%.3f)\n",
                q.num_rows, q.row_step, q.mean_luma_code_std, q.mean_chroma_code_std,
                q.nlt_slope_lut[4], q.nlt_slope_lut[128], q.nlt_slope_lut[240]);
}

void print_info_json(
    const nijinwvdc::NevClipInfo& clip,
    const nijinwvdc::NevFrameMeta& m0,
    const nijinwvdc::VideoFrameTiming& t0,
    const nijinwvdc::TicoRawInfo& jxs,
    const nijinwvdc::WaveletQuantProfile* qprof = nullptr) {
    print_info_json_prefix(clip, m0, t0, jxs.width, jxs.height);
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
    std::printf("  }");
    if (qprof && qprof->available) {
        print_quant_json_block(*qprof);
    }
    std::printf("\n}\n");
}

void print_info_arw6_json(
    const nijinwvdc::NevClipInfo& clip,
    const nijinwvdc::NevFrameMeta& m0,
    const nijinwvdc::VideoFrameTiming& t0,
    const nijinwvdc::Arw6Info& arw6,
    const nijinwvdc::WaveletQuantProfile* qprof = nullptr) {
    print_info_json_prefix(clip, m0, t0, arw6.width, arw6.height);
    std::printf("  \"arw6\": {\n");
    std::printf("    \"strip_bytes\": %u,\n", arw6.codestream_size);
    std::printf("    \"tiles\": %zu,\n", arw6.tiles.size());
    std::printf("    \"bit_depth\": %u,\n", arw6.bit_depth);
    std::printf("    \"internal_depth\": %u,\n", arw6.internal_depth);
    std::printf("    \"decomp_levels\": %u,\n", arw6.decomp_levels);
    std::printf("    \"subsampling_mask\": \"0x%04x\",\n", arw6.subsampling_mask);
    std::printf("    \"sony_raw_file_type\": %u\n", m0.sony_raw_file_type);
    std::printf("  }");
    if (qprof && qprof->available) {
        print_quant_json_block(*qprof);
    }
    std::printf("\n}\n");
}

void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage:\n"
        "  %s --version\n"
        "  %s --info [--json] [--quant] <in.NEV | in.NEF | in.ARW | in.jxs>\n"
        "  %s <in.NEV | in.NEF | in.ARW> [--frame <N|start-end|all>] -o <out.dng|out.raw|dir/> [options]\n"
        "  %s --raw-jxs <in.jxs> -o <out.raw|out.dng> [options]\n"
        "  %s --extract-jxs <in.NEV | in.NEF> [--frame <N>] -o <out.jxs>\n"
        "  %s --extract-audio <in.NEV> -o <out.wav>\n"
        "  %s --extract-preview <in.NEV | in.NEF | in.ARW> [--thumb] -o <out.jpg>\n"
        "  %s --bench <in.NEV | in.ARW> [--frames N] [--threads N]\n\n"
        "Options:\n"
        "  --format dng|raw        Force output format when writing to a directory\n"
        "  --active-crop           Crop sensor margins to camera active crop area\n"
        "  --orientation <1..8>    Override TIFF Orientation\n"
        "  --quant                 Include wavelet quantization profile in --info output\n"
        "  --threads <N>           Worker threads (default: hardware concurrency)\n",
        prog, prog, prog, prog, prog, prog, prog, prog);
}

} // namespace

int main(int argc, char** argv) {
    bool info_mode = false;
    bool json_mode = false;
    bool raw_jxs_mode = false;
    bool extract_jxs_mode = false;
    bool extract_audio_mode = false;
    bool extract_preview_mode = false;
    bool extract_thumb = false;
    bool bench_mode = false;
    bool active_crop = false;
    bool quant_mode = false;

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
        } else if (std::strcmp(a, "-V") == 0 || std::strcmp(a, "--version") == 0) {
            std::printf("nijinWVDC %s\n", nijinwvdc::version_string());
            return 0;
        } else if (std::strcmp(a, "--info") == 0) {
            info_mode = true;
        } else if (std::strcmp(a, "--json") == 0) {
            json_mode = true;
        } else if (std::strcmp(a, "--raw-jxs") == 0) {
            raw_jxs_mode = true;
        } else if (std::strcmp(a, "--extract-jxs") == 0) {
            extract_jxs_mode = true;
        } else if (std::strcmp(a, "--extract-audio") == 0) {
            extract_audio_mode = true;
        } else if (std::strcmp(a, "--extract-preview") == 0) {
            extract_preview_mode = true;
        } else if (std::strcmp(a, "--thumb") == 0) {
            extract_thumb = true;
        } else if (std::strcmp(a, "--bench") == 0) {
            bench_mode = true;
        } else if (std::strcmp(a, "--active-crop") == 0) {
            active_crop = true;
        } else if (std::strcmp(a, "--quant") == 0) {
            quant_mode = true;
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
            nijinwvdc::DecodedFrameInfo q_info;
            if (quant_mode) {
                nijinwvdc::DecoderSession q_sess(threads);
                nijinwvdc::DecodeOptions q_opts;
                q_opts.num_threads = threads;
                q_opts.want_quant_profile = true;
                std::vector<uint16_t> q_bayer;
                if (!q_sess.decode_codestream(cs.data(), cs.size(), q_opts, q_bayer, q_info, err)) {
                    std::fprintf(stderr, "Error: %s\n", err.c_str());
                    return 1;
                }
            }
            if (json_mode) {
                std::printf("{\"width\":%u,\"height\":%u,\"lcod\":%u,\"cdt_depth\":%d,\"in_bitdepth\":%d,\"out_bitdepth\":%d,\"gamma_type\":%d,\"slices\":%d,\"precincts\":%d",
                            jxs.width, jxs.height, jxs.lcod, jxs.cdt_depth, jxs.in_bitdepth, jxs.out_bitdepth, jxs.gamma_type, jxs.num_slices, jxs.num_precincts);
                if (quant_mode && q_info.quant_profile.available) {
                    print_quant_json_block(q_info.quant_profile);
                }
                std::printf("\n}\n");
            } else {
                std::printf("Raw TicoRAW: %ux%u, Lcod=%u B, cdt_depth=%d, slices=%d, precincts=%d\n",
                            jxs.width, jxs.height, jxs.lcod, jxs.cdt_depth, jxs.num_slices, jxs.num_precincts);
                if (quant_mode && q_info.quant_profile.available) {
                    print_quant_human_line(q_info.quant_profile);
                }
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

    // Container / clip modes via MediaReader + DecoderSession
    nijinwvdc::MediaReader reader;
    if (!reader.open_file(input_path, err)) {
        std::fprintf(stderr, "Error: %s\n", err.c_str());
        return 1;
    }
    const nijinwvdc::NevClipInfo& clip = reader.clip_info();

    if (info_mode) {
        nijinwvdc::NevFrameMeta m0;
        std::vector<uint8_t> cs0;
        if (!reader.read_frame_packet(0, m0, &cs0, err)) {
            std::fprintf(stderr, "Error: %s\n", err.c_str());
            return 1;
        }
        nijinwvdc::VideoFrameTiming t0;
        std::string t_err;
        reader.get_frame_timing(0, t0, t_err);
        nijinwvdc::DecodedFrameInfo q_info;
        if (quant_mode) {
            nijinwvdc::DecoderSession q_sess(threads);
            nijinwvdc::DecodeOptions q_opts;
            q_opts.num_threads = threads;
            q_opts.apply_active_crop = active_crop;
            q_opts.want_quant_profile = true;
            std::vector<uint16_t> q_bayer;
            if (!q_sess.decode_frame(reader, 0, q_opts, q_bayer, q_info, err)) {
                std::fprintf(stderr, "Error: %s\n", err.c_str());
                return 1;
            }
        }
        if (clip.codec_family == nijinwvdc::CodecFamily::Arw6) {
            nijinwvdc::Arw6Info arw6;
            const bool ok = (cs0.size() >= 16 && std::memcmp(cs0.data(), "A000", 4) == 0)
                                ? nijinwvdc::probe_arw6_codestream(cs0.data(), cs0.size(), arw6, err)
                                : nijinwvdc::probe_arw6_strip(cs0.data(), cs0.size(), arw6, err);
            if (!ok) {
                std::fprintf(stderr, "Error: %s\n", err.c_str());
                return 1;
            }
            if (json_mode) {
                print_info_arw6_json(clip, m0, t0, arw6, quant_mode ? &q_info.quant_profile : nullptr);
            } else {
                print_info_arw6_human(clip, m0, arw6);
                if (quant_mode) {
                    print_quant_human_line(q_info.quant_profile);
                }
            }
            return 0;
        }
        nijinwvdc::TicoRawInfo jxs;
        if (!nijinwvdc::probe_ticoraw_header(cs0.data(), cs0.size(), jxs, err)) {
            std::fprintf(stderr, "Error: %s\n", err.c_str());
            return 1;
        }
        if (json_mode) {
            print_info_json(clip, m0, t0, jxs, quant_mode ? &q_info.quant_profile : nullptr);
        } else {
            print_info_human(clip, m0, t0, jxs);
            if (quant_mode) {
                print_quant_human_line(q_info.quant_profile);
            }
        }
        return 0;
    }

    if (extract_audio_mode) {
        if (output_path.empty()) {
            std::fprintf(stderr, "Error: --extract-audio requires -o <out.wav>\n");
            return 2;
        }
        const auto ainfo = reader.audio_info();
        if (!reader.export_audio_wav(output_path, err)) {
            std::fprintf(stderr, "Error: %s\n", err.c_str());
            return 1;
        }
        std::printf("Extracted %llu audio frames (%.0f Hz, %u ch, %u-bit) to %s\n",
                    static_cast<unsigned long long>(ainfo.total_frames),
                    ainfo.sample_rate, ainfo.channels, ainfo.bits_per_channel, output_path.c_str());
        return 0;
    }

    if (extract_preview_mode) {
        if (output_path.empty()) {
            std::fprintf(stderr, "Error: --extract-preview requires -o <out.jpg>\n");
            return 2;
        }
        std::vector<uint8_t> blob;
        if (extract_thumb) {
            if (!nijinwvdc::extract_nev_blob(clip.file_path, clip.ncth_offset, clip.ncth_size, blob, err)) {
                std::fprintf(stderr, "Error: %s\n", err.c_str());
                return 1;
            }
        } else if (!reader.extract_preview_jpeg(blob, err)) {
            std::fprintf(stderr, "Error: %s\n", err.c_str());
            return 1;
        }
        if (!write_raw_file(output_path, blob.data(), blob.size(), err)) {
            std::fprintf(stderr, "Error: %s\n", err.c_str());
            return 1;
        }
        std::printf("Extracted %zu bytes to %s\n", blob.size(), output_path.c_str());
        return 0;
    }

    if (extract_jxs_mode) {
        if (clip.codec_family == nijinwvdc::CodecFamily::Arw6) {
            std::fprintf(stderr, "Error: --extract-jxs applies to Nikon TicoRAW (.NEV/.NEF), not Sony ARW6\n");
            return 2;
        }
        if (output_path.empty()) {
            std::fprintf(stderr, "Error: --extract-jxs requires -o <out.jxs>\n");
            return 2;
        }
        std::vector<uint32_t> frames;
        if (!parse_frame_spec(frame_spec, reader.frame_count(), frames, err) || frames.size() != 1) {
            std::fprintf(stderr, "Error: --extract-jxs requires a single valid --frame index (%s)\n", err.c_str());
            return 2;
        }
        nijinwvdc::NevFrameMeta meta;
        std::vector<uint8_t> cs;
        if (!reader.read_frame_packet(frames[0], meta, &cs, err) ||
            !write_raw_file(output_path, cs.data(), cs.size(), err)) {
            std::fprintf(stderr, "Error: %s\n", err.c_str());
            return 1;
        }
        std::printf("Extracted frame %u (%zu bytes) to %s\n", frames[0], cs.size(), output_path.c_str());
        return 0;
    }

    nijinwvdc::DecoderSession session(threads);

    if (bench_mode) {
        size_t count = (reader.frame_count() == 1u)
                           ? static_cast<size_t>(std::max(1, bench_frames))
                           : static_cast<size_t>(std::min<long>(std::max(1, bench_frames), static_cast<long>(reader.frame_count())));
        std::vector<uint8_t> cs;
        std::vector<uint16_t> bayer;
        nijinwvdc::NevFrameMeta meta;
        nijinwvdc::DecodeOptions dopts;
        dopts.num_threads = threads;
        nijinwvdc::DecodedFrameInfo dec_info;

        if (!reader.read_frame_packet(0, meta, &cs, err) ||
            !session.decode_codestream(cs.data(), cs.size(), dopts, bayer, dec_info, err)) {
            std::fprintf(stderr, "Benchmark warmup failed: %s\n", err.c_str());
            return 1;
        }

        double dec_ms_total = 0.0;
        auto t_e2e_start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < count; ++i) {
            uint32_t fidx = (reader.frame_count() == 1u) ? 0u : static_cast<uint32_t>(i);
            if (!reader.read_frame_packet(fidx, meta, &cs, err)) {
                std::fprintf(stderr, "Read failed at iter %zu: %s\n", i, err.c_str());
                return 1;
            }
            auto t0 = std::chrono::steady_clock::now();
            if (!session.decode_codestream_into(
                    cs.data(), cs.size(), dopts,
                    bayer.data(), static_cast<size_t>(dec_info.coded_width) * sizeof(uint16_t),
                    dec_info.coded_height, dec_info, err)) {
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
                    count, threads, dec_info.coded_width, dec_info.coded_height,
                    avg_dec, 1000.0 / avg_dec, avg_e2e, 1000.0 / avg_e2e);
        return 0;
    }

    if (output_path.empty()) {
        std::fprintf(stderr, "Error: missing -o <output>\n");
        print_usage(argv[0]);
        return 2;
    }

    std::vector<uint32_t> frames;
    if (!parse_frame_spec(frame_spec, reader.frame_count(), frames, err)) {
        std::fprintf(stderr, "Error: %s\n", err.c_str());
        return 2;
    }

    bool out_is_dir = (frames.size() > 1) || is_path_sep(output_path.back()) || path_is_dir(output_path);
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

    std::vector<uint16_t> bayer;
    nijinwvdc::DecodeOptions dec_opts;
    dec_opts.num_threads = threads;
    dec_opts.apply_active_crop = active_crop;
    for (uint32_t fidx : frames) {
        nijinwvdc::DecodedFrameInfo dec_info;
        if (!session.decode_frame(reader, fidx, dec_opts, bayer, dec_info, err)) {
            std::fprintf(stderr, "Error decoding frame %u: %s\n", fidx, err.c_str());
            return 1;
        }

        nijinwvdc::DngWriteOptions opts;
        if (clip.codec_family == nijinwvdc::CodecFamily::Arw6) {
            nijinwvdc::Arw6Info arw6;
            arw6.width = dec_info.coded_width;
            arw6.height = dec_info.coded_height;
            arw6.bit_depth = dec_info.bit_depth;
            nijinwvdc::build_dng_options_from_clip_and_frame(
                clip, dec_info.meta, arw6, active_crop, override_orientation, opts);
        } else {
            nijinwvdc::TicoRawInfo jxs;
            jxs.width = static_cast<uint16_t>(dec_info.coded_width);
            jxs.height = static_cast<uint16_t>(dec_info.coded_height);
            jxs.cdt_depth = dec_info.bit_depth;
            jxs.out_bitdepth = dec_info.bit_depth;
            nijinwvdc::build_dng_options_from_clip_and_frame(
                clip, dec_info.meta, jxs, active_crop, override_orientation, opts);
        }
        opts.width = dec_info.output_width;
        opts.height = dec_info.output_height;
        std::memcpy(opts.cfa_pattern, dec_info.cfa_pattern, 4);
        if (active_crop && (dec_info.output_width != dec_info.coded_width ||
                            dec_info.output_height != dec_info.coded_height)) {
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
            if (!write_raw_file(target, bayer.data(), nbytes, err)) {
                std::fprintf(stderr, "Error: %s\n", err.c_str());
                return 1;
            }
        } else {
            if (!nijinwvdc::write_cfa_dng(target, bayer.data(), opts, err)) {
                std::fprintf(stderr, "Error: %s\n", err.c_str());
                return 1;
            }
        }
        std::printf("Wrote frame %u (%ux%u, %s) -> %s\n",
                    fidx, opts.width, opts.height, use_raw ? "raw" : "dng", target.c_str());
    }
    return 0;
}
