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

#pragma once
#include "fif_internal.h"
#include <vector>

namespace fif {

/* Where one coded block meets the next.  Recorded per plane at min-block
 * granularity while the quadtrees are read, then used to drive the filter. */
struct EdgeMap {
    static constexpr uint8_t LEFT = 1, TOP = 2;
    /* What the leaf covering a cell is.  The filter treats a seam by what
     * meets there: a flat block has no detail to lose and takes a wide ramp,
     * a fractal one carries texture and is touched only at the seam. */
    static constexpr uint8_t FLAT = 0, PLANAR = 1, FRACTAL = 2;
    int cell = 4, cw = 0, ch = 0;
    std::vector<uint8_t> flags;     /* LEFT | TOP */
    std::vector<uint8_t> logsize;   /* log2 of the leaf edge covering the cell */
    std::vector<uint8_t> kind;      /* FLAT, PLANAR or FRACTAL */

    void init(int w, int h, int min_block) {
        cell = min_block < 1 ? 1 : min_block;
        cw = (w + cell - 1) / cell;
        ch = (h + cell - 1) / cell;
        flags.assign((size_t)cw * ch, 0);
        logsize.assign((size_t)cw * ch, 0);
        kind.assign((size_t)cw * ch, FRACTAL);
    }
    bool valid() const { return cw > 0 && ch > 0; }
    void mark(int x, int y, int n, uint8_t k = FRACTAL) {
        if (!valid()) return;
        const int x0 = x / cell, y0 = y / cell;
        const int x1 = (x + n) / cell, y1 = (y + n) / cell;
        const int lg = ilog2_ceil((uint32_t)n);
        for (int cy = y0; cy < y1 && cy < ch; ++cy)
            for (int cx = x0; cx < x1 && cx < cw; ++cx) {
                const size_t i = (size_t)cy * cw + cx;
                logsize[i] = (uint8_t)lg;
                kind[i] = k;
                if (cx == x0 && x > 0) flags[i] |= LEFT;
                if (cy == y0 && y > 0) flags[i] |= TOP;
            }
    }
};

/* A leaf without a transform - flat, or flat plus gradient - as placed in an
 * assembled plane. */
struct FlatRect { int x, y, n; };

/* Bends smooth blocks so that they meet their neighbours at the corners.  At
 * each corner the target is the mean of the smooth (flat or planar) blocks
 * that meet there and differ from this block's own corner value by at most
 * `tol` - a larger step is an edge in the picture, and texture is no basis
 * for a gradient.  The difference between target and own value at the four
 * corners is spread over the block as a bilinear correction of mean zero,
 * so the block mean stays what the stream coded.  A flat block becomes a soft surface, a planar one keeps its coded
 * gradient and loses only its step.  The staircase a sky or a cheek shows at
 * low rates is these steps side by side. */
class ThreadPool;
void smooth_flat_blocks(Plane &pl, const std::vector<FlatRect> &blocks,
                        const EdgeMap &em, float tol, ThreadPool &pool);

/* Smooths the seams the block partition leaves behind.  `strength` is 0..100;
 * 0 disables the filter.
 *
 * The two passes are split because they parallelise along different axes and
 * must not be interleaved: the horizontal pass reads what the vertical one
 * wrote, which is what makes corners come out symmetric.
 *
 * A vertical seam only ever writes inside its own cell row, so that pass bands
 * by `cy`; a horizontal seam only ever writes inside its own cell column, so
 * that pass bands by `cx`.  Each band is therefore disjoint from every other
 * by construction rather than by a margin someone has to keep checking.
 * [b0, b1) is the half-open band; the whole plane is [0, ch) and [0, cw). */
void deblock_vertical(Plane &pl, const EdgeMap &em, const CodecParams &p,
                      int strength, int b0, int b1);
void deblock_horizontal(Plane &pl, const EdgeMap &em, const CodecParams &p,
                        int strength, int b0, int b1);

inline void deblock_plane(Plane &pl, const EdgeMap &em, const CodecParams &p,
                          int strength) {
    deblock_vertical(pl, em, p, strength, 0, em.ch);
    deblock_horizontal(pl, em, p, strength, 0, em.cw);
}

} /* namespace fif */
