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

#include "codec.h"
#include "format.h"
#include "debug.h"
#include "progress.h"
#include "threadpool.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>

namespace fif {
namespace {

/* Largest integer upscale the decoder renders natively. */
constexpr int kMaxScale = 16;

/* Filtered iterations run once the unfiltered attractor has converged.  A
 * seam left in the reconstruction is read back as domain data on the next
 * pass and reproduced, so filtering inside the loop is what removes it;
 * measured, the benefit is complete after two. */
constexpr int kDeblockInLoop = 3;

/* Soft DC surfaces: the largest step between two smooth blocks, in DC
 * quantiser steps, that still counts as quantisation and is bent away.
 * Measured on five photographs at q 5..70: 3 lowers blockiness by 1-3 % for
 * at most 0.012 dB; larger values smooth more and cost more.  Rebuilding
 * flat blocks from their DC instead of correcting them raised PSNR by
 * 0.015 dB but undid the in-loop seam filter and left them 5 % blockier. */
constexpr float kSmoothTol = 3.0f;

/* Edge-aware chroma: the luma variance (in grey levels squared) below which
 * a window counts as flat, and the cap on the chroma-over-luma slope.
 * Measured on five photographs at q 20..85: +0.01 to +0.02 dB over bilinear;
 * an unbounded slope or a variance floor under 25 gave back half of it, and
 * fitting chroma itself rather than adding luma detail to bilinear chroma
 * lost 0.1-0.5 dB. */
constexpr float kGuideEps = 100.0f;
constexpr float kGuideSlope = 0.25f;

/* Has the iteration reached its fixed point?
 *
 * Only the verdict matters, never the value, so the scan stops at the first
 * sample that has moved too far - which on every pass but the last is a few
 * hundred samples rather than the whole picture.  The chunk is there so the
 * inner loop stays a plain float max-reduction the compiler can vectorise;
 * a per-sample branch would not be. */
bool converged(const Plane *a, const Plane *b, int nplanes, double eps) {
    constexpr size_t kChunk = 1024;
    for (int c = 0; c < nplanes; ++c) {
        const float *pa = a[c].px.data(), *pb = b[c].px.data();
        const size_t n = a[c].px.size();
        for (size_t k = 0; k < n; k += kChunk) {
            const size_t m = std::min(kChunk, n - k);
            float mx = 0;
            for (size_t j = 0; j < m; ++j)
                mx = std::max(mx, std::fabs(pa[k + j] - pb[k + j]));
            if ((double)mx >= eps) return false;
        }
    }
    return true;
}

/* The relaxed step: b <- a + w*(b - a), with b the map's output.
 *
 * Standard acceleration for a fixed-point iteration, and it leaves the
 * attractor exactly where it was: wherever b == a already, no w moves
 * anything, so the fixed point of the relaxed iteration is the fixed point of
 * the plain one.  Only the path there gets shorter (w > 1, extrapolating past
 * the step) or longer and steadier (w < 1).
 *
 * Flat over px so this stays a plain fused multiply-add the compiler can
 * vectorise, the same shape as converged(). */
void relax(const Plane *a, Plane *b, int nplanes, float w) {
    for (int c = 0; c < nplanes; ++c) {
        const float *pa = a[c].px.data();
        float *pb = b[c].px.data();
        const size_t n = b[c].px.size();
        for (size_t k = 0; k < n; ++k)
            pb[k] = pa[k] + w * (pb[k] - pa[k]);
    }
}

} /* namespace */
} /* namespace fif */

using namespace fif;

extern "C" {

fif_status fif_get_info(const uint8_t *data, size_t size, fif_info *info) {
    if (!data || !info) return FIF_ERR_ARG;
    /* The header alone: a FIC section that is encrypted or will not unpack
     * still leaves everything here to report. */
    Container c;
    if (!parse_container(data, size, c)) return FIF_ERR_FORMAT;
    std::memset(info, 0, sizeof(*info));
    info->width = c.width;
    info->height = c.height;
    info->channels = c.channels;
    info->tile_size = c.p.tile_size;
    info->min_block = c.p.min_block;
    info->max_block = c.p.max_block;
    info->subsampling = c.p.subsampling;
    info->chroma_mode = c.p.chroma_mode;
    /* Quality and effort only shaped the encode; they travel as ENCO chunks
     * and are -1 when the file does not carry them. */
    info->quality = c.quality;
    info->effort = c.effort;
    info->version_major = c.major;
    info->version_minor = c.minor;
    info->file_size = (int64_t)c.file_size;
    info->truncated = c.truncated;
    /* source_name points into the chunks; an unpacked copy dies with `c`. */
    if (!c.packed(SEC_CHUNKS)) {
        info->source_name = c.source_name;
        info->source_name_len = c.source_name_len;
    }
    for (int k = 0; k < 4 && c.remarks[k]; ++k) info->remarks[k] = c.remarks[k];
    info->compressed = c.compressed;
    info->encrypted = c.encrypted;
    info->compress_method = c.compress_method;
    info->encrypt_method = c.encrypt_method;
    info->chunks_compressed = c.packed(SEC_CHUNKS);
    info->raw_compressed = c.packed(SEC_RAW);
    info->detail_offset = (int64_t)c.detail_offset;
    info->source_width = (int)c.source_width;
    info->source_height = (int)c.source_height;
    info->source_scale = c.source_scale;
    /* Every section is opened so that its CRC, which is over the unpacked
     * bytes, can be checked; one that cannot be opened reports -1. */
    for (int s = SEC_THUMBS; s < SEC_COUNT; ++s) open_section(c, s);
    info->thumbnail_count = std::max(0, thumb_count(c.thumb));
    info->chunk_count = (int)c.chunk_count;
    {
        /* The count is the first field of the raw section. */
        Rd r{c.raw.p, c.raw.p + c.raw.avail};
        const uint32_t n = c.raw.avail ? r.u32() : 0;
        info->raw_count = r.bad || n > INT32_MAX ? 0 : (int)n;
    }
    info->chunk_bytes = (int64_t)c.stored[SEC_CHUNKS].size;
    info->thumbnail_bytes = (int64_t)c.stored[SEC_THUMBS].size;
    info->fic_bytes = (int64_t)c.stored[SEC_FIC].size;
    info->raw_bytes = (int64_t)c.stored[SEC_RAW].size;
    auto crc_state = [&](int s) {
        const Section &v = c.view(s);
        return (c.stored[s].size == 0 || c.opened[s] <= 0 || !v.complete()) ? -1
               : (v.crc_ok() ? 1 : 0);
    };
    info->chunk_crc_ok = crc_state(SEC_CHUNKS);
    info->thumbnail_crc_ok = crc_state(SEC_THUMBS);
    info->fic_crc_ok = crc_state(SEC_FIC);
    info->raw_crc_ok = crc_state(SEC_RAW);

    /* Where the base layers end: a client that fetches this many bytes has a
     * complete coarse picture and can stop, or continue for detail.  Only a
     * plain FIC section has such a prefix. */
    if (!c.packed(SEC_FIC) && c.stored[SEC_FIC].size)
        info->base_prefix_bytes =
            (int64_t)(c.stored[SEC_FIC].p - data) + (int64_t)c.detail_offset;
    info->tile_count = (int64_t)c.tile_count;
    info->tiles_x = c.tiles_x;
    info->tiles_y = c.tiles_y;
    return FIF_OK;
}

fif_status fif_get_tile(const uint8_t *data, size_t size, int index, fif_tile *out) {
    if (!data || !out || index < 0) return FIF_ERR_ARG;
    Container c;
    if (!parse_container(data, size, c)) return FIF_ERR_FORMAT;
    /* The offsets are into `data`; a packed section has none there. */
    if (c.locked(SEC_FIC)) return FIF_ERR_ENCRYPTED;
    if (c.packed(SEC_FIC)) return FIF_ERR_UNSUPPORTED;
    if ((uint32_t)index >= c.tile_count) return FIF_ERR_ARG;
    const std::vector<Tile> all = make_tiles(c.pw, c.ph, c.ts);
    if ((size_t)index >= all.size()) return FIF_ERR_FORMAT;
    const Tile &t = all[(size_t)index];
    std::memset(out, 0, sizeof(*out));
    /* Clipped to the picture: the padding is not part of anything a caller
     * can ask for. */
    out->x = t.x; out->y = t.y;
    out->w = std::min(t.w, c.width - t.x);
    out->h = std::min(t.h, c.height - t.y);
    if (out->w < 0) out->w = 0;
    if (out->h < 0) out->h = 0;
    /* The layers' offsets follow from the directory alone: bases back to back
     * after it, then details back to back. */
    Rd r{c.fic.p, c.fic.p + c.fic.avail};
    uint64_t base_before = 0, base_total = 0, det_before = 0;
    for (uint32_t i = 0; i < c.tile_count; ++i) {
        const uint64_t b = r.var(), d = r.var();
        if (r.bad) return FIF_ERR_FORMAT;
        if (i < (uint32_t)index) { base_before += b; det_before += d; }
        if (i == (uint32_t)index) { out->base_size = (int64_t)b; out->detail_size = (int64_t)d; }
        base_total += b;
    }
    const int64_t dir_end = (int64_t)(r.p - data);
    const int64_t bo = dir_end + (int64_t)base_before;
    const int64_t dofs = dir_end + (int64_t)base_total + (int64_t)det_before;
    const int64_t fic_end = (int64_t)(c.fic.p - data) + (int64_t)c.fic.avail;
    out->base_offset = bo + out->base_size <= fic_end ? bo : -1;
    out->detail_offset = dofs + out->detail_size <= fic_end ? dofs : -1;
    return FIF_OK;
}

/* The decode body.  `dbg` is null for every normal decode, which costs one
 * predictable branch per tile-iteration and nothing per pixel; fif_decode
 * below is the public entry and always passes null. */
static fif_status decode_run(const uint8_t *data, size_t size,
                             const fif_decode_options *opts,
                             fif_image *out_img, fif_stats *stats,
                             fif::DebugSink *dbg) {
    if (!data || !out_img) return FIF_ERR_ARG;

    fif_decode_options o;
    if (opts) o = *opts; else fif_decode_options_defaults(&o);
    /* One of the two may be above 1.  An upscale can be any integer - every
     * coordinate is simply multiplied, so block edges stay on pixels - but a
     * native reduction has to be a power of two: the leaf grid is a quadtree,
     * so anything else would put block edges between pixels.  Other sizes go
     * through fif_decode's resampling path, not through here. */
    if (o.scale_num < 1 || o.scale_num > kMaxScale ||
        o.scale_den < 1 || o.scale_den > 8 || (o.scale_den & (o.scale_den - 1)) ||
        (o.scale_num > 1 && o.scale_den > 1))
        return FIF_ERR_UNSUPPORTED;
    const Scale sc{o.scale_num, o.scale_den};

    Container c;
    if (!parse_container(data, size, c)) return FIF_ERR_FORMAT;
    {
        const fif_status st = open_section(c, SEC_FIC);
        if (st != FIF_OK) return st;
    }

    const auto t0 = std::chrono::steady_clock::now();
    const CodecParams &p = c.p;
    const int nplanes = c.nplanes, cs = c.cs;
    const int pw = c.pw, ph = c.ph;
    /* A short file is not a broken one: the layers are ordered so that a prefix
     * is a valid, lower-quality stream.  Take whatever of the FIC section
     * arrived. */
    const uint8_t *fic = c.fic.p;
    const size_t fic_size = c.fic.avail;

    /* Tile geometry is not carried in the stream; it follows from the padded
     * dimensions and the tile size, in raster order. */
    const std::vector<Tile> all = make_tiles(pw, ph, c.ts);

    /* read the transform part directory */
    struct Part {
        Tile tile;
        const uint8_t *base = nullptr; size_t base_size = 0;
        const uint8_t *detail = nullptr; size_t detail_size = 0;
    };
    std::vector<Part> parts;
    {
        Rd r{fic, fic + fic_size};
        std::vector<uint64_t> base_len(c.tile_count), det_len(c.tile_count);
        for (const Tile &t : all) {
            Part pt{};
            pt.tile = t;
            parts.push_back(pt);
        }
        for (uint32_t i = 0; i < c.tile_count; ++i) {
            base_len[i] = r.var();
            det_len[i] = r.var();
            /* Attacker-controlled: a length that alone exceeds the part section
             * can never be satisfied, and catching it here keeps the running
             * cursor below from having to worry about overflow. */
            if (r.bad) return FIF_ERR_FORMAT;
            if (base_len[i] > fic_size || det_len[i] > fic_size)
                return FIF_ERR_FORMAT;
        }

        /* Base layers follow the directory, detail layers follow those, and
         * the header says where the detail layers start.  That is where the
         * lengths say they start, or the file is lying about one of them. */
        uint64_t base_total = 0;
        for (uint32_t i = 0; i < c.tile_count; ++i) base_total += base_len[i];
        if ((uint64_t)(r.p - fic) + base_total != c.detail_offset) return FIF_ERR_FORMAT;

        /* A truncated file simply runs out: every tile whose base layer is
         * complete still decodes, at DC resolution. */
        const uint8_t *cur = r.p;
        const uint8_t *end = fic + fic_size;
        for (size_t i = 0; i < parts.size(); ++i) {
            if ((uint64_t)(end - cur) < base_len[i]) { parts.resize(i); break; }
            parts[i].base = cur; parts[i].base_size = base_len[i];
            cur += base_len[i];
        }
        if (parts.size() == c.tile_count && !o.base_only) {
            for (size_t i = 0; i < parts.size(); ++i) {
                if ((uint64_t)(end - cur) < det_len[i]) break;
                parts[i].detail = cur; parts[i].detail_size = det_len[i];
                cur += det_len[i];
            }
        }
        if (parts.empty()) return FIF_ERR_FORMAT;
    }

    /* Which tiles to decode, and the box of the plane they span.  Without a
     * region the box is the whole padded plane.  With one it is the tiles the
     * region touches, widened by a margin: the post-processing reaches less
     * than two blocks from where it starts, so with the neighbours within two
     * blocks of the region decoded as well, every pixel of the region comes
     * out exactly as in a full decode.  The debug sink speaks whole-image
     * coordinates and keeps its own tile mask, so it always gets the whole
     * plane. */
    std::vector<uint8_t> want(all.size(), 1);
    const bool region = !dbg && (o.region_w > 0 || o.region_h > 0);
    if (region && (o.region_w <= 0 || o.region_h <= 0 || o.region_x < 0 || o.region_y < 0 ||
                   o.region_x > c.width - o.region_w || o.region_y > c.height - o.region_h))
        return FIF_ERR_ARG;
    if (!dbg && o.tile_list) {
        std::fill(want.begin(), want.end(), 0);
        for (int k = 0; k < o.tile_list_len; ++k) {
            const int t = o.tile_list[k];
            if (t < 0 || (size_t)t >= all.size()) return FIF_ERR_ARG;
            want[(size_t)t] = 1;
        }
    }
    int bx0 = 0, by0 = 0, bx1 = pw, by1 = ph;
    if (region) {
        /* Two blocks: the DC smoothing bends a block by its neighbours, and
         * the seam filter then carries that up to 8 samples into the next. */
        const int m = 2 * (p.max_block << cs);
        const int ex0 = o.region_x - m, ey0 = o.region_y - m;
        const int ex1 = o.region_x + o.region_w + m, ey1 = o.region_y + o.region_h + m;
        bx0 = pw; by0 = ph; bx1 = 0; by1 = 0;
        for (size_t i = 0; i < all.size(); ++i) {
            const Tile &t = all[i];
            const bool hit = t.x < ex1 && t.x + t.w > ex0 && t.y < ey1 && t.y + t.h > ey0;
            if (!hit) { want[i] = 0; continue; }
            bx0 = std::min(bx0, t.x); by0 = std::min(by0, t.y);
            bx1 = std::max(bx1, t.x + t.w); by1 = std::max(by1, t.y + t.h);
        }
    }

    /* Bound the work before committing to it: the dimensions come straight out
     * of the file and the padded, scaled plane is what gets allocated. */
    const int64_t cap = o.max_pixels > 0 ? o.max_pixels
                                         : (int64_t)FIF_DEFAULT_MAX_PIXELS;
    const int bw = sc.pos(bx1) - sc.pos(bx0), bh = sc.pos(by1) - sc.pos(by0);
    if ((int64_t)bw * bh > cap) return FIF_ERR_TOO_LARGE;

    /* Tiles whose base layer did not arrive stay mid-grey rather than black.
     * Filled at allocation: on a nine-hundred-megapixel image, zeroing the
     * planes and then overwriting them with grey is two passes over several
     * gigabytes to arrive where one pass already was. */
    Plane full[3];
    full[0].resize(bw, bh, 128.f);
    if (nplanes == 3)
        for (int c = 1; c < 3; ++c)
            full[c].resize(sc.pos(bx1 >> cs) - sc.pos(bx0 >> cs),
                           sc.pos(by1 >> cs) - sc.pos(by0 >> cs), 128.f);

    const int deblock = (o.deblock >= 0) ? o.deblock : std::max(c.rec_deblock, 0);
    const bool smooth_dc = o.smooth_dc != 0;
    /* The seam map serves the DC smoothing too: it tells it which neighbours
     * are smooth enough to blend with. */
    const bool want_edges = deblock > 0 || smooth_dc;
    std::vector<std::vector<FlatRect>> flats[3];
    if (smooth_dc)
        for (int c = 0; c < nplanes; ++c) flats[c].resize(parts.size());
    /* Seam maps at min-block granularity, one per plane of `pl`. */
    int cmin, cmax;
    chroma_block_range(p, &cmin, &cmax);
    const auto init_edges = [&](EdgeMap *em, const Plane *pl) {
        for (int c = 0; c < nplanes; ++c)
            em[c].init(pl[c].w, pl[c].h, sc.len(c ? cmin : p.min_block));
    };
    EdgeMap edges[3];
    if (want_edges) init_edges(edges, full);

    /* Precedence: the caller's value, then the stream's recommendation, then
     * the decoder's default.  Zero at either of the first two levels means
     * "no opinion" and passes the decision down; a caller's cap wins above the
     * stream's value as well as below it. */
    const int max_it = o.max_iterations > 0 ? o.max_iterations
                     : c.rec_iterations > 0 ? c.rec_iterations
                     : FIF_DEFAULT_MAX_ITERATIONS;
    const double eps = o.convergence > 0  ? o.convergence
                     : c.rec_epsilon > 0  ? (double)c.rec_epsilon
                     : FIF_DEFAULT_CONVERGENCE;

    /* Frame 0 is the DC field the iteration starts from, then one frame per
     * application of the map. */
    if (dbg) {
        const int ow0 = sc.pos(c.width), oh0 = sc.pos(c.height);
        int step = 1;
        if (dbg->preview_max > 0) {
            const int longest = std::max(ow0, oh0);
            while (longest / step > dbg->preview_max) ++step;
        }
        dbg->init(max_it + 1, ow0, oh0, c.channels, step);
        /* Byte offsets into the file; a packed FIC section has none there. */
        const auto file_off = [&](const uint8_t *q) {
            return q && !c.packed(SEC_FIC) ? (int64_t)(q - data) : (int64_t)-1;
        };
        for (size_t i = 0; i < parts.size(); ++i) {
            dbg->set_tile_geometry((int)i, sc.pos(parts[i].tile.x),
                                   sc.pos(parts[i].tile.y),
                                   sc.pos(parts[i].tile.w),
                                   sc.pos(parts[i].tile.h));
            DebugTile &dt = dbg->tiles[i];
            dt.base_off = file_off(parts[i].base);
            dt.base_len = (int64_t)parts[i].base_size;
            dt.det_off  = file_off(parts[i].detail);
            dt.det_len  = (int64_t)parts[i].detail_size;
        }
    }

    ThreadPool pool(o.threads);
    std::vector<int> iters(parts.size(), 0);
    std::atomic<bool> failed{false};
    /* A tile is entropy-decoded and iterated to its own fixed point in one go,
     * so tiles finished is the only progress signal there is. */
    Progress prog(o.progress, o.progress_user);
    std::atomic<size_t> done{0};

    /* Domains never leave their tile, so each tile is its own contraction and
     * can iterate to its own fixed point independently. */
    pool.parallel_for(parts.size(), [&](size_t i, int) {
        if (failed.load(std::memory_order_relaxed)) return;
        if ((dbg && !dbg->tile_enabled((int)i)) || !want[i]) {
            prog.step(done.fetch_add(1) + 1, parts.size());
            return;                      /* stays mid-grey, as if never sent */
        }
        const auto tile_t0 = std::chrono::steady_clock::now();
        Tile tile = parts[i].tile;
        Tile local = tile;
        local.x = 0; local.y = 0;
        if (!deserialize_tile(parts[i].base, parts[i].base_size,
                              parts[i].detail, parts[i].detail_size,
                              nplanes, p, local)) {
            failed.store(true, std::memory_order_relaxed);
            return;
        }
        /* Tiles cover disjoint cells, so filling the shared maps needs no lock. */
        if (want_edges)
            collect_edges(local, nplanes, p, sc.pos(tile.x) - sc.pos(bx0),
                          sc.pos(tile.y) - sc.pos(by0), sc, edges);

        const int lw = sc.pos(tile.w), lh = sc.pos(tile.h);
        Plane a[3], b[3];
        a[0].resize(lw, lh); b[0].resize(lw, lh);
        if (nplanes == 3) {
            const int cw = sc.pos(tile.w >> cs), ch = sc.pos(tile.h >> cs);
            for (int c = 1; c < 3; ++c) { a[c].resize(cw, ch); b[c].resize(cw, ch); }
        }

        /* Tile-local seam map, for filtering inside the iteration. */
        EdgeMap local_edges[3];
        if (deblock > 0) {
            init_edges(local_edges, a);
            collect_edges(local, nplanes, p, 0, 0, sc, local_edges);
        }
        /* Frame 0 is the coded means.  The map is a contraction, so the fixed
         * point does not depend on the start; starting from the DC field that
         * the stream carries exactly puts the iteration most of the way there
         * before the first pass, and a tile cut short by max_iterations still
         * shows a coherent picture rather than noise. */
        init_from_dc(local, nplanes, p, a, sc);

        /* Capturing a tile costs a colour conversion, and in from-luma mode a
         * chroma render too, because chroma is not part of the contraction and
         * is normally painted once at the end.  Both are debug-only. */
        Plane cap[3];
        const auto capture = [&](int frame_ix, Plane *src, int planes_now) {
            if (!dbg) return;
            const Plane *use = src;
            if (planes_now < nplanes) {
                for (int c = 0; c < nplanes; ++c) cap[c] = src[c];
                render_chroma_from_luma(local, nplanes, p, cap[0], cap, sc);
                use = cap;
            }
            dbg->on_iteration(frame_ix, use, nplanes, cs,
                              sc.pos(tile.x), sc.pos(tile.y));
        };

        /* Filtering inside the loop is what actually removes seams: the decoder
         * feeds its own output back in as domain data, so a seam left standing
         * is re-read and reproduced on the next pass.  Filtering every step
         * makes the filtered image the fixed point, which is the one worth
         * having - but filtering while the picture is still forming drags the
         * whole trajectory out.  So the attractor is converged first, and the
         * filter is switched on only once it has settled, starting the second
         * contraction already close to its own fixed point. */
        const int filter_budget = deblock > 0 ? kDeblockInLoop : 0;
        int filtered = 0;
        DecodeScratch scratch;
        /* Chroma predicted from luma is not part of the contraction - it is a
         * function of the converged luma - so it is left out of the loop and
         * painted once at the end. */
        const int iter_planes = chroma_from_luma(p) ? 1 : nplanes;
        /* Zero or less is "no opinion", the same reading --eps and
         * --iterations give it; at 1 the relaxation pass is skipped outright,
         * so the default path costs nothing. */
        const float w = o.relaxation > 0 ? (float)o.relaxation : 1.f;
        capture(0, a, iter_planes);
        int used = 0;
        bool filtering = false;
        for (int it = 0; it < max_it; ++it) {
            decode_tile(local, iter_planes, p, a, b, scratch, sc);
            if (filtering && filtered++ < filter_budget)
                for (int c = 0; c < iter_planes; ++c)
                    deblock_plane(b[c], local_edges[c], p, deblock);
            /* After the filter, not before it: in the filtered phase the
             * operator whose fixed point is being sought is deblock o T, and
             * that is the one worth relaxing.  Before the convergence test,
             * so converged() measures the step actually taken. */
            if (w != 1.f) relax(a, b, iter_planes, w);
            const bool settled = converged(a, b, iter_planes, eps);
            /* The graph wants the size of the step, which converged() will not
             * tell us: it stops at the first sample past eps, which is exactly
             * what makes it cheap. */
            if (dbg) dbg->set_delta((int)i, it + 1, plane_delta(a, b, iter_planes));
            for (int c = 0; c < iter_planes; ++c) std::swap(a[c], b[c]);
            used = it + 1;
            capture(used, a, iter_planes);
            if (filtering && filtered >= filter_budget) break;
            if (settled) {
                if (filter_budget <= 0 || filtering) break;
                filtering = true;          /* converge the filtered attractor */
            }
        }
        iters[i] = used;
        if (iter_planes < nplanes)
            render_chroma_from_luma(local, nplanes, p, a[0], a, sc);

        if (dbg) {
            dbg->on_leaves((int)i, local, sc.pos(tile.x), sc.pos(tile.y), nplanes, p, sc);
            /* A tile that settled early is still on screen in later frames. */
            dbg->on_tile_done(used, a, nplanes, cs,
                              sc.pos(tile.x), sc.pos(tile.y));
            dbg->set_time((int)i, std::chrono::duration<float, std::milli>(
                                      std::chrono::steady_clock::now() - tile_t0).count());
        }

        for (int c = 0; c < nplanes; ++c) {
            const int sh = (c == 0) ? 0 : cs;
            const int ox = sc.pos(tile.x >> sh) - sc.pos(bx0 >> sh);
            const int oy = sc.pos(tile.y >> sh) - sc.pos(by0 >> sh);
            if (smooth_dc)
                collect_flats(local, c, p, ox, oy, sc, flats[c][i]);
            for (int y = 0; y < a[c].h; ++y)
                std::copy_n(a[c].row(y), a[c].w, full[c].row(oy + y) + ox);
        }
        prog.step(done.fetch_add(1) + 1, parts.size());
    });
    if (failed.load()) return FIF_ERR_FORMAT;

    /* Soft DC surfaces, before the seam filter: what is left of a step
     * between two flat blocks afterwards is a small one, and the filter's
     * ramp finishes it.  The tolerance is the step that still counts as
     * quantisation rather than as an edge, tied to the DC quantiser. */
    if (smooth_dc) {
        const float qstep = 255.0f / (float)(1 << p.dc_bits);
        for (int c = 0; c < nplanes; ++c) {
            std::vector<FlatRect> all_flats;
            for (auto &v : flats[c]) all_flats.insert(all_flats.end(), v.begin(), v.end());
            smooth_flat_blocks(full[c], all_flats, edges[c], kSmoothTol * qstep, pool);
        }
    }

    /* Banded rather than one plane per worker: on a large image the assembled
     * planes are most of the decode, and three of them is not parallelism. */
    if (deblock > 0)
        for (int c = 0; c < nplanes; ++c) {
            const EdgeMap &em = edges[c];
            pool.bands(em.ch, 1, [&](int b0, int b1) {
                deblock_vertical(full[c], em, p, deblock, b0, b1);
            });
            pool.bands(em.cw, 1, [&](int b0, int b1) {
                deblock_horizontal(full[c], em, p, deblock, b0, b1);
            });
        }

    /* The output: the region, or the picture without its padding. */
    const int rx = region ? o.region_x : 0, ry = region ? o.region_y : 0;
    const int ow = std::max(1, sc.pos(rx + (region ? o.region_w : c.width)) - sc.pos(rx));
    const int oh = std::max(1, sc.pos(ry + (region ? o.region_h : c.height)) - sc.pos(ry));
    const int px0 = sc.pos(rx) - sc.pos(bx0), py0 = sc.pos(ry) - sc.pos(by0);
    /* Edge-aware chroma needs its coefficient planes before the conversion
     * reads them; they are a quarter of the picture each. */
    const bool use_guide = nplanes == 3 && cs > 0 && !o.chroma_bilinear;
    Plane guide[3];
    if (use_guide) {
        luma_to_chroma_grid(full[0], cs, full[1].w, full[1].h, guide[2], pool);
        chroma_guide(guide[2], full[1], kGuideEps, kGuideSlope, guide[0], pool);
        chroma_guide(guide[2], full[2], kGuideEps, kGuideSlope, guide[1], pool);
    }
    ConvertOpts conv;
    conv.guide = use_guide ? guide : nullptr;
    conv.dither = o.dither != 0;
    conv.dither_x0 = sc.pos(rx);
    conv.dither_y0 = sc.pos(ry);
    fif_status st = fif_image_alloc(out_img, ow, oh, c.channels);
    if (st != FIF_OK) return st;
    /* One row band per worker: the conversion is a fifth of the decode on a
     * large image and there is nothing shared between rows to serialise it. */
    pool.bands(oh, 1, [&](int y0, int y1) {
        planes_to_image(full, nplanes, cs, ow, y0, y1, *out_img, px0, py0, &conv);
    });

    if (stats) {
        std::memset(stats, 0, sizeof(*stats));
        stats->decode_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        stats->compressed_bytes = (int64_t)size;
        stats->bits_per_pixel = 8.0 * size / ((double)c.width * c.height);
        stats->iterations_used = iters.empty()
            ? 0 : *std::max_element(iters.begin(), iters.end());
    }
    return FIF_OK;
}

fif_status fif_decode(const uint8_t *data, size_t size,
                      const fif_decode_options *opts,
                      fif_image *out_img, fif_stats *stats) {
    fif_decode_options o;
    if (opts) o = *opts; else fif_decode_options_defaults(&o);
    const bool den_native = o.scale_den >= 1 && o.scale_den <= 8 &&
                            !(o.scale_den & (o.scale_den - 1));
    if (o.out_width <= 0 && o.out_height <= 0 && den_native)
        return decode_run(data, size, &o, out_img, stats, nullptr);
    if (!data || !out_img) return FIF_ERR_ARG;
    if (o.out_width < 0 || o.out_height < 0 || o.scale_num < 1 || o.scale_den < 1)
        return FIF_ERR_ARG;

    fif_info in{};
    fif_status st = fif_get_info(data, size, &in);
    if (st != FIF_OK) return st;
    /* A region is the picture as far as sizing is concerned. */
    const bool region = o.region_w > 0 || o.region_h > 0;
    const int w = region ? o.region_w : in.width, h = region ? o.region_h : in.height;
    if (region && (w <= 0 || h <= 0)) return FIF_ERR_ARG;

    /* The box to fill.  A bare 1/N is a box too, just one derived from N. */
    int tw = o.out_width, th = o.out_height;
    if (tw <= 0 && th <= 0) {
        tw = std::max(1, (int)std::lround((double)w * o.scale_num / o.scale_den));
        th = std::max(1, (int)std::lround((double)h * o.scale_num / o.scale_den));
    } else if (tw <= 0) {
        tw = std::max(1, (int)std::lround((double)th * w / h));
    } else if (th <= 0) {
        th = std::max(1, (int)std::lround((double)tw * h / w));
    }

    /* Cover the box and crop the excess: the aspect ratio is kept, so one axis
     * fits exactly and the other overhangs by less than a pixel or by the
     * part that is cropped. */
    const double f = std::max((double)tw / w, (double)th / h);
    const int k = std::max(1, (int)std::ceil(f - 1e-9));
    if (k > kMaxScale) return FIF_ERR_UNSUPPORTED;
    const int rw = std::clamp((int)std::lround(w * f), tw, w * k);
    const int rh = std::clamp((int)std::lround(h * f), th, h * k);

    fif_decode_options ko = o;
    ko.scale_num = k; ko.scale_den = 1;
    ko.out_width = ko.out_height = 0;
    fif_image big{};
    st = decode_run(data, size, &ko, &big, stats, nullptr);
    if (st != FIF_OK) return st;

    fif_image mid{};
    if (rw == big.width && rh == big.height) {
        mid = big;
    } else {
        st = fif_image_downscale(&big, rw, rh, &mid);
        fif_image_free(&big);
        if (st != FIF_OK) return st;
    }
    if (rw == tw && rh == th) { *out_img = mid; return FIF_OK; }

    fif_image crop{};
    st = fif_image_alloc(&crop, tw, th, mid.channels);
    if (st != FIF_OK) { fif_image_free(&mid); return st; }
    const int x0 = (rw - tw) / 2, y0 = (rh - th) / 2;
    for (int y = 0; y < th; ++y)
        std::memcpy(crop.pixels + (size_t)y * crop.stride,
                    mid.pixels + (size_t)(y0 + y) * mid.stride + (size_t)x0 * mid.channels,
                    (size_t)tw * mid.channels);
    fif_image_free(&mid);
    *out_img = crop;
    return FIF_OK;
}

} /* extern "C" */

namespace fif {
/* Internal entry for the visualiser; not part of the public C API. */
fif_status decode_with_debug(const uint8_t *data, size_t size,
                             const fif_decode_options *opts,
                             fif_image *out_img, fif_stats *stats,
                             DebugSink *dbg) {
    return decode_run(data, size, opts, out_img, stats, dbg);
}
} /* namespace fif */
