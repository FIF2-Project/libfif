/* ------------------------------------------------------------------------
 * libfif v3.5 / fif v2.5 - FIF2 fractal image format
 *
 * libfif and fif:  Oskar Behzadpour
 * FIF2 format:     Rezar Behzad
 * Web:             www.FractalImageCompression.com
 * Contact:         oskar@fractalimagecompression.com
 *
 * Copyright (c) 2026 Oskar Behzadpour
 * SPDX-License-Identifier: MIT
 * ---------------------------------------------------------------------- */

/* SDK smoke test: a plain C program against the public API only.
 * CI links it against the shared library (libfif.dll / .dylib / .so), which
 * proves the exports, the C linkage and allocation across the boundary. */
#include <fif/fif.h>
#include <stdio.h>

static int fail(const char *what, fif_status st)
{
    fprintf(stderr, "sdk_smoke: %s: %s\n", what, fif_status_string(st));
    return 1;
}

int main(void)
{
    fif_image img = {0};
    fif_status st = fif_image_alloc(&img, 192, 128, 3);
    if (st != FIF_OK) return fail("alloc", st);
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            uint8_t *p = img.pixels + (size_t)y * img.stride + (size_t)x * 3;
            p[0] = (uint8_t)x;
            p[1] = (uint8_t)(y * 2);
            p[2] = (uint8_t)(((x / 24 + y / 24) & 1) ? 200 : 40);
        }

    fif_encode_options eo;
    fif_encode_options_defaults(&eo);
    eo.quality = 70;

    uint8_t *data = NULL;
    size_t size = 0;
    st = fif_encode(&img, &eo, &data, &size, NULL);
    if (st != FIF_OK) return fail("encode", st);

    fif_info info;
    st = fif_get_info(data, size, &info);
    if (st != FIF_OK) return fail("get_info", st);
    if (info.width != img.width || info.height != img.height) {
        fprintf(stderr, "sdk_smoke: info reports %dx%d\n", info.width, info.height);
        return 1;
    }

    fif_decode_options dopt;
    fif_decode_options_defaults(&dopt);
    fif_image out = {0};
    st = fif_decode(data, size, &dopt, &out, NULL);
    if (st != FIF_OK) return fail("decode", st);

    double psnr = fif_psnr(&img, &out);
    printf("libfif %d.%d (%s): %zu bytes, %.2f dB\n", FIF_VERSION_MAJOR,
           FIF_VERSION_MINOR, fif_simd_name(), size, psnr);

    fif_image_free(&out);
    fif_free(data);
    fif_image_free(&img);
    if (psnr < 25.0) {
        fprintf(stderr, "sdk_smoke: quality too low\n");
        return 1;
    }
    puts("sdk smoke test passed");
    return 0;
}
