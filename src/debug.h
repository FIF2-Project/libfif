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
#include "codec.h"
#include <mutex>
#include <string>
#include <vector>

namespace fif {

/* Instrumentation for the decode visualiser.
 *
 * The decoder throws away everything a debugger would want to see: the leaf
 * tree is stack-local scratch freed when a tile finishes, and the per-tile
 * planes never exist at the same time as each other.  A DebugSink is an
 * optional observer threaded through the decode; when the pointer is null the
 * cost is one predictable branch per tile-iteration, never per pixel.
 *
 * On the frame model.  Tiles are independent contractions and iterate out of
 * sync, so there is no instant at which the whole image is "at iteration 3".
 * Frame k here is a composite: every tile at its own iteration k, or at its
 * converged state if it finished earlier.  That is the only definition that
 * yields a whole picture, and the viewer says so rather than implying a global
 * clock the decoder does not have. */

/* One coded leaf.  Every coordinate is in the decoded image's luma pixels,
 * already scaled, chroma leaves included: a 4:2:0 chroma leaf of n samples
 * covers 2n image pixels, and that is the size reported. */
struct DebugLeaf {
    enum : int32_t { FLAT = 1, PLANAR = 2, FROM_LUMA = 4 };
    int32_t tile;
    int32_t plane;           /* 0 Y, 1 Cb, 2 Cr */
    int32_t x, y, size;
    int32_t domain;          /* grid index; -1 when none */
    int32_t sym;
    int32_t flags;           /* FLAT | PLANAR | FROM_LUMA */
    float   dc, s;           /* dequantised mean and contrast */
    float   gx, gy;          /* planar rise across the block, in levels */
    /* The square the leaf reads: twice its size and decimated, or for a
     * FROM_LUMA leaf the co-located luma block.  dext 0 when there is none. */
    int32_t dx, dy, dext;
};

struct DebugTile {
    int32_t id = 0;
    int32_t x = 0, y = 0, w = 0, h = 0;   /* absolute, already scaled */
    int32_t iters = 0;
    float   ms = 0;                       /* wall time of this tile's decode */
    int64_t base_off = 0, base_len = 0;
    int64_t det_off = 0, det_len = 0;
    std::vector<float> dmax;              /* per iteration, in levels */
    std::vector<DebugLeaf> leaves;        /* in decode order, luma first */
};

class DebugSink {
public:
    /* nframes counts frame 0 (the DC seed) plus one per iteration.  `step`
     * decimates the stored frames by an integer factor: every frame has to be
     * held at once, because tiles finish out of sync, so at full 4K a 17-frame
     * ladder is some 450 MB.  The viewer only ever displays them. */
    void init(int nframes, int w, int h, int channels, int step);
    int  step() const { return step_; }

    /* The leaf trees of one tile, every plane, captured before they are
     * destroyed.
     *
     * `ox, oy` are the tile origin in the output image, and must be passed
     * separately: the decoder iterates a copy of the tile whose own x and y
     * have been zeroed, so the Tile handed in here does not know where it
     * lives. */
    void on_leaves(int tile_id, const Tile &tile, int ox, int oy, int nplanes,
                   const CodecParams &p, const Scale &sc);

    /* One tile at one iteration. `planes` are tile-local. */
    void on_iteration(int frame_ix, const Plane *planes,
                      int nplanes, int cs, int tx, int ty);

    /* Called once the tile has stopped moving, to carry its final state
     * forward into every later frame. */
    void on_tile_done(int used, const Plane *planes,
                      int nplanes, int cs, int tx, int ty);

    void set_tile_geometry(int tile_id, int x, int y, int w, int h);
    void set_delta(int tile_id, int iter, float dmax);
    void set_time(int tile_id, float ms);

    int  frames() const { return (int)frames_.size(); }
    int  width()  const { return w_; }
    int  height() const { return h_; }
    int  channels() const { return ch_; }
    const std::vector<uint8_t> &frame(int i) const { return frames_[i]; }
    /* Indexed by tile id.  Each tile's leaves sit with it, so the order they
     * are reported in does not depend on which thread finished first. */
    std::vector<DebugTile>  tiles;
    bool want_leaves = true;

    /* Region streaming. Empty means every tile, which is the normal case.
     * Otherwise a skipped tile is simply never decoded and keeps the mid-grey
     * the output planes start at - the same thing a reader sees when a tile's
     * bytes never arrived, rather than a greyed-out imitation of it. */
    std::vector<uint8_t> tile_mask;

    /* Cap on the long edge of a stored frame; 0 keeps full resolution.  The
     * decode picks the integer divisor from this before allocating. */
    int preview_max = 0;
    bool tile_enabled(int id) const {
        return tile_mask.empty() || (id < (int)tile_mask.size() && tile_mask[id]);
    }

private:
    void blit(int frame_ix, const Plane *planes, int nplanes, int cs,
              int tx, int ty);
    DebugTile &tile_at(int tile_id);        /* with mu_ held */
    std::mutex mu_;
    std::vector<std::vector<uint8_t>> frames_;
    int w_ = 0, h_ = 0, ch_ = 3, step_ = 1;
};

/* Largest pixel movement between two plane sets, in levels.  Deliberately not
 * `converged()`: that one stops at the first sample past the threshold, which
 * is what makes it cheap and also what makes it useless for a graph. */
float plane_delta(const Plane *a, const Plane *b, int nplanes);

/* Internal decode entry that drives a sink.  Not in the public C API. */
fif_status decode_with_debug(const uint8_t *data, size_t size,
                             const fif_decode_options *opts,
                             fif_image *out_img, fif_stats *stats,
                             DebugSink *dbg);

} /* namespace fif */
