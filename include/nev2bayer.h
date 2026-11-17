// SPDX-License-Identifier: Apache-2.0
#ifndef NEV2BAYER_H
#define NEV2BAYER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nev2bayer {

struct NevSampleLocation {
    uint64_t file_offset = 0;
    uint32_t sample_size = 0;
};

struct NevTrackSummary {
    uint32_t track_id = 0;
    std::string handler_type;
    std::string format;
    uint32_t timescale = 0;
    uint64_t duration = 0;
    uint32_t sample_count = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    // Audio (QuickTime Sound v2 / LPCM)
    double audio_sample_rate = 0.0;
    uint32_t audio_channels = 0;
    uint32_t audio_bits_per_channel = 0;
    uint32_t audio_bytes_per_packet = 0;
    // Timecode
    uint32_t tmcd_flags = 0;
    uint32_t tmcd_timescale = 0;
    uint32_t tmcd_frame_duration = 0;
    uint32_t tmcd_num_frames = 0;
};

struct NevFrameMeta {
    uint32_t frame_index = 0;
    uint64_t sample_offset = 0;
    uint32_t sample_size = 0;
    uint64_t codestream_offset = 0;
    uint32_t codestream_size = 0;

    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t bit_depth = 14;
    uint16_t orientation = 1;
    float exposure_time_s = 0.0f;
    float fnumber = 0.0f;
    uint32_t iso = 0;
    float focal_length_mm = 0.0f;
    uint8_t cfa_pattern[4] = {0, 1, 1, 2};

    bool has_wb = false;
    double wb_rggb[4] = {1.0, 1.0, 1.0, 1.0}; // [R, G0, G1, B]
    bool has_black = false;
    uint16_t black[4] = {1008, 1008, 1008, 1008};
    uint16_t white_level = 0; // 0 = derive (1 << bit_depth) - 1
    bool has_crop = false;
    uint16_t crop[4] = {0, 0, 0, 0}; // left, top, width, height
    uint16_t color_temp = 0;
    uint16_t nef_compression = 0;
    uint16_t sony_raw_file_type = 0;
    std::string wb_mode;
    std::string picture_control_base;
    std::string picture_control_name;
};

struct NevClipInfo {
    std::string file_path;
    uint64_t file_size = 0;
    std::string major_brand;
    uint32_t minor_version = 0;
    uint32_t movie_timescale = 0;
    uint64_t movie_duration = 0;
    double fps = 0.0;

    // From NCDT / NCTG / TIFF IFD0 + Exif
    std::string make;
    std::string model;
    std::string firmware_version;
    std::string datetime;
    std::string lens_model;
    std::string picture_control_name;
    std::string picture_control_base;
    uint16_t nctg_active_w = 0;
    uint16_t nctg_active_h = 0;
    uint16_t nctg_kelvin = 0;

    // Embedded JPEGs in moov/udta/NCDT or TIFF previews
    uint64_t ncth_offset = 0;
    uint32_t ncth_size = 0;
    uint64_t ncvw_offset = 0;
    uint32_t ncvw_size = 0;
    uint64_t ncm1_offset = 0;
    uint32_t ncm1_size = 0;

    std::vector<NevTrackSummary> tracks;
    int video_track_index = -1;
    std::vector<NevSampleLocation> video_samples;

    bool has_single_frame_meta = false;
    NevFrameMeta single_frame_meta;
};

struct TicoRawInfo {
    uint32_t lcod = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t h_pad_pix = 0;
    uint16_t slh = 0;
    uint16_t slh_pix = 0;
    uint8_t nc = 0;
    uint8_t ng = 0;
    uint8_t ss = 0;
    uint8_t bw = 0;
    uint8_t fq = 0;
    uint8_t br = 0;
    uint8_t ppoc = 0;
    uint8_t ndecomp_h = 0;
    uint8_t ndecomp_v = 0;
    uint8_t quantizer_type = 0;
    uint8_t sign_strategy = 0;
    uint8_t run_mode = 0;
    bool is_long_pkt_hdr = false;
    bool interframe = false;
    bool gamma_enabled = false;
    int gamma_type = -1;
    int in_bitdepth = 0;
    int out_bitdepth = 0;
    int offset_16 = 0;
    int offset_sqrt = 0;
    int r_val = 0;
    int r_flip = 0;
    int cdt_depth = 0;
    int yh_hor_ndecomp = 0;
    int yh_ver_ndecomp = 0;
    int num_sb = 0;
    int num_slices = 0;
    int num_precincts = 0;
    int line_stride = 0;
};

struct Arw6TileInfo {
    uint64_t rel_offset = 0;
    uint32_t data_size = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct Arw6Info {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t codestream_size = 0;
    uint16_t bit_depth = 14;
    uint16_t internal_depth = 12;
    uint16_t coded_depth = 16;
    uint8_t num_comps = 3;
    uint8_t decomp_levels = 3;
    uint16_t subsampling_mask = 0x0200;
    std::vector<Arw6TileInfo> tiles;
};

struct DngWriteOptions {
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t bit_depth = 14;
    uint16_t black[4] = {1008, 1008, 1008, 1008};
    uint16_t white = 16383;
    uint16_t orientation = 1;
    uint8_t cfa_pattern[4] = {0, 1, 1, 2};
    bool has_crop = false;
    uint16_t crop_origin[2] = {0, 0}; // [left, top]
    uint16_t crop_size[2] = {0, 0};   // [width, height]
    std::string make;
    std::string model;
    std::string unique_model;
    std::string software;
    std::string datetime;
    float exposure_time_s = 0.0f;
    float fnumber = 0.0f;
    double as_shot_neutral[3] = {1.0, 1.0, 1.0};
    double color_matrix1[9] = {
         1.1423, -0.4564, -0.1123,
        -0.4816,  1.2895,  0.2119,
        -0.0210,  0.1061,  0.7282
    };
};

// Container parsing (64-bit file I/O)
bool parse_nev_clip(const std::string& path, NevClipInfo& clip, std::string& err);
bool read_nev_frame_meta_and_codestream(
    const NevClipInfo& clip,
    uint32_t frame_index,
    NevFrameMeta& meta,
    std::vector<uint8_t>* codestream_out,
    std::string& err);
bool extract_nev_blob(
    const std::string& path,
    uint64_t offset,
    uint32_t size,
    std::vector<uint8_t>& out,
    std::string& err);

// TicoRAW / JPEG XS R0 decoding
bool probe_ticoraw_header(
    const uint8_t* data,
    size_t len,
    TicoRawInfo& info,
    std::string& err);

bool decode_ticoraw_bayer(
    const uint8_t* data,
    size_t len,
    int num_threads,
    std::vector<uint16_t>& bayer_out,
    TicoRawInfo& info_out,
    std::string& err);

// Sony ARW6 (Compression=32766, SonyRawFileType=6) multi-tile & "A000" wavelet decoding
bool probe_arw6_codestream(
    const uint8_t* data,
    size_t len,
    Arw6Info& info,
    std::string& err);

bool probe_arw6_strip(
    const uint8_t* strip_data,
    size_t strip_len,
    Arw6Info& info,
    std::string& err);

bool decode_arw6_tile(
    const uint8_t* cs_data,
    size_t cs_len,
    int num_threads,
    uint32_t tile_x,
    uint32_t tile_y,
    uint32_t full_w,
    uint32_t full_h,
    std::vector<uint16_t>& bayer_out,
    Arw6Info& tile_info_out,
    std::string& err);

bool decode_arw6_bayer(
    const uint8_t* strip_data,
    size_t strip_len,
    int num_threads,
    std::vector<uint16_t>& bayer_out,
    Arw6Info& info_out,
    std::string& err);

// DNG 1.4 writer & camera matrix lookup
bool lookup_nikon_d65_colormatrix(const std::string& model, double out_cm[9]);
bool lookup_camera_d65_colormatrix(const std::string& make, const std::string& model, double out_cm[9]);
void build_dng_options_from_clip_and_frame(
    const NevClipInfo& clip,
    const NevFrameMeta& meta,
    const TicoRawInfo& jxs,
    bool apply_active_crop,
    uint16_t override_orientation,
    DngWriteOptions& opts);
void build_dng_options_from_clip_and_frame(
    const NevClipInfo& clip,
    const NevFrameMeta& meta,
    const Arw6Info& arw6,
    bool apply_active_crop,
    uint16_t override_orientation,
    DngWriteOptions& opts);
bool apply_bayer_crop(
    const uint16_t* src,
    uint32_t src_w,
    uint32_t src_h,
    const uint16_t crop_xywh[4],
    const uint8_t src_cfa[4],
    std::vector<uint16_t>& dst,
    uint8_t dst_cfa[4]);
bool write_cfa_dng(
    const std::string& path,
    const uint16_t* pixels,
    const DngWriteOptions& opts,
    std::string& err);

} // namespace nev2bayer

#endif // NEV2BAYER_H
