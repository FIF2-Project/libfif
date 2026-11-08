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

#include "pool.h"
#include "kernels.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace fif {

void decimate(const Plane &pl, int x, int y, int n, float *out) {
    for (int j = 0; j < n; ++j) {
        const float *r0 = pl.row(y + 2 * j) + x;
        const float *r1 = pl.row(y + 2 * j + 1) + x;
        float *o = out + (size_t)j * n;
        for (int i = 0; i < n; ++i)
            o[i] = 0.25f * (r0[2 * i] + r0[2 * i + 1] + r1[2 * i] + r1[2 * i + 1]);
    }
}

void copy_block(const Plane &pl, int x, int y, int n, float *out) {
    for (int j = 0; j < n; ++j)
        std::copy_n(pl.row(y + j) + x, n, out + (size_t)j * n);
}

/* ---------------------------------------------------------------- feature */

static constexpr int kZigzag[8][2] = { /* (row, col) of AC coefficients 1..8 */
    {0,1},{1,0},{2,0},{1,1},{0,2},{0,3},{1,2},{2,1}
};

/* The basis, built once before main.  It used to be a `static bool init` guard
 * inside the function, which every encoder thread raced through on a cold
 * cache: two of them could be halfway into filling the same table while a
 * third read it. */
struct Dct4Basis {
    float c[4][4];
    Dct4Basis() {
        for (int u = 0; u < 4; ++u)
            for (int x = 0; x < 4; ++x)
                c[u][x] = (u == 0 ? 0.5f : 0.70710678f) *
                          std::cos((2 * x + 1) * u * 3.14159265358979f / 8.0f);
    }
};
static const Dct4Basis kDct4;

static void dct4x4(const float *in, float *out) {
    const auto &C = kDct4.c;
    float tmp[16];
    for (int y = 0; y < 4; ++y)          /* rows */
        for (int u = 0; u < 4; ++u) {
            float s = 0;
            for (int x = 0; x < 4; ++x) s += C[u][x] * in[y * 4 + x];
            tmp[y * 4 + u] = s;
        }
    for (int u = 0; u < 4; ++u)          /* columns */
        for (int v = 0; v < 4; ++v) {
            float s = 0;
            for (int y = 0; y < 4; ++y) s += C[v][y] * tmp[y * 4 + u];
            out[v * 4 + u] = s;
        }
}

/* Quadrant-sum ordering picks one representative per dihedral orbit. */
static int canonical_sym(const float *c4) {
    float q[4] = {0, 0, 0, 0};
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            q[(y / 2) * 2 + (x / 2)] += c4[y * 4 + x];

    int best = 0;
    float bt[4];
    for (int k = 0; k < 4; ++k) {
        int sx, sy;
        sym_map(0, k % 2, k / 2, 2, &sx, &sy);
        bt[k] = q[sy * 2 + sx];
    }
    for (int g = 1; g < 8; ++g) {
        float t[4];
        for (int k = 0; k < 4; ++k) {
            int sx, sy;
            sym_map(g, k % 2, k / 2, 2, &sx, &sy);
            t[k] = q[sy * 2 + sx];
        }
        for (int k = 0; k < 4; ++k) {
            if (t[k] < bt[k] - 1e-6f) { best = g; std::copy(t, t + 4, bt); break; }
            if (t[k] > bt[k] + 1e-6f) break;
        }
    }
    return best;
}

static void feature_from_c4(const float *c4, float *feat) {
    int g = canonical_sym(c4);
    float rot[16];
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            int sx, sy;
            sym_map(g, x, y, 4, &sx, &sy);
            rot[y * 4 + x] = c4[sy * 4 + sx];
        }
    float norm = std::sqrt(dot(rot, rot, 16));
    if (norm < 1e-6f) { std::fill(feat, feat + FEAT_DIM, 0.f); return; }
    float inv = 1.0f / norm;
    for (int i = 0; i < 16; ++i) rot[i] *= inv;
    float co[16];
    dct4x4(rot, co);
    for (int i = 0; i < FEAT_DIM; ++i)
        feat[i] = co[kZigzag[i][0] * 4 + kZigzag[i][1]];
}

void make_feature(const float *blk, int n, float *feat_pos, float *feat_neg) {
    float c4[16];
    if (n == 4) {
        std::copy(blk, blk + 16, c4);
    } else {
        const int b = n / 4;
        const float inv = 1.0f / (float)(b * b);
        for (int qy = 0; qy < 4; ++qy)
            for (int qx = 0; qx < 4; ++qx) {
                float s = 0;
                for (int y = 0; y < b; ++y) {
                    const float *r = blk + (size_t)(qy * b + y) * n + qx * b;
                    for (int x = 0; x < b; ++x) s += r[x];
                }
                c4[qy * 4 + qx] = s * inv;
            }
    }
    float m = 0;
    for (int i = 0; i < 16; ++i) m += c4[i];
    m *= 1.0f / 16.0f;
    for (int i = 0; i < 16; ++i) c4[i] -= m;

    if (feat_pos) feature_from_c4(c4, feat_pos);
    if (feat_neg) {
        float neg[16];
        for (int i = 0; i < 16; ++i) neg[i] = -c4[i];
        feature_from_c4(neg, feat_neg);
    }
}

/* ---------------------------------------------------------------- kd-tree */

/* One tile's pool at the smallest block size runs to tens of thousands of
 * keys, and in eight dimensions a kd-tree prunes weakly, so most of the query
 * ends up scanning leaves anyway.  A larger leaf trades branchy traversal for
 * a sequential, vectorised distance scan; the answer is the same either way,
 * exact nearest neighbours do not depend on how the tree was cut. */
static constexpr int kLeafSize = 48;

void KdTree::build(const float *points, int n) {
    count = n;
    nodes.clear();
    order.resize(n);
    std::iota(order.begin(), order.end(), 0);
    if (n == 0) { pts_.clear(); return; }

    /* Partitioning reads the keys through order[], so the build runs against
     * the caller's array; the tree-ordered copy is made once it is final. */
    build_ = points;
    nodes.reserve((size_t)(2 * n / kLeafSize + 4));
    build_range(0, n);
    build_ = nullptr;

    pts_.resize((size_t)n * FEAT_DIM);
    for (int32_t i = 0; i < n; ++i)
        std::copy_n(points + (size_t)order[i] * FEAT_DIM, FEAT_DIM,
                    pts_.data() + (size_t)i * FEAT_DIM);
}

int32_t KdTree::build_range(int32_t begin, int32_t end) {
    int32_t self = (int32_t)nodes.size();
    nodes.push_back(Node{});
    nodes[self].begin = begin;
    nodes[self].end = end;

    if (end - begin <= kLeafSize) return self;

    /* split on the dimension with the widest spread */
    float lo[FEAT_DIM], hi[FEAT_DIM];
    for (int d = 0; d < FEAT_DIM; ++d) { lo[d] = 1e30f; hi[d] = -1e30f; }
    for (int32_t i = begin; i < end; ++i) {
        const float *p = build_ + (size_t)order[i] * FEAT_DIM;
        for (int d = 0; d < FEAT_DIM; ++d) {
            lo[d] = std::min(lo[d], p[d]);
            hi[d] = std::max(hi[d], p[d]);
        }
    }
    int dim = 0;
    float spread = -1;
    for (int d = 0; d < FEAT_DIM; ++d)
        if (hi[d] - lo[d] > spread) { spread = hi[d] - lo[d]; dim = d; }
    if (spread <= 1e-9f) return self;   /* degenerate: keep as a leaf */

    int32_t mid = begin + (end - begin) / 2;
    std::nth_element(order.begin() + begin, order.begin() + mid, order.begin() + end,
                     [&](int32_t a, int32_t b) {
                         return build_[(size_t)a * FEAT_DIM + dim] <
                                build_[(size_t)b * FEAT_DIM + dim];
                     });
    float split = build_[(size_t)order[mid] * FEAT_DIM + dim];

    int32_t l = build_range(begin, mid);
    int32_t r = build_range(mid, end);
    nodes[self].dim = dim;
    nodes[self].split = split;
    nodes[self].left = l;
    nodes[self].right = r;
    return self;
}

namespace {
struct Heap {
    /* max-heap on distance, capacity k */
    float   *d;
    int32_t *i;
    int n = 0, k = 0;
    void push(float dist, int32_t idx) {
        if (n < k) {
            d[n] = dist; i[n] = idx; ++n;
            int c = n - 1;
            while (c > 0) {
                int p = (c - 1) / 2;
                if (d[p] >= d[c]) break;
                std::swap(d[p], d[c]); std::swap(i[p], i[c]); c = p;
            }
        } else if (dist < d[0]) {
            d[0] = dist; i[0] = idx;
            int p = 0;
            for (;;) {
                int a = 2 * p + 1, b = a + 1, m = p;
                if (a < n && d[a] > d[m]) m = a;
                if (b < n && d[b] > d[m]) m = b;
                if (m == p) break;
                std::swap(d[p], d[m]); std::swap(i[p], i[m]); p = m;
            }
        }
    }
    float worst() const { return n < k ? 1e30f : d[0]; }
};
} /* namespace */

void KdTree::knn(const float *q, int k, std::vector<int32_t> &out) const {
    if (count == 0 || k <= 0) return;
    if (k > 64) k = 64;
    if (k > count) k = count;

    float   hd[64];
    int32_t hi[64];
    Heap heap{hd, hi, 0, k};
    /* The search radius, kept in a register.  It only ever shrinks, and it is
     * read once per split plane and once per candidate, so reloading it out of
     * the heap each time is the difference between a compare and a dependent
     * load on the hottest path in the encoder. */
    float worst = 1e30f;

    /* Deferred far sides: the node and the distance to its cell. */
    struct Entry { int32_t node; float bound; };
    Entry stack[kMaxDepth];
    int sp = 0;

    int32_t cur = 0;
    float bound = 0.f;
    for (bool have = true; have; ) {
        /* Descend the near side directly rather than through the stack: the
         * near child is always the next node to look at, and pushing it only
         * to pop it again costs a store, a load and a branch per level. */
        for (;;) {
            const Node &nd = nodes[cur];
            if (nd.dim < 0) break;
            const float diff = q[nd.dim] - nd.split;
            const float far_bound = bound + diff * diff;
            const int32_t far = diff < 0 ? nd.right : nd.left;
            cur = diff < 0 ? nd.left : nd.right;
            if (far_bound < worst && sp < kMaxDepth)
                stack[sp++] = {far, far_bound};
        }
        const Node &leaf = nodes[cur];
        const float *pt = pts_.data() + (size_t)leaf.begin * FEAT_DIM;
        for (int32_t i = leaf.begin; i < leaf.end; ++i, pt += FEAT_DIM) {
            const float d = dist2(q, pt, FEAT_DIM);
            if (d < worst) { heap.push(d, order[i]); worst = heap.worst(); }
        }
        have = false;
        while (sp > 0) {                     /* the radius may have shrunk */
            const Entry e = stack[--sp];
            if (e.bound < worst) { cur = e.node; bound = e.bound; have = true; break; }
        }
    }
    for (int i = 0; i < heap.n; ++i) out.push_back(hi[i]);
}

/* ------------------------------------------------------------- block pool */

void BlockPool::build(const Plane &pl, int tx, int ty, int tw, int th,
                      int range_n, int step_, bool with_tree) {
    n = range_n;
    step = step_;
    pool_grid(tw, th, n, step, &grid_w, &grid_h);
    count = grid_w * grid_h;
    idx_bits = 0;
    if (count == 0) return;
    idx_bits = bits_for((uint32_t)count);

    const size_t bn = (size_t)n * n;
    data.resize((size_t)count * bn);
    norm2.resize(count);
    /* The keys only feed the kd-tree; a pool searched by window alone never
     * reads them. */
    if (with_tree) feat.resize((size_t)count * FEAT_DIM);

    for (int gy = 0; gy < grid_h; ++gy)
        for (int gx = 0; gx < grid_w; ++gx) {
            const int i = gy * grid_w + gx;
            float *blk = data.data() + (size_t)i * bn;
            decimate(pl, tx + gx * step, ty + gy * step, n, blk);
            centre(blk, bn);
            norm2[i] = dot(blk, blk, bn);
            if (with_tree)
                make_feature(blk, n, feat.data() + (size_t)i * FEAT_DIM, nullptr);
        }
    if (!with_tree) return;

    tree.build(feat.data(), count);
    /* The tree keeps its own tree-ordered copy, so the build-order keys go
     * back to the allocator rather than sitting on the pool for the life of
     * the tile. */
    feat.clear();
    feat.shrink_to_fit();
}

} /* namespace fif */
