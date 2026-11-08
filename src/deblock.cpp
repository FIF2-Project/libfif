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

#include "deblock.h"
#include "threadpool.h"
#include <algorithm>
#include <cmath>

namespace fif {
namespace {

/* A conditional filter in the style H.264 and AV1 use: only smooth a seam that
 * looks like a coding artefact.  The test is that the step across the boundary
 * is small enough to be quantisation noise while the samples on either side are
 * locally flat - a genuine image edge fails it and is left alone.
 *
 * Fractal coding needs this more than a transform codec does. A range block is
 * filled from a domain somewhere else in the image, so neighbouring blocks
 * carry no shared basis and their seams do not cancel; and because the decoder
 * feeds its own output back in, an untreated seam is re-read as domain data and
 * reproduced at the next iteration. */
struct Params {
    float alpha;   /* largest step across the seam still treated as artefact */
    float beta;    /* largest variation inside a block still treated as flat */
    float limit;   /* cap on how far the seam samples may move              */
};

bool smooth(uint8_t k) { return k != EdgeMap::FRACTAL; }

/* The thresholds for one seam, from what meets there.  Two smooth blocks
 * (flat or planar) meeting is the staircase in a sky: the step is all
 * quantisation and nothing else, so a larger one still counts as artefact.
 * Where texture is involved, a step of that size is as likely to be detail,
 * and the filter holds back. */
Params params_for(const CodecParams &p, int strength, int logl, int logr,
                  uint8_t kl, uint8_t kr) {
    const float qstep = 255.0f / (float)(1 << p.dc_bits);
    /* Larger blocks carry larger fit errors, so their seams step harder. */
    const float size_gain = 0.7f + 0.15f * (float)std::max(0, std::max(logl, logr) - 2);
    const float s = (float)strength / 50.0f * size_gain;
    const int nsmooth = smooth(kl) + smooth(kr);
    /* Measured: raising the threshold where texture is involved bought
     * nothing, lowering it below 1 left seams standing. */
    const float edge = nsmooth == 2 ? 1.6f : 1.0f;
    Params q;
    q.alpha = 3.6f * qstep * s * edge;
    q.beta  = 1.6f * qstep * s;
    /* Between two smooth blocks the seam may close completely; with texture
     * on either side only part of the step is taken out. */
    q.limit = nsmooth == 2 ? q.alpha : 1.6f * qstep * s;
    return q;
}

/* How far into one block the correction may reach.  A step between two
 * 32-pixel blocks stays visible however well the two samples either side of
 * it are patched - the eye locks onto the discontinuity, not its amplitude -
 * so a flat block, which has no detail to lose, takes a ramp a quarter of its
 * width.  A planar one already varies and takes less; a fractal one carries
 * texture and is touched only at the seam. */
int reach(uint8_t kind, int logsize) {
    const int n = 1 << logsize;
    switch (kind) {
    case EdgeMap::FLAT:   return std::clamp(n / 4, 1, 8);
    case EdgeMap::PLANAR: return std::clamp(n / 8, 1, 4);
    default:              return logsize >= 5 ? 2 : 1;
    }
}

/* Applies the ramp across one seam.  `left` walks away from the seam on one
 * side for wl samples, `right` on the other for wr; `stride` is the step
 * between samples.  The two widths differ when the blocks do. */
inline void filter_seam(float *left, float *right, int stride, const Params &q,
                        int wl, int wr, bool tex_l, bool tex_r) {
    const float d = *right - *left;
    if (std::fabs(d) >= q.alpha) return;                 /* a real edge */
    if (wl < 1 || wr < 1) return;
    /* Texture next to the seam means the step may be detail; only a side
     * known to be smooth skips the test. */
    if (tex_l && std::fabs(*(left - (wl - 1) * stride) - *left) >= q.beta) return;
    if (tex_r && std::fabs(*(right + (wr - 1) * stride) - *right) >= q.beta) return;

    const float delta = std::clamp(d * 0.5f, -q.limit, q.limit);
    for (int k = 0; k < wl; ++k)
        *(left - k * stride) += delta * (float)(wl - k) / (float)(wl + 1);
    for (int k = 0; k < wr; ++k)
        *(right + k * stride) -= delta * (float)(wr - k) / (float)(wr + 1);
}

} /* namespace */

void deblock_vertical(Plane &pl, const EdgeMap &em, const CodecParams &p,
                      int strength, int b0, int b1) {
    if (strength <= 0 || !em.valid()) return;
    for (int cy = b0; cy < b1; ++cy)
        for (int cx = 1; cx < em.cw; ++cx) {
            const size_t i = (size_t)cy * em.cw + cx;
            if (!(em.flags[i] & EdgeMap::LEFT)) continue;
            /* Zero is a cell no decoded leaf covers - a tile left out of a
             * partial decode.  Nothing to blend against. */
            const int ll = em.logsize[i - 1], lr = em.logsize[i];
            if (ll == 0 || lr == 0) continue;
            const uint8_t kl = em.kind[i - 1], kr = em.kind[i];
            const int x = cx * em.cell;
            const Params q = params_for(p, strength, ll, lr, kl, kr);
            /* Never past the middle of either block, or off the plane. */
            const int wl = std::min({reach(kl, ll), (1 << ll) / 2, x});
            const int wr = std::min({reach(kr, lr), (1 << lr) / 2, pl.w - x});
            const int y0 = cy * em.cell, y1 = std::min(y0 + em.cell, pl.h);
            for (int y = y0; y < y1; ++y) {
                float *r = pl.row(y) + x;
                filter_seam(r - 1, r, 1, q, wl, wr, !smooth(kl), !smooth(kr));
            }
        }
}

void deblock_horizontal(Plane &pl, const EdgeMap &em, const CodecParams &p,
                        int strength, int b0, int b1) {
    if (strength <= 0 || !em.valid()) return;
    for (int cy = 1; cy < em.ch; ++cy)
        for (int cx = b0; cx < b1; ++cx) {
            const size_t i = (size_t)cy * em.cw + cx;
            if (!(em.flags[i] & EdgeMap::TOP)) continue;
            const int lt = em.logsize[i - em.cw], lb = em.logsize[i];
            if (lt == 0 || lb == 0) continue;
            const uint8_t kt = em.kind[i - em.cw], kb = em.kind[i];
            const int y = cy * em.cell;
            const Params q = params_for(p, strength, lt, lb, kt, kb);
            const int wl = std::min({reach(kt, lt), (1 << lt) / 2, y});
            const int wr = std::min({reach(kb, lb), (1 << lb) / 2, pl.h - y});
            const int x0 = cx * em.cell, x1 = std::min(x0 + em.cell, pl.w);
            for (int x = x0; x < x1; ++x) {
                float *r = pl.row(y) + x;
                filter_seam(r - pl.w, r, pl.w, q, wl, wr, !smooth(kt), !smooth(kb));
            }
        }
}

void smooth_flat_blocks(Plane &pl, const std::vector<FlatRect> &blocks,
                        const EdgeMap &em, float tol, ThreadPool &pool) {
    if (!em.valid()) return;
    const auto smooth_at = [&](int x, int y) {
        const size_t c = (size_t)(y / em.cell) * em.cw + (size_t)(x / em.cell);
        return em.logsize[c] != 0 && em.kind[c] != EdgeMap::FRACTAL;
    };
    /* Target at the corner point (cx, cy), seen from a block whose own pixel
     * there has value `own`. */
    const auto target = [&](int cx, int cy, float own) {
        float sum = 0;
        int cnt = 0;
        for (int dy = -1; dy <= 0; ++dy)
            for (int dx = -1; dx <= 0; ++dx) {
                const int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= pl.w || y >= pl.h || !smooth_at(x, y)) continue;
                const float v = pl.row(y)[x];
                if (std::fabs(v - own) > tol) continue;
                sum += v;
                ++cnt;
            }
        return cnt ? sum / (float)cnt : own;
    };
    /* Every correction is worked out before any block is changed, so the
     * result does not depend on the order the blocks come in. */
    std::vector<float> e4(blocks.size() * 4, 0.f);
    const size_t nb = blocks.size(), chunks = std::min<size_t>(nb, (size_t)pool.size() * 4);
    const auto each = [&](auto &&fn) {
        if (!nb) return;
        pool.parallel_for(chunks, [&](size_t k, int) {
            for (size_t b = nb * k / chunks; b < nb * (k + 1) / chunks; ++b) fn(b);
        });
    };
    each([&](size_t b) {
        const FlatRect &r = blocks[b];
        if (r.n < 2 || r.x + r.n > pl.w || r.y + r.n > pl.h) return;
        const int x1 = r.x + r.n - 1, y1 = r.y + r.n - 1;
        const float o00 = pl.row(r.y)[r.x], o10 = pl.row(r.y)[x1];
        const float o01 = pl.row(y1)[r.x], o11 = pl.row(y1)[x1];
        e4[4 * b + 0] = target(r.x, r.y, o00) - o00;
        e4[4 * b + 1] = target(r.x + r.n, r.y, o10) - o10;
        e4[4 * b + 2] = target(r.x, r.y + r.n, o01) - o01;
        e4[4 * b + 3] = target(r.x + r.n, r.y + r.n, o11) - o11;
    });
    /* Blocks do not overlap, so the writes need no ordering. */
    each([&](size_t b) {
        const FlatRect &r = blocks[b];
        if (r.n < 2 || r.x + r.n > pl.w || r.y + r.n > pl.h) return;
        const float *e = &e4[4 * b];
        /* A bilinear surface's mean is the mean of its corners; subtracting
         * it keeps the block mean where the stream put it. */
        const float shift = -0.25f * (e[0] + e[1] + e[2] + e[3]);
        const float inv = 1.f / (float)r.n;
        for (int j = 0; j < r.n; ++j) {
            const float v = ((float)j + 0.5f) * inv;
            const float a = e[0] + (e[2] - e[0]) * v, bb = e[1] + (e[3] - e[1]) * v;
            float *row = pl.row(r.y + j) + r.x;
            for (int i = 0; i < r.n; ++i)
                row[i] += a + (bb - a) * (((float)i + 0.5f) * inv) + shift;
        }
    });
}

} /* namespace fif */
