/* SPDX-License-Identifier: Apache-2.0 */
#include "../include/nijinWVDC_c.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C_CHECK(expr)                                                                         \
    do {                                                                                      \
        if (!(expr)) {                                                                        \
            fprintf(stderr, "C_CHECK FAILED (%s:%d): %s\n", __FILE__, __LINE__, #expr);       \
            abort();                                                                          \
        }                                                                                     \
    } while (0)

typedef struct {
    const uint8_t* data;
    size_t len;
} mem_slice_ctx_t;

static int mem_read_cb(void* user_data, uint64_t offset, void* dst, size_t size, size_t* bytes_read_out) {
    const mem_slice_ctx_t* ctx = (const mem_slice_ctx_t*)user_data;
    if (!ctx || offset > ctx->len || size > ctx->len - (size_t)offset) {
        return -1;
    }
    if (size > 0) {
        memcpy(dst, ctx->data + (size_t)offset, size);
    }
    *bytes_read_out = size;
    return 0;
}

static void put_be16_c(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xFFu);
    p[1] = (uint8_t)(v & 0xFFu);
}

typedef struct {
    uint8_t* buf;
    size_t pos;
    uint8_t cur;
    int bit_pos;
} c_bw_t;

static void cbw_init(c_bw_t* bw, uint8_t* dst) {
    bw->buf = dst;
    bw->pos = 0;
    bw->cur = 0;
    bw->bit_pos = 0;
}

static void cbw_push(c_bw_t* bw, uint32_t v, int n) {
    int i;
    for (i = n - 1; i >= 0; --i) {
        bw->cur = (uint8_t)(bw->cur | (((v >> i) & 1u) << (7 - bw->bit_pos)));
        bw->bit_pos++;
        if (bw->bit_pos == 8) {
            bw->buf[bw->pos++] = bw->cur;
            bw->cur = 0;
            bw->bit_pos = 0;
        }
    }
}

static size_t cbw_finish16(c_bw_t* bw) {
    if (bw->bit_pos > 0) {
        bw->buf[bw->pos++] = bw->cur;
        bw->cur = 0;
        bw->bit_pos = 0;
    }
    while (bw->pos == 0 || (bw->pos % 16u) != 0u) {
        bw->buf[bw->pos++] = 0u;
    }
    return bw->pos;
}

static void put_be24_c(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)((v >> 16) & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)(v & 0xFFu);
}

static size_t emit_res_stream_c(uint8_t* dst, int n_bands, int chunk_count) {
    c_bw_t hdr, dir;
    size_t hlen, dlen, plen;
    int ci, b;
    cbw_init(&hdr, dst);
    cbw_push(&hdr, 1u, 16);
    cbw_push(&hdr, 1u, 24);
    cbw_push(&hdr, 4u, 4);
    cbw_push(&hdr, 0u, 4);
    cbw_push(&hdr, (uint32_t)n_bands, 2);
    cbw_push(&hdr, 0u, 6);
    cbw_push(&hdr, (uint32_t)chunk_count, 16);
    cbw_push(&hdr, 0x10u, 8);
    cbw_push(&hdr, 0x00u, 8);
    cbw_push(&hdr, 0u, 32);
    cbw_push(&hdr, 0u, 8);
    hlen = cbw_finish16(&hdr);

    cbw_init(&dir, dst + hlen);
    for (ci = 0; ci < chunk_count; ++ci) {
        cbw_push(&dir, 8u, 16);
        for (b = 0; b < n_bands; ++b) {
            cbw_push(&dir, 0u, 4);
        }
    }
    dlen = cbw_finish16(&dir);
    plen = 16u;
    memset(dst + hlen + dlen, 0, plen);
    return hlen + dlen + plen;
}

static void build_synth_arw6_16x16_zero_c(uint8_t* out_cs, size_t cap, size_t* out_len) {
    size_t stream_lens[5][3];
    uint32_t grp_bytes[4] = {0, 0, 0, 0};
    size_t pos = 128u;
    int r, c, g;

    C_CHECK(cap >= 1024u);
    memset(out_cs, 0, 128u);
    memcpy(out_cs, "A000", 4);
    put_be16_c(out_cs + 8, 16u);
    put_be16_c(out_cs + 10, 8u);
    put_be16_c(out_cs + 12, (uint16_t)(14u << 4));
    out_cs[14] = (3u << 5) | (3u << 2) | 0x02u;
    out_cs[15] = 0x00u;

    for (r = 0; r < 4; ++r) {
        int nb = (r == 0) ? 1 : 3;
        int nc = (r == 0) ? 1 : 2;
        for (c = 0; c < 3; ++c) {
            size_t sl = emit_res_stream_c(out_cs + pos, nb, nc);
            stream_lens[r][c] = sl;
            grp_bytes[r] += (uint32_t)sl;
            pos += sl;
        }
    }
    stream_lens[4][0] = emit_res_stream_c(out_cs + pos, 1, 2);
    pos += stream_lens[4][0];

    for (g = 0; g < 4; ++g) {
        put_be24_c(out_cs + 16u + (size_t)g * 3u, grp_bytes[g] >> 4);
    }
    for (g = 0; g < 5; ++g) {
        size_t rec = 48u + (size_t)g * 16u;
        int cnt = (g < 4) ? 3 : 1;
        out_cs[rec] = (uint8_t)cnt;
        for (c = 0; c < cnt; ++c) {
            put_be24_c(out_cs + rec + 1u + (size_t)c * 3u, (uint32_t)(stream_lens[g][c] >> 4));
        }
    }
    *out_len = pos;
}

static void get_tmp_path(const char* fname, char* out, size_t cap) {
    const char* td = getenv("TMPDIR");
#ifdef _WIN32
    if (!td || !*td) td = getenv("TEMP");
    if (!td || !*td) td = getenv("TMP");
    if (!td || !*td) td = ".";
#else
    if (!td || !*td) td = "/tmp";
#endif
    snprintf(out, cap, "%s/%s", td, fname);
}

int main(void) {
    uint8_t cs[1024];
    size_t cs_len = 0;
    nijin_reader_t* reader = NULL;
    nijin_reader_t* reader_cb = NULL;
    nijin_decoder_t* dec = NULL;
    nijin_clip_info_t clip;
    nijin_frame_meta_t fmeta;
    nijin_decoded_info_t dinfo;
    nijin_decode_options_t dopts;
    nijin_dng_options_t dng_opts;
    mem_slice_ctx_t ctx;
    nijin_io_callbacks_t cb;
    uint8_t pkt_buf[1024];
    size_t pkt_sz = 0;
    uint16_t bayer[16 * 16];
    uint16_t strided_bayer[16 * 24];
    uint16_t roi_bayer[10 * 10];
    uint16_t p00[8 * 8], p01[8 * 8], p10[8 * 8], p11[8 * 8];
    float fnorm[16 * 16];
    const float fblk[4] = {100.0f, 100.0f, 100.0f, 100.0f};
    uint8_t hdr[4096];
    size_t hdr_sz = 0;
    char dng_path[512];
    char err_buf[256];
    FILE* fp = NULL;
    uint8_t fhead[16];
    uint32_t x, y;

    C_CHECK(strcmp(nijin_version(), NIJINWVDC_VERSION_STRING) == 0);
    C_CHECK(nijin_status_string(NIJIN_OK) != NULL);
    (void)nijin_simd_capabilities();

    build_synth_arw6_16x16_zero_c(cs, sizeof(cs), &cs_len);

    reader = nijin_reader_create();
    C_CHECK(reader != NULL);
    C_CHECK(nijin_reader_open_memory(reader, cs, cs_len) == NIJIN_OK);
    C_CHECK(nijin_reader_is_open(reader) == 1);
    C_CHECK(nijin_reader_frame_count(reader) == 1u);
    C_CHECK(nijin_reader_get_clip_info(reader, &clip) == NIJIN_OK);
    C_CHECK(clip.container_format == NIJIN_CONTAINER_RAW_CODESTREAM);
    C_CHECK(clip.codec_family == NIJIN_CODEC_ARW6);
    C_CHECK(clip.width == 16u && clip.height == 16u);
    C_CHECK(nijin_reader_get_frame_meta(reader, 0u, &fmeta) == NIJIN_OK);
    C_CHECK(fmeta.focus_distance_m == 0.0f && fmeta.focus_is_infinity == 0u &&
            fmeta.has_lens_position_abs == 0u);
    C_CHECK(nijin_reader_read_frame_packet(reader, 0u, NULL, 0u, &pkt_sz, NULL) == NIJIN_OK);
    C_CHECK(pkt_sz == cs_len);
    C_CHECK(nijin_reader_read_frame_packet(reader, 0u, pkt_buf, 4u, &pkt_sz, NULL) == NIJIN_ERR_BUFFER_TOO_SMALL);
    C_CHECK(nijin_reader_read_frame_packet(reader, 0u, pkt_buf, sizeof(pkt_buf), &pkt_sz, &fmeta) == NIJIN_OK);
    C_CHECK(memcmp(pkt_buf, cs, cs_len) == 0);

    ctx.data = cs;
    ctx.len = cs_len;
    cb.user_data = &ctx;
    cb.file_size = cs_len;
    cb.read_at = mem_read_cb;
    reader_cb = nijin_reader_create();
    C_CHECK(reader_cb != NULL);
    C_CHECK(nijin_reader_open_callbacks(reader_cb, &cb) == NIJIN_OK);

    dec = nijin_decoder_create(2);
    C_CHECK(dec != NULL);
    C_CHECK(nijin_decoder_num_threads(dec) == 2);
    nijin_decoder_set_num_threads(dec, 1);
    C_CHECK(nijin_decoder_num_threads(dec) == 1);

    nijin_decode_options_init(&dopts);
    C_CHECK(nijin_decoder_decode_frame(dec, reader, 0u, &dopts, bayer, 0u, 16u, &dinfo) == NIJIN_OK);
    C_CHECK(dinfo.codec == NIJIN_CODEC_ARW6);
    C_CHECK(dinfo.coded_width == 16u && dinfo.coded_height == 16u);
    C_CHECK(dinfo.output_width == 16u && dinfo.output_height == 16u);

    /* Insufficient height capacity must be rejected cleanly */
    C_CHECK(nijin_decoder_decode_frame(dec, reader_cb, 0u, &dopts, bayer, 0u, 8u, &dinfo) == NIJIN_ERR_BUFFER_TOO_SMALL);

    /* Strided decode + ROI decode */
    for (x = 0; x < 16u * 24u; ++x) strided_bayer[x] = 0xCAFEu;
    C_CHECK(nijin_decoder_decode_codestream(
        dec, cs, cs_len, &dopts, strided_bayer, 24u * sizeof(uint16_t), 16u, &dinfo) == NIJIN_OK);
    for (y = 0; y < 16u; ++y) {
        C_CHECK(memcmp(strided_bayer + y * 24u, bayer + y * 16u, 16u * sizeof(uint16_t)) == 0);
        for (x = 16u; x < 24u; ++x) C_CHECK(strided_bayer[y * 24u + x] == 0xCAFEu);
    }

    dopts.has_roi = 1u;
    dopts.want_quant_profile = 1u;
    dopts.roi[0] = 2u; dopts.roi[1] = 2u; dopts.roi[2] = 10u; dopts.roi[3] = 10u;
    C_CHECK(nijin_decoder_decode_frame(dec, reader_cb, 0u, &dopts, roi_bayer, 0u, 10u, &dinfo) == NIJIN_OK);
    C_CHECK(dinfo.output_width == 10u && dinfo.output_height == 10u);
    C_CHECK(dinfo.has_quant_profile == 1u);
    for (y = 0; y < 10u; ++y) {
        C_CHECK(memcmp(roi_bayer + y * 10u, bayer + (y + 2u) * 16u + 2u, 10u * sizeof(uint16_t)) == 0);
    }
    {
        nijin_wavelet_quant_summary_t qsum;
        float q_luma[8], q_chroma[8];
        float walsh_sum = 0.0f;
        uint32_t k;
        nijin_wavelet_quant_summary_init(&qsum);
        C_CHECK(nijin_decoder_get_last_quant_profile(dec, &qsum, q_luma, q_chroma, 1u) == NIJIN_ERR_BUFFER_TOO_SMALL);
        C_CHECK(nijin_decoder_get_last_quant_profile(dec, &qsum, q_luma, q_chroma, 8u) == NIJIN_OK);
        C_CHECK(qsum.available == 1u && qsum.codec == NIJIN_CODEC_ARW6 && qsum.row_step == 8u && qsum.num_rows == 2u);
        for (k = 0; k < 16u; ++k) walsh_sum += qsum.walsh16_luma_norm[k];
        C_CHECK(walsh_sum > 15.5f && walsh_sum < 16.5f);
        C_CHECK(nijin_decoder_recalibrate_last_quant_range(dec, 512.0f, 16383.0f, &qsum) == NIJIN_OK);
        C_CHECK(qsum.black_ref == 512.0f && qsum.nlt_slope_lut[240] > qsum.nlt_slope_lut[4]);
    }

    C_CHECK(nijin_split_bayer_planes(bayer, 0u, 16u, 16u, p00, p01, p10, p11, 0u) == NIJIN_OK);
    C_CHECK(p00[0] == bayer[0] && p01[0] == bayer[1] && p10[0] == bayer[16] && p11[0] == bayer[17]);

    C_CHECK(nijin_normalize_bayer_float(bayer, 0u, 16u, 16u, fblk, 16383.0f, 1, fnorm, 0u) == NIJIN_OK);
    C_CHECK(fnorm[0] >= 0.0f && fnorm[0] <= 1.0f);

    nijin_dng_options_init(&dng_opts);
    C_CHECK(nijin_dng_options_from_clip(reader, 0u, &dinfo, 0, 1u, &dng_opts) == NIJIN_OK);
    dng_opts.focal_length_mm = 35.0f;
    dng_opts.focus_distance_m = 2.5f;
    dng_opts.focus_is_infinity = 0u;
    C_CHECK(nijin_build_dng_header(&dng_opts, NULL, 0u, &hdr_sz) == NIJIN_OK);
    C_CHECK(hdr_sz > 100u && hdr_sz <= sizeof(hdr));
    C_CHECK(nijin_build_dng_header(&dng_opts, hdr, 16u, &hdr_sz) == NIJIN_ERR_BUFFER_TOO_SMALL);
    C_CHECK(nijin_build_dng_header(&dng_opts, hdr, sizeof(hdr), &hdr_sz) == NIJIN_OK);

    /* Write strided ROI DNG */
    dng_opts.width = 16u;
    dng_opts.height = 16u;
    get_tmp_path("nijin_c_api_test.dng", dng_path, sizeof(dng_path));
    C_CHECK(nijin_write_dng_file(
        dng_path, strided_bayer, 24u * sizeof(uint16_t), &dng_opts, err_buf, sizeof(err_buf)) == NIJIN_OK);
    fp = fopen(dng_path, "rb");
    C_CHECK(fp != NULL);
    C_CHECK(fread(fhead, 1u, 8u, fp) == 8u);
    C_CHECK(fhead[0] == 'I' && fhead[1] == 'I' && fhead[2] == 42u);
    fclose(fp);
    remove(dng_path);

    /* Verify unused helpers compile & reject invalid inputs cleanly */
    {
        uint16_t crop_xywh[4] = {0u, 0u, 8u, 8u};
        uint8_t dst_cfa[4] = {0};
        uint16_t dst_crop[64];
        uint8_t src_cfa[4] = {0u, 1u, 1u, 2u};
        C_CHECK(nijin_crop_bayer_strided(
            bayer, 0u, 16u, 16u, crop_xywh, src_cfa, dst_crop, 0u, dst_cfa) == NIJIN_OK);
    }

    nijin_decoder_destroy(dec);
    nijin_reader_destroy(reader_cb);
    nijin_reader_destroy(reader);
    printf("test_c_api: ALL PASSED\n");
    return 0;
}
