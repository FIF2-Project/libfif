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

#include "codec.h"
#include "kernels.h"
#include <algorithm>
#include <cmath>

namespace fif {

void derive_params(CodecParams &p, int quality, int effort) {
    quality = std::clamp(quality, 0, 100);
    effort  = std::clamp(effort, 0, 9);
    p.quality = quality;
    p.effort  = effort;
    p.s_bits  = 4 + (quality >= 30) + (quality >= 65) + (quality >= 90);
    p.dc_bits = 6 + (quality >= 25) + (quality >= 60);
    /* Grid density, window width in block widths, and feature-space neighbours.
     *
     * Density turns out to matter far more than reach.  A grid at half the
     * block size finds a good match within a block or two, so the window can
     * shrink as the grid tightens - measured, that is smaller, better *and*
     * three times faster than the coarse grid with a wide window it replaced.
     * The index does not get more expensive either: a closer match means a
     * smaller displacement from the predictor. */
    static constexpr int shf[10] = {1, 1, 2, 2, 2,  2,  2,  2,  2,  3};
    static constexpr int rad[10] = {1, 2, 2, 3, 2,  2,  3,  4,  6,  4};
    static constexpr int kdc[10] = {0, 0, 0, 0, 8, 16, 16, 24, 48, 64};
    p.domain_shift  = shf[effort];
    p.local_radius  = rad[effort];
    p.kd_candidates = kdc[effort];
    p.exhaustive    = (effort >= 9);
}

namespace {

/* Which pool serves a block of n samples: level 0 is max_b, each step down
 * halves. */
inline int level_of(int n, int max_b) {
    int l = 0;
    while (max_b > n) { max_b >>= 1; ++l; }
    return l;
}

/* Cost in bits of coding a domain-grid displacement, mirroring what the signed
 * residual coder charges: a zero flag, a sign, a unary length prefix and a
 * uniform mantissa. */
inline double delta_bits(int v) {
    if (v == 0) return 0.7;
    const unsigned m = (unsigned)(v < 0 ? -v : v);
    return 1.4 + 2.0 * (double)bit_length(m);
}

/* Least squares ramp through a centred block.  The two axes are orthogonal to
 * each other and to the constant, so each coefficient is an independent
 * projection and the energy they remove simply adds - which also makes the
 * residual after quantisation exact rather than estimated.
 *
 * Every coder needs this, chroma included: planar makes the luma quadtree
 * coarser, and a shared-partition chroma plane that could not answer with a
 * ramp of its own would be left approximating the same smooth region with
 * bigger steps than before. */
void fit_plane_block(const float *centred, int n, double rr,
                     const CodecParams &p, int32_t *gx_q, int32_t *gy_q,
                     double *err) {
    *gx_q = *gy_q = 0;
    *err = rr;
    if (!p.planar_mode || n < 2) return;

    const double axis = planar_axis_energy(n);
    const double denom = (double)n * axis;
    if (denom < 1e-12) return;

    double px = 0, py = 0;
    for (int y = 0; y < n; ++y) {
        const double uy = ((double)y + 0.5) / n - 0.5;
        const float *r = centred + (size_t)y * n;
        for (int x = 0; x < n; ++x) {
            const double ux = ((double)x + 0.5) / n - 0.5;
            px += r[x] * ux;
            py += r[x] * uy;
        }
    }

    const double step = 255.0 / (double)(1 << p.dc_bits);
    const int32_t lim = 4 << p.dc_bits;
    *gx_q = (int32_t)std::clamp(std::lround(px / denom / step), (long)-lim, (long)lim);
    *gy_q = (int32_t)std::clamp(std::lround(py / denom / step), (long)-lim, (long)lim);

    const double qx = *gx_q * step, qy = *gy_q * step;
    *err = rr - 2.0 * qx * px - 2.0 * qy * py + (qx * qx + qy * qy) * denom;
    if (*err < 0) *err = 0;
}

/* The result of searching one block, independent of how it is later coded.
 * Cached, because the split decision visits every block size of a region. */
struct BlockFit {
    uint8_t valid = 0;
    uint8_t sym = 0;
    int32_t dc_q = 0;
    int32_t dom = -1;
    int32_t s_q = 0;
    double  rr = 0;      /* energy of the centred block          */
    double  ddc = 0;     /* distortion from quantising the mean  */
    double  err = 0;     /* residual after the quantised fit     */
    float   mean = 0, xd = 0, nd = 0;
    /* Best linear ramp through the centred block, quantised, and what it leaves
     * behind.  Costs one pass over the block and is wanted on every leaf. */
    int32_t gx_q = 0, gy_q = 0;
    double  planar_err = 0;
};
using FitCache = std::vector<std::vector<BlockFit>>;

struct PlaneCoder {
    const Plane *pl = nullptr;
    int tx = 0, ty = 0, tw = 0, th = 0;
    const CodecParams *p = nullptr;
    int min_b = 4, max_b = 32;
    std::vector<BlockPool> *pools = nullptr;   /* by level, 0 == max_b */
    FitCache *cache = nullptr;
    const RateModel *rm = nullptr;
    double lambda = 1.0;
    EncodeCounters *ctr = nullptr;

    /* One contiguous buffer for the eight oriented copies of the range block:
     * the candidate loop streams all eight against the same domain, so they
     * want to sit together in cache. */
    std::vector<float> rbuf, rtbuf;
    std::vector<int32_t> cands;
    float fpos[FEAT_DIM], fneg[FEAT_DIM];

    /* Fills rt[0..7] with the centred range block under each dihedral
     * transform.  dot(rt[k], d) equals the inner product of the range with the
     * domain transformed by sym_inverse[k], so one pass over the eight
     * orientations covers the whole group. */
    void prepare_range(const float *centred, int n) {
        const size_t bn = (size_t)n * n;
        rtbuf.resize(bn * 8);
        std::copy_n(centred, bn, rtbuf.data());
        for (int k = 1; k < 8; ++k) {
            const SymIndex ix(k, n);
            float *dst = rtbuf.data() + bn * k;
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x)
                    dst[(size_t)y * n + x] = centred[ix(x, y)];
        }
    }
    const float *oriented(int k, size_t bn) const {
        return rtbuf.data() + bn * (size_t)k;
    }

    /* `r` is given in block widths, not grid steps, so that changing the grid
     * density changes how finely the window is sampled and not how far it
     * reaches.  Measured the other way round, a denser grid quietly halved the
     * searched area and cost a full dB. */
    void add_window(const BlockPool &pool, int x, int y, int r) {
        r = std::max(1, r * (1 << p->domain_shift) / 2);
        const int cgx = std::clamp(x / pool.step, 0, pool.grid_w - 1);
        const int cgy = std::clamp(y / pool.step, 0, pool.grid_h - 1);
        for (int gy = std::max(0, cgy - r); gy <= std::min(pool.grid_h - 1, cgy + r); ++gy)
            for (int gx = std::max(0, cgx - r); gx <= std::min(pool.grid_w - 1, cgx + r); ++gx)
                cands.push_back(gy * pool.grid_w + gx);
    }

    void gather(const BlockPool &pool, const float *centred, int n, int x, int y) {
        cands.clear();
        if (pool.count == 0) return;

        if (p->exhaustive && pool.count <= 16384) {
            cands.resize(pool.count);
            for (int i = 0; i < pool.count; ++i) cands[i] = i;
            return;
        }

        /* Two complementary sources.  The local window covers the short-range
         * self-similarity that dominates natural images; the feature-space
         * neighbours reach anywhere in the tile.  Blocks below 4 pixels get the
         * window only - the 4x4 feature reduction has nothing to reduce. */
        if (p->local_radius > 0) add_window(pool, x, y, p->local_radius);

        if (p->kd_candidates > 0 && n >= 4 && pool.tree.count > 0) {
            make_feature(centred, n, fpos, fneg);
            pool.tree.knn(fpos, p->kd_candidates, cands);
            pool.tree.knn(fneg, p->kd_candidates, cands);
        }
        std::sort(cands.begin(), cands.end());
        cands.erase(std::unique(cands.begin(), cands.end()), cands.end());
    }

    /* Searches the block at (x,y,n) and records the fit; no coding decision. */
    void compute_fit(int x, int y, int n, BlockFit &f) {
        const size_t bn = (size_t)n * n;
        rbuf.resize(bn);
        copy_block(*pl, tx + x, ty + y, n, rbuf.data());
        const float m = centre(rbuf.data(), bn);
        const float rr = dot(rbuf.data(), rbuf.data(), bn);

        f.mean = m;
        f.dc_q = quant_dc(m, *p);
        const float dcd = dequant_dc(f.dc_q, *p);
        f.ddc = (double)bn * (double)(m - dcd) * (double)(m - dcd);
        f.rr = rr;
        f.dom = -1; f.sym = 0; f.s_q = 0; f.err = rr;
        fit_plane_block(rbuf.data(), n, f.rr, *p, &f.gx_q, &f.gy_q, &f.planar_err);

        if (rr < 1e-3f) return;
        const int lvl = level_of(n, max_b);
        if (lvl >= (int)pools->size()) return;
        const BlockPool &pool = (*pools)[lvl];
        if (pool.count == 0) return;

        gather(pool, rbuf.data(), n, x, y);
        if (cands.empty()) return;
        prepare_range(rbuf.data(), n);
        ctr->candidate_evals += (int64_t)cands.size() * 8;

        /* Pick the candidate that minimises distortion *plus* what it will cost
         * to point at, not the one with the smallest error.  The domain index
         * is coded as a displacement from the neighbouring block's domain, so a
         * match on the far side of the tile can cost a dozen bits more than an
         * equally good one next door.  Without this term the search happily
         * buys a fraction of a dB for bits it never accounts for. */
        const int gx0 = std::clamp(x / pool.step, 0, pool.grid_w - 1);
        const int gy0 = std::clamp(y / pool.step, 0, pool.grid_h - 1);

        double best_cost = 1e300;
        float best_dot = 0;
        int best_dom = -1, best_k = 0;
        for (int32_t j : cands) {
            const float nd = pool.norm2[j];
            if (nd < 1e-6f) continue;
            const double rate = lambda *
                (delta_bits(j % pool.grid_w - gx0) +
                 delta_bits(j / pool.grid_w - gy0));
            if (rate >= best_cost) continue;      /* cannot win on rate alone */

            /* The cost falls strictly as the inner product grows, and the
             * denominator is the same for all eight orientations, so the
             * winning one is simply the largest |dt|.  Finding it that way
             * turns eight divisions into one - and a double divide is not
             * pipelined, so those seven cost more than the dot products
             * they sat between. */
            const float *d = pool.block(j);
            float best_dt = dot(oriented(0, bn), d, bn);
            double best_dt2 = (double)best_dt * best_dt;
            int k_best = 0;
            for (int k = 1; k < 8; ++k) {
                const float dt = dot(oriented(k, bn), d, bn);
                const double dt2 = (double)dt * dt;
                if (dt2 > best_dt2) { best_dt2 = dt2; best_dt = dt; k_best = k; }
            }
            const double cost = (double)rr - best_dt2 / nd + rate;
            if (cost < best_cost) {
                best_cost = cost; best_dot = best_dt; best_dom = j; best_k = k_best;
            }
        }
        if (best_dom < 0) return;

        const float nd = pool.norm2[best_dom];
        int sq = quant_s(best_dot / nd, *p);
        float sd = dequant_s(sq, *p);
        double E = (double)rr - 2.0 * sd * best_dot + (double)sd * sd * nd;

        if (p->effort >= 4) {              /* the neighbouring contrast levels */
            for (int d = -1; d <= 1; d += 2) {
                const int q2 = sq + d;
                if (q2 < 0 || q2 >= (1 << p->s_bits)) continue;
                const float s2 = dequant_s(q2, *p);
                const double E2 = (double)rr - 2.0 * s2 * best_dot
                                + (double)s2 * s2 * nd;
                if (E2 < E) { E = E2; sq = q2; sd = s2; }
            }
        }
        f.dom = best_dom;
        f.sym = (uint8_t)sym_inverse[best_k];
        f.s_q = sq;
        f.err = E < 0 ? 0 : E;
        f.xd = best_dot;
        f.nd = nd;
    }

    const BlockFit &fit(int x, int y, int n) {
        const int lvl = level_of(n, max_b);
        auto &v = (*cache)[lvl];
        const int gw = tw / n;
        if (v.empty()) v.assign((size_t)gw * (th / n), BlockFit{});
        BlockFit &f = v[(size_t)(y / n) * gw + (x / n)];
        if (!f.valid) { compute_fit(x, y, n, f); f.valid = 1; }
        return f;
    }

    /* Turns a fit into a coded leaf under the current cost model. */
    void best_leaf(int x, int y, int n, Leaf &out, double &D, double &R) {
        const BlockFit &f = fit(x, y, n);
        out.x = x; out.y = y; out.size = n; out.dc_q = f.dc_q;
        out.domain = -1; out.sym = 0; out.s_q = 0; out.flat = 1;
        out.mean = f.mean; out.rr = (float)f.rr; out.xd = f.xd; out.nd = f.nd;
        out.planar_rr = (float)f.rr;
        out.planar = 0; out.gx_q = 0; out.gy_q = 0;

        const int lvl = level_of(n, max_b);
        const bool has_pool = lvl < (int)pools->size() && (*pools)[lvl].count > 0;
        const double mode_bits = has_pool ? rm->mode : 0.0;
        const double planar_bits = p->planar_mode ? rm->planar : 0.0;

        D = f.rr + f.ddc;
        R = mode_bits + rm->dc + planar_bits;

        /* A ramp instead of a single mean.  Cheaper than the four children it
         * saves whenever the block really is part of one. */
        if (p->planar_mode && (f.gx_q || f.gy_q)) {
            const double D_pl = f.planar_err + f.ddc;
            const double R_pl = mode_bits + rm->dc + planar_bits + 2.0 * rm->grad;
            if (D_pl + lambda * R_pl < D + lambda * R) {
                out.planar = 1; out.gx_q = f.gx_q; out.gy_q = f.gy_q;
                out.planar_rr = (float)f.planar_err;
                D = D_pl; R = R_pl;
            }
        }
        if (f.dom < 0) return;

        const double D_frac = f.err + f.ddc;
        const double R_frac = mode_bits + rm->dc
                            + rm->domain((*pools)[lvl].idx_bits)
                            + rm->sym + rm->contrast;
        if (D_frac + lambda * R_frac < D + lambda * R) {
            out.flat = 0;
            out.planar = 0; out.gx_q = 0; out.gy_q = 0;
            out.domain = f.dom;
            out.sym = f.sym;
            out.s_q = f.s_q;
            D = D_frac; R = R_frac;
        }
    }

    double encode_node(int x, int y, int n, std::vector<QNode> &outv) {
        const double flag = (n > min_b) ? rm->split : 0.0;

        Leaf leaf;
        double D, R;
        best_leaf(x, y, n, leaf, D, R);
        const double J_leaf = D + lambda * (R + flag);

        if (flag) {
            /* A split subtree cannot be cheaper than four DC-only children. */
            const double min_split_rate =
                rm->split + 4.0 * ((n / 2 > min_b ? rm->split : 0.0) + rm->dc);
            if (J_leaf > lambda * min_split_rate) {
                const size_t mark = outv.size();
                outv.push_back(QNode{true, Leaf{}});
                const int h = n / 2;
                double J = lambda * rm->split;
                J += encode_node(x,     y,     h, outv);
                J += encode_node(x + h, y,     h, outv);
                J += encode_node(x,     y + h, h, outv);
                J += encode_node(x + h, y + h, h, outv);
                if (J < J_leaf) return J;
                outv.resize(mark);
            }
        }
        outv.push_back(QNode{false, leaf});
        ++ctr->blocks_total;
        if (leaf.flat) ++ctr->blocks_flat; else ++ctr->blocks_fractal;
        ctr->coded_pixels += (int64_t)n * n;
        return J_leaf;
    }
};

/* What both chroma coders do first: read the block, centre it, quantise the
 * mean and try a ramp.  Fills `cl` as the best leaf with no transform, leaves
 * the centred samples in `rbuf`, and returns that leaf's cost; `mode_bits` is
 * included in the rate as given. */
double chroma_flat_leaf(const Plane &pl, int x0, int y0, int cn,
                        const CodecParams &p, const RateModel &rm,
                        double lambda, double mode_bits,
                        std::vector<float> &rbuf, Leaf &cl, double *ddc_out) {
    const size_t bn = (size_t)cn * cn;
    rbuf.resize(bn);
    copy_block(pl, x0, y0, cn, rbuf.data());
    const float m = centre(rbuf.data(), bn);
    const float rr = dot(rbuf.data(), rbuf.data(), bn);

    cl.size = cn;
    cl.mean = m; cl.rr = rr; cl.planar_rr = rr;
    cl.dc_q = quant_dc(m, p);
    cl.sym = 0; cl.s_q = 0; cl.flat = 1;

    const float dcd = dequant_dc(cl.dc_q, p);
    const double ddc = (double)bn * (double)(m - dcd) * (double)(m - dcd);
    const double planar_bits = p.planar_mode ? rm.planar : 0.0;
    double J_flat = (double)rr + ddc + lambda * (mode_bits + rm.dc + planar_bits);

    int32_t gx = 0, gy = 0;
    double perr = rr;
    fit_plane_block(rbuf.data(), cn, rr, p, &gx, &gy, &perr);
    if (gx || gy) {
        const double J_pl = perr + ddc + lambda *
            (mode_bits + rm.dc + planar_bits + 2.0 * rm.grad);
        if (J_pl < J_flat) {
            J_flat = J_pl;
            cl.planar = 1; cl.gx_q = gx; cl.gy_q = gy;
            cl.planar_rr = (float)perr;
        }
    }
    *ddc_out = ddc;
    return J_flat;
}

/* Chroma inherits the luma partition.  In SHARED mode it also inherits the
 * domain and symmetry, and only DC and contrast are fitted to the chroma
 * samples.  In FROM_LUMA mode the domain is the co-located luma block - under
 * 4:2:0 twice the size in each direction, so the ordinary 2:1 decimation
 * applies - and nothing is searched.  Either way a per-leaf mode bit allows
 * falling back to DC only. */
struct ChromaCoder {
    const Plane *pl = nullptr;      /* the chroma plane being coded  */
    const Plane *luma = nullptr;    /* FROM_LUMA only                */
    int tx = 0, ty = 0;             /* tile origin, chroma coords    */
    const CodecParams *p = nullptr;
    int cs = 0;
    bool from_luma = false;
    std::vector<BlockPool> *pools = nullptr;   /* SHARED only */
    int max_b = 0;                             /* luma size */
    const RateModel *rm = nullptr;
    double lambda = 1.0;
    EncodeCounters *ctr = nullptr;
    std::vector<float> rbuf, tbuf;

    size_t walk(const std::vector<QNode> &ytree, size_t i,
                int x, int y, int n, std::vector<QNode> &outv) {
        if (ytree[i].split) {
            outv.push_back(QNode{true, Leaf{}});
            const int h = n / 2;
            size_t j = i + 1;
            for (int k = 0; k < 4; ++k)
                j = walk(ytree, j, x + (k & 1) * h, y + (k >> 1) * h, h, outv);
            return j;
        }
        const Leaf &yl = ytree[i].leaf;
        const int cx = x >> cs, cy = y >> cs, cn = n >> cs;
        const size_t bn = (size_t)cn * cn;

        Leaf cl{};
        cl.x = cx; cl.y = cy;
        cl.domain = from_luma ? 0 : -1;
        double ddc;
        /* SHARED codes the mode bit only under a fractal luma leaf, so it is
         * left out here and added to the comparison below. */
        const double J_flat = chroma_flat_leaf(*pl, tx + cx, ty + cy, cn, *p, *rm,
                                               lambda, from_luma ? rm->mode : 0.0,
                                               rbuf, cl, &ddc);
        const float rr = cl.rr;

        float dt = 0, nd = 0;
        bool have = false;
        if (from_luma && rr > 1e-3f) {
            tbuf.resize(bn);
            luma_block(*luma, tx + cx, ty + cy, cn, cs, tbuf.data());
            centre(tbuf.data(), bn);
            nd = dot(tbuf.data(), tbuf.data(), bn);
            if (nd > 1e-6f) { dt = dot(rbuf.data(), tbuf.data(), bn); have = true; }
        } else if (!from_luma && !yl.flat && rr > 1e-3f) {
            const BlockPool &pool = (*pools)[level_of(n, max_b)];
            if (pool.count > yl.domain && pool.norm2[yl.domain] > 1e-6f) {
                const SymIndex ix(sym_inverse[yl.sym], cn);
                tbuf.resize(bn);
                for (int yy = 0; yy < cn; ++yy)
                    for (int xx = 0; xx < cn; ++xx)
                        tbuf[(size_t)yy * cn + xx] = rbuf[ix(xx, yy)];
                nd = pool.norm2[yl.domain];
                dt = dot(tbuf.data(), pool.block(yl.domain), bn);
                have = true;
            }
        }
        if (have) {
            const int sq = quant_s(dt / nd, *p);
            const float sd = dequant_s(sq, *p);
            const double E = (double)rr - 2.0 * sd * dt + (double)sd * sd * nd;
            cl.xd = dt; cl.nd = nd;
            ctr->candidate_evals += 1;
            const double J_frac = E + ddc + lambda * (rm->mode + rm->dc + rm->contrast);
            if (J_frac < (from_luma ? J_flat : J_flat + lambda * rm->mode)) {
                cl.flat = 0; cl.planar = 0; cl.gx_q = 0; cl.gy_q = 0;
                cl.s_q = sq;
                if (!from_luma) { cl.domain = yl.domain; cl.sym = yl.sym; }
            }
        }
        outv.push_back(QNode{false, cl});
        ++ctr->blocks_total;
        if (cl.flat) ++ctr->blocks_flat; else ++ctr->blocks_fractal;
        return i + 1;
    }
};

void build_pools(const Plane &pl, int tx, int ty, int tw, int th,
                 int max_b, int min_b, const CodecParams &p, bool with_tree,
                 std::vector<BlockPool> &pools) {
    int levels = 0;
    for (int s = max_b; s >= min_b; s >>= 1) ++levels;
    pools.assign(levels, BlockPool{});
    int l = 0;
    for (int s = max_b; s >= min_b; s >>= 1, ++l)
        pools[l].build(pl, tx, ty, tw, th, s, step_for(p, s), with_tree && s >= 4);
}

} /* namespace */

void encode_tile(const Plane *planes, int nplanes, Tile &tile,
                 const CodecParams &p, EncodeCounters &ctr,
                 std::vector<uint8_t> &base_out, std::vector<uint8_t> &detail_out,
                 SymbolStats &sym_out, int force_strategy, int *won) {
    const double lambda = lambda_for_quality(p.quality);
    const int cs = chroma_shift(p);
    const int ctw = tile.w >> cs, cth = tile.h >> cs;
    int cmin, cmax;
    chroma_block_range(p, &cmin, &cmax);

    const bool from_luma = chroma_from_luma(p);
    std::vector<BlockPool> ypools;
    build_pools(planes[0], tile.x, tile.y, tile.w, tile.h,
                p.max_block, p.min_block, p, p.kd_candidates > 0, ypools);
    std::vector<BlockPool> cpools[3];
    /* Predicting from luma needs no chroma domain pool at all - that is most of
     * what the mode saves on the encoding side. */
    if (!from_luma)
        for (int c = 1; c < nplanes; ++c)
            build_pools(planes[c], tile.x >> cs, tile.y >> cs, ctw, cth,
                        cmax, cmin, p, false, cpools[c]);

    /* Which candidate source wins is a property of the picture, not of the
     * settings: feature-space neighbours are worth several dB on content with
     * long-range self-similarity, and cost rate for nothing on smooth
     * photographic gradients, where every useful match is next door.  Rather
     * than pick one, encode the tile both ways and keep whichever actually
     * came out cheaper - the local-only variant is a small fraction of the
     * cost of the one with the tree, so the comparison is nearly free.
     * Strategy 0 is the local window on its own, whatever the effort preset
     * asked for; strategy 1 adds the feature-space neighbours. */
    const bool wide = p.kd_candidates > 0 || p.exhaustive;
    int n_strategies = wide ? 2 : 1;
    int first = 0;
    if (wide && force_strategy >= 0) { n_strategies = 1; first = force_strategy; }

    double best_cost = 1e300;
    std::vector<uint8_t> best_payload, best_detail;
    Tile best_tile;
    EncodeCounters best_local;
    SymbolStats best_stats;

    for (int si = 0; si < n_strategies; ++si) {
        const bool tree = first + si == 1;
        CodecParams sp = p;
        sp.kd_candidates = tree ? p.kd_candidates : 0;
        sp.exhaustive    = tree && p.exhaustive;

        FitCache cache;
        cache.assign(ypools.size(), {});
        const RateModel rm = RateModel::initial(sp);
        EncodeCounters local;
        SymbolStats stats;
        std::vector<uint8_t> buf, buf1;
        for (int c = 0; c < nplanes; ++c) tile.trees[c].clear();

        PlaneCoder yc;
        yc.pl = &planes[0];
        yc.tx = tile.x; yc.ty = tile.y; yc.tw = tile.w; yc.th = tile.h;
        yc.p = &sp; yc.min_b = sp.min_block; yc.max_b = sp.max_block;
        yc.pools = &ypools; yc.cache = &cache; yc.rm = &rm;
        yc.lambda = lambda; yc.ctr = &local;

        auto &ytree = tile.trees[0];
        for (int by = 0; by < tile.h; by += sp.max_block)
            for (int bx = 0; bx < tile.w; bx += sp.max_block)
                yc.encode_node(bx, by, sp.max_block, ytree);

        for (int c = 1; c < nplanes; ++c) {
            ChromaCoder cc;
            cc.pl = &planes[c]; cc.luma = &planes[0];
            cc.tx = tile.x >> cs; cc.ty = tile.y >> cs;
            cc.p = &sp; cc.cs = cs; cc.from_luma = from_luma;
            cc.pools = &cpools[c]; cc.max_b = sp.max_block;
            cc.rm = &rm;
            cc.lambda = lambda * kChromaLambdaScale;
            cc.ctr = &local;
            size_t i = 0;
            for (int by = 0; by < tile.h; by += sp.max_block)
                for (int bx = 0; bx < tile.w; bx += sp.max_block)
                    i = cc.walk(ytree, i, bx, by, sp.max_block, tile.trees[c]);
        }
        /* This is the payload, and what the strategy comparison below is
         * judged on. */
        serialize_tile(tile, nplanes, sp, buf, buf1, &stats);

        /* Judged on the payload that was actually produced and the error that
         * actually remains, so the comparison needs no rate model and no
         * assumption about what the quantisation search did. */
        const double cost = stats.distortion
            + lambda * 8.0 * (double)(buf.size() + buf1.size());
        if (cost < best_cost) {
            best_cost = cost;
            if (won) *won = first + si;
            best_payload = std::move(buf);
            best_detail = std::move(buf1);
            best_tile = tile;
            best_local = local;
            best_stats = stats;
        }
    }

    tile = std::move(best_tile);
    base_out = std::move(best_payload);
    detail_out = std::move(best_detail);
    best_local.collage_sse = best_stats.distortion;
    ctr += best_local;
    sym_out += best_stats;
}

} /* namespace fif */
