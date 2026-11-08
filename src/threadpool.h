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
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace fif {

/* 0 when the system does not report a count. */
inline int cpu_cores() { return (int)std::thread::hardware_concurrency(); }

inline int thread_count(int n) {
    if (n <= 0) {
        const int cores = cpu_cores();
        n = cores <= 2 ? cores : cores * 3 / 4;
    }
    return n < 1 ? 1 : n;
}

/* Work-stealing is overkill here: every parallel section is a flat range of
 * independent items, so a shared atomic cursor gives the same load balance
 * with none of the bookkeeping. */
class ThreadPool {
public:
    explicit ThreadPool(int n) : n_(thread_count(n)) {}
    int size() const { return n_; }

    template <typename F>
    void parallel_for(size_t count, F &&fn) {
        if (count == 0) return;
        if (n_ == 1 || count == 1) {
            for (size_t i = 0; i < count; ++i) fn(i, 0);
            return;
        }
        std::atomic<size_t> cursor{0};
        std::vector<std::thread> ts;
        int workers = (int)std::min<size_t>(n_, count);
        ts.reserve(workers);
        for (int t = 0; t < workers; ++t) {
            ts.emplace_back([&, t] {
                for (;;) {
                    size_t i = cursor.fetch_add(1, std::memory_order_relaxed);
                    if (i >= count) break;
                    fn(i, t);
                }
            });
        }
        for (auto &th : ts) th.join();
    }

    /* Runs fn(b0, b1) over half-open bands of [0, h), `per_worker` bands per
     * thread and never more than h. */
    template <typename F>
    void bands(int h, int per_worker, F &&fn) {
        const int n = std::max(1, std::min(n_ * per_worker, h));
        parallel_for((size_t)n, [&](size_t k, int) {
            fn((int)((int64_t)h * (int64_t)k / n), (int)((int64_t)h * (int64_t)(k + 1) / n));
        });
    }

private:
    int n_ = 1;
};

} /* namespace fif */
