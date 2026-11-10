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

#pragma once
#include "fif_internal.h"
#include "context.h"
#include "pool.h"
#include "deblock.h"
#include <algorithm>
#include <vector>

namespace fif {

/* Depth-first pre-order quadtree.  A node with split=true is followed by its
 * four children (recursively); a leaf carries the coded parameters. */
struct QNode {
    bool split = false;
    Leaf leaf{};
};

struct Tile {
    int x = 0, y = 0, w = 0, h = 0;     /* luma coordinates, padded */
    std::vector<QNode> trees[3];        /* one per plane, luma first */
};

/* What a symbol costs, in bits, as the rate-distortion search estimates it.
 * The raw field widths are nowhere near the truth once the range coder has
 * modelled them: a DC field declared at 8 bits costs about 3, a 14-bit domain
 * index about 10. */
struct RateModel {
    double split    = 0.15;
    double mode     = 0.35;
    double dc       = 4.0;
    double sym      = 1.7;
    double contrast = 4.2;
    double planar   = 0.6;     /* the flag, on a leaf with no transform */
    double grad     = 3.0;     /* one gradient component                */
    /* Domain cost scales with the level's index width, so it is carried as a
     * fraction of that width rather than as one absolute number. */
    double domain_frac = 0.78;

    static RateModel initial(const CodecParams &p) {
        RateModel r;
        r.dc       = 0.55 * p.dc_bits;
        r.contrast = 0.72 * p.s_bits;
        return r;
    }
    double domain(int idx_bits) const { return domain_frac * idx_bits; }
};

struct EncodeCounters {
    int64_t blocks_total = 0;
    int64_t blocks_flat = 0;
    int64_t blocks_fractal = 0;
    int64_t candidate_evals = 0;
    /* Distortion of what was actually coded, as the serialiser measured it. */
    double  collage_sse = 0;
    int64_t coded_pixels = 0;

    void operator+=(const EncodeCounters &o) {
        blocks_total += o.blocks_total; blocks_flat += o.blocks_flat;
        blocks_fractal += o.blocks_fractal; candidate_evals += o.candidate_evals;
        collage_sse += o.collage_sse; coded_pixels += o.coded_pixels;
    }
};

void derive_params(CodecParams &p, int quality, int effort);

/* Where a leaf reads its domain from, in the coordinates render_plane works
 * in: tile-local and already multiplied by `scale`, so the caller adds its own
 * plane origin.  `dext` is the edge of the square actually read, which is
 * twice the range edge - domains are 2n x 2n decimated to n x n.  False for a
 * leaf that codes no transform.
 *
 * Shared between the renderer and the debug dump deliberately.  A visualiser
 * that recomputed this for itself would be drawing a second opinion, and the
 * first time the two disagreed the picture would be the thing that looked
 * wrong. */
inline bool domain_rect(const Leaf &lf, const CodecParams &p,
                        int tw, int th, const Scale &sc,
                        int *dx, int *dy, int *dext) {
    if (lf.flat || lf.domain < 0) return false;
    const int step = step_for(p, lf.size);
    int gw, gh;
    pool_grid(tw, th, lf.size, step, &gw, &gh);
    if (gw <= 0) return false;
    /* The grid is resolved in the tile's own coordinates and only then mapped,
     * because the step can be as small as one pixel and a reduced scale would
     * otherwise divide the grid spacing itself away. */
    *dx   = sc.pos((lf.domain % gw) * step);
    *dy   = sc.pos((lf.domain / gw) * step);
    /* What the renderer reads: twice the rendered block, which at a reduced
     * scale is clamped to at least one sample - so 2 even where scaling the
     * 2N extent itself would round down to 1. */
    *dext = std::max(sc.len(2 * lf.size), 2 * sc.len(lf.size));
    /* Rounding the origin down can push the 2N square a pixel past the plane
     * when the size was clamped; the read is bounded here rather than at every
     * caller.  Only reachable when reducing - an upscale divides nothing. */
    if (sc.down()) {
        const int pw = sc.pos(tw), ph = sc.pos(th);
        if (*dx + *dext > pw) *dx = pw - *dext;
        if (*dy + *dext > ph) *dy = ph - *dext;
        if (*dx < 0) *dx = 0;
        if (*dy < 0) *dy = 0;
    }
    return true;
}

/* The gradient a planar leaf adds on top of its DC at (x, y) inside a block of
 * n samples.  Returns zero for every other leaf, so renderers can add it
 * unconditionally.  The basis is centred, so the block mean is still exactly
 * the coded DC - which is what keeps the DC predictors honest. */
inline float planar_offset(const Leaf &lf, const CodecParams &p,
                           int x, int y, int n) {
    if (!lf.planar) return 0.f;
    const float step = 255.0f / (float)(1 << p.dc_bits);
    const float ux = ((float)x + 0.5f) / (float)n - 0.5f;
    const float uy = ((float)y + 0.5f) / (float)n - 0.5f;
    return step * ((float)lf.gx_q * ux + (float)lf.gy_q * uy);
}

/* Sum of the squared centred ramp over one axis of an n-sample block, the
 * denominator of the planar least squares fit. */
inline double planar_axis_energy(int n) {
    double s = 0;
    for (int i = 0; i < n; ++i) {
        const double u = ((double)i + 0.5) / n - 0.5;
        s += u * u;
    }
    return s;
}

/* Chroma predicted from the reconstructed luma plane rather than from its own.
 * Changes what the chroma trees mean, so both sides read it from the stream. */
inline bool chroma_from_luma(const CodecParams &p) {
    return p.chroma_mode == FIF_CHROMA_FROM_LUMA;
}

/* Chroma plane dimensions relative to luma. */
inline int chroma_shift(const CodecParams &p) {
    return p.subsampling == FIF_SUBSAMPLING_420 ? 1 : 0;
}

/* Range block sizes for the chroma planes.  With 4:2:0 they are half the luma
 * sizes and may reach 2x2; blocks below 4 pixels fall back to a local-window
 * search because the 4x4 feature reduction has nothing to reduce. Both chroma
 * modes use the same range so that the only difference measured between them
 * is the inheritance itself. */
inline void chroma_block_range(const CodecParams &p, int *cmin, int *cmax) {
    const int cs = chroma_shift(p);
    *cmax = p.max_block >> cs;
    *cmin = p.min_block >> cs;
    if (*cmin < 2) *cmin = 2;
    if (*cmax < *cmin) *cmax = *cmin;
}

/* Encodes one tile into its base and detail layers.  `force_strategy` is -1
 * to try both candidate sources and report which won in `won`, or 0/1 to
 * commit to one of them. */
void encode_tile(const Plane *planes, int nplanes, Tile &tile,
                 const CodecParams &p, EncodeCounters &ctr,
                 std::vector<uint8_t> &base, std::vector<uint8_t> &detail,
                 SymbolStats &sym_out,
                 int force_strategy = -1, int *won = nullptr);

/* The payload splits into two layers.  `base` carries the partition, the
 * per-leaf mode and the DC; `detail` carries the domain, the dihedral
 * transform and the contrast.  The two use disjoint context models, so the
 * split costs only one coder flush per stream and nothing in adaptation - and
 * a reader holding only the base layers of every tile has a complete, blocky
 * picture rather than part of a sharp one. */
void serialize_tile(Tile &tile, int nplanes, const CodecParams &p,
                    std::vector<uint8_t> &base, std::vector<uint8_t> &detail,
                    SymbolStats *stats);

/* `detail` may be null or empty: every leaf then decodes as DC only. */
bool deserialize_tile(const uint8_t *base, size_t base_size,
                      const uint8_t *detail, size_t detail_size,
                      int nplanes, const CodecParams &p, Tile &tile);

/* Scratch that lives across the decoder's iterations, one set per tile.
 *
 * `half` is the 2:1 box reduction of the source plane.  Every domain is a
 * 2n x 2n region reduced that way, and the regions overlap heavily - at the
 * default grid density each source sample is averaged into sixteen different
 * domains - so reducing the plane once per iteration replaces four strided
 * reads and three adds per domain sample with one sequential read.  It is
 * bit-identical: the same expression in the same order. */
struct DecodeScratch {
    Plane half[3];
    std::vector<float> block;
};

/* One fixed-point iteration: reads domains from `src`, writes ranges to `dst`.
 * `scale` renders the attractor at an integer multiple of the coded resolution;
 * the fractal code carries no intrinsic scale, so this is exact, not resampling. */
void decode_tile(const Tile &tile, int nplanes, const CodecParams &p,
                 const Plane *src, Plane *dst, DecodeScratch &scratch,
                 const Scale &sc = Scale{});

/* Fills the chroma planes from the reconstructed luma plane, for streams coded
 * with FIF_CHROMA_FROM_LUMA.  Runs once, after luma has converged. */
void render_chroma_from_luma(const Tile &tile, int nplanes,
                             const CodecParams &p, const Plane &luma,
                             Plane *dst, const Scale &sc);

/* Paints every leaf with its coded mean: the collage's own low-frequency part,
 * carried exactly in the stream.  This is where the decode loop starts. */
void init_from_dc(const Tile &tile, int nplanes, const CodecParams &p,
                  Plane *dst, const Scale &sc);

/* The flat and planar leaves of plane c of a tile, at plane offset (ox, oy). */
void collect_flats(const Tile &tile, int c, const CodecParams &p, int ox, int oy,
                   const Scale &sc, std::vector<FlatRect> &out);
/* Records every leaf boundary of a decoded tile into the full-plane edge maps.
 * (ox, oy) is where the tile sits in the assembled plane, already scaled. */
void collect_edges(const Tile &tile, int nplanes, const CodecParams &p,
                   int ox, int oy, const Scale &sc, EdgeMap *maps);

/* Tile edge actually used: the requested size, rounded up to whole alignment
 * units.  Encoder and decoder must agree, so there is one of it. */
inline int effective_tile_size(int requested, int align) {
    return std::max(align, align_up(requested, align));
}

/* The tiles covering a padded pw x ph plane, in raster order; the last row and
 * column are clipped.  Not stored in the stream: both sides compute it. */
inline std::vector<Tile> make_tiles(int pw, int ph, int ts) {
    std::vector<Tile> tiles;
    for (int y = 0; y < ph; y += ts)
        for (int x = 0; x < pw; x += ts) {
            Tile t;
            t.x = x; t.y = y;
            t.w = std::min(ts, pw - x);
            t.h = std::min(ts, ph - y);
            tiles.push_back(t);
        }
    return tiles;
}

/* --- colour (image.cpp) --- */
void rgb_to_planes(const fif_image &img, Plane *pl, int pw, int ph);
/* 4:2:0 in one pass, without the full-resolution chroma planes in between. */
void rgb_to_planes_420(const fif_image &img, Plane *pl, int pw, int ph);
void gray_to_plane(const fif_image &img, Plane *pl, int pw, int ph);
/* How the planes become 8-bit pixels.  `guide`, when given for 4:2:0,
 * holds the slope planes of Cb and Cr from chroma_guide and the luma on the
 * chroma grid: chroma is then bilinear plus slope times the luma detail the
 * chroma grid cannot carry.  `dither` adds an ordered dither below one grey
 * level before rounding; (dither_x0, dither_y0) is where the output sits in
 * the whole picture, so that a region gets the pattern a full decode has. */
struct ConvertOpts {
    const Plane *guide = nullptr;
    bool dither = false;
    int dither_x0 = 0, dither_y0 = 0;
};
/* Rows are independent; [y0, y1) lets the caller split the work. */
void planes_to_image(const Plane *pl, int nplanes, int cs,
                     int w, int y0, int y1, fif_image &out, int px0 = 0, int py0 = 0,
                     const ConvertOpts *opt = nullptr);
/* Luma averaged down to the chroma grid. */
class ThreadPool;
void luma_to_chroma_grid(const Plane &Y, int cs, int w, int h, Plane &yd, ThreadPool &pool);
/* Edge-aware 4:2:0 upsampling, after the guided filter: per chroma window
 * the least-squares slope of chroma over luma, clamped to +-amax and
 * smoothed. */
void chroma_guide(const Plane &yd, const Plane &C, float eps, float amax, Plane &A,
                  ThreadPool &pool);

} /* namespace fif */
