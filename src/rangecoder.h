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

#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace fif {

/* Adaptive binary range coder, LZMA's variant: 11-bit probabilities, one
 * multiply per bin, five-bit adaptation rate.
 *
 * Chosen over rANS because almost every symbol here is a context-dependent
 * binary decision.  An adaptive coder learns the statistics of the image it is
 * compressing as it goes, so nothing has to be measured in a first pass or
 * shipped in the file; rANS would need a frequency table per context, and the
 * tables would cost more than the contexts save. */

static constexpr int   kProbBits  = 11;
static constexpr uint16_t kProbOne = 1 << kProbBits;      /* 2048 */
static constexpr int   kMoveBits  = 5;
static constexpr uint32_t kTopValue = 1u << 24;

class RangeEncoder {
public:
    void bit(uint16_t &p, int b) {
        const uint32_t bound = (range_ >> kProbBits) * p;
        if (!b) {
            range_ = bound;
            p = (uint16_t)(p + ((kProbOne - p) >> kMoveBits));
        } else {
            low_ += bound;
            range_ -= bound;
            p = (uint16_t)(p - (p >> kMoveBits));
        }
        while (range_ < kTopValue) { range_ <<= 8; shift_low(); }
    }

    /* Uniformly distributed bits, no model, no adaptation. */
    void direct(uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) {
            range_ >>= 1;
            if ((v >> i) & 1) low_ += range_;
            while (range_ < kTopValue) { range_ <<= 8; shift_low(); }
        }
    }

    void flush() { for (int i = 0; i < 5; ++i) shift_low(); }

    const std::vector<uint8_t> &bytes() const { return out_; }
    std::vector<uint8_t> &bytes() { return out_; }

private:
    void shift_low() {
        if ((uint32_t)(low_ >> 32) != 0 || (uint32_t)low_ < 0xFF000000u) {
            const uint8_t carry = (uint8_t)(low_ >> 32);
            do {
                out_.push_back((uint8_t)(cache_ + carry));
                cache_ = 0xFF;
            } while (--cache_size_);
            cache_ = (uint8_t)((uint32_t)low_ >> 24);
        }
        ++cache_size_;
        low_ = (uint32_t)low_ << 8;
    }

    std::vector<uint8_t> out_;
    uint64_t low_ = 0;
    uint32_t range_ = 0xFFFFFFFFu;
    uint8_t  cache_ = 0;
    int64_t  cache_size_ = 1;
};

class RangeDecoder {
public:
    RangeDecoder(const uint8_t *data, size_t size) : p_(data), end_(data + size) {
        next();                                   /* the encoder's lead byte */
        for (int i = 0; i < 4; ++i) code_ = (code_ << 8) | next();
    }

    int bit(uint16_t &p) {
        const uint32_t bound = (range_ >> kProbBits) * p;
        int b;
        if (code_ < bound) {
            range_ = bound;
            p = (uint16_t)(p + ((kProbOne - p) >> kMoveBits));
            b = 0;
        } else {
            code_ -= bound;
            range_ -= bound;
            p = (uint16_t)(p - (p >> kMoveBits));
            b = 1;
        }
        while (range_ < kTopValue) { range_ <<= 8; code_ = (code_ << 8) | next(); }
        return b;
    }

    uint32_t direct(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            range_ >>= 1;
            code_ -= range_;
            const uint32_t t = 0u - (code_ >> 31);   /* all ones if it borrowed */
            code_ += range_ & t;
            v = (v << 1) + (t + 1);
            while (range_ < kTopValue) { range_ <<= 8; code_ = (code_ << 8) | next(); }
        }
        return v;
    }

    bool overrun() const { return overrun_; }

private:
    uint8_t next() {
        if (p_ >= end_) { overrun_ = true; return 0; }
        return *p_++;
    }
    const uint8_t *p_, *end_;
    uint32_t range_ = 0xFFFFFFFFu;
    uint32_t code_ = 0;
    bool overrun_ = false;
};

/* MSB-first adaptive tree over 2^nbits symbols; probs has 1 << nbits entries.
 * The encoding direction lives in context.h as enc_tree, which also prices the
 * bits it writes. */
inline uint32_t bittree_decode(RangeDecoder &d, uint16_t *probs, int nbits) {
    uint32_t m = 1;
    for (int i = 0; i < nbits; ++i) m = (m << 1) | (uint32_t)d.bit(probs[m]);
    return m - (1u << nbits);
}

} /* namespace fif */
