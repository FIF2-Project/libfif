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
#include "syntax.h"
#include <algorithm>
#include <optional>

namespace fif {

namespace {

int level_index(int n) { return std::min(ilog2_ceil((uint32_t)n), Contexts::NL - 1); }

/* Both neighbours are already coded whenever they exist: the quadtree is
 * walked depth-first over roots in raster order, so anything to the left or
 * above the current block has been emitted. */
int split_context(const NeighborMap &nm, int x, int y, int n) {
    int c = 0;
    if (const auto *l = nm.at_px(x - 1, y)) c += (l->size < n);
    if (const auto *a = nm.at_px(x, y - 1)) c += (a->size < n);
    return c;
}
/* Deliberately keyed on the coded mode bit rather than on whether a domain
 * position is present: a reader that has only the base layer must arrive at the
 * same context as one holding the whole file, or it would mis-decode the very
 * bits it does have. */
int mode_context(const NeighborMap &nm, int x, int y) {
    int c = 0;
    if (const auto *l = nm.at_px(x - 1, y)) c += (l->fractal == 0);
    if (const auto *a = nm.at_px(x, y - 1)) c += (a->fractal == 0);
    return c;
}

/* Planar leaves cluster in smooth regions and are absent from busy ones, so the
 * two already-coded neighbours predict the flag well. */
int planar_context(const NeighborMap &nm, int x, int y) {
    int c = 0;
    if (const auto *l = nm.at_px(x - 1, y)) c += (l->planar != 0);
    if (const auto *a = nm.at_px(x, y - 1)) c += (a->planar != 0);
    return c;
}

struct DcContext {
    int pred;
    int act;
};

DcContext dc_context(const NeighborMap &nm, int x, int y, int dc_bits) {
    const auto *l  = nm.at_px(x - 1, y);
    const auto *a  = nm.at_px(x, y - 1);
    const auto *ul = nm.at_px(x - 1, y - 1);
    if (l && a) {
        const int c = ul ? ul->dc : ((l->dc + a->dc + 1) >> 1);
        return {med_predict(l->dc, a->dc, c),
                activity_class(l->dc, a->dc, c, dc_bits)};
    }
    if (l) return {l->dc, 0};
    if (a) return {a->dc, 0};
    return {1 << (dc_bits - 1), 0};
}

/* Adjacent range blocks tend to map to adjacent domains, so the neighbour's
 * domain position shifted by the range displacement is a strong predictor.
 * Positions are kept in pixels, not grid indices, so a neighbour coded at a
 * different block size - and therefore a different domain step - still
 * predicts usefully. */
bool predict_domain(const NeighborMap &nm, int x, int y, int n, int step,
                    int gw, int gh, int *pgx, int *pgy) {
    /* Each coded neighbour with a domain votes for where this block's domain
     * should be, by carrying its own domain along the displacement between the
     * two range blocks.  Taking the component-wise median of three votes is the
     * same construction H.264 uses for motion vectors, and for the same reason:
     * it ignores one outlier for free. */
    int cx[3], cy[3];
    int k = 0;
    const NeighborMap::Cell *cand[3] = {
        nm.at_px(x - 1, y), nm.at_px(x, y - 1), nm.at_px(x + n, y - 1)
    };
    for (const auto *c : cand) {
        if (!c || c->dpx < 0) continue;
        cx[k] = c->dpx + (x - c->ox);
        cy[k] = c->dpy + (y - c->oy);
        ++k;
    }
    if (k == 0) return false;
    int px, py;
    if (k == 3) {
        px = std::max(std::min(cx[0], cx[1]), std::min(std::max(cx[0], cx[1]), cx[2]));
        py = std::max(std::min(cy[0], cy[1]), std::min(std::max(cy[0], cy[1]), cy[2]));
    } else if (k == 2) {
        px = (cx[0] + cx[1]) >> 1;
        py = (cy[0] + cy[1]) >> 1;
    } else {
        px = cx[0]; py = cy[0];
    }
    *pgx = std::clamp(px / step, 0, gw - 1);
    *pgy = std::clamp(py / step, 0, gh - 1);
    return true;
}

struct Geometry {
    int step = 1, gw = 0, gh = 0;
};
Geometry geometry_for(int tw, int th, int n, const CodecParams &p) {
    Geometry g;
    g.step = step_for(p, n);
    pool_grid(tw, th, n, g.step, &g.gw, &g.gh);
    return g;
}

/* ------------------------------------------------------- the leaf syntax */

/* Coded and derived state for one leaf, gathered once so the syntax below
 * reads as a list of symbols rather than a thicket of recomputation. */
struct LeafSite {
    int lvl;
    Geometry g;
    bool can_fractal;
    DcContext dc;
};

LeafSite leaf_site(const NeighborMap &nm, const CodecParams &p, int x, int y,
                   int n, int tw, int th, bool code_geometry,
                   bool inherit_fractal) {
    LeafSite st;
    st.lvl = level_index(n);
    st.g = geometry_for(tw, th, n, p);
    st.can_fractal = code_geometry ? (st.g.gw > 0) : inherit_fractal;
    st.dc = dc_context(nm, x, y, p.dc_bits);
    return st;
}

template <class IO>
void code_leaf(IO &io, Contexts &ctx, const CodecParams &p, double lambda,
               NeighborMap &nm, Leaf &lf, int x, int y, int n,
               int tw, int th, int pc, bool code_geometry,
               bool inherit_fractal, int32_t inherit_domain, int32_t inherit_sym) {
    const LeafSite st = leaf_site(nm, p, x, y, n, tw, th,
                                  code_geometry, inherit_fractal);
    const double lam = (pc == 0) ? lambda : lambda * kChromaLambdaScale;
    SymbolStats *stats = io.stats();

    /* ---- mode ---- */
    int32_t fractal = IO::writing() ? (lf.flat ? 0 : 1) : 0;
    if (st.can_fractal) {
        io.bin(LAYER_BASE, ctx.mode[pc][st.lvl][mode_context(nm, x, y)], fractal,
               &SymbolStats::mode);
    } else {
        fractal = 0;
    }
    /* The coded mode belongs to the leaf whether or not the detail layer
     * arrived: the base layer's own syntax depends on it. */
    const int32_t coded_fractal = fractal;
    if (!io.detail_present()) fractal = 0;

    /* ---- DC ---- */
    if (IO::writing()) {
        const double pixels = (double)n * n;
        double best = 1e300;
        int32_t pick = lf.dc_q;
        for (int d = -1; d <= 1; ++d) {
            const int32_t q = lf.dc_q + d;
            if (q < 0 || q >= (1 << p.dc_bits)) continue;
            const double dv = dequant_dc(q, p) - lf.mean;
            const double j = pixels * dv * dv + lam *
                cost_signed(ctx.dc_zero[pc][st.lvl][st.dc.act],
                            ctx.dc_sign[pc][st.dc.act],
                            ctx.dc_len[pc][st.dc.act], Contexts::MAXLEN,
                            q - st.dc.pred);
            if (j < best) { best = j; pick = q; }
        }
        lf.dc_q = pick;
    }
    int32_t dc_res = lf.dc_q - st.dc.pred;
    io.residual(LAYER_BASE, ctx.dc_zero[pc][st.lvl][st.dc.act],
                ctx.dc_sign[pc][st.dc.act], ctx.dc_len[pc][st.dc.act],
                Contexts::MAXLEN, dc_res, &SymbolStats::dc);
    if (!IO::writing()) {
        lf.dc_q = std::clamp(st.dc.pred + dc_res, 0, (1 << p.dc_bits) - 1);
        lf.x = x; lf.y = y; lf.size = n;
        lf.flat = coded_fractal ? 0 : 1;
        lf.domain = -1; lf.sym = 0; lf.s_q = 0;
    }

    /* ---- gradient, on leaves with no transform ---- *
     *
     * The DC is the block mean either way, so this rides on top of the syntax
     * above rather than replacing it, and a decoder that stops at the base
     * layer still gets the gradient. */
    if (!IO::writing()) { lf.planar = 0; lf.gx_q = 0; lf.gy_q = 0; }
    /* Gated on the mode as coded, not on the mode as usable: this lives in the
     * base layer, so whether the detail layer arrived cannot be allowed to
     * change how many symbols are read here. */
    if (p.planar_mode && !coded_fractal) {
        int32_t planar = IO::writing() ? (lf.planar ? 1 : 0) : 0;
        io.bin(LAYER_BASE, ctx.planar[pc][st.lvl][planar_context(nm, x, y)],
               planar, &SymbolStats::planar);
        /* The flag is in the base layer so the base parse stays self-contained;
         * the two coefficients are detail.  A reader holding only the base
         * therefore knows the leaf is planar and renders it at its mean - the
         * same picture the codec produced before planar existed - instead of
         * paying two thirds of the file for the gradients up front. */
        if (planar && io.detail_present()) {
            int32_t gx = lf.gx_q, gy = lf.gy_q;
            io.residual(LAYER_DETAIL, ctx.g_zero[pc][0], ctx.g_sign[pc][0],
                        ctx.g_len[pc][0], Contexts::MAXLEN, gx,
                        &SymbolStats::grad);
            io.residual(LAYER_DETAIL, ctx.g_zero[pc][1], ctx.g_sign[pc][1],
                        ctx.g_len[pc][1], Contexts::MAXLEN, gy,
                        &SymbolStats::grad);
            if (!IO::writing()) {
                const int32_t lim = 4 << p.dc_bits;
                lf.planar = 1;
                lf.gx_q = std::clamp(gx, -lim, lim);
                lf.gy_q = std::clamp(gy, -lim, lim);
            }
        } else if (planar && !IO::writing()) {
            lf.planar = 1;              /* flag only; no gradient arrived */
        }
    }

    /* ---- transform ---- */
    int32_t dpx = -1, dpy = -1;
    if (fractal) {
        int32_t gx = 0, gy = 0;
        if (code_geometry) {
            if (IO::writing()) { gx = lf.domain % st.g.gw; gy = lf.domain / st.g.gw; }
            int pgx, pgy;
            if (predict_domain(nm, x, y, n, st.g.step, st.g.gw, st.g.gh, &pgx, &pgy)) {
                int32_t dx = gx - pgx, dy = gy - pgy;
                io.residual(LAYER_DETAIL, ctx.dd_zero[pc][0], ctx.dd_sign[pc][0],
                            ctx.dd_len[pc][0], Contexts::MAXLEN, dx,
                            &SymbolStats::domain);
                io.residual(LAYER_DETAIL, ctx.dd_zero[pc][1], ctx.dd_sign[pc][1],
                            ctx.dd_len[pc][1], Contexts::MAXLEN, dy,
                            &SymbolStats::domain);
                if (!IO::writing()) { gx = pgx + dx; gy = pgy + dy; }
            } else {
                io.uniform(LAYER_DETAIL, gx, bits_for((uint32_t)st.g.gw),
                           &SymbolStats::domain);
                io.uniform(LAYER_DETAIL, gy, bits_for((uint32_t)st.g.gh),
                           &SymbolStats::domain);
            }
            int32_t sym = lf.sym;
            io.tree(LAYER_DETAIL, ctx.sym[pc][st.lvl], 3, sym, &SymbolStats::sym);
            if (!IO::writing()) {
                gx = std::clamp<int32_t>(gx, 0, st.g.gw - 1);
                gy = std::clamp<int32_t>(gy, 0, st.g.gh - 1);
                lf.domain = gy * st.g.gw + gx;
                lf.sym = (uint8_t)sym;
            }
        } else {
            if (!IO::writing()) {
                lf.domain = inherit_domain;
                lf.sym = (uint8_t)inherit_sym;
            }
            if (st.g.gw > 0 && lf.domain >= 0) {
                gx = lf.domain % st.g.gw; gy = lf.domain / st.g.gw;
            }
        }

        if (IO::writing() && lf.nd > 1e-6f) {
            double best = 1e300;
            int32_t pick = lf.s_q;
            for (int d = -2; d <= 2; ++d) {
                const int32_t q = lf.s_q + d;
                if (q < 0 || q >= (1 << p.s_bits)) continue;
                const float sd = dequant_s(q, p);
                const double dist = (double)lf.rr - 2.0 * sd * lf.xd
                                  + (double)sd * sd * lf.nd;
                const double j = dist + lam *
                    cost_tree(ctx.contrast[pc][st.lvl], p.s_bits, (uint32_t)q);
                if (j < best) { best = j; pick = q; }
            }
            lf.s_q = pick;
        }
        int32_t s_q = lf.s_q;
        io.tree(LAYER_DETAIL, ctx.contrast[pc][st.lvl], p.s_bits, s_q,
                &SymbolStats::contrast);
        if (!IO::writing()) lf.s_q = s_q;

        dpx = gx * st.g.step;
        dpy = gy * st.g.step;
    }

    if (stats) {
        const double dv = dequant_dc(lf.dc_q, p) - lf.mean;
        double d = (double)n * n * dv * dv;
        if (fractal && lf.nd > 1e-6f) {
            const float sd = dequant_s(lf.s_q, p);
            d += (double)lf.rr - 2.0 * sd * lf.xd + (double)sd * sd * lf.nd;
        } else if (lf.planar) {
            d += lf.planar_rr;   /* residual the fitted gradient actually left */
        } else {
            d += lf.rr;
        }
        stats->distortion += d;
    }
    nm.fill(x, y, n, lf.dc_q, coded_fractal, dpx, dpy, lf.sym, lf.s_q,
            lf.planar);
}

/* ---------------------------------------------------------- the tree walk */

/* One walk for both directions.  The encoder reads an existing tree by index;
 * the decoder appends as it goes and ignores the index.  The returned value is
 * the index just past this subtree, which only the encoder uses. */
template <class IO>
size_t code_node(IO &io, Contexts &ctx, const CodecParams &p, double lambda,
                 std::vector<QNode> &t, size_t i, NeighborMap &nm,
                 int x, int y, int n, int min_b, int tw, int th, int pc) {
    size_t self;
    int32_t split = 0;
    if (IO::writing()) {
        self = i;
        split = t[i].split ? 1 : 0;
    } else {
        self = t.size();
        t.push_back(QNode{});
    }

    if (n > min_b) {
        io.bin(LAYER_BASE, ctx.split[pc][level_index(n)][split_context(nm, x, y, n)],
               split, &SymbolStats::split);
    }
    if (!IO::writing()) t[self].split = (split != 0);

    if (split) {
        const int h = n / 2;
        size_t j = i + 1;
        j = code_node(io, ctx, p, lambda, t, j, nm, x,     y,     h, min_b, tw, th, pc);
        j = code_node(io, ctx, p, lambda, t, j, nm, x + h, y,     h, min_b, tw, th, pc);
        j = code_node(io, ctx, p, lambda, t, j, nm, x,     y + h, h, min_b, tw, th, pc);
        j = code_node(io, ctx, p, lambda, t, j, nm, x + h, y + h, h, min_b, tw, th, pc);
        return j;
    }

    /* The encoder codes a copy, so that requantisation does not persist into
     * the tree and make a second serialisation of the same tile disagree with
     * the first. */
    Leaf tmp = t[self].leaf;
    code_leaf(io, ctx, p, lambda, nm, IO::writing() ? tmp : t[self].leaf,
              x, y, n, tw, th, pc, true, false, -1, 0);
    return IO::writing() ? i + 1 : t.size();
}

/* Shared chroma walks the luma tree for its structure and codes only what
 * differs per plane. */
template <class IO>
size_t code_shared(IO &io, Contexts &ctx, const CodecParams &p, double lambda,
                   const std::vector<QNode> &yt, size_t yi,
                   std::vector<QNode> &ct, size_t ci, size_t *next_ci,
                   NeighborMap &nm, int x, int y, int n, int cs,
                   int ctw, int cth) {
    if (yt[yi].split) {
        if (!IO::writing()) ct.push_back(QNode{true, Leaf{}});
        const int h = n / 2;
        size_t yj = yi + 1, cj = ci + 1;
        for (int k = 0; k < 4; ++k)
            yj = code_shared(io, ctx, p, lambda, yt, yj, ct, cj, &cj, nm,
                             x + (k & 1) * h, y + (k >> 1) * h, h, cs, ctw, cth);
        *next_ci = cj;
        return yj;
    }

    const Leaf &yl = yt[yi].leaf;
    size_t self = ci;
    if (!IO::writing()) { self = ct.size(); ct.push_back(QNode{false, Leaf{}}); }

    /* Inheriting the luma transform only makes sense if there is one.  Chroma
     * predicted from luma has no index to inherit - the domain is the block
     * itself - so every leaf may carry a contrast whatever luma did. */
    const bool from_luma = chroma_from_luma(p);
    const bool can = from_luma ? true : !yl.flat;
    const int32_t dom = from_luma ? 0 : yl.domain;
    const int32_t sym = from_luma ? 0 : yl.sym;

    Leaf tmp = ct[self].leaf;
    code_leaf(io, ctx, p, lambda, nm, IO::writing() ? tmp : ct[self].leaf,
              x >> cs, y >> cs, n >> cs, ctw, cth, 1, false, can, dom, sym);
    *next_ci = ci + 1;
    return yi + 1;
}

/* The plane loop, shared by both directions. */
template <class IO>
void code_planes(IO &io, Contexts &ctx, const CodecParams &p, double lambda,
                 Tile &tile, int nplanes) {
    NeighborMap nm;
    nm.init(tile.w, tile.h, p.min_block);
    auto &yt = tile.trees[0];
    if (!IO::writing()) yt.clear();

    size_t i = 0;
    for (int by = 0; by < tile.h; by += p.max_block)
        for (int bx = 0; bx < tile.w; bx += p.max_block)
            i = code_node(io, ctx, p, lambda, yt, i, nm, bx, by,
                          p.max_block, p.min_block, tile.w, tile.h, 0);

    if (nplanes == 1) return;
    const int cs = chroma_shift(p);
    const int ctw = tile.w >> cs, cth = tile.h >> cs;
    int cmin, cmax;
    chroma_block_range(p, &cmin, &cmax);

    for (int c = 1; c < nplanes; ++c) {
        auto &ct = tile.trees[c];
        if (!IO::writing()) ct.clear();
        NeighborMap cnm;
        cnm.init(ctw, cth, cmin);
        size_t yi = 0, ci = 0;
        for (int by = 0; by < tile.h; by += p.max_block)
            for (int bx = 0; bx < tile.w; bx += p.max_block) {
                size_t nci = ci;
                yi = code_shared(io, ctx, p, lambda, yt, yi, ct, ci, &nci,
                                 cnm, bx, by, p.max_block, cs, ctw, cth);
                ci = nci;
            }
    }
}

} /* namespace */

void serialize_tile(Tile &tile, int nplanes, const CodecParams &p,
                    std::vector<uint8_t> &base, std::vector<uint8_t> &detail,
                    SymbolStats *stats) {
    RangeEncoder e, e1;
    Contexts ctx;
    ctx.reset();
    EncodeIO io(e, e1, stats);
    code_planes(io, ctx, p, lambda_for_quality(p.quality), tile, nplanes);
    e.flush();
    e1.flush();
    if (contexts_train_active()) contexts_train_add(ctx);
    base = std::move(e.bytes());
    detail = std::move(e1.bytes());
}

bool deserialize_tile(const uint8_t *base, size_t base_size,
                      const uint8_t *detail, size_t detail_size,
                      int nplanes, const CodecParams &p, Tile &tile) {
    RangeDecoder d(base, base_size);
    /* Five bytes is what a flush writes, so anything shorter was never a
     * layer; the format says to read such a tile from its base layer alone
     * rather than start a decoder on bytes that carry no symbols. */
    const bool have_detail = detail && detail_size >= 5;
    /* Only constructed when there is something to read: a decoder reads five
     * bytes at construction, and pointing it at the base layer to satisfy a
     * reference it then never uses is how a reader ends up depending on bytes
     * nobody meant it to touch. */
    std::optional<RangeDecoder> d1;
    if (have_detail) d1.emplace(detail, detail_size);
    Contexts ctx;
    ctx.reset();
    DecodeIO io(d, have_detail ? &*d1 : nullptr);
    code_planes(io, ctx, p, lambda_for_quality(p.quality), tile, nplanes);
    return !d.overrun();
}

} /* namespace fif */
