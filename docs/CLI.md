# CLI & C++17 Library Reference

## 1. CLI Synopsis

```sh
nev2bayer --info [--json] <in.NEV | in.NEF | in.ARW | in.jxs>
nev2bayer <in.NEV | in.NEF | in.ARW> [--frame <N|start-end|all>] -o <out.dng|out.raw|dir/> [options]
nev2bayer --raw-jxs <in.jxs> -o <out.raw|out.dng> [options]
nev2bayer --extract-jxs <in.NEV | in.NEF> [--frame <N>] -o <out.jxs>
nev2bayer --extract-preview <in.NEV | in.NEF | in.ARW> [--thumb] -o <out.jpg>
nev2bayer --bench <in.NEV | in.ARW> [--frames N] [--threads N]
```

### Options

| Flag | Argument | Description |
|---|---|---|
| `-i`, `--input` | `<path>` | Input file (`.NEV`, `.NEF`, `.ARW`, or `.jxs`; also accepted positionally) |
| `-o`, `--output` | `<path>` | Output file (`.dng`, `.raw`, `.jxs`, `.jpg`) or directory (creates directory and writes `frame_NNNN.<ext>`) |
| `--frame` | `<N \| start-end \| all>` | Frame index, inclusive range (e.g., `0-29`), or `all` (default `0`; `.NEF` and `.ARW` accept `0`) |
| `--format` | `dng \| raw` | Overrides extension inference when writing into a directory (`.raw` is 16-bit LE RGGB row-major) |
| `--active-crop` | — | Crops decoded buffer to camera active crop (`CropArea` / `DefaultCrop`) and adjusts `2x2 CFAPattern` parity |
| `--orientation` | `<1..8>` | Overrides TIFF Orientation tag in output DNG |
| `--threads` | `<N>` | Worker thread count for slice/chunk entropy decode and 2D IDWT (`0` or omitted uses hardware concurrency) |
| `--info` | — | Inspect container, camera metadata, tracks, and codestream header without decoding pixels |
| `--json` | — | Emits machine-readable JSON when combined with `--info` |
| `--raw-jxs` | `<in.jxs>` | Decodes a standalone TicoRAW R0 codestream to `.raw` or `.dng` |
| `--extract-jxs` | `<in.NEV \| in.NEF>` | Extracts bare frame `--frame` TicoRAW codestream bytes to `.jxs` |
| `--extract-preview` | `<in.NEV \| in.NEF \| in.ARW>` | Extracts embedded preview JPEG (`NCVW` or largest TIFF `Compression=6` SubIFD); add `--thumb` for thumbnail |
| `--bench` | `<in.NEV \| in.ARW>` | Benchmarks `min(N, total_frames)` clip frames or `N` iterations of a single `.ARW` strip |
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

For a Sony ARW6 (`.ARW`) still (`Compression=32766`, `SonyRawFileType=6`), `--info --json` reports the `arw6` codec block alongside TIFF/Exif calibration fields:

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
./build/nev2bayer clip.NEV --frame 54 --active-crop -o frame_0054.dng --threads 8

# Export all frames in a .NEV clip as uncompressed 16-bit CFA DNGs
./build/nev2bayer clip.NEV --frame all -o frames_dng/ --active-crop --threads 8

# Batch-convert a folder of Nikon HE / HE* .NEF stills to calibrated DNGs
for f in *.NEF; do
  ./build/nev2bayer "$f" --active-crop -o "${f%.NEF}.dng" --threads 8
done

# Batch-convert a folder of Sony ARW6 / HQ RAW .ARW stills to calibrated DNGs
for f in *.ARW; do
  ./build/nev2bayer "$f" --active-crop -o "${f%.ARW}.dng" --threads 8
done
```

---

## 4. C++17 Static Library (`nev2bayer_core`) Example

Link against `nev2bayer_core` via CMake and include [`include/nev2bayer.h`](../include/nev2bayer.h):

```cpp
#include "nev2bayer.h"
#include <cstdio>
#include <vector>

int decode_one_frame(const std::string& input_path, const std::string& out_dng) {
    std::string err;
    nev2bayer::NevClipInfo clip;
    if (!nev2bayer::parse_nev_clip(input_path, clip, err)) {
        std::fprintf(stderr, "parse failed: %s\n", err.c_str());
        return 1;
    }

    nev2bayer::NevFrameMeta meta;
    std::vector<uint8_t> payload;
    if (!nev2bayer::read_nev_frame_meta_and_codestream(clip, 0, meta, &payload, err)) {
        std::fprintf(stderr, "read frame failed: %s\n", err.c_str());
        return 1;
    }

    std::vector<uint16_t> bayer;
    nev2bayer::DngWriteOptions opts;
    uint32_t raw_w = 0, raw_h = 0;

    if (clip.major_brand == "ARW") {
        nev2bayer::Arw6Info arw6;
        if (!nev2bayer::decode_arw6_bayer(payload.data(), payload.size(), 8, bayer, arw6, err)) {
            std::fprintf(stderr, "arw6 decode failed: %s\n", err.c_str());
            return 1;
        }
        nev2bayer::build_dng_options_from_clip_and_frame(clip, meta, arw6, /*apply_active_crop=*/true, 0, opts);
        raw_w = arw6.width;
        raw_h = arw6.height;
    } else {
        nev2bayer::TicoRawInfo jxs;
        if (!nev2bayer::decode_ticoraw_bayer(payload.data(), payload.size(), 8, bayer, jxs, err)) {
            std::fprintf(stderr, "ticoraw decode failed: %s\n", err.c_str());
            return 1;
        }
        nev2bayer::build_dng_options_from_clip_and_frame(clip, meta, jxs, /*apply_active_crop=*/true, 0, opts);
        raw_w = jxs.width;
        raw_h = jxs.height;
    }

    std::vector<uint16_t> cropped;
    const uint16_t* pix = bayer.data();
    if (meta.has_crop &&
        nev2bayer::apply_bayer_crop(bayer.data(), raw_w, raw_h, meta.crop, opts.cfa_pattern, cropped, opts.cfa_pattern)) {
        pix = cropped.data();
        opts.width = meta.crop[2];
        opts.height = meta.crop[3];
        opts.has_crop = false;
    }

    if (!nev2bayer::write_cfa_dng(out_dng, pix, opts, err)) {
        std::fprintf(stderr, "write dng failed: %s\n", err.c_str());
        return 1;
    }
    return 0;
}
```
