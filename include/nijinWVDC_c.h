/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NIJINWVDC_C_H
#define NIJINWVDC_C_H

#include "nijinWVDC_export.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum nijin_status_t {
    NIJIN_OK = 0,
    NIJIN_ERR_INVALID_ARG = 1,
    NIJIN_ERR_IO = 2,
    NIJIN_ERR_FORMAT = 3,
    NIJIN_ERR_DECODE = 4,
    NIJIN_ERR_BUFFER_TOO_SMALL = 5,
    NIJIN_ERR_UNSUPPORTED = 6,
    NIJIN_ERR_INTERNAL = 7
} nijin_status_t;

typedef enum nijin_container_format_t {
    NIJIN_CONTAINER_UNKNOWN = 0,
    NIJIN_CONTAINER_MP4_NEV = 1,
    NIJIN_CONTAINER_TIFF_NEF = 2,
    NIJIN_CONTAINER_TIFF_ARW = 3,
    NIJIN_CONTAINER_RAW_CODESTREAM = 4
} nijin_container_format_t;

typedef enum nijin_codec_family_t {
    NIJIN_CODEC_UNKNOWN = 0,
    NIJIN_CODEC_TICORAW = 1,
    NIJIN_CODEC_ARW6 = 2
} nijin_codec_family_t;

typedef struct nijin_io_callbacks_t {
    void* user_data;
    uint64_t file_size;
    /* Return 0 on success and write exact byte count to *bytes_read_out. */
    int (*read_at)(void* user_data, uint64_t offset, void* dst, size_t size, size_t* bytes_read_out);
} nijin_io_callbacks_t;

typedef struct nijin_clip_info_t {
    uint32_t container_format;
    uint32_t codec_family;
    uint64_t file_size;
    uint32_t frame_count;
    uint32_t movie_timescale;
    uint64_t movie_duration;
    double fps;
    uint32_t width;
    uint32_t height;
    uint16_t active_w;
    uint16_t active_h;
    uint8_t has_audio;
    uint8_t has_timecode;
    uint8_t reserved[2];
    uint32_t audio_channels;
    uint32_t audio_bits_per_channel;
    double audio_sample_rate;
    uint64_t audio_total_frames;
    char make[64];
    char model[64];
    char major_brand[16];
} nijin_clip_info_t;

typedef struct nijin_frame_meta_t {
    uint32_t frame_index;
    uint32_t width;
    uint32_t height;
    uint16_t bit_depth;
    uint16_t orientation;
    uint8_t cfa_pattern[4];
    uint8_t has_crop;
    uint8_t has_black;
    uint8_t has_wb;
    uint8_t has_timecode;
    uint16_t crop[4];
    uint16_t black[4];
    uint16_t white_level;
    uint8_t drop_frame;
    uint8_t reserved;
    double wb_rggb[4];
    float exposure_time_s;
    float fnumber;
    uint32_t iso;
    float focal_length_mm;
    uint64_t sample_offset;
    uint32_t sample_size;
    uint64_t codestream_offset;
    uint32_t codestream_size;
    uint32_t timescale;
    uint64_t pts_ticks;
    uint32_t duration_ticks;
    double pts_seconds;
    double duration_seconds;
    char timecode_str[16];
} nijin_frame_meta_t;

typedef struct nijin_decode_options_t {
    uint32_t struct_size;
    int32_t num_threads;
    uint8_t apply_active_crop;
    uint8_t has_roi;
    uint8_t reserved[2];
    uint32_t roi[4];
} nijin_decode_options_t;

typedef struct nijin_decoded_info_t {
    uint32_t codec;
    uint32_t coded_width;
    uint32_t coded_height;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t offset_x;
    uint32_t offset_y;
    uint16_t bit_depth;
    uint8_t cfa_pattern[4];
    uint8_t has_meta;
    uint8_t reserved;
    nijin_frame_meta_t meta;
} nijin_decoded_info_t;

typedef struct nijin_audio_info_t {
    uint8_t available;
    uint8_t is_signed;
    uint8_t is_float;
    uint8_t is_big_endian;
    char format[16];
    uint32_t timescale;
    double sample_rate;
    uint32_t channels;
    uint32_t bits_per_channel;
    uint32_t bytes_per_packet;
    uint32_t lpcm_flags;
    uint64_t total_frames;
} nijin_audio_info_t;

typedef struct nijin_dng_options_t {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint16_t bit_depth;
    uint16_t black[4];
    uint16_t white;
    uint16_t orientation;
    uint8_t cfa_pattern[4];
    uint8_t has_crop;
    uint8_t reserved;
    uint16_t crop_origin[2];
    uint16_t crop_size[2];
    float exposure_time_s;
    float fnumber;
    double as_shot_neutral[3];
    double color_matrix1[9];
    char make[64];
    char model[64];
    char unique_model[64];
    char software[64];
    char datetime[32];
} nijin_dng_options_t;

typedef struct nijin_reader_s nijin_reader_t;
typedef struct nijin_decoder_s nijin_decoder_t;

NIJINWVDC_API const char* nijin_version(void);
NIJINWVDC_API uint32_t nijin_simd_capabilities(void);
NIJINWVDC_API const char* nijin_status_string(nijin_status_t status);

NIJINWVDC_API nijin_reader_t* nijin_reader_create(void);
NIJINWVDC_API void nijin_reader_destroy(nijin_reader_t* reader);
NIJINWVDC_API const char* nijin_reader_last_error(const nijin_reader_t* reader);

NIJINWVDC_API nijin_status_t nijin_reader_open_file(nijin_reader_t* reader, const char* utf8_path);
NIJINWVDC_API nijin_status_t nijin_reader_open_memory(nijin_reader_t* reader, const uint8_t* data, size_t size);
NIJINWVDC_API nijin_status_t nijin_reader_open_callbacks(nijin_reader_t* reader, const nijin_io_callbacks_t* callbacks);
NIJINWVDC_API void nijin_reader_close(nijin_reader_t* reader);
NIJINWVDC_API int nijin_reader_is_open(const nijin_reader_t* reader);

NIJINWVDC_API nijin_status_t nijin_reader_get_clip_info(const nijin_reader_t* reader, nijin_clip_info_t* out_info);
NIJINWVDC_API uint32_t nijin_reader_frame_count(const nijin_reader_t* reader);
NIJINWVDC_API nijin_status_t nijin_reader_get_frame_meta(const nijin_reader_t* reader, uint32_t frame_index, nijin_frame_meta_t* out_meta);
NIJINWVDC_API nijin_status_t nijin_reader_read_frame_packet(
    const nijin_reader_t* reader,
    uint32_t frame_index,
    uint8_t* out_buf,
    size_t capacity,
    size_t* out_size,
    nijin_frame_meta_t* out_meta);

NIJINWVDC_API nijin_status_t nijin_reader_get_audio_info(const nijin_reader_t* reader, nijin_audio_info_t* out_info);
NIJINWVDC_API nijin_status_t nijin_reader_read_audio_pcm(
    const nijin_reader_t* reader,
    uint64_t start_frame,
    uint64_t frame_count,
    void* dst,
    size_t dst_bytes,
    uint64_t* out_frames_read,
    size_t* out_bytes_written);
NIJINWVDC_API nijin_status_t nijin_reader_export_audio_wav(const nijin_reader_t* reader, const char* utf8_wav_path);
NIJINWVDC_API nijin_status_t nijin_reader_extract_preview_jpeg(
    const nijin_reader_t* reader,
    uint8_t* out_buf,
    size_t capacity,
    size_t* out_size);

NIJINWVDC_API nijin_decoder_t* nijin_decoder_create(int32_t num_threads);
NIJINWVDC_API void nijin_decoder_destroy(nijin_decoder_t* decoder);
NIJINWVDC_API const char* nijin_decoder_last_error(const nijin_decoder_t* decoder);
NIJINWVDC_API void nijin_decoder_set_num_threads(nijin_decoder_t* decoder, int32_t num_threads);
NIJINWVDC_API int32_t nijin_decoder_num_threads(const nijin_decoder_t* decoder);

NIJINWVDC_API void nijin_decode_options_init(nijin_decode_options_t* opts);
NIJINWVDC_API nijin_status_t nijin_decoder_decode_codestream(
    nijin_decoder_t* decoder,
    const uint8_t* data,
    size_t len,
    const nijin_decode_options_t* opts,
    uint16_t* dst,
    size_t dst_row_stride_bytes,
    size_t dst_height_capacity,
    nijin_decoded_info_t* out_info);

NIJINWVDC_API nijin_status_t nijin_decoder_decode_frame(
    nijin_decoder_t* decoder,
    const nijin_reader_t* reader,
    uint32_t frame_index,
    const nijin_decode_options_t* opts,
    uint16_t* dst,
    size_t dst_row_stride_bytes,
    size_t dst_height_capacity,
    nijin_decoded_info_t* out_info);

NIJINWVDC_API nijin_status_t nijin_crop_bayer_strided(
    const uint16_t* src,
    size_t src_row_stride_bytes,
    uint32_t src_w,
    uint32_t src_h,
    const uint16_t crop_xywh[4],
    const uint8_t src_cfa[4],
    uint16_t* dst,
    size_t dst_row_stride_bytes,
    uint8_t dst_cfa[4]);

NIJINWVDC_API nijin_status_t nijin_split_bayer_planes(
    const uint16_t* src,
    size_t src_row_stride_bytes,
    uint32_t width,
    uint32_t height,
    uint16_t* c00,
    uint16_t* c01,
    uint16_t* c10,
    uint16_t* c11,
    size_t plane_row_stride_bytes);

NIJINWVDC_API nijin_status_t nijin_normalize_bayer_float(
    const uint16_t* src,
    size_t src_row_stride_bytes,
    uint32_t width,
    uint32_t height,
    const float black[4],
    float white_level,
    int clamp_01,
    float* dst,
    size_t dst_row_stride_bytes);

NIJINWVDC_API void nijin_dng_options_init(nijin_dng_options_t* opts);
NIJINWVDC_API nijin_status_t nijin_dng_options_from_clip(
    const nijin_reader_t* reader,
    uint32_t frame_index,
    const nijin_decoded_info_t* decoded,
    int apply_active_crop,
    uint16_t override_orientation,
    nijin_dng_options_t* out_opts);

NIJINWVDC_API nijin_status_t nijin_build_dng_header(
    const nijin_dng_options_t* opts,
    uint8_t* out_buf,
    size_t capacity,
    size_t* out_size);

NIJINWVDC_API nijin_status_t nijin_write_dng_file(
    const char* utf8_path,
    const uint16_t* pixels,
    size_t src_row_stride_bytes,
    const nijin_dng_options_t* opts,
    char* err_buf,
    size_t err_buf_cap);

#ifdef __cplusplus
}
#endif

#endif /* NIJINWVDC_C_H */
