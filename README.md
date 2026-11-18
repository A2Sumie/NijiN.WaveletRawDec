# NijiN.WaveletRawDec

**Clean-Room Multi-Threaded C++20 & C99 Decoder for Wavelet-Compressed Camera & Cinema RAW Formats**  
(`Apache-2.0` · `C++20` / `C99 ABI` · `ARM64 NEON` & `x86-64 AVX2/SSE2` · `Zero External Dependencies`)

`NijiN.WaveletRawDec` (libraries `nijinWVDC::static` / `nijinWVDC::shared` · CMake / pkg-config `nijinWVDC` · CLI `nijinWVDC`) is purpose-built for the **two modern 2D-wavelet camera RAW codec families** whose open-source ecosystem support has historically been absent or incomplete:

1. **TicoRAW R0 (`5H+1V` LeGall 5/3 lifting + Star-Tetrix RCT + Fliproot NLT)** — Nikon N-RAW video (`.NEV`), Nikon High Efficiency stills (`.NEF` `HE` / `HE*`), and bare `.jxs` codestreams.
2. **Sony ARW 6.0 `"A000"` (`3`-level 5/3 lifting + Quincunx diamond green synthesis + 12-to-15-bit NLT)** — Sony `Compressed (HQ)` and `Compressed` stills (`.ARW`, `Compression=32766`, `SonyRawFileType=6`).

Conventional uncompressed, packed-linear, and lossless-JPEG RAW modes (such as legacy Nikon `.NEF` variants, or Sony `Compression=1`, `7`, and `32767`) are maturely handled by standard open-source decoders and intentionally return explicit format-classification errors here so downstream pipelines can route them cleanly. For both wavelet-compressed families, `NijiN.WaveletRawDec` outputs **physical 14-bit linear RGGB Bayer buffers (`.raw`)** and **calibrated uncompressed CFA DNG 1.4 negatives (`.dng`)**.

> **Documentation**: [**`docs/SPECIFICATION.md`** (Containers, Entropy/Wavelet Pipeline & DNG Calibration)](docs/SPECIFICATION.md) · [**`docs/CLI.md`** (CLI Reference & C++20 API)](docs/CLI.md) · [**`LICENSE`**](LICENSE) · [**`NOTICE`**](NOTICE)

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

- **Fused Wavelet Reconstruction & Trailing-Tile Architecture (`src/ticoraw_dec.cpp`)**:
  - Uses compile-time (`consteval`) segment/subband topology (`kTicoTopology`) and sign-expansion `.rodata` tables (`kSignExpandTable`), executing slice-parallel entropy decoding + 1D horizontal IDWT into 4 compact arena planes, followed by a fused Stage 1.5+2+3 L1/L2 strip loop (on-demand 4-row vertical LeGall 5/3 IDWT ring -> two-step Star-Tetrix inverse RCT -> baked Fliproot NLT LUT -> direct full-frame or ROI/cropped RGGB emission).
  - Eliminates trailing-tile edge artifacts on heights divisible by `64` (`H % 64 == 0`) or congruent to `4 mod 64` (`H % 64 == 4`) without cross-slice overlap re-decoding.
  - Enforces bounded vertical-prediction GCLI unary upper limits (`u <= 2 * bw`), strict per-packet `(data_B, gcli_B, sign_B)` bit accounting, bounds-safe tail bit-window reads (`peek64z`), branchless `std::countl_one`/`std::popcount` bit unpacking, and C++20 arithmetic right shifts (`asr_s32`).
- **Sony ARW6 (`"A000"`) Wavelet & Quincunx Pipeline (`src/arw6_dec.cpp`)**:
  - Decodes `1`-tile and `4`-tile ARW6 strips using chunk-parallel predictive Golomb-Rice 4-coefficient quad entropy coding (`k=0` `std::bit_width`/`std::countl_zero` adaptive zero-run mode, `update_k` predictor tracking, branchless midpoint dequantization & `kSignExpandTable` sign expansion) and 15-bit signed modular `LL0` horizontal DPCM (`[-16384, +16383]`).
  - Reconstructs `(G_L, R, B)` across 3 decomposition levels via **vertical-first, horizontal-second** integer LeGall 5/3 lifting IDWT (`G_H` bypassed at resolution `0`), followed by two-pass Quincunx diamond green synthesis, `(R, B)` residual reconstruction, direct `CropRect` partial-tile ROI emission, and a `consteval` 4096-entry 12-to-15-bit inverse NLT LUT (`kArw6NltLut14`).
- **Physical 14-bit Linear Light & Out-of-the-Box DNG 1.4 Compatibility**:
  - Outputs linear sensor RGGB code values (e.g., Nikon `BlackLevel = 1008`, Sony `BlackLevel = 512`, `WhiteLevel = 16383`) cleanly separated from `.NEV` N-Log preview metadata (`NCTG 0x02000023 = N-LOG`, nominal `0x0025 = 12`).
  - `--active-crop` trims sensor margins to camera `CropArea` / `DefaultCrop` and recomputes the `2x2 CFAPattern` modulo phase (`(x0 & 1, y0 & 1)`).
  - Emits scalar `BlackLevel (50714) RATIONAL[1]` (omitting `BlackLevelRepeatDim`) when all four CFA channels share the same pedestal, ensuring consistent black-level parsing across `LibRaw`, `dcraw_emu`, `macOS sips`, and standard DNG 1.4 tools. Built-in D65 `ColorMatrix1` profiles cover Nikon (`Z 8`, `Z 9`, `Z6 III`, `ZR`, `Z f`, `Z50 II`, `Z5 II`) and Sony Alpha (`ILCE-7M5`, `ILCE-7RM6`, `ILCE-7RM5`, `ILCE-7M4`, `ILCE-7CR`, `ILCE-7CM2`, `ILCE-1M2`, `ILCE-1`, `ILCE-9M3`, `ZV-E1`, `ILME-FX3`).
- **First-Principles Wavelet Quantization Telemetry & Optical Focus Metadata (`--quant`, [`src/quant_model_internal.h`](src/quant_model_internal.h), [`src/focus_meta_internal.h`](src/focus_meta_internal.h))**:
  - Computes deterministic per-row code-domain luma/chroma quantization RMS (`q_luma_code_row`, `q_chroma_code_row`), 6-scale PSD (`luma_scale_psd` / `chroma_scale_psd`), 16-bin 4x4 Walsh-Hadamard spatial spectrum (`walsh16_luma_norm` / `walsh16_chroma_norm`), and 256-entry inverse-NLT derivative LUT (`nlt_slope_lut`) from exact 5/3 IDWT impulse synthesis gains (`WaveletQuantProfile`).
  - Deterministically parses Sony (`0x9402` `(c ^ 55) % 249` with optical focal length at `+0x28`, `0x9404`/`0x9403`, `0x0020`/`0x09BB`) and Nikon (`LensData` `0100..0802` `kXlat0`/`kXlat1` stream decryption, Z-mount `0x35 == 1`, `0x0085` `ManualFocusDistance`) focus distance, diopter uncertainty bounds (`focus_distance_low_m` / `focus_distance_high_m`), and infinity state, emitting DNG `SubjectDistance (37382)` and `FocalLength (37386)`.

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

Single-threaded and multi-threaded runs (`1 / 4 / 8 / 16` threads) produce bit-identical outputs (`b1 == b4 == b8 == b16`) across ARM64 NEON, x86-64 SSE2, and x86-64 AVX2 (`AppleClang`, `GCC`, and `MSVC`).

### 2. Cross-Platform Decode Throughput (Steady-State Session Average)

| Platform / Compiler / ISA | Format (`W x H`) | Threads | Decode Only (`ms/frame`) | Decode Throughput (`fps`) |
|---|---|---:|---:|---:|
| **Apple M1 Max** (`v0.3.0`, `AppleClang 21`, `NEON`, 30f avg) | Sony ARW6 HQ (`7040 x 4688`) | `1` | `171.29` | `5.84` |
| **Apple M1 Max** (`v0.3.0`, `AppleClang 21`, `NEON`, 30f avg) | Sony ARW6 HQ (`7040 x 4688`) | `auto` | **`24.73`** | **`40.44`** |
| **Apple M1 Max** (`v0.3.0`, `AppleClang 21`, `NEON`, 30f avg) | Sony ARW6 Compressed (`7040 x 4688`) | `auto` | **`23.32`** | **`42.87`** |
| **Apple M1 Max** (`v0.3.0`, `AppleClang 21`, `NEON`, 30f avg) | Nikon `.jxs` (`8268 x 5512`) | `1` | `190.65` | `5.25` |
| **Apple M1 Max** (`v0.3.0`, `AppleClang 21`, `NEON`, 30f avg) | Nikon `.jxs` (`8268 x 5512`) | `auto` | **`27.21`** | **`36.75`** |
| **AMD Ryzen 7 5800X3D** (`v0.3.0`, `Linux GCC 15.2`, `AVX2`, 40f avg) | Sony ARW6 HQ (`7040 x 4688`) | `1` | `181.48` | `5.51` |
| **AMD Ryzen 7 5800X3D** (`v0.3.0`, `Linux GCC 15.2`, `AVX2`, 40f avg) | Sony ARW6 HQ (`7040 x 4688`) | `8` | `33.97` | `29.44` |
| **AMD Ryzen 7 5800X3D** (`v0.3.0`, `Linux GCC 15.2`, `AVX2`, 40f avg) | Sony ARW6 HQ (`7040 x 4688`) | `16` | **`31.64`** | **`31.60`** |
| **AMD Ryzen 7 5800X3D** (`v0.3.0`, `Linux GCC 15.2`, `AVX2`, 40f avg) | Sony ARW6 Compressed (`7040 x 4688`) | `16` | **`27.55`** | **`36.30`** |
| **AMD Ryzen 7 5800X3D** (`v0.3.0`, `Linux GCC 15.2`, `AVX2`, 40f avg) | Nikon `.jxs` (`8268 x 5512`) | `1` | `292.28` | `3.42` |
| **AMD Ryzen 7 5800X3D** (`v0.3.0`, `Linux GCC 15.2`, `AVX2`, 40f avg) | Nikon `.jxs` (`8268 x 5512`) | `8` | `47.50` | `21.05` |
| **AMD Ryzen 7 5800X3D** (`v0.3.0`, `Linux GCC 15.2`, `AVX2`, 40f avg) | Nikon `.jxs` (`8268 x 5512`) | `16` | **`35.44`** | **`28.22`** |
| **AMD Ryzen 7 5800X3D** (`v0.3.0`, `Windows MinGW-w64 GCC 16.2`, `AVX2`, 40f avg) | Sony ARW6 HQ (`7040 x 4688`) | `1` | `173.53` | `5.76` |
| **AMD Ryzen 7 5800X3D** (`v0.3.0`, `Windows MinGW-w64 GCC 16.2`, `AVX2`, 40f avg) | Sony ARW6 HQ (`7040 x 4688`) | `8` | `35.02` | `28.55` |
| **AMD Ryzen 7 5800X3D** (`v0.3.0`, `Windows MinGW-w64 GCC 16.2`, `AVX2`, 40f avg) | Sony ARW6 HQ (`7040 x 4688`) | `16` | **`27.31`** | **`36.61`** |
| **AMD Ryzen 7 5800X3D** (`v0.3.0`, `Windows MinGW-w64 GCC 16.2`, `AVX2`, 40f avg) | Sony ARW6 Compressed (`7040 x 4688`) | `16` | **`27.08`** | **`36.93`** |
| **AMD Ryzen 7 5800X3D** (`v0.3.0`, `Windows MinGW-w64 GCC 16.2`, `AVX2`, 40f avg) | Nikon `.jxs` (`8268 x 5512`) | `1` | `260.52` | `3.84` |
| **AMD Ryzen 7 5800X3D** (`v0.3.0`, `Windows MinGW-w64 GCC 16.2`, `AVX2`, 40f avg) | Nikon `.jxs` (`8268 x 5512`) | `8` | `45.29` | `22.08` |
| **AMD Ryzen 7 5800X3D** (`v0.3.0`, `Windows MinGW-w64 GCC 16.2`, `AVX2`, 40f avg) | Nikon `.jxs` (`8268 x 5512`) | `16` | **`34.56`** | **`28.94`** |

---

## Prebuilt Releases (`v0.3.0`)

Prebuilt SDK + CLI archives are published on [**GitHub Releases**](https://github.com/A2Sumie/NijiN.WaveletRawDec/releases). Every archive contains the `nijinWVDC` CLI, static + shared libraries, public C++20 & C99 headers, `pkg-config` (`.pc`), and CMake config packages (`nijinWVDC::static` / `nijinWVDC::shared`):

| Platform Archive | Contents |
|---|---|
| `nijinWVDC-v0.3.0-macos-arm64.tar.gz` | Apple Silicon `arm64` static (`.a`), shared (`.dylib`), CLI, headers, CMake/pkg-config |
| `nijinWVDC-v0.3.0-linux-x86_64.tar.gz` | Linux `x86_64` (GCC 15.2, glibc 2.25+, SSE2 + runtime AVX2 dispatch) `.a`, `.so`, CLI, headers, CMake/pkg-config |
| `nijinWVDC-v0.3.0-windows-x86_64.zip` | Windows `x64` (MinGW-w64 GCC 16.2, `nijinWVDC.dll`, `.dll.a` / `.lib` import libs, `.a`), `nijinWVDC.exe`, headers, CMake/pkg-config |

---

## Build & Quick Start

Requires a `C++20` / `C11` toolchain (macOS `14.0+` / AppleClang, Linux `GCC 15+`, or Windows `MinGW-w64 GCC 16+` / `MSVC 2022`), with zero external third-party libraries. x86-64 builds detect `AVX2` at runtime via `CPUID` + `XGETBV` and fall back automatically to pure `SSE2`.

### Build & Test with Make (includes AddressSanitizer + UBSan & C11 ABI test)

```sh
make clean && make all test -j8
```

Build outputs:
- `build/nijinWVDC` (`-O3` release CLI) and `build/nijinWVDC_asan` (ASan + UBSan diagnostic CLI)
- `build/libnijinWVDC.a` (static library) & `build/libnijinWVDC.dylib` / `.so` (shared library)
- `build/test_nijinWVDC`, `build/test_c_api`, `build/test_nijinWVDC_asan`, `build/test_c_api_asan` (optional `NIJINWVDC_TEST_NEV`, `NIJINWVDC_TEST_NEF`, and `NIJINWVDC_TEST_ARW` enable real-sample checks)

### Build & Install with CMake

```sh
cmake -S . -B cmake-build -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build -j
ctest --test-dir cmake-build --output-on-failure
cmake --install cmake-build --prefix /usr/local
```

### Library APIs (C++20 & Pure C99 FFI)

- **`nijinwvdc::MediaReader` ([`include/nijinWVDC.h`](include/nijinWVDC.h))**: Thread-safe >4 GiB 64-bit random-access container reader (`open_file(std::string_view)`, `open_memory(std::span<const uint8_t>)`, `open_callbacks`), supporting `.NEV`, `.NEF`, `.ARW`, and bare `.jxs` / `"A000"` codestreams, exact `stts` PTS/duration + SMPTE `tmcd` timecode (`get_frame_timing`), `O(log C)` `lpcm` audio extraction & RIFF `.wav` export (`read_audio_pcm`, `export_audio_wav`), and JPEG preview extraction.
- **`nijinwvdc::DecoderSession` ([`include/nijinWVDC.h`](include/nijinWVDC.h))**: Movable session owning a cache-line-isolated `std::atomic::wait` work-stealing thread pool and reusable per-codec arena buffers for zero-reallocation NLE/video playback, `std::span` bounds-checked / strided caller-owned buffers (`decode_frame_into` / `decode_codestream_into`), and direct Stage 3 ROI / `--active-crop` decoding.
- **SIMD Post-Processing & In-Memory DNG**: `split_bayer_cfa_planes`, `normalize_bayer_to_float`, `apply_bayer_crop_strided`, `build_cfa_dng_header`, and `encode_cfa_dng_memory`.
- **Pure C99 / FFI ABI ([`include/nijinWVDC_c.h`](include/nijinWVDC_c.h))**: Exception-free opaque handle API (`nijin_reader_t*`, `nijin_decoder_t*`) for Rust, Python `ctypes`, Go `cgo`, and pure C11 applications linking against `libnijinWVDC` / `nijinWVDC.dll`.

### CLI Quick Start

```sh
# 1. Inspect .NEV / .NEF / .ARW / .jxs headers, focus telemetry, quantization profile, tracks, and codestream parameters
./build/nijinWVDC --version
./build/nijinWVDC --info clip.NEV
./build/nijinWVDC --info --json photo.NEF
./build/nijinWVDC --info --quant --json photo.ARW

# 2. Extract embedded LPCM audio from a .NEV clip as RIFF WAV
./build/nijinWVDC --extract-audio clip.NEV -o audio.wav

# 3. Decode a frame to linear 14-bit-in-16-bit CFA DNG (full sensor buffer + DefaultCrop metadata)
./build/nijinWVDC clip.NEV --frame 0 -o frame_0000.dng --threads 8
./build/nijinWVDC photo.ARW -o sony_full.dng --threads 8

# 4. Crop to camera active crop with CFA phase rotation -> .dng or 16-bit LE .raw
./build/nijinWVDC clip.NEV --frame 0 --active-crop -o frame_0000_crop.dng --threads 8
./build/nijinWVDC photo.NEF --active-crop -o photo_crop.dng --threads 8
./build/nijinWVDC photo.ARW --active-crop -o sony_crop.dng --threads 8
./build/nijinWVDC clip.NEV --frame 0 -o frame_0000.raw --threads 8

# 5. Batch decode a frame range, extract bare .jxs / JPEG previews, or benchmark throughput
./build/nijinWVDC clip.NEV --frame 0-9 -o out_dng/ --active-crop --threads 8
./build/nijinWVDC --extract-jxs clip.NEV --frame 0 -o frame_0000.jxs
./build/nijinWVDC --raw-jxs frame_0000.jxs -o frame_0000.dng --threads 8
./build/nijinWVDC --extract-preview clip.NEV -o preview.jpg
./build/nijinWVDC --bench clip.NEV --frames 40 --threads 0
```

See [**`docs/CLI.md`**](docs/CLI.md) for all flags, JSON schema, C++20 `MediaReader`/`DecoderSession`, and C99 `nijinWVDC_c.h` examples.

---

## Repository Layout

| Path | Role |
|---|---|
| [`include/nijinWVDC.h`](include/nijinWVDC.h) | Public C++20 API (`MediaReader`, `DecoderSession`, `std::span`/`std::string_view` overloads, post-processing, DNG & legacy functional APIs) |
| [`include/nijinWVDC_c.h`](include/nijinWVDC_c.h) | Pure C99 shared-library FFI ABI (`nijin_reader_t`, `nijin_decoder_t`, caller-buffer APIs) |
| [`include/nijinWVDC_export.h`](include/nijinWVDC_export.h) | Portable symbol visibility (`dllexport`/`dllimport`/hidden), version macros, and SIMD flags |
| [`src/simd_platform.h`](src/simd_platform.h) · [`src/worker_pool.h`](src/worker_pool.h) | Portable intrinsics, `<bit>` / `consteval` tables, `CPUID`/`XGETBV` runtime AVX2 detection, and `std::atomic::wait` work-stealing pool |
| [`src/codec_internal.h`](src/codec_internal.h) · [`src/tiff_internal.h`](src/tiff_internal.h) | Internal persistent-arena strided/ROI decoder interfaces and thread-safe 64-bit `RandomAccessReader` |
| [`src/quant_model_internal.h`](src/quant_model_internal.h) · [`src/focus_meta_internal.h`](src/focus_meta_internal.h) | Compile-time (`consteval`) 5/3 IDWT + 4x4 Walsh quantization telemetry (`WaveletQuantProfile`) and Sony/Nikon MakerNote focus/DoF parsers |
| [`src/nev_container.cpp`](src/nev_container.cpp) | `.NEV` ISOBMFF/`NCTG`/`lpcm`/`tmcd`/`stts`/`NRFH` parser, `.NEF` TIFF/MakerNote, bare codestreams |
| [`src/arw_container.cpp`](src/arw_container.cpp) | Sony `.ARW` (`ARW6` `Compressed HQ` / `Compressed`) TIFF `SubIFDs`, calibration tags, and JPEG preview parser |
| [`src/ticoraw_dec.cpp`](src/ticoraw_dec.cpp) | SIMD-accelerated TicoRAW R0 parallel entropy decoder, fused L2-ring vertical 5/3 IDWT + Star-Tetrix RCT + Fliproot NLT |
| [`src/arw6_dec.cpp`](src/arw6_dec.cpp) | SIMD-accelerated Sony ARW6 (`"A000"`) SWAR/`<bit>` entropy decoder, fused 5/3 IDWT, diamond synthesis, and `consteval` NLT LUT |
| [`src/session_api.cpp`](src/session_api.cpp) · [`src/c_api.cpp`](src/c_api.cpp) | `MediaReader`, `DecoderSession`, SIMD CFA-plane split & float normalizer, WAV exporter, and C99 ABI wrapper |
| [`src/dng_writer.cpp`](src/dng_writer.cpp) | In-memory & file DNG 1.4 writer, `constexpr` D65 `ColorMatrix1` lookup, and strided modulo CFA cropper |
| [`src/main.cpp`](src/main.cpp) | Cross-platform CLI driver (`--version` / `--info` / `--json` / batch decode / `--extract-audio` / `--bench`) |
| [`tests/test_nijinWVDC.cpp`](tests/test_nijinWVDC.cpp) · [`tests/test_c_api.c`](tests/test_c_api.c) | C++20 & C11 shared-library unit, `std::span`, concurrency, strided/ROI, audio/timecode WAV, and corpus suites |
| [`CMakeLists.txt`](CMakeLists.txt) · [`cmake/`](cmake) · [`Makefile`](Makefile) | Static + shared library targets, CMake package config, pkg-config, and [`scripts/package_release.py`](scripts/package_release.py) release packager |

---

## License & Provenance

All original source code is licensed under the [**Apache License, Version 2.0 (`Apache-2.0`)**](LICENSE). See [**`NOTICE`**](NOTICE) for copyright, interoperability, and patent scope notices.
