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
#include "fif/fif.h"
#include <mutex>

namespace fif {

/* Serialises the caller's progress callback out of the worker threads, keeps
 * the fraction monotone, and throttles it.  Workers finish tiles in whatever
 * order they please, so without this a caller would see the bar jump backwards;
 * without the throttle, an image of many small tiles would spend real time in
 * someone else's terminal code. */
class Progress {
public:
    Progress() = default;
    Progress(fif_progress_fn fn, void *user) : fn_(fn), user_(user) {}

    /* Maps everything reported from here on into [base, base+span] of the
     * whole job, for callers that run several stages back to back. */
    void stage(double base, double span) { base_ = base; span_ = span; }

    void report(double f) {
        if (!fn_) return;
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        const double g = base_ + span_ * f;
        std::lock_guard<std::mutex> lock(m_);
        if (g < last_ + kStep && g < 1.0) return;
        last_ = g;
        fn_(user_, g);
    }

    /* done/total, the form every call site actually has. */
    void step(size_t done, size_t total) {
        if (total) report((double)done / (double)total);
    }

private:
    static constexpr double kStep = 0.005;
    fif_progress_fn fn_ = nullptr;
    void *user_ = nullptr;
    double base_ = 0, span_ = 1;
    double last_ = -1;
    std::mutex m_;
};

} /* namespace fif */
