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

#include "fif/fif.h"
#include "format.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef FIF_HAVE_PNG
#include <png.h>
#endif
#ifdef FIF_HAVE_JPEG
#include <jpeglib.h>
#include <setjmp.h>
#endif

namespace {

const char *ext_of(const char *path) {
    const char *d = std::strrchr(path, '.');
    return d ? d + 1 : "";
}
bool ieq(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b)
        if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b)) return false;
    return !*a && !*b;
}

/* ------------------------------------------------------------------- pnm */

int pnm_token(FILE *f) {
    int c;
    for (;;) {
        c = fgetc(f);
        if (c == '#') { while (c != '\n' && c != EOF) c = fgetc(f); continue; }
        if (!std::isspace(c)) break;
    }
    if (c == EOF) return -1;
    int v = 0;
    while (c != EOF && std::isdigit(c)) { v = v * 10 + (c - '0'); c = fgetc(f); }
    return v;
}

fif_status pnm_load(const char *path, fif_image *out) {
    FILE *f = std::fopen(path, "rb");
    if (!f) return FIF_ERR_IO;
    char magic[3] = {0};
    if (std::fread(magic, 1, 2, f) != 2 || magic[0] != 'P' ||
        (magic[1] != '5' && magic[1] != '6')) { std::fclose(f); return FIF_ERR_FORMAT; }
    const int ch = (magic[1] == '5') ? 1 : 3;
    int w = pnm_token(f), h = pnm_token(f), mx = pnm_token(f);
    if (w <= 0 || h <= 0 || mx != 255) { std::fclose(f); return FIF_ERR_FORMAT; }
    fif_status st = fif_image_alloc(out, w, h, ch);
    if (st != FIF_OK) { std::fclose(f); return st; }
    const size_t n = (size_t)w * h * ch;
    if (std::fread(out->pixels, 1, n, f) != n) {
        std::fclose(f); fif_image_free(out); return FIF_ERR_FORMAT;
    }
    std::fclose(f);
    return FIF_OK;
}

/* The extension says what the file is: .pgm is grey, .ppm is colour, .pnm is
 * whatever the image has.  A colour image saved as .pgm is reduced to luma
 * (BT.601, as the codec uses), a grey one saved as .ppm is replicated. */
fif_status pnm_save(const char *path, const fif_image *img) {
    const char *e = ext_of(path);
    const int out_ch = ieq(e, "pgm") ? 1 : ieq(e, "ppm") ? 3 : img->channels;
    FILE *f = std::fopen(path, "wb");
    if (!f) return FIF_ERR_IO;
    bool ok = std::fprintf(f, "P%c\n%d %d\n255\n", out_ch == 1 ? '5' : '6',
                           img->width, img->height) > 0;
    const size_t n = (size_t)img->width * out_ch;
    std::vector<uint8_t> row(n);
    for (int y = 0; y < img->height && ok; ++y) {
        const uint8_t *src = img->pixels + (size_t)y * img->stride;
        const uint8_t *out = src;
        if (out_ch != img->channels) {
            for (int x = 0; x < img->width; ++x) {
                if (out_ch == 1) {
                    const uint8_t *px = src + (size_t)x * 3;
                    row[x] = (uint8_t)std::lround(fif::luma601(px[0], px[1], px[2]));
                } else {
                    row[(size_t)x * 3] = row[(size_t)x * 3 + 1] = row[(size_t)x * 3 + 2] = src[x];
                }
            }
            out = row.data();
        }
        ok = std::fwrite(out, 1, n, f) == n;
    }
    /* A full disk reports itself at the close, not at the write. */
    if (std::fclose(f) != 0) ok = false;
    return ok ? FIF_OK : FIF_ERR_IO;
}

/* ------------------------------------------------------------------- png */
#ifdef FIF_HAVE_PNG
/* libpng prints its errors and warnings to stderr by default.  A library has
 * no business writing to the caller's terminal: the failure is reported as a
 * status code, and the caller decides what to tell the user. */
void png_quiet_error(png_structp png, png_const_charp) { png_longjmp(png, 1); }
void png_quiet_warning(png_structp, png_const_charp) {}

fif_status png_load(const char *path, fif_image *out) {
    FILE *f = std::fopen(path, "rb");
    if (!f) return FIF_ERR_IO;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, png_quiet_error, png_quiet_warning);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    if (!png || !info) { if (png) png_destroy_read_struct(&png, &info, nullptr);
                         std::fclose(f); return FIF_ERR_NOMEM; }
    out->pixels = nullptr;
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        fif_image_free(out);                 /* allocated before the error */
        std::fclose(f); return FIF_ERR_FORMAT;
    }
    png_init_io(png, f);
    png_read_info(png, info);
    png_set_strip_16(png);
    png_set_palette_to_rgb(png);
    png_set_expand_gray_1_2_4_to_8(png);
    /* tRNS has to become alpha before alpha is stripped.  The other way round
     * libpng hands back four channels into a three-channel allocation. */
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    png_set_strip_alpha(png);
    png_read_update_info(png, info);

    const int w = (int)png_get_image_width(png, info);
    const int h = (int)png_get_image_height(png, info);
    /* What the rows will actually hold after the transforms, not what the
     * colour type was before them. */
    const int ch = (int)png_get_channels(png, info);
    if ((ch != 1 && ch != 3) || png_get_bit_depth(png, info) != 8) {
        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(f); return FIF_ERR_UNSUPPORTED;
    }

    if (fif_image_alloc(out, w, h, ch) != FIF_OK) {
        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(f); return FIF_ERR_NOMEM;
    }
    std::vector<png_bytep> rows(h);
    for (int y = 0; y < h; ++y) rows[y] = out->pixels + (size_t)y * out->stride;
    png_read_image(png, rows.data());
    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(f);
    return FIF_OK;
}

fif_status png_save(const char *path, const fif_image *img) {
    FILE *f = std::fopen(path, "wb");
    if (!f) return FIF_ERR_IO;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, png_quiet_error, png_quiet_warning);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    if (!png || !info) { if (png) png_destroy_write_struct(&png, &info);
                         std::fclose(f); return FIF_ERR_NOMEM; }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        std::fclose(f); return FIF_ERR_IO;
    }
    png_init_io(png, f);
    png_set_IHDR(png, info, img->width, img->height, 8,
                 img->channels == 1 ? PNG_COLOR_TYPE_GRAY : PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (int y = 0; y < img->height; ++y)
        png_write_row(png, img->pixels + (size_t)y * img->stride);
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    std::fclose(f);
    return FIF_OK;
}
#endif

/* ------------------------------------------------------------------ jpeg */
#ifdef FIF_HAVE_JPEG
struct JpegErr { jpeg_error_mgr pub; jmp_buf jb; };
void jpeg_fail(j_common_ptr cinfo) { longjmp(((JpegErr *)cinfo->err)->jb, 1); }

#endif

} /* namespace */

namespace fif {

fif_status jpeg_encode_mem(const fif_image &img, int quality, std::vector<uint8_t> &out) {
#ifdef FIF_HAVE_JPEG
    if (!img.pixels || (img.channels != 1 && img.channels != 3)) return FIF_ERR_ARG;
    jpeg_compress_struct ci{};
    JpegErr jerr{};
    ci.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_fail;
    unsigned char *mem = nullptr;
    unsigned long mem_size = 0;
    if (setjmp(jerr.jb)) {
        jpeg_destroy_compress(&ci);
        std::free(mem);
        return FIF_ERR_IO;
    }
    jpeg_create_compress(&ci);
    jpeg_mem_dest(&ci, &mem, &mem_size);
    ci.image_width = (JDIMENSION)img.width;
    ci.image_height = (JDIMENSION)img.height;
    ci.input_components = img.channels;
    ci.in_color_space = img.channels == 1 ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_set_defaults(&ci);
    jpeg_set_quality(&ci, quality < 1 ? 1 : quality > 100 ? 100 : quality, TRUE);
    jpeg_start_compress(&ci, TRUE);
    while (ci.next_scanline < ci.image_height) {
        JSAMPROW row = img.pixels + (size_t)ci.next_scanline * img.stride;
        jpeg_write_scanlines(&ci, &row, 1);
    }
    jpeg_finish_compress(&ci);
    jpeg_destroy_compress(&ci);
    out.assign(mem, mem + mem_size);
    std::free(mem);
    return FIF_OK;
#else
    (void)img; (void)quality; (void)out;
    return FIF_ERR_UNSUPPORTED;
#endif
}

fif_status jpeg_decode_mem(const uint8_t *d, size_t n, fif_image *out) {
#ifdef FIF_HAVE_JPEG
    if (!d || !n || !out) return FIF_ERR_ARG;
    jpeg_decompress_struct ci{};
    JpegErr jerr{};
    ci.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_fail;
    out->pixels = nullptr;
    if (setjmp(jerr.jb)) {
        jpeg_destroy_decompress(&ci);
        fif_image_free(out);
        return FIF_ERR_FORMAT;
    }
    jpeg_create_decompress(&ci);
    jpeg_mem_src(&ci, d, (unsigned long)n);
    jpeg_read_header(&ci, TRUE);
    ci.out_color_space = (ci.num_components == 1) ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_start_decompress(&ci);
    if (fif_image_alloc(out, ci.output_width, ci.output_height, ci.output_components) != FIF_OK) {
        jpeg_destroy_decompress(&ci);
        return FIF_ERR_NOMEM;
    }
    while (ci.output_scanline < ci.output_height) {
        JSAMPROW row = out->pixels + (size_t)ci.output_scanline * out->stride;
        jpeg_read_scanlines(&ci, &row, 1);
    }
    jpeg_finish_decompress(&ci);
    jpeg_destroy_decompress(&ci);
    return FIF_OK;
#else
    (void)d; (void)n; (void)out;
    return FIF_ERR_UNSUPPORTED;
#endif
}

} /* namespace fif */

namespace {

#ifdef FIF_HAVE_JPEG
/* Reads the whole file and decodes it in memory: one JPEG decoder, not two. */
fif_status jpeg_load(const char *path, fif_image *out) {
    FILE *f = std::fopen(path, "rb");
    if (!f) return FIF_ERR_IO;
    std::vector<uint8_t> bytes;
    uint8_t buf[1 << 16];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    const bool err = std::ferror(f) != 0;
    std::fclose(f);
    if (err) return FIF_ERR_IO;
    return fif::jpeg_decode_mem(bytes.data(), bytes.size(), out);
}

fif_status jpeg_save(const char *path, const fif_image *img) {
    std::vector<uint8_t> bytes;
    fif_status st = fif::jpeg_encode_mem(*img, 90, bytes);
    if (st != FIF_OK) return st;
    FILE *f = std::fopen(path, "wb");
    if (!f) return FIF_ERR_IO;
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    if (std::fclose(f) != 0) ok = false;
    return ok ? FIF_OK : FIF_ERR_IO;
}
#endif

} /* namespace */

extern "C" {

fif_status fif_image_load(const char *path, fif_image *out) {
    if (!path || !out) return FIF_ERR_ARG;
    const char *e = ext_of(path);
    if (ieq(e, "pgm") || ieq(e, "ppm") || ieq(e, "pnm")) return pnm_load(path, out);
#ifdef FIF_HAVE_PNG
    if (ieq(e, "png")) return png_load(path, out);
#endif
#ifdef FIF_HAVE_JPEG
    if (ieq(e, "jpg") || ieq(e, "jpeg")) return jpeg_load(path, out);
#endif
    return pnm_load(path, out);
}

fif_status fif_image_save(const char *path, const fif_image *img) {
    if (!path || !img || !img->pixels) return FIF_ERR_ARG;
    const char *e = ext_of(path);
    /* The extension decides, and an extension this build cannot write is an
     * error.  Falling back to PNM here once produced ".jpg" files full of PPM
     * bytes that no viewer would open. */
    if (ieq(e, "pgm") || ieq(e, "ppm") || ieq(e, "pnm")) return pnm_save(path, img);
#ifdef FIF_HAVE_PNG
    if (ieq(e, "png")) return png_save(path, img);
#endif
#ifdef FIF_HAVE_JPEG
    if (ieq(e, "jpg") || ieq(e, "jpeg")) return jpeg_save(path, img);
#endif
    return FIF_ERR_UNSUPPORTED;
}

} /* extern "C" */
