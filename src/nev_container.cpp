// SPDX-License-Identifier: Apache-2.0
#include "../include/nijinWVDC.h"
#include "tiff_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace nijinwvdc {
namespace {

using detail::ScopedFile;
using detail::TiffTagValue;
using detail::fourcc_to_str;
using detail::rd_be16;
using detail::rd_be32;
using detail::rd_be64;
using detail::rd_be_f32;
using detail::rd_be_f64;
using detail::rd_le16;
using detail::rd_le32;
using detail::rd_u16_e;
using detail::rd_u32_e;
using detail::read_tiff_ifd;
using detail::rstrip_ascii;
using detail::seek_read;
using detail::tag_as_rational;
using detail::tag_as_u32;

bool starts_with_ci(const std::string& s, const char* prefix) {
    size_t n = std::strlen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

struct BoxHeader {
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t header_size = 8;
    char type[5] = {0, 0, 0, 0, 0};
};

bool read_box_header(FILE* fp, uint64_t pos, uint64_t parent_end, BoxHeader& out) {
    if (pos + 8 > parent_end) {
        return false;
    }
    uint8_t hdr[16] = {0};
    if (!seek_read(fp, pos, hdr, 8)) {
        return false;
    }
    uint32_t sz32 = rd_be32(hdr);
    std::memcpy(out.type, hdr + 4, 4);
    out.type[4] = '\0';
    out.offset = pos;
    if (sz32 == 1) {
        if (pos + 16 > parent_end || !seek_read(fp, pos + 8, hdr + 8, 8)) {
            return false;
        }
        out.size = rd_be64(hdr + 8);
        out.header_size = 16;
    } else if (sz32 == 0) {
        out.size = parent_end - pos;
        out.header_size = 8;
    } else {
        out.size = sz32;
        out.header_size = 8;
    }
    if (out.size < out.header_size || pos + out.size > parent_end) {
        return false;
    }
    return true;
}

struct StscEntry {
    uint32_t first_chunk;
    uint32_t samples_per_chunk;
    uint32_t desc_idx;
};

struct RawTrackData {
    NevTrackSummary summary;
    std::vector<StscEntry> stsc;
    uint32_t constant_sample_size = 0;
    std::vector<uint32_t> sample_sizes;
    std::vector<uint64_t> chunk_offsets;
    std::vector<std::pair<uint32_t, uint32_t>> stts;
};

bool parse_nctg_buffer(const uint8_t* data, size_t len, NevClipInfo& clip) {
    static const size_t kTypeSizes[13] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};
    size_t pos = 0;
    while (pos + 8 <= len) {
        uint32_t tag = rd_be32(data + pos);
        uint16_t dtype = rd_be16(data + pos + 4);
        uint16_t count = rd_be16(data + pos + 6);
        if (dtype == 0 || dtype >= 13) {
            break;
        }
        size_t blen = kTypeSizes[dtype] * static_cast<size_t>(count);
        if (pos + 8 + blen > len) {
            break;
        }
        const uint8_t* val = data + pos + 8;
        if (tag == 0x00000001 && dtype == 2) {
            clip.make = rstrip_ascii(val, blen);
        } else if (tag == 0x00000002 && dtype == 2) {
            clip.model = rstrip_ascii(val, blen);
        } else if (tag == 0x00000003 && dtype == 2) {
            clip.firmware_version = rstrip_ascii(val, blen);
        } else if (tag == 0x00000011 && dtype == 2) {
            clip.datetime = rstrip_ascii(val, blen);
        } else if (tag == 0x00000016 && dtype == 5 && count >= 1) {
            uint32_t num = rd_be32(val);
            uint32_t den = rd_be32(val + 4);
            if (den > 0) {
                clip.fps = static_cast<double>(num) / static_cast<double>(den);
            }
        } else if (tag == 0x00000022 && dtype == 3 && count >= 1) {
            clip.nctg_active_w = rd_be16(val);
        } else if (tag == 0x00000023 && dtype == 3 && count >= 1) {
            clip.nctg_active_h = rd_be16(val);
        } else if (tag == 0x00001017 && dtype == 3 && count >= 1) {
            clip.nctg_kelvin = rd_be16(val);
        } else if (tag == 0x0110a434 && dtype == 2) {
            clip.lens_model = rstrip_ascii(val, blen);
        } else if (tag == 0x02000023 && dtype == 7 && blen >= 48) {
            clip.picture_control_name = rstrip_ascii(val + 8, 20);
            clip.picture_control_base = rstrip_ascii(val + 28, 20);
        }
        pos += 8 + blen;
    }
    return true;
}

bool parse_ncdt(FILE* fp, uint64_t start, uint64_t end, NevClipInfo& clip) {
    uint64_t pos = start;
    while (pos + 8 <= end) {
        BoxHeader bh;
        if (!read_box_header(fp, pos, end, bh)) {
            return false;
        }
        uint64_t pay_off = bh.offset + bh.header_size;
        uint64_t pay_sz = bh.size - bh.header_size;
        if (std::strcmp(bh.type, "NCTG") == 0 && pay_sz <= 4 * 1024 * 1024) {
            std::vector<uint8_t> buf(static_cast<size_t>(pay_sz));
            if (seek_read(fp, pay_off, buf.data(), buf.size())) {
                parse_nctg_buffer(buf.data(), buf.size(), clip);
            }
        } else if (std::strcmp(bh.type, "NCTH") == 0) {
            clip.ncth_offset = pay_off;
            clip.ncth_size = static_cast<uint32_t>(pay_sz);
        } else if (std::strcmp(bh.type, "NCVW") == 0) {
            clip.ncvw_offset = pay_off;
            clip.ncvw_size = static_cast<uint32_t>(pay_sz);
        } else if (std::strcmp(bh.type, "NCM1") == 0) {
            clip.ncm1_offset = pay_off;
            clip.ncm1_size = static_cast<uint32_t>(pay_sz);
        }
        pos += bh.size;
    }
    return true;
}

bool parse_stbl(FILE* fp, uint64_t start, uint64_t end, RawTrackData& tr, std::string& err) {
    uint64_t pos = start;
    while (pos + 8 <= end) {
        BoxHeader bh;
        if (!read_box_header(fp, pos, end, bh)) {
            err = "Malformed box inside stbl";
            return false;
        }
        uint64_t pay_off = bh.offset + bh.header_size;
        uint64_t pay_sz = bh.size - bh.header_size;
        if (pay_sz <= 16 * 1024 * 1024) {
            std::vector<uint8_t> buf(static_cast<size_t>(pay_sz));
            if (!seek_read(fp, pay_off, buf.data(), buf.size())) {
                err = "Failed reading stbl box payload";
                return false;
            }
            if (std::strcmp(bh.type, "stsd") == 0 && buf.size() >= 16) {
                uint32_t entry_count = rd_be32(buf.data() + 4);
                if (entry_count >= 1 && buf.size() >= 16) {
                    uint32_t ent_sz = rd_be32(buf.data() + 8);
                    if (ent_sz >= 8 && 8 + ent_sz <= buf.size()) {
                        const uint8_t* ent = buf.data() + 8;
                        tr.summary.format = fourcc_to_str(ent + 4);
                        if (tr.summary.handler_type == "vide" && ent_sz >= 86) {
                            tr.summary.width = rd_be16(ent + 32);
                            tr.summary.height = rd_be16(ent + 34);
                        } else if (tr.summary.handler_type == "soun" && ent_sz >= 24) {
                            uint16_t ver = rd_be16(ent + 16);
                            if (ver == 2 && ent_sz >= 72) {
                                tr.summary.audio_sample_rate = rd_be_f64(ent + 40);
                                tr.summary.audio_channels = rd_be32(ent + 48);
                                tr.summary.audio_bits_per_channel = rd_be32(ent + 56);
                                tr.summary.audio_bytes_per_packet = rd_be32(ent + 64);
                            } else if (ver <= 1 && ent_sz >= 36) {
                                tr.summary.audio_channels = rd_be16(ent + 24);
                                tr.summary.audio_bits_per_channel = rd_be16(ent + 26);
                                tr.summary.audio_sample_rate = static_cast<double>(rd_be32(ent + 32)) / 65536.0;
                            }
                        } else if (tr.summary.handler_type == "tmcd" && ent_sz >= 34) {
                            tr.summary.tmcd_flags = rd_be32(ent + 20);
                            tr.summary.tmcd_timescale = rd_be32(ent + 24);
                            tr.summary.tmcd_frame_duration = rd_be32(ent + 28);
                            tr.summary.tmcd_num_frames = ent[32];
                        }
                    }
                }
            } else if (std::strcmp(bh.type, "stts") == 0 && buf.size() >= 8) {
                uint32_t n = rd_be32(buf.data() + 4);
                for (uint32_t i = 0; i < n && 8 + (size_t)(i + 1) * 8 <= buf.size(); ++i) {
                    uint32_t sc = rd_be32(buf.data() + 8 + i * 8);
                    uint32_t sd = rd_be32(buf.data() + 12 + i * 8);
                    tr.stts.push_back({sc, sd});
                }
            } else if (std::strcmp(bh.type, "stsc") == 0 && buf.size() >= 8) {
                uint32_t n = rd_be32(buf.data() + 4);
                for (uint32_t i = 0; i < n && 8 + (size_t)(i + 1) * 12 <= buf.size(); ++i) {
                    StscEntry e;
                    e.first_chunk = rd_be32(buf.data() + 8 + i * 12);
                    e.samples_per_chunk = rd_be32(buf.data() + 12 + i * 12);
                    e.desc_idx = rd_be32(buf.data() + 16 + i * 12);
                    tr.stsc.push_back(e);
                }
            } else if (std::strcmp(bh.type, "stsz") == 0 && buf.size() >= 12) {
                tr.constant_sample_size = rd_be32(buf.data() + 4);
                tr.summary.sample_count = rd_be32(buf.data() + 8);
                if (tr.constant_sample_size == 0) {
                    tr.sample_sizes.reserve(tr.summary.sample_count);
                    for (uint32_t i = 0; i < tr.summary.sample_count && 12 + (size_t)(i + 1) * 4 <= buf.size(); ++i) {
                        tr.sample_sizes.push_back(rd_be32(buf.data() + 12 + i * 4));
                    }
                }
            } else if (std::strcmp(bh.type, "stco") == 0 && buf.size() >= 8) {
                uint32_t n = rd_be32(buf.data() + 4);
                tr.chunk_offsets.reserve(n);
                for (uint32_t i = 0; i < n && 8 + (size_t)(i + 1) * 4 <= buf.size(); ++i) {
                    tr.chunk_offsets.push_back(rd_be32(buf.data() + 8 + i * 4));
                }
            } else if (std::strcmp(bh.type, "co64") == 0 && buf.size() >= 8) {
                uint32_t n = rd_be32(buf.data() + 4);
                tr.chunk_offsets.reserve(n);
                for (uint32_t i = 0; i < n && 8 + (size_t)(i + 1) * 8 <= buf.size(); ++i) {
                    tr.chunk_offsets.push_back(rd_be64(buf.data() + 8 + i * 8));
                }
            }
        }
        pos += bh.size;
    }
    return true;
}

bool parse_trak(FILE* fp, uint64_t start, uint64_t end, RawTrackData& tr, std::string& err);

bool parse_container_recurse(
    FILE* fp,
    uint64_t start,
    uint64_t end,
    RawTrackData& tr,
    std::string& err) {
    uint64_t pos = start;
    while (pos + 8 <= end) {
        BoxHeader bh;
        if (!read_box_header(fp, pos, end, bh)) {
            err = "Malformed box inside trak";
            return false;
        }
        uint64_t pay_off = bh.offset + bh.header_size;
        uint64_t pay_end = bh.offset + bh.size;
        if (std::strcmp(bh.type, "tkhd") == 0) {
            uint8_t buf[96] = {0};
            size_t to_read = static_cast<size_t>(std::min<uint64_t>(sizeof(buf), pay_end - pay_off));
            if (seek_read(fp, pay_off, buf, to_read) && to_read >= 24) {
                uint8_t ver = buf[0];
                if (ver == 0 && to_read >= 84) {
                    tr.summary.track_id = rd_be32(buf + 12);
                } else if (ver == 1 && to_read >= 96) {
                    tr.summary.track_id = rd_be32(buf + 20);
                }
            }
        } else if (std::strcmp(bh.type, "mdhd") == 0) {
            uint8_t buf[36] = {0};
            size_t to_read = static_cast<size_t>(std::min<uint64_t>(sizeof(buf), pay_end - pay_off));
            if (seek_read(fp, pay_off, buf, to_read) && to_read >= 24) {
                uint8_t ver = buf[0];
                if (ver == 0) {
                    tr.summary.timescale = rd_be32(buf + 12);
                    tr.summary.duration = rd_be32(buf + 16);
                } else if (ver == 1 && to_read >= 32) {
                    tr.summary.timescale = rd_be32(buf + 20);
                    tr.summary.duration = rd_be64(buf + 24);
                }
            }
        } else if (std::strcmp(bh.type, "hdlr") == 0) {
            uint8_t buf[24] = {0};
            size_t to_read = static_cast<size_t>(std::min<uint64_t>(sizeof(buf), pay_end - pay_off));
            if (seek_read(fp, pay_off, buf, to_read) && to_read >= 12) {
                tr.summary.handler_type = fourcc_to_str(buf + 8);
            }
        } else if (std::strcmp(bh.type, "mdia") == 0 || std::strcmp(bh.type, "minf") == 0) {
            if (!parse_container_recurse(fp, pay_off, pay_end, tr, err)) {
                return false;
            }
        } else if (std::strcmp(bh.type, "stbl") == 0) {
            if (!parse_stbl(fp, pay_off, pay_end, tr, err)) {
                return false;
            }
        }
        pos += bh.size;
    }
    return true;
}

bool parse_trak(FILE* fp, uint64_t start, uint64_t end, RawTrackData& tr, std::string& err) {
    return parse_container_recurse(fp, start, end, tr, err);
}

bool build_sample_table(const RawTrackData& tr, uint64_t file_size, std::vector<NevSampleLocation>& out, std::string& err) {
    uint32_t total = tr.summary.sample_count;
    if (total == 0) {
        err = "Video track has 0 samples";
        return false;
    }
    if (tr.chunk_offsets.empty() || tr.stsc.empty()) {
        err = "Missing co64/stco or stsc in video track";
        return false;
    }
    if (tr.constant_sample_size == 0 && tr.sample_sizes.size() < total) {
        err = "Incomplete stsz sample size table";
        return false;
    }

    out.clear();
    out.reserve(total);
    uint32_t sample_idx = 0;
    size_t stsc_idx = 0;

    for (size_t ci = 0; ci < tr.chunk_offsets.size() && sample_idx < total; ++ci) {
        uint32_t chunk_num = static_cast<uint32_t>(ci + 1);
        while (stsc_idx + 1 < tr.stsc.size() && chunk_num >= tr.stsc[stsc_idx + 1].first_chunk) {
            ++stsc_idx;
        }
        uint32_t spc = tr.stsc[stsc_idx].samples_per_chunk;
        uint64_t cur_off = tr.chunk_offsets[ci];
        for (uint32_t s = 0; s < spc && sample_idx < total; ++s) {
            uint32_t sz = tr.constant_sample_size != 0 ? tr.constant_sample_size : tr.sample_sizes[sample_idx];
            if (cur_off + sz > file_size) {
                err = "Sample extends beyond file EOF";
                return false;
            }
            out.push_back({cur_off, sz});
            cur_off += sz;
            ++sample_idx;
        }
    }
    if (out.size() != total) {
        err = "Failed reconstructing all sample offsets";
        return false;
    }
    return true;
}

void apply_makernote_tag(
    uint16_t tag,
    uint16_t dtype,
    uint32_t count,
    const uint8_t* val,
    size_t blen,
    bool be,
    NevFrameMeta& meta) {
    if (tag == 0x0005 && dtype == 2) {
        meta.wb_mode = rstrip_ascii(val, blen);
    } else if (tag == 0x000c && dtype == 5 && count == 4 && blen >= 32) {
        // Order: R, B, G0, G1
        uint32_t rn = rd_u32_e(val + 0, be), rd = rd_u32_e(val + 4, be);
        uint32_t bn = rd_u32_e(val + 8, be), bd = rd_u32_e(val + 12, be);
        uint32_t g0n = rd_u32_e(val + 16, be), g0d = rd_u32_e(val + 20, be);
        uint32_t g1n = rd_u32_e(val + 24, be), g1d = rd_u32_e(val + 28, be);
        if (rd && bd && g0d && g1d) {
            meta.wb_rggb[0] = static_cast<double>(rn) / rd;
            meta.wb_rggb[1] = static_cast<double>(g0n) / g0d;
            meta.wb_rggb[2] = static_cast<double>(g1n) / g1d;
            meta.wb_rggb[3] = static_cast<double>(bn) / bd;
            meta.has_wb = true;
        }
    } else if (tag == 0x0023 && dtype == 7 && blen >= 52) {
        meta.picture_control_base = rstrip_ascii(val + 8, 20);
        meta.picture_control_name = rstrip_ascii(val + 28, 20);
    } else if (tag == 0x003d && dtype == 3 && count == 4 && blen >= 8) {
        for (int c = 0; c < 4; ++c) {
            meta.black[c] = rd_u16_e(val + 2 * c, be);
        }
        meta.has_black = true;
    } else if (tag == 0x0045 && dtype == 3 && count == 4 && blen >= 8) {
        for (int c = 0; c < 4; ++c) {
            meta.crop[c] = rd_u16_e(val + 2 * c, be);
        }
        meta.has_crop = true;
    } else if (tag == 0x004f && dtype == 3 && count >= 1 && blen >= 2) {
        meta.color_temp = rd_u16_e(val, be);
    } else if (tag == 0x0093 && dtype == 3 && count >= 1 && blen >= 2) {
        meta.nef_compression = rd_u16_e(val, be);
    }
}

void parse_sequential_makernote(const uint8_t* buf, size_t len, NevFrameMeta& meta) {
    if (len < 2 || buf[0] != 'I' || buf[1] != 'I') {
        return;
    }
    static const size_t kTypeSizes[13] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};
    size_t pos = 2;
    while (pos + 8 <= len) {
        uint16_t tag = rd_le16(buf + pos);
        uint16_t dtype = rd_le16(buf + pos + 2);
        uint32_t count = rd_le32(buf + pos + 4);
        if (dtype == 0 || dtype >= 13) {
            break;
        }
        uint64_t blen64 = static_cast<uint64_t>(kTypeSizes[dtype]) * static_cast<uint64_t>(count);
        if (blen64 > len - (pos + 8)) {
            break;
        }
        size_t blen = static_cast<size_t>(blen64);
        apply_makernote_tag(tag, dtype, count, buf + pos + 8, blen, false, meta);
        pos += 8 + blen;
    }
}

} // namespace

namespace detail {

size_t tiff_dtype_size(uint16_t dtype) {
    static const size_t kSizes[14] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4};
    return (dtype < 14) ? kSizes[dtype] : 0;
}

bool read_tiff_ifd(
    FILE* fp,
    uint64_t tiff_base,
    uint64_t file_size,
    bool be,
    uint32_t ifd_rel_off,
    std::vector<TiffTagValue>& out_tags,
    uint32_t* next_ifd_off) {
    out_tags.clear();
    if (next_ifd_off) *next_ifd_off = 0;
    uint64_t ifd_abs = tiff_base + ifd_rel_off;
    if (ifd_abs + 2 > file_size) {
        return false;
    }
    uint8_t cnt_buf[2];
    if (!seek_read(fp, ifd_abs, cnt_buf, 2)) {
        return false;
    }
    uint16_t cnt = rd_u16_e(cnt_buf, be);
    if (cnt > 512) {
        return false;
    }
    uint64_t entries_bytes = static_cast<uint64_t>(cnt) * 12ULL;
    if (ifd_abs + 2 + entries_bytes + 4 > file_size) {
        return false;
    }
    std::vector<uint8_t> raw(static_cast<size_t>(entries_bytes + 4));
    if (!seek_read(fp, ifd_abs + 2, raw.data(), raw.size())) {
        return false;
    }
    if (next_ifd_off) {
        *next_ifd_off = rd_u32_e(raw.data() + entries_bytes, be);
    }

    out_tags.reserve(cnt);
    for (uint16_t i = 0; i < cnt; ++i) {
        const uint8_t* ent = raw.data() + static_cast<size_t>(i) * 12;
        TiffTagValue tv;
        tv.tag = rd_u16_e(ent + 0, be);
        tv.dtype = rd_u16_e(ent + 2, be);
        tv.count = rd_u32_e(ent + 4, be);
        size_t esz = tiff_dtype_size(tv.dtype);
        uint64_t blen64 = static_cast<uint64_t>(esz) * static_cast<uint64_t>(tv.count);
        if (esz > 0 && blen64 <= 4) {
            size_t blen = static_cast<size_t>(blen64);
            tv.data.assign(ent + 8, ent + 8 + blen);
        } else if (esz > 0 && blen64 > 4) {
            uint32_t rel_vo = rd_u32_e(ent + 8, be);
            uint64_t abs_vo = tiff_base + rel_vo;
            tv.file_offset = abs_vo;
            if (blen64 <= 4 * 1024 * 1024 && abs_vo + blen64 <= file_size) {
                tv.data.resize(static_cast<size_t>(blen64));
                if (!seek_read(fp, abs_vo, tv.data.data(), tv.data.size())) {
                    tv.data.clear();
                }
            }
        }
        out_tags.push_back(std::move(tv));
    }
    return true;
}

uint32_t tag_as_u32(const TiffTagValue& tv, bool be) {
    if (tv.dtype == 3 && tv.data.size() >= 2) {
        return rd_u16_e(tv.data.data(), be);
    }
    if ((tv.dtype == 4 || tv.dtype == 13) && tv.data.size() >= 4) {
        return rd_u32_e(tv.data.data(), be);
    }
    return 0;
}

bool tag_as_rational(const TiffTagValue& tv, bool be, double& out) {
    if (tv.dtype == 5 && tv.data.size() >= 8) {
        uint32_t num = rd_u32_e(tv.data.data(), be);
        uint32_t den = rd_u32_e(tv.data.data() + 4, be);
        if (den > 0) {
            out = static_cast<double>(num) / static_cast<double>(den);
            return true;
        }
    }
    return false;
}

} // namespace detail

namespace {

bool parse_tiff_raw_clip(
    FILE* fp,
    uint64_t file_size,
    bool be,
    NevClipInfo& clip,
    std::string& err) {
    uint8_t hdr[8] = {0};
    if (!seek_read(fp, 0, hdr, 8)) {
        err = "Failed reading TIFF header";
        return false;
    }
    uint32_t ifd0_off = rd_u32_e(hdr + 4, be);
    uint32_t ifd0_next_off = 0;
    std::vector<TiffTagValue> ifd0;
    if (!read_tiff_ifd(fp, 0, file_size, be, ifd0_off, ifd0, &ifd0_next_off)) {
        err = "Malformed TIFF IFD0";
        return false;
    }

    NevFrameMeta meta;
    meta.frame_index = 0;
    meta.orientation = 1;
    std::vector<uint32_t> subifds;
    uint32_t exif_ifd_off = 0;

    for (const auto& tv : ifd0) {
        if (tv.tag == 271 && tv.dtype == 2 && !tv.data.empty()) {
            clip.make = rstrip_ascii(tv.data.data(), tv.data.size());
        } else if (tv.tag == 272 && tv.dtype == 2 && !tv.data.empty()) {
            clip.model = rstrip_ascii(tv.data.data(), tv.data.size());
        } else if (tv.tag == 274) {
            meta.orientation = static_cast<uint16_t>(tag_as_u32(tv, be));
        } else if (tv.tag == 305 && tv.dtype == 2 && !tv.data.empty()) {
            clip.firmware_version = rstrip_ascii(tv.data.data(), tv.data.size());
        } else if (tv.tag == 306 && tv.dtype == 2 && !tv.data.empty()) {
            clip.datetime = rstrip_ascii(tv.data.data(), tv.data.size());
        } else if (tv.tag == 330 && (tv.dtype == 4 || tv.dtype == 13)) {
            for (size_t off = 0; off + 4 <= tv.data.size(); off += 4) {
                subifds.push_back(rd_u32_e(tv.data.data() + off, be));
            }
        } else if (tv.tag == 513) {
            clip.ncth_offset = tag_as_u32(tv, be);
        } else if (tv.tag == 514) {
            clip.ncth_size = tag_as_u32(tv, be);
        } else if (tv.tag == 34665) {
            exif_ifd_off = tag_as_u32(tv, be);
        }
    }

    if (starts_with_ci(clip.make, "sony")) {
        return detail::parse_sony_arw_tiff(
            fp, file_size, be, ifd0_next_off, ifd0, subifds, exif_ifd_off, clip, meta, err);
    }

    bool found_he_raw = false;
    bool saw_34713 = false;
    bool saw_sony_tag = false;
    uint64_t raw_off = 0;
    uint32_t raw_sz = 0;

    for (uint32_t sub_off : subifds) {
        std::vector<TiffTagValue> sifd;
        if (!read_tiff_ifd(fp, 0, file_size, be, sub_off, sifd)) {
            continue;
        }
        uint32_t w = 0, h = 0, bps = 0, comp = 0, phot = 0;
        uint64_t soff = 0, jpg_off = 0;
        uint32_t sbc = 0, jpg_sz = 0;
        bool has_cfa = false;
        uint8_t cfa[4] = {0, 1, 1, 2};
        for (const auto& tv : sifd) {
            if (tv.tag == 256) w = tag_as_u32(tv, be);
            else if (tv.tag == 257) h = tag_as_u32(tv, be);
            else if (tv.tag == 258) bps = tag_as_u32(tv, be);
            else if (tv.tag == 259) comp = tag_as_u32(tv, be);
            else if (tv.tag == 262) phot = tag_as_u32(tv, be);
            else if (tv.tag == 273) soff = tag_as_u32(tv, be);
            else if (tv.tag == 279) sbc = tag_as_u32(tv, be);
            else if (tv.tag == 513) jpg_off = tag_as_u32(tv, be);
            else if (tv.tag == 514) jpg_sz = tag_as_u32(tv, be);
            else if (tv.tag == 0x7000 || comp == 32766) saw_sony_tag = true;
            else if (tv.tag == 33422 && tv.data.size() >= 4) {
                std::memcpy(cfa, tv.data.data(), 4);
                has_cfa = true;
            }
        }
        if (comp == 6 && jpg_off > 0 && jpg_sz > clip.ncvw_size && jpg_off + jpg_sz <= file_size) {
            clip.ncvw_offset = jpg_off;
            clip.ncvw_size = jpg_sz;
        }
        if (comp == 34713 && phot == 32803 && soff > 0 && sbc >= 16 && soff + sbc <= file_size) {
            saw_34713 = true;
            uint8_t sig[2] = {0, 0};
            if (seek_read(fp, soff, sig, 2) && sig[0] == 0xFF && sig[1] == 0x10) {
                found_he_raw = true;
                raw_off = soff;
                raw_sz = sbc;
                meta.width = w;
                meta.height = h;
                if (bps > 0) meta.bit_depth = static_cast<uint16_t>(bps);
                if (has_cfa) std::memcpy(meta.cfa_pattern, cfa, 4);
                break;
            }
        }
    }

    if (!found_he_raw && saw_sony_tag) {
        return detail::parse_sony_arw_tiff(
            fp, file_size, be, ifd0_next_off, ifd0, subifds, exif_ifd_off, clip, meta, err);
    }

    if (!found_he_raw) {
        err = saw_34713 ? "NEF uses legacy Nikon compression rather than HE/HE* TicoRAW"
                        : "TIFF contains no supported TicoRAW (34713) or Sony ARW6 (32766) CFA SubIFD";
        return false;
    }

    if (exif_ifd_off > 0) {
        std::vector<TiffTagValue> exif;
        if (read_tiff_ifd(fp, 0, file_size, be, exif_ifd_off, exif)) {
            for (const auto& tv : exif) {
                double rval = 0.0;
                if (tv.tag == 33434 && tag_as_rational(tv, be, rval)) {
                    meta.exposure_time_s = static_cast<float>(rval);
                } else if (tv.tag == 33437 && tag_as_rational(tv, be, rval)) {
                    meta.fnumber = static_cast<float>(rval);
                } else if ((tv.tag == 34855 || tv.tag == 34866) && meta.iso == 0) {
                    meta.iso = tag_as_u32(tv, be);
                } else if (tv.tag == 37386 && tag_as_rational(tv, be, rval)) {
                    meta.focal_length_mm = static_cast<float>(rval);
                } else if (tv.tag == 42036 && tv.dtype == 2 && !tv.data.empty()) {
                    clip.lens_model = rstrip_ascii(tv.data.data(), tv.data.size());
                } else if (tv.tag == 37500 && tv.file_offset > 0 && tv.data.size() >= 18 &&
                           std::memcmp(tv.data.data(), "Nikon\0", 6) == 0) {
                    bool mn_le = (tv.data[10] == 'I' && tv.data[11] == 'I');
                    bool mn_be = (tv.data[10] == 'M' && tv.data[11] == 'M');
                    if (mn_le || mn_be) {
                        uint16_t mn_magic = rd_u16_e(tv.data.data() + 12, mn_be);
                        uint32_t mn_ifd = rd_u32_e(tv.data.data() + 14, mn_be);
                        if (mn_magic == 42) {
                            std::vector<TiffTagValue> mn_tags;
                            if (read_tiff_ifd(fp, tv.file_offset + 10, file_size, mn_be, mn_ifd, mn_tags)) {
                                for (const auto& mt : mn_tags) {
                                    apply_makernote_tag(
                                        mt.tag, mt.dtype, mt.count, mt.data.data(), mt.data.size(), mn_be, meta);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (meta.has_crop) {
        clip.nctg_active_w = meta.crop[2];
        clip.nctg_active_h = meta.crop[3];
    }
    clip.picture_control_base = meta.picture_control_base;
    clip.picture_control_name = meta.picture_control_name;
    clip.nctg_kelvin = meta.color_temp;

    meta.sample_offset = raw_off;
    meta.sample_size = raw_sz;
    meta.codestream_offset = raw_off;
    meta.codestream_size = raw_sz;

    clip.major_brand = "NEF";
    clip.video_samples.push_back({raw_off, raw_sz});
    clip.video_track_index = 0;

    NevTrackSummary tr;
    tr.track_id = 1;
    tr.handler_type = "vide";
    if (meta.nef_compression == 13) tr.format = "HE";
    else if (meta.nef_compression == 14) tr.format = "HE*";
    else tr.format = "HE/HE*";
    tr.sample_count = 1;
    tr.width = meta.width;
    tr.height = meta.height;
    clip.tracks.push_back(tr);

    clip.has_single_frame_meta = true;
    clip.single_frame_meta = meta;
    return true;
}

void parse_nrfh(const uint8_t* buf, size_t len, NevFrameMeta& meta) {
    size_t pos = 0;
    while (pos + 8 <= len) {
        uint32_t sz = rd_be32(buf + pos);
        if (sz < 8 || pos + sz > len) {
            break;
        }
        const uint8_t* fcc = buf + pos + 4;
        const uint8_t* body = buf + pos + 8;
        size_t blen = sz - 8;
        if (std::memcmp(fcc, "NRMT", 4) == 0 && blen >= 5) {
            uint16_t grp = rd_be16(body);
            uint16_t tag = rd_be16(body + 2);
            const uint8_t* v = body + 5;
            size_t vlen = blen - 5;
            if (grp == 0x0110) {
                if (tag == 0x0100 && vlen == 4) meta.width = rd_be32(v);
                else if (tag == 0x0101 && vlen == 4) meta.height = rd_be32(v);
                else if (tag == 0x0102 && vlen == 2) meta.bit_depth = rd_be16(v);
                else if (tag == 0x0112 && vlen == 2) meta.orientation = rd_be16(v);
                else if (tag == 0x829a && vlen == 4) meta.exposure_time_s = rd_be_f32(v);
                else if (tag == 0x829d && vlen == 4) meta.fnumber = rd_be_f32(v);
                else if (tag == 0x8832 && vlen == 4) meta.iso = rd_be32(v);
                else if (tag == 0x920a && vlen == 4) meta.focal_length_mm = rd_be_f32(v);
                else if (tag == 0xa302 && vlen == 8) {
                    std::memcpy(meta.cfa_pattern, v + 4, 4);
                }
            } else if (grp == 0x0200 && tag == 0x0001) {
                parse_sequential_makernote(v, vlen, meta);
            }
        }
        pos += sz;
    }
}

} // namespace

bool parse_nev_clip(const std::string& path, NevClipInfo& clip, std::string& err) {
    clip = NevClipInfo();
    clip.file_path = path;

    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || st.st_size <= 0) {
        err = "Cannot stat input file: " + path;
        return false;
    }
    clip.file_size = static_cast<uint64_t>(st.st_size);

    ScopedFile sf(path);
    if (!sf.fp) {
        err = "Cannot open input file: " + path;
        return false;
    }

    uint8_t magic4[4] = {0, 0, 0, 0};
    if (clip.file_size >= 8 && seek_read(sf.fp, 0, magic4, 4)) {
        bool is_le_tiff = (magic4[0] == 'I' && magic4[1] == 'I' && magic4[2] == 42 && magic4[3] == 0);
        bool is_be_tiff = (magic4[0] == 'M' && magic4[1] == 'M' && magic4[2] == 0 && magic4[3] == 42);
        if (is_le_tiff || is_be_tiff) {
            return parse_tiff_raw_clip(sf.fp, clip.file_size, is_be_tiff, clip, err);
        }
    }

    bool seen_ftyp = false;
    bool seen_moov = false;
    uint64_t pos = 0;
    while (pos + 8 <= clip.file_size) {
        BoxHeader bh;
        if (!read_box_header(sf.fp, pos, clip.file_size, bh)) {
            err = "Invalid top-level MOV box at offset " + std::to_string(pos);
            return false;
        }
        uint64_t pay_off = bh.offset + bh.header_size;
        uint64_t pay_end = bh.offset + bh.size;

        if (std::strcmp(bh.type, "ftyp") == 0) {
            seen_ftyp = true;
            uint8_t buf[16] = {0};
            size_t r = static_cast<size_t>(std::min<uint64_t>(sizeof(buf), pay_end - pay_off));
            if (seek_read(sf.fp, pay_off, buf, r) && r >= 8) {
                clip.major_brand = fourcc_to_str(buf);
                clip.minor_version = rd_be32(buf + 4);
            }
        } else if (std::strcmp(bh.type, "moov") == 0) {
            seen_moov = true;
            uint64_t mpos = pay_off;
            while (mpos + 8 <= pay_end) {
                BoxHeader mb;
                if (!read_box_header(sf.fp, mpos, pay_end, mb)) {
                    err = "Invalid box inside moov";
                    return false;
                }
                uint64_t m_off = mb.offset + mb.header_size;
                uint64_t m_end = mb.offset + mb.size;
                if (std::strcmp(mb.type, "mvhd") == 0) {
                    uint8_t buf[36] = {0};
                    size_t r = static_cast<size_t>(std::min<uint64_t>(sizeof(buf), m_end - m_off));
                    if (seek_read(sf.fp, m_off, buf, r) && r >= 24) {
                        uint8_t ver = buf[0];
                        if (ver == 0) {
                            clip.movie_timescale = rd_be32(buf + 12);
                            clip.movie_duration = rd_be32(buf + 16);
                        } else if (ver == 1 && r >= 32) {
                            clip.movie_timescale = rd_be32(buf + 20);
                            clip.movie_duration = rd_be64(buf + 24);
                        }
                    }
                } else if (std::strcmp(mb.type, "trak") == 0) {
                    RawTrackData tr;
                    if (!parse_trak(sf.fp, m_off, m_end, tr, err)) {
                        return false;
                    }
                    int t_idx = static_cast<int>(clip.tracks.size());
                    clip.tracks.push_back(tr.summary);
                    if (tr.summary.handler_type == "vide" && tr.summary.format == "NRAW") {
                        clip.video_track_index = t_idx;
                        if (!build_sample_table(tr, clip.file_size, clip.video_samples, err)) {
                            return false;
                        }
                        if (clip.fps <= 0.0 && !tr.stts.empty() && tr.stts[0].second > 0 && tr.summary.timescale > 0) {
                            clip.fps = static_cast<double>(tr.summary.timescale) /
                                       static_cast<double>(tr.stts[0].second);
                        }
                    }
                } else if (std::strcmp(mb.type, "udta") == 0) {
                    uint64_t upos = m_off;
                    while (upos + 8 <= m_end) {
                        BoxHeader ub;
                        if (!read_box_header(sf.fp, upos, m_end, ub)) {
                            break;
                        }
                        if (std::strcmp(ub.type, "NCDT") == 0) {
                            parse_ncdt(sf.fp, ub.offset + ub.header_size, ub.offset + ub.size, clip);
                        }
                        upos += ub.size;
                    }
                }
                mpos += mb.size;
            }
        }
        pos += bh.size;
    }

    if (!seen_ftyp || !seen_moov || clip.video_track_index < 0) {
        err = "Not a valid Nikon NRAW (.NEV) clip";
        return false;
    }
    return true;
}

bool read_nev_frame_meta_and_codestream(
    const NevClipInfo& clip,
    uint32_t frame_index,
    NevFrameMeta& meta,
    std::vector<uint8_t>* codestream_out,
    std::string& err) {
    if (frame_index >= clip.video_samples.size()) {
        err = "Frame index out of range (" + std::to_string(frame_index) + " >= " +
              std::to_string(clip.video_samples.size()) + ")";
        return false;
    }

    if (clip.has_single_frame_meta) {
        meta = clip.single_frame_meta;
        meta.frame_index = frame_index;
        if (codestream_out) {
            ScopedFile sf(clip.file_path);
            if (!sf.fp) {
                err = "Cannot reopen input file: " + clip.file_path;
                return false;
            }
            codestream_out->resize(meta.codestream_size);
            if (!seek_read(sf.fp, meta.codestream_offset, codestream_out->data(), meta.codestream_size)) {
                err = "Failed reading raw codestream bytes";
                return false;
            }
        }
        return true;
    }

    const NevSampleLocation& loc = clip.video_samples[frame_index];
    if (loc.sample_size < 32) {
        err = "Corrupt NRAW video sample (size < 32)";
        return false;
    }

    ScopedFile sf(clip.file_path);
    if (!sf.fp) {
        err = "Cannot reopen NEV file: " + clip.file_path;
        return false;
    }

    std::vector<uint8_t> head_buf(std::min<uint32_t>(loc.sample_size, 4096u));
    if (!seek_read(sf.fp, loc.file_offset, head_buf.data(), head_buf.size())) {
        err = "Failed reading NRAW sample header";
        return false;
    }

    uint32_t outer_sz = rd_be32(head_buf.data());
    if (std::memcmp(head_buf.data() + 4, "NRAW", 4) != 0 || outer_sz != loc.sample_size) {
        err = "Missing outer NRAW sample box at frame " + std::to_string(frame_index);
        return false;
    }

    meta = NevFrameMeta();
    meta.frame_index = frame_index;
    meta.sample_offset = loc.file_offset;
    meta.sample_size = loc.sample_size;
    meta.picture_control_name = clip.picture_control_name;
    meta.picture_control_base = clip.picture_control_base;

    uint32_t jxs_rel_off = 0;
    uint32_t jxs_len = 0;
    size_t pos = 8;
    while (pos + 8 <= head_buf.size()) {
        uint32_t sz = rd_be32(head_buf.data() + pos);
        const uint8_t* fcc = head_buf.data() + pos + 4;
        if (sz >= 8 && pos + sz <= loc.sample_size &&
            (std::memcmp(fcc, "NRFM", 4) == 0 ||
             std::memcmp(fcc, "NRFH", 4) == 0 ||
             std::memcmp(fcc, "NRTH", 4) == 0)) {
            if (std::memcmp(fcc, "NRFH", 4) == 0 && pos + sz <= head_buf.size()) {
                parse_nrfh(head_buf.data() + pos + 8, sz - 8, meta);
            } else if (std::memcmp(fcc, "NRTH", 4) == 0 && pos + sz <= head_buf.size()) {
                // NRTH wraps NRTI (16B) + NRTA
                size_t p2 = pos + 8;
                size_t e2 = pos + sz;
                while (p2 + 8 <= e2) {
                    uint32_t s2 = rd_be32(head_buf.data() + p2);
                    if (s2 < 8 || p2 + s2 > e2) break;
                    if (std::memcmp(head_buf.data() + p2 + 4, "NRTI", 4) == 0 && s2 >= 16) {
                        jxs_len = rd_be32(head_buf.data() + p2 + 12);
                    }
                    p2 += s2;
                }
            }
            pos += sz;
        } else {
            break;
        }
    }

    jxs_rel_off = static_cast<uint32_t>(pos);
    if (jxs_rel_off >= loc.sample_size) {
        err = "Failed locating codestream offset inside NRAW sample";
        return false;
    }
    if (jxs_len == 0 || jxs_rel_off + jxs_len > loc.sample_size) {
        jxs_len = loc.sample_size - jxs_rel_off;
    }

    meta.codestream_offset = loc.file_offset + jxs_rel_off;
    meta.codestream_size = jxs_len;

    if (codestream_out) {
        codestream_out->resize(jxs_len);
        if (!seek_read(sf.fp, meta.codestream_offset, codestream_out->data(), jxs_len)) {
            err = "Failed reading TicoRAW codestream bytes";
            return false;
        }
    }
    return true;
}

bool extract_nev_blob(
    const std::string& path,
    uint64_t offset,
    uint32_t size,
    std::vector<uint8_t>& out,
    std::string& err) {
    if (size == 0) {
        err = "Requested blob does not exist in clip";
        return false;
    }
    ScopedFile sf(path);
    if (!sf.fp) {
        err = "Cannot open NEV file: " + path;
        return false;
    }
    out.resize(size);
    if (!seek_read(sf.fp, offset, out.data(), size)) {
        err = "Failed reading embedded blob";
        return false;
    }
    return true;
}

} // namespace nijinwvdc
