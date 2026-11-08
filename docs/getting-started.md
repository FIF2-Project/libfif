# Getting started with FIF2

The FIF2 reference implementation consists of a library,
`libfif`, and a command-line tool, `fif`. This page
shows how to build them and how to convert your first picture.

## What you get

`fif` (`fif.exe` on Windows)
:   A command-line tool. It converts pictures to FIF2 and back, shows what a
    FIF2 file contains, and extracts its preview image.

`libfif`
:   A C library with the encoder and decoder, for programs that want to read
    or write FIF2 themselves. It has a plain C interface and can be used from
    C, C++ and any language that can call C functions.

The tool reads PNG, JPEG and PNM pictures and writes PNG, JPEG and PNM. PNG
and JPEG support requires the libraries libpng and libjpeg-turbo at build time.
Without them, the tool still works, but only with PNM files.

## Building

You need a C++20 compiler. The code is written for `clang++`;
`g++` works as well on Linux and macOS. There is no build for Microsoft's
compiler: the library itself is portable C++20 and has an MSVC path in its
SIMD layer, but `build.bat` and the makefile both drive a GCC-style command
line, and no `cl.exe` build is maintained or tested.

### Linux and macOS

```
# Ubuntu: compiler and optional image libraries
sudo apt install clang make pkg-config libpng-dev libjpeg-turbo8-dev

# Fedora
sudo dnf install clang make pkgconf-pkg-config libpng-devel libjpeg-turbo-devel

# macOS with Homebrew; jpeg-turbo must be made visible to pkg-config
brew install pkg-config libpng jpeg-turbo
export PKG_CONFIG_PATH="$(brew --prefix jpeg-turbo)/lib/pkgconfig:$PKG_CONFIG_PATH"

# in the source directory
make
```

The result is the tool `./fif` and the library
`./libfif.a`.

### Windows

1. Install the “C++ Clang tools for Windows” component of Visual Studio or
   of the Visual Studio Build Tools.
2. For PNG and JPEG support, install the image libraries with
   [vcpkg](https://vcpkg.io/):

   ```
   vcpkg install libpng libjpeg-turbo --triplet x64-windows-static
   ```

   The static triplet is deliberate: `clang++` links the static C runtime on
   Windows, so libraries built against the DLL runtime do not link.
   `build.bat` looks for the vcpkg tree in `%VCPKG_ROOT%`, then in
   `%USERPROFILE%\vcpkg`, then in `C:\vcpkg`. If yours is elsewhere, set
   `VCPKG_ROOT` before the build.
3. Run `build.bat` in the source directory.

The result is `fif.exe` and `libfif.lib`. The build
prints which image formats it found, for example
`image i/o png, jpeg, pnm`.

## Your first FIF2 file

```
fif encode photo.jpg photo.fif
fif decode photo.fif photo-decoded.png
```

The first command compresses `photo.jpg` with the default
settings. The second command reconstructs the picture and saves it as PNG. The
format of the output follows the file extension: `.png`,
`.jpg`, `.ppm` or `.pgm`.

The output name may be left out. `fif encode photo.jpg` writes
`photo.fif`; `fif decode photo.fif` writes the picture under the
name of the original, here `photo.jpg`. If that file already exists,
the tool asks before it overwrites it; `-y` answers yes in advance.

**Careful with `-y` here.** The name of the original is usually the original
photograph itself, sitting next to the FIF2 file. `fif decode photo.fif -y`
replaces it with the decoded, lossy version. Give an output name of your own
whenever the original is still around.

To see what the file contains:

```
fif info photo.fif
```

## Choosing the quality

Two settings control the result:

`-q`, quality, 0 to 100 (default 70)
:   Higher values give better pictures and larger files. Values between 50
    and 85 suit most photographs. FIF2 is at its strongest at low values, where
    other formats show heavy artefacts.

`-e`, effort, 0 to 9 (default 5)
:   How hard the encoder searches for matching blocks. Higher values take
    longer and find more usable matches, which raises the quality at a given
    quality setting and, because more matches are worth coding, usually makes
    the file slightly *larger* rather than smaller. Effort 2 is often close to
    5 and several times faster, but not identical, so compare if quality
    matters. Above 7 the gain stops: on many pictures effort 9 is no better
    than 7 and takes two to three times as long.

Do not expect a quality setting to mean a file size. Unlike JPEG, where a
given quality costs roughly a given number of bits per pixel, a fractal
encoder pays for each block with material from the picture itself, so the
size depends on how much of the picture repeats. Two photographs of the same
dimensions at the same quality can come out several times apart in size.
Trying it is the way: encode the picture and look. If a size or a quality is
fixed for you, hand the search to the encoder with `--target-bpp` or
`--target-psnr` below.

```
fif encode photo.jpg small.fif -q 40
fif encode photo.jpg best.fif  -q 90 -e 7
```

If you need a particular file size or quality instead, let the encoder find
the setting:

```
fif encode photo.jpg web.fif --target-bpp 0.5     # 0.5 bits per pixel
fif encode photo.jpg web.fif --target-psnr 32     # 32 dB
```

## Decoding at a different size

A FIF2 file can be decoded larger or smaller than it was coded:

```
fif decode photo.fif big.png --scale 2            # twice the size
fif decode photo.fif hd.png --size 1920x1080      # fill this box
fif decode photo.fif preview.jpg --width 400      # 400 pixels wide
```

## Getting file and picture information

```
fif info photo.fif
```

shows what a FIF2 file contains without decoding the picture, so it is
instant even for large files - unless the image data is stored compressed,
where the checksum can only be verified after unpacking: the picture size, the name and size of the
original, the quality it was encoded with, the stored previews, the metadata
such as camera, author and copyright, and whether the file is intact. The line
**checksums** reads *ok* for an undamaged file.

## Comparing your result

```
fif decode photo.fif check.png
fif compare photo.jpg check.png
```

`fif compare` measures how close the decoded picture is to the
original. It prints three numbers; for all of them, higher means closer.
**PSNR** is given in decibels: around 30 dB is a good picture, above 35 dB
differences are hard to see. **SSIM** and **MS-SSIM** run from 0 to 1 and
follow the eye more closely; above 0.98 the decoded picture is hard to tell
from the original at normal viewing size. Both pictures must have the same
size, so compare against the original only if it was not reduced with
`--max-dim` or a similar option.

`fif bench photo.jpg -q 60` does both steps at once: it encodes, decodes
and prints the size together with the three measures, without writing a
file. It is the quickest way to try out settings.

The next page, [Using the fif tool](cli.md), explains these
and all other options.
