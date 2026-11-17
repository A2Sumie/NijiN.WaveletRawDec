// SPDX-License-Identifier: Apache-2.0
#include "../include/nijinWVDC_c.h"
#include "../include/nijinWVDC.h"
#include "tiff_internal.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <new>
#include <string>
#include <vector>

struct nijin_reader_s {
    nijinwvdc::MediaReader cpp;
    nijin_io_callbacks_t c_callbacks{};
    std::string last_err;
};

struct nijin_decoder_s {
    nijinwvdc::DecoderSession cpp;
    std::string last_err;

    explicit nijin_decoder_s(int32_t threads) : cpp(threads) {}
};

namespace {

void copy_str_trunc(char* dst, size_t cap, const std::string& s) {
    if (!dst || cap == 0) {
        return;
    }
    const size_t n = (s.size() < cap - 1u) ? s.size() : (cap - 1u);
    if (n > 0) {
        std::memcpy(dst, s.data(), n);
    }
    dst[n] = '\0';
}

void write_err_buf(char* buf, size_t cap, const std::string& msg) {
    if (buf && cap > 0) {
        copy_str_trunc(buf, cap, msg);
    }
}

bool c_io_read_trampoline(void* user_data, uint64_t offset, void* dst, size_t size, size_t* bytes_read_out) {
    const nijin_io_callbacks_t* cb = static_cast<const nijin_io_callbacks_t*>(user_data);
    if (!cb || !cb->read_at) {
        return false;
    }
    size_t got = 0;
    const int rc = cb->read_at(cb->user_data, offset, dst, size, &got);
    if (bytes_read_out) {
        *bytes_read_out = got;
    }
    return (rc == 0) && (got == size);
}

void fill_c_frame_meta(
    const nijinwvdc::NevFrameMeta& m,
    const nijinwvdc::VideoFrameTiming* timing,
    nijin_frame_meta_t* out) {
    std::memset(out, 0, sizeof(*out));
    out->frame_index = m.frame_index;
    out->width = m.width;
    out->height = m.height;
    out->bit_depth = m.bit_depth;
    out->orientation = m.orientation;
    std::memcpy(out->cfa_pattern, m.cfa_pattern, 4);
    out->has_crop = m.has_crop ? 1u : 0u;
    out->has_black = m.has_black ? 1u : 0u;
    out->has_wb = m.has_wb ? 1u : 0u;
    std::memcpy(out->crop, m.crop, sizeof(out->crop));
    std::memcpy(out->black, m.black, sizeof(out->black));
    out->white_level = m.white_level;
    std::memcpy(out->wb_rggb, m.wb_rggb, sizeof(out->wb_rggb));
    out->exposure_time_s = m.exposure_time_s;
    out->fnumber = m.fnumber;
    out->iso = m.iso;
    out->focal_length_mm = m.focal_length_mm;
    out->sample_offset = m.sample_offset;
    out->sample_size = m.sample_size;
    out->codestream_offset = m.codestream_offset;
    out->codestream_size = m.codestream_size;
    if (timing) {
        out->timescale = timing->timescale;
        out->pts_ticks = timing->pts_ticks;
        out->duration_ticks = timing->duration_ticks;
        out->pts_seconds = timing->pts_seconds;
        out->duration_seconds = timing->duration_seconds;
        out->has_timecode = timing->has_timecode ? 1u : 0u;
        out->drop_frame = timing->drop_frame ? 1u : 0u;
        std::memcpy(out->timecode_str, timing->timecode_str, sizeof(out->timecode_str));
    }
}

void fill_c_decoded_info(const nijinwvdc::DecodedFrameInfo& src, nijin_decoded_info_t* dst) {
    std::memset(dst, 0, sizeof(*dst));
    dst->codec = static_cast<uint32_t>(src.codec);
    dst->coded_width = src.coded_width;
    dst->coded_height = src.coded_height;
    dst->output_width = src.output_width;
    dst->output_height = src.output_height;
    dst->offset_x = src.offset_x;
    dst->offset_y = src.offset_y;
    dst->bit_depth = src.bit_depth;
    std::memcpy(dst->cfa_pattern, src.cfa_pattern, 4);
    dst->has_meta = src.has_meta ? 1u : 0u;
    if (src.has_meta) {
        fill_c_frame_meta(src.meta, src.has_timing ? &src.timing : nullptr, &dst->meta);
    } else if (src.has_timing) {
        dst->meta.timescale = src.timing.timescale;
        dst->meta.pts_ticks = src.timing.pts_ticks;
        dst->meta.duration_ticks = src.timing.duration_ticks;
        dst->meta.pts_seconds = src.timing.pts_seconds;
        dst->meta.duration_seconds = src.timing.duration_seconds;
        dst->meta.has_timecode = src.timing.has_timecode ? 1u : 0u;
        dst->meta.drop_frame = src.timing.drop_frame ? 1u : 0u;
        std::memcpy(dst->meta.timecode_str, src.timing.timecode_str, sizeof(dst->meta.timecode_str));
    }
}

nijinwvdc::DecodeOptions to_cpp_decode_options(const nijin_decode_options_t* opts) {
    nijinwvdc::DecodeOptions out;
    if (opts && (opts->struct_size == 0 || opts->struct_size >= sizeof(nijin_decode_options_t))) {
        out.num_threads = opts->num_threads;
        out.apply_active_crop = (opts->apply_active_crop != 0);
        out.has_roi = (opts->has_roi != 0);
        std::memcpy(out.roi, opts->roi, sizeof(out.roi));
    }
    return out;
}

nijin_status_t to_cpp_dng_options(const nijin_dng_options_t* opts, nijinwvdc::DngWriteOptions& out) {
    if (!opts) {
        return NIJIN_ERR_INVALID_ARG;
    }
    if (opts->struct_size != 0 && opts->struct_size < sizeof(nijin_dng_options_t)) {
        return NIJIN_ERR_INVALID_ARG;
    }
    out = nijinwvdc::DngWriteOptions();
    out.width = opts->width;
    out.height = opts->height;
    out.bit_depth = opts->bit_depth;
    std::memcpy(out.black, opts->black, sizeof(out.black));
    out.white = opts->white;
    out.orientation = opts->orientation;
    std::memcpy(out.cfa_pattern, opts->cfa_pattern, 4);
    out.has_crop = (opts->has_crop != 0);
    std::memcpy(out.crop_origin, opts->crop_origin, sizeof(out.crop_origin));
    std::memcpy(out.crop_size, opts->crop_size, sizeof(out.crop_size));
    out.exposure_time_s = opts->exposure_time_s;
    out.fnumber = opts->fnumber;
    std::memcpy(out.as_shot_neutral, opts->as_shot_neutral, sizeof(out.as_shot_neutral));
    std::memcpy(out.color_matrix1, opts->color_matrix1, sizeof(out.color_matrix1));
    out.make = opts->make;
    out.model = opts->model;
    out.unique_model = opts->unique_model;
    out.software = opts->software;
    out.datetime = opts->datetime;
    return NIJIN_OK;
}

void to_c_dng_options(const nijinwvdc::DngWriteOptions& src, nijin_dng_options_t* dst) {
    std::memset(dst, 0, sizeof(*dst));
    dst->struct_size = static_cast<uint32_t>(sizeof(*dst));
    dst->width = src.width;
    dst->height = src.height;
    dst->bit_depth = src.bit_depth;
    std::memcpy(dst->black, src.black, sizeof(dst->black));
    dst->white = src.white;
    dst->orientation = src.orientation;
    std::memcpy(dst->cfa_pattern, src.cfa_pattern, 4);
    dst->has_crop = src.has_crop ? 1u : 0u;
    std::memcpy(dst->crop_origin, src.crop_origin, sizeof(dst->crop_origin));
    std::memcpy(dst->crop_size, src.crop_size, sizeof(dst->crop_size));
    dst->exposure_time_s = src.exposure_time_s;
    dst->fnumber = src.fnumber;
    std::memcpy(dst->as_shot_neutral, src.as_shot_neutral, sizeof(dst->as_shot_neutral));
    std::memcpy(dst->color_matrix1, src.color_matrix1, sizeof(dst->color_matrix1));
    copy_str_trunc(dst->make, sizeof(dst->make), src.make);
    copy_str_trunc(dst->model, sizeof(dst->model), src.model);
    copy_str_trunc(dst->unique_model, sizeof(dst->unique_model), src.unique_model);
    copy_str_trunc(dst->software, sizeof(dst->software), src.software);
    copy_str_trunc(dst->datetime, sizeof(dst->datetime), src.datetime);
}

nijin_status_t copy_blob_to_c_buf(
    const std::vector<uint8_t>& blob,
    uint8_t* out_buf,
    size_t capacity,
    size_t* out_size) {
    if (!out_size) {
        return NIJIN_ERR_INVALID_ARG;
    }
    *out_size = blob.size();
    if (!out_buf) {
        return NIJIN_OK;
    }
    if (capacity < blob.size()) {
        return NIJIN_ERR_BUFFER_TOO_SMALL;
    }
    if (!blob.empty()) {
        std::memcpy(out_buf, blob.data(), blob.size());
    }
    return NIJIN_OK;
}

} // namespace

extern "C" {

const char* nijin_version(void) {
    return nijinwvdc::version_string();
}

uint32_t nijin_simd_capabilities(void) {
    return nijinwvdc::simd_capabilities();
}

const char* nijin_status_string(nijin_status_t status) {
    switch (status) {
        case NIJIN_OK: return "ok";
        case NIJIN_ERR_INVALID_ARG: return "invalid argument";
        case NIJIN_ERR_IO: return "I/O error";
        case NIJIN_ERR_FORMAT: return "unsupported or invalid format";
        case NIJIN_ERR_DECODE: return "decode failed";
        case NIJIN_ERR_BUFFER_TOO_SMALL: return "buffer too small";
        case NIJIN_ERR_UNSUPPORTED: return "unsupported operation";
        case NIJIN_ERR_INTERNAL: return "internal error";
        default: return "unknown error";
    }
}

nijin_reader_t* nijin_reader_create(void) {
    try {
        return new nijin_reader_s();
    } catch (...) {
        return nullptr;
    }
}

void nijin_reader_destroy(nijin_reader_t* reader) {
    delete reader;
}

const char* nijin_reader_last_error(const nijin_reader_t* reader) {
    return reader ? reader->last_err.c_str() : "";
}

nijin_status_t nijin_reader_open_file(nijin_reader_t* reader, const char* utf8_path) {
    if (!reader || !utf8_path || !*utf8_path) {
        return NIJIN_ERR_INVALID_ARG;
    }
    try {
        reader->last_err.clear();
        if (!reader->cpp.open_file(utf8_path, reader->last_err)) {
            return (reader->last_err.find("open") != std::string::npos ||
                    reader->last_err.find("stat") != std::string::npos ||
                    reader->last_err.find("read") != std::string::npos)
                       ? NIJIN_ERR_IO
                       : NIJIN_ERR_FORMAT;
        }
        return NIJIN_OK;
    } catch (const std::exception& e) {
        reader->last_err = e.what();
        return NIJIN_ERR_INTERNAL;
    } catch (...) {
        reader->last_err = "internal exception";
        return NIJIN_ERR_INTERNAL;
    }
}

nijin_status_t nijin_reader_open_memory(nijin_reader_t* reader, const uint8_t* data, size_t size) {
    if (!reader || !data || size == 0) {
        return NIJIN_ERR_INVALID_ARG;
    }
    try {
        reader->last_err.clear();
        if (!reader->cpp.open_memory(data, size, reader->last_err)) {
            return NIJIN_ERR_FORMAT;
        }
        return NIJIN_OK;
    } catch (const std::exception& e) {
        reader->last_err = e.what();
        return NIJIN_ERR_INTERNAL;
    } catch (...) {
        reader->last_err = "internal exception";
        return NIJIN_ERR_INTERNAL;
    }
}

nijin_status_t nijin_reader_open_callbacks(nijin_reader_t* reader, const nijin_io_callbacks_t* callbacks) {
    if (!reader || !callbacks || !callbacks->read_at || callbacks->file_size == 0) {
        return NIJIN_ERR_INVALID_ARG;
    }
    try {
        reader->last_err.clear();
        reader->c_callbacks = *callbacks;
        nijinwvdc::IoCallbacks cb;
        cb.user_data = &reader->c_callbacks;
        cb.file_size = callbacks->file_size;
        cb.read_at = &c_io_read_trampoline;
        if (!reader->cpp.open_callbacks(cb, reader->last_err)) {
            return NIJIN_ERR_FORMAT;
        }
        return NIJIN_OK;
    } catch (const std::exception& e) {
        reader->last_err = e.what();
        return NIJIN_ERR_INTERNAL;
    } catch (...) {
        reader->last_err = "internal exception";
        return NIJIN_ERR_INTERNAL;
    }
}

void nijin_reader_close(nijin_reader_t* reader) {
    if (reader) {
        reader->cpp.close();
        reader->last_err.clear();
    }
}

int nijin_reader_is_open(const nijin_reader_t* reader) {
    return (reader && reader->cpp.is_open()) ? 1 : 0;
}

nijin_status_t nijin_reader_get_clip_info(const nijin_reader_t* reader, nijin_clip_info_t* out_info) {
    if (!reader || !out_info || !reader->cpp.is_open()) {
        return NIJIN_ERR_INVALID_ARG;
    }
    const auto& c = reader->cpp.clip_info();
    std::memset(out_info, 0, sizeof(*out_info));
    out_info->container_format = static_cast<uint32_t>(c.container_format);
    out_info->codec_family = static_cast<uint32_t>(c.codec_family);
    out_info->file_size = c.file_size;
    out_info->frame_count = static_cast<uint32_t>(c.video_samples.size());
    out_info->movie_timescale = c.movie_timescale;
    out_info->movie_duration = c.movie_duration;
    out_info->fps = c.fps;
    if (c.video_track_index >= 0 &&
        static_cast<size_t>(c.video_track_index) < c.tracks.size()) {
        out_info->width = c.tracks[static_cast<size_t>(c.video_track_index)].width;
        out_info->height = c.tracks[static_cast<size_t>(c.video_track_index)].height;
    }
    if ((out_info->width == 0 || out_info->height == 0) && c.has_single_frame_meta) {
        out_info->width = c.single_frame_meta.width;
        out_info->height = c.single_frame_meta.height;
    }
    out_info->active_w = c.nctg_active_w;
    out_info->active_h = c.nctg_active_h;
    const auto a = reader->cpp.audio_info();
    out_info->has_audio = a.available ? 1u : 0u;
    out_info->has_timecode = (c.timecode_track_index >= 0 && !c.timecode_samples.empty()) ? 1u : 0u;
    out_info->audio_channels = a.channels;
    out_info->audio_bits_per_channel = a.bits_per_channel;
    out_info->audio_sample_rate = a.sample_rate;
    out_info->audio_total_frames = a.total_frames;
    copy_str_trunc(out_info->make, sizeof(out_info->make), c.make);
    copy_str_trunc(out_info->model, sizeof(out_info->model), c.model);
    copy_str_trunc(out_info->major_brand, sizeof(out_info->major_brand), c.major_brand);
    return NIJIN_OK;
}

uint32_t nijin_reader_frame_count(const nijin_reader_t* reader) {
    return reader ? reader->cpp.frame_count() : 0u;
}

nijin_status_t nijin_reader_get_frame_meta(
    const nijin_reader_t* reader,
    uint32_t frame_index,
    nijin_frame_meta_t* out_meta) {
    if (!reader || !out_meta || !reader->cpp.is_open()) {
        return NIJIN_ERR_INVALID_ARG;
    }
    try {
        nijinwvdc::NevFrameMeta m;
        std::string err;
        if (!reader->cpp.read_frame_packet(frame_index, m, nullptr, err)) {
            return NIJIN_ERR_IO;
        }
        nijinwvdc::VideoFrameTiming timing;
        std::string terr;
        const bool has_t = reader->cpp.get_frame_timing(frame_index, timing, terr);
        fill_c_frame_meta(m, has_t ? &timing : nullptr, out_meta);
        return NIJIN_OK;
    } catch (...) {
        return NIJIN_ERR_INTERNAL;
    }
}

nijin_status_t nijin_reader_read_frame_packet(
    const nijin_reader_t* reader,
    uint32_t frame_index,
    uint8_t* out_buf,
    size_t capacity,
    size_t* out_size,
    nijin_frame_meta_t* out_meta) {
    if (!reader || !out_size || !reader->cpp.is_open()) {
        return NIJIN_ERR_INVALID_ARG;
    }
    try {
        nijinwvdc::NevFrameMeta m;
        std::string err;
        if (!out_buf) {
            if (!reader->cpp.read_frame_packet(frame_index, m, nullptr, err)) {
                return NIJIN_ERR_IO;
            }
            *out_size = m.codestream_size;
            if (out_meta) {
                nijinwvdc::VideoFrameTiming timing;
                std::string terr;
                const bool has_t = reader->cpp.get_frame_timing(frame_index, timing, terr);
                fill_c_frame_meta(m, has_t ? &timing : nullptr, out_meta);
            }
            return NIJIN_OK;
        }
        std::vector<uint8_t> cs;
        if (!reader->cpp.read_frame_packet(frame_index, m, &cs, err)) {
            return NIJIN_ERR_IO;
        }
        if (out_meta) {
            nijinwvdc::VideoFrameTiming timing;
            std::string terr;
            const bool has_t = reader->cpp.get_frame_timing(frame_index, timing, terr);
            fill_c_frame_meta(m, has_t ? &timing : nullptr, out_meta);
        }
        return copy_blob_to_c_buf(cs, out_buf, capacity, out_size);
    } catch (...) {
        return NIJIN_ERR_INTERNAL;
    }
}

nijin_status_t nijin_reader_get_audio_info(const nijin_reader_t* reader, nijin_audio_info_t* out_info) {
    if (!reader || !out_info || !reader->cpp.is_open()) {
        return NIJIN_ERR_INVALID_ARG;
    }
    const auto a = reader->cpp.audio_info();
    std::memset(out_info, 0, sizeof(*out_info));
    out_info->available = a.available ? 1u : 0u;
    out_info->is_signed = a.is_signed ? 1u : 0u;
    out_info->is_float = a.is_float ? 1u : 0u;
    out_info->is_big_endian = a.is_big_endian ? 1u : 0u;
    copy_str_trunc(out_info->format, sizeof(out_info->format), a.format);
    out_info->timescale = a.timescale;
    out_info->sample_rate = a.sample_rate;
    out_info->channels = a.channels;
    out_info->bits_per_channel = a.bits_per_channel;
    out_info->bytes_per_packet = a.bytes_per_packet;
    out_info->lpcm_flags = a.lpcm_flags;
    out_info->total_frames = a.total_frames;
    return NIJIN_OK;
}

nijin_status_t nijin_reader_read_audio_pcm(
    const nijin_reader_t* reader,
    uint64_t start_frame,
    uint64_t frame_count,
    void* dst,
    size_t dst_bytes,
    uint64_t* out_frames_read,
    size_t* out_bytes_written) {
    if (!reader || !reader->cpp.is_open()) {
        return NIJIN_ERR_INVALID_ARG;
    }
    try {
        const auto a = reader->cpp.audio_info();
        if (!a.available || a.bytes_per_packet == 0) {
            if (out_frames_read) *out_frames_read = 0;
            if (out_bytes_written) *out_bytes_written = 0;
            return NIJIN_ERR_UNSUPPORTED;
        }
        uint64_t got_frames = 0;
        std::string err;
        if (!reader->cpp.read_audio_pcm_into(start_frame, frame_count, dst, dst_bytes, &got_frames, err)) {
            if (err.find("too small") != std::string::npos) {
                return NIJIN_ERR_BUFFER_TOO_SMALL;
            }
            return NIJIN_ERR_IO;
        }
        if (out_frames_read) *out_frames_read = got_frames;
        if (out_bytes_written) {
            *out_bytes_written = static_cast<size_t>(got_frames * static_cast<uint64_t>(a.bytes_per_packet));
        }
        return NIJIN_OK;
    } catch (...) {
        return NIJIN_ERR_INTERNAL;
    }
}

nijin_status_t nijin_reader_export_audio_wav(const nijin_reader_t* reader, const char* utf8_wav_path) {
    if (!reader || !utf8_wav_path || !*utf8_wav_path || !reader->cpp.is_open()) {
        return NIJIN_ERR_INVALID_ARG;
    }
    try {
        std::string err;
        if (!reader->cpp.export_audio_wav(utf8_wav_path, err)) {
            return (err.find("does not contain") != std::string::npos ||
                    err.find("Unsupported") != std::string::npos)
                       ? NIJIN_ERR_UNSUPPORTED
                       : NIJIN_ERR_IO;
        }
        return NIJIN_OK;
    } catch (...) {
        return NIJIN_ERR_INTERNAL;
    }
}

nijin_status_t nijin_reader_extract_preview_jpeg(
    const nijin_reader_t* reader,
    uint8_t* out_buf,
    size_t capacity,
    size_t* out_size) {
    if (!reader || !out_size || !reader->cpp.is_open()) {
        return NIJIN_ERR_INVALID_ARG;
    }
    try {
        std::vector<uint8_t> jpeg;
        std::string err;
        if (!reader->cpp.extract_preview_jpeg(jpeg, err)) {
            return (err.find("does not contain") != std::string::npos)
                       ? NIJIN_ERR_UNSUPPORTED
                       : NIJIN_ERR_IO;
        }
        return copy_blob_to_c_buf(jpeg, out_buf, capacity, out_size);
    } catch (...) {
        return NIJIN_ERR_INTERNAL;
    }
}

nijin_decoder_t* nijin_decoder_create(int32_t num_threads) {
    try {
        return new nijin_decoder_s(num_threads);
    } catch (...) {
        return nullptr;
    }
}

void nijin_decoder_destroy(nijin_decoder_t* decoder) {
    delete decoder;
}

const char* nijin_decoder_last_error(const nijin_decoder_t* decoder) {
    return decoder ? decoder->last_err.c_str() : "";
}

void nijin_decoder_set_num_threads(nijin_decoder_t* decoder, int32_t num_threads) {
    if (decoder) {
        decoder->cpp.set_num_threads(num_threads);
    }
}

int32_t nijin_decoder_num_threads(const nijin_decoder_t* decoder) {
    return decoder ? decoder->cpp.num_threads() : 0;
}

void nijin_decode_options_init(nijin_decode_options_t* opts) {
    if (!opts) {
        return;
    }
    std::memset(opts, 0, sizeof(*opts));
    opts->struct_size = static_cast<uint32_t>(sizeof(*opts));
}

nijin_status_t nijin_decoder_decode_codestream(
    nijin_decoder_t* decoder,
    const uint8_t* data,
    size_t len,
    const nijin_decode_options_t* opts,
    uint16_t* dst,
    size_t dst_row_stride_bytes,
    size_t dst_height_capacity,
    nijin_decoded_info_t* out_info) {
    if (!decoder || !data || len == 0 || !dst) {
        return NIJIN_ERR_INVALID_ARG;
    }
    try {
        decoder->last_err.clear();
        const nijinwvdc::DecodeOptions cpp_opts = to_cpp_decode_options(opts);
        nijinwvdc::DecodedFrameInfo info;
        if (!decoder->cpp.decode_codestream_into(
                data, len, cpp_opts, dst, dst_row_stride_bytes, dst_height_capacity,
                info, decoder->last_err)) {
            return (decoder->last_err.find("too small") != std::string::npos)
                       ? NIJIN_ERR_BUFFER_TOO_SMALL
                       : NIJIN_ERR_DECODE;
        }
        if (out_info) {
            fill_c_decoded_info(info, out_info);
        }
        return NIJIN_OK;
    } catch (const std::exception& e) {
        decoder->last_err = e.what();
        return NIJIN_ERR_INTERNAL;
    } catch (...) {
        decoder->last_err = "internal exception";
        return NIJIN_ERR_INTERNAL;
    }
}

nijin_status_t nijin_decoder_decode_frame(
    nijin_decoder_t* decoder,
    const nijin_reader_t* reader,
    uint32_t frame_index,
    const nijin_decode_options_t* opts,
    uint16_t* dst,
    size_t dst_row_stride_bytes,
    size_t dst_height_capacity,
    nijin_decoded_info_t* out_info) {
    if (!decoder || !reader || !reader->cpp.is_open() || !dst) {
        return NIJIN_ERR_INVALID_ARG;
    }
    try {
        decoder->last_err.clear();
        const nijinwvdc::DecodeOptions cpp_opts = to_cpp_decode_options(opts);
        nijinwvdc::DecodedFrameInfo info;
        if (!decoder->cpp.decode_frame_into(
                reader->cpp, frame_index, cpp_opts, dst, dst_row_stride_bytes, dst_height_capacity,
                info, decoder->last_err)) {
            return (decoder->last_err.find("too small") != std::string::npos)
                       ? NIJIN_ERR_BUFFER_TOO_SMALL
                       : NIJIN_ERR_DECODE;
        }
        if (out_info) {
            fill_c_decoded_info(info, out_info);
        }
        return NIJIN_OK;
    } catch (const std::exception& e) {
        decoder->last_err = e.what();
        return NIJIN_ERR_INTERNAL;
    } catch (...) {
        decoder->last_err = "internal exception";
        return NIJIN_ERR_INTERNAL;
    }
}

nijin_status_t nijin_crop_bayer_strided(
    const uint16_t* src,
    size_t src_row_stride_bytes,
    uint32_t src_w,
    uint32_t src_h,
    const uint16_t crop_xywh[4],
    const uint8_t src_cfa[4],
    uint16_t* dst,
    size_t dst_row_stride_bytes,
    uint8_t dst_cfa[4]) {
    return nijinwvdc::apply_bayer_crop_strided(
               src, src_row_stride_bytes, src_w, src_h, crop_xywh, src_cfa,
               dst, dst_row_stride_bytes, dst_cfa)
               ? NIJIN_OK
               : NIJIN_ERR_INVALID_ARG;
}

nijin_status_t nijin_split_bayer_planes(
    const uint16_t* src,
    size_t src_row_stride_bytes,
    uint32_t width,
    uint32_t height,
    uint16_t* c00,
    uint16_t* c01,
    uint16_t* c10,
    uint16_t* c11,
    size_t plane_row_stride_bytes) {
    return nijinwvdc::split_bayer_cfa_planes(
               src, src_row_stride_bytes, width, height,
               c00, c01, c10, c11, plane_row_stride_bytes)
               ? NIJIN_OK
               : NIJIN_ERR_INVALID_ARG;
}

nijin_status_t nijin_normalize_bayer_float(
    const uint16_t* src,
    size_t src_row_stride_bytes,
    uint32_t width,
    uint32_t height,
    const float black[4],
    float white_level,
    int clamp_01,
    float* dst,
    size_t dst_row_stride_bytes) {
    return nijinwvdc::normalize_bayer_to_float(
               src, src_row_stride_bytes, width, height,
               black, white_level, (clamp_01 != 0), dst, dst_row_stride_bytes)
               ? NIJIN_OK
               : NIJIN_ERR_INVALID_ARG;
}

void nijin_dng_options_init(nijin_dng_options_t* opts) {
    if (!opts) {
        return;
    }
    try {
        nijinwvdc::DngWriteOptions def;
        to_c_dng_options(def, opts);
    } catch (...) {
        std::memset(opts, 0, sizeof(*opts));
    }
}

nijin_status_t nijin_dng_options_from_clip(
    const nijin_reader_t* reader,
    uint32_t frame_index,
    const nijin_decoded_info_t* decoded,
    int apply_active_crop,
    uint16_t override_orientation,
    nijin_dng_options_t* out_opts) {
    if (!reader || !reader->cpp.is_open() || !decoded || !out_opts) {
        return NIJIN_ERR_INVALID_ARG;
    }
    try {
        nijinwvdc::NevFrameMeta meta;
        std::string err;
        if (!reader->cpp.read_frame_packet(frame_index, meta, nullptr, err)) {
            return NIJIN_ERR_IO;
        }
        const auto& clip = reader->cpp.clip_info();
        nijinwvdc::DngWriteOptions dopts;
        if (decoded->codec == NIJIN_CODEC_ARW6) {
            nijinwvdc::Arw6Info arw;
            arw.width = decoded->coded_width;
            arw.height = decoded->coded_height;
            arw.bit_depth = decoded->bit_depth;
            nijinwvdc::build_dng_options_from_clip_and_frame(
                clip, meta, arw, (apply_active_crop != 0), override_orientation, dopts);
        } else {
            nijinwvdc::TicoRawInfo jxs;
            jxs.width = static_cast<uint16_t>(decoded->coded_width);
            jxs.height = static_cast<uint16_t>(decoded->coded_height);
            jxs.cdt_depth = decoded->bit_depth;
            jxs.out_bitdepth = decoded->bit_depth;
            nijinwvdc::build_dng_options_from_clip_and_frame(
                clip, meta, jxs, (apply_active_crop != 0), override_orientation, dopts);
        }
        if (decoded->output_width > 0 && decoded->output_height > 0 &&
            (decoded->output_width != decoded->coded_width ||
             decoded->output_height != decoded->coded_height ||
             decoded->offset_x != 0 || decoded->offset_y != 0)) {
            dopts.width = decoded->output_width;
            dopts.height = decoded->output_height;
            std::memcpy(dopts.cfa_pattern, decoded->cfa_pattern, 4);
            dopts.has_crop = false;
            dopts.crop_origin[0] = 0;
            dopts.crop_origin[1] = 0;
            dopts.crop_size[0] = 0;
            dopts.crop_size[1] = 0;
        }
        to_c_dng_options(dopts, out_opts);
        return NIJIN_OK;
    } catch (...) {
        return NIJIN_ERR_INTERNAL;
    }
}

nijin_status_t nijin_build_dng_header(
    const nijin_dng_options_t* opts,
    uint8_t* out_buf,
    size_t capacity,
    size_t* out_size) {
    if (!opts || !out_size) {
        return NIJIN_ERR_INVALID_ARG;
    }
    try {
        nijinwvdc::DngWriteOptions cpp_opts;
        nijin_status_t st = to_cpp_dng_options(opts, cpp_opts);
        if (st != NIJIN_OK) {
            return st;
        }
        std::vector<uint8_t> hdr;
        std::string err;
        if (!nijinwvdc::build_cfa_dng_header(cpp_opts, hdr, err)) {
            return NIJIN_ERR_INVALID_ARG;
        }
        return copy_blob_to_c_buf(hdr, out_buf, capacity, out_size);
    } catch (...) {
        return NIJIN_ERR_INTERNAL;
    }
}

nijin_status_t nijin_write_dng_file(
    const char* utf8_path,
    const uint16_t* pixels,
    size_t src_row_stride_bytes,
    const nijin_dng_options_t* opts,
    char* err_buf,
    size_t err_buf_cap) {
    if (!utf8_path || !*utf8_path || !pixels || !opts) {
        write_err_buf(err_buf, err_buf_cap, "Invalid argument");
        return NIJIN_ERR_INVALID_ARG;
    }
    try {
        nijinwvdc::DngWriteOptions cpp_opts;
        nijin_status_t st = to_cpp_dng_options(opts, cpp_opts);
        if (st != NIJIN_OK) {
            write_err_buf(err_buf, err_buf_cap, "Invalid DNG options struct_size");
            return st;
        }
        const size_t min_stride = static_cast<size_t>(cpp_opts.width) * sizeof(uint16_t);
        const size_t stride = (src_row_stride_bytes == 0) ? min_stride : src_row_stride_bytes;
        if (cpp_opts.width == 0 || cpp_opts.height == 0 || stride < min_stride) {
            write_err_buf(err_buf, err_buf_cap, "Invalid DNG dimensions or row stride");
            return NIJIN_ERR_INVALID_ARG;
        }
        std::string err;
        if (stride == min_stride) {
            if (!nijinwvdc::write_cfa_dng(utf8_path, pixels, cpp_opts, err)) {
                write_err_buf(err_buf, err_buf_cap, err);
                return NIJIN_ERR_IO;
            }
            return NIJIN_OK;
        }

        std::vector<uint8_t> hdr;
        if (!nijinwvdc::build_cfa_dng_header(cpp_opts, hdr, err)) {
            write_err_buf(err_buf, err_buf_cap, err);
            return NIJIN_ERR_INVALID_ARG;
        }
        FILE* fp = nijinwvdc::detail::open_c_file_write(utf8_path);
        if (!fp) {
            write_err_buf(err_buf, err_buf_cap, "Failed opening DNG output for writing");
            return NIJIN_ERR_IO;
        }
        bool ok = (std::fwrite(hdr.data(), 1, hdr.size(), fp) == hdr.size());
        const uint8_t* p = reinterpret_cast<const uint8_t*>(pixels);
        for (uint32_t y = 0; y < cpp_opts.height && ok; ++y) {
            if (std::fwrite(p + static_cast<size_t>(y) * stride, 1, min_stride, fp) != min_stride) {
                ok = false;
            }
        }
        if (std::fclose(fp) != 0) ok = false;
        if (!ok) {
            write_err_buf(err_buf, err_buf_cap, "Write failure while writing DNG");
            return NIJIN_ERR_IO;
        }
        return NIJIN_OK;
    } catch (const std::exception& e) {
        write_err_buf(err_buf, err_buf_cap, e.what());
        return NIJIN_ERR_INTERNAL;
    } catch (...) {
        write_err_buf(err_buf, err_buf_cap, "internal exception");
        return NIJIN_ERR_INTERNAL;
    }
}

} // extern "C"
