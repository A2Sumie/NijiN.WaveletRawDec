// SPDX-License-Identifier: Apache-2.0
#ifndef NIJINWVDC_FOCUS_META_INTERNAL_H
#define NIJINWVDC_FOCUS_META_INTERNAL_H

#include "tiff_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace nijinwvdc {
namespace detail {

constexpr char ascii_tolower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr char ascii_toupper(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

constexpr bool starts_with_ci_ascii(std::string_view s, std::string_view prefix) noexcept {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (ascii_tolower(s[i]) != ascii_tolower(prefix[i])) {
            return false;
        }
    }
    return true;
}

constexpr bool ends_with_ci_ascii(std::string_view s, std::string_view suffix) noexcept {
    if (s.size() < suffix.size()) return false;
    const size_t off = s.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (ascii_tolower(s[off + i]) != ascii_tolower(suffix[i])) {
            return false;
        }
    }
    return true;
}

inline std::string to_upper_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = ascii_toupper(c);
    }
    return out;
}

consteval uint8_t sony_pow_mod_249(uint8_t base, uint8_t exp) noexcept {
    uint32_t res = 1u;
    uint32_t b = base % 249u;
    uint8_t e = exp;
    while (e > 0u) {
        if (e & 1u) res = (res * b) % 249u;
        b = (b * b) % 249u;
        e >>= 1u;
    }
    return static_cast<uint8_t>(res);
}

consteval std::array<uint8_t, 256> make_sony_94xx_inv_lut() noexcept {
    std::array<uint8_t, 256> lut{};
    for (uint32_t c = 0; c < 249u; ++c) {
        lut[c] = sony_pow_mod_249(static_cast<uint8_t>(c), 55u);
    }
    for (uint32_t c = 249u; c < 256u; ++c) {
        lut[c] = static_cast<uint8_t>(c);
    }
    return lut;
}

inline constexpr std::array<uint8_t, 256> kSony94xxInvLut = make_sony_94xx_inv_lut();

inline void set_focus_distance_with_rel_sigma(NevFrameMeta& meta, double dist_m, double rel_sig) {
    if (!(dist_m > 0.0) || !std::isfinite(dist_m)) return;
    double rs = std::max(0.02, std::min(0.85, rel_sig));
    meta.focus_is_infinity = false;
    meta.focus_distance_m = static_cast<float>(dist_m);
    meta.focus_distance_low_m = static_cast<float>(dist_m / (1.0 + rs));
    meta.focus_distance_high_m = static_cast<float>(dist_m / (1.0 - rs));
}

inline void set_focus_infinity(NevFrameMeta& meta) {
    meta.focus_is_infinity = true;
    meta.focus_distance_m = 1000000.0f;
    meta.focus_distance_low_m = 0.0f;
    meta.focus_distance_high_m = 0.0f;
}

inline void parse_sony_makernote_focus(
    const RandomAccessReader& r,
    uint64_t tiff_base,
    bool be,
    uint64_t mn_file_off,
    const std::string& make,
    const std::string& model,
    NevFrameMeta& meta) {
    if (mn_file_off <= tiff_base || mn_file_off + 16 > r.size()) return;
    const std::string mu = to_upper_ascii(make);
    const std::string ml = to_upper_ascii(model);
    bool is_sony = (mu.find("SONY") != std::string::npos ||
                    mu.find("MINOLTA") != std::string::npos ||
                    starts_with_ci_ascii(ml, "ILCE-") ||
                    starts_with_ci_ascii(ml, "ILCA-") ||
                    starts_with_ci_ascii(ml, "ILME-") ||
                    starts_with_ci_ascii(ml, "NEX-") ||
                    starts_with_ci_ascii(ml, "SLT-") ||
                    starts_with_ci_ascii(ml, "DSLR-A") ||
                    starts_with_ci_ascii(ml, "ZV-"));
    if (!is_sony) return;

    uint8_t mn_hdr[12] = {};
    if (!r.read(mn_file_off, mn_hdr, sizeof(mn_hdr))) return;
    uint64_t mn_ifd_abs = mn_file_off + (std::memcmp(mn_hdr, "SONY", 4) == 0 ? 12u : 0u);
    if (mn_ifd_abs < tiff_base || mn_ifd_abs - tiff_base > 0xFFFFFFFFULL) return;

    std::vector<TiffTagValue> mn_tags;
    if (!read_tiff_ifd(r, tiff_base, be, static_cast<uint32_t>(mn_ifd_abs - tiff_base), mn_tags)) {
        return;
    }

    const auto& kInvLut = kSony94xxInvLut;
    const TiffTagValue* tag_9402 = nullptr;
    const TiffTagValue* tag_9403 = nullptr;
    const TiffTagValue* tag_9404 = nullptr;
    const TiffTagValue* tag_0020 = nullptr;
    for (const auto& tv : mn_tags) {
        if (tv.tag == 0x9402) tag_9402 = &tv;
        else if (tv.tag == 0x9403) tag_9403 = &tv;
        else if (tv.tag == 0x9404) tag_9404 = &tv;
        else if (tv.tag == 0x0020) tag_0020 = &tv;
    }

    float f_nom = 0.0f;
    float f_opt = 0.0f;
    uint16_t focus_pos = 0;
    bool is_log_pos2 = false;
    bool is_slt_ilca = starts_with_ci_ascii(ml, "SLT-") ||
                       starts_with_ci_ascii(ml, "HV") ||
                       starts_with_ci_ascii(ml, "ILCA-");
    bool is_dsc = starts_with_ci_ascii(ml, "DSC-") || starts_with_ci_ascii(ml, "STELLAR");

    // Branch A: modern E-mount Tag 0x9402
    if (!is_slt_ilca && !is_dsc && tag_9402 && tag_9402->data.size() >= 0x2Eu) {
        const auto& raw = tag_9402->data;
        if (raw[0] != 0x05u && raw[0] != 0xFFu) {
            uint8_t dec[0x30] = {};
            for (size_t i = 0; i < 0x30u; ++i) {
                dec[i] = kInvLut[raw[i]];
            }
            if (dec[0] != 0x00u) {
                uint16_t u_nom = rd_le16(dec + 0x24);
                uint16_t u_opt = rd_le16(dec + 0x28);
                if (u_nom > 0u && u_nom < 60000u) f_nom = static_cast<float>(u_nom) * 0.1f;
                if (u_opt > 0u && u_opt < 60000u) f_opt = static_cast<float>(u_opt) * 0.1f;
                focus_pos = dec[0x2D];
                is_log_pos2 = (focus_pos > 0u);
            }
        }
    }

    // Branch B: SLT / ILCA Tag 0x9404 / 0x9403
    if (focus_pos == 0u && is_slt_ilca && tag_9404 && tag_9404->data.size() > 0x20u) {
        const auto& raw = tag_9404->data;
        uint8_t d0 = kInvLut[raw[0]];
        uint8_t d3 = kInvLut[raw[3]];
        if ((d0 == 9u || d0 == 12u || d0 == 13u || d0 == 15u || d0 == 16u) && d3 == 2u) {
            focus_pos = kInvLut[raw[0x20]];
            is_log_pos2 = (focus_pos > 0u);
        }
    }
    if (focus_pos == 0u && is_slt_ilca && tag_9403 && tag_9403->data.size() > 0x20u) {
        focus_pos = kInvLut[tag_9403->data[0x20]];
        is_log_pos2 = (focus_pos > 0u);
    }

    // Branch C / D: legacy MoreInfo / FocusInfo Tag 0x0020
    if (focus_pos == 0u && tag_0020) {
        const auto& b20 = tag_0020->data;
        size_t cnt = b20.size();
        if ((cnt == 19154u || cnt == 19148u) && starts_with_ci_ascii(ml, "DSLR-A")) {
            if (0x09BBu < cnt) {
                focus_pos = b20[0x09BB];
                is_log_pos2 = false;
            }
        } else if (cnt >= 64u && cnt != 19154u && cnt != 19148u) {
            uint16_t num = rd_le16(b20.data());
            if (num >= 1u && num <= 50u && 4u + static_cast<size_t>(num) * 4u <= cnt) {
                for (uint16_t i = 0; i < num; ++i) {
                    uint16_t stag = rd_le16(b20.data() + 4u + i * 4u);
                    uint16_t soff = rd_le16(b20.data() + 6u + i * 4u);
                    if (stag == 0x0001u && static_cast<size_t>(soff) + 0x30u <= cnt) {
                        size_t poff = 0x2Fu;
                        if (starts_with_ci_ascii(ml, "DSLR-A450") ||
                            starts_with_ci_ascii(ml, "DSLR-A500") ||
                            starts_with_ci_ascii(ml, "DSLR-A550")) {
                            poff = 0x29u;
                        } else if (ml == "NEX-3" || ml == "NEX-5" || ml == "NEX-5C") {
                            poff = 0x2Bu;
                        }
                        focus_pos = b20[static_cast<size_t>(soff) + poff];
                        is_log_pos2 = (focus_pos > 0u);
                        break;
                    }
                }
            }
        }
    }

    if (meta.focal_length_mm <= 0.0f && f_nom > 0.5f) {
        meta.focal_length_mm = f_nom;
    }
    float f_ref = (f_nom > 0.5f) ? f_nom : meta.focal_length_mm;
    float f_eff = meta.focal_length_mm;
    if (f_opt > 0.5f && (f_ref <= 0.5f || (f_opt >= 0.5f * f_ref && f_opt <= 1.5f * f_ref))) {
        meta.focal_length_opt_mm = f_opt;
        f_eff = f_opt;
    } else if (f_nom > 0.5f && (meta.focal_length_mm <= 0.5f ||
                                (f_nom >= 0.5f * meta.focal_length_mm && f_nom <= 1.5f * meta.focal_length_mm))) {
        f_eff = f_nom;
    }

    if (focus_pos == 0u) return;
    meta.focus_position = focus_pos;
    if (is_log_pos2) {
        if (focus_pos >= 255u) {
            set_focus_infinity(meta);
        } else if (f_eff > 0.5f) {
            double d = (std::pow(2.0, static_cast<double>(focus_pos) / 16.0 - 5.0) + 1.0) *
                       (static_cast<double>(f_eff) / 1000.0);
            set_focus_distance_with_rel_sigma(meta, d, 0.12);
        }
    } else {
        if (focus_pos >= 128u) {
            set_focus_infinity(meta);
        } else if (f_eff > 0.5f) {
            double d = static_cast<double>(focus_pos) * static_cast<double>(f_eff) / 1000.0;
            double rel_sig = std::max(0.18, 1.5 / static_cast<double>(focus_pos));
            set_focus_distance_with_rel_sigma(meta, d, rel_sig);
        }
    }
}

inline bool parse_nikon_serial_key(const std::string& serial_str, const std::string& model, uint8_t& out_key) {
    if (!serial_str.empty()) {
        bool all_digits = true;
        uint32_t acc = 0u;
        for (char ch : serial_str) {
            if (ch >= '0' && ch <= '9') {
                acc = (acc * 10u + static_cast<uint32_t>(ch - '0')) & 0xFFu;
            } else {
                all_digits = false;
                break;
            }
        }
        if (all_digits) {
            out_key = static_cast<uint8_t>(acc);
            return true;
        }
    }
    if (ends_with_ci_ascii(model, "D50")) {
        out_key = 0x22u;
        return true;
    }
    if (!model.empty()) {
        out_key = 0x60u;
        return true;
    }
    return false;
}

inline bool decrypt_nikon_lens_data(
    const uint8_t* src,
    size_t len,
    const std::string& serial_str,
    uint32_t shutter_count,
    bool has_shutter_count,
    const std::string& model,
    std::vector<uint8_t>& dec_out) {
    if (!src || len < 8u) return false;
    static constexpr uint8_t kXlat0[256] = {
        0xc1,0xbf,0x6d,0x0d,0x59,0xc5,0x13,0x9d,0x83,0x61,0x6b,0x4f,0xc7,0x7f,0x3d,0x3d,
        0x53,0x59,0xe3,0xc7,0xe9,0x2f,0x95,0xa7,0x95,0x1f,0xdf,0x7f,0x2b,0x29,0xc7,0x0d,
        0xdf,0x07,0xef,0x71,0x89,0x3d,0x13,0x3d,0x3b,0x13,0xfb,0x0d,0x89,0xc1,0x65,0x1f,
        0xb3,0x0d,0x6b,0x29,0xe3,0xfb,0xef,0xa3,0x6b,0x47,0x7f,0x95,0x35,0xa7,0x47,0x4f,
        0xc7,0xf1,0x59,0x95,0x35,0x11,0x29,0x61,0xf1,0x3d,0xb3,0x2b,0x0d,0x43,0x89,0xc1,
        0x9d,0x9d,0x89,0x65,0xf1,0xe9,0xdf,0xbf,0x3d,0x7f,0x53,0x97,0xe5,0xe9,0x95,0x17,
        0x1d,0x3d,0x8b,0xfb,0xc7,0xe3,0x67,0xa7,0x07,0xf1,0x71,0xa7,0x53,0xb5,0x29,0x89,
        0xe5,0x2b,0xa7,0x17,0x29,0xe9,0x4f,0xc5,0x65,0x6d,0x6b,0xef,0x0d,0x89,0x49,0x2f,
        0xb3,0x43,0x53,0x65,0x1d,0x49,0xa3,0x13,0x89,0x59,0xef,0x6b,0xef,0x65,0x1d,0x0b,
        0x59,0x13,0xe3,0x4f,0x9d,0xb3,0x29,0x43,0x2b,0x07,0x1d,0x95,0x59,0x59,0x47,0xfb,
        0xe5,0xe9,0x61,0x47,0x2f,0x35,0x7f,0x17,0x7f,0xef,0x7f,0x95,0x95,0x71,0xd3,0xa3,
        0x0b,0x71,0xa3,0xad,0x0b,0x3b,0xb5,0xfb,0xa3,0xbf,0x4f,0x83,0x1d,0xad,0xe9,0x2f,
        0x71,0x65,0xa3,0xe5,0x07,0x35,0x3d,0x0d,0xb5,0xe9,0xe5,0x47,0x3b,0x9d,0xef,0x35,
        0xa3,0xbf,0xb3,0xdf,0x53,0xd3,0x97,0x53,0x49,0x71,0x07,0x35,0x61,0x71,0x2f,0x43,
        0x2f,0x11,0xdf,0x17,0x97,0xfb,0x95,0x3b,0x7f,0x6b,0xd3,0x25,0xbf,0xad,0xc7,0xc5,
        0xc5,0xb5,0x8b,0xef,0x2f,0xd3,0x07,0x6b,0x25,0x49,0x95,0x25,0x49,0x6d,0x71,0xc7
    };
    static constexpr uint8_t kXlat1[256] = {
        0xa7,0xbc,0xc9,0xad,0x91,0xdf,0x85,0xe5,0xd4,0x78,0xd5,0x17,0x46,0x7c,0x29,0x4c,
        0x4d,0x03,0xe9,0x25,0x68,0x11,0x86,0xb3,0xbd,0xf7,0x6f,0x61,0x22,0xa2,0x26,0x34,
        0x2a,0xbe,0x1e,0x46,0x14,0x68,0x9d,0x44,0x18,0xc2,0x40,0xf4,0x7e,0x5f,0x1b,0xad,
        0x0b,0x94,0xb6,0x67,0xb4,0x0b,0xe1,0xea,0x95,0x9c,0x66,0xdc,0xe7,0x5d,0x6c,0x05,
        0xda,0xd5,0xdf,0x7a,0xef,0xf6,0xdb,0x1f,0x82,0x4c,0xc0,0x68,0x47,0xa1,0xbd,0xee,
        0x39,0x50,0x56,0x4a,0xdd,0xdf,0xa5,0xf8,0xc6,0xda,0xca,0x90,0xca,0x01,0x42,0x9d,
        0x8b,0x0c,0x73,0x43,0x75,0x05,0x94,0xde,0x24,0xb3,0x80,0x34,0xe5,0x2c,0xdc,0x9b,
        0x3f,0xca,0x33,0x45,0xd0,0xdb,0x5f,0xf5,0x52,0xc3,0x21,0xda,0xe2,0x22,0x72,0x6b,
        0x3e,0xd0,0x5b,0xa8,0x87,0x8c,0x06,0x5d,0x0f,0xdd,0x09,0x19,0x93,0xd0,0xb9,0xfc,
        0x8b,0x0f,0x84,0x60,0x33,0x1c,0x9b,0x45,0xf1,0xf0,0xa3,0x94,0x3a,0x12,0x77,0x33,
        0x4d,0x44,0x78,0x28,0x3c,0x9e,0xfd,0x65,0x57,0x16,0x94,0x6b,0xfb,0x59,0xd0,0xc8,
        0x22,0x36,0xdb,0xd2,0x63,0x98,0x43,0xa1,0x04,0x87,0x86,0xf7,0xa6,0x26,0xbb,0xd6,
        0x59,0x4d,0xbf,0x6a,0x2e,0xaa,0x2b,0xef,0xe6,0x78,0xb6,0x4e,0xe0,0x2f,0xdc,0x7c,
        0xbe,0x57,0x19,0x32,0x7e,0x2a,0xd0,0xb8,0xba,0x29,0x00,0x3c,0x52,0x7d,0xa8,0x49,
        0x3b,0x2d,0xeb,0x25,0x49,0xfa,0xa3,0xaa,0x39,0xa7,0xc5,0xa7,0x50,0x11,0x36,0xfb,
        0xc6,0x67,0x4a,0xf5,0xa5,0x12,0x65,0x7e,0xb0,0xdf,0xaf,0x4e,0xb3,0x61,0x7f,0x2f
    };

    bool need_decrypt = (src[0] == '0' && (src[1] == '2' || src[1] == '4' || src[1] == '8'));
    dec_out.assign(src, src + len);
    if (!need_decrypt) {
        return (src[0] == '0' && src[1] == '1');
    }
    uint8_t s_key = 0;
    if (!has_shutter_count || !parse_nikon_serial_key(serial_str, model, s_key)) {
        return false;
    }
    uint8_t c_key = static_cast<uint8_t>(
        ((shutter_count >> 24) ^ (shutter_count >> 16) ^ (shutter_count >> 8) ^ shutter_count) & 0xFFu);
    uint8_t ci = kXlat0[s_key];
    uint8_t cj = kXlat1[c_key];
    uint8_t ck = 0x60u;
    for (size_t i = 4u; i < dec_out.size(); ++i) {
        cj = static_cast<uint8_t>((static_cast<uint32_t>(cj) + static_cast<uint32_t>(ci) * static_cast<uint32_t>(ck)) & 0xFFu);
        ck = static_cast<uint8_t>((static_cast<uint32_t>(ck) + 1u) & 0xFFu);
        dec_out[i] ^= cj;
    }
    return true;
}

inline void decode_nikon_lens_data(
    const uint8_t* raw_ld,
    size_t len,
    const std::string& serial_str,
    uint32_t shutter_count,
    bool has_shutter_count,
    const std::string& model,
    NevFrameMeta& meta) {
    std::vector<uint8_t> dec;
    if (!decrypt_nikon_lens_data(raw_ld, len, serial_str, shutter_count, has_shutter_count, model, dec)) {
        return;
    }
    if (dec.size() < 12u) return;

    const char v0 = static_cast<char>(dec[0]);
    const char v1 = static_cast<char>(dec[1]);
    const char v2 = static_cast<char>(dec[2]);
    const char v3 = static_cast<char>(dec[3]);

    if (v0 == '0' && v1 == '8' && v2 == '0' && (v3 == '0' || v3 == '1' || v3 == '2') && dec.size() >= 0x60u) {
        bool has_new = false;
        for (size_t i = 0x30u; i <= 0x3Eu; ++i) {
            if (dec[i] != 0u) {
                has_new = true;
                break;
            }
        }
        if (has_new && dec[0x35] == 1u) {
            uint16_t u16_fl = rd_le16(dec.data() + 0x3C);
            if (meta.focal_length_mm <= 0.0f && u16_fl > 0u && u16_fl < 3000u) {
                meta.focal_length_mm = static_cast<float>(u16_fl);
            }
            uint8_t range_w = dec[0x4C];
            uint16_t u16_fd = rd_le16(dec.data() + 0x4E);
            uint8_t drive_end = dec[0x56];
            uint8_t steps_from_inf = dec[0x58];
            int32_t lpa = static_cast<int32_t>(rd_le32(dec.data() + 0x5A));
            meta.lens_position_abs = lpa;
            meta.has_lens_position_abs = true;
            meta.focus_position = u16_fd;
            if (steps_from_inf == 0u && (lpa <= 0 || drive_end == 2u)) {
                set_focus_infinity(meta);
            } else if (u16_fd > 0u) {
                double d = std::pow(2.0, (static_cast<double>(u16_fd) / 256.0 - 80.0) / 12.0);
                double rel_sig = std::max(0.05, std::min(0.35, 0.04 + 0.008 * static_cast<double>(range_w)));
                set_focus_distance_with_rel_sigma(meta, d, rel_sig);
            }
            return;
        }
        bool has_old = false;
        for (size_t i = 0x04u; i <= 0x12u; ++i) {
            if (dec[i] != 0u) {
                has_old = true;
                break;
            }
        }
        if (has_old) {
            uint8_t v = dec[0x0B];
            meta.focus_position = v;
            if (v == 255u) {
                set_focus_infinity(meta);
            } else if (v > 0u) {
                double d = 0.01 * std::pow(10.0, static_cast<double>(v) / 40.0);
                set_focus_distance_with_rel_sigma(meta, d, 0.30);
            }
        }
        return;
    }

    size_t legacy_off = 0u;
    if (v0 == '0' && v1 == '2' && v2 == '0' && v3 == '4' && dec.size() > 0x0Au) {
        legacy_off = 0x0Au;
    } else if (((v0 == '0' && v1 == '2' && v2 == '0' && (v3 >= '1' && v3 <= '3')) ||
                (v0 == '0' && v1 == '1' && v2 == '0' && v3 == '1')) &&
               dec.size() > 0x09u) {
        legacy_off = 0x09u;
    } else if (v0 == '0' && v1 == '1' && v2 == '0' && v3 == '0' && dec.size() > 0x08u) {
        legacy_off = 0x08u;
    }
    if (legacy_off > 0u) {
        uint8_t v = dec[legacy_off];
        meta.focus_position = v;
        if (v == 255u) {
            set_focus_infinity(meta);
        } else if (v > 0u) {
            double d = 0.01 * std::pow(10.0, static_cast<double>(v) / 40.0);
            set_focus_distance_with_rel_sigma(meta, d, 0.30);
        }
    }
}

} // namespace detail
} // namespace nijinwvdc

#endif // NIJINWVDC_FOCUS_META_INTERNAL_H
