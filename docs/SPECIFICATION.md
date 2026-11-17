# Format & Codec Specification

This document specifies the **Nikon `.NEV` ISOBMFF/QuickTime container**, **Nikon `.NEF` (`HE` / `HE*`) TIFF SubIFD layout**, **TicoRAW R0 (ISO/IEC 21122 Bayer extension) decoding pipeline**, and **calibrated DNG 1.4 emission** implemented in `NijiN.EntropyRawDec` (`nev2bayer`).

---

## 1. Nikon `.NEV` Container & Per-Frame `NRAW` Box Hierarchy

### 1.1 Top-Level ISOBMFF / QuickTime Layout

| Box | Typical Layout | Purpose |
|---|---|---|
| `ftyp` | `major_brand = "niko"`, `minor_version = 1` | Nikon N-RAW container brand header |
| `moov` | Contains `mvhd`, three `trak` boxes, and `udta/NCDT` | Movie timescale, chunk/sample tables, and camera static metadata |
| `free` | Padding aligned to a `512 KiB` (`0x80000`) boundary | Aligns `mdat` start to flash storage block boundaries |
| `mdat` | Interleaved `NRAW`, LPCM audio, and timecode payloads | Stores raw compressed video frames plus synced audio/timecode |

### 1.2 Media Tracks & `moov/udta/NCDT` Metadata

- **Video Track (`vide` / `stsd` FourCC = `NRAW`)**: Uses `stts` for frame deltas, `stsz` for per-sample outer `NRAW` box byte counts (supporting both fixed and variable frame sizes), and `co64` / `stco` + `stsc` for `O(1)` 64-bit random frame access (`fseeko`, supporting clips beyond `4 GiB`).
- **Audio Track (`soun` / QuickTime Sound Sample Description v2 `lpcm`)**: Parses the 72-byte v2 sample entry (`float64` big-endian `sampleRate`, e.g., `48000.0 Hz`, `numAudioChannels`, `bitsPerChannel`, `bytesPerPacket`), with fallback support for v0/v1 sound sample descriptions.
- **Timecode Track (`tmcd`)**: Extracts QuickTime Timecode flags, timescale, frame duration, and `number_of_frames` from sample entry bytes `+20..32`.
- **`udta/NCDT` Global Block**:
  - `NCTG`: Sequential big-endian tag stream (`tag(u32 BE) + dtype(u16 BE) + count(u16 BE) + payload`), carrying Make/Model/Firmware strings, frame rate rational, active frame size, `0x0110*` exposure/lens tags, and global Picture Control tag `0x02000023`.
  - `NCTH` / `NCVW` / `NCM1`: Embedded JPEG blobs (thumbnail, camera-rendered preview such as `1920x1080` Rec.2020 N-Log JPEG, and reference preview).

### 1.3 Per-Frame `NRAW` Sample Structure

Each video sample inside `mdat` is a self-contained `NRAW` box:

```text
+0      [8 B]   NRAW Box Header (size = sample_size, type = "NRAW")
+8      [12 B]  NRFM: ASCII version identifier (e.g. "0100")
+20     [var]   NRFH: [4 B inner][NRHM][series of NRMT tags]
                  ├─ group 0x0110: coded dimensions (0x0100/0x0101), bit depth (0x0102),
                  │                orientation (0x0112), exposure/f-number/ISO/focal length,
                  │                CFAPattern (0xa302)
                  ├─ group 0x0190: per-frame stabilization / IMU motion vectors
                  └─ group 0x0200 (tag 0x0001): sequential little-endian TIFF MakerNote
                       ├─ 0x0005 WB mode ASCII
                       ├─ 0x000c WB_RBLevels (R, B, G0, G1 rationals)
                       ├─ 0x0023 PictureControl base / name
                       ├─ 0x003d BlackLevel uint16[4]
                       ├─ 0x0045 CropArea [left, top, width, height]
                       ├─ 0x004f ColorTemp (Kelvin)
                       └─ 0x0093 NEFCompression (=15 for Nikon N-RAW)
+header [var]   NRTH:
                  ├─ NRTI (16 B): version + u32 BE codestream_len
                  └─ NRTA (var): zero-filled alignment padding to +1024 B boundary
+1024   [Lcod]  Bare TicoRAW R0 codestream: SOC(0xFF10) + CAP + PIH + CDT + WGT + Slices + EOC(0xFF11)
```

`read_nev_frame_meta_and_codestream` dynamically walks the `NRFM` / `NRFH` / `NRTH` sub-boxes and reads `codestream_len` from `NRTH/NRTI`, remaining independent of fixed sample or padding sizes.

---

## 2. Nikon `.NEF` (`HE` / `HE*`) TIFF Layout & `"Nikon\0"` MakerNote

When the input starts with a standard TIFF header (`II*\0` or `MM\0*`), `parse_nev_clip` dispatches to `parse_nef_he_clip`:

1. **`IFD0`**: Extracts `Make (271)`, `Model (272)`, `Orientation (274)`, `Software (305)`, `DateTime (306)`, `SubIFDs (330)`, `ExifIFD (34665)`, and optional `IFD0` thumbnail JPEG offsets (`513` / `514`).
2. **`SubIFDs` Selection**: Scans every SubIFD to locate the Bayer CFA entry with `Compression (259) == 34713`, `PhotometricInterpretation (262) == 32803`, and strip payload starting with TicoRAW `SOC (0xFF10)` (recording the largest `Compression == 6` JPEG SubIFD as preview `ncvw`). Legacy `34713` files lacking the `0xFF10` signature return an explicit format-classification error.
3. **Exif & Embedded `"Nikon\0"` MakerNote**: Reads exposure parameters, ISO, focal length, and `LensModel (0xA434)` from `ExifIFD`; when `MakerNote (0x927C)` carries the `"Nikon\0"` header with an embedded TIFF IFD at offset `+10`, `apply_makernote_tag` extracts `0x0005`, `0x000c`, `0x0023`, `0x003d`, `0x0045`, `0x004f`, and `0x0093` relative to `makernote_offset + 10`.

---

## 3. N-Log Preview Metadata vs. 14-Bit Linear Sensor Signal

In `.NEV` clips recorded with in-camera N-Log, `NCTG` typically reports nominal bit depth `0x0025 = 12` and Picture Control `0x02000023 = N-LOG`:

- Those tags describe the embedded `NCVW` (`1920x1080` N-Log Rec.2020) preview stream and downstream NLE grading LUT selection.
- The embedded TicoRAW R0 video frames are purely intra-coded (`interframe = 0`) with `in_bitdepth = 14`, `out_bitdepth = 16`, `CDT depth = 14`, and Fliproot non-linear point parameters (`gamma_type = 1`, `offset_16 = 4032`, `offset_mode = 1`, `r_val = 84212`, `r_flip = 84429`, `offset_sqrt = 16255`) matching still `.NEF` `HE` / `HE*` files.
- Consequently, decoded pixels represent **14-bit physical linear sensor ADU values** (`BlackLevel = 1008`, `WhiteLevel = 16383`) ready for linear DNG color pipelines.

---

## 4. TicoRAW R0 Codestream Taxonomy & Decoding Architecture

### 4.1 Compression Class & Rate Adaptation (`Normal` / `High` / `HE` / `HE*`)

Nikon N-RAW (`.NEV`) and HE / HE\* (`.NEF`) share the **wavelet-subband + block-floating GCLI entropy coding** family (intoPIX TicoRAW, Bayer extension of ISO/IEC 21122 JPEG XS):

1. **Sensor-Domain Pre-Processing**: Linear RGGB samples undergo **Fliproot square-root-like perceptual NLT** to match photon shot-noise scaling across shadows and highlights, followed by two-step **Star-Tetrix reversible Bayer color/phase decorrelation (RCT)** producing four half-resolution planes `(Yl, Yh, Cr, Cb)` (`w_pix = W / 2`, `h_pix = ceil_even((H + H_pad) / 2)`).
2. **2D LeGall 5/3 Lifting Wavelet (`5H + 1V`)**: `Yl`, `Cr`, and `Cb` undergo 5 horizontal and 1 vertical integer 5/3 lifting levels while `Yh` bypasses wavelet filtering (`0` levels), forming 25 weighted subbands across 26 segments (`SegmentDesc`).
3. **Per-Precinct Bitrate Tiers**: Each precinct encodes 2 lines of `(Yl, Cr, Cb)` with adaptive quantization `Qp` and refinement threshold `Rp` (`gtli = clamp(Qp - gain - [prio < Rp], 0, 15)`). Thus `N-RAW Normal (~1.5 bpp)`, `N-RAW High (~2.3–2.5 bpp)`, `HE (~3.0 bpp)`, and `HE* (~5.0 bpp)` use one identical codestream grammar and decoder kernel, differing only in bit budget and per-precinct `(Qp, Rp)` operating points.

### 4.2 Four-Stage Parallel Decoding Pipeline (`src/ticoraw_dec.cpp`)

```text
[TicoRAW R0 Codestream]
   │
   ├─► parse_r0_header & CodecGeometry::init
   │     └─ Parses SOC(0xFF10), CAP(0xFF50), PIH (39B + 13B Star-Tetrix/Fliproot extension),
   │        CDT(16B), WGT(54B); builds 26 segments, 25 subbands, and slice/precinct offsets.
   │
   ├─► Stage 1 [Slice-Parallel Threads]: decode_slice_entropy_and_hdwt
   │     ├─ Parses precinct headers (Qp, Rp, bsig/bvpred modes, subband pkt_len).
   │     ├─ Decodes 4-coefficient GCLI groups: raw or bounded vertical prediction (u <= 2 * bw).
   │     ├─ Deadzone-free mid-tread uniform dequantization with exact (data_B, gcli_B, sign_B) checks.
   │     └─ 5-level / 1-level integer 5/3 lifting horizontal IDWT via portable asr_s32.
   │
   ├─► Stage 1.5 [Full-Frame Parallel Rows]: Global 1-Level Vertical 5/3 IDWT
   │     ├─ Pass A: computes all even rows even[2k] = L[k] - ((H[k-1] + H[k] + 2) >> 2).
   │     └─ Pass B: interpolates all odd rows odd[2k+1] = H[k] + ((even[2k] + even[2k+2]) >> 1).
   │        Handles arbitrary even heights (including H % 64 == 0 / 4) with zero overlap re-decoding.
   │
   ├─► Stage 2 [Parallel Rows]: Inverse Star-Tetrix Step 1 -> reconstructs (G0, G1).
   │
   └─► Stage 3 [Parallel Rows]: Inverse Star-Tetrix Step 2 (R, B) + build_inverse_nlt_lut -> 14-bit RGGB.
```

### 4.3 Boundary & Memory-Safety Invariants

- **Bounded Vertical-Prediction Unary Bound (`u <= 2 * bw`)**: Allows valid sharp drops in GCLI across bright highlights or flat noise floors up to the full `2 * bw` theoretical differential range while guarding remaining packet bits.
- **Trailing-Tile Vertical Symmetry**: Performing global vertical 5/3 lifting across full `(Yl, Cr, Cb)` planes preserves exact top/bottom reflection parity when `H % 64 == 0` (e.g., `512x512`) or `H % 64 == 4` (e.g., `512x516`), matching reference output pixel-for-pixel.
- **Bounds-Safe Bit Window (`peek64z`)**: Assembles remaining bytes near packet end without out-of-bounds reads under AddressSanitizer / UBSan.

---

## 5. DNG 1.4 Calibration & CFA Geometry

- **Active Crop & CFA Modulo Rotation (`apply_bayer_crop`)**: When trimming margins to MakerNote `CropArea = [x0, y0, w, h]`, shifts the `2x2` Bayer pattern according to `dst_cfa[ry*2 + rx] = src_cfa[((ry ^ (y0 & 1)) * 2) + (rx ^ (x0 & 1))]`; otherwise emits the full decoded buffer with `DefaultCropOrigin (50719)` and `DefaultCropSize (50720)`.
- **Scalar `BlackLevel (50714)` Convention**: When all 4 CFA channels share one black level (e.g., `1008`), omits `BlackLevelRepeatDim (50713)` and writes scalar `BlackLevel (50714) RATIONAL[1]` alongside `WhiteLevel (50717) = (1 << bit_depth) - 1 = 16383`, ensuring uniform black-level attribution across `LibRaw` and standard DNG readers. Distinct per-channel black levels emit `BlackLevelRepeatDim = [2, 2]` and 4 rationals.
- **Built-In D65 `ColorMatrix1 (50721)`**: Embeds CIE XYZ(D65)-to-camera matrices for Nikon Z 8, Z 9, Z6 III, ZR, Z f, Z50 II, and Z5 II (falling back to Z 8 for unlisted models).
