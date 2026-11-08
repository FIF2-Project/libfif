# Using the library

This page is for developers who want to read or write FIF2
files in their own programs. It explains the concepts of the
`libfif` interface, gives a complete example, and lists the
functions.

## Setting up

The library has one header, `include/fif/fif.h`, and one library
file: `libfif.a` on Linux and macOS, `libfif.lib` on
Windows. The interface is plain C and can be called from C, C++ and other
languages with a C interface.

```
clang   -O2 -Iinclude -c myprogram.c
clang++ myprogram.o libfif.a -lpng -ljpeg -lz -lpthread -o myprogram
```

The library is written in C++20, so the final program must be linked with
a C++ compiler or with the C++ standard library. Add `-lpng`,
`-ljpeg` and `-lz` only if the library was built with PNG, JPEG and zlib
support.

## Five rules

1. **Every function that can fail returns a status.** `FIF_OK` (0) means
   success; every error is negative. `fif_status_string()` gives a readable
   description. The few functions that cannot fail return their answer
   directly: `fif_find_thumbnail()` an index or −1, `fif_raw_count()`,
   `fif_thread_count()` and `fif_cpu_cores()` a count, and `fif_psnr()`,
   `fif_ssim()` and `fif_ms_ssim()` a measured value.
2. **Always start from the defaults.** Call
   `fif_encode_options_defaults()` or
   `fif_decode_options_defaults()` before you set individual fields.
   New fields may be added in later versions; the defaults function gives them
   correct values.
3. **Pictures are 8-bit, interleaved, 1 or 3 channels.** A
   `fif_image` holds grey or RGB pixels row by row, with an explicit
   row stride.
4. **Free what the library allocates.** Encoded data is released with
   `fif_free()`, pictures with `fif_image_free()`.
5. **Views point into your buffer.** Functions that inspect a file
   (`fif_get_info`, `fif_get_chunk`,
   `fif_get_thumbnail`, `fif_get_raw`) return pointers
   into the data you passed. They stay valid as long as that data exists.
   A file whose sections are stored compressed has nothing to point into:
   `fif_get_chunk`, `fif_get_thumbnail`, `fif_get_raw` and
   `fif_get_tile` then return `FIF_ERR_UNSUPPORTED` for a compressed
   section. Call `fif_unpack` once and inspect the copy it returns.
   `fif_decode`, `fif_decode_thumbnail`, `fif_find_thumbnail` and
   `fif_raw_count` unpack by themselves.

## Example: encode and decode

```
#include <fif/fif.h>
#include <stdio.h>

int main(void)
{
    fif_image img = {0};
    fif_status st = fif_image_load("photo.png", &img);
    if (st != FIF_OK) {
        fprintf(stderr, "load: %s\n", fif_status_string(st));
        return 1;
    }

    /* Encode with quality 70 and a 200-pixel preview. */
    fif_encode_options eo;
    fif_encode_options_defaults(&eo);
    eo.quality        = 70;
    eo.thumbnail_size = 200;
    eo.source_name    = "photo.png";

    uint8_t *data = NULL;
    size_t size = 0;
    st = fif_encode(&img, &eo, &data, &size, NULL);
    fif_image_free(&img);
    if (st != FIF_OK) {
        fprintf(stderr, "encode: %s\n", fif_status_string(st));
        return 1;
    }

    /* Decode at 1920 pixels wide. */
    fif_decode_options dopt;
    fif_decode_options_defaults(&dopt);
    dopt.out_width = 1920;

    fif_image out = {0};
    st = fif_decode(data, size, &dopt, &out, NULL);
    if (st == FIF_OK)
        st = fif_image_save("photo-1920.png", &out);

    fif_image_free(&out);
    fif_free(data);
    return st == FIF_OK ? 0 : 1;
}
```

## Example: inspect a file

```
fif_info in;
if (fif_get_info(data, size, &in) == FIF_OK) {
    printf("%d x %d, from %.*s (%d x %d)\n", in.width, in.height,
           (int)in.source_name_len, in.source_name,
           in.source_width, in.source_height);
    if (in.fic_crc_ok == 0)
        printf("warning: picture data is damaged\n");
}

/* List all metadata entries. */
fif_chunk c;
for (int i = 0; fif_get_chunk(data, size, i, &c) == FIF_OK; ++i)
    if (c.value_type == FIF_VALUE_TEXT)
        printf("%s %.*s = %.*s\n", c.type, (int)c.name_len, c.name,
               (int)c.size, (const char *)c.data);

/* Show a preview of at least 256 pixels while the full picture decodes. */
fif_image preview = {0};
int k = fif_find_thumbnail(data, size, 256, "JPEG");
if (k >= 0 && fif_decode_thumbnail(data, size, k, &preview) == FIF_OK) {
    /* ... display preview ... */
    fif_image_free(&preview);
}
```

## Functions

### Encoding and decoding

`fif_status fif_encode(const fif_image *img, const fif_encode_options *opts, uint8_t **out_data, size_t *out_size, fif_stats *stats)`
:   Compresses a picture. On success, `*out_data` points to a new
    buffer of `*out_size` bytes; release it with
    `fif_free()`. `opts` and `stats` may be
    `NULL`.

`fif_status fif_decode(const uint8_t *data, size_t size, const fif_decode_options *opts, fif_image *out, fif_stats *stats)`
:   Reconstructs a picture. `out` receives a new picture; release
    it with `fif_image_free()`. A truncated file is not an error: the
    function returns the best picture the data allows.

### Inspecting a file

`fif_get_info(data, size, &info)`
:   Reads the header without decoding the picture: size, coding parameters,
    source name and size, the REMARKS field, the tile grid, section sizes,
    the two section bit fields (`compressed` and `encrypted`: which sections
    are stored compressed or need a password, `FIF_SECTION_*`), checksum status,
    the header's `detail_offset`, and the number of bytes needed for a coarse
    picture (`base_prefix_bytes`). Cheap and safe on untrusted data.

`fif_get_chunk(data, size, index, &chunk)`
:   Returns the metadata entry with the given index. Count up from 0 until
    the function returns `FIF_ERR_ARG`: that is how this function and
    `fif_get_thumbnail()` both report "there is no such index". Any other
    error is a real failure and not the end of the list -
    `FIF_ERR_ENCRYPTED` and `FIF_ERR_UNSUPPORTED` mean the section needs a
    password, or has to be unpacked with `fif_unpack()` first.

`fif_get_thumbnail(data, size, index, &thumb)`
:   Returns the stored preview with the given index without decoding it: its
    format code (`"JPEG"`), width, height, and a pointer to the
    complete image file. A file can hold any number of previews. Count up from 0
    until the function returns `FIF_ERR_ARG`.

`fif_find_thumbnail(data, size, min_edge, format)`
:   Returns the index of the smallest preview whose longer edge is at least
    `min_edge`, or of the largest if none is that big. A non-NULL
    `format` limits the choice to that format code. Returns −1 if
    there is no preview.

`fif_decode_thumbnail(data, size, index, &image)`
:   Returns a preview as pixels. Needs JPEG support in the build.

`fif_raw_count(data, size)`, `fif_get_raw(data, size, index, &entry)`
:   The number of attached files, and one of them: type, name, codec, the
    free-text `metadata` and the data (see the specification,
    section 7).

`fif_get_tile(data, size, index, &tile)`
:   Where tile `index` lies in the picture and where its base and
    detail layer lie in the file. With these byte ranges a client can fetch just
    the tiles of a region, for example with HTTP range requests, and decode them
    with `region_*` or `tile_list`.

`fif_unpack(data, size, &out, &out_size)`
:   A copy of the file with the compressed sections unpacked, released with
    `fif_free()`. Every inspecting function works on the copy. Needs
    zlib in the build for compressed files. Encrypted sections are copied as
    they are; the library cannot decrypt.

### Pictures and helpers

`fif_image_load(path, &img)`, `fif_image_save(path, &img)`
:   Read and write PNG, JPEG and PNM files. The format follows the file
    extension. When saving, an extension the build cannot write returns
    `FIF_ERR_UNSUPPORTED`.

`fif_image_alloc(&img, width, height, channels)`, `fif_image_free(&img)`
:   Allocate and release a picture.

`fif_image_downscale(&src, w, h, &dst)`
:   Reduce a picture with an exact area filter.

`fif_psnr`, `fif_ssim`, `fif_ms_ssim`
:   Quality measures between two pictures of the same size.

`fif_thread_count(threads)`, `fif_cpu_cores()`
:   How many threads an encode or decode would run with this `threads`
    option, and how many cores the machine reports (0 when it does not say).
    For a caller that wants to name the number before the work starts, rather
    than reproducing the rule behind `threads = 0`. The answer is the size of
    the thread pool; a parallel step with fewer items than that starts only as
    many threads as it has items.

## Important options

The tables below hold the fields most callers set, not the whole structure;
`fif.h` is the complete list. The encoder also takes `min_block`,
`max_block`, `tile_size`, `planar`, `chroma_mode`, `domain_shift`,
`local_radius` and `kd_candidates`, and the decoder takes `relaxation`, the
step size of the iteration that the tool exposes as `--relaxation`.

### fif\_encode\_options

| Field | Default | Meaning |
| --- | --- | --- |
| `quality` | 70 | 0 to 100. |
| `effort` | 5 | 0 to 9. |
| `subsampling` | 4:2:0 | `FIF_SUBSAMPLING_444`, `_420` or `_400`. |
| `target_bpp`, `target_psnr` | 0 (off) | Search the quality that reaches this value. |
| `threads` | 0 | 0 = three quarters of the processor cores; all of them on one or two. |
| `thumbnail_size` | 0 | Longer edge of the JPEG preview; 0 = none. The encoder writes at most one preview; the format allows more. |
| `thumbnail_quality` | 0 (= 85) | JPEG quality of the preview. |
| `source_name`, `source_width`, `source_height`, `source_scale` | unknown | Origin of the picture, stored in the file (the name as the chunk `ORGN source_name`). The library cannot know these; set them if you reduced the picture before encoding. |
| `chunks`, `chunk_count` | none | Additional chunks. The type may be one of the predefined ones (`ORGN` for anything from the source picture, `META`, `MISC`, …) or a four-character type of your own for information only your decoder reads. |
| `source_bytes` | 0 | Size of the source file in bytes, stored as the `ORGN src_size` chunk. 0 = unknown, no chunk. |
| `compress` | 0 | Sections to store zlib-compressed: `FIF_SECTION_CHUNKS`, `FIF_SECTION_THUMBNAILS`, `FIF_SECTION_FIC`, combined with `\|`. Worth it for long texts in the chunks. Needs zlib in the build. |
| `remarks` | zeros | Four characters for the header field REMARKS, free for any use. |
| `omit_credits` | 0 | 1 = leave out the `ENCO credits` chunk naming the authors of encoder and format and the website (about 130 bytes). |
| `base_only` | 0 | 1 = write the base layers only; the detail layers are left out and the directory records them as empty. The file decodes to the coarse picture and is much smaller; no thumbnail is stored. Not combinable with `target_psnr` or `target_bpp` (`FIF_ERR_ARG`). |
| `omit_chunks` | 0 | 1 = write no chunks at all (no `ENCO`, `ORGN` or `DECO` entries, and the caller's chunks, source name and credits are dropped). |
| `progress`, `progress_user` | none | Callback that receives the progress from 0 to 1. |

### fif\_decode\_options

| Field | Default | Meaning |
| --- | --- | --- |
| `scale_num`, `scale_den` | 1, 1 | Render at `scale_num / scale_den` of the coded size. `scale_num` 1 to 16. |
| `out_width`, `out_height` | 0 | Output size in pixels. One of them keeps the shape; both fill the box and trim the excess. |
| `region_x`, `region_y`, `region_w`, `region_h` | 0 | Decode only this rectangle, in coded pixels. Only the tiles it touches are decoded, and its pixels equal those of a full decode. The output is the rectangle, scaled; `out_width`/`out_height` then refer to the rectangle. |
| `tile_list`, `tile_list_len` | NULL | Decode only these tiles; the others stay mid-grey. |
| `deblock` | −1 | Edge filter strength; −1 = recommendation in the file. |
| `smooth_dc` | 1 | Flat and planar blocks bent to meet their neighbours, block means kept; 0 = single values. |
| `chroma_bilinear` | 0 | 4:2:0 colour: 0 = edge-aware (follows the luma edges), 1 = the reference bilinear interpolation. |
| `dither` | 0 | 1 = ordered dither before rounding to 8 bits, against banding in smooth gradients. |
| `base_only` | 0 | 1 = decode only the base layers: the coarse picture without the fractal detail. |
| `max_iterations`, `convergence` | 0 | Maximum reconstruction rounds and stop threshold. 0 = use the file's recommendation, else the defaults 16 and 0.5. |
| `max_pixels` | 0 (= `FIF_DEFAULT_MAX_PIXELS`, 456785920) | Safety limit for the decoded size. |
| `threads` | 0 | 0 = three quarters of the processor cores; all of them on one or two. |

## Errors

| Status | Meaning |
| --- | --- |
| `FIF_OK` | Success. |
| `FIF_ERR_ARG` | Invalid argument, or the requested item does not exist. |
| `FIF_ERR_IO` | A file could not be read or written. |
| `FIF_ERR_FORMAT` | The data is not a valid FIF2 file, or is from an unsupported format version. |
| `FIF_ERR_NOMEM` | Out of memory. |
| `FIF_ERR_UNSUPPORTED` | Valid request that this build cannot serve, e.g. JPEG without libjpeg, or an enlargement beyond 16×. |
| `FIF_ERR_TOO_LARGE` | The decoded picture would exceed `max_pixels`. |
| `FIF_ERR_ENCRYPTED` | A section the call needs is encrypted. Unpacking it needs a password; the library cannot decrypt. |

## Threads

Encoding and decoding use several threads internally. The functions can also
be called from several threads at the same time, each on its own data. The one
exception is the context training interface
(`fif_context_training_*`), which is meant for building the codec
itself and is not thread-safe.
