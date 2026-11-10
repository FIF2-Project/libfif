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

#include "fif/fif.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace {

std::vector<float> to_gray_plane(const fif_image *im, int ch) {
    std::vector<float> v((size_t)im->width * im->height);
    for (int y = 0; y < im->height; ++y) {
        const uint8_t *r = im->pixels + (size_t)y * im->stride;
        for (int x = 0; x < im->width; ++x)
            v[(size_t)y * im->width + x] = r[(size_t)x * im->channels + ch];
    }
    return v;
}

/* 11x11 gaussian, sigma 1.5 - the window from Wang et al. */
const std::vector<float> &gauss11() {
    static std::vector<float> k = [] {
        std::vector<float> g(11);
        float sum = 0;
        for (int i = 0; i < 11; ++i) {
            float d = (float)(i - 5);
            g[i] = std::exp(-d * d / (2 * 1.5f * 1.5f));
            sum += g[i];
        }
        for (auto &v : g) v /= sum;
        return g;
    }();
    return k;
}

void blur(const std::vector<float> &src, std::vector<float> &dst, int w, int h) {
    const auto &g = gauss11();
    std::vector<float> tmp(src.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float s = 0;
            for (int i = 0; i < 11; ++i) {
                int xx = std::clamp(x + i - 5, 0, w - 1);
                s += g[i] * src[(size_t)y * w + xx];
            }
            tmp[(size_t)y * w + x] = s;
        }
    dst.resize(src.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float s = 0;
            for (int i = 0; i < 11; ++i) {
                int yy = std::clamp(y + i - 5, 0, h - 1);
                s += g[i] * tmp[(size_t)yy * w + x];
            }
            dst[(size_t)y * w + x] = s;
        }
}

/* Returns mean SSIM and, separately, the contrast-structure term that MS-SSIM
 * needs at the coarser scales. */
void ssim_maps(const std::vector<float> &a, const std::vector<float> &b,
               int w, int h, double *out_ssim, double *out_cs) {
    const double C1 = 6.5025, C2 = 58.5225;
    std::vector<float> mu_a, mu_b, aa, bb, ab, saa, sbb, sab;
    blur(a, mu_a, w, h);
    blur(b, mu_b, w, h);
    const size_t n = a.size();
    aa.resize(n); bb.resize(n); ab.resize(n);
    for (size_t i = 0; i < n; ++i) { aa[i] = a[i]*a[i]; bb[i] = b[i]*b[i]; ab[i] = a[i]*b[i]; }
    blur(aa, saa, w, h); blur(bb, sbb, w, h); blur(ab, sab, w, h);

    double ssum = 0, csum = 0;
    for (size_t i = 0; i < n; ++i) {
        double ma = mu_a[i], mb = mu_b[i];
        double va = saa[i] - ma*ma, vb = sbb[i] - mb*mb, cov = sab[i] - ma*mb;
        double l  = (2*ma*mb + C1) / (ma*ma + mb*mb + C1);
        double cs = (2*cov + C2) / (va + vb + C2);
        ssum += l * cs;
        csum += cs;
    }
    if (out_ssim) *out_ssim = ssum / (double)n;
    if (out_cs)   *out_cs   = csum / (double)n;
}

void downsample2(const std::vector<float> &src, int w, int h,
                 std::vector<float> &dst, int *dw, int *dh) {
    *dw = w / 2; *dh = h / 2;
    dst.assign((size_t)(*dw) * (*dh), 0.f);
    for (int y = 0; y < *dh; ++y)
        for (int x = 0; x < *dw; ++x)
            dst[(size_t)y * (*dw) + x] = 0.25f *
                (src[(size_t)(2*y)*w + 2*x]     + src[(size_t)(2*y)*w + 2*x+1] +
                 src[(size_t)(2*y+1)*w + 2*x]   + src[(size_t)(2*y+1)*w + 2*x+1]);
}

bool same_shape(const fif_image *a, const fif_image *b) {
    return a && b && a->width == b->width && a->height == b->height &&
           a->channels == b->channels;
}

} /* namespace */

extern "C" {

double fif_psnr(const fif_image *a, const fif_image *b) {
    if (!same_shape(a, b)) return -1;
    double se = 0;
    size_t n = 0;
    for (int y = 0; y < a->height; ++y) {
        const uint8_t *ra = a->pixels + (size_t)y * a->stride;
        const uint8_t *rb = b->pixels + (size_t)y * b->stride;
        for (size_t i = 0; i < (size_t)a->width * a->channels; ++i) {
            double d = (double)ra[i] - rb[i];
            se += d * d; ++n;
        }
    }
    if (se == 0) return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 * (double)n / se);
}

double fif_ssim(const fif_image *a, const fif_image *b) {
    if (!same_shape(a, b)) return -1;
    double acc = 0;
    for (int c = 0; c < a->channels; ++c) {
        auto pa = to_gray_plane(a, c), pb = to_gray_plane(b, c);
        double s;
        ssim_maps(pa, pb, a->width, a->height, &s, nullptr);
        acc += s;
    }
    return acc / a->channels;
}

double fif_ms_ssim(const fif_image *a, const fif_image *b) {
    if (!same_shape(a, b)) return -1;
    static const double wts[5] = {0.0448, 0.2856, 0.3001, 0.2363, 0.1333};
    double total = 0;
    for (int c = 0; c < a->channels; ++c) {
        auto pa = to_gray_plane(a, c), pb = to_gray_plane(b, c);
        int w = a->width, h = a->height;
        double prod = 1.0;
        int levels = 0;
        for (int l = 0; l < 5; ++l) {
            if (w < 16 || h < 16) break;
            double ss, cs;
            ssim_maps(pa, pb, w, h, &ss, &cs);
            double term = (l == 4) ? ss : cs;
            prod *= std::pow(std::max(term, 1e-6), wts[l]);
            ++levels;
            if (l == 4) break;
            std::vector<float> na, nb; int nw, nh;
            downsample2(pa, w, h, na, &nw, &nh);
            downsample2(pb, w, h, nb, &nw, &nh);
            pa = std::move(na); pb = std::move(nb); w = nw; h = nh;
        }
        /* renormalise if the image was too small for all five scales */
        if (levels < 5) {
            double used = 0;
            for (int l = 0; l < levels; ++l) used += wts[l];
            prod = std::pow(prod, 1.0 / std::max(used, 1e-6));
        }
        total += prod;
    }
    return total / a->channels;
}

} /* extern "C" */
