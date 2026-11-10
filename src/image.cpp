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

#include "codec.h"
#include "threadpool.h"
#include "kernels.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace fif {

/* JPEG-style full-range BT.601. Chosen over Rec.709 so that 8-bit round trips
 * stay exact for grey and the chroma planes keep a 0..255 nominal range, which
 * lets the DC quantiser be shared between luma and chroma. */
namespace {

struct Ycc { float y, cb, cr; };

inline Ycc to_ycc(const uint8_t *px, int channels) {
    float r, g, b;
    if (channels == 1) { r = g = b = px[0]; }
    else { r = px[0]; g = px[1]; b = px[2]; }
    return { luma601(r, g, b),
            -0.168736f * r - 0.331264f * g + 0.5f * b + 128.f,
             0.5f * r - 0.418688f * g - 0.081312f * b + 128.f };
}

inline const uint8_t *sample(const fif_image &img, int x, int y) {
    const int sy = std::min(y, img.height - 1);
    const int sx = std::min(x, img.width - 1);
    return img.pixels + (size_t)sy * img.stride + (size_t)sx * img.channels;
}

} /* namespace */

void rgb_to_planes(const fif_image &img, Plane *pl, int pw, int ph) {
    pl[0].resize(pw, ph);
    pl[1].resize(pw, ph);
    pl[2].resize(pw, ph);
    for (int y = 0; y < ph; ++y) {
        float *Y = pl[0].row(y), *Cb = pl[1].row(y), *Cr = pl[2].row(y);
        for (int x = 0; x < pw; ++x) {
            const Ycc c = to_ycc(sample(img, x, y), img.channels);
            Y[x] = c.y; Cb[x] = c.cb; Cr[x] = c.cr;
        }
    }
}

/* The same thing with the chroma planes built at their final size directly.
 *
 * Converting the whole image and then halving the chroma needs three
 * full-resolution float planes at once, which on a 900 megapixel input is about
 * five gigabytes of peak footprint spent on samples that are thrown away two
 * lines later.  The result is identical to the byte: the four converted values
 * are averaged in the same order the separate downsample used. */
void rgb_to_planes_420(const fif_image &img, Plane *pl, int pw, int ph) {
    pl[0].resize(pw, ph);
    pl[1].resize(pw / 2, ph / 2);
    pl[2].resize(pw / 2, ph / 2);
    for (int y = 0; y < ph; y += 2) {
        float *Y0 = pl[0].row(y);
        float *Y1 = (y + 1 < ph) ? pl[0].row(y + 1) : nullptr;
        const int cy = y / 2;
        float *Cb = (cy < pl[1].h) ? pl[1].row(cy) : nullptr;
        float *Cr = (cy < pl[2].h) ? pl[2].row(cy) : nullptr;
        for (int x = 0; x < pw; x += 2) {
            const Ycc a0 = to_ycc(sample(img, x,     y),     img.channels);
            const Ycc a1 = to_ycc(sample(img, x + 1, y),     img.channels);
            const Ycc b0 = to_ycc(sample(img, x,     y + 1), img.channels);
            const Ycc b1 = to_ycc(sample(img, x + 1, y + 1), img.channels);
            Y0[x] = a0.y;
            if (x + 1 < pw) Y0[x + 1] = a1.y;
            if (Y1) { Y1[x] = b0.y; if (x + 1 < pw) Y1[x + 1] = b1.y; }
            const int cx = x / 2;
            if (Cb && cx < pl[1].w)
                Cb[cx] = 0.25f * (a0.cb + a1.cb + b0.cb + b1.cb);
            if (Cr && cx < pl[2].w)
                Cr[cx] = 0.25f * (a0.cr + a1.cr + b0.cr + b1.cr);
        }
    }
}

void gray_to_plane(const fif_image &img, Plane *pl, int pw, int ph) {
    pl[0].resize(pw, ph);
    for (int y = 0; y < ph; ++y) {
        const int sy = std::min(y, img.height - 1);
        const uint8_t *row = img.pixels + (size_t)sy * img.stride;
        float *Y = pl[0].row(y);
        for (int x = 0; x < pw; ++x) {
            const int sx = std::min(x, img.width - 1);
            const uint8_t *px = row + (size_t)sx * img.channels;
            Y[x] = img.channels == 1 ? px[0] : luma601(px[0], px[1], px[2]);
        }
    }
}

static inline uint8_t clamp8(float v) {
    int i = (int)std::lround(v);
    return (uint8_t)(i < 0 ? 0 : (i > 255 ? 255 : i));
}

/* Chroma siting follows JPEG: sample i covers luma columns 2i and 2i+1, so its
 * centre sits at 2i+0.5 and luma column x maps to chroma coordinate
 * (x-0.5)/2.  Interpolating on that grid rather than replicating samples is
 * worth well over a dB on chroma-rich material and costs almost nothing. */
struct Tap { int i0, i1; float t; };

static inline Tap chroma_tap(int x, int cs, int limit) {
    if (!cs) return Tap{x, x, 0.f};
    float f = ((float)x - 0.5f) * 0.5f;
    int i0 = (int)std::floor(f);
    float t = f - (float)i0;
    int i1 = i0 + 1;
    i0 = std::clamp(i0, 0, limit - 1);
    i1 = std::clamp(i1, 0, limit - 1);
    return Tap{i0, i1, t};
}

/* Mean over the (2r+1)^2 window around every sample, edges clamped: a
 * horizontal pass, then a vertical one. */
static void box_mean(const Plane &src, int r, Plane &dst, ThreadPool &pool) {
    const int w = src.w, h = src.h;
    Plane tmp;
    tmp.resize(w, h);
    dst.resize(w, h);
    pool.bands(h, 2, [&](int ya, int yb) {
    for (int y = ya; y < yb; ++y) {
        const float *s = src.row(y);
        float *t = tmp.row(y);
        for (int x = 0; x < w; ++x) {
            const int a = std::max(0, x - r), b = std::min(w - 1, x + r);
            float sum = 0;
            for (int k = a; k <= b; ++k) sum += s[k];
            t[x] = sum / (float)(b - a + 1);
        }
    }
    });
    pool.bands(h, 2, [&](int ya, int yb) {
    for (int y = ya; y < yb; ++y) {
        const int a = std::max(0, y - r), b = std::min(h - 1, y + r);
        float *d = dst.row(y);
        for (int x = 0; x < w; ++x) {
            float sum = 0;
            for (int k = a; k <= b; ++k) sum += tmp.row(k)[x];
            d[x] = sum / (float)(b - a + 1);
        }
    }
    });
}

void luma_to_chroma_grid(const Plane &Y, int cs, int w, int h, Plane &yd, ThreadPool &pool) {
    const int f = 1 << cs;
    yd.resize(w, h);
    pool.bands(h, 2, [&](int ja, int jb) {
    for (int j = ja; j < jb; ++j)
        for (int i = 0; i < w; ++i) {
            float sum = 0;
            int n = 0;
            for (int dy = 0; dy < f; ++dy)
                for (int dx = 0; dx < f; ++dx) {
                    const int x = i * f + dx, y = j * f + dy;
                    if (x < Y.w && y < Y.h) { sum += Y.row(y)[x]; ++n; }
                }
            yd.row(j)[i] = n ? sum / (float)n : 128.f;
        }
    });
}

void chroma_guide(const Plane &yd, const Plane &C, float eps, float amax, Plane &A,
                  ThreadPool &pool) {
    const int w = C.w, h = C.h;
    /* The least-squares slope of chroma over luma in the 3x3 window around
     * each chroma sample, all four window sums in one pass.  eps is the luma
     * variance below which a window counts as flat: there the slope tends to
     * 0 and the result is plain bilinear. */
    Plane a;
    a.resize(w, h);
    pool.bands(h, 2, [&](int ja, int jb) {
        for (int j = ja; j < jb; ++j) {
            const int y0 = std::max(0, j - 1), y1 = std::min(h - 1, j + 1);
            for (int i = 0; i < w; ++i) {
                const int x0 = std::max(0, i - 1), x1 = std::min(w - 1, i + 1);
                float sy = 0, sc = 0, syc = 0, syy = 0;
                for (int y = y0; y <= y1; ++y) {
                    const float *ry = yd.row(y), *rc = C.row(y);
                    for (int x = x0; x <= x1; ++x) {
                        sy += ry[x]; sc += rc[x]; syc += ry[x] * rc[x]; syy += ry[x] * ry[x];
                    }
                }
                const float n = (float)((y1 - y0 + 1) * (x1 - x0 + 1));
                const float my = sy / n, mc = sc / n;
                const float var = syy / n - my * my, cov = syc / n - my * mc;
                a.row(j)[i] = std::clamp(cov / (var + eps), -amax, amax);
            }
        }
    });
    box_mean(a, 1, A, pool);
}

/* Output rows [y0, y1), `w` wide, read from the planes at (px0, py0): the
 * offset lets a region be converted out of larger planes without a copy. */
void planes_to_image(const Plane *pl, int nplanes, int cs,
                     int w, int y0, int y1, fif_image &out, int px0, int py0,
                     const ConvertOpts *opt) {
    const Plane *guide = opt ? opt->guide : nullptr;
    const bool dither = opt && opt->dither;
    /* An 8x8 Bayer matrix as offsets in (-0.5, 0.5): a smooth gradient that
     * would round to bands of equal grey breaks up into a fine pattern the eye
     * averages back into the gradient. */
    const auto dith = [&](int x, int y) {
        if (!dither) return 0.f;
        static const uint8_t bayer[8][8] = {
            {0, 32, 8, 40, 2, 34, 10, 42}, {48, 16, 56, 24, 50, 18, 58, 26},
            {12, 44, 4, 36, 14, 46, 6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
            {3, 35, 11, 43, 1, 33, 9, 41}, {51, 19, 59, 27, 49, 17, 57, 25},
            {15, 47, 7, 39, 13, 45, 5, 37}, {63, 31, 55, 23, 61, 29, 53, 21}};
        const int u = (x + opt->dither_x0) & 7, v = (y + opt->dither_y0) & 7;
        return ((float)bayer[v][u] + 0.5f) / 64.f - 0.5f;
    };
    if (nplanes == 1) {
        for (int y = y0; y < y1; ++y) {
            uint8_t *row = out.pixels + (size_t)y * out.stride;
            const float *Y = pl[0].row(y + py0) + px0;
            for (int x = 0; x < w; ++x) row[x] = clamp8(Y[x] + dith(x, y));
        }
        return;
    }

    std::vector<Tap> xt((size_t)w);
    for (int x = 0; x < w; ++x) xt[x] = chroma_tap(x + px0, cs, pl[1].w);

    for (int y = y0; y < y1; ++y) {
        uint8_t *row = out.pixels + (size_t)y * out.stride;
        const float *Y = pl[0].row(y + py0) + px0;
        const Tap ty = chroma_tap(y + py0, cs, pl[1].h);

        for (int x = 0; x < w; ++x) {
            const Tap &tx = xt[x];
            const float yy = Y[x];
            /* Bilinear on the chroma grid. */
            const auto lerp = [&](const Plane &P) {
                const float *r0 = P.row(ty.i0), *r1 = P.row(ty.i1);
                return (r0[tx.i0] * (1 - tx.t) + r0[tx.i1] * tx.t) * (1 - ty.t) +
                       (r1[tx.i0] * (1 - tx.t) + r1[tx.i1] * tx.t) * ty.t;
            };
            float cb, cr;
            if (guide) {
                /* Edge-aware: bilinear chroma, plus the luma detail the
                 * chroma grid cannot hold, scaled by how chroma follows
                 * luma locally - so a colour edge sits where the
                 * brightness edge is, not a chroma sample away from it.
                 * Where chroma does not follow luma the slope is 0 and
                 * this is plain bilinear. */
                const float detail = yy - lerp(guide[2]);
                cb = lerp(pl[1]) - 128.f + lerp(guide[0]) * detail;
                cr = lerp(pl[2]) - 128.f + lerp(guide[1]) * detail;
            } else {
                cb = lerp(pl[1]) - 128.f;
                cr = lerp(pl[2]) - 128.f;
            }
            uint8_t *px = row + (size_t)x * 3;
            const float d = dith(x, y);
            px[0] = clamp8(yy + 1.402f * cr + d);
            px[1] = clamp8(yy - 0.344136f * cb - 0.714136f * cr + d);
            px[2] = clamp8(yy + 1.772f * cb + d);
        }
    }
}

} /* namespace fif */

extern "C" {

void fif_image_free(fif_image *img) {
    if (img && img->pixels) { free(img->pixels); img->pixels = nullptr; }
}

fif_status fif_image_alloc(fif_image *img, int width, int height, int channels) {
    if (!img || width <= 0 || height <= 0 || (channels != 1 && channels != 3))
        return FIF_ERR_ARG;
    img->stride = (size_t)width * channels;
    img->pixels = (uint8_t *)std::calloc(img->stride, (size_t)height);
    if (!img->pixels) return FIF_ERR_NOMEM;
    img->width = width; img->height = height; img->channels = channels;
    return FIF_OK;
}

void fif_free(void *p) { free(p); }

const char *fif_simd_name(void) { return fif::simd_name(); }

fif_status fif_image_downscale(const fif_image *src, int w, int h,
                               fif_image *out) {
    if (!src || !out || !src->pixels || w <= 0 || h <= 0) return FIF_ERR_ARG;
    if (w > src->width || h > src->height) return FIF_ERR_ARG;

    const int ch = src->channels;
    fif_status st = fif_image_alloc(out, w, h, ch);
    if (st != FIF_OK) return st;

    const double fx = (double)src->width / w, fy = (double)src->height / h;
    std::vector<double> acc((size_t)w * ch);

    for (int oy = 0; oy < h; ++oy) {
        const double y0 = oy * fy, y1 = y0 + fy;
        const int iy0 = (int)y0, iy1 = (int)std::ceil(y1);
        std::fill(acc.begin(), acc.end(), 0.0);
        double wsum = 0;

        for (int sy = iy0; sy < iy1 && sy < src->height; ++sy) {
            /* How much of this source row falls inside the footprint. */
            const double wy = std::min(y1, (double)sy + 1) - std::max(y0, (double)sy);
            if (wy <= 0) continue;
            wsum += wy;
            const uint8_t *row = src->pixels + (size_t)sy * src->stride;

            for (int ox = 0; ox < w; ++ox) {
                const double x0 = ox * fx, x1 = x0 + fx;
                const int ix0 = (int)x0, ix1 = (int)std::ceil(x1);
                for (int sx = ix0; sx < ix1 && sx < src->width; ++sx) {
                    const double wx =
                        std::min(x1, (double)sx + 1) - std::max(x0, (double)sx);
                    if (wx <= 0) continue;
                    const double wgt = wy * wx;
                    const uint8_t *p = row + (size_t)sx * ch;
                    for (int c = 0; c < ch; ++c)
                        acc[(size_t)ox * ch + c] += wgt * p[c];
                }
            }
        }

        uint8_t *orow = out->pixels + (size_t)oy * out->stride;
        /* The column weights sum to fx for every interior pixel, and the row
         * weights to wsum, so one divisor covers the whole row. */
        const double norm = (wsum > 0) ? 1.0 / (wsum * fx) : 0.0;
        for (int ox = 0; ox < w; ++ox)
            for (int c = 0; c < ch; ++c) {
                const double v = acc[(size_t)ox * ch + c] * norm;
                orow[(size_t)ox * ch + c] = fif::clamp8((float)v);
            }
    }
    return FIF_OK;
}

const char *fif_status_string(fif_status s) {
    switch (s) {
        case FIF_OK: return "ok";
        case FIF_ERR_ARG: return "invalid argument";
        case FIF_ERR_IO: return "i/o error";
        case FIF_ERR_FORMAT: return "malformed FIF2 file";
        case FIF_ERR_NOMEM: return "out of memory";
        case FIF_ERR_UNSUPPORTED: return "unsupported feature";
        case FIF_ERR_TOO_LARGE: return "larger than the decoder's pixel limit";
        case FIF_ERR_ENCRYPTED: return "encrypted; a password is needed";
    }
    return "unknown";
}

} /* extern "C" */
