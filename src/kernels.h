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
#include <cstddef>

/* ------------------------------------------------------------------ ISA
 *
 * One kernel set is chosen at compile time from what the target supports.
 * Both kernels here are encoder-only: `dot` drives the candidate search and
 * `dist2` the kd-tree leaf scan.  Nothing on the decode path calls either, so
 * the width only moves which domain the search settles on - by float
 * reassociation - and two widths may emit different, equally valid streams.
 * That is why the accumulator counts below are tuned per width rather than
 * forced to agree.
 *
 * Do not read that as "the build flags cannot affect decoding".  They can, just
 * not through here: -march also lets the compiler vectorise the decoder's own
 * float loops differently, which is worth about one level on a handful of
 * pixels.
 *
 * Note on FMA: -mavx2 alone does not imply FMA on gcc or clang, so the fused
 * path keys off __FMA__.  MSVC has no such macro and folds FMA into
 * /arch:AVX2, hence the second arm - which has to exclude clang, because
 * clang-cl and clang targeting *-windows-msvc define _MSC_VER and _M_X64 too
 * while still honouring -mno-fma / -mno-sse2.  For any GCC-like compiler the
 * __SSE2__ / __AVX__ / __FMA__ macros are exact, so they are asked first. */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#  include <arm_neon.h>
#  define FIF_NEON 1
#else
#  define FIF_NEON 0
#endif

#define FIF_SSE2 0
#define FIF_AVX  0
#define FIF_FMA  0

/* True only for a compiler that does not advertise the ISA through the
 * GCC-style feature macros. */
#if defined(_MSC_VER) && !defined(__clang__)
#  define FIF_MSVC_ONLY 1
#else
#  define FIF_MSVC_ONLY 0
#endif

#if !FIF_NEON
#  if defined(__AVX512F__) || defined(__AVX2__) || defined(__AVX__) || \
      defined(__SSE2__) || \
      (FIF_MSVC_ONLY && (defined(_M_X64) || \
                         (defined(_M_IX86_FP) && _M_IX86_FP >= 2)))
#    include <immintrin.h>
#    undef  FIF_SSE2
#    define FIF_SSE2 1
#  endif
#  if defined(__AVX__)
#    undef  FIF_AVX
#    define FIF_AVX 1
#  endif
#  if defined(__FMA__) || (FIF_MSVC_ONLY && defined(__AVX2__))
#    undef  FIF_FMA
#    define FIF_FMA 1
#  endif
#endif

/* AVX2 adds no float op these kernels use, so it selects the same 256-bit path
 * as AVX; it is tracked only so the reported name is truthful. */
#if !FIF_NEON && defined(__AVX2__)
#  define FIF_AVX2 1
#else
#  define FIF_AVX2 0
#endif

/* The 256-bit reduction finishes in a 128-bit one, so the AVX kernels need the
 * SSE2 helpers.  Every real target satisfies this - the SSE2 gate above tests
 * __AVX__ explicitly - but say so rather than leave it to be rediscovered from
 * an "undeclared hsum128" further down. */
#if FIF_AVX && !FIF_SSE2
#  error "FIF_AVX without FIF_SSE2: the 256-bit kernels reuse hsum128/fma128"
#endif

namespace fif {

/* Which kernel set this binary was built with; reported by `fif version`. */
inline const char *simd_name() {
#if FIF_NEON
    return "neon";
#elif FIF_AVX2 && FIF_FMA
    return "avx2+fma";
#elif FIF_AVX2
    return "avx2";
#elif FIF_AVX && FIF_FMA
    return "avx+fma";
#elif FIF_AVX
    return "avx";
#elif FIF_SSE2 && FIF_FMA
    return "sse2+fma";
#elif FIF_SSE2
    return "sse2";
#else
    return "scalar";
#endif
}

#if FIF_SSE2
/* Horizontal sum of a 128-bit vector: shuffle-add twice.  No hadd - it is a
 * two-uop instruction on every Intel core and no faster than this. */
static inline float hsum128(__m128 v) {
    __m128 sh = _mm_movehl_ps(v, v);
    v  = _mm_add_ps(v, sh);
    sh = _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1));
    v  = _mm_add_ss(v, sh);
    return _mm_cvtss_f32(v);
}
static inline __m128 fma128(__m128 a, __m128 b, __m128 c) {
#if FIF_FMA
    return _mm_fmadd_ps(a, b, c);
#else
    return _mm_add_ps(c, _mm_mul_ps(a, b));
#endif
}
#endif

#if FIF_AVX
static inline float hsum256(__m256 v) {
    return hsum128(_mm_add_ps(_mm256_castps256_ps128(v),
                              _mm256_extractf128_ps(v, 1)));
}
static inline __m256 fma256(__m256 a, __m256 b, __m256 c) {
#if FIF_FMA
    return _mm256_fmadd_ps(a, b, c);
#else
    return _mm256_add_ps(c, _mm256_mul_ps(a, b));
#endif
}
#endif

/* Inner product of two centred blocks. This is the hot loop of the encoder:
 * every (range, domain, symmetry) triple costs exactly one call.  Block
 * lengths are n*n for n in 4..32, so every call but the 4x4 feature key is a
 * multiple of 32 and stays in the widest loop. */
inline float dot(const float *a, const float *b, size_t n) {
#if FIF_NEON
    float32x4_t s0 = vdupq_n_f32(0), s1 = vdupq_n_f32(0);
    float32x4_t s2 = vdupq_n_f32(0), s3 = vdupq_n_f32(0);
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        s0 = vfmaq_f32(s0, vld1q_f32(a + i),      vld1q_f32(b + i));
        s1 = vfmaq_f32(s1, vld1q_f32(a + i + 4),  vld1q_f32(b + i + 4));
        s2 = vfmaq_f32(s2, vld1q_f32(a + i + 8),  vld1q_f32(b + i + 8));
        s3 = vfmaq_f32(s3, vld1q_f32(a + i + 12), vld1q_f32(b + i + 12));
    }
    for (; i + 4 <= n; i += 4)
        s0 = vfmaq_f32(s0, vld1q_f32(a + i), vld1q_f32(b + i));
    float acc = vaddvq_f32(vaddq_f32(vaddq_f32(s0, s1), vaddq_f32(s2, s3)));
    for (; i < n; ++i) acc += a[i] * b[i];
    return acc;
#elif FIF_AVX
    /* Four accumulators: an FMA retires in 4-5 cycles at two per cycle, so
     * two chains would leave half the FMA issue width idle. */
    __m256 s0 = _mm256_setzero_ps(), s1 = _mm256_setzero_ps();
    __m256 s2 = _mm256_setzero_ps(), s3 = _mm256_setzero_ps();
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        s0 = fma256(_mm256_loadu_ps(a + i),      _mm256_loadu_ps(b + i),      s0);
        s1 = fma256(_mm256_loadu_ps(a + i + 8),  _mm256_loadu_ps(b + i + 8),  s1);
        s2 = fma256(_mm256_loadu_ps(a + i + 16), _mm256_loadu_ps(b + i + 16), s2);
        s3 = fma256(_mm256_loadu_ps(a + i + 24), _mm256_loadu_ps(b + i + 24), s3);
    }
    for (; i + 8 <= n; i += 8)
        s0 = fma256(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), s0);
    float acc = hsum256(_mm256_add_ps(_mm256_add_ps(s0, s1),
                                      _mm256_add_ps(s2, s3)));
    for (; i < n; ++i) acc += a[i] * b[i];
    return acc;
#elif FIF_SSE2
    __m128 s0 = _mm_setzero_ps(), s1 = _mm_setzero_ps();
    __m128 s2 = _mm_setzero_ps(), s3 = _mm_setzero_ps();
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        s0 = fma128(_mm_loadu_ps(a + i),      _mm_loadu_ps(b + i),      s0);
        s1 = fma128(_mm_loadu_ps(a + i + 4),  _mm_loadu_ps(b + i + 4),  s1);
        s2 = fma128(_mm_loadu_ps(a + i + 8),  _mm_loadu_ps(b + i + 8),  s2);
        s3 = fma128(_mm_loadu_ps(a + i + 12), _mm_loadu_ps(b + i + 12), s3);
    }
    for (; i + 4 <= n; i += 4)
        s0 = fma128(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i), s0);
    float acc = hsum128(_mm_add_ps(_mm_add_ps(s0, s1), _mm_add_ps(s2, s3)));
    for (; i < n; ++i) acc += a[i] * b[i];
    return acc;
#else
    float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        a0 += a[i] * b[i];         a1 += a[i+1] * b[i+1];
        a2 += a[i+2] * b[i+2];     a3 += a[i+3] * b[i+3];
    }
    float acc = a0 + a1 + a2 + a3;
    for (; i < n; ++i) acc += a[i] * b[i];
    return acc;
#endif
}

/* Squared euclidean distance, used by the kd-tree.  Always called with
 * n == FEAT_DIM (8), so the 8-wide block is the whole job on AVX and the
 * 4-wide pair is the whole job on SSE2; the wider loops are there for
 * correctness, not because they run. */
inline float dist2(const float *a, const float *b, size_t n) {
#if FIF_NEON
    float32x4_t s0 = vdupq_n_f32(0), s1 = vdupq_n_f32(0);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        float32x4_t d0 = vsubq_f32(vld1q_f32(a + i),     vld1q_f32(b + i));
        float32x4_t d1 = vsubq_f32(vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
        s0 = vfmaq_f32(s0, d0, d0);
        s1 = vfmaq_f32(s1, d1, d1);
    }
    float acc = vaddvq_f32(vaddq_f32(s0, s1));
    for (; i < n; ++i) { float d = a[i] - b[i]; acc += d * d; }
    return acc;
#elif FIF_AVX
    __m256 s0 = _mm256_setzero_ps(), s1 = _mm256_setzero_ps();
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(a + i),     _mm256_loadu_ps(b + i));
        __m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8));
        s0 = fma256(d0, d0, s0);
        s1 = fma256(d1, d1, s1);
    }
    for (; i + 8 <= n; i += 8) {
        __m256 d = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
        s0 = fma256(d, d, s0);
    }
    float acc = hsum256(_mm256_add_ps(s0, s1));
    for (; i < n; ++i) { float d = a[i] - b[i]; acc += d * d; }
    return acc;
#elif FIF_SSE2
    __m128 s0 = _mm_setzero_ps(), s1 = _mm_setzero_ps();
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m128 d0 = _mm_sub_ps(_mm_loadu_ps(a + i),     _mm_loadu_ps(b + i));
        __m128 d1 = _mm_sub_ps(_mm_loadu_ps(a + i + 4), _mm_loadu_ps(b + i + 4));
        s0 = fma128(d0, d0, s0);
        s1 = fma128(d1, d1, s1);
    }
    for (; i + 4 <= n; i += 4) {
        __m128 d = _mm_sub_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i));
        s0 = fma128(d, d, s0);
    }
    float acc = hsum128(_mm_add_ps(s0, s1));
    for (; i < n; ++i) { float d = a[i] - b[i]; acc += d * d; }
    return acc;
#else
    float acc = 0;
    for (size_t i = 0; i < n; ++i) { float d = a[i] - b[i]; acc += d * d; }
    return acc;
#endif
}

} /* namespace fif */
