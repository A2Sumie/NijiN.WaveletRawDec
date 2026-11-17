// SPDX-License-Identifier: Apache-2.0
#ifndef NIJINWVDC_CODEC_INTERNAL_H
#define NIJINWVDC_CODEC_INTERNAL_H

#include "../include/nijinWVDC.h"
#include "worker_pool.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace nijinwvdc {
namespace detail {

struct TicoArena;
struct Arw6Arena;

TicoArena* create_tico_arena();
void destroy_tico_arena(TicoArena* arena);

Arw6Arena* create_arw6_arena();
void destroy_arw6_arena(Arw6Arena* arena);

bool decode_ticoraw_bayer_strided(
    const uint8_t* data,
    size_t len,
    int num_threads,
    uint16_t* dst,
    size_t dst_stride_u16,
    size_t dst_height_capacity,
    TicoRawInfo& info_out,
    std::string& err,
    WorkerPool* external_pool,
    TicoArena* external_arena);

bool decode_arw6_bayer_strided(
    const uint8_t* strip_data,
    size_t strip_len,
    int num_threads,
    uint16_t* dst,
    size_t dst_stride_u16,
    size_t dst_height_capacity,
    Arw6Info& info_out,
    std::string& err,
    WorkerPool* external_pool,
    Arw6Arena* external_arena);

bool decode_arw6_tile_strided(
    const uint8_t* cs_data,
    size_t cs_len,
    int num_threads,
    uint32_t tile_x,
    uint32_t tile_y,
    uint32_t full_w,
    uint32_t full_h,
    uint16_t* dst,
    size_t dst_stride_u16,
    size_t dst_height_capacity,
    Arw6Info& tile_info_out,
    std::string& err,
    WorkerPool* external_pool,
    Arw6Arena* external_arena);

} // namespace detail
} // namespace nijinwvdc

#endif // NIJINWVDC_CODEC_INTERNAL_H
