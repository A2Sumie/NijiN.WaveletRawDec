# CLI, C++17 & Pure C99 Library Reference

## 1. CLI Synopsis

```sh
nijinWVDC --version
nijinWVDC --info [--json] <in.NEV | in.NEF | in.ARW | in.jxs>
nijinWVDC <in.NEV | in.NEF | in.ARW> [--frame <N|start-end|all>] -o <out.dng|out.raw|dir/> [options]
nijinWVDC --raw-jxs <in.jxs> -o <out.raw|out.dng> [options]
nijinWVDC --extract-jxs <in.NEV | in.NEF> [--frame <N>] -o <out.jxs>
nijinWVDC --extract-audio <in.NEV> -o <out.wav>
nijinWVDC --extract-preview <in.NEV | in.NEF | in.ARW> [--thumb] -o <out.jpg>
nijinWVDC --bench <in.NEV | in.ARW> [--frames N] [--threads N]
```

### Options

| Flag | Argument | Description |
|---|---|---|
| `-V`, `--version` | — | Prints library/CLI version string (`nijinWVDC 0.2.0`) and exits |
| `-i`, `--input` | `<path>` | Input file (`.NEV`, `.NEF`, `.ARW`, or `.jxs`; also accepted positionally) |
| `-o`, `--output` | `<path>` | Output file (`.dng`, `.raw`, `.jxs`, `.wav`, `.jpg`) or directory (creates directory and writes `frame_NNNN.<ext>`) |
| `--frame` | `<N \| start-end \| all>` | Frame index, inclusive range (e.g., `0-29`), or `all` (default `0`; `.NEF` and `.ARW` accept `0`) |
| `--format` | `dng \| raw` | Overrides extension inference when writing into a directory (`.raw` is 16-bit LE RGGB row-major) |
| `--active-crop` | — | Crops decoded buffer to camera active crop (`CropArea` / `DefaultCrop`) and adjusts `2x2 CFAPattern` parity |
| `--orientation` | `<1..8>` | Overrides TIFF Orientation tag in output DNG |
| `--threads` | `<N>` | Worker thread count for slice/chunk entropy decode and 2D IDWT (`0` or omitted uses hardware concurrency) |
| `--info` | — | Inspect container, camera metadata, tracks (including start SMPTE `timecode`), and codestream header without decoding pixels |
| `--json` | — | Emits machine-readable JSON when combined with `--info` |
| `--raw-jxs` | `<in.jxs>` | Decodes a standalone TicoRAW R0 codestream to `.raw` or `.dng` |
| `--extract-jxs` | `<in.NEV \| in.NEF>` | Extracts bare frame `--frame` TicoRAW codestream bytes to `.jxs` |
| `--extract-audio` | `<in.NEV>` | Exports embedded QuickTime `lpcm` / PCM audio track as a standard RIFF `.wav` file to `-o <out.wav>` |
| `--extract-preview` | `<in.NEV \| in.NEF \| in.ARW>` | Extracts embedded preview JPEG (`NCM1` / `NCVW` or largest TIFF `Compression=6` SubIFD); add `--thumb` for thumbnail |
| `--bench` | `<in.NEV \| in.ARW>` | Benchmarks `min(N, total_frames)` clip frames or `N` iterations of a single `.ARW`/`.jxs` using a persistent `DecoderSession` |
| `--frames` | `<N>` | Frame count for `--bench` (default `20`) |

### Exit Codes

- `0`: Success
- `1`: File I/O, container parse, or TicoRAW decode error
- `2`: Invalid/missing CLI arguments or out-of-range frame specification

---

## 2. `--info --json` Schema

Example output on a Nikon Z 8 `.NEV` file (`Ver.03.12`):

```json
{
  "file": "clip.NEV",
  "file_size": 780513280,
  "major_brand": "niko",
  "make": "NIKON CORPORATION",
  "model": "NIKON Z 8",
  "firmware": "NIKON Z 8 Ver.03.12",
  "lens": "NIKKOR Z 24-120mm f/4 S",
  "frames": 108,
  "fps": 29.970030,
  "codestream_width": 8268,
  "codestream_height": 4652,
  "active_crop": [6, 4, 8256, 4644],
  "orientation": 1,
  "iso": 4000,
  "exposure_time_s": 0.008000,
  "fnumber": 4.00,
  "black_level": [1008, 1008, 1008, 1008],
  "wb_rggb": [1.91015625, 1.00000000, 1.00000000, 1.51171875],
  "picture_control_base": "N-LOG",
  "nef_compression": 15,
  "ticoraw": {
    "lcod": 7214080,
    "in_bitdepth": 14,
    "out_bitdepth": 16,
    "cdt_depth": 14,
    "gamma_enabled": true,
    "gamma_type": 1,
    "offset_16": 4032,
    "r_flip": 84429,
    "slices": 73,
    "precincts": 1163
  }
}
```

For a Sony ARW6 (`.ARW`) still (`Compressed HQ` / `Compressed`, `Compression=32766`, `SonyRawFileType=6`), `--info --json` reports the `arw6` codec block alongside TIFF/Exif calibration fields:

```json
{
  "file": "photo.ARW",
  "file_size": 16965632,
  "major_brand": "ARW",
  "make": "SONY",
  "model": "ILCE-7M5",
  "codestream_width": 7040,
  "codestream_height": 4688,
  "active_crop": [12, 8, 7008, 4672],
  "black_level": [512, 512, 512, 512],
  "arw6": {
    "strip_bytes": 14540288,
    "tiles": 1,
    "bit_depth": 14,
    "internal_depth": 12,
    "decomp_levels": 3,
    "subsampling_mask": "0x0200",
    "sony_raw_file_type": 6
  }
}
```

For a bare `.jxs` codestream, `--info --json` emits a compact header summary:

```json
{"width":8268,"height":4652,"lcod":7214080,"cdt_depth":14,"in_bitdepth":14,"out_bitdepth":16,"gamma_type":1,"slices":73,"precincts":1163}
```

---

## 3. Batch Workflow Examples

```sh
# Decode a single frame to cropped linear DNG
./build/nijinWVDC clip.NEV --frame 54 --active-crop -o frame_0054.dng --threads 8

# Export all frames in a .NEV clip as uncompressed 16-bit CFA DNGs
./build/nijinWVDC clip.NEV --frame all -o frames_dng/ --active-crop --threads 8

# Batch-convert a folder of Nikon HE / HE* .NEF stills to calibrated DNGs
for f in *.NEF; do
  ./build/nijinWVDC "$f" --active-crop -o "${f%.NEF}.dng" --threads 8
done

# Batch-convert a folder of Sony ARW6 (Compressed HQ / Compressed) .ARW stills to calibrated DNGs
for f in *.ARW; do
  ./build/nijinWVDC "$f" --active-crop -o "${f%.ARW}.dng" --threads 8
done
```

---

## 4. C++17 Stateful Pipeline (`MediaReader` + `DecoderSession`) & Pure C99 ABI (`nijinWVDC_c.h`)

Link via CMake using `find_package(nijinWVDC CONFIG REQUIRED)` against either `nijinWVDC::static` or `nijinWVDC::shared`:

```cmake
find_package(nijinWVDC CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE nijinWVDC::shared) # or nijinWVDC::static
```

### 4.1 High-Throughput Video/Frame, Audio & Float-Plane C++17 Example (`include/nijinWVDC.h`)

```cpp
#include "nijinWVDC.h"
#include <vector>

bool process_clip(const std::string& path, const std::string& out_wav, std::string& err) {
    nijinwvdc::MediaReader reader;
    if (!reader.open_file(path, err)) return false;

    // Optional synced audio export from .NEV clips
    if (reader.audio_info().available) {
        if (!reader.export_audio_wav(out_wav, err)) return false;
    }

    // Reuse one DecoderSession across frames to amortize worker-pool & arena allocations
    nijinwvdc::DecoderSession session(/*num_threads=*/0);
    nijinwvdc::DecodeOptions opts;
    opts.apply_active_crop = true;

    std::vector<uint16_t> bayer;
    nijinwvdc::DecodedFrameInfo info;
    for (uint32_t f = 0; f < reader.frame_count(); ++f) {
        if (!session.decode_frame(reader, f, opts, bayer, info, err)) return false;

        // Example: extract normalized [0, 1] planar float RAW tensor for GPU/ML pipelines
        const float blk[4] = {
            static_cast<float>(info.meta.black[0]), static_cast<float>(info.meta.black[1]),
            static_cast<float>(info.meta.black[2]), static_cast<float>(info.meta.black[3])
        };
        const float white = (info.meta.white_level > 0)
                                ? static_cast<float>(info.meta.white_level)
                                : static_cast<float>((1u << info.bit_depth) - 1u);
        std::vector<float> norm(static_cast<size_t>(info.output_width) * info.output_height);
        nijinwvdc::normalize_bayer_to_float(
            bayer.data(), 0, info.output_width, info.output_height, blk, white, true, norm.data(), 0);
    }
    return true;
}
```

> **Note**: Custom I/O sources (`.NEV`, `.NEF`, `.ARW`, bare `.jxs`/`"A000"`) can open via `reader.open_memory(...)` or `reader.open_callbacks(...)`, and callers can supply strided destination buffers via `session.decode_frame_into(...)` or split 2x2 Bayer planes via `split_bayer_cfa_planes(...)`. All legacy free functions (`parse_nev_clip`, `read_nev_frame_meta_and_codestream`, `decode_ticoraw_bayer`, `decode_arw6_bayer`, `apply_bayer_crop`, `write_cfa_dng`) remain preserved in [`include/nijinWVDC.h`](../include/nijinWVDC.h).

### 4.2 Pure C99 FFI Example (`include/nijinWVDC_c.h`)

```c
#include "nijinWVDC_c.h"
#include <stdlib.h>

nijin_status_t decode_first_frame_c(const char* path, const char* out_dng) {
    nijin_reader_t* reader = nijin_reader_create();
    nijin_decoder_t* dec = nijin_decoder_create(0);
    nijin_clip_info_t clip;
    nijin_decode_options_t dopts;
    nijin_decoded_info_t dinfo;
    nijin_dng_options_t dng_opts;
    uint16_t* bayer = NULL;
    nijin_status_t st;

    st = nijin_reader_open_file(reader, path);
    if (st != NIJIN_OK) goto done;
    st = nijin_reader_get_clip_info(reader, &clip);
    if (st != NIJIN_OK) goto done;

    bayer = (uint16_t*)malloc((size_t)clip.width * clip.height * sizeof(uint16_t));
    nijin_decode_options_init(&dopts);
    dopts.apply_active_crop = 1;
    st = nijin_decoder_decode_frame(dec, reader, 0u, &dopts, bayer, 0u, clip.height, &dinfo);
    if (st == NIJIN_OK) {
        nijin_dng_options_from_clip(reader, 0u, &dinfo, 1, 0u, &dng_opts);
        st = nijin_write_dng_file(out_dng, bayer, 0u, &dng_opts, NULL, 0u);
    }
done:
    free(bayer);
    nijin_decoder_destroy(dec);
    nijin_reader_destroy(reader);
    return st;
}
```
