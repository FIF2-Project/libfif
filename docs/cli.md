# Using the fif tool

The command-line tool `fif` converts pictures to
FIF2 and back and inspects FIF2 files. This page first shows common tasks and
how to find the best quality for the size, then lists every command and option
and explains the error messages.

Every command follows the same pattern: first the command, then the input
and output files, then the options.

```
fif <command> <input> [<output>] [options]
```

Help comes in three levels, so that nobody has to read more than they need:
`fif` alone only says where the help is; `fif help` lists the commands with a
few examples; `fif help decode` (or `fif decode --help`) lists all options of
one command with their defaults – the everyday ones first, grouped by what
they are for, and the ones for specialists at the end.

Options **must come after** the file names. `fif encode in.png
out.fif -q 80` works; `fif encode -q 80 in.png out.fif`
does not.

The output name is always optional. Without it, the result is written next to
the input:

| Command | Output without a name |
| --- | --- |
| `fif encode photo.jpg` | `photo.fif` |
| `fif decode photo.fif` | the name of the original picture stored in the file, e.g. `photo.jpg`; a format fif cannot write becomes `.png`, and a file without a stored name gives `photo.png` |
| `fif thumbnail photo.fif` | `photo-thumb.jpg` |

**No file is overwritten without asking.** If the output already exists, the
tool asks `Overwrite? [y/N]` before it starts the work. `-y` (or
`--overwrite`) answers yes in advance. In a script, where nobody can answer,
the tool stops with an error unless `-y` is given.

## Common tasks

### Compress a photograph

```
fif encode holiday.jpg
```

This writes `holiday.fif` with the default quality 70, which gives a good
picture for most photographs; `-q 50` gives a smaller file, `-q 85` a better
picture. The tool prints the size of the result and the bits per pixel.

### Make a small file for the web

```
fif encode holiday.jpg web.fif --max-dim 1600 -q 50 --thumbnail 200
```

`--max-dim 1600` reduces the picture so that its longer edge is at
most 1600 pixels before it is encoded. `--thumbnail 200` adds a JPEG
preview of 200 pixels, which a viewer can show before it has decoded the
picture. The original file name, the original size and the reduction factor
are recorded in the file.

### Add author and copyright

```
fif encode holiday.jpg holiday.fif --meta author="Ada Lovelace" --text copyright="(c) 2026 Ada Lovelace"
```

The camera's own data comes along by itself: from the EXIF block of a JPEG or
PNG the tool takes the camera, lens, date taken, exposure, author, copyright and
description, and it turns the picture upright if the camera stored it sideways.
The GPS position is never taken over. A `--meta` or
`--text` value with the same name replaces the camera's;
`--no-exif` ignores the EXIF data altogether.

Each `--meta` and `--text` option stores one named
value. You can use them as often as you like. The values may contain any
characters, including accents and non-Latin scripts.

### Decode at a larger size

```
fif decode holiday.fif poster.png --scale 3
```

The decoder carries out the fractal reconstruction at three times the
encoded resolution. This gives sharper edges than enlarging the normal decode in an
image editor. Factors of 2 to 3 give the best results; up to 16 is
possible.

### Decode to an exact size

```
fif decode holiday.fif wallpaper.png --size 2560x1440
```

The picture fills the box completely. If its shape differs from the box,
the excess is cut off equally on both sides; the picture is never stretched.
To keep the whole picture, give only one dimension:

```
fif decode holiday.fif wide.png --width 2560
```

### Decode only part of a picture

```
fif info holiday.fif --tiles                              # the tile map
fif decode holiday.fif detail.png --region 400x300+600+200
fif decode holiday.fif detail.png --region 400x300+600+200 --scale 4
fif decode holiday.fif some.png --tiles 0,5-6
```

A FIF2 picture is made of tiles that decode independently, so a part of
it costs only the tiles it touches. `--region` takes the rectangle in
the usual geometry notation: *width*x*height*+*left*+*top*,
in the pixels of the picture as `fif info` reports them. The result
is exactly the same as cutting the rectangle out of a full decode, only much
faster: on a 3840 × 2160 picture, a 512 × 512
region decoded four times larger took 0.09 s instead of 0.62 s for
the whole picture. `--region` combines with `--scale`,
`--width`, `--height` and `--size`, which then
refer to the region.

`--tiles` picks tiles by number, with ranges as in a print
dialog. The picture keeps its full size; tiles that are not listed stay grey.
`fif info … --tiles` shows where each tile is and which bytes of the
file it needs. The extents it prints are the visible ones, cut to the picture,
so the tiles of the last row and column are smaller than the tile size; the
coded tile geometry works on the padded size (specification 6.2).

### Get a quick preview

```
fif thumbnail holiday.fif                    # export the stored preview: holiday-thumb.jpg
fif thumbnail holiday.fif small.jpg --id 1   # preview 1, if there are several
fif decode holiday.fif preview.png --scale 1/4    # fast reduced decode
fif decode holiday.fif coarse.png --base-only     # the coarse picture only
```

A reduced decode with `1/2`, `1/4` or `1/8`
is much faster than a full decode, because the reconstruction itself runs at
the smaller size. The result is an approximation: `1/2` is almost
indistinguishable from a properly reduced picture, `1/8` is good
enough for a preview only.

`--base-only` decodes only the coarse picture: soft blocks without the fine
detail. It needs only the first part of the image data – the header states
how long that part is – so it is very fast and also works on a file that has
only partly arrived.

### Look inside a file

```
fif info holiday.fif
```

Example output:

```
file        holiday.fif
format      FIF2 v3.3
dimensions  1000 x 560, 3 channel(s)
size        102.3 KiB  (1.4965 bpp)
source      holiday.jpg, 1749 x 980, reduced by 1.749 before encoding
blocks      4..32, tile 256
chroma      4:2:0, shared
base layer  first 52.9 KiB of 102.3 KiB (51.7%) decodes to the coarse picture
tiles       12 (4 x 3), independently decodable
sections    chunks 523 B, thumbnail 16.6 KiB, fic 85.2 KiB, raw 0 B
checksums   fic ok, chunks ok, thumbnail ok
thumbnails  1
  [1] 333 x 186 JPEG, 16.6 KiB
chunks      11
  ENCO  encoder        "libfif v3.5"
  ORGN  source_name    "holiday.jpg"
  ORGN  src_size       2174395
  ENCO  credits        "libfif v3.5 - (c) 2026 Oskar Behzadpour
                        FIF2 format v3.3 by Rezar Behzad
                        www.FractalImageCompression.com"
  ORGN  transfer       "sRGB"
  ORGN  primaries      "sRGB"
  ORGN  author         "Ada Lovelace"
  ORGN  copyright      "(c) 2026 Ada Lovelace"
  ENCO  quality        70
  ENCO  effort         5
  DECO  deblock        60
```

Real output can hold a few lines this example does not show: `colour` for a
greyscale file, `remarks` when that header field is set, and, while encoding a
JPEG or PNG that carries camera data, a first line `exif N entries`.

The line **base layer** states how much of the file is needed for a
complete, coarse picture. The numbers in brackets under **thumbnails** are the
ones `fif thumbnail … --id` takes. The line **sections** shows the size of each part
of the file and whether it is stored compressed (*zlib*) or encrypted, in
both cases with the method the header names. The
line **checksums** shows whether the file is intact.

A part that is stored compressed has to be unpacked before anything inside it
can be read: the bytes the header locates as that part are inflated first,
and only then do the usual structures apply. The checksums work the same way
– they always cover the unpacked content – which is why `fif info` has to
unpack a compressed part before it can say whether it is intact.

### Compare two pictures

```
fif compare original.png decoded.png
```

Prints three common quality measures: PSNR in decibels, SSIM and MS-SSIM.
For all three, higher means closer to the original. As a rough guide, around
30 dB is a good picture and above 35 dB differences are hard to see; SSIM and
MS-SSIM above 0.98 are hard to tell from the original at normal viewing size.

### Try settings without writing a file

```
fif bench holiday.jpg -q 60 -e 2
```

`fif bench` encodes and decodes in one step and shows what the settings
give, without writing a file. Example output for a small picture, measured on
an AMD Ryzen 9 9950X (16 cores, 32 threads, AVX2):

```
using 24 of 32 threads
holiday.jpg              600 x 375  q=60 e=2 4:2:0 shared
  size      36.9 KiB   1.3447 bpp   (17.8 : 1)
  psnr      29.71 dB
  ssim      0.8973
  ms-ssim   0.9835
  encode    0.014 s   16.50 Mpixel/s
  decode    0.024 s   9.56 Mpixel/s   14 iterations
```

Read the two rates with the size of the picture in mind. A picture of a
quarter of a megapixel is over before the work has spread out: starting the
threads, reading the file and writing the result cost more than the coding
itself, so the megapixels per second come out low. The same machine and the
same settings on a 9.2-megapixel photograph reach **57.7 Mpixel/s** encoding
and **67.7 Mpixel/s** decoding.

**size** is the file the settings would produce, with the bits per pixel and
the compression ratio against the uncompressed picture. **psnr**,
**ssim** and **ms-ssim** measure the decoded picture against the
original, as `fif compare` does. **encode** and **decode** give the time and
the speed in million pixels per second. `bench` takes every encode option;
`--stats` adds the encoder's internal figures for specialists. The next
section uses it to find the best quality for a picture.

## Finding the best quality for the size

No single setting is best for every picture, but five steps find the best
one for yours in a few minutes. The numbers below come from a landscape
photograph reduced to 1920×1080. Other pictures give other numbers, but
the curve has the same shape.

### 1. Work at the size the picture will be shown

```
fif encode photo.jpg photo.fif --max-dim 1920
```

The number of pixels matters more than any quality setting. Half the width
and height means a quarter of the pixels and roughly a third of the file size.
Use the same `--max-dim` in all the following steps, so that what you
measure is what you will encode.

### 2. Measure the quality curve

`fif bench` encodes and decodes in one step, writes no file and
reports the size and the quality. Run it for several quality values:

```
for q in 20 35 50 65 75 85 95; do
  echo "q=$q"; fif bench photo.jpg --max-dim 1920 -q $q -e 2 --no-progress | grep -E 'size|psnr|ms-ssim'
done
```

| `-q` | Size | Bits per pixel | PSNR | MS-SSIM |
| --- | --- | --- | --- | --- |
| 20 | 37.8 KiB | 0.15 | 26.81 dB | 0.871 |
| 35 | 102.3 KiB | 0.40 | 29.28 dB | 0.940 |
| 50 | 177.8 KiB | 0.70 | 30.76 dB | 0.967 |
| 65 | 304.6 KiB | 1.20 | 31.57 dB | 0.981 |
| 75 | 379.3 KiB | 1.50 | 31.76 dB | 0.984 |
| 85 | 469.9 KiB | 1.86 | 31.86 dB | 0.986 |
| 95 | 551.7 KiB | 2.18 | 31.91 dB | 0.987 |

Look for the **knee**: the point after which more bytes buy hardly any
quality. From 35 to 50, 75 KiB more bring 1.5 dB. From 75 to 95, 172 KiB more
bring only 0.15 dB. A useful rule: **go higher only while 10% more size still
brings at least 0.1 dB.** Here the knee lies between 50 and 65.

The quality of a fractal code levels off at the top, so above about 75 you
mainly buy bytes. The numbers are a guide; look at the two candidates at the
knee before you decide. MS-SSIM follows the eye better than PSNR, and values
above 0.98 are hard to tell from the original at normal viewing size.

```
fif encode photo.jpg a.fif --max-dim 1920 -q 50 && fif decode a.fif a.png
fif encode photo.jpg b.fif --max-dim 1920 -q 65 && fif decode b.fif b.png
```

### 3. Choose the effort

The effort changes the time much more than the size. At quality 65:

| `-e` | Encode time | Size | PSNR |
| --- | --- | --- | --- |
| 0 | 0.04 s | 304.7 KiB | 30.88 dB |
| 2 | 0.12 s | 304.6 KiB | 31.57 dB |
| 5 (default) | 0.56 s | 304.6 KiB | 31.57 dB |
| 7 | 0.95 s | 311.9 KiB | 31.72 dB |
| 9 | 4.35 s | 319.4 KiB | 31.81 dB |

For photographs `-e 2` is usually the right choice: close to the default and
several times faster. How close depends on the picture – on this one the two
are equal, on others the default is a few tenths of a decibel better. Higher
effort pays off for textures, patterns and aerial pictures; compare
`-e 2` and `-e 7` with `fif bench` to find out.

The same options on a 9-megapixel photograph, measured with MS-SSIM as
well:

| `-e` | Size | PSNR | MS-SSIM | Encode |
| --- | --- | --- | --- | --- |
| 0 | 941.7 KiB | 30.83 dB | 0.9838 | 0.09 s |
| 2 | 962.0 KiB | 31.70 dB | 0.9842 | 0.18 s |
| 5 (default) | 1.02 MiB | 32.14 dB | 0.9855 | 1.43 s |
| 7 | 1.02 MiB | 32.31 dB | 0.9858 | 2.16 s |
| 9 | 1.00 MiB | 32.20 dB | 0.9850 | 6.10 s |

Two things to take from it: effort buys quality rather than size – the files
get slightly larger, not smaller – and the gain stops. Effort 9 came out
worse than 7 here while taking three times as long, so 7 is the highest
value worth trying.

### 4. Choose the colour resolution

| `--subsampling` | Size at quality 65 | Use for |
| --- | --- | --- |
| `420` (default) | 304.6 KiB | Photographs. The eye sees colour less sharply than brightness. |
| `444` | 368.4 KiB (+21%) | Screenshots, diagrams, coloured text, sharp colour edges. |
| `400` | 186.2 KiB (−39%) | Greyscale pictures, or when colour does not matter. |

### 5. Apply it to one picture or to many

- **Fixed quality** (`-q 55`): the same look for every
  picture; the size depends on the content. Right for most batches.
- **Fixed size** (`--target-bpp 1.0`): the file is as large as
  asked; the quality varies. Right when storage or bandwidth is fixed.
- **Fixed minimum quality** (`--target-psnr 31`): the smallest
  file that reaches it.

A target tries seven qualities, so use `-e 2` to keep it fast. The
tool tells you which quality it chose, and warns you if the target cannot be
reached:

```
$ fif encode photo.jpg photo.fif --max-dim 1920 --target-psnr 31 -e 2
using 6 of 8 threads
photo.fif: 1920x1080 -> 198.6 KiB (0.7847 bpp) in 0.933 s
target      31 dB -> quality 54, 31.00 dB

$ fif encode photo.jpg photo.fif --max-dim 1920 --target-psnr 34 -e 2
using 6 of 8 threads
photo.fif: 1920x1080 -> 583.6 KiB (2.3055 bpp) in 1.121 s
target      34 dB -> quality 100, 31.92 dB
fif: warning: 34 dB is not reachable for this image; the best result has 31.92 dB
```

The warning is useful in itself: this picture levels off just below 32 dB,
and anything above about 31 dB costs a lot of bytes for little gain.

**In short:** reduce to the display size with `--max-dim`;
measure with `fif bench` and pick the knee, usually between 50 and 70;
use `-e 2` for photographs; use `--subsampling 444` for
graphics and `400` for greyscale; fix `-q` for batches, or
use `--target-bpp` for a fixed budget.

## What the decoder adds after the fractal step

When the iteration has settled, `fif decode` does not hand over the bare
result. Four steps follow, each under the control of an option. The block seams
are filtered first, and the filter knows the quadtree: it sees where every
seam is, how large the blocks on either side are and whether they are flat,
planar or fractal, so it ramps widely between two smooth blocks, barely
touches a seam inside texture, and leaves a step that belongs to the picture
alone (`--deblock`, by default the strength the file recommends). Flat and
planar blocks are then bent so that they meet their smooth neighbours at the
corners, while each keeps the mean the file states for it
(`--no-smooth-dc` turns this off). For a 4:2:0 file, the half-resolution
colour planes are not merely interpolated: the decoder adds to the bilinear
chroma the luma detail the colour grid cannot carry, scaled by the local
slope of colour over brightness, so that colour edges follow brightness edges
(`--chroma-upsampling bilinear` for the plain interpolation). Last, the
rounding to 8 bits can dither, which removes banding from wide gradients
(`--dither`). None of this is part of the FIF2 format: another decoder may
do none of it, or something better, and the same file will then look slightly
different. That is deliberate - a better decoder improves pictures that were
encoded years ago, without anyone re-encoding them.

## Two options every command takes

| Option | Effect |
| --- | --- |
| `--quiet` | No banner and no status lines: the thread count, the EXIF count and the `downscaled` and `target` lines stay away. What remains is the one line that says what the command produced - the result of an encode, a decode or a thumbnail export - because a script that writes files wants to read that line. Warnings and errors still appear, and the normal output of `fif info`, `fif bench` and `fif compare` is untouched. |
| `--strict` | A warning makes the exit code 3 instead of 0. Nothing else changes: the command still does its work, and real failures still end with 1 or 2. |

Both may stand anywhere after the command, like `-y`, and both work with
every command. Together they make a pipeline readable: `--quiet` leaves only
the lines that mean something, and `--strict` lets the script see them in the
exit code. Status lines and the result line go to the standard output, so a
script can read them with a pipe; warnings and errors go to the error output,
with `--quiet` as without it.

```
fif encode photo.jpg web.fif -q 50 --quiet --strict || echo "look at photo.jpg"
```

## Command reference

| Command | Purpose |
| --- | --- |
| `fif encode <in> [<out.fif>]` | Compress a PNG, JPEG or PNM picture. Without an output name it writes `<in>` with the extension `.fif`. An output name without an extension gets `.fif` as well, so `fif encode photo.jpg web` writes `web.fif`. |
| `fif decode <in.fif> [<out>]` | Turn a FIF2 file back into a picture. The extension of `out` selects the format: `.png`, `.jpg`, `.jpeg`, `.ppm`, `.pgm`, `.pnm`. Without `out` it writes the name of the original picture stored in the file. |
| `fif info <in.fif> [--tiles]` | Show header, metadata, preview and attached files; with `--tiles` also the tile map. |
| `fif thumbnail <in.fif> [<out>]` | Export the preview stored in the file. To `.jpg` a JPEG preview is copied unchanged; other formats are converted. Without `out` it exports `<in>-thumb.jpg`. |
| `fif compare <a> <b>` | Measure the difference between two pictures of the same size. |
| `fif bench <in>` | Encode, decode and report quality and speed in one step. Takes the encode options, except `--base-only` and `--no-chunks`, which say how to store a file rather than how to encode it. |
| `fif debug <in.fif>` | For developers: stream every iteration of a decode, with the block structure, for debugging and analysis tools, or write one iteration as a picture. |
| `fif version` | Show the versions of the tool, the library and the file format, what the build supports, and the links to the source code and the format. `fif --version` prints the same table, for scripts that probe a tool that way. |

`fif version` prints a table:

```
  Component    Version   Description
  ---------    -------   -----------
  fif.exe      2.5       command-line tool
  libfif       3.5       library (SIMD avx2+fma, PNG, JPEG, zlib)
  FIF2         3.3       file format

  Source       https://github.com/FIF2-Project/libfif
  Format       https://fif2.org
  About        https://www.FractalImageCompression.com
```

The tool has its own version number, independent of the library's: `fif`
2.5 contains libfif 3.5, which reads and writes the FIF2 format 3.3. The name
in the first column is the name of the executable, so it reads `fif.exe` on
Windows and `fif` elsewhere.

`fif help <command>` lists every option of a command with its default value.

### Encode options

| Option | Default | Effect |
| --- | --- | --- |
| `-q`, `--quality N` | 70 | Quality, 0 to 100. Lower values give smaller files. |
| `-e`, `--effort N` | 5 | How hard the encoder searches, 0 to 9. 2 is much faster and almost as good; higher values take longer and help mainly with textures. |
| `--target-bpp X` | – | Choose the quality that gives X bits per pixel, i.e. a file size. |
| `--target-psnr X` | – | Choose the quality that gives X dB, i.e. a picture quality. |
| `--max-dim N` | – | Reduce the picture so that its longer edge is at most N pixels. Smaller pictures are not changed. |
| `--width N`, `--height N` | – | Reduce the picture to this width or height, keeping its shape. |
| `--downscale F` | – | Reduce the picture by the factor F, e.g. 2 for half the width and height. |
| `--subsampling S` | 420 | Colour resolution. `420`: half in each direction, as in JPEG; right for photographs. `444`: full, for graphics, screenshots and coloured text. `400`: no colour at all – the picture is stored as greyscale, and on a colour input the tool warns that the colour is being discarded. |
| `--thumbnail N` | none | Store a JPEG preview with a longer edge of N pixels, at most a third of the picture's longer edge. A larger value is reduced to that limit and the tool says so (`--thumbnail 2000 reduced to 1280 px, a third of the image's longer edge`). |
| `--meta KEY=VALUE` | – | Store a metadata entry, e.g. `--meta author="Ada Lovelace"`. Repeatable. |
| `--text KEY=VALUE` | – | Store a text entry, e.g. a copyright notice. Repeatable. |
| `--no-exif` | off | Ignore the camera data (EXIF) of the input: no camera metadata, and the picture is not turned upright. |
| `-y`, `--overwrite` | off | Replace an existing output file without asking. |
| `-t`, `--threads N` | 0 | Number of threads. 0 uses three quarters of the processor cores, or all of them on a machine with one or two. `fif encode`, `fif decode` and `fif bench` name the number they settled on (`using 6 of 8 threads`) before they start. |
| `--no-progress` | – | Hide the progress bar. It is only shown in a terminal anyway. |

**For specialists.** 

| Option | Default | Effect |
| --- | --- | --- |
| `--chroma M` | shared | How colour is coded. `shared`: follows the brightness structure. `from-luma`: predicted from brightness; decodes faster, files are slightly larger. |
| `--thumbnail-quality Q` | 85 | JPEG quality of the preview, 1 to 100. |
| `--misc KEY=VALUE` | – | Store an entry of type MISC, for anything that is neither metadata nor text. Repeatable. |
| `--chunk TYPE:KEY=VALUE` | – | Store an entry of a type of your own, 1 to 4 characters, e.g. `--chunk ACME:render=soft`. Other programs skip types they do not know. |
| `--compress LIST` | none | Store these parts of the file zlib-compressed: `chunks` (the entries), `thumbnails`, `fic` (the image data – the fractal image code), or `all` for all three, separated by commas. `raw` is not accepted. Worth it for long texts in the entries. Previews and image data are compressed already: `fic` usually comes out slightly *larger*, and compressed image data can no longer be fetched in parts by byte ranges, so `fic` and `all` are rarely a good idea. A section that did not shrink is reported, as is the loss of byte-range access. |
| `--remarks ABCD` | – | Set the four-character REMARKS field in the header, free for any use, e.g. to mark processed files. |
| `--no-credits` | off | Leave out the credits entry naming the authors and the website (about 130 bytes). Worth it only for very small files. |
| `--base-only` | off | Store only the coarse base layer, without the detail layers and without a preview. The file is much smaller and decodes to a blurred picture. Cannot be combined with `--target-psnr` or `--target-bpp`. |
| `--no-chunks` | off | Store no chunks at all (encoder, credits, everything about the source and your own `--meta`, `--text`, `--misc`, `--chunk`). Together with `--base-only` the file is the header plus the base layer. |
| `--tile N` | 0 | Edge of the independently decodable tiles, in pixels. 0 picks 512 for pictures above about two megapixels and 256 below that. Another value is rounded up to a multiple of `--max-block` times the chroma factor (2 with 4:2:0) – `--tile 300` becomes 320 at the default settings – and the tool says so. |
| `--min-block N`, `--max-block N` | 4, 32 | Smallest and largest block, powers of two from 2 to 256. |
| `--local-radius N`, `--kd-candidates N`, `--domain-shift N` | set by `-e` | Details of the search. |
| `--planar N` | 1 | 1: flat blocks may carry a gradient. 0: a single value only. |
| `--stats` | off | Print block counts and how the bits are divided. |
| `--progress` | – | Show the progress bar even when the output is not a terminal. |
| `--train-contexts F` | – | Write context statistics to F. Used to develop the codec itself. |

### Decode options

| Option | Default | Effect |
| --- | --- | --- |
| `--scale S` | 1 | `N` (2 to 16): decode N times larger, with real detail. `1/2`, `1/4`, `1/8`: fast, smaller preview. Other `1/N`: full decode, then reduced. |
| `--width W`, `--height H` | – | Output width or height; the other side follows the shape of the picture. |
| `--size WxH` | – | Output box. The picture fills it and is trimmed at the centre if necessary, never stretched. |
| `--region WxH+X+Y` | – | Decode only this rectangle, e.g. `400x300+600+200`. Only the tiles it touches are decoded; the pixels equal those of a full decode. |
| `--tiles LIST` | – | Decode only these tiles, e.g. `0,5-6`; the others stay grey. `fif info … --tiles` lists them. |
| `--base-only` | off | Decode only the coarse picture, without the fine detail. |
| `--deblock N` | from file | Smooth the block edges, 0 (off) to 100. `-1` uses the file's recommendation, which is what happens when the option is left out. Because the filter also runs inside the last rounds, switching it off changes how many rounds a decode needs, not only the final step. |
| `--dither` | off | Avoid banding in smooth gradients when rounding to 8 bits. |
| `-y`, `--overwrite` | off | Replace an existing output file without asking. |
| `-t`, `--threads N` | 0 | Number of threads. 0 uses three quarters of the processor cores, or all of them on a machine with one or two. `fif encode`, `fif decode` and `fif bench` name the number they settled on (`using 6 of 8 threads`) before they start. |
| `--no-progress` | – | Hide the progress bar. |

**For specialists.**

| Option | Default | Effect |
| --- | --- | --- |
| `--no-smooth-dc` | off | Paint flat areas as plain blocks instead of soft gradients. |
| `--chroma-upsampling M` | edge | 4:2:0 colour: `edge` follows the brightness edges, `bilinear` is the reference interpolation. |
| `--iterations N` | from file, else 16 | Maximum number of reconstruction rounds. |
| `--eps X` | from file, else 0.5 | Stop when no pixel changes by more than X between rounds. |
| `--relaxation X` | 1 | Step size of the rounds. 1 is the measured optimum; above about 1.2 the picture breaks up. |
| `--max-pixels N` | 456785920 | Above this many output pixels `fif` warns about the memory the decode will need and carries on. |
| `--progress` | – | Show the progress bar even when the output is not a terminal. |

### Thumbnail options

| Option | Default | Effect |
| --- | --- | --- |
| `--id N` | the largest | Which preview to export, if the file has several, counted from 1. `fif info` lists them with their numbers. |
| `-y`, `--overwrite` | off | Replace an existing output file without asking. |

### Debug options and the visualiser

`fif debug` takes the decode options `--scale` (N, 1/2, 1/4, 1/8),
`--iterations`, `--eps`, `--relaxation`, `--deblock`, `--base-only`,
`--no-smooth-dc`, `--chroma-upsampling`, `--dither` and `-t`, and:

| Option | Default | Effect |
| --- | --- | --- |
| `--iteration N` | – | Write iteration N as a picture instead of streaming; 0 is the coarse start, the last one the finished picture. |
| `-o`, `--out <file>` | `<in>-iterNN.png` | Where `--iteration` writes. |
| `--ref <image>` | – | The original picture: added to the stream, with PSNR and SSIM of every iteration against it. |
| `--prefix N` | the whole file | Decode only the first N bytes, as if the rest had not arrived. |
| `--only-tiles LIST` | all | Decode only these tiles, e.g. `0,3,5-8`, or `none`. |
| `--preview-max N` | 0 = full size | Cap the long edge of the streamed frames. |
| `--no-leaves` | off | Leave the block structure out. |

The stream is one JSON line followed by raw frames, meant for debugging and
analysis tools built against the same version of `fif`.

## When something goes wrong

Every message has the same form: one line that says what went wrong, and
indented hints that say what to do about it.

```
fif: error: cannot open 'holiday.png': no such file
  hint: the file is looked for in the current directory; give the full path if it is elsewhere
```

`fif: warning:` means the command ran, but perhaps not as you
expected. `fif: error:` means it stopped. Warnings and errors go to the error
output, the results of a command to the standard output, so the normal output
stays clean for scripts.

| Message (shortened) | What to do |
| --- | --- |
| `unknown command 'ecnode'` | The tool suggests the command you probably meant. |
| `unknown command 'photo.png'` | The command is missing: `fif encode photo.png out.fif`. |
| `options must come after the file names` | Write the files first, then the options. |
| `unknown option '--qualty'` | The tool suggests the closest option, or names the command the option belongs to. |
| `-q 150 is out of range; allowed is 0..100` | Every value is checked before anything is read; the hint explains the option. |
| `the output … is the input file` | Choose another output name. The tool never overwrites the picture it reads. |
| `'photo.jpg' already exists` | The output file exists. Interactively the tool asks; in a script it stops. Add `-y` to replace it, or choose another name - and check which file it is first: without an output name `fif decode` writes the name of the original picture. |
| `the image data … is encrypted` | Part of the file is password-protected. fif cannot decrypt; use the program that encrypted it. |
| `.bmp is not an image format fif writes` | Use one of the listed extensions. This is checked before decoding. |
| `the directory 'x' does not exist` | Create it first. This is checked before encoding, so no work is lost. |
| `'a.png' is named .png but contains a JPEG image` | Rename the file; the tool picks the reader by the extension. |
| `'a.webp' is a WebP image, which fif cannot read` | Convert it to PNG first. |
| `this fif cannot read PNG` | The tool was built without libpng (or libjpeg). Install it and build again. |
| `is a PNG image, not a FIF2 file` | Use `fif encode` for pictures, `fif decode` for FIF2 files. |
| `is not a FIF2 file` | The tool shows the first bytes. An interrupted download or a renamed file looks like this. |
| `uses format version 3.0` | An old development file. Encode the picture again from the original. |
| `is cut off` / `the header checksum does not match` | The file is incomplete or damaged. Copy or download it again. |
| `the image data does not decode` | Check with `fif info`; the embedded preview may still be usable. |
| `warning: … pixels: increased RAM usage` | The decode is larger than `--max-pixels`. It runs anyway; the line is there so a surprising memory load does not come silently. Decode smaller, or raise `--max-pixels`, to be rid of it. |
| `--region … reaches past the picture` | The hint gives the largest region that fits. |
| `there is no tile 40` | The message says how many tiles the file has; `fif info … --tiles` lists them. |
| `warning: … is not reachable for this image` | A `--target-bpp` or `--target-psnr` cannot be met. The file is the closest result. |

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Success. There may be warnings. |
| 1 | The command failed: a file is missing, cannot be read or written, is not a valid FIF2 file or is damaged, or an existing output file was not to be overwritten. |
| 2 | The command line is wrong: unknown command or option, missing or invalid value, options before the file names, an unusable output name. Nothing was read or written. |
| 3 | Only with `--strict`: the command succeeded, and warned while doing it. |

Without `--strict` a warning does not change the exit code: a decode of a
file that was cut off, or a `--target-psnr` that could not be reached, still
ends with 0. A script that has to tell a clean run from one with warnings
either adds `--strict` and tests for 3, or reads the error output.

## What the format allows and fif does not do yet

The specification describes a few things that this version of the tool does
not write or cannot read. A file stays valid either way; these are gaps in
the tool, not in the format.

- **Encryption.** The header can mark a compressed section as
  encrypted. `fif` neither encrypts nor decrypts. It writes no encrypted
  section, and on reading one it says so and stops rather than guessing:
  `fif info` reports the section as *encrypted*, and a decode of an
  encrypted picture ends with the advice to use the program that encrypted
  it. `fif info` also names the encryption method the header gives, so you
  can see what a locked file was encrypted with.
- **The method fields.** Version 3.3 of the format has a byte for the
  compression method and one for the encryption method of a file. `fif`
  writes `deflate` whenever it compresses, and `none` otherwise; it writes no
  encrypted file, so the encryption method is always `none` in what it
  produces. A file from another writer that names a method `fif` does not
  know keeps its unaffected parts readable: `fif info` reads the header and
  the sections that are stored plainly.
- **Raw data.** The format can carry arbitrary files next to the
  picture, for example the camera raw file. `fif` reads such a section and
  lists its entries, but has no option to write one. `--compress` therefore
  takes `chunks`, `thumbnails` and `fic`, but not `raw`.
- **More than one preview.** The thumbnail section holds any number of
  previews in any image format. `fif encode --thumbnail` writes exactly one,
  as JPEG. `fif thumbnail --id` can export any of them from a file that
  another writer produced.
- **Correction data.** Sixteen bytes of the header are held open for a
  later section of pixel-block corrections. No tool writes them yet; they are
  zero in every file `fif` produces.

## Things to know

- **Overwriting.** An existing file is only replaced after a yes at the
  prompt or with `-y`. Scripts that write the same file repeatedly need
  `-y`.
- **The output of an encode is always a `.fif` file.** Given an output name
  without an extension, `fif encode` appends `.fif` and then works with that
  name: the question whether the file exists, and the `-y` that answers it,
  apply to the name with the extension. A name with a different extension is
  still refused, because it would say that the file holds something it does
  not.
- **Banner.** Every run starts with three lines naming the tool, the
  library, the format and their authors. They go to the error output, so the
  normal output of `fif info` and `fif debug` stays clean for scripts.
  `--quiet` leaves them out.
- **Which output.** Status lines and the one-line result of a command go to
  the standard output; warnings, errors and the banner go to the error
  output. `fif encode photo.jpg --quiet` therefore prints exactly one line,
  on the standard output, and nothing else unless something went wrong.
- **Transparency.** FIF2 has no transparency. The alpha channel of a PNG
  is discarded when it is read.
- **Colour depth.** A PNG with 16 bits per channel is reduced to 8 bits
  automatically. A 16-bit PNM is refused. Convert such a file to
  PNG or to 8-bit PNM first.
- **Truncated files.** A file that was cut short still decodes, at lower
  quality. `fif info` shows *truncated* in that case, and its **base layer**
  line says whether the part needed for the coarse picture has arrived.
