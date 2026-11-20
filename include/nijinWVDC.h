// SPDX-License-Identifier: Apache-2.0
#ifndef NIJINWVDC_H
#define NIJINWVDC_H

#include "nijinWVDC_export.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nijinwvdc {

NIJINWVDC_API const char* version_string();
NIJINWVDC_API uint32_t simd_capabilities();

enum class ContainerFormat : uint8_t {
    Unknown = 0,
    Mp4Nev = 1,
    TiffNef = 2,
    TiffArw = 3,
    RawCodestream = 4
};

enum class CodecFamily : uint8_t {
    Unknown = 0,
    TicoRaw = 1,
    Arw6 = 2
};

struct TrackChunkSpan {
    uint64_t offset = 0;
    uint32_t sample_count = 0;
};

struct SttsRun {
    uint32_t sample_count = 0;
    uint32_t sample_delta = 0;
};

struct IoCallbacks {
    void* user_data = nullptr;
    uint64_t file_size = 0;
    bool (*read_at)(void* user_data, uint64_t offset, void* dst, size_t size, size_t* bytes_read_out) = nullptr;
};

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
    uint16_t tkhd_orientation = 1;
    // Audio (QuickTime Sound v0/v1/v2 / LPCM)
    double audio_sample_rate = 0.0;
    uint32_t audio_channels = 0;
    uint32_t audio_bits_per_channel = 0;
    uint32_t audio_bytes_per_packet = 0;
    uint32_t audio_lpcm_flags = 0;
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
    float focal_length_opt_mm = 0.0f;
    float focus_distance_m = 0.0f;
    float focus_distance_low_m = 0.0f;
    float focus_distance_high_m = 0.0f;
    uint16_t focus_position = 0;
    int32_t lens_position_abs = 0;
    bool has_lens_position_abs = false;
    bool focus_is_infinity = false;
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
    std::string serial_number;
    uint32_t shutter_count = 0;
    bool has_shutter_count = false;
    std::vector<uint8_t> nikon_lens_data;
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

    ContainerFormat container_format = ContainerFormat::Unknown;
    CodecFamily codec_family = CodecFamily::Unknown;
    std::vector<SttsRun> video_stts;
    int audio_track_index = -1;
    std::vector<TrackChunkSpan> audio_chunks;
    uint64_t total_audio_frames = 0;
    int timecode_track_index = -1;
    std::vector<uint32_t> timecode_samples;
};

struct VideoFrameTiming {
    uint32_t frame_index = 0;
    uint32_t timescale = 0;
    uint64_t pts_ticks = 0;
    uint32_t duration_ticks = 0;
    double pts_seconds = 0.0;
    double duration_seconds = 0.0;
    bool has_timecode = false;
    bool drop_frame = false;
    uint8_t tc_hours = 0;
    uint8_t tc_minutes = 0;
    uint8_t tc_seconds = 0;
    uint8_t tc_frames = 0;
    char timecode_str[16] = {};
};

struct AudioFormatInfo {
    bool available = false;
    std::string format;
    uint32_t timescale = 0;
    double sample_rate = 0.0;
    uint32_t channels = 0;
    uint32_t bits_per_channel = 0;
    uint32_t bytes_per_packet = 0;
    uint32_t lpcm_flags = 0;
    bool is_signed = true;
    bool is_float = false;
    bool is_big_endian = false;
    uint64_t total_frames = 0;
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

struct WaveletQuantProfile {
    static constexpr uint32_t kNltSlopeLutSize = 256u;
    static constexpr uint32_t kScaleBins = 6u;
    static constexpr uint32_t kWalsh16Bins = 16u;

    bool available = false;
    CodecFamily codec = CodecFamily::Unknown;
    bool nlt_enabled = false;
    uint16_t raw_bit_depth = 14u;
    uint16_t code_bit_depth = 16u;
    uint32_t nlt_raw_white_adu = 16383u;
    uint32_t nlt_code_max = 65535u;

    float black_ref = 0.0f;
    float white_ref = 16383.0f;

    // Spatial row granularity in output Bayer rows (e.g. 4 for TicoRAW, 8 for ARW6).
    uint32_t row_step = 4u;
    uint32_t num_rows = 0u;

    // Normalized code-domain RMS quantization std per row block; multiply by
    // nlt_slope_lut(I) for linear [0, 1] normalized quantization std.
    std::vector<float> q_luma_code_row;
    std::vector<float> q_chroma_code_row;

    float mean_luma_code_std = 0.0f;
    float mean_chroma_code_std = 0.0f;
    float max_luma_code_std = 0.0f;
    float max_chroma_code_std = 0.0f;

    // Normalized spectral power across [DC/LL, coarse..fine, Nyquist/G_H] (sum == 1 when active).
    float luma_scale_psd[kScaleBins] = {};
    float chroma_scale_psd[kScaleBins] = {};

    // Normalized 4x4 Walsh-Hadamard spatial spectrum (white noise == 1.0 in every bin, sum == 16).
    float walsh16_luma_norm[kWalsh16Bins] = {};
    float walsh16_chroma_norm[kWalsh16Bins] = {};

    // Calibrated inverse-NLT derivative multiplier indexed by normalized linear intensity I in [0, 1].
    float nlt_slope_lut[kNltSlopeLutSize] = {};

    // Codec-intrinsic d(ADU)/du indexed by normalized raw ADU in [0, 1] = raw / nlt_raw_white_adu.
    float nlt_dadu_du_raw[kNltSlopeLutSize] = {};
};

struct DecodeOptions {
    int num_threads = 0;          // 0 = use session default
    bool apply_active_crop = false;
    bool has_roi = false;
    uint32_t roi[4] = {0, 0, 0, 0}; // [x, y, width, height] in coded pixels
    bool want_quant_profile = false;
};

struct DecodedFrameInfo {
    CodecFamily codec = CodecFamily::Unknown;
    uint32_t coded_width = 0;
    uint32_t coded_height = 0;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    uint32_t offset_x = 0;
    uint32_t offset_y = 0;
    uint16_t bit_depth = 14;
    uint8_t cfa_pattern[4] = {0, 1, 1, 2};
    bool has_meta = false;
    NevFrameMeta meta;
    bool has_timing = false;
    VideoFrameTiming timing;
    WaveletQuantProfile quant_profile;
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
    float focal_length_mm = 0.0f;
    float focus_distance_m = 0.0f;
    bool focus_is_infinity = false;
    double as_shot_neutral[3] = {1.0, 1.0, 1.0};
    double color_matrix1[9] = {
         1.1423, -0.4564, -0.1123,
        -0.4816,  1.2895,  0.2119,
        -0.0210,  0.1061,  0.7282
    };
};

// Container parsing (64-bit file I/O)
NIJINWVDC_API bool parse_nev_clip(std::string_view path, NevClipInfo& clip, std::string& err);
NIJINWVDC_API bool read_nev_frame_meta_and_codestream(
    const NevClipInfo& clip,
    uint32_t frame_index,
    NevFrameMeta& meta,
    std::vector<uint8_t>* codestream_out,
    std::string& err);
NIJINWVDC_API bool extract_nev_blob(
    std::string_view path,
    uint64_t offset,
    uint32_t size,
    std::vector<uint8_t>& out,
    std::string& err);

// TicoRAW / JPEG XS R0 decoding
NIJINWVDC_API bool probe_ticoraw_header(
    const uint8_t* data,
    size_t len,
    TicoRawInfo& info,
    std::string& err);

inline bool probe_ticoraw_header(
    std::span<const uint8_t> data,
    TicoRawInfo& info,
    std::string& err) {
    return probe_ticoraw_header(data.data(), data.size(), info, err);
}

NIJINWVDC_API bool decode_ticoraw_bayer(
    const uint8_t* data,
    size_t len,
    int num_threads,
    std::vector<uint16_t>& bayer_out,
    TicoRawInfo& info_out,
    std::string& err);

inline bool decode_ticoraw_bayer(
    std::span<const uint8_t> data,
    int num_threads,
    std::vector<uint16_t>& bayer_out,
    TicoRawInfo& info_out,
    std::string& err) {
    return decode_ticoraw_bayer(data.data(), data.size(), num_threads, bayer_out, info_out, err);
}

// Sony ARW6 (Compression=32766, SonyRawFileType=6) multi-tile & "A000" wavelet decoding
NIJINWVDC_API bool probe_arw6_codestream(
    const uint8_t* data,
    size_t len,
    Arw6Info& info,
    std::string& err);

inline bool probe_arw6_codestream(
    std::span<const uint8_t> data,
    Arw6Info& info,
    std::string& err) {
    return probe_arw6_codestream(data.data(), data.size(), info, err);
}

NIJINWVDC_API bool probe_arw6_strip(
    const uint8_t* strip_data,
    size_t strip_len,
    Arw6Info& info,
    std::string& err);

inline bool probe_arw6_strip(
    std::span<const uint8_t> strip_data,
    Arw6Info& info,
    std::string& err) {
    return probe_arw6_strip(strip_data.data(), strip_data.size(), info, err);
}

NIJINWVDC_API bool decode_arw6_tile(
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

inline bool decode_arw6_tile(
    std::span<const uint8_t> cs_data,
    int num_threads,
    uint32_t tile_x,
    uint32_t tile_y,
    uint32_t full_w,
    uint32_t full_h,
    std::vector<uint16_t>& bayer_out,
    Arw6Info& tile_info_out,
    std::string& err) {
    return decode_arw6_tile(
        cs_data.data(), cs_data.size(), num_threads,
        tile_x, tile_y, full_w, full_h, bayer_out, tile_info_out, err);
}

NIJINWVDC_API bool decode_arw6_bayer(
    const uint8_t* strip_data,
    size_t strip_len,
    int num_threads,
    std::vector<uint16_t>& bayer_out,
    Arw6Info& info_out,
    std::string& err);

inline bool decode_arw6_bayer(
    std::span<const uint8_t> strip_data,
    int num_threads,
    std::vector<uint16_t>& bayer_out,
    Arw6Info& info_out,
    std::string& err) {
    return decode_arw6_bayer(strip_data.data(), strip_data.size(), num_threads, bayer_out, info_out, err);
}

// DNG 1.4 writer & camera matrix lookup
NIJINWVDC_API bool lookup_nikon_d65_colormatrix(std::string_view model, double out_cm[9]);
NIJINWVDC_API bool lookup_camera_d65_colormatrix(std::string_view make, std::string_view model, double out_cm[9]);
NIJINWVDC_API void build_dng_options_from_clip_and_frame(
    const NevClipInfo& clip,
    const NevFrameMeta& meta,
    const TicoRawInfo& jxs,
    bool apply_active_crop,
    uint16_t override_orientation,
    DngWriteOptions& opts);
NIJINWVDC_API void build_dng_options_from_clip_and_frame(
    const NevClipInfo& clip,
    const NevFrameMeta& meta,
    const Arw6Info& arw6,
    bool apply_active_crop,
    uint16_t override_orientation,
    DngWriteOptions& opts);
NIJINWVDC_API bool apply_bayer_crop(
    const uint16_t* src,
    uint32_t src_w,
    uint32_t src_h,
    const uint16_t crop_xywh[4],
    const uint8_t src_cfa[4],
    std::vector<uint16_t>& dst,
    uint8_t dst_cfa[4]);

inline bool apply_bayer_crop(
    std::span<const uint16_t> src,
    uint32_t src_w,
    uint32_t src_h,
    const uint16_t crop_xywh[4],
    const uint8_t src_cfa[4],
    std::vector<uint16_t>& dst,
    uint8_t dst_cfa[4]) {
    if (src_w == 0 || src_h == 0 ||
        src.size() < static_cast<size_t>(src_w) * static_cast<size_t>(src_h)) {
        return false;
    }
    return apply_bayer_crop(src.data(), src_w, src_h, crop_xywh, src_cfa, dst, dst_cfa);
}

NIJINWVDC_API bool write_cfa_dng(
    std::string_view path,
    const uint16_t* pixels,
    const DngWriteOptions& opts,
    std::string& err);

inline bool write_cfa_dng(
    std::string_view path,
    std::span<const uint16_t> pixels,
    const DngWriteOptions& opts,
    std::string& err) {
    if (opts.width == 0 || opts.height == 0 ||
        pixels.size() < static_cast<size_t>(opts.width) * static_cast<size_t>(opts.height)) {
        err = "Pixel span smaller than DNG dimensions";
        return false;
    }
    return write_cfa_dng(path, pixels.data(), opts, err);
}

// High-throughput strided post-processing & in-memory DNG helpers
NIJINWVDC_API bool apply_bayer_crop_strided(
    const uint16_t* src,
    size_t src_row_stride_bytes,
    uint32_t src_w,
    uint32_t src_h,
    const uint16_t crop_xywh[4],
    const uint8_t src_cfa[4],
    uint16_t* dst,
    size_t dst_row_stride_bytes,
    uint8_t dst_cfa[4]);

NIJINWVDC_API bool split_bayer_cfa_planes(
    const uint16_t* src,
    size_t src_row_stride_bytes,
    uint32_t width,
    uint32_t height,
    uint16_t* c00,
    uint16_t* c01,
    uint16_t* c10,
    uint16_t* c11,
    size_t plane_row_stride_bytes);

inline bool split_bayer_cfa_planes(
    std::span<const uint16_t> src,
    uint32_t width,
    uint32_t height,
    std::span<uint16_t> c00,
    std::span<uint16_t> c01,
    std::span<uint16_t> c10,
    std::span<uint16_t> c11) {
    if (width == 0 || height == 0 || (width & 1u) != 0 || (height & 1u) != 0) {
        return false;
    }
    const size_t full_px = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t q_px = static_cast<size_t>(width / 2u) * static_cast<size_t>(height / 2u);
    if (src.size() < full_px || c00.size() < q_px || c01.size() < q_px ||
        c10.size() < q_px || c11.size() < q_px) {
        return false;
    }
    return split_bayer_cfa_planes(
        src.data(), 0, width, height,
        c00.data(), c01.data(), c10.data(), c11.data(), 0);
}

NIJINWVDC_API bool normalize_bayer_to_float(
    const uint16_t* src,
    size_t src_row_stride_bytes,
    uint32_t width,
    uint32_t height,
    const float black[4],
    float white_level,
    bool clamp_01,
    float* dst,
    size_t dst_row_stride_bytes);

inline bool normalize_bayer_to_float(
    std::span<const uint16_t> src,
    uint32_t width,
    uint32_t height,
    const float black[4],
    float white_level,
    bool clamp_01,
    std::span<float> dst) {
    const size_t px = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (width == 0 || height == 0 || src.size() < px || dst.size() < px) {
        return false;
    }
    return normalize_bayer_to_float(src.data(), 0, width, height, black, white_level, clamp_01, dst.data(), 0);
}

NIJINWVDC_API bool build_cfa_dng_header(
    const DngWriteOptions& opts,
    std::vector<uint8_t>& header_out,
    std::string& err);

NIJINWVDC_API bool encode_cfa_dng_memory(
    const uint16_t* pixels,
    const DngWriteOptions& opts,
    std::vector<uint8_t>& dng_out,
    std::string& err);

inline bool encode_cfa_dng_memory(
    std::span<const uint16_t> pixels,
    const DngWriteOptions& opts,
    std::vector<uint8_t>& dng_out,
    std::string& err) {
    if (opts.width == 0 || opts.height == 0 ||
        pixels.size() < static_cast<size_t>(opts.width) * static_cast<size_t>(opts.height)) {
        err = "Pixel span smaller than DNG dimensions";
        return false;
    }
    return encode_cfa_dng_memory(pixels.data(), opts, dng_out, err);
}

// Stateful, thread-safe container & media reader (file / memory / callbacks)
class MediaReader {
public:
    NIJINWVDC_API MediaReader();
    NIJINWVDC_API ~MediaReader();
    NIJINWVDC_API MediaReader(MediaReader&& other) noexcept;
    NIJINWVDC_API MediaReader& operator=(MediaReader&& other) noexcept;
    MediaReader(const MediaReader&) = delete;
    MediaReader& operator=(const MediaReader&) = delete;

    NIJINWVDC_API bool open_file(std::string_view path, std::string& err);
    NIJINWVDC_API bool open_memory(const uint8_t* data, size_t size, std::string& err);
    inline bool open_memory(std::span<const uint8_t> data, std::string& err) {
        return open_memory(data.data(), data.size(), err);
    }
    NIJINWVDC_API bool open_callbacks(const IoCallbacks& callbacks, std::string& err);
    NIJINWVDC_API void close();

    NIJINWVDC_API bool is_open() const;
    NIJINWVDC_API ContainerFormat container_format() const;
    NIJINWVDC_API CodecFamily codec_family() const;
    NIJINWVDC_API const NevClipInfo& clip_info() const;
    NIJINWVDC_API uint32_t frame_count() const;

    NIJINWVDC_API bool read_frame_packet(
        uint32_t frame_index,
        NevFrameMeta& meta,
        std::vector<uint8_t>* codestream_out,
        std::string& err) const;

    NIJINWVDC_API bool get_frame_timing(
        uint32_t frame_index,
        VideoFrameTiming& timing_out,
        std::string& err) const;

    NIJINWVDC_API AudioFormatInfo audio_info() const;
    NIJINWVDC_API bool read_audio_pcm(
        uint64_t start_frame,
        uint64_t frame_count,
        std::vector<uint8_t>& pcm_out,
        std::string& err) const;
    NIJINWVDC_API bool read_audio_pcm_into(
        uint64_t start_frame,
        uint64_t frame_count,
        void* dst,
        size_t dst_bytes,
        uint64_t* frames_read_out,
        std::string& err) const;
    inline bool read_audio_pcm_into(
        uint64_t start_frame,
        uint64_t frame_count,
        std::span<uint8_t> dst,
        uint64_t* frames_read_out,
        std::string& err) const {
        return read_audio_pcm_into(start_frame, frame_count, dst.data(), dst.size(), frames_read_out, err);
    }
    NIJINWVDC_API bool export_audio_wav(std::string_view wav_path, std::string& err) const;
    NIJINWVDC_API bool extract_preview_jpeg(std::vector<uint8_t>& jpeg_out, std::string& err) const;

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

// Reusable stateful decoder session owning persistent worker pool and arenas
class DecoderSession {
public:
    NIJINWVDC_API explicit DecoderSession(int num_threads = 0);
    NIJINWVDC_API ~DecoderSession();
    NIJINWVDC_API DecoderSession(DecoderSession&& other) noexcept;
    NIJINWVDC_API DecoderSession& operator=(DecoderSession&& other) noexcept;
    DecoderSession(const DecoderSession&) = delete;
    DecoderSession& operator=(const DecoderSession&) = delete;

    NIJINWVDC_API void set_num_threads(int num_threads);
    NIJINWVDC_API int num_threads() const;

    NIJINWVDC_API bool decode_codestream(
        const uint8_t* data,
        size_t len,
        const DecodeOptions& opts,
        std::vector<uint16_t>& bayer_out,
        DecodedFrameInfo& info_out,
        std::string& err);

    inline bool decode_codestream(
        std::span<const uint8_t> data,
        const DecodeOptions& opts,
        std::vector<uint16_t>& bayer_out,
        DecodedFrameInfo& info_out,
        std::string& err) {
        return decode_codestream(data.data(), data.size(), opts, bayer_out, info_out, err);
    }

    NIJINWVDC_API bool decode_codestream_into(
        const uint8_t* data,
        size_t len,
        const DecodeOptions& opts,
        uint16_t* dst,
        size_t dst_row_stride_bytes,
        size_t dst_height_capacity,
        DecodedFrameInfo& info_out,
        std::string& err);

    inline bool decode_codestream_into(
        std::span<const uint8_t> data,
        const DecodeOptions& opts,
        std::span<uint16_t> dst,
        size_t dst_row_stride_bytes,
        DecodedFrameInfo& info_out,
        std::string& err) {
        return decode_codestream_into_checked(
            data.data(), data.size(), opts, dst.data(), dst.size(), dst_row_stride_bytes, info_out, err);
    }

    NIJINWVDC_API bool decode_frame(
        const MediaReader& reader,
        uint32_t frame_index,
        const DecodeOptions& opts,
        std::vector<uint16_t>& bayer_out,
        DecodedFrameInfo& info_out,
        std::string& err);

    NIJINWVDC_API bool decode_frame_into(
        const MediaReader& reader,
        uint32_t frame_index,
        const DecodeOptions& opts,
        uint16_t* dst,
        size_t dst_row_stride_bytes,
        size_t dst_height_capacity,
        DecodedFrameInfo& info_out,
        std::string& err);

    inline bool decode_frame_into(
        const MediaReader& reader,
        uint32_t frame_index,
        const DecodeOptions& opts,
        std::span<uint16_t> dst,
        size_t dst_row_stride_bytes,
        DecodedFrameInfo& info_out,
        std::string& err) {
        return decode_frame_into_checked(
            reader, frame_index, opts, dst.data(), dst.size(), dst_row_stride_bytes, info_out, err);
    }

    struct Impl;
private:
    NIJINWVDC_API bool decode_codestream_into_checked(
        const uint8_t* data,
        size_t len,
        const DecodeOptions& opts,
        uint16_t* dst,
        size_t dst_elems,
        size_t dst_row_stride_bytes,
        DecodedFrameInfo& info_out,
        std::string& err);

    NIJINWVDC_API bool decode_frame_into_checked(
        const MediaReader& reader,
        uint32_t frame_index,
        const DecodeOptions& opts,
        uint16_t* dst,
        size_t dst_elems,
        size_t dst_row_stride_bytes,
        DecodedFrameInfo& info_out,
        std::string& err);

    std::unique_ptr<Impl> impl_;
};

// Recalibrate nlt_slope_lut inside an extracted WaveletQuantProfile to caller linear [black, white].
NIJINWVDC_API void recalibrate_quant_profile_range(
    WaveletQuantProfile& profile,
    float black_level,
    float white_level);

// Vertical crop/slice of per-row quantization arrays after active-crop or ROI extraction.
NIJINWVDC_API bool crop_quant_profile_vertical(
    WaveletQuantProfile& profile,
    uint32_t offset_y,
    uint32_t output_h);

} // namespace nijinwvdc

#endif // NIJINWVDC_H
