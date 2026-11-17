// SPDX-License-Identifier: Apache-2.0
#include "../include/nijinWVDC.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace nijinwvdc {
namespace {

enum TiffType : uint16_t {
    T_BYTE = 1,
    T_ASCII = 2,
    T_SHORT = 3,
    T_LONG = 4,
    T_RATIONAL = 5,
    T_SRATIONAL = 10,
};

inline size_t tiff_type_size(uint16_t t) {
    switch (t) {
        case T_BYTE:
        case T_ASCII:
            return 1;
        case T_SHORT:
            return 2;
        case T_LONG:
            return 4;
        case T_RATIONAL:
        case T_SRATIONAL:
            return 8;
        default:
            return 1;
    }
}

inline void put_le16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

inline void put_le32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

struct IfdEntry {
    uint16_t tag = 0;
    uint16_t type = 0;
    uint32_t count = 0;
    uint32_t value_or_offset = 0;
    std::vector<uint8_t> inline_bytes;
    std::vector<uint8_t> ool_bytes;
};

struct IfdBuilder {
    std::vector<IfdEntry> entries;

    void add(uint16_t tag, uint16_t type, uint32_t count, const void* payload) {
        IfdEntry e;
        e.tag = tag;
        e.type = type;
        e.count = count;
        size_t sz = tiff_type_size(type) * static_cast<size_t>(count);
        const uint8_t* src = static_cast<const uint8_t*>(payload);
        if (sz <= 4) {
            e.inline_bytes.assign(4, 0);
            if (src && sz > 0) {
                std::memcpy(e.inline_bytes.data(), src, sz);
            }
        } else {
            e.ool_bytes.assign(src, src + sz);
            if (e.ool_bytes.size() & 1u) {
                e.ool_bytes.push_back(0);
            }
        }
        entries.push_back(std::move(e));
    }

    void add_u16(uint16_t tag, uint16_t v) {
        uint8_t p[2];
        put_le16(p, v);
        add(tag, T_SHORT, 1, p);
    }

    void add_u32(uint16_t tag, uint32_t v) {
        uint8_t p[4];
        put_le32(p, v);
        add(tag, T_LONG, 1, p);
    }

    void add_ascii(uint16_t tag, const std::string& s) {
        add(tag, T_ASCII, static_cast<uint32_t>(s.size() + 1), s.c_str());
    }
};

std::string to_lower_copy(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

struct CameraMatrixEntry {
    const char* key;
    double cm[9];
};

bool lookup_sony_d65_colormatrix(const std::string& lm, double out_cm[9]) {
    static const CameraMatrixEntry kSonyTable[] = {
        {"ilce-7m5",  {0.9089, -0.3577, -0.0787, -0.3563, 1.1326, 0.2557, -0.0114, 0.0928, 0.5904}},
        {"ilce-7rm6", {1.1765, -0.5595, -0.1192, -0.3689, 1.1507, 0.2485,  0.0051, 0.0681, 0.5731}},
        {"ilce-7rm5", {0.8200, -0.2976, -0.0719, -0.4296, 1.2053, 0.2532, -0.0429, 0.1282, 0.5774}},
        {"ilce-7m4",  {0.7460, -0.2365, -0.0588, -0.5687, 1.3442, 0.2474, -0.0624, 0.1156, 0.6584}},
        {"ilce-7cr",  {0.8200, -0.2976, -0.0719, -0.4296, 1.2053, 0.2532, -0.0429, 0.1282, 0.5774}},
        {"ilce-7cm2", {0.7460, -0.2365, -0.0588, -0.5687, 1.3442, 0.2474, -0.0624, 0.1156, 0.6584}},
        {"ilce-1m2",  {1.0279, -0.3899, -0.1462, -0.7108, 1.6980, -0.0168, -0.0168, 0.0544, 0.6116}},
        {"ilce-1",    {0.8161, -0.2947, -0.0739, -0.4811, 1.2668, 0.2389, -0.0437, 0.1229, 0.6524}},
        {"ilce-9m3",  {0.9811, -0.3908, -0.0752, -0.3704, 1.1577, 0.2417, -0.0073, 0.0950, 0.5980}},
        {"zv-e1",     {0.6912, -0.2127, -0.0469, -0.4470, 1.2175, 0.2587, -0.0398, 0.1478, 0.6492}},
        {"ilme-fx3",  {0.6912, -0.2127, -0.0469, -0.4470, 1.2175, 0.2587, -0.0398, 0.1478, 0.6492}},
    };
    for (const auto& ent : kSonyTable) {
        if (lm.find(ent.key) != std::string::npos) {
            std::memcpy(out_cm, ent.cm, sizeof(ent.cm));
            return true;
        }
    }
    std::memcpy(out_cm, kSonyTable[0].cm, sizeof(kSonyTable[0].cm));
    return false;
}

void populate_dng_options_common(
    const NevClipInfo& clip,
    const NevFrameMeta& meta,
    uint32_t width,
    uint32_t height,
    uint16_t bit_depth,
    bool apply_active_crop,
    uint16_t override_orientation,
    DngWriteOptions& opts) {
    opts.width = width;
    opts.height = height;
    opts.bit_depth = bit_depth;
    opts.white = (meta.white_level > 0)
                     ? meta.white_level
                     : static_cast<uint16_t>((1u << opts.bit_depth) - 1u);

    if (meta.has_black) {
        std::memcpy(opts.black, meta.black, sizeof(opts.black));
    }

    if (override_orientation >= 1 && override_orientation <= 8) {
        opts.orientation = override_orientation;
    } else if (meta.orientation >= 1 && meta.orientation <= 8) {
        opts.orientation = meta.orientation;
    } else {
        opts.orientation = 1;
    }

    std::memcpy(opts.cfa_pattern, meta.cfa_pattern, 4);

    if (!apply_active_crop && meta.has_crop &&
        meta.crop[2] > 0 && meta.crop[3] > 0 &&
        static_cast<uint32_t>(meta.crop[0]) + meta.crop[2] <= opts.width &&
        static_cast<uint32_t>(meta.crop[1]) + meta.crop[3] <= opts.height) {
        opts.has_crop = true;
        opts.crop_origin[0] = meta.crop[0];
        opts.crop_origin[1] = meta.crop[1];
        opts.crop_size[0] = meta.crop[2];
        opts.crop_size[1] = meta.crop[3];
    }

    const bool is_sony = (clip.major_brand == "ARW") ||
                         (to_lower_copy(clip.make).find("sony") != std::string::npos);
    opts.make = clip.make.empty() ? (is_sony ? "SONY" : "NIKON CORPORATION") : clip.make;
    opts.model = clip.model.empty() ? (is_sony ? "ILCE-7M5" : "NIKON Z 8") : clip.model;
    opts.unique_model = opts.model;
    opts.software = clip.firmware_version.empty() ? "nijinWVDC" : clip.firmware_version;
    opts.datetime = clip.datetime;
    opts.exposure_time_s = meta.exposure_time_s;
    opts.fnumber = meta.fnumber;

    if (meta.has_wb && meta.wb_rggb[0] > 0.0 && meta.wb_rggb[3] > 0.0) {
        double g_avg = 0.5 * (meta.wb_rggb[1] + meta.wb_rggb[2]);
        if (g_avg <= 0.0) g_avg = 1.0;
        opts.as_shot_neutral[0] = g_avg / meta.wb_rggb[0];
        opts.as_shot_neutral[1] = 1.0;
        opts.as_shot_neutral[2] = g_avg / meta.wb_rggb[3];
    } else {
        opts.as_shot_neutral[0] = 0.477230;
        opts.as_shot_neutral[1] = 1.0;
        opts.as_shot_neutral[2] = 0.827337;
    }
    lookup_camera_d65_colormatrix(opts.make, opts.model, opts.color_matrix1);
}

} // namespace

bool lookup_nikon_d65_colormatrix(const std::string& model, double out_cm[9]) {
    static const CameraMatrixEntry kTable[] = {
        {"z 8",   { 1.1423, -0.4564, -0.1123, -0.4816, 1.2895, 0.2119, -0.0210, 0.1061, 0.7282}},
        {"z 9",   { 1.3389, -0.6049, -0.1441, -0.4544, 1.2757, 0.1969,  0.0229, 0.0498, 0.7390}},
        {"z6_3",  { 1.1206, -0.4286, -0.0941, -0.4879, 1.2847, 0.2251, -0.0745, 0.1654, 0.7374}},
        {"zr",    { 1.1206, -0.4286, -0.0941, -0.4879, 1.2847, 0.2251, -0.0745, 0.1654, 0.7374}},
        {"z f",   { 1.1607, -0.4491, -0.0977, -0.4522, 1.2460, 0.2304, -0.0458, 0.1519, 0.7616}},
        {"z50_2", { 1.1640, -0.4829, -0.1079, -0.5107, 1.3006, 0.2325, -0.0972, 0.1711, 0.7380}},
        {"z5_2",  { 0.9874, -0.3784, -0.0823, -0.4728, 1.2673, 0.2286, -0.0648, 0.1513, 0.6375}},
    };
    std::string lm = to_lower_copy(model);
    for (const auto& ent : kTable) {
        if (lm.find(ent.key) != std::string::npos) {
            std::memcpy(out_cm, ent.cm, sizeof(ent.cm));
            return true;
        }
    }
    // Fallback to Nikon Z 8 D65 matrix
    std::memcpy(out_cm, kTable[0].cm, sizeof(kTable[0].cm));
    return false;
}

bool lookup_camera_d65_colormatrix(const std::string& make, const std::string& model, double out_cm[9]) {
    std::string lmake = to_lower_copy(make);
    std::string lmodel = to_lower_copy(model);
    if (lmake.find("sony") != std::string::npos ||
        lmodel.find("ilce-") != std::string::npos ||
        lmodel.find("ilme-") != std::string::npos ||
        lmodel.find("zv-e") != std::string::npos) {
        return lookup_sony_d65_colormatrix(lmodel, out_cm);
    }
    return lookup_nikon_d65_colormatrix(model, out_cm);
}

void build_dng_options_from_clip_and_frame(
    const NevClipInfo& clip,
    const NevFrameMeta& meta,
    const TicoRawInfo& jxs,
    bool apply_active_crop,
    uint16_t override_orientation,
    DngWriteOptions& opts) {
    opts = DngWriteOptions();
    uint16_t bd = static_cast<uint16_t>(jxs.cdt_depth > 0 ? jxs.cdt_depth : meta.bit_depth);
    populate_dng_options_common(
        clip, meta, jxs.width, jxs.height, bd, apply_active_crop, override_orientation, opts);
    if (!meta.has_black && jxs.offset_16 > 0 && jxs.out_bitdepth >= opts.bit_depth) {
        uint16_t blk = static_cast<uint16_t>(jxs.offset_16 >> (jxs.out_bitdepth - opts.bit_depth));
        opts.black[0] = opts.black[1] = opts.black[2] = opts.black[3] = blk;
    }
}

void build_dng_options_from_clip_and_frame(
    const NevClipInfo& clip,
    const NevFrameMeta& meta,
    const Arw6Info& arw6,
    bool apply_active_crop,
    uint16_t override_orientation,
    DngWriteOptions& opts) {
    opts = DngWriteOptions();
    uint16_t bd = (arw6.bit_depth > 0) ? arw6.bit_depth : meta.bit_depth;
    if (!meta.has_black) {
        opts.black[0] = opts.black[1] = opts.black[2] = opts.black[3] = 512;
    }
    populate_dng_options_common(
        clip, meta, arw6.width, arw6.height, bd, apply_active_crop, override_orientation, opts);
}

bool apply_bayer_crop(
    const uint16_t* src,
    uint32_t src_w,
    uint32_t src_h,
    const uint16_t crop_xywh[4],
    const uint8_t src_cfa[4],
    std::vector<uint16_t>& dst,
    uint8_t dst_cfa[4]) {
    uint32_t x0 = crop_xywh[0];
    uint32_t y0 = crop_xywh[1];
    uint32_t cw = crop_xywh[2];
    uint32_t ch = crop_xywh[3];
    if (!src || cw == 0 || ch == 0 || x0 + cw > src_w || y0 + ch > src_h) {
        return false;
    }
    dst.resize(static_cast<size_t>(cw) * static_cast<size_t>(ch));
    for (uint32_t r = 0; r < ch; ++r) {
        std::memcpy(
            dst.data() + static_cast<size_t>(r) * cw,
            src + static_cast<size_t>(y0 + r) * src_w + x0,
            static_cast<size_t>(cw) * sizeof(uint16_t));
    }
    uint32_t dy = y0 & 1u;
    uint32_t dx = x0 & 1u;
    for (uint32_t ry = 0; ry < 2; ++ry) {
        for (uint32_t rx = 0; rx < 2; ++rx) {
            dst_cfa[ry * 2 + rx] = src_cfa[((ry ^ dy) * 2) + (rx ^ dx)];
        }
    }
    return true;
}

bool write_cfa_dng(
    const std::string& path,
    const uint16_t* pixels,
    const DngWriteOptions& opts,
    std::string& err) {
    if (!pixels || opts.width == 0 || opts.height == 0) {
        err = "Invalid image dimensions for DNG output";
        return false;
    }

    const uint64_t strip_bytes64 = static_cast<uint64_t>(opts.width) * static_cast<uint64_t>(opts.height) * 2ULL;
    if (strip_bytes64 > 0xFFFFFFFFULL) {
        err = "Image strip exceeds 32-bit TIFF StripByteCounts";
        return false;
    }
    const uint32_t strip_bytes = static_cast<uint32_t>(strip_bytes64);

    IfdBuilder b;
    b.add_u32(254, 0u);                                    // NewSubfileType
    b.add_u32(256, opts.width);                            // ImageWidth
    b.add_u32(257, opts.height);                           // ImageLength
    b.add_u16(258, 16u);                                   // BitsPerSample
    b.add_u16(259, 1u);                                    // Compression = None
    b.add_u16(262, 32803u);                                // Photometric = CFA
    if (!opts.make.empty()) b.add_ascii(271, opts.make);   // Make
    if (!opts.model.empty()) b.add_ascii(272, opts.model); // Model
    // Placeholder for StripOffsets (273) updated after laying out OOL data
    size_t strip_offset_idx = b.entries.size();
    b.add_u32(273, 0u);
    b.add_u16(274, (opts.orientation >= 1 && opts.orientation <= 8) ? opts.orientation : 1u);
    b.add_u16(277, 1u);                                    // SamplesPerPixel
    b.add_u32(278, opts.height);                           // RowsPerStrip
    b.add_u32(279, strip_bytes);                           // StripByteCounts
    b.add_u16(284, 1u);                                    // PlanarConfiguration
    if (!opts.software.empty()) b.add_ascii(305, opts.software);
    if (opts.datetime.size() == 19) b.add_ascii(306, opts.datetime);
    {
        uint8_t dim[4];
        put_le16(dim, 2);
        put_le16(dim + 2, 2);
        b.add(33421, T_SHORT, 2, dim);                     // CFARepeatPatternDim
    }
    b.add(33422, T_BYTE, 4, opts.cfa_pattern);             // CFAPattern
    if (opts.exposure_time_s > 0.0f) {
        uint8_t rat[8];
        uint32_t den = 1000000u;
        uint32_t num = static_cast<uint32_t>(std::llround(static_cast<double>(opts.exposure_time_s) * den));
        put_le32(rat, num);
        put_le32(rat + 4, den);
        b.add(33434, T_RATIONAL, 1, rat);                  // ExposureTime
    }
    if (opts.fnumber > 0.0f) {
        uint8_t rat[8];
        uint32_t den = 100u;
        uint32_t num = static_cast<uint32_t>(std::llround(static_cast<double>(opts.fnumber) * den));
        put_le32(rat, num);
        put_le32(rat + 4, den);
        b.add(33437, T_RATIONAL, 1, rat);                  // FNumber
    }
    {
        const uint8_t ver[4] = {1, 4, 0, 0};
        b.add(50706, T_BYTE, 4, ver);                      // DNGVersion
        const uint8_t bver[4] = {1, 1, 0, 0};
        b.add(50707, T_BYTE, 4, bver);                     // DNGBackwardVersion
    }
    b.add_ascii(50708, opts.unique_model.empty() ? "NIKON Z 8" : opts.unique_model);
    {
        const uint8_t pc[3] = {0, 1, 2};
        b.add(50710, T_BYTE, 3, pc);                       // CFAPlaneColor
    }
    b.add_u16(50711, 1u);                                  // CFALayout = Rectangular
    const bool uniform_black = (opts.black[0] == opts.black[1] &&
                                opts.black[1] == opts.black[2] &&
                                opts.black[2] == opts.black[3]);
    if (uniform_black) {
        uint8_t blk[8];
        put_le32(blk, opts.black[0]);
        put_le32(blk + 4, 1u);
        b.add(50714, T_RATIONAL, 1, blk);                  // Scalar BlackLevel (1x1)
    } else {
        uint8_t bdim[4];
        put_le16(bdim, 2);
        put_le16(bdim + 2, 2);
        b.add(50713, T_SHORT, 2, bdim);                    // BlackLevelRepeatDim
        uint8_t blk[32];
        for (int i = 0; i < 4; ++i) {
            put_le32(blk + 8 * i, opts.black[i]);
            put_le32(blk + 8 * i + 4, 1u);
        }
        b.add(50714, T_RATIONAL, 4, blk);                  // BlackLevel 2x2
    }
    b.add_u16(50717, opts.white);                          // WhiteLevel (pre-subtraction saturation)
    if (opts.has_crop && opts.crop_size[0] > 0 && opts.crop_size[1] > 0) {
        uint8_t co[4], cs[4];
        put_le16(co, opts.crop_origin[0]);
        put_le16(co + 2, opts.crop_origin[1]);
        put_le16(cs, opts.crop_size[0]);
        put_le16(cs + 2, opts.crop_size[1]);
        b.add(50719, T_SHORT, 2, co);                      // DefaultCropOrigin
        b.add(50720, T_SHORT, 2, cs);                      // DefaultCropSize
    }
    {
        uint8_t cm1[72];
        for (int i = 0; i < 9; ++i) {
            int64_t v = std::llround(opts.color_matrix1[i] * 10000.0);
            put_le32(cm1 + 8 * i, static_cast<uint32_t>(v));
            put_le32(cm1 + 8 * i + 4, 10000u);
        }
        b.add(50721, T_SRATIONAL, 9, cm1);                 // ColorMatrix1 (XYZ->Cam)
    }
    {
        uint8_t asn[24];
        for (int i = 0; i < 3; ++i) {
            uint32_t num = static_cast<uint32_t>(std::llround(opts.as_shot_neutral[i] * 100000.0));
            put_le32(asn + 8 * i, num);
            put_le32(asn + 8 * i + 4, 100000u);
        }
        b.add(50728, T_RATIONAL, 3, asn);                  // AsShotNeutral
    }
    b.add_u16(50778, 21u);                                 // CalibrationIlluminant1 = D65

    // Verify strictly ascending TIFF tags
    for (size_t i = 1; i < b.entries.size(); ++i) {
        if (b.entries[i].tag <= b.entries[i - 1].tag) {
            err = "Internal error: non-ascending DNG tags";
            return false;
        }
    }

    const uint32_t ifd_off = 8u;
    const uint32_t data_base = ifd_off + 2u + static_cast<uint32_t>(b.entries.size()) * 12u + 4u;
    std::vector<uint8_t> ool_data;
    for (auto& ent : b.entries) {
        if (!ent.ool_bytes.empty()) {
            ent.value_or_offset = data_base + static_cast<uint32_t>(ool_data.size());
            ool_data.insert(ool_data.end(), ent.ool_bytes.begin(), ent.ool_bytes.end());
        }
    }
    // Pad OOL data to 16-byte alignment before pixel strip
    while ((data_base + ool_data.size()) & 15u) {
        ool_data.push_back(0);
    }
    const uint32_t strip_off = data_base + static_cast<uint32_t>(ool_data.size());
    put_le32(b.entries[strip_offset_idx].inline_bytes.data(), strip_off);

    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) {
        err = "Failed opening DNG output for writing: " + path;
        return false;
    }

    uint8_t tiff_hdr[8] = {'I', 'I', 42, 0, 8, 0, 0, 0};
    uint8_t cnt_buf[2];
    put_le16(cnt_buf, static_cast<uint16_t>(b.entries.size()));
    bool ok = (std::fwrite(tiff_hdr, 1, 8, fp) == 8) &&
              (std::fwrite(cnt_buf, 1, 2, fp) == 2);

    for (const auto& ent : b.entries) {
        uint8_t rec[12];
        put_le16(rec, ent.tag);
        put_le16(rec + 2, ent.type);
        put_le32(rec + 4, ent.count);
        if (!ent.ool_bytes.empty()) {
            put_le32(rec + 8, ent.value_or_offset);
        } else {
            std::memcpy(rec + 8, ent.inline_bytes.data(), 4);
        }
        if (ok && std::fwrite(rec, 1, 12, fp) != 12) ok = false;
    }
    uint8_t next_ifd[4] = {0, 0, 0, 0};
    if (ok && std::fwrite(next_ifd, 1, 4, fp) != 4) ok = false;
    if (ok && !ool_data.empty() && std::fwrite(ool_data.data(), 1, ool_data.size(), fp) != ool_data.size()) ok = false;
    if (ok && std::fwrite(pixels, 1, strip_bytes, fp) != strip_bytes) ok = false;

    if (std::fclose(fp) != 0) ok = false;
    if (!ok) {
        err = "Write failure while writing DNG: " + path;
        return false;
    }
    return true;
}

} // namespace nijinwvdc
