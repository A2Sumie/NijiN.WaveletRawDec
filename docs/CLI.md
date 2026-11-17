# CLI & C++17 Library Reference

## 1. CLI Synopsis

```sh
nev2bayer --info [--json] <in.NEV | in.NEF | in.jxs>
nev2bayer <in.NEV | in.NEF> [--frame <N|start-end|all>] -o <out.dng|out.raw|dir/> [options]
nev2bayer --raw-jxs <in.jxs> -o <out.raw|out.dng> [options]
nev2bayer --extract-jxs <in.NEV | in.NEF> [--frame <N>] -o <out.jxs>
nev2bayer --extract-preview <in.NEV | in.NEF> [--thumb] -o <out.jpg>
nev2bayer --bench <in.NEV> [--frames N] [--threads N]
```

### Options

| Flag | Argument | Description |
|---|---|---|
| `-i`, `--input` | `<path>` | Input file (`.NEV`, `.NEF`, or `.jxs`; also accepted positionally) |
| `-o`, `--output` | `<path>` | Output file (`.dng`, `.raw`, `.jxs`, `.jpg`) or directory (creates directory and writes `frame_NNNN.<ext>`) |
| `--frame` | `<N \| start-end \| all>` | Frame index, inclusive range (e.g., `0-29`), or `all` (default `0`; `.NEF` accepts `0`) |
| `--format` | `dng \| raw` | Overrides extension inference when writing into a directory (`.raw` is 16-bit LE RGGB row-major) |
| `--active-crop` | — | Crops decoded buffer to MakerNote `0x0045 CropArea` and adjusts `2x2 CFAPattern` parity |
| `--orientation` | `<1..8>` | Overrides TIFF Orientation tag in output DNG |
| `--threads` | `<N>` | Worker thread count for slice entropy decode and 2D IDWT (`0` or omitted uses hardware concurrency) |
| `--info` | — | Inspect container, MakerNote, tracks, and TicoRAW header without decoding pixels |
| `--json` | — | Emits machine-readable JSON when combined with `--info` |
| `--raw-jxs` | `<in.jxs>` | Decodes a standalone TicoRAW R0 codestream to `.raw` or `.dng` |
| `--extract-jxs` | `<in.NEV \| in.NEF>` | Extracts bare frame `--frame` TicoRAW codestream bytes to `.jxs` |
| `--extract-preview` | `<in.NEV \| in.NEF>` | Extracts embedded preview JPEG (`NCVW` or largest NEF `Compression=6` SubIFD); add `--thumb` for thumbnail |
| `--bench` | `<in.NEV>` | Benchmarks `min(N, total_frames)` frames reporting decode-only and read+decode ms/fps |
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
    std::vector<uint8_t> codestream;
    if (!nev2bayer::read_nev_frame_meta_and_codestream(clip, 0, meta, &codestream, err)) {
        std::fprintf(stderr, "read frame failed: %s\n", err.c_str());
        return 1;
    }

    nev2bayer::TicoRawInfo jxs;
    std::vector<uint16_t> bayer;
    if (!nev2bayer::decode_ticoraw_bayer(codestream.data(), codestream.size(), 8, bayer, jxs, err)) {
        std::fprintf(stderr, "decode failed: %s\n", err.c_str());
        return 1;
    }

    nev2bayer::DngWriteOptions opts;
    nev2bayer::build_dng_options_from_clip_and_frame(clip, meta, jxs, /*apply_active_crop=*/true, 0, opts);

    std::vector<uint16_t> cropped;
    const uint16_t* pix = bayer.data();
    if (meta.has_crop &&
        nev2bayer::apply_bayer_crop(bayer.data(), jxs.width, jxs.height, meta.crop, opts.cfa_pattern, cropped, opts.cfa_pattern)) {
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
