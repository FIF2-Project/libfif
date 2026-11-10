# libfif

**The reference encoder, decoder and command-line tool for the FIF2 fractal image format.**

[**The FIF2 format — fif2.org**](https://fif2.org) ·
[**What is fractal image compression? — fractalimagecompression.com**](https://www.fractalimagecompression.com) ·
[Download](https://github.com/FIF2-Project/libfif/releases/latest) ·
[Documentation](#documentation)

[![CI](https://github.com/FIF2-Project/libfif/actions/workflows/ci.yml/badge.svg)](https://github.com/FIF2-Project/libfif/actions/workflows/ci.yml)
[![Latest release](https://img.shields.io/github/v/release/FIF2-Project/libfif?label=release)](https://github.com/FIF2-Project/libfif/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

---

FIF2 stores a picture as a *fractal code*: a set of contractive transforms that
map parts of the image onto other parts. Decoding iterates that map until it
converges on the picture. Because the code describes structure, not pixels, a FIF2 file
can be rendered at any size.

This repository contains:

- **`libfif`**: the codec as a C library (C++20 inside, plain C interface).
- **`fif`**: a command-line tool to encode, decode, inspect and benchmark FIF2 files.

| Component | Version |
| --- | --- |
| libfif | 3.5 |
| fif (CLI) | 2.5 |
| FIF2 format | 3.3 |

## Features

- **Resolution-independent decoding.** Render the attractor at 2×, 4× or any
  target size instead of resampling pixels.
- **Progressive by design.** Every prefix of a file is a valid picture, and a
  longer prefix is never a worse one.
- **Random access by region.** Tiles decode independently, and a directory at the
  front gives the byte range of each one.
- **Built-in preview, metadata and attachments.** Thumbnails, EXIF, text chunks
  and raw sections live in the container.
- **Quality or size targets.** Use `-q 0..100`, or let the encoder bisect to a
  target PSNR or bits per pixel.
- **Fast.** Threaded across tiles, with SIMD kernels for NEON, AVX2, AVX and SSE2,
  and no GPU required.
- **Robust.** Truncated and damaged files decode as far as the data reaches,
  and CRCs report damage.

## Download

Prebuilt binaries are attached to every
[release](https://github.com/FIF2-Project/libfif/releases/latest):

| File | Contents |
| --- | --- |
| `fif-<ver>-windows-x64.zip` | `fif.exe` command-line tool, Windows 10/11, 64-bit |
| `fif-<ver>-macos-universal.zip` | *coming soon*: `fif` command-line tool, macOS 11+, Apple Silicon and Intel |
| `libfif-<ver>-sdk-windows-x64.zip` | header, `libfif.dll` + import library, static library |
| `libfif-<ver>-sdk-macos-universal.zip` | *coming soon*: header, `libfif.3.dylib`, static library |
| `SHA256SUMS.txt` | checksums for verifying the downloads |

Release builds target generic x86-64 and ARM64, so they run on any 64-bit CPU.
For maximum encoder speed on your own machine, [build from source](#building-from-source).

## Quick start

```sh
fif encode photo.jpg photo.fif -q 80          # compress (quality 0..100)
fif decode photo.fif photo.png                # back to PNG / JPEG / PNM
fif decode photo.fif big.png --scale 4        # render at 4x, not resampled
fif decode photo.fif hd.png --size 1920x1080  # render, filter and centre-crop to a size
fif info   photo.fif                          # header, metadata, preview, tiles
fif bench  photo.jpg -q 60                    # size, PSNR, MS-SSIM and speed in one step
```

`-e 0..9` sets how hard the encoder searches. `--target-psnr` and `--target-bpp`
hit a quality or size instead of a setting. `fif --help` and `fif help <command>`
list everything. The full guide is [Using the fif tool](https://fif2.org/lib-cli.html).

## Using the library

```c
#include <fif/fif.h>

fif_image img = {0};
fif_image_load("photo.png", &img);

fif_encode_options eo;
fif_encode_options_defaults(&eo);       /* always start from the defaults */
eo.quality = 70;

uint8_t *data = NULL;
size_t   size = 0;
if (fif_encode(&img, &eo, &data, &size, NULL) == FIF_OK) {
    fif_decode_options dopt;
    fif_decode_options_defaults(&dopt);
    dopt.out_width = 1920;               /* decode straight to the size you need */

    fif_image out = {0};
    if (fif_decode(data, size, &dopt, &out, NULL) == FIF_OK)
        fif_image_save("photo-1920.png", &out);
    fif_image_free(&out);
    fif_free(data);
}
fif_image_free(&img);
```

The library comes in two forms:

| | Windows | macOS | Linux (source build) |
| --- | --- | --- | --- |
| **shared**, recommended | `libfif.dll` + import library `libfif.lib` | `libfif.3.dylib` | `libfif.so.3` |
| **static** | `libfif_static.lib` (`/MT` Release only) | `libfif.a` | `libfif.a` |

The shared library has the image libraries built in and exports only the
`fif_*` functions. It works with any compiler setting, Debug or Release, and
from any language with a C FFI. The static library must be linked with a C++
linker plus libpng, libjpeg and zlib. Every function returns an
`fif_status`, and `fif_status_string()` explains the code. See
[Using the library](https://fif2.org/lib-api.html) for the five rules of the interface and the full
function reference.

## Building from source

You need a C++20 compiler. The code is written for clang, and GCC also works on
Linux and macOS. libpng, libjpeg-turbo and zlib are optional: without them, the tool
reads and writes PNM only.

**macOS / Linux**

```sh
# macOS
brew install pkg-config libpng jpeg-turbo
export PKG_CONFIG_PATH="$(brew --prefix jpeg-turbo)/lib/pkgconfig:$PKG_CONFIG_PATH"
# Ubuntu / Debian
sudo apt install clang make pkg-config libpng-dev libjpeg-turbo8-dev

make            # builds ./fif and ./libfif.a, tuned for this machine
make ARCH=      # generic build that runs on any CPU of the architecture
make shared     # also builds libfif.3.dylib (macOS) / libfif.so.3 (Linux)
make san        # address + undefined-behaviour sanitiser build
```

**Windows** (clang++ from the *C++ Clang tools for Windows* component of Visual Studio or Build Tools)

```bat
vcpkg install libpng libjpeg-turbo zlib --triplet x64-windows-static
build.bat                       :: fif.exe + libfif_static.lib, tuned for this machine
build.bat release generic       :: runs on any x64 CPU
build.bat release shared        :: also libfif.dll, libfif.lib (import), libfif.pdb
build.bat debug | asan | clean
```

`build.bat` finds vcpkg via `%VCPKG_ROOT%`, `%USERPROFILE%\vcpkg` or `C:\vcpkg`.
Details, including why the static triplet is required, are in
[Getting started](https://fif2.org/lib-getting-started.html).

## How it works

Each tile is split by a **rate-distortion quadtree**. A block is divided only when
the distortion saved is worth the bits spent. Every leaf is matched against
a pool of domain blocks twice its size, under the eight dihedral transforms,
and coded as `dc + s·(D − mean D)`. The contrast `s` has a closed form, so the
hot loop is a single inner product. Candidates come from a local window and
from a kd-tree over DCT feature keys (Saupe, 1995). From effort 3 up, the encoder
codes each tile both ways and keeps whichever came out cheaper.

Leaves without a transform can carry a **planar gradient**, which avoids most of the
splits smooth ramps would otherwise need. An **adaptive binary range coder** with
contexts trained offline codes the result. The fields use strong predictors:
a JPEG-LS median edge detector for DC, and an H.264-style median predictor for the
domain index. The decoder seeds the iteration with the coded block means, converges,
then switches on an in-loop **seam filter**. Nine iterations are typical.

**Where it stands:** at low bit rates FIF2 clearly beats JPEG, by nearly 4 dB at
180 KB on an 8.3 MP photograph. At high rates it saturates, because a range
block can only be as good as the best domain in the picture. Modern
intra codecs such as WebP stay ahead throughout. The measurements, the comparison
tables and the ideas that were tried and removed are in
[docs/design.md](docs/design.md).

## Source layout

```
include/fif/fif.h       public C API: structs, defaults, status codes
tools/fif_cli.cpp       the fif tool: options, help texts, error messages
src/encode_api.cpp      fif_encode: parameters, tile scheduling, chunks, thumbnail
src/decode.cpp          fif_decode / fif_get_info: iteration, scaling, post-processing
src/encode.cpp          rate-distortion quadtree, candidate search, strategy choice
src/pool.cpp            domain pool, feature keys, kd-tree
src/kernels.h           SIMD inner product and distance (NEON / AVX2 / AVX / SSE2 / scalar)
src/serialize.cpp       bitstream syntax, both directions
src/rangecoder.h        adaptive binary range coder
src/context.h           context models, predictors, residual coding
src/ctxtable.inc        trained initial probabilities (generated)
src/render.cpp          reconstruction: one application of the fractal map
src/deblock.cpp         in-loop seam filter
src/format.cpp          FIF2 container: header, sections, chunks, zlib, CRC
src/debug.cpp           decode instrumentation behind fif debug
tools/train_contexts.sh rebuilds src/ctxtable.inc from a training set
```

The CLI calls only the public API. Changes to the context layout require
retraining: run `tools/train_contexts.sh img1.png img2.png ...`, then rebuild.
A `static_assert` fails the build if the table and the context struct disagree.

## Documentation

| Document | Contents |
| --- | --- |
| [Getting started](https://fif2.org/lib-getting-started.html) | building, first encode and decode, measuring quality |
| [Using the fif tool](https://fif2.org/lib-cli.html) | every command and option, error messages |
| [Using the library](https://fif2.org/lib-api.html) | concepts, examples, function reference |
| [Design notes](docs/design.md) | how the codec works, with measurements |
| [The FIF2 format](https://fif2.org) | the open format; [binary specification](https://fif2.org/fif2-spec.html) |

## Contributing

Bug reports, files that decode wrongly and pull requests are welcome. For larger
changes, please open an issue first. Questions about the format itself belong to
[fif2.org](https://fif2.org).

## License

libfif and fif are released under the [MIT License](LICENSE). The release
binaries also contain libpng, libjpeg-turbo and zlib under their own permissive
licenses; see `THIRD-PARTY-NOTICES.txt` in each download.

## Credits

- **libfif and fif**: Oskar Behzadpour
- **FIF2 format**: Rezar Behzad

Copyright © 2026 Oskar Behzadpour.
New to the technique? [What is fractal image compression?](https://www.fractalimagecompression.com)
