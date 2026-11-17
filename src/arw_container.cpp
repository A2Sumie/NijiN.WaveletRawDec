// SPDX-License-Identifier: Apache-2.0
#include "tiff_internal.h"

#include <algorithm>
#include <cstring>

namespace nijinwvdc {
namespace detail {

namespace {

inline int16_t rd_i16_e(const uint8_t* p, bool be) {
    return static_cast<int16_t>(rd_u16_e(p, be));
}

void parse_arw_exif_ifd(
    const RandomAccessReader& r,
    bool be,
    uint32_t exif_ifd_off,
    NevClipInfo& clip,
    NevFrameMeta& meta) {
    if (exif_ifd_off == 0) {
        return;
    }
    std::vector<TiffTagValue> exif;
    if (!read_tiff_ifd(r, 0, be, exif_ifd_off, exif)) {
        return;
    }
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
        }
    }
}

void consider_jpeg_preview(uint64_t off, uint32_t sz, uint64_t file_size, NevClipInfo& clip) {
    if (off == 0 || sz == 0 || off + sz > file_size) {
        return;
    }
    if (sz > clip.ncvw_size) {
        if (clip.ncvw_size > 0 && clip.ncth_size == 0) {
            clip.ncth_offset = clip.ncvw_offset;
            clip.ncth_size = clip.ncvw_size;
        }
        clip.ncvw_offset = off;
        clip.ncvw_size = sz;
    } else if (clip.ncth_size == 0 || sz < clip.ncth_size) {
        clip.ncth_offset = off;
        clip.ncth_size = sz;
    }
}

} // namespace

bool parse_sony_arw_tiff_reader(
    const RandomAccessReader& r,
    bool be,
    uint32_t ifd0_next_off,
    const std::vector<TiffTagValue>& ifd0,
    const std::vector<uint32_t>& subifds,
    uint32_t exif_ifd_off,
    NevClipInfo& clip,
    NevFrameMeta& meta,
    std::string& err) {
    const uint64_t file_size = r.size();
    // IFD0 may already hold the preview JPEG offset/size populated in clip.ncth_* by the TIFF dispatcher.
    if (clip.ncth_size > 0 && clip.ncth_offset + clip.ncth_size <= file_size) {
        uint64_t o = clip.ncth_offset;
        uint32_t s = clip.ncth_size;
        clip.ncth_offset = 0;
        clip.ncth_size = 0;
        consider_jpeg_preview(o, s, file_size, clip);
    }

    if (ifd0_next_off > 0) {
        std::vector<TiffTagValue> ifd1;
        if (read_tiff_ifd(r, 0, be, ifd0_next_off, ifd1)) {
            uint64_t joff = 0;
            uint32_t jsz = 0;
            for (const auto& tv : ifd1) {
                if (tv.tag == 513) joff = tag_as_u32(tv, be);
                else if (tv.tag == 514) jsz = tag_as_u32(tv, be);
            }
            consider_jpeg_preview(joff, jsz, file_size, clip);
        }
    }

    // Collect candidate image IFDs: SubIFDs first, plus IFD0 itself if SubIFDs is absent.
    std::vector<std::vector<TiffTagValue>> candidate_ifds;
    for (uint32_t sub_off : subifds) {
        std::vector<TiffTagValue> sifd;
        if (read_tiff_ifd(r, 0, be, sub_off, sifd)) {
            candidate_ifds.push_back(std::move(sifd));
        }
    }
    if (candidate_ifds.empty()) {
        candidate_ifds.push_back(ifd0);
    }

    bool found_arw6 = false;
    uint32_t other_comp = 0;
    uint16_t other_raw_type = 0;
    uint64_t raw_off = 0;
    uint32_t raw_sz = 0;

    for (const auto& sifd : candidate_ifds) {
        uint32_t w = 0, h = 0, bps = 0, comp = 0, phot = 0;
        uint32_t soff = 0, sbc = 0;
        uint32_t jpeg_off = 0, jpeg_sz = 0;
        uint16_t raw_type = 0;
        uint16_t white = 0;
        bool has_cfa = false;
        uint8_t cfa[4] = {0, 1, 1, 2};
        bool has_blk = false;
        uint16_t blk[4] = {512, 512, 512, 512};
        bool has_wb = false;
        double wb[4] = {1.0, 1.0, 1.0, 1.0};
        bool has_crop = false;
        uint16_t crop[4] = {0, 0, 0, 0};

        for (const auto& tv : sifd) {
            if (tv.tag == 256) w = tag_as_u32(tv, be);
            else if (tv.tag == 257) h = tag_as_u32(tv, be);
            else if (tv.tag == 258) bps = tag_as_u32(tv, be);
            else if (tv.tag == 259) comp = tag_as_u32(tv, be);
            else if (tv.tag == 262) phot = tag_as_u32(tv, be);
            else if (tv.tag == 273) soff = tag_as_u32(tv, be);
            else if (tv.tag == 279) sbc = tag_as_u32(tv, be);
            else if (tv.tag == 513) jpeg_off = tag_as_u32(tv, be);
            else if (tv.tag == 514) jpeg_sz = tag_as_u32(tv, be);
            else if (tv.tag == 33422 && tv.data.size() >= 4) {
                std::memcpy(cfa, tv.data.data(), 4);
                has_cfa = true;
            } else if (tv.tag == 0x7000) {
                raw_type = static_cast<uint16_t>(tag_as_u32(tv, be));
            } else if ((tv.tag == 0x7310 || tv.tag == 50714) && tv.dtype == 3 && tv.data.size() >= 8) {
                for (int c = 0; c < 4; ++c) {
                    blk[c] = rd_u16_e(tv.data.data() + 2 * c, be);
                }
                has_blk = true;
            } else if (tv.tag == 0x7313 && (tv.dtype == 8 || tv.dtype == 3) && tv.data.size() >= 8) {
                double tmp[4];
                bool ok_wb = true;
                for (int c = 0; c < 4; ++c) {
                    int16_t v = rd_i16_e(tv.data.data() + 2 * c, be);
                    if (v <= 0) ok_wb = false;
                    tmp[c] = static_cast<double>(v);
                }
                if (ok_wb) {
                    std::memcpy(wb, tmp, sizeof(wb));
                    has_wb = true;
                }
            } else if (tv.tag == 50717 && tv.data.size() >= 2) {
                white = static_cast<uint16_t>(tag_as_u32(tv, be));
            } else if ((tv.tag == 50719 || tv.tag == 0x74c7) && tv.data.size() >= 8) {
                // DefaultCropOrigin [left, top]
                if (!has_crop) {
                    crop[0] = static_cast<uint16_t>(rd_u32_e(tv.data.data(), be));
                    crop[1] = static_cast<uint16_t>(rd_u32_e(tv.data.data() + 4, be));
                }
            } else if ((tv.tag == 50720 || tv.tag == 0x74c8) && tv.data.size() >= 8) {
                // DefaultCropSize [width, height]
                crop[2] = static_cast<uint16_t>(rd_u32_e(tv.data.data(), be));
                crop[3] = static_cast<uint16_t>(rd_u32_e(tv.data.data() + 4, be));
                if (crop[2] > 0 && crop[3] > 0) has_crop = true;
            }
        }

        if (comp == 6 && jpeg_off > 0 && jpeg_sz > 0) {
            consider_jpeg_preview(jpeg_off, jpeg_sz, file_size, clip);
        }

        if (comp == 32766 && (phot == 32803 || phot == 0) && raw_type == 6 &&
            soff > 0 && sbc >= 0x68 && static_cast<uint64_t>(soff) + sbc <= file_size) {
            uint8_t thdr[0x68] = {};
            if (!r.read(soff, thdr, sizeof(thdr))) {
                err = "Failed reading ARW6 strip header";
                return false;
            }
            uint64_t tile_cnt = static_cast<uint64_t>(rd_le32(thdr)) |
                                (static_cast<uint64_t>(rd_le32(thdr + 4)) << 32);
            if (tile_cnt != 1 && tile_cnt != 4) {
                err = "Unsupported ARW6 tile count (" + std::to_string(tile_cnt) + ")";
                return false;
            }
            uint64_t rel0 = static_cast<uint64_t>(rd_le32(thdr + 8)) |
                            (static_cast<uint64_t>(rd_le32(thdr + 12)) << 32);
            if (rel0 < 8 + tile_cnt * 24 || rel0 + 16 > sbc) {
                err = "Invalid ARW6 tile offset table";
                return false;
            }
            uint8_t sig[4] = {};
            if (!r.read(static_cast<uint64_t>(soff) + rel0, sig, 4) ||
                std::memcmp(sig, "A000", 4) != 0) {
                err = "ARW SubIFD (Compression=32766) is missing A000 wavelet codestream";
                return false;
            }

            found_arw6 = true;
            raw_off = soff;
            raw_sz = sbc;
            meta.width = w;
            meta.height = h;
            if (bps > 0) meta.bit_depth = static_cast<uint16_t>(bps);
            if (has_cfa) std::memcpy(meta.cfa_pattern, cfa, 4);
            meta.sony_raw_file_type = raw_type;
            if (white > 0) meta.white_level = white;
            if (has_blk) {
                std::memcpy(meta.black, blk, sizeof(meta.black));
                meta.has_black = true;
            } else {
                meta.black[0] = meta.black[1] = meta.black[2] = meta.black[3] = 512;
                meta.has_black = true;
            }
            if (has_wb) {
                std::memcpy(meta.wb_rggb, wb, sizeof(meta.wb_rggb));
                meta.has_wb = true;
            }
            if (has_crop) {
                std::memcpy(meta.crop, crop, sizeof(meta.crop));
                meta.has_crop = true;
            }
            break;
        } else if (phot == 32803 && comp != 0) {
            other_comp = comp;
            other_raw_type = raw_type;
        }
    }

    if (!found_arw6) {
        if (other_comp != 0) {
            err = "Unsupported Sony ARW compression (" + std::to_string(other_comp) +
                  ", SonyRawFileType=" + std::to_string(other_raw_type) +
                  "); only ARW6 Compressed HQ / Compressed (Compression=32766, SonyRawFileType=6) is supported";
        } else {
            err = "ARW contains no ARW6 (Compression=32766, SonyRawFileType=6) CFA SubIFD";
        }
        return false;
    }

    parse_arw_exif_ifd(r, be, exif_ifd_off, clip, meta);

    if (meta.has_crop) {
        clip.nctg_active_w = meta.crop[2];
        clip.nctg_active_h = meta.crop[3];
    }

    meta.sample_offset = raw_off;
    meta.sample_size = raw_sz;
    meta.codestream_offset = raw_off;
    meta.codestream_size = raw_sz;

    clip.container_format = ContainerFormat::TiffArw;
    clip.codec_family = CodecFamily::Arw6;
    clip.major_brand = "ARW";
    clip.video_samples.push_back({raw_off, raw_sz});
    clip.video_track_index = 0;

    NevTrackSummary tr;
    tr.track_id = 1;
    tr.handler_type = "vide";
    tr.format = "ARW6";
    tr.sample_count = 1;
    tr.width = meta.width;
    tr.height = meta.height;
    clip.tracks.push_back(tr);

    clip.has_single_frame_meta = true;
    clip.single_frame_meta = meta;
    return true;
}

} // namespace detail
} // namespace nijinwvdc
