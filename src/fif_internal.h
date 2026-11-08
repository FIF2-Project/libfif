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
#include "fif/fif.h"
#include <cstdint>
#include <cstring>
#include <vector>
#include <cmath>

namespace fif {

/* --- dihedral group D4 ------------------------------------------------- *
 * sym(g) maps a DESTINATION coordinate to the SOURCE coordinate it reads. */
inline void sym_map(int g, int x, int y, int n, int *sx, int *sy) {
    const int m = n - 1;
    switch (g) {
        case 0: *sx = x;     *sy = y;     break; /* identity        */
        case 1: *sx = y;     *sy = m - x; break; /* rot 90          */
        case 2: *sx = m - x; *sy = m - y; break; /* rot 180         */
        case 3: *sx = m - y; *sy = x;     break; /* rot 270         */
        case 4: *sx = m - x; *sy = y;     break; /* mirror x        */
        case 5: *sx = y;     *sy = x;     break; /* transpose       */
        case 6: *sx = x;     *sy = m - y; break; /* mirror y        */
        default:*sx = m - y; *sy = m - x; break; /* anti-transpose  */
    }
}
/* g composed with sym_inverse[g] is the identity. */
inline constexpr int sym_inverse[8] = {0, 3, 2, 1, 4, 5, 6, 7};

/* Precomputed destination-to-source offsets for one transform at one block
 * size.  Both the encoder's range preparation and the decoder's inner render
 * loop walk a whole block through sym_map; a table turns that per-pixel switch
 * and multiply into a load.  Returns null outside the tabulated sizes. */
const int16_t *sym_indices(int g, int n);

/* Destination-to-source lookup that works at any block size.  The table covers
 * everything a normal stream asks for, but it cannot cover everything that is
 * reachable: a 32-pixel leaf rendered at 8x is 256 wide, and tabulating up to
 * there would cost tens of megabytes for sizes almost nobody decodes.  Outside
 * the table the mapping is computed - a predictable branch and a switch, on a
 * path that by definition runs on very few blocks. */
struct SymIndex {
    const int16_t *tbl;
    int g, n;
    SymIndex(int g_, int n_) : tbl(sym_indices(g_, n_)), g(g_), n(n_) {}
    int operator()(int x, int y) const {
        if (tbl) return tbl[(size_t)y * n + x];
        int sx, sy;
        sym_map(g, x, y, n, &sx, &sy);
        return sy * n + sx;
    }
};

struct Plane {
    int w = 0, h = 0;
    std::vector<float> px;          /* row-major, nominal range 0..255 */
    void resize(int W, int H, float v = 0.f) {
        w = W; h = H;
        px.assign((size_t)W * H, v);
    }
    float *row(int y) { return px.data() + (size_t)y * w; }
    const float *row(int y) const { return px.data() + (size_t)y * w; }
};

/* One coded leaf of the range quadtree. */
struct Leaf {
    int32_t x, y;          /* position inside the tile        */
    int32_t size;          /* range block edge                */
    int32_t domain;        /* domain index, -1 when flat      */
    uint8_t sym;           /* dihedral transform 0..7         */
    uint8_t flat;          /* 1 = no transform: DC, or DC+gradient */
    uint8_t planar;        /* 1 = flat leaf carries a gradient */
    int32_t dc_q;          /* quantised block mean            */
    int32_t s_q;           /* quantised contrast              */
    /* End-to-end rise of the planar mode across the block, in DC quantiser
     * steps.  The mean stays in dc_q, so every predictor that works on the DC
     * field keeps working unchanged. */
    int32_t gx_q, gy_q;

    /* Encoder-side only, never serialised: what the quantised values were
     * derived from, so the serialiser can revisit the choice once it knows
     * what each alternative would actually cost to code. */
    float mean = 0;        /* true block mean                 */
    float rr = 0;          /* energy of the centred block     */
    float xd = 0;          /* inner product with the domain   */
    float nd = 0;          /* energy of the centred domain    */
    float planar_rr = 0;   /* residual the coded gradient leaves */
};

/* Coding parameters.  The ones the decoder needs travel in the header; the
 * search shape is encoder-side only. */
struct CodecParams {
    int quality      = 75;
    int effort       = 5;
    int tile_size    = 512;
    int min_block    = 4;
    int max_block    = 32;
    int subsampling  = FIF_SUBSAMPLING_420;
    int chroma_mode  = FIF_CHROMA_SHARED;
    /* Let a leaf with no transform carry a linear gradient instead of a single
     * mean.  Most leaves in a photograph are exactly that - a piece of a smooth
     * ramp - and without it the quadtree has to keep splitting to approximate
     * one with steps.  Travels in the stream: it is a syntax element. */
    bool planar_mode = true;
    int dc_bits      = 8;
    int s_bits       = 5;
    int domain_shift = 1;   /* domain step = (2*n) >> domain_shift */
    /* Search shape. Encoder-side only - the decoder never needs to know how a
     * match was found, only which domain it was. Not carried in the stream. */
    int local_radius   = 8;
    int kd_candidates  = 24;
    bool exhaustive    = false;
};

/* Contrast range.  Fixed at 1 so every map contracts; not stored. */
static constexpr float kSMax = 1.0f;

/* Block and tile sizes both sides accept.  The encoder refuses anything else
 * up front, and the reader refuses a header that asks for it: max_block is
 * bounded by the context tables (Contexts::NL), and every size must be a
 * power of two for the quadtree to tile the plane. */
inline bool valid_block_sizes(int min_block, int max_block) {
    const auto pow2 = [](int v) { return v > 0 && (v & (v - 1)) == 0; };
    return pow2(min_block) && pow2(max_block) && min_block >= 2 &&
           max_block <= 256 && min_block <= max_block;
}

/* ceil(log2(v)) for v >= 1. */
inline int ilog2_ceil(uint32_t v) {
    int b = 0;
    while ((1u << b) < v) ++b;
    return b;
}

/* Full-range BT.601 luma, the one the codec uses everywhere. */
inline float luma601(float r, float g, float b) {
    return 0.299f * r + 0.587f * g + 0.114f * b;
}

/* Chroma errors are less visible than luma ones, so chroma is coded at a
 * looser operating point.  One named constant rather than a 0.6 sprinkled
 * through the encoder. */
static constexpr double kChromaLambdaScale = 0.6;

inline int step_for(const CodecParams &p, int n) {
    int st = (2 * n) >> p.domain_shift;
    return st < 1 ? 1 : st;
}

/* The render scale, as a rational num/den.  At most one of them is above 1;
 * num is any integer up to 16, den a power of two up to 8.
 *
 * A reduction needs the distinction this type makes:
 * pos() maps a coordinate, len() a size, and they are *not* the same map.  A
 * size is clamped at one pixel, because a range block thinner than a pixel has
 * nowhere to go; a coordinate is free to round down onto the pixel grid.
 *
 * That clamp is the whole reason a reduction is an approximation rather than a
 * cheaper route to the same picture.  Once blocks hit the clamp, several
 * leaves land on the same pixel and the last one rendered wins - and the
 * domain grid, whose step can be as fine as one pixel, no longer divides, so
 * the domains are read from rounded addresses.  Both effects are bounded and
 * measured. */
struct Scale {
    int num = 1, den = 1;
    int  pos(int v) const { return v * num / den; }
    int  len(int v) const { const int s = v * num / den; return s > 0 ? s : 1; }
    bool down()     const { return den > 1; }
};

/* Domain grid geometry.  Encoder and decoder derive it from the tile
 * dimensions, so it is never stored. */
inline void pool_grid(int tw, int th, int n, int step, int *gw, int *gh) {
    const int dn = 2 * n;
    if (tw < dn || th < dn) { *gw = 0; *gh = 0; return; }
    *gw = (tw - dn) / step + 1;
    *gh = (th - dn) / step + 1;
}

inline int align_up(int v, int a) { return ((v + a - 1) / a) * a; }

inline int bits_for(uint32_t count) { return count <= 1 ? 0 : ilog2_ceil(count); }

inline float dequant_s(int q, const CodecParams &p) {
    const int levels = 1 << p.s_bits;
    return (float)(2.0 * kSMax * (q + 0.5) / levels - kSMax);
}
inline int quant_s(float s, const CodecParams &p) {
    const int levels = 1 << p.s_bits;
    int q = (int)std::floor((s + kSMax) / (2.0f * kSMax) * levels);
    return q < 0 ? 0 : (q >= levels ? levels - 1 : q);
}
inline float dequant_dc(int q, const CodecParams &p) {
    const int levels = 1 << p.dc_bits;
    return (float)((q + 0.5) * 255.0 / levels);
}
inline int quant_dc(float dc, const CodecParams &p) {
    const int levels = 1 << p.dc_bits;
    int q = (int)std::floor(dc / 255.0f * levels);
    return q < 0 ? 0 : (q >= levels ? levels - 1 : q);
}

/* quality 0..100 -> lagrange multiplier, geometric interpolation */
inline double lambda_for_quality(double q) {
    const double lo = 0.40, hi = 4000.0;
    double t = (100.0 - q) / 100.0;
    return lo * std::pow(hi / lo, t);
}

} /* namespace fif */
