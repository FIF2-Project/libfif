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

/* Blocks are reduced to a 4x4 average, centred, brought into a canonical
 * orientation and unit-normalised before the 2D-DCT; the 8 lowest AC
 * coefficients form the search key.  Saupe's result applies: for centred,
 * unit-norm vectors the least-squares error is a monotone function of the
 * euclidean distance, so nearest-neighbour search in this space *is* the
 * search for the best affine match.  Canonicalising the orientation makes the
 * key invariant under the 8 dihedral transforms, which keeps the tree at one
 * entry per domain instead of eight. */
static constexpr int FEAT_DIM = 8;

struct KdTree {
    /* Deepest deferred far-side chain the query stack has to hold.  The tree
     * halves at every level, so this covers 2^48 points. */
    static constexpr int kMaxDepth = 48;
    struct Node {
        float   split = 0.f;
        int32_t dim   = -1;      /* -1 marks a leaf */
        int32_t left = -1, right = -1;
        int32_t begin = 0, end = 0;
    };
    std::vector<Node>    nodes;
    /* order[i] is the point that slot i of the tree holds; pts_ holds that
     * point's key.  Storing the keys in tree order rather than indirecting
     * through order[] on every distance is what makes a leaf scan sequential:
     * at the smallest block size a tile's pool is tens of thousands of keys,
     * far past any cache, and one random 32-byte read per candidate was the
     * single most expensive thing the encoder did. */
    std::vector<int32_t> order;
    std::vector<float>   pts_;
    int                  count = 0;

    /* Takes a copy of the keys; the caller may release its own afterwards. */
    void build(const float *points, int n);
    /* Appends up to k nearest point indices to `out` (unsorted, may contain
     * entries already present from a previous call - the caller dedups).
     *
     * Distances are deliberately not exposed.  They look like they should bound
     * how good a match a candidate can be, but the key is made invariant to the
     * eight dihedral transforms by canonicalising the orientation, and the
     * canonical alignment is not always the best of the eight - so the bound
     * does not hold.  Using it to stop the scan early measured 0.85 dB worse. */
    void knn(const float *q, int k, std::vector<int32_t> &out) const;

private:
    int32_t build_range(int32_t begin, int32_t end);
    const float *build_ = nullptr;   /* the caller's keys, during build only */
};

/* Domain pool for one range size inside one tile. */
struct BlockPool {
    int n = 0;              /* range edge; domains are 2n, decimated to n */
    int step = 0;
    int grid_w = 0, grid_h = 0;
    int count = 0;
    int idx_bits = 0;
    std::vector<float> data;    /* count * n*n, centred */
    std::vector<float> norm2;   /* count */
    std::vector<float> feat;    /* count * FEAT_DIM, freed once built */
    KdTree tree;

    const float *block(int i) const { return data.data() + (size_t)i * n * n; }
    void build(const Plane &pl, int tx, int ty, int tw, int th,
               int range_n, int step_, bool with_tree);
};

/* Decimate a 2n x 2n region of `pl` at (x,y) into an n x n block by 2x2
 * averaging.  Shared by encoder and decoder so both see identical domains. */
void decimate(const Plane &pl, int x, int y, int n, float *out);

/* The n x n block of `pl` at (x,y), row-major. */
void copy_block(const Plane &pl, int x, int y, int n, float *out);

/* The luma block under the n x n chroma block at chroma (x,y): under 4:2:0
 * (cs 1) twice the size and decimated like any domain, under 4:4:4 read
 * straight across. */
inline void luma_block(const Plane &luma, int x, int y, int n, int cs, float *out) {
    if (cs) decimate(luma, x << cs, y << cs, n, out);
    else    copy_block(luma, x, y, n, out);
}

/* Subtracts the mean from n samples and returns it. */
inline float centre(float *v, size_t n) {
    float m = 0;
    for (size_t k = 0; k < n; ++k) m += v[k];
    m /= (float)n;
    for (size_t k = 0; k < n; ++k) v[k] -= m;
    return m;
}

/* Centred 4x4 reduction -> canonical orientation -> unit norm -> DCT key. */
void make_feature(const float *blk, int n, float *feat_pos, float *feat_neg);

} /* namespace fif */
