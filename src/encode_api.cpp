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

/* fif_encode: settles the parameters, runs the tile encoder (encode.cpp) over
 * all tiles and hands the result, with the chunks and the thumbnail, to the
 * container writer (format.cpp). */
#include "codec.h"
#include "format.h"
#include "progress.h"
#include "threadpool.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstring>

namespace fif {
namespace {

void chunk_uint(Buf &b, uint32_t &count, const char type[4], const char *name,
                uint64_t v, int width) {
    uint8_t d[8];
    for (int i = 0; i < width; ++i) d[i] = (uint8_t)(v >> (8 * i));
    write_chunk(b, type, FIF_VALUE_UINT, name, std::strlen(name), d, (uint64_t)width);
    ++count;
}
void chunk_text(Buf &b, uint32_t &count, const char type[4], const char *name,
                const char *v) {
    write_chunk(b, type, FIF_VALUE_TEXT, name, std::strlen(name), v, std::strlen(v));
    ++count;
}

/* Deblocking strength the encoder suggests for its own output.  Coarse
 * quantisation and large blocks seam harder, so the filter tracks quality. */
int recommended_deblock(const CodecParams &p) {
    /* Measured: at quality 50 and below, full strength wins on PSNR, MS-SSIM
     * and measured blockiness simultaneously - there is no trade to make.  The
     * curve only backs off near the top, where seams are already faint and the
     * filter starts costing detail. */
    return std::clamp(130 - p.quality, 25, 100);
}

} /* namespace */

/* ------------------------------------------------------------------ encode */

struct EncodeResult {
    std::vector<uint8_t> bytes;
    EncodeCounters ctr;
    double search_seconds = 0;
    SymbolStats sym;
    /* Where this encode sits in the caller's overall job.  A --target search
     * runs several encodes; each one is a slice of one progress bar. */
    double progress_base = 0.0, progress_span = 1.0;
};

/* Source description, fixed chunks, caller's chunks and the thumbnail. */
static fif_status build_meta(const fif_image *img, const fif_encode_options &o,
                             ContainerInput &meta) {
    std::memcpy(meta.remarks, o.remarks, 4);
    const int sections = FIF_SECTION_CHUNKS | FIF_SECTION_THUMBNAILS | FIF_SECTION_FIC;
    if (o.compress & ~sections) return FIF_ERR_ARG;
    meta.compress = (uint8_t)o.compress;
    if (meta.compress && !have_zlib()) return FIF_ERR_UNSUPPORTED;
    meta.source_width  = o.source_width  > 0 ? (uint32_t)o.source_width  : (uint32_t)img->width;
    meta.source_height = o.source_height > 0 ? (uint32_t)o.source_height : (uint32_t)img->height;
    meta.source_scale  = o.source_scale  > 0 ? o.source_scale : 1.0;

    Buf b;
    char ver[32];
    std::snprintf(ver, sizeof ver, "libfif v%d.%d", FIF_VERSION_MAJOR, FIF_VERSION_MINOR);
    if (!o.omit_chunks) chunk_text(b, meta.chunk_count, "ENCO", "encoder", ver);
    /* The one variable-length thing the header used to carry; as a chunk it
     * leaves the header a fixed 144 bytes. */
    if (!o.omit_chunks && o.source_name && *o.source_name) {
        const size_t n = std::min<size_t>(std::strlen(o.source_name), 0xFFFF);
        write_chunk(b, "ORGN", FIF_VALUE_TEXT, "source_name", 11, o.source_name, n);
        ++meta.chunk_count;
    }
    /* What the source file cost on disk.  Eight bytes: a source larger than
     * 4 GiB is odd but not impossible, and the field is written once. */
    if (!o.omit_chunks && o.source_bytes > 0)
        chunk_uint(b, meta.chunk_count, "ORGN", "src_size", (uint64_t)o.source_bytes, 8);
    /* Who made the encoder and the format, and where to read about them -
     * the banner of the fif tool, as one text in one chunk so that it stands
     * in the file as one block.  ENCO, not TEXT or META: it describes the
     * software, not the picture or its owner. */
    if (!o.omit_chunks && !o.omit_credits) {
        char credits[192];
        std::snprintf(credits, sizeof credits,
                      "libfif v%d.%d - (c) 2026 Oskar Behzadpour\n"
                      "FIF2 format v%d.%d by Rezar Behzad\n"
                      "www.FractalImageCompression.com",
                      FIF_VERSION_MAJOR, FIF_VERSION_MINOR, kVerMajor, kVerMinor);
        chunk_text(b, meta.chunk_count, "ENCO", "credits", credits);
    }
    /* What the encoder assumed the input to be.  The codec does not convert
     * colour, so this is a statement about the source, not a transform -
     * which is why it goes into ORGN like everything else from the source. */
    if (!o.omit_chunks) {
        chunk_text(b, meta.chunk_count, "ORGN", "transfer", "sRGB");
        chunk_text(b, meta.chunk_count, "ORGN", "primaries", "sRGB");
    }
    for (int i = 0; !o.omit_chunks && i < o.chunk_count && o.chunks; ++i) {
        const fif_chunk &c = o.chunks[i];
        if (!c.name) return FIF_ERR_ARG;
        const size_t nl = c.name_len ? c.name_len : std::strlen(c.name);
        if (nl > 0xFFFF || (c.size && !c.data)) return FIF_ERR_ARG;
        char type[4] = {' ', ' ', ' ', ' '};
        for (int k = 0; k < 4 && c.type[k]; ++k) type[k] = c.type[k];
        write_chunk(b, type, (uint8_t)c.value_type, c.name, nl, c.data, c.size);
        ++meta.chunk_count;
    }
    meta.chunks = std::move(b.v);

    if (o.thumbnail_size > 0 && !o.base_only) {
        /* A third of the longest edge: a preview, not a second copy of the
         * picture at a size where it would compete with the fractal code. */
        const int longest = std::max(img->width, img->height);
        const int edge = std::min(o.thumbnail_size, longest / 3);
        if (edge < 1) return FIF_ERR_ARG;
        const double f = (double)edge / longest;
        const int tw = std::max(1, (int)std::lround(img->width * f));
        const int th = std::max(1, (int)std::lround(img->height * f));
        fif_image small{};
        fif_status st = fif_image_downscale(img, tw, th, &small);
        if (st != FIF_OK) return st;
        std::vector<uint8_t> jpg;
        st = jpeg_encode_mem(small, o.thumbnail_quality > 0
                                        ? std::min(o.thumbnail_quality, 100) : 85, jpg);
        fif_image_free(&small);
        if (st != FIF_OK) return st;
        /* Stored as the JPEG file itself.  The format allows any number of
         * thumbnails in any format; this encoder writes one. */
        ContainerInput::Thumb t{{'J', 'P', 'E', 'G'}, (uint32_t)tw, (uint32_t)th, std::move(jpg)};
        meta.thumbs.push_back(std::move(t));
    }
    /* The header counts the thumbnails in one byte.  This encoder writes at
     * most one, so the limit is only ever reached by a caller of the
     * internal container writer. */
    if (meta.thumbs.size() > kMaxThumbnails) return FIF_ERR_ARG;
    return FIF_OK;
}

/* Parameters for one encode at quality q: the effort preset, then the
 * caller's explicit overrides. */
static CodecParams settle(CodecParams p, const fif_encode_options &o, int q) {
    derive_params(p, q, o.effort);
    if (o.local_radius >= 0) p.local_radius = o.local_radius;
    if (o.kd_candidates >= 0) p.kd_candidates = o.kd_candidates;
    if (o.domain_shift >= 0) p.domain_shift = o.domain_shift;
    p.planar_mode = o.planar != 0;
    return p;
}

static fif_status encode_once(const fif_image *img, const fif_encode_options *o,
                              CodecParams p, const ContainerInput &meta,
                              EncodeResult &res) {
    const int nplanes =
        (img->channels == 1 || p.subsampling == FIF_SUBSAMPLING_400) ? 1 : 3;
    const int cs = (nplanes == 1) ? 0 : chroma_shift(p);

    /* Pad to whole max_block units so no range block ever straddles the edge. */
    const int align = p.max_block << cs;
    const int pw = align_up(img->width, align);
    const int ph = align_up(img->height, align);

    Plane planes[3];
    if (nplanes == 1) gray_to_plane(*img, planes, pw, ph);
    else if (cs)      rgb_to_planes_420(*img, planes, pw, ph);
    else              rgb_to_planes(*img, planes, pw, ph);

    int ts = p.tile_size;
    if (ts <= 0) ts = ((int64_t)pw * ph > 2000000) ? 512 : 256;
    ts = effective_tile_size(ts, align);
    p.tile_size = ts;
    std::vector<Tile> tiles = make_tiles(pw, ph, ts);

    ThreadPool pool(o->threads);
    std::vector<EncodeCounters> ctrs(pool.size());
    std::vector<std::vector<uint8_t>> payloads(tiles.size());
    std::vector<std::vector<uint8_t>> details(tiles.size());
    std::vector<SymbolStats> sstats(tiles.size());
    const auto t0 = std::chrono::steady_clock::now();

    /* Trying both candidate sources on every tile doubles the search.  Which
     * one wins is a property of the picture, so probe a sample of tiles and, if
     * they agree, commit the rest to the winner.  The sample is spread across
     * the image rather than taken from the front: the leading tiles are one
     * strip of the picture and routinely disagree with the rest of it. */
    const bool wide = p.kd_candidates > 0 || p.exhaustive;
    std::vector<size_t> probe_ix, rest_ix;
    if (wide && tiles.size() >= 6) {
        const size_t n_probe = std::clamp<size_t>(tiles.size() / 5, 3, 10);
        std::vector<bool> picked(tiles.size(), false);
        for (size_t k = 0; k < n_probe; ++k) {
            const size_t i = (k * tiles.size() + tiles.size() / 2) / n_probe;
            if (i < tiles.size()) picked[i] = true;
        }
        for (size_t i = 0; i < tiles.size(); ++i)
            (picked[i] ? probe_ix : rest_ix).push_back(i);
    } else {
        for (size_t i = 0; i < tiles.size(); ++i) probe_ix.push_back(i);
    }

    Progress prog(o->progress, o->progress_user);
    prog.stage(res.progress_base, res.progress_span);
    std::atomic<size_t> done{0};

    std::vector<int> winners(probe_ix.size(), -1);
    pool.parallel_for(probe_ix.size(), [&](size_t k, int tid) {
        const size_t i = probe_ix[k];
        encode_tile(planes, nplanes, tiles[i], p, ctrs[tid],
                    payloads[i], details[i], sstats[i], -1, &winners[k]);
        prog.step(done.fetch_add(1) + 1, tiles.size());
    });

    if (!rest_ix.empty()) {
        int votes[2] = {0, 0};
        for (int w : winners) if (w >= 0 && w < 2) ++votes[w];
        /* Only commit on a clear verdict; a split jury keeps both in play. */
        const int lead = votes[0] >= votes[1] ? 0 : 1;
        const int total = votes[0] + votes[1];
        const int forced = (total > 0 && votes[lead] * 5 >= total * 4) ? lead : -1;
        pool.parallel_for(rest_ix.size(), [&](size_t k, int tid) {
            const size_t i = rest_ix[k];
            encode_tile(planes, nplanes, tiles[i], p, ctrs[tid],
                        payloads[i], details[i], sstats[i], forced, nullptr);
            prog.step(done.fetch_add(1) + 1, tiles.size());
        });
    }
    res.search_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    for (const auto &c : ctrs) res.ctr += c;
    for (const auto &s2 : sstats) res.sym += s2;

    /* Directory first, then every tile's base layer, then every tile's detail
     * layer.  A reader holding a prefix of the file therefore holds a complete
     * low-frequency picture, and one that wants a single tile can compute its
     * byte ranges from the directory without scanning. */
    Buf parts;
    /* Only the two lengths are written.  Where a tile sits and how big it is
     * follows from the image dimensions and the tile size, both of which are
     * already in the stream - and a reader that derives the geometry cannot be
     * lied to about it. */
    for (size_t i = 0; i < tiles.size(); ++i) {
        parts.var(payloads[i].size());
        parts.var(o->base_only ? 0 : details[i].size());
    }
    for (const auto &b : payloads) parts.raw(b.data(), b.size());
    /* The header records where the base layers end, so a reader can fetch or
     * decode the coarse picture alone without walking the directory. */
    const uint64_t detail_offset = parts.size();
    if (!o->base_only)
        for (const auto &b : details) parts.raw(b.data(), b.size());

    ContainerInput ci = meta;
    ci.width = img->width;
    ci.height = img->height;
    ci.nplanes = nplanes;
    ci.p = p;
    ci.detail_offset = detail_offset;

    /* The chunks that depend on the parameters this encode settled on.  The
     * caller's own chunks and the fixed ones are already in meta.chunks. */
    Buf chunks;
    chunks.v = meta.chunks;
    if (!o->omit_chunks) {
        chunk_uint(chunks, ci.chunk_count, "ENCO", "quality", (uint64_t)p.quality, 1);
        chunk_uint(chunks, ci.chunk_count, "ENCO", "effort", (uint64_t)p.effort, 1);
        chunk_uint(chunks, ci.chunk_count, "DECO", "deblock", (uint64_t)recommended_deblock(p), 1);
    }
    ci.chunks = std::move(chunks.v);

    if (!write_container(ci, parts.v, res.bytes)) return FIF_ERR_UNSUPPORTED;
    return FIF_OK;
}

} /* namespace fif */

using namespace fif;

extern "C" {

int fif_thread_count(int threads) { return fif::thread_count(threads); }
int fif_cpu_cores(void) { return fif::cpu_cores(); }

void fif_context_training_begin(void) { fif::contexts_train_begin(); }
int fif_context_training_write(const char *path) {
    return fif::contexts_train_write(path) ? 1 : 0;
}

void fif_encode_options_defaults(fif_encode_options *o) {
    if (!o) return;
    /* Zero first, then set what is not zero.  Callers declare this on the stack;
     * a field added to the struct and forgotten here is otherwise garbage, and
     * when the field is a function pointer that garbage gets called. */
    *o = fif_encode_options{};
    o->quality = 70;
    o->effort = 5;
    o->subsampling = FIF_SUBSAMPLING_420;
    o->chroma_mode = FIF_CHROMA_SHARED;
    o->min_block = 4;
    o->max_block = 32;
    o->local_radius = -1;
    o->kd_candidates = -1;
    o->domain_shift = -1;
    o->planar = 1;
}

void fif_decode_options_defaults(fif_decode_options *o) {
    if (!o) return;
    *o = fif_decode_options{};
    o->relaxation = 1.0;
    o->scale_num = 1;
    o->scale_den = 1;
    o->deblock = -1;
    o->smooth_dc = 1;
}

fif_status fif_encode(const fif_image *img, const fif_encode_options *opts,
                      uint8_t **out_data, size_t *out_size, fif_stats *stats) {
    if (!img || !img->pixels || !out_data || !out_size) return FIF_ERR_ARG;
    if (img->channels != 1 && img->channels != 3) return FIF_ERR_UNSUPPORTED;

    fif_encode_options o;
    if (opts) o = *opts; else fif_encode_options_defaults(&o);

    CodecParams p;
    p.tile_size = o.tile_size;
    p.min_block = o.min_block > 0 ? o.min_block : 4;
    p.max_block = o.max_block > 0 ? o.max_block : 32;
    p.subsampling = o.subsampling;
    p.chroma_mode = o.chroma_mode;
    /* The same limits the reader applies: a file this encoder writes must be
     * one it can read back. */
    if (!valid_block_sizes(p.min_block, p.max_block)) return FIF_ERR_ARG;
    if (p.tile_size < 0) return FIF_ERR_ARG;
    if (p.subsampling < FIF_SUBSAMPLING_444 || p.subsampling > FIF_SUBSAMPLING_400 ||
        p.chroma_mode < FIF_CHROMA_SHARED || p.chroma_mode > FIF_CHROMA_FROM_LUMA)
        return FIF_ERR_ARG;
    if (o.domain_shift > 4) return FIF_ERR_ARG;
    /* The targets measure a full file; a base-only one would never reach them. */
    if (o.base_only && (o.target_bpp > 0 || o.target_psnr > 0)) return FIF_ERR_ARG;

    /* Everything about the container that does not depend on the parameters
     * the encode settles on - built once, outside a --target search. */
    ContainerInput meta;
    fif_status mst = build_meta(img, o, meta);
    if (mst != FIF_OK) return mst;

    const auto t0 = std::chrono::steady_clock::now();
    EncodeResult res;

    if (o.target_bpp > 0 || o.target_psnr > 0) {
        /* Bisect the quality knob; each probe is a full encode (plus a decode
         * when the target is expressed in PSNR). */
        /* The best probe that meets the target; failing that, the lowest
         * quality tried, which is the closest to it. */
        int lo = 0, hi = 100, best = -1, lowest = 101;
        EncodeResult bestres, lowres;
        const int probes = 7;
        for (int it = 0; it < probes && lo <= hi; ++it) {
            int q = (lo + hi) / 2;
            const CodecParams pp = settle(p, o, q);
            EncodeResult r;
            r.progress_base = (double)it / probes;
            r.progress_span = 1.0 / probes;
            fif_status st = encode_once(img, &o, pp, meta, r);
            if (st != FIF_OK) return st;

            double measure;
            bool too_high;
            if (o.target_bpp > 0) {
                measure = 8.0 * r.bytes.size() / ((double)img->width * img->height);
                too_high = measure > o.target_bpp;
            } else {
                fif_image tmp{};
                fif_decode_options dopt; fif_decode_options_defaults(&dopt);
                dopt.threads = o.threads;
                if (fif_decode(r.bytes.data(), r.bytes.size(), &dopt, &tmp, nullptr) != FIF_OK)
                    return FIF_ERR_FORMAT;
                measure = fif_psnr(img, &tmp);
                fif_image_free(&tmp);
                too_high = measure > o.target_psnr;
            }
            if (too_high) {
                if (q < lowest) { lowest = q; lowres = std::move(r); }
                hi = q - 1;
            } else {
                best = q; bestres = std::move(r);
                lo = q + 1;
            }
        }
        res = std::move(best >= 0 ? bestres : lowres);
    } else {
        fif_status st = encode_once(img, &o, settle(p, o, o.quality), meta, res);
        if (st != FIF_OK) return st;
    }

    *out_size = res.bytes.size();
    *out_data = (uint8_t *)std::malloc(res.bytes.size());
    if (!*out_data) return FIF_ERR_NOMEM;
    std::memcpy(*out_data, res.bytes.data(), res.bytes.size());

    if (stats) {
        std::memset(stats, 0, sizeof(*stats));
        stats->encode_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        stats->search_seconds = res.search_seconds;
        stats->blocks_total = res.ctr.blocks_total;
        stats->blocks_flat = res.ctr.blocks_flat;
        stats->blocks_fractal = res.ctr.blocks_fractal;
        stats->candidate_evals = res.ctr.candidate_evals;
        stats->compressed_bytes = (int64_t)res.bytes.size();
        stats->bits_per_pixel =
            8.0 * res.bytes.size() / ((double)img->width * img->height);
        stats->bits_split    = res.sym.split;
        stats->bits_mode     = res.sym.mode;
        stats->bits_dc       = res.sym.dc;
        stats->bits_domain   = res.sym.domain;
        stats->bits_sym      = res.sym.sym;
        stats->bits_contrast = res.sym.contrast;
        stats->bits_planar   = res.sym.planar;
        stats->bits_grad     = res.sym.grad;
        stats->collage_psnr = (res.ctr.collage_sse > 0 && res.ctr.coded_pixels > 0)
            ? 10.0 * std::log10(255.0 * 255.0 * (double)res.ctr.coded_pixels / res.ctr.collage_sse)
            : 99.0;
    }
    return FIF_OK;
}

} /* extern "C" */
