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
#include "rangecoder.h"
#include <array>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <vector>

namespace fif {

/* Adaptive contexts, reset at the start of every tile so that tiles stay
 * independently decodable.  They start from trained probabilities (see
 * contexts_default_probs), so even a small tile does not pay for learning. */
struct Contexts {
    static constexpr int NL = 9;    /* log2 of the block edge, 0..8 */
    static constexpr int NP = 2;    /* 0 = luma, 1 = chroma (Cb and Cr share) */
    static constexpr int MAXLEN = 18;
    static constexpr int NACT = 4;   /* local activity classes */
    /* The contrast tree is indexed by 1..2^s_bits-1, so the array size caps how
     * wide a contrast field the format can carry.  The decoder enforces it:
     * s_bits arrives from the file and a larger value would index straight past
     * the struct. */
    static constexpr int MAX_S_BITS = 7;
    static constexpr int MAX_DC_BITS = 12;

    uint16_t split   [NP][NL][3];
    uint16_t mode    [NP][NL][3];
    uint16_t dc_zero [NP][NL][NACT];
    uint16_t dc_sign [NP][NACT];
    uint16_t dc_len  [NP][NACT][MAXLEN];
    /* The two components are coded independently.  Conditioning the vertical
     * one on the magnitude of the horizontal measured slightly worse: they are
     * not correlated enough to pay for three times the models. */
    uint16_t dd_zero [NP][2];
    uint16_t dd_sign [NP][2];
    uint16_t dd_len  [NP][2][MAXLEN];
    uint16_t sym     [NP][NL][8];
    /* Planar leaves cluster: a smooth region is made of them and a busy one has
     * none, so the flag is conditioned on how many of the coded neighbours were
     * planar, the same two-bit summary the mode flag uses. */
    uint16_t planar  [NP][NL][3];
    uint16_t g_zero  [NP][2];
    uint16_t g_sign  [NP][2];
    uint16_t g_len   [NP][2][MAXLEN];
    uint16_t contrast[NP][NL][1 << MAX_S_BITS];
    void reset();
};

/* Number of adaptive probabilities in one Contexts. */
constexpr size_t contexts_prob_count() { return sizeof(Contexts) / sizeof(uint16_t); }

/* Every context starts at one half, which costs real bits while the models
 * learn - measured at about 9% of the payload for a 128-pixel tile.  Seeding
 * them from probabilities trained offline removes that without transmitting
 * anything and without giving up tile independence. */
extern const uint16_t *contexts_default_probs();

/* Training hooks; inert unless the encoder was asked to collect statistics. */
void contexts_train_begin();
void contexts_train_add(const Contexts &c);
bool contexts_train_active();
bool contexts_train_write(const char *path);

/* Gradient-adjusted prediction, the median edge detector from JPEG-LS.  An
 * average of the two neighbours smears every edge it meets; this picks the
 * neighbour on the correct side of one instead, and falls back to the planar
 * estimate a+b-c where there is no edge to detect. */
inline int med_predict(int a, int b, int c) {
    const int lo = a < b ? a : b, hi = a < b ? b : a;
    if (c >= hi) return lo;
    if (c <= lo) return hi;
    return a + b - c;
}

/* How busy the neighbourhood is.  Residuals in flat regions are tiny and in
 * busy ones are not, and one shared model has to straddle both; splitting the
 * models by activity is the single most effective trick in lossless image
 * coding and costs one table lookup. */
inline int activity_class(int a, int b, int c, int dc_bits) {
    const int d = std::abs(a - c) + std::abs(b - c);
    const int unit = 1 << (dc_bits > 4 ? dc_bits - 4 : 0);
    if (d <= unit) return 0;
    if (d <= unit * 3) return 1;
    if (d <= unit * 8) return 2;
    return 3;
}

/* Per-plane record of what has already been coded, used for prediction.  The
 * decoder fills it exactly as the encoder does, so no side information. */
struct NeighborMap {
    struct Cell {
        int16_t size = 0;      /* 0 = not coded yet */
        int16_t dc   = 0;
        int16_t ox = 0, oy = 0;
        int16_t dpx = -1, dpy = -1;   /* domain position in pixels, -1 if absent */
        int16_t s_q = 0;              /* coded contrast                      */
        int8_t  sym = 0;              /* coded dihedral transform            */
        int8_t  fractal = 0;          /* the coded mode bit, base layer only */
        int8_t  planar = 0;           /* carried a gradient rather than one mean */
    };
    int cell = 4, cw = 0, ch = 0;
    std::vector<Cell> cells;

    void init(int tw, int th, int min_block) {
        cell = min_block < 1 ? 1 : min_block;
        cw = (tw + cell - 1) / cell;
        ch = (th + cell - 1) / cell;
        cells.assign((size_t)cw * ch, Cell{});
    }
    const Cell *at_px(int x, int y) const {
        if (x < 0 || y < 0) return nullptr;
        const int cx = x / cell, cy = y / cell;
        if (cx >= cw || cy >= ch) return nullptr;
        const Cell &c = cells[(size_t)cy * cw + cx];
        return c.size ? &c : nullptr;
    }
    void fill(int x, int y, int n, int dc, int fractal, int dpx, int dpy,
              int sym = 0, int s_q = 0, int planar = 0) {
        const int x0 = x / cell, y0 = y / cell;
        const int x1 = (x + n + cell - 1) / cell, y1 = (y + n + cell - 1) / cell;
        for (int cy = y0; cy < y1 && cy < ch; ++cy)
            for (int cx = x0; cx < x1 && cx < cw; ++cx) {
                Cell &c = cells[(size_t)cy * cw + cx];
                c.size = (int16_t)n;
                c.dc = (int16_t)dc;
                c.ox = (int16_t)x; c.oy = (int16_t)y;
                c.fractal = (int8_t)fractal;
                c.dpx = (int16_t)dpx; c.dpy = (int16_t)dpy;
                c.sym = (int8_t)sym; c.s_q = (int16_t)s_q;
                c.planar = (int8_t)planar;
            }
    }
};

/* Contrast and the dihedral transform are coded under a block-size context
 * only.  Conditioning either on the neighbouring block, and contrast
 * separately on the local activity, all measured no better than this: the
 * extra models dilute faster than the added information pays back, which says
 * both fields are already close to their conditional entropy. */

/* Where the bits went, in the coder's own units, as fif_stats reports it. */
struct SymbolStats {
    double split = 0, mode = 0, dc = 0, domain = 0, sym = 0, contrast = 0;
    double planar = 0, grad = 0;
    /* Squared error of what was actually coded, after the quantisation search
     * revisited the values.  The figure the encoder decided on beforehand no
     * longer matches, and comparing it against a payload size measured after
     * the fact silently mis-ranks the candidate search strategies. */
    double distortion = 0;

    void operator+=(const SymbolStats &o) {
        split += o.split; mode += o.mode; dc += o.dc;
        domain += o.domain; sym += o.sym; contrast += o.contrast;
        planar += o.planar; grad += o.grad;
        distortion += o.distortion;
    }
};

/* What one modelled bit costs, for every probability the coder can hold.
 *
 * The encoder charges this on every bit it writes, and the quantisation search
 * prices several hundred hypothetical ones per leaf without writing any.  At
 * eleven bits the whole domain is a two-thousand entry table, so the logarithm
 * is computed once per process rather than once per bit. */
inline const std::array<double, kProbOne + 1> kBitCost = [] {
    std::array<double, kProbOne + 1> t{};
    for (int i = 1; i <= kProbOne; ++i)
        t[i] = -std::log2((double)i / (double)kProbOne);
    return t;                       /* t[0] is unreachable: p never hits zero */
}();

inline double bit_cost(uint16_t p, int b) {
    return kBitCost[b ? (size_t)(kProbOne - p) : (size_t)p];
}

inline void enc_bit(RangeEncoder &e, uint16_t &p, int b, double *cost) {
    if (cost) *cost += bit_cost(p, b);
    e.bit(p, b);
}
inline void enc_direct(RangeEncoder &e, uint32_t v, int n, double *cost) {
    if (cost) *cost += n;
    e.direct(v, n);
}

inline int bit_length(uint32_t m) {
    int k = 0;
    while (m) { ++k; m >>= 1; }
    return k;
}

/* The two composite symbols, each written once for both of its users: the
 * coder, and the quantisation search that prices alternatives without coding
 * them.  bit(p, b) handles one modelled bit, raw(v, n) n uniform ones. */

/* MSB-first adaptive tree over 2^nbits symbols; probs has 1 << nbits entries. */
template <class P, class Bit>
inline void walk_tree(P *probs, int nbits, uint32_t sym, Bit &&bit) {
    uint32_t m = 1;
    for (int i = nbits - 1; i >= 0; --i) {
        const int b = (sym >> i) & 1;
        bit(probs[m], b);
        m = (m << 1) | (uint32_t)b;
    }
}

/* Signed residual: zero flag, sign, then an Elias-gamma style magnitude whose
 * length prefix is adaptive and whose mantissa is uniform.  The length prefix
 * is where the modelling pays - residual magnitudes are strongly peaked. */
template <class P, class Bit, class Raw>
inline void walk_signed(P &zero, P &sign, P *len, int max_len, int value,
                        Bit &&bit, Raw &&raw) {
    if (value == 0) { bit(zero, 0); return; }
    bit(zero, 1);
    bit(sign, value < 0 ? 1 : 0);
    const uint32_t m = (uint32_t)(value < 0 ? -value : value);
    int k = bit_length(m);
    if (k > max_len) k = max_len;
    for (int i = 0; i < k - 1; ++i) bit(len[i], 1);
    if (k < max_len) bit(len[k - 1], 0);
    if (k > 1) raw(m & ((1u << (k - 1)) - 1), k - 1);
}

inline void enc_tree(RangeEncoder &e, uint16_t *probs, int nbits, uint32_t sym,
                     double *cost) {
    walk_tree(probs, nbits, sym, [&](uint16_t &p, int b) { enc_bit(e, p, b, cost); });
}
inline void enc_signed(RangeEncoder &e, uint16_t &zero, uint16_t &sign,
                       uint16_t *len, int max_len, int value, double *cost) {
    walk_signed(zero, sign, len, max_len, value,
                [&](uint16_t &p, int b) { enc_bit(e, p, b, cost); },
                [&](uint32_t v, int n) { enc_direct(e, v, n, cost); });
}
inline double cost_tree(const uint16_t *probs, int nbits, uint32_t sym) {
    double c = 0;
    walk_tree(probs, nbits, sym, [&](const uint16_t &p, int b) { c += bit_cost(p, b); });
    return c;
}
inline double cost_signed(const uint16_t &zero, const uint16_t &sign,
                          const uint16_t *len, int max_len, int value) {
    double c = 0;
    walk_signed(zero, sign, len, max_len, value,
                [&](const uint16_t &p, int b) { c += bit_cost(p, b); },
                [&](uint32_t, int n) { c += n; });
    return c;
}

inline int dec_signed(RangeDecoder &d, uint16_t &zero, uint16_t &sign,
                      uint16_t *len, int max_len) {
    if (!d.bit(zero)) return 0;
    const int neg = d.bit(sign);
    int k = 1;
    while (k < max_len && d.bit(len[k - 1])) ++k;
    uint32_t m = 1u << (k - 1);
    if (k > 1) m |= d.direct(k - 1);
    return neg ? -(int)m : (int)m;
}

} /* namespace fif */
