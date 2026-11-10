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

#include "fif_internal.h"
#include <array>
#include <vector>

namespace fif {
namespace {

struct SymTables {
    /* [log2(n)][g], log2(n) in 1..kMaxLg.  128 is the largest side an int16
     * offset can address (128*128 = 16384) and covers both the largest range
     * block the format admits and the default 32-pixel leaf at 4x zoom. */
    static constexpr int kMaxLg = 7;
    std::array<std::array<std::vector<int16_t>, 8>, kMaxLg + 1> t;
    SymTables() {
        for (int lg = 1; lg <= kMaxLg; ++lg) {
            const int n = 1 << lg;
            for (int g = 0; g < 8; ++g) {
                auto &v = t[lg][g];
                v.resize((size_t)n * n);
                for (int y = 0; y < n; ++y)
                    for (int x = 0; x < n; ++x) {
                        int sx, sy;
                        sym_map(g, x, y, n, &sx, &sy);
                        v[(size_t)y * n + x] = (int16_t)(sy * n + sx);
                    }
            }
        }
    }
};

const SymTables &tables() {
    static const SymTables s;      /* thread-safe initialisation */
    return s;
}

} /* namespace */

const int16_t *sym_indices(int g, int n) {
    const int lg = n > 0 ? ilog2_ceil((uint32_t)n) : 0;
    if (lg < 1 || lg > SymTables::kMaxLg || (1 << lg) != n) return nullptr;
    return tables().t[lg][g].data();
}

} /* namespace fif */
