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

#include "debug.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace fif {

void DebugSink::init(int nframes, int w, int h, int channels, int step) {
    step_ = step < 1 ? 1 : step;
    /* Round up: a 3-pixel edge at step 2 still needs two samples. */
    w_ = (w + step_ - 1) / step_;
    h_ = (h + step_ - 1) / step_;
    ch_ = channels;
    frames_.assign((size_t)nframes,
                   std::vector<uint8_t>((size_t)w_ * h_ * channels, 128));
}

float plane_delta(const Plane *a, const Plane *b, int nplanes) {
    float mx = 0;
    for (int c = 0; c < nplanes; ++c) {
        const float *pa = a[c].px.data(), *pb = b[c].px.data();
        const size_t n = a[c].px.size();
        for (size_t k = 0; k < n; ++k)
            mx = std::max(mx, std::fabs(pa[k] - pb[k]));
    }
    return mx;
}

/* Tile planes -> RGB bytes, straight into the frame at the tile origin.
 *
 * planes_to_image works on whole planes and a row range, so it is fed the
 * tile-local planes and a tile-sized image, and the result is copied in.  That
 * keeps one colour conversion in the codebase; a second one written here would
 * be a second answer to "what colour is this pixel". */
void DebugSink::blit(int frame_ix, const Plane *planes, int nplanes, int cs,
                     int tx, int ty) {
    const int tw = planes[0].w, th = planes[0].h;
    if (tw <= 0 || th <= 0) return;

    fif_image tmp{};
    if (fif_image_alloc(&tmp, tw, th, ch_) != FIF_OK) return;
    planes_to_image(planes, nplanes, cs, tw, 0, th, tmp);

    /* tx, ty and the tile are in full-resolution image space; the frame is in
     * preview space.  Walk the destination and point-sample the source, so a
     * tile whose origin is not a multiple of step still lands correctly. */
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<uint8_t> &dst = frames_[frame_ix];
    const int dy0 = std::max(0, (ty + step_ - 1) / step_);
    const int dy1 = std::min(h_, (ty + th + step_ - 1) / step_);
    const int dx0 = std::max(0, (tx + step_ - 1) / step_);
    const int dx1 = std::min(w_, (tx + tw + step_ - 1) / step_);
    for (int dy = dy0; dy < dy1; ++dy) {
        const int sy = dy * step_ - ty;
        if (sy < 0 || sy >= th) continue;
        uint8_t *drow = &dst[((size_t)dy * w_) * ch_];
        const uint8_t *srow = tmp.pixels + (size_t)sy * tmp.stride;
        for (int dx = dx0; dx < dx1; ++dx) {
            const int sx = dx * step_ - tx;
            if (sx < 0 || sx >= tw) continue;
            std::memcpy(drow + (size_t)dx * ch_, srow + (size_t)sx * ch_,
                        (size_t)ch_);
        }
    }
    fif_image_free(&tmp);
}

void DebugSink::on_iteration(int frame_ix, const Plane *planes,
                             int nplanes, int cs, int tx, int ty) {
    if (frame_ix < 0 || frame_ix >= (int)frames_.size()) return;
    blit(frame_ix, planes, nplanes, cs, tx, ty);
}

void DebugSink::on_tile_done(int used, const Plane *planes,
                             int nplanes, int cs, int tx, int ty) {
    /* A tile that settled at iteration 3 is still on screen at frame 9. */
    for (int f = used + 1; f < (int)frames_.size(); ++f)
        blit(f, planes, nplanes, cs, tx, ty);
}

DebugTile &DebugSink::tile_at(int tile_id) {
    if (tile_id >= (int)tiles.size()) tiles.resize((size_t)tile_id + 1);
    return tiles[(size_t)tile_id];
}

void DebugSink::set_tile_geometry(int tile_id, int x, int y, int w, int h) {
    std::lock_guard<std::mutex> lk(mu_);
    DebugTile &t = tile_at(tile_id);
    t.id = tile_id; t.x = x; t.y = y; t.w = w; t.h = h;
}

void DebugSink::set_delta(int tile_id, int iter, float dmax) {
    std::lock_guard<std::mutex> lk(mu_);
    DebugTile &t = tile_at(tile_id);
    if (iter >= (int)t.dmax.size()) t.dmax.resize(iter + 1, 0.f);
    t.dmax[iter] = dmax;
    t.iters = std::max(t.iters, iter);
}

void DebugSink::set_time(int tile_id, float ms) {
    std::lock_guard<std::mutex> lk(mu_);
    tile_at(tile_id).ms = ms;
}

void DebugSink::on_leaves(int tile_id, const Tile &tile, int ox, int oy, int nplanes,
                          const CodecParams &p, const Scale &sc) {
    if (!want_leaves) return;
    const int cs = nplanes > 1 ? chroma_shift(p) : 0;
    const float qstep = 255.0f / (float)(1 << p.dc_bits);
    std::vector<DebugLeaf> out;
    for (int c = 0; c < nplanes; ++c) {
        /* Chroma is reported in image pixels like everything else: positions
         * and sizes on the chroma grid, shifted up by the subsampling. */
        const int sh = c ? cs : 0;
        const bool from_luma = c && chroma_from_luma(p);
        /* The tile holds one pre-order quadtree per max_block region, laid
         * end to end, so the leaves in vector order are the leaves in decode
         * order - the same walk the renderer does. */
        for (const QNode &nd : tile.trees[c]) {
            if (nd.split) continue;
            const Leaf &lf = nd.leaf;
            DebugLeaf d{};
            d.tile   = tile_id;
            d.plane  = c;
            d.x      = ox + (sc.pos(lf.x) << sh);
            d.y      = oy + (sc.pos(lf.y) << sh);
            d.size   = sc.len(lf.size) << sh;
            d.domain = -1;
            d.flags  = (lf.flat ? DebugLeaf::FLAT : 0) | (lf.planar ? DebugLeaf::PLANAR : 0);
            d.dc     = dequant_dc(lf.dc_q, p);
            d.gx     = lf.planar ? qstep * (float)lf.gx_q : 0.f;
            d.gy     = lf.planar ? qstep * (float)lf.gy_q : 0.f;
            /* A leaf coded with a transform whose detail layer did not arrive
             * has no domain and renders as its mean, like a flat one. */
            int dx, dy, dext;
            if (from_luma) {
                if (!lf.flat && lf.domain >= 0) {
                    /* The domain is the luma block under the leaf itself. */
                    d.flags |= DebugLeaf::FROM_LUMA;
                    d.dx = d.x; d.dy = d.y; d.dext = d.size;
                }
            } else if (domain_rect(lf, p, tile.w >> sh, tile.h >> sh, sc, &dx, &dy, &dext)) {
                d.domain = lf.domain;
                d.dx = ox + (dx << sh); d.dy = oy + (dy << sh); d.dext = dext << sh;
            }
            if (d.dext) { d.sym = lf.sym; d.s = dequant_s(lf.s_q, p); }
            out.push_back(d);
        }
    }
    std::lock_guard<std::mutex> lk(mu_);
    tile_at(tile_id).leaves = std::move(out);
}

} /* namespace fif */
