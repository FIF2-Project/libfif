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

#include "context.h"
#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

namespace fif {
namespace {

std::mutex g_mutex;
std::vector<uint64_t> g_sum;
uint64_t g_count = 0;
/* Read once per tile from every encoder thread, written from the caller's. */
std::atomic<bool> g_active{false};

#include "ctxtable.inc"

/* The table is generated from a build of this same struct.  Adding a context
 * changes the struct's size and silently turns the copy below into an
 * out-of-bounds read, so the two sizes are tied together here rather than left
 * to whoever remembers to re-run the trainer. */
static_assert(sizeof(kDefaultProbs) == sizeof(Contexts),
              "src/ctxtable.inc is stale - run tools/train_contexts.sh");

} /* namespace */

const uint16_t *contexts_default_probs() { return kDefaultProbs; }

void Contexts::reset() {
    std::memcpy(this, contexts_default_probs(), sizeof(*this));
}

void contexts_train_begin() {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_sum.assign(contexts_prob_count(), 0);
    g_count = 0;
    g_active = true;
}

bool contexts_train_active() { return g_active; }

void contexts_train_add(const Contexts &c) {
    if (!g_active) return;
    const uint16_t *p = (const uint16_t *)&c;
    std::lock_guard<std::mutex> lk(g_mutex);
    for (size_t i = 0; i < g_sum.size(); ++i) g_sum[i] += p[i];
    ++g_count;
}

bool contexts_train_write(const char *path) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_count) return false;
    FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    /* Plain text so the table can be diffed and hand-inspected. */
    std::fprintf(f, "%zu %llu\n", g_sum.size(), (unsigned long long)g_count);
    for (size_t i = 0; i < g_sum.size(); ++i)
        std::fprintf(f, "%llu\n", (unsigned long long)g_sum[i]);
    std::fclose(f);
    return true;
}

} /* namespace fif */
