# NijiN.WaveletRawDec

**Clean-Room Multi-Threaded C++17 Decoder for Wavelet-Compressed Camera & Cinema RAW Formats**  
(`Apache-2.0` · `C++17` · `POSIX Threads` · `Zero External Dependencies`)

`NijiN.WaveletRawDec` (library `nijinWVDC_core` / CLI `nijinWVDC`) is purpose-built for the **two modern 2D-wavelet camera RAW codec families** whose open-source ecosystem support has historically been absent or incomplete:

1. **TicoRAW R0 (`5H+1V` LeGall 5/3 lifting + Star-Tetrix RCT + Fliproot NLT)** — Nikon N-RAW video (`.NEV`), Nikon High Efficiency stills (`.NEF` `HE` / `HE*`), and bare `.jxs` codestreams.
2. **Sony ARW 6.0 `"A000"` (`3`-level 5/3 lifting + Quincunx diamond green synthesis + 12-to-15-bit NLT)** — Sony `Compressed (HQ)` and `Compressed` stills (`.ARW`, `Compression=32766`, `SonyRawFileType=6`).

Conventional uncompressed, packed-linear, and lossless-JPEG RAW modes (such as legacy Nikon `.NEF` variants, or Sony `Compression=1`, `7`, and `32767`) are maturely handled by standard open-source decoders and intentionally return explicit format-classification errors here so downstream pipelines can route them cleanly. For both wavelet-compressed families, `NijiN.WaveletRawDec` outputs **physical 14-bit linear RGGB Bayer buffers (`.raw`)** and **calibrated uncompressed CFA DNG 1.4 negatives (`.dng`)**.

> **Documentation**: [**`docs/SPECIFICATION.md`** (Containers, Entropy/Wavelet Pipeline & DNG Calibration)](docs/SPECIFICATION.md) · [**`docs/CLI.md`** (CLI Reference & C++17 API)](docs/CLI.md) · [**`LICENSE`**](LICENSE) · [**`NOTICE`**](NOTICE)

---

## Format Coverage & Roadmap

### Currently Shipped (`nijinWVDC_core` / `nijinWVDC`)

- **Nikon N-RAW (`.NEV`)**:
  - 64-bit QuickTime / ISOBMFF container parser (`ftyp=niko`, `co64` / `stsz` / `stsc`), QuickTime Sound v2 `lpcm` audio and `tmcd` timecode inspection, `moov/udta/NCDT/NCTG` metadata extraction, embedded `NCVW` / `NCTH` JPEG preview extraction, and per-sample `NRAW -> NRFM + NRFH(NRMT) + NRTH(NRTI/NRTA)` framing.
  - Decodes Normal (`~1.5 bpp`) and High (`~2.3–2.5 bpp`) bitrates across arbitrary even sensor geometries (`8.3K`, `4.1K`, etc.).
- **Nikon High Efficiency RAW (`.NEF` HE / HE\*)**:
  - Endian-aware TIFF `IFD0`, `SubIFDs` (`Compression=34713`, `PhotometricInterpretation=32803` with `0xFF10` TicoRAW `SOC`), Exif IFD, and `"Nikon\0"` embedded MakerNote parser (White Balance `0x000c`, Black Level `0x003d`, Active Crop `0x0045`, Color Temperature `0x004f`, Picture Control `0x0023`).
- **Sony ARW6 — `Compressed (HQ)` & `Compressed` (`.ARW`)**:
  - Endian-aware TIFF `IFD0`, `IFD1`, and `SubIFDs` parser targeting `Compression=32766` (`PhotometricInterpretation=32803`, `SonyRawFileType (0x7000) = 6`), covering both **Compressed (HQ)** and standard **Compressed** wavelet modes across Full-Frame (`7040x4688`) and APS-C (`4640x3088`) geometries, extracting `0x7310` / `50714` `BlackLevel`, `0x7313` WB RGGB levels, `50717` `WhiteLevel`, `50719/50720` & `0x74c7/0x74c8` Active Crop, `ExifIFD` metadata, and embedded JPEG preview / thumbnail streams.
  - Supports `0x68`-byte `1`-tile and `4`-tile strip directories with chunk-parallel `"A000"` wavelet entropy decoding and multi-tile Bayer stitching.
- **Standalone TicoRAW R0 Codestreams (`.jxs`)**:
  - Header probing and multi-threaded direct decoding for bare ISO/IEC 21122 Bayer-extension codestreams.

### Modular Expansion Targets

Designed with clean separation between container demuxing, parallel entropy/wavelet decoding, and calibrated CFA DNG emission to accommodate additional wavelet-compressed cinema RAW families:

- **Sony X-OCN (`.MXF` Extended Tonal Range Original Camera Negative: X-OCN XT / ST / LT)** — planned

---

## Core Engineering Highlights

- **Exact Wavelet & Trailing-Tile Architecture**:
  - Executes slice-parallel entropy decoding + 1D horizontal IDWT, followed by a global two-pass 1-level vertical LeGall 5/3 lifting IDWT (even rows first, odd rows interpolated), two-step Star-Tetrix inverse RCT, and an 81,792-entry Fliproot inverse NLT lookup table.
  - Eliminates trailing-tile edge artifacts on heights divisible by `64` (`H % 64 == 0`) or congruent to `4 mod 64` (`H % 64 == 4`) without cross-slice overlap re-decoding.
  - Enforces bounded vertical-prediction GCLI unary upper limits (`u <= 2 * bw`), strict per-packet `(data_B, gcli_B, sign_B)` bit accounting, bounds-safe tail bit-window reads (`peek64z`), and portable integer arithmetic right shifts (`asr_s32`).
- **Sony ARW6 (`"A000"`) Wavelet & Quincunx Pipeline (`src/arw6_dec.cpp`)**:
  - Decodes `1`-tile and `4`-tile ARW6 strips using chunk-parallel predictive Golomb-Rice 4-coefficient quad entropy coding (`k=0` adaptive zero-run mode, `update_k` predictor tracking, midpoint dequantization) and 15-bit signed modular `LL0` horizontal DPCM (`[-16384, +16383]`).
  - Reconstructs `(G_L, R, B)` across 3 decomposition levels via **vertical-first, horizontal-second** integer LeGall 5/3 lifting IDWT (`G_H` bypassed at resolution `0`), followed by two-pass Quincunx diamond green synthesis, `(R, B)` residual reconstruction, and a 4096-entry 12-to-15-bit inverse non-linear tone mapping LUT (`kArw6NltLut`).
- **Physical 14-bit Linear Light & Out-of-the-Box DNG 1.4 Compatibility**:
  - Outputs linear sensor RGGB code values (e.g., Nikon `BlackLevel = 1008`, Sony `BlackLevel = 512`, `WhiteLevel = 16383`) cleanly separated from `.NEV` N-Log preview metadata (`NCTG 0x02000023 = N-LOG`, nominal `0x0025 = 12`).
  - `--active-crop` trims sensor margins to camera `CropArea` / `DefaultCrop` and recomputes the `2x2 CFAPattern` modulo phase (`(x0 & 1, y0 & 1)`).
  - Emits scalar `BlackLevel (50714) RATIONAL[1]` (omitting `BlackLevelRepeatDim`) when all four CFA channels share the same pedestal, ensuring consistent black-level parsing across `LibRaw`, `dcraw_emu`, `macOS sips`, and standard DNG 1.4 tools. Built-in D65 `ColorMatrix1` profiles cover Nikon (`Z 8`, `Z 9`, `Z6 III`, `ZR`, `Z f`, `Z50 II`, `Z5 II`) and Sony Alpha (`ILCE-7M5`, `ILCE-7RM6`, `ILCE-7RM5`, `ILCE-7M4`, `ILCE-7CR`, `ILCE-7CM2`, `ILCE-1M2`, `ILCE-1`, `ILCE-9M3`, `ZV-E1`, `ILME-FX3`).

---

## Verified Accuracy & Performance

### 1. Pixel-Exact Parity vs. Reference Decoder Output

| Test Sample / Boundary Case | Geometry (`W x H`) / Mode | Max Abs Diff | Mismatched Pixels | Bottom-64 Rows Max Diff |
|---|---|---:|---:|---:|
| Nikon N-RAW `.NEV` clip (first / mid / last frames) | `45.6 MP` (`1.50 bpp` N-RAW, `1 / 8` threads) | **`0`** | **`0`** | **`0`** |
| Nikon Z 8 `.NEF` burst corpus (`132` frames) | `8280 x 5520` (`5.00 bpp` `HE*`, all `132` frames) | **`0`** | **`0`** | **`0`** |
| Nikon `Z 8` / `Z 9` / `Z6 III` / `Z5 II` / `Z50 II` multi-mode suite (`26` `.NEF` files) | `FX` / `DX` / `16:9` / `1:1` crops, `HE` (`~3 bpp`) & `HE*` (`~5 bpp`) | **`0`** | **`0`** | **`0`** |
| Sony `ILCE-7M5` `.ARW` suite (`6` camera files) | `ARW6` `Compressed (HQ)` & `Compressed`, `7040 x 4688` FF & `4640 x 3088` APS-C | **`0`** | **`0`** | **`0`** |
| Sony `.ARW` 4-tile quadrant stitch (`2` geometries) | `14080 x 9376` (`132.0 MP`) & `9280 x 6176` (`57.3 MP`), `8` threads | **`0`** | **`0`** | **`0`** |
| Trailing-tile edge case (`H % 64 == 0`) | `512 x 512` synthetic stream | **`0`** | **`0`** | **`0`** |
| Trailing-tile edge case (`H % 64 == 4`) | `512 x 516` synthetic stream | **`0`** | **`0`** | **`0`** |

Single-threaded and multi-threaded runs (`1 / 4 / 8` threads) produce bit-identical outputs (`b1 == b4`, `b1 == b8`).

### 2. Apple Silicon Multi-Threaded Benchmark (`45.6 MP` `.NEV` & `33.0 MP` `.ARW`, 20-frame average)

| Format / Resolution | Threads | Decode Only (`ms/frame`) | Decode Throughput (`fps`) | Read + Decode E2E (`ms/frame`) | E2E Throughput (`fps`) |
|---|---:|---:|---:|---:|---:|
| Nikon `.NEV` (`45.6 MP`, `1.50 bpp`) | `1` | `291.74` | `3.43` | `293.55` | `3.41` |
| Nikon `.NEV` (`45.6 MP`, `1.50 bpp`) | `4` | `81.30` | `12.30` | `82.56` | `12.11` |
| Nikon `.NEV` (`45.6 MP`, `1.50 bpp`) | **`8`** | **`49.67`** | **`20.13`** | **`50.95`** | **`19.63`** |
| Sony `.ARW` (`ARW6`, `7040 x 4688`) | `1` | `399.22` | `2.50` | `402.28` | `2.49` |
| Sony `.ARW` (`ARW6`, `7040 x 4688`) | `4` | `97.56` | `10.25` | `100.20` | `9.98` |
| Sony `.ARW` (`ARW6`, `7040 x 4688`) | **`8`** | **`72.38`** | **`13.82`** | **`74.65`** | **`13.40`** |

---

## Build & Quick Start

Requires only a `C++17` compiler (Clang or GCC) and POSIX threads (`-lpthread`), with zero external third-party libraries.

### Build & Test with Make (includes AddressSanitizer + UBSan)

```sh
make clean all test
```

Build outputs:
- `build/nijinWVDC` (`-O3` release CLI)
- `build/nijinWVDC_asan` (ASan + UBSan diagnostic CLI)
- `build/test_nijinWVDC` / `build/test_nijinWVDC_asan` (standalone unit suite; optional `NIJINWVDC_TEST_NEV`, `NIJINWVDC_TEST_NEF`, and `NIJINWVDC_TEST_ARW` environment variables enable real-sample regression checks)

### Build with CMake

```sh
cmake -S . -B cmake-build -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build -j
ctest --test-dir cmake-build --output-on-failure
```

### CLI Quick Start

```sh
# 1. Inspect .NEV / .NEF / .ARW / .jxs headers, metadata, audio/timecode, and codestream parameters
./build/nijinWVDC --info clip.NEV
./build/nijinWVDC --info --json photo.NEF
./build/nijinWVDC --info --json photo.ARW

# 2. Decode a frame to linear 14-bit-in-16-bit CFA DNG (full sensor buffer + DefaultCrop metadata)
./build/nijinWVDC clip.NEV --frame 0 -o frame_0000.dng --threads 8
./build/nijinWVDC photo.ARW -o sony_full.dng --threads 8

# 3. Crop to camera active crop with CFA phase rotation -> .dng or 16-bit LE .raw
./build/nijinWVDC clip.NEV --frame 0 --active-crop -o frame_0000_crop.dng --threads 8
./build/nijinWVDC photo.NEF --active-crop -o photo_crop.dng --threads 8
./build/nijinWVDC photo.ARW --active-crop -o sony_crop.dng --threads 8
./build/nijinWVDC clip.NEV --frame 0 -o frame_0000.raw --threads 8

# 4. Batch decode a frame range or entire clip into a directory
./build/nijinWVDC clip.NEV --frame 0-9 -o out_dng/ --active-crop --threads 8

# 5. Extract/decode bare .jxs codestreams, embedded JPEG previews, or run a throughput benchmark
./build/nijinWVDC --extract-jxs clip.NEV --frame 0 -o frame_0000.jxs
./build/nijinWVDC --raw-jxs frame_0000.jxs -o frame_0000.dng --threads 8
./build/nijinWVDC --extract-preview clip.NEV -o preview.jpg
./build/nijinWVDC --bench clip.NEV --frames 20 --threads 8
```

See [**`docs/CLI.md`**](docs/CLI.md) for all flags, JSON fields, and C++17 `nijinWVDC_core` integration examples.

---

## Repository Layout

| Path | Role |
|---|---|
| [`include/nijinWVDC.h`](include/nijinWVDC.h) | Public C++17 data structures and container, decode, crop, and DNG APIs |
| [`src/tiff_internal.h`](src/tiff_internal.h) | Shared endian-aware TIFF IFD reader and Sony ARW dispatch helper |
| [`src/nev_container.cpp`](src/nev_container.cpp) | 64-bit `.NEV` ISOBMFF/NCDT/NRFH/NRTH parser and `.NEF` HE/HE\* TIFF/MakerNote parser |
| [`src/arw_container.cpp`](src/arw_container.cpp) | Sony `.ARW` (`ARW6` `Compressed HQ` / `Compressed`) TIFF `SubIFDs`, calibration tags, and JPEG preview parser |
| [`src/ticoraw_dec.cpp`](src/ticoraw_dec.cpp) | Clean-room TicoRAW R0 parallel entropy decoder, 5/3 2D IDWT, Star-Tetrix RCT, and Fliproot NLT |
| [`src/arw6_dec.cpp`](src/arw6_dec.cpp) | Clean-room Sony ARW6 (`"A000"`) multi-tile parser, chunk-parallel entropy decoder, vertical-first 5/3 IDWT, Quincunx synthesis, and NLT LUT |
| [`src/dng_writer.cpp`](src/dng_writer.cpp) | Scalar `BlackLevel` DNG 1.4 writer, Nikon/Sony D65 `ColorMatrix1` lookup, and modulo CFA cropper |
| [`src/main.cpp`](src/main.cpp) | CLI driver (`--info` / `--json` / batch decode / stream & preview extraction / `--bench`) |
| [`tests/test_nijinWVDC.cpp`](tests/test_nijinWVDC.cpp) | Standalone negative container/stream checks, CFA phase rotation, DNG round-trip, and optional corpus hooks |
| [`CMakeLists.txt`](CMakeLists.txt) · [`Makefile`](Makefile) | Zero-dependency build configuration (`nijinWVDC_core` static library, `nijinWVDC`, and tests) |

---

## License & Provenance

All original source code is licensed under the [**Apache License, Version 2.0 (`Apache-2.0`)**](LICENSE). See [**`NOTICE`**](NOTICE) for copyright, interoperability, and patent scope notices.
