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

/* Reconstruction: turns decoded quadtrees into pixels.  One application of the
 * fractal map per call to decode_tile; the iteration around it is in
 * decode.cpp.
 *
 * Every renderer here walks the pre-order node vector directly.  A leaf
 * carries its own position, so the tree structure is not needed to place it,
 * and the leaves of one plane never overlap at full scale. */
#include "codec.h"
#include <algorithm>

namespace fif {
namespace {

/* Paints a leaf that carries no transform: its coded mean, plus the coded
 * gradient where there is one. */
void fill_flat(const Leaf &lf, const CodecParams &p, Plane &dst,
               int x, int y, int sz) {
    const float dc = dequant_dc(lf.dc_q, p);
    if (!lf.planar) {
        for (int j = 0; j < sz; ++j)
            std::fill_n(dst.row(y + j) + x, sz, dc);
        return;
    }
    for (int j = 0; j < sz; ++j) {
        float *r = dst.row(y + j) + x;
        for (int i = 0; i < sz; ++i)
            r[i] = dc + planar_offset(lf, p, i, j, sz);
    }
}

/* The 2:1 box reduction the domains are read from.  See DecodeScratch. */
void build_half(const Plane &src, Plane &half) {
    const int hw = src.w / 2, hh = src.h / 2;
    if (half.w != hw || half.h != hh) half.resize(hw, hh);
    for (int y = 0; y < hh; ++y) {
        const float *r0 = src.row(2 * y), *r1 = src.row(2 * y + 1);
        float *o = half.row(y);
        for (int x = 0; x < hw; ++x)
            o[x] = 0.25f * (r0[2 * x] + r0[2 * x + 1] + r1[2 * x] + r1[2 * x + 1]);
    }
}

void render_leaf(const Leaf &lf, int ox, int oy, int tw, int th,
                 const CodecParams &p, const Plane &src, const Plane &half,
                 Plane &dst, std::vector<float> &scratch, const Scale &sc) {
    const int sz = sc.len(lf.size);
    const int lx = sc.pos(lf.x), ly = sc.pos(lf.y);

    int rdx, rdy, rdext;
    if (!domain_rect(lf, p, tw, th, sc, &rdx, &rdy, &rdext)) {
        fill_flat(lf, p, dst, ox + lx, oy + ly, sz);
        return;
    }
    const int dx = ox + rdx;
    const int dy = oy + rdy;

    /* The reduced plane already holds the domain whenever the grid offset is
     * even, which it is for every step the format admits above a one-pixel
     * grid.  The odd case still has to average its own. */
    const size_t bn = (size_t)sz * sz;
    const float *dom;
    int dstride;
    if (((dx | dy) & 1) == 0) {
        dom = half.row(dy >> 1) + (dx >> 1);
        dstride = half.w;
    } else {
        scratch.resize(bn);
        decimate(src, dx, dy, sz, scratch.data());
        dom = scratch.data();
        dstride = sz;
    }
    /* A dihedral transform is a gather through a table of flat offsets, so a
     * strided domain has to be made contiguous first.  The identity needs no
     * gather at all. */
    if (lf.sym != 0 && dstride != sz) {
        scratch.resize(bn);
        for (int j = 0; j < sz; ++j)
            std::copy_n(dom + (size_t)j * dstride, sz,
                        scratch.data() + (size_t)j * sz);
        dom = scratch.data();
        dstride = sz;
    }

    float m = 0;
    for (int j = 0; j < sz; ++j) {
        const float *r = dom + (size_t)j * dstride;
        for (int k = 0; k < sz; ++k) m += r[k];
    }
    m /= (float)bn;

    const float dc = dequant_dc(lf.dc_q, p);
    const float s = dequant_s(lf.s_q, p);
    if (lf.sym == 0) {
        for (int y = 0; y < sz; ++y) {
            const float *sr = dom + (size_t)y * dstride;
            float *r = dst.row(oy + ly + y) + ox + lx;
            for (int x = 0; x < sz; ++x) r[x] = dc + s * (sr[x] - m);
        }
        return;
    }
    const SymIndex ix(lf.sym, sz);
    for (int y = 0; y < sz; ++y) {
        float *r = dst.row(oy + ly + y) + ox + lx;
        if (ix.tbl) {                      /* tabulated fast path */
            const int16_t *row_ix = ix.tbl + (size_t)y * sz;
            for (int x = 0; x < sz; ++x)
                r[x] = dc + s * (dom[row_ix[x]] - m);
        } else {
            for (int x = 0; x < sz; ++x)
                r[x] = dc + s * (dom[ix(x, y)] - m);
        }
    }
}

/* Paints one chroma leaf from the reconstructed luma plane.
 *
 * Under FIF_CHROMA_FROM_LUMA a chroma leaf's domain is the luma block it sits
 * on top of, so there is nothing to search for and nothing to iterate: the
 * chroma planes are a function of the converged luma, evaluated once.  Under
 * 4:2:0 that co-located luma block is twice the size in each direction and is
 * decimated exactly as any other domain would be; under 4:4:4 it is read
 * straight across. */
void render_from_luma(const Leaf &lf, const CodecParams &p, const Plane &luma,
                      Plane &dst, int cs, const Scale &sc,
                      std::vector<float> &scratch) {
    const int sz = sc.len(lf.size);
    const int lx = sc.pos(lf.x), ly = sc.pos(lf.y);
    /* A leaf coded with a transform whose detail layer did not arrive has no
     * contrast to apply - base-only or a cut-off file - and renders as its
     * mean, like every other leaf without a domain. */
    if (lf.flat || lf.domain < 0) {
        fill_flat(lf, p, dst, lx, ly, sz);
        return;
    }

    const size_t bn = (size_t)sz * sz;
    scratch.resize(bn);
    luma_block(luma, lx, ly, sz, cs, scratch.data());

    float m = 0;
    for (size_t k = 0; k < bn; ++k) m += scratch[k];
    m /= (float)bn;

    const float dc = dequant_dc(lf.dc_q, p);
    const float s = dequant_s(lf.s_q, p);
    for (int j = 0; j < sz; ++j) {
        float *r = dst.row(ly + j) + lx;
        const float *src = scratch.data() + (size_t)j * sz;
        for (int k = 0; k < sz; ++k) r[k] = dc + s * (src[k] - m);
    }
}

/* Calls fn(leaf) for every leaf of a pre-order tree. */
template <class F>
void for_each_leaf(const std::vector<QNode> &t, F &&fn) {
    for (const QNode &nd : t)
        if (!nd.split) fn(nd.leaf);
}

} /* namespace */

void init_from_dc(const Tile &tile, int nplanes, const CodecParams &p,
                  Plane *dst, const Scale &sc) {
    for (int c = 0; c < nplanes; ++c)
        for_each_leaf(tile.trees[c], [&](const Leaf &lf) {
            fill_flat(lf, p, dst[c], sc.pos(lf.x), sc.pos(lf.y), sc.len(lf.size));
        });
}

void collect_flats(const Tile &tile, int c, const CodecParams &p, int ox, int oy,
                   const Scale &sc, std::vector<FlatRect> &out) {
    for_each_leaf(tile.trees[c], [&](const Leaf &lf) {
        if (lf.flat)
            out.push_back(FlatRect{ox + sc.pos(lf.x), oy + sc.pos(lf.y),
                                   sc.len(lf.size)});
    });
}

void collect_edges(const Tile &tile, int nplanes, const CodecParams &p,
                   int ox, int oy, const Scale &sc, EdgeMap *maps) {
    const int cs = chroma_shift(p);
    for (int c = 0; c < nplanes; ++c) {
        const int x0 = c ? (ox >> cs) : ox, y0 = c ? (oy >> cs) : oy;
        for_each_leaf(tile.trees[c], [&](const Leaf &lf) {
            const uint8_t k = !lf.flat ? EdgeMap::FRACTAL
                            : lf.planar ? EdgeMap::PLANAR : EdgeMap::FLAT;
            maps[c].mark(x0 + sc.pos(lf.x), y0 + sc.pos(lf.y), sc.len(lf.size), k);
        });
    }
}

void render_chroma_from_luma(const Tile &tile, int nplanes,
                             const CodecParams &p, const Plane &luma,
                             Plane *dst, const Scale &sc) {
    const int cs = chroma_shift(p);
    std::vector<float> scratch;
    for (int c = 1; c < nplanes; ++c)
        for_each_leaf(tile.trees[c], [&](const Leaf &lf) {
            render_from_luma(lf, p, luma, dst[c], cs, sc, scratch);
        });
}

void decode_tile(const Tile &tile, int nplanes, const CodecParams &p,
                 const Plane *src, Plane *dst, DecodeScratch &bb,
                 const Scale &sc) {
    const int cs = chroma_shift(p);
    for (int c = 0; c < nplanes; ++c) {
        const int sh = c ? cs : 0;
        build_half(src[c], bb.half[c]);
        const int ox = sc.pos(tile.x >> sh), oy = sc.pos(tile.y >> sh);
        const int tw = tile.w >> sh, th = tile.h >> sh;
        for_each_leaf(tile.trees[c], [&](const Leaf &lf) {
            render_leaf(lf, ox, oy, tw, th, p, src[c], bb.half[c], dst[c],
                        bb.block, sc);
        });
    }
}

} /* namespace fif */
