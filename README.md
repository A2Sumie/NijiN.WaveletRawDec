# NijiN.EntropyRawDec

**Clean-Room Multi-Threaded C++17 Decoder for Entropy-Compressed Camera & Cinema RAW Formats**  
(`Apache-2.0` · `C++17` · `POSIX Threads` · `Zero External Dependencies`)

`NijiN.EntropyRawDec` (library `nev2bayer_core` / CLI `nev2bayer`) provides an independent, dependency-free C++17 decoder for wavelet-entropy compressed Bayer RAW formats, converting container frames directly into **physical 14-bit linear RGGB Bayer buffers (`.raw`)** and **calibrated uncompressed CFA DNG 1.4 negatives (`.dng`)**.

> **Documentation**: [**`docs/SPECIFICATION.md`** (Containers, Entropy/Wavelet Pipeline & DNG Calibration)](docs/SPECIFICATION.md) · [**`docs/CLI.md`** (CLI Reference & C++17 API)](docs/CLI.md) · [**`LICENSE`**](LICENSE) · [**`NOTICE`**](NOTICE)

---

## Format Coverage & Roadmap

### Currently Shipped (`nev2bayer_core` / `nev2bayer`)

- **Nikon N-RAW (`.NEV`)**:
  - 64-bit QuickTime / ISOBMFF container parser (`ftyp=niko`, `co64` / `stsz` / `stsc`), QuickTime Sound v2 `lpcm` audio and `tmcd` timecode inspection, `moov/udta/NCDT/NCTG` metadata extraction, embedded `NCVW` / `NCTH` JPEG preview extraction, and per-sample `NRAW -> NRFM + NRFH(NRMT) + NRTH(NRTI/NRTA)` framing.
  - Decodes Normal (`~1.5 bpp`) and High (`~2.3–2.5 bpp`) bitrates across arbitrary even sensor geometries (`8.3K`, `4.1K`, etc.).
- **Nikon High Efficiency RAW (`.NEF` HE / HE\*)**:
  - Endian-aware TIFF `IFD0`, `SubIFDs` (`Compression=34713`, `PhotometricInterpretation=32803` with `0xFF10` TicoRAW `SOC`), Exif IFD, and `"Nikon\0"` embedded MakerNote parser (White Balance `0x000c`, Black Level `0x003d`, Active Crop `0x0045`, Color Temperature `0x004f`, Picture Control `0x0023`).
- **Standalone TicoRAW R0 Codestreams (`.jxs`)**:
  - Header probing and multi-threaded direct decoding for bare ISO/IEC 21122 Bayer-extension codestreams.

### Modular Expansion Targets

Designed with clean separation between container demuxing, parallel entropy/wavelet decoding, and calibrated CFA DNG emission to accommodate additional entropy-compressed RAW families:

- **Sony Compressed / Lossless HQ RAW (`.ARW`)** — planned
- **Sony X-OCN (`.MXF` Extended Tonal Range Original Camera Negative: X-OCN XT / ST / LT)** — planned

---

## Core Engineering Highlights

- **Exact Wavelet & Trailing-Tile Architecture**:
  - Executes slice-parallel entropy decoding + 1D horizontal IDWT, followed by a global two-pass 1-level vertical LeGall 5/3 lifting IDWT (even rows first, odd rows interpolated), two-step Star-Tetrix inverse RCT, and an 81,792-entry Fliproot inverse NLT lookup table.
  - Eliminates trailing-tile edge artifacts on heights divisible by `64` (`H % 64 == 0`) or congruent to `4 mod 64` (`H % 64 == 4`) without cross-slice overlap re-decoding.
  - Enforces bounded vertical-prediction GCLI unary upper limits (`u <= 2 * bw`), strict per-packet `(data_B, gcli_B, sign_B)` bit accounting, bounds-safe tail bit-window reads (`peek64z`), and portable integer arithmetic right shifts (`asr_s32`).
- **Physical 14-bit Linear Light & Out-of-the-Box DNG 1.4 Compatibility**:
  - Outputs linear sensor RGGB code values (e.g., `BlackLevel = 1008`, `WhiteLevel = 16383`) cleanly separated from `.NEV` N-Log preview metadata (`NCTG 0x02000023 = N-LOG`, nominal `0x0025 = 12`).
  - `--active-crop` trims sensor margins to MakerNote `CropArea` and recomputes the `2x2 CFAPattern` modulo phase (`(x0 & 1, y0 & 1)`).
  - Emits scalar `BlackLevel (50714) RATIONAL[1]` (omitting `BlackLevelRepeatDim`) when all four CFA channels share the same pedestal, ensuring consistent black-level parsing across `LibRaw`, `dcraw_emu`, `macOS sips`, and standard DNG 1.4 tools. Built-in D65 `ColorMatrix1` profiles cover Nikon Z 8, Z 9, Z6 III, ZR, Z f, Z50 II, and Z5 II.

---

## Verified Accuracy & Performance

### 1. Pixel-Exact Parity vs. Reference Decoder Output

| Test Sample / Boundary Case | Geometry (`W x H`) / Mode | Max Abs Diff | Mismatched Pixels | Bottom-64 Rows Max Diff |
|---|---|---:|---:|---:|
| Nikon N-RAW `.NEV` clip (first / mid / last frames) | `45.6 MP` (`1.50 bpp` N-RAW) | **`0`** | **`0`** | **`0`** |
| Nikon Z 8 `.NEF` (`HE*`) | `8280 x 5520` (`5.00 bpp` HE\*) | **`0`** | **`0`** | **`0`** |
| Nikon Z50 II `.NEF` (`HE*`) | `5600 x 3728` (`5.00 bpp` HE\*) | **`0`** | **`0`** | **`0`** |
| Trailing-tile edge case (`H % 64 == 0`) | `512 x 512` synthetic stream | **`0`** | **`0`** | **`0`** |
| Trailing-tile edge case (`H % 64 == 4`) | `512 x 516` synthetic stream | **`0`** | **`0`** | **`0`** |

Single-threaded and multi-threaded runs (`1 / 4 / 8` threads) produce bit-identical outputs (`b1 == b4`).

### 2. Apple Silicon Multi-Threaded Benchmark (`45.6 MP`, `1.50 bpp` `.NEV`, 20-frame average)

| Threads | Decode Only (`ms/frame`) | Decode Throughput (`fps`) | Read + Decode E2E (`ms/frame`) | E2E Throughput (`fps`) |
|---:|---:|---:|---:|---:|
| `1` | `291.74` | `3.43` | `293.55` | `3.41` |
| `4` | `81.30` | `12.30` | `82.56` | `12.11` |
| **`8`** | **`49.67`** | **`20.13`** | **`50.95`** | **`19.63`** |

---

## Build & Quick Start

Requires only a `C++17` compiler (Clang or GCC) and POSIX threads (`-lpthread`), with zero external third-party libraries.

### Build & Test with Make (includes AddressSanitizer + UBSan)

```sh
make clean all test
```

Build outputs:
- `build/nev2bayer` (`-O3` release CLI)
- `build/nev2bayer_asan` (ASan + UBSan diagnostic CLI)
- `build/test_nev2bayer` / `build/test_nev2bayer_asan` (standalone unit suite; optional `NEV2BAYER_TEST_NEV` and `NEV2BAYER_TEST_NEF` environment variables enable real-sample regression checks)

### Build with CMake

```sh
cmake -S . -B cmake-build -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build -j
ctest --test-dir cmake-build --output-on-failure
```

### CLI Quick Start

```sh
# 1. Inspect .NEV / .NEF / .jxs headers, MakerNote, LPCM audio, timecode, and codestream parameters
./build/nev2bayer --info clip.NEV
./build/nev2bayer --info --json photo.NEF

# 2. Decode a frame to linear 14-bit-in-16-bit CFA DNG (full sensor buffer + DefaultCrop metadata)
./build/nev2bayer clip.NEV --frame 0 -o frame_0000.dng --threads 8

# 3. Crop to MakerNote CropArea with CFA phase rotation -> .dng or 16-bit LE .raw
./build/nev2bayer clip.NEV --frame 0 --active-crop -o frame_0000_crop.dng --threads 8
./build/nev2bayer photo.NEF --active-crop -o photo_crop.dng --threads 8
./build/nev2bayer clip.NEV --frame 0 -o frame_0000.raw --threads 8

# 4. Batch decode a frame range or entire clip into a directory
./build/nev2bayer clip.NEV --frame 0-9 -o out_dng/ --active-crop --threads 8

# 5. Extract/decode bare .jxs codestreams, embedded JPEG previews, or run a throughput benchmark
./build/nev2bayer --extract-jxs clip.NEV --frame 0 -o frame_0000.jxs
./build/nev2bayer --raw-jxs frame_0000.jxs -o frame_0000.dng --threads 8
./build/nev2bayer --extract-preview clip.NEV -o preview.jpg
./build/nev2bayer --bench clip.NEV --frames 20 --threads 8
```

See [**`docs/CLI.md`**](docs/CLI.md) for all flags, JSON fields, and C++17 `nev2bayer_core` integration examples.

---

## Repository Layout

| Path | Role |
|---|---|
| [`include/nev2bayer.h`](include/nev2bayer.h) | Public C++17 data structures and container, decode, crop, and DNG APIs |
| [`src/nev_container.cpp`](src/nev_container.cpp) | 64-bit `.NEV` ISOBMFF/NCDT/NRFH/NRTH parser and `.NEF` HE/HE\* TIFF/MakerNote parser |
| [`src/ticoraw_dec.cpp`](src/ticoraw_dec.cpp) | Clean-room TicoRAW R0 parallel entropy decoder, 5/3 2D IDWT, Star-Tetrix RCT, and Fliproot NLT |
| [`src/dng_writer.cpp`](src/dng_writer.cpp) | Scalar `BlackLevel` DNG 1.4 writer, D65 `ColorMatrix1` lookup, and modulo CFA cropper |
| [`src/main.cpp`](src/main.cpp) | CLI driver (`--info` / `--json` / batch decode / stream & preview extraction / `--bench`) |
| [`tests/test_nev2bayer.cpp`](tests/test_nev2bayer.cpp) | Standalone negative container/stream checks, CFA phase rotation, DNG round-trip, and optional corpus hooks |
| [`CMakeLists.txt`](CMakeLists.txt) · [`Makefile`](Makefile) | Zero-dependency build configuration (`nev2bayer_core` static library, `nev2bayer`, and tests) |

---

## License & Provenance

All original source code is licensed under the [**Apache License, Version 2.0 (`Apache-2.0`)**](LICENSE). See [**`NOTICE`**](NOTICE) for copyright, interoperability, and patent scope notices.
