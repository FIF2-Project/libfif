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

namespace fif {

/* The leaf syntax is written once and walked in both directions.
 *
 * It used to exist twice, as an encoder function and a hand-mirrored decoder
 * function that had to agree symbol for symbol.  Three separate desyncs came
 * out of that: a guard added on one side and not the other, a decision keyed on
 * a value the decoder did not have yet, and a context that differed between a
 * full and a truncated stream.  Each was invisible to a round-trip test and
 * each cost ten dB.  With one description there is no second copy to drift.
 *
 * Values are passed by reference: the encoding side reads them out, the
 * decoding side fills them in.  Anything that is a *decision* rather than a
 * symbol - which quantisation level to send - stays guarded by `writing()`,
 * because only the encoder gets to decide and both sides then code the same
 * answer. */

enum Layer { LAYER_BASE = 0, LAYER_DETAIL = 1 };

using StatBucket = double SymbolStats::*;

class EncodeIO {
public:
    EncodeIO(RangeEncoder &base, RangeEncoder &detail, SymbolStats *st)
        : base_(base), detail_(detail), st_(st) {}

    static constexpr bool writing() { return true; }
    bool detail_present() const { return true; }
    SymbolStats *stats() const { return st_; }

    void bin(Layer l, uint16_t &p, int32_t &v, StatBucket b) {
        enc_bit(pick(l), p, (int)v, cost(b));
    }
    void tree(Layer l, uint16_t *probs, int nbits, int32_t &v, StatBucket b) {
        enc_tree(pick(l), probs, nbits, (uint32_t)v, cost(b));
    }
    void uniform(Layer l, int32_t &v, int nbits, StatBucket b) {
        enc_direct(pick(l), (uint32_t)v, nbits, cost(b));
    }
    void residual(Layer l, uint16_t &zero, uint16_t &sign, uint16_t *len,
                  int maxlen, int32_t &v, StatBucket b) {
        enc_signed(pick(l), zero, sign, len, maxlen, (int)v, cost(b));
    }

private:
    RangeEncoder &pick(Layer l) { return l == LAYER_BASE ? base_ : detail_; }
    double *cost(StatBucket b) { return st_ ? &(st_->*b) : nullptr; }
    RangeEncoder &base_, &detail_;
    SymbolStats *st_;
};

class DecodeIO {
public:
    DecodeIO(RangeDecoder &base, RangeDecoder *detail)
        : base_(base), detail_(detail) {}

    static constexpr bool writing() { return false; }
    bool detail_present() const { return detail_ != nullptr; }
    SymbolStats *stats() const { return nullptr; }

    void bin(Layer l, uint16_t &p, int32_t &v, StatBucket) {
        v = pick(l).bit(p);
    }
    void tree(Layer l, uint16_t *probs, int nbits, int32_t &v, StatBucket) {
        v = (int32_t)bittree_decode(pick(l), probs, nbits);
    }
    void uniform(Layer l, int32_t &v, int nbits, StatBucket) {
        v = (int32_t)pick(l).direct(nbits);
    }
    void residual(Layer l, uint16_t &zero, uint16_t &sign, uint16_t *len,
                  int maxlen, int32_t &v, StatBucket) {
        v = dec_signed(pick(l), zero, sign, len, maxlen);
    }

private:
    RangeDecoder &pick(Layer l) { return l == LAYER_BASE ? base_ : *detail_; }
    RangeDecoder &base_;
    RangeDecoder *detail_;
};

} /* namespace fif */
