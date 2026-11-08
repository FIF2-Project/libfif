/* ------------------------------------------------------------------------
 * libfif v3.5 / fif v2.5 - FIF2 fractal image format
 *
 * libfif and fif:  Oskar Behzadpour
 * FIF2 format:     Rezar Behzad
 * Web:             www.FractalImageCompression.com
 * Contact:         oskar@fractalimagecompression.com
 *
 * Copyright (c) 2026 Oskar Behzadpour
 * ---------------------------------------------------------------------- */

/* libfif - fractal image format, public C API */
#ifndef FIF_H
#define FIF_H

#include <stddef.h>
#include <stdint.h>

/* FIF_API marks the functions libfif exports.
 * Building the shared library: define FIF_BUILD_SHARED (the build scripts do).
 * Using libfif, static or shared: nothing to define.  On Windows, FIF_USE_DLL
 * may be defined to import the functions from libfif.dll directly. */
#if defined(FIF_BUILD_SHARED)
#  if defined(_WIN32)
#    define FIF_API __declspec(dllexport)
#  else
#    define FIF_API __attribute__((visibility("default")))
#  endif
#elif defined(FIF_USE_DLL) && defined(_WIN32)
#  define FIF_API __declspec(dllimport)
#else
#  define FIF_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define FIF_VERSION_MAJOR 3
#define FIF_VERSION_MINOR 5

typedef enum {
    FIF_OK = 0,
    FIF_ERR_ARG = -1,
    FIF_ERR_IO = -2,
    FIF_ERR_FORMAT = -3,
    FIF_ERR_NOMEM = -4,
    FIF_ERR_UNSUPPORTED = -5,
    /* The stream decodes to more pixels than fif_decode_options.max_pixels
     * allows.  Distinct from UNSUPPORTED because the caller can do something
     * about it: raise the limit, or decide the file is a bomb and stop. */
    FIF_ERR_TOO_LARGE = -6,
    /* A section the call needs is stored encrypted.  Unpacking it needs a
     * password, and libfif cannot decrypt. */
    FIF_ERR_ENCRYPTED = -7
} fif_status;

/* The sections of the file, as bits of the header's two section bit fields.
 * A COMPRESSED section has to be unpacked before anything in it can be read;
 * an ENCRYPTED one (only ever set together with COMPRESSED) needs a password
 * to unpack.  fif_info.compressed and .encrypted report the two bytes;
 * fif_encode_options.compress takes the sections to pack. */
#define FIF_SECTION_CHUNKS     0x01
#define FIF_SECTION_THUMBNAILS 0x02
#define FIF_SECTION_FIC        0x04
#define FIF_SECTION_RAW        0x08

/* How the marked sections are packed and locked: one of each per file, in
 * fif_info.compress_method and .encrypt_method.  Which number means which
 * method belongs to the file format version, so a later version may add
 * values; this library packs with deflate and cannot decrypt at all. */
#define FIF_COMPRESS_NONE      0
#define FIF_COMPRESS_DEFLATE   1
#define FIF_ENCRYPT_NONE       0
#define FIF_ENCRYPT_BLOWFISH   1

/* 4:0:0 throws the colour away and codes luma only - the chroma planes are
 * never formed, so it is both the smallest and the fastest mode. */
typedef enum {
    FIF_SUBSAMPLING_444 = 0,
    FIF_SUBSAMPLING_420 = 1,
    FIF_SUBSAMPLING_400 = 2
} fif_subsampling;

/* SHARED: chroma inherits the luma quadtree, domain index and symmetry; only
 * DC and contrast are coded per plane.  FROM_LUMA: chroma is predicted from
 * the reconstructed luma plane - the co-located luma block is the domain, so no
 * index and no symmetry are coded at all, and the decoder needs no chroma
 * iteration: once luma has reached its fixed point, one pass paints Cb and Cr. */
typedef enum {
    FIF_CHROMA_SHARED = 0,
    FIF_CHROMA_FROM_LUMA = 1
} fif_chroma_mode;


/* Progress reporting.  Called with a monotone fraction in [0,1] as work
 * completes; it is called from whichever thread finished the piece of work, but
 * never from two at once, so a callback that only touches its own `user` data
 * needs no locking of its own.  Null disables it.
 *
 * Reporting only - there is deliberately no way to say "stop" from here.  A
 * cancel that has to unwind a thread pool mid-tile is a different feature with
 * different failure modes, and pretending a progress hook provides it would be
 * worse than not offering it. */
typedef void (*fif_progress_fn)(void *user, double fraction);

/* ---- chunks ----
 * A chunk is one named value: type (what kind of information), name, value.
 * There may be any number of chunks of one type.  Nothing a decoder needs is
 * in a chunk - those values live in the header - so a reader may skip every
 * chunk it does not understand.
 *
 * Types: "ENCO" encoder settings, "ORGN" everything that comes from the
 * source picture - its file name and size, its camera data, how its colours
 * are to be read - "DECO" decoder recommendations, "META" metadata that is
 * not from the source, "TEXT" free text such as copyright, "POST"
 * postprocessing, "MISC" anything else.  The type is not a closed list:
 * an encoder may use a four-character type of its own for information only
 * its own decoder understands; every other reader skips it.  The value type
 * says how to read the bytes; numbers are little-endian and 1, 2, 4 or 8
 * bytes wide (floats 4 or 8). */
typedef enum {
    FIF_VALUE_BINARY = 0,
    FIF_VALUE_UINT = 1,
    FIF_VALUE_INT = 2,
    FIF_VALUE_FLOAT = 3,
    FIF_VALUE_TEXT = 4          /* UTF-8, not NUL-terminated */
} fif_value_type;

typedef struct {
    char type[5];               /* four characters plus NUL */
    fif_value_type value_type;
    const char *name;           /* UTF-8; on input NUL-terminated if name_len is 0 */
    size_t name_len;
    const uint8_t *data;
    uint64_t size;
    int crc_ok;                 /* output only: the chunk's checksum matched */
} fif_chunk;

typedef struct {
    int quality;            /* 0..100 (default 70), sets the RD multiplier   */
    int effort;             /* 0..9, search depth                            */
    fif_subsampling subsampling;
    fif_chroma_mode chroma_mode;
    int tile_size;          /* 0 = auto (256/512 by image size)              */
    /* Range block edges.  Powers of two, 2 <= min_block <= max_block <= 256;
     * anything else is FIF_ERR_ARG. */
    int min_block;          /* default 4                                     */
    int max_block;          /* default 32                                    */
    int threads;            /* 0 = 3/4 of the cores (all of 1 or 2)          */
    double target_psnr;     /* <= 0 disabled; otherwise bisect quality       */
    double target_bpp;      /* <= 0 disabled; otherwise bisect quality       */
    /* Search shape. -1 takes the value implied by `effort`.  These affect only
     * how hard the encoder looks, never how the result is decoded. */
    int local_radius;
    int kd_candidates;
    /* Domain grid density: step = (2*block) >> shift, 0..4.  Denser grids
     * offer better matches and cost more bits per index.  < 0 takes the
     * effort preset. */
    int domain_shift;
    /* Let a leaf with no transform carry a linear gradient rather than a single
     * mean.  1 = on (default), 0 = off. */
    int planar;
    fif_progress_fn progress;
    void *progress_user;
    /* Embedded JPEG preview.  0 = none (default).  Otherwise the longest edge
     * of the thumbnail in pixels; capped at a third of the longest edge of the
     * encoded image.  Needs libjpeg in the build, else FIF_ERR_UNSUPPORTED. */
    int thumbnail_size;
    int thumbnail_quality;  /* JPEG quality 1..100, 0 = 85                   */
    /* Where the image came from, stored in the header.  The library never sees
     * the file, so the caller fills these in.  NULL / 0 = unknown, in which
     * case the source size is the encoded size and the scale is 1. */
    const char *source_name;
    int source_width, source_height;
    double source_scale;    /* factor the source was reduced by, >= 1       */
    /* Size of the source file in bytes, for the ORGN src_size chunk - what
     * the picture cost before it was encoded, so a reader can state the
     * ratio without the original at hand.  0 = unknown, no chunk. */
    int64_t source_bytes;
    /* Additional chunks to store, e.g. META or TEXT entries. */
    const fif_chunk *chunks;
    int chunk_count;
    /* Sections to store zlib-compressed: FIF_SECTION_CHUNKS,
     * _THUMBNAILS and _FIC, or'ed.  0 = none (default).  Worth it for chunks
     * with large text or ICC profiles; the thumbnails and the FIC data are
     * compressed already and gain little, and a compressed FIC section can
     * no longer be fetched in parts by byte range (fif_get_tile).  Needs
     * zlib in the build, else FIF_ERR_UNSUPPORTED; FIF_SECTION_RAW is
     * FIF_ERR_ARG, since libfif writes no raw section. */
    int compress;
    /* Four characters for the header's REMARKS field, free for any use - a
     * tool may mark files it has seen or processed.  NUL bytes = none. */
    char remarks[4];
    /* 1 = leave out the ENCO chunks naming the encoder's and the format's
     * authors and the website (about 130 bytes).  The ENCO encoder chunk
     * with the version is always written. */
    int omit_credits;
    /* 1 = write the base layers only: the detail layers are left out and the
     * directory records them as empty.  The file decodes to the coarse picture
     * (as with fif_decode_options.base_only) and is much smaller.  No
     * thumbnail is stored either.  Not combinable with target_psnr or
     * target_bpp (FIF_ERR_ARG). */
    int base_only;
    /* 1 = write no chunks at all: no ENCO, ORGN or DECO entries, and the
     * caller's chunks, source name, source size and credits are dropped.  The
     * file is the header plus the image data (and a thumbnail if asked for). */
    int omit_chunks;
} fif_encode_options;

/* The decoder's own iteration cap and convergence threshold.  Precedence for
 * both: the caller's value if it is positive, else the stream's recommendation
 * (a DECO chunk) if it is present and positive, else these defaults. */
#define FIF_DEFAULT_MAX_ITERATIONS 16
#define FIF_DEFAULT_CONVERGENCE 0.5

/* The decoder's pixel limit when the caller does not set one.  The header is
 * attacker-controlled: a 144-byte header can claim a trillion pixels, so the
 * size it implies is checked before a single plane is allocated.  The value
 * bounds that allocation to a few gigabytes - the decoder holds the picture
 * as float planes while it iterates - and still clears any real photograph
 * by a wide margin. */
#define FIF_DEFAULT_MAX_PIXELS 350000000

typedef struct {
    int max_iterations;     /* > 0 wins over the stream; 0 = not set (default) */
    double convergence;     /* max pixel delta to stop at; 0 = not set        */
    /* Step scaling of the fixed-point iteration: a += relaxation * (T(a) - a).
     * 1 applies the map as it is.  Above 1 extrapolates past the step, below
     * 1 damps it.  The
     * fixed point does not depend on it - where T(a) == a, no value of it
     * moves anything - so only the number of passes changes, not the picture.
     * Not clamped: somewhere above 2 the iteration diverges and the output is
     * rubbish.  Zero or less means the default, not "off". */
    double relaxation;
    int threads;            /* 0 = 3/4 of the cores (all of 1 or 2)          */
    /* Decode at scale_num/scale_den, default 1/1.  scale_num may be any integer
     * 1..16: the attractor is rendered at that multiple, which is exact.  A
     * power-of-two scale_den (2, 4, 8) iterates natively at the smaller size;
     * any other scale_den decodes at full size and filters down. */
    int scale_num;
    int scale_den;
    /* Deblocking strength 0..100. -1 takes the value the encoder recommended
     * in the stream; 0 disables the filter. */
    int deblock;
    /* Refuse streams whose decoded size would exceed this many pixels, after
     * padding and scaling.  0 takes FIF_DEFAULT_MAX_PIXELS (457 megapixels).
     * The header is attacker-controlled, so the allocation it implies has to
     * be bounded before anything is allocated. */
    int64_t max_pixels;
    fif_progress_fn progress;
    void *progress_user;
    /* Output size in pixels; 0 = not asked for.  With only one of the two set
     * the other follows from the aspect ratio.  With both set and a different
     * aspect, the picture is scaled to cover the box and the excess is cropped
     * from the centre - never stretched.  The attractor is rendered at the
     * smallest integer multiple (at most 16) that reaches the size and then
     * area-filtered down.  Overrides scale_num/scale_den. */
    int out_width;
    int out_height;
    /* Decode only part of the picture.  Tiles are independent, so the work
     * and the memory scale with the part, not with the whole.
     *
     * region_*: a rectangle in coded pixels (the size fif_get_info reports),
     * before any scaling.  region_w or region_h 0 = the whole picture.  The
     * output is the rectangle, scaled, and its pixels are exactly those of a
     * full decode: the tiles the seam filter reaches into are decoded too.
     *
     * tiles: indices of the tiles to decode, tile_list_len of them; null =
     * all.  Tiles not in the list stay mid-grey.  With a region as well, the
     * list further limits which of the region's tiles are decoded.
     *
     * An index past the last tile or a rectangle outside the picture is
     * FIF_ERR_ARG. */
    int region_x, region_y, region_w, region_h;
    const int *tile_list;
    int tile_list_len;
    /* Flat blocks as soft surfaces instead of single values: 1 = on (the
     * default), 0 = off.  Removes the staircase in skies and skin at low
     * rates; the block means stay the coded ones. */
    int smooth_dc;
    /* 4:2:0 chroma: 0 = edge-aware, following the luma edges (default);
     * 1 = plain bilinear interpolation, the reference upsampling. */
    int chroma_bilinear;
    /* 1 = ordered dither below one grey level before rounding to 8 bits:
     * smooth gradients show no bands, at the cost of a fine pattern and a
     * few hundredths of a dB.  0 = off (default). */
    int dither;
    /* 1 = decode the base layers only: the coarse picture, block means and
     * gradients without the fractal detail.  Reads only the FIC bytes up to
     * the header's detail offset.  0 = everything (default). */
    int base_only;
} fif_decode_options;

typedef struct {
    double encode_seconds;     /* filled by the encoder */
    double decode_seconds;     /* filled by the decoder */
    double search_seconds;
    int64_t blocks_total;
    int64_t blocks_flat;
    int64_t blocks_fractal;
    int64_t candidate_evals;   /* exact (range,domain,symmetry) evaluations */
    int64_t compressed_bytes;
    double bits_per_pixel;
    int iterations_used;       /* filled by the decoder */
    /* Distortion of the collage the encoder chose, before any decoder
     * iteration.  It isolates search quality from the fixed-point dynamics. */
    double collage_psnr;
    /* Where the bits went, in the range coder's own units. */
    double bits_split, bits_mode, bits_dc, bits_domain, bits_sym, bits_contrast;
    double bits_planar, bits_grad;
} fif_stats;

/* 8-bit interleaved image, channels is 1 (gray) or 3 (RGB). */
typedef struct {
    uint8_t *pixels;
    int width;
    int height;
    int channels;
    size_t stride;          /* bytes per row */
} fif_image;

FIF_API void fif_encode_options_defaults(fif_encode_options *opts);
FIF_API void fif_decode_options_defaults(fif_decode_options *opts);

FIF_API fif_status fif_image_alloc(fif_image *img, int width, int height, int channels);
FIF_API void fif_image_free(fif_image *img);

/* Encodes into a freshly malloc'd buffer; caller releases it with fif_free. */
FIF_API fif_status fif_encode(const fif_image *img, const fif_encode_options *opts,
                      uint8_t **out_data, size_t *out_size, fif_stats *stats);

FIF_API fif_status fif_decode(const uint8_t *data, size_t size,
                      const fif_decode_options *opts,
                      fif_image *out_img, fif_stats *stats);

/* Header inspection without decoding the image body. */
typedef struct {
    int width, height, channels;
    int tile_size, min_block, max_block;
    int subsampling, chroma_mode;
    int quality, effort;
    int64_t tile_count;
    /* Length of the prefix of the file that decodes to a complete coarse
     * picture: everything up to the detail layers.  Everything after it
     * refines that picture.  0 when the FIC section is compressed, since no
     * prefix of a packed section can be decoded on its own. */
    int64_t base_prefix_bytes;
    /* The header's detail offset: where the detail layers start, relative to
     * the start of the unpacked FIC section. */
    int64_t detail_offset;
    /* Container. */
    int version_major, version_minor;
    int64_t file_size;          /* as declared in the header */
    int truncated;              /* the buffer is shorter than file_size */
    const char *source_name;    /* points into the stream, not NUL-terminated */
    size_t source_name_len;
    int source_width, source_height;
    double source_scale;
    int thumbnail_count;        /* 0 = none */
    int chunk_count;
    int raw_count;              /* from the raw section; 0 when it is compressed */
    char remarks[5];            /* REMARKS field plus NUL; "" when unset    */
    /* The header's two section bit fields, FIF_SECTION_* or'ed.  For a
     * compressed section fif_get_chunk, fif_get_thumbnail, fif_get_raw and
     * fif_get_tile return FIF_ERR_UNSUPPORTED, since what they hand out points
     * into the buffer; read them from the copy fif_unpack makes.  source_name
     * is null for compressed chunks.  A bit in `encrypted` is never set
     * without the same bit in `compressed`. */
    int compressed, encrypted;
    /* How the marked sections are packed and locked, for the whole file:
     * FIF_COMPRESS_* and FIF_ENCRYPT_*.  A value this library does not know
     * is passed on as it stands, so a caller can name what a file it cannot
     * open was made with. */
    int compress_method, encrypt_method;
    int chunks_compressed, raw_compressed;  /* the chunk and raw bits */
    int tiles_x, tiles_y;       /* the tile grid, in raster order           */
    int64_t chunk_bytes, thumbnail_bytes, fic_bytes, raw_bytes;
    /* 1 = checksum matched, 0 = mismatch, -1 = section absent, incomplete or
     * not readable (encrypted).  Every one is over the unpacked section.
     * thumbnail_crc_ok covers the whole thumbnail section; each thumbnail
     * also has its own (fif_thumbnail.crc_ok), and each chunk too. */
    int thumbnail_crc_ok, fic_crc_ok, raw_crc_ok;
    int chunk_crc_ok;
} fif_info;

FIF_API fif_status fif_get_info(const uint8_t *data, size_t size, fif_info *info);

/* One tile: where it is in the picture (coded pixels, clipped to the
 * picture) and where its two layers are in the file.  An offset of -1 means
 * the layer is not in the buffer (a truncated file).  Enough to fetch a
 * region over HTTP range requests without reading the rest. */
typedef struct {
    int x, y, w, h;
    int64_t base_offset, base_size;
    int64_t detail_offset, detail_size;
} fif_tile;
FIF_API fif_status fif_get_tile(const uint8_t *data, size_t size, int index, fif_tile *out);

/* A copy of the file with the compressed sections unpacked and their bits in
 * the header's compressed field cleared, freed with fif_free.  Every
 * accessor works on the copy; for a file without compressed sections it is a plain copy.  An
 * encrypted section is copied as it is, still encrypted. */
FIF_API fif_status fif_unpack(const uint8_t *data, size_t size, uint8_t **out_data, size_t *out_size);

/* The index-th chunk, in file order.  Pointers point into `data`.  An index
 * past the last chunk returns FIF_ERR_ARG. */
FIF_API fif_status fif_get_chunk(const uint8_t *data, size_t size, int index, fif_chunk *out);

/* A file may carry any number of thumbnails, each a complete image file of
 * its own, stored unchanged, in any size and format.  The format is a
 * four-character code: "JPEG" is the only one the encoder writes; others
 * ("PNG ", "WEBP", ...) need no change to the container.  A reader that does
 * not know a code ignores that thumbnail. */
typedef struct {
    char format[5];             /* four characters plus NUL, e.g. "JPEG"      */
    int width, height;          /* from the file for JPEG, else from the
                                   directory; 0 if not known                  */
    const uint8_t *data;        /* the stored file                             */
    uint64_t size;
    int crc_ok;
} fif_thumbnail;

/* The index-th thumbnail, in file order, without decoding it.  FIF_ERR_ARG
 * past the last one; FIF_ERR_FORMAT if it is damaged or cut off. */
FIF_API fif_status fif_get_thumbnail(const uint8_t *data, size_t size, int index, fif_thumbnail *out);
/* The index of the smallest thumbnail whose longer edge is at least
 * `min_edge`, or of the largest when none is that big.  `format` restricts
 * the choice to one code ("JPEG"); NULL takes any.  -1 when there is none. */
FIF_API int fif_find_thumbnail(const uint8_t *data, size_t size, int min_edge, const char *format);
/* The index-th thumbnail decoded to pixels.  JPEG only, and needs libjpeg in
 * the build; other formats return FIF_ERR_UNSUPPORTED. */
FIF_API fif_status fif_decode_thumbnail(const uint8_t *data, size_t size, int index, fif_image *out);

/* One entry of the raw-data section: a file in another format (a camera raw,
 * the original JPEG, ...) carried alongside the fractal code.  The encoder
 * never writes these; the format and this reader support them. */
typedef struct {
    char file_type[5];          /* four characters plus NUL, e.g. "DNG " */
    const char *file_name;  size_t file_name_len;
    uint64_t file_size;         /* size of the original file */
    char file_codec[5];         /* how data is stored: "NONE", "ZLIB", ... */
    const char *metadata;  size_t metadata_len;  /* free description, UTF-8 */
    const uint8_t *data;
    uint64_t data_size;         /* declared */
    uint64_t data_avail;        /* present; less than data_size if truncated */
    int complete;
} fif_raw_entry;

FIF_API fif_status fif_get_raw(const uint8_t *data, size_t size, int index, fif_raw_entry *out);
/* Number of raw entries as the raw section states it; -1 when the section is
 * encrypted or damaged. */
FIF_API int fif_raw_count(const uint8_t *data, size_t size);

/* Collects the adapted context statistics of every tile encoded from now on,
 * and writes the accumulated table.  Used to build the codec's default
 * probabilities; not part of normal encoding. */
FIF_API void fif_context_training_begin(void);
FIF_API int  fif_context_training_write(const char *path);

FIF_API void fif_free(void *p);
FIF_API const char *fif_status_string(fif_status s);

/* Name of the SIMD kernel set this library was compiled for: "neon",
 * "avx2+fma", "avx2", "avx+fma", "avx", "sse2+fma", "sse2" or "scalar".
 * The kernels are encoder-side only, so the set changes how fast a file is
 * produced and, through float reassociation, which domains the search settles
 * on - but nothing on the decode path calls them. */
FIF_API const char *fif_simd_name(void);

/* How many threads an encode or decode would run with this `threads` option
 * (0 = let libfif decide), and how many cores the machine reports.  A caller
 * that wants to name the number before the work starts asks here rather than
 * reproducing the rule; fif_cpu_cores returns 0 when the system does not say.
 * The answer is the size of the thread pool: a parallel step with fewer items
 * than that starts only as many threads as it has items. */
FIF_API int fif_thread_count(int threads);
FIF_API int fif_cpu_cores(void);

/* ---- image i/o helpers (pnm always, png/jpeg when linked) ---- */
FIF_API fif_status fif_image_load(const char *path, fif_image *out);
/* The format follows the extension: .png, .jpg/.jpeg (quality 90), .ppm/.pgm/
 * .pnm.  Anything else, or a format missing from the build, is
 * FIF_ERR_UNSUPPORTED - never a file whose contents do not match its name. */
FIF_API fif_status fif_image_save(const char *path, const fif_image *img);

/* Reduce an image to w x h before encoding.  Every output pixel is the mean of
 * the input pixels it covers - an exact area filter, including the fractional
 * coverage at the edges of each footprint.  That is the right filter here:
 * dropping or interpolating samples would alias detail down into the
 * encoder's input, and the encoder would then spend bits coding the aliases.
 *
 * Downscaling only.  A target larger than the source in either axis returns
 * FIF_ERR_ARG rather than inventing pixels; `out` is allocated by this call
 * and belongs to the caller. */
FIF_API fif_status fif_image_downscale(const fif_image *src, int w, int h,
                               fif_image *out);

/* ---- quality metrics ---- */
FIF_API double fif_psnr(const fif_image *a, const fif_image *b);
FIF_API double fif_ssim(const fif_image *a, const fif_image *b);
FIF_API double fif_ms_ssim(const fif_image *a, const fif_image *b);

#ifdef __cplusplus
}
#endif
#endif /* FIF_H */
