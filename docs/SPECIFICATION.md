# Format & Codec Specification

This document specifies the **Nikon `.NEV` ISOBMFF/QuickTime container**, **Nikon `.NEF` (`HE` / `HE*`) TIFF SubIFD layout**, **TicoRAW R0 (ISO/IEC 21122 Bayer extension) decoding pipeline**, **Sony `.ARW` (`ARW6` `Compressed HQ` / `Compressed`) multi-tile container and `"A000"` wavelet pipeline**, and **calibrated DNG 1.4 emission** implemented in `NijiN.WaveletRawDec` (`nev2bayer`).

> **Scope Note**: `NijiN.WaveletRawDec` targets specifically the two 2D-wavelet Bayer codec families lacking complete open-ecosystem support (**TicoRAW R0** in `.NEV`/`.NEF` `HE`/`HE*`/`.jxs` and **Sony ARW6 `"A000"`** in `.ARW` `Compression=32766`). Conventional uncompressed, packed-linear, and lossless-JPEG RAW variants return explicit format-classification errors so host pipelines can dispatch them to standard open decoders.

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

## 5. Sony `.ARW` (`ARW6`: `Compressed HQ` & `Compressed`) Container & `"A000"` Wavelet Architecture

### 5.1 TIFF Container Selection & Calibration Tags (`src/arw_container.cpp`)

When a TIFF input carries `Make` containing `"SONY"` or contains a Bayer `SubIFD` with `Compression (259) == 32766`, `parse_nev_clip` dispatches to `parse_sony_arw_tiff`:

1. **`SubIFDs` & Compression Selection**: Scans `IFD0`, `SubIFDs`, and `IFD1` for a CFA image directory (`PhotometricInterpretation == 32803` or `0`) matching `Compression (259) == 32766` and `SonyRawFileType (0x7000) == 6` (covering both **Compressed (HQ)** and **Compressed** wavelet modes across Full-Frame and APS-C geometries), verifying an embedded `"A000"` signature at the first tile's `rel_offset`. Legacy Sony ARW modes (such as `Compression=32767`) return an explicit unsupported-format error.
2. **Sony Sensor Calibration & Crop Tags**:
   - `BlackLevel`: Reads Sony tag `0x7310` or DNG `50714` (`uint16[4]`), defaulting to `[512, 512, 512, 512]` when absent.
   - `WhiteLevel (50717)`: Captured alongside `CFAPattern (33422)`.
   - `WB_RGGBLevels (0x7313)`: Signed 16-bit `[R, G0, G1, B]` multipliers.
   - `DefaultCrop`: Reads `DefaultCropOrigin` (`50719` / `0x74c7`) and `DefaultCropSize` (`50720` / `0x74c8`).
   - `ExifIFD` & Preview JPEG: Extracts exposure, aperture, ISO, focal length, and `LensModel`, while recording the largest `Compression == 6` SubIFD (or `IFD1` `513`/`514`) JPEG as `ncvw` / `ncth`.

### 5.2 Multi-Tile Strip Header & `"A000"` Bitstream Grammar (`src/arw6_dec.cpp`)

The TIFF strip begins with a little-endian tile directory of at least `0x68` bytes supporting `1`-tile (e.g., `7040x4688`) and `4`-tile quadrant layouts:

```text
+0x00  [u64 LE]  tile_count (1 or 4)
+0x08  [N x 24B] tile[i]:
                   ├─ +0x00 [u64 LE] rel_offset (from strip start)
                   ├─ +0x08 [u32 LE] tile_x     (2x2 CFA aligned)
                   ├─ +0x0C [u32 LE] tile_y     (2x2 CFA aligned)
                   ├─ +0x10 [u32 LE] width      (even, >= 2)
                   └─ +0x14 [u32 LE] height     (even, >= 2)
```

Each tile points to a big-endian `"A000"` wavelet codestream:

```text
+0x00  [4 B]   "A000" magic
+0x08  [2 B]   width (BE u16),  +0x0A [2 B] half_height = height / 2 (BE u16)
+0x0C  [2 B]   bit-depth word (coded_bd = (word >> 4) & 0x3F)
+0x0E  [2 B]   num_comps (=3), decomp_levels (=3), subsampling_mask (=0x0200)
+0x10  [32 B]  4 x 24-bit BE resolution-group byte sizes (in 16-byte blocks)
+0x30  [var]   5 group records (16 B or 32 B each): per-component stream lengths for
               resolutions 0..3 of (G_L, R, B) in groups 0..3 and raw G_H in group 4
[res streams]  For each resolution stream:
                 └─ 16B directory header (dir_blocks, payload_blocks, magic=4, n_bands, chunk_count <= 8192)
                 └─ chunk descriptors (u16 clen + n_bands x 4-bit band shifts) + aligned chunk payloads
```

### 5.3 ARW6 Multi-Threaded Decoding Pipeline

```text
["A000" Tile Codestream]
   │
   ├─► Chunk-Parallel Entropy Decode [Worker Pool across all (comp, res, chunk) slices]
   │     ├─ MSB-first WordBitReader parsing predictive Golomb-Rice parameter k (0..19).
   │     ├─ k == 0 triggers adaptive zero-quad run-length skip (rem >= 2).
   │     ├─ Predictor update_k runs BEFORE reading sign bits of the current 4-coeff quad.
   │     ├─ Midpoint dequantization: m != 0, shift > 0 -> (((m << 1) | 1) << (shift - 1)) - (m & 1).
   │     └─ LL0 horizontal DPCM with 15-bit signed modular wrap: (acc + v) & 0x7FFF -> [-16384, +16383].
   │
   ├─► Component-Parallel 2D LeGall 5/3 IDWT [(G_L, R, B) across r = 1..3; G_H bypassed at r = 0]
   │     ├─ Pass 1 (Vertical FIRST): 1D 5/3 inverse lifting over [LL|HL] even rows and [LH|HH] odd rows.
   │     └─ Pass 2 (Horizontal SECOND): 1D 5/3 inverse lifting across columns into next_ll.
   │        (Integer floor rounding in 5/3 lifting makes Vert->Horiz non-commutative with Horiz->Vert.)
   │
   ├─► Pass 3 [Parallel Rows]: Quincunx Diamond Even-Diagonal G0 Synthesis
   │     └─ Reconstructs G0[r][c] from G_L[r][c] minus rounded average of four G_H neighbors
   │        (3-neighbor boundary handling at the rightmost column w_comp - 1).
   │
   └─► Pass 4 [Parallel Rows]: Odd-Diagonal G1 + (R, B) + 4096-Entry NLT LUT
         ├─ G1[r][c] = G_H[r][c] + rounded diamond average of G0 neighbors.
         ├─ Clamps ug0, ug1 into [0, 4095] around bias 2048; computes g_avg = (ug0 + ug1) >> 1.
         ├─ ur = clamp(g_avg + 2 * R, 0, 4095), ub = clamp(g_avg + 2 * B, 0, 4095).
         └─ Maps via kArw6NltLut[4096] >> 1 (clamped <= 0x4000) to 14-bit linear RGGB output.
```

---

## 6. DNG 1.4 Calibration & CFA Geometry

- **Active Crop & CFA Modulo Rotation (`apply_bayer_crop`)**: When trimming margins to Nikon MakerNote `CropArea` or Sony `DefaultCrop = [x0, y0, w, h]`, shifts the `2x2` Bayer pattern according to `dst_cfa[ry*2 + rx] = src_cfa[((ry ^ (y0 & 1)) * 2) + (rx ^ (x0 & 1))]`; otherwise emits the full decoded buffer with `DefaultCropOrigin (50719)` and `DefaultCropSize (50720)`.
- **Scalar `BlackLevel (50714)` Convention**: When all 4 CFA channels share one black level (e.g., Nikon `1008`, Sony `512`), omits `BlackLevelRepeatDim (50713)` and writes scalar `BlackLevel (50714) RATIONAL[1]` alongside `WhiteLevel (50717) = 16383`, ensuring uniform black-level attribution across `LibRaw` and standard DNG readers. Distinct per-channel black levels emit `BlackLevelRepeatDim = [2, 2]` and 4 rationals.
- **Built-In D65 `ColorMatrix1 (50721)`**: Embeds CIE XYZ(D65)-to-camera matrices for Nikon (`Z 8`, `Z 9`, `Z6 III`, `ZR`, `Z f`, `Z50 II`, `Z5 II`) and Sony Alpha (`ILCE-7M5`, `ILCE-7RM6`, `ILCE-7RM5`, `ILCE-7M4`, `ILCE-7CR`, `ILCE-7CM2`, `ILCE-1M2`, `ILCE-1`, `ILCE-9M3`, `ZV-E1`, `ILME-FX3`), falling back cleanly per make for unlisted models.
