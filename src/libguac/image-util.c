/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "config.h"

#include "image-util.h"

#include <string.h>

#ifdef HAVE_VECTOR_EXTENSIONS
/**
 * 16-byte vector of unsigned chars used for per-row byte shuffles.
 * Matches the SSE2 xmm width and the NEON quadword width, so one vector
 * holds 4 BGRx pixels (16 bytes) or 4+ RGB pixels' worth of output.
 */
typedef unsigned char guac_image_v16u8 __attribute__((vector_size(16)));

/**
 * PSHUFB/TBL control mask that reads 16 bytes of BGRx input (4 pixels)
 * and produces the 12 RGB bytes in the first 12 output positions. The
 * last 4 output lanes are don't-cares - callers commit only the first
 * 12 bytes out via a truncated memcpy, so whatever GCC fills those
 * lanes with is discarded without needing to be zeroed.
 */
static const guac_image_v16u8 guac_image_bgrx_to_rgb_shuffle = {
    2,  1,  0,    /* pixel 0: R G B  (from input B G R) */
    6,  5,  4,    /* pixel 1                            */
    10, 9,  8,    /* pixel 2                            */
    14, 13, 12,   /* pixel 3                            */
    0,  0,  0,  0 /* unused padding                     */
};
#endif

/**
 * Scalar fallback path for guac_image_bgrx_to_rgb_row. Per-pixel byte
 * gather from Cairo BGRx to packed RGB. Used when no shuffle-capable
 * SIMD is available at runtime (strict SSE2 x86 CPU running a baseline
 * binary, or a build without vector-extension support).
 */
static void guac_image_bgrx_to_rgb_row_scalar(unsigned char* restrict dst,
        const unsigned char* restrict src, int width) {
    for (int i = 0; i < width; i++) {
        dst[i * 3 + 0] = src[i * 4 + 2]; /* R */
        dst[i * 3 + 1] = src[i * 4 + 1]; /* G */
        dst[i * 3 + 2] = src[i * 4 + 0]; /* B */
    }
}

#ifdef HAVE_VECTOR_EXTENSIONS
/**
 * Shuffle-based fast path for guac_image_bgrx_to_rgb_row. Loads 16
 * bytes of BGRx (4 pixels), permutes to packed RGB order via
 * __builtin_shuffle, and writes the first 12 bytes of the result.
 * Carries a target("ssse3") attribute on x86 so the compiler emits
 * PSHUFB even when the translation unit itself was compiled for the
 * baseline x86-64 target - the shuffle-availability check in
 * guac_image_bgrx_to_rgb_row below gates invocation to CPUs that
 * actually support the instruction. On AArch64 NEON the shuffle
 * lowers to TBL with no target attribute needed.
 */
#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("ssse3")))
#endif
static void guac_image_bgrx_to_rgb_row_shuffle(unsigned char* restrict dst,
        const unsigned char* restrict src, int width) {

    int i = 0;

    /* 4 pixels per iteration via byte shuffle. Each iteration reads 16
     * bytes (4 BGRx pixels) and writes exactly 12 bytes (4 RGB
     * pixels); the remaining 4 lanes of the shuffled vector are
     * ignored via a truncated memcpy. */
    for (; i + 4 <= width; i += 4) {
        guac_image_v16u8 v;
        memcpy(&v, src + i * 4, sizeof(v));
        v = __builtin_shuffle(v, guac_image_bgrx_to_rgb_shuffle);
        memcpy(dst + i * 3, &v, 12);
    }

    /* Scalar tail handles the 0-3 leftover pixels when width isn't a
     * multiple of 4. */
    for (; i < width; i++) {
        dst[i * 3 + 0] = src[i * 4 + 2]; /* R */
        dst[i * 3 + 1] = src[i * 4 + 1]; /* G */
        dst[i * 3 + 2] = src[i * 4 + 0]; /* B */
    }

}
#endif

/* Forced tree-loop-vectorize on this single helper so the loop auto-
 * vectorizes even at -O2 on GCC <12 (which otherwise leaves loop
 * vectorization off by default at that level). The loop is a pure
 * element-wise OR + store that the auto-vectorizer trivially widens
 * to the build target's native vector length - SSE2 gets paired 128-
 * bit ops, AVX2 256-bit, AVX-512 512-bit, NEON/SVE/RVV their widest
 * native width. */
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("tree-loop-vectorize")))
#endif
void guac_image_force_alpha_row(uint32_t* restrict dst,
        const uint32_t* restrict src, int width) {
    for (int x = 0; x < width; x++)
        dst[x] = src[x] | 0xFF000000u;
}

void guac_image_bgrx_to_rgb_row(unsigned char* restrict dst,
        const unsigned char* restrict src, int width) {

    /* Runtime dispatch. The branch is predictable per call site (the
     * condition is driven by a CPU capability that's invariant over
     * the process's lifetime), so the per-row check is effectively
     * free. The shuffle path is lifted out under a target("ssse3")
     * function so PSHUFB is emitted regardless of the translation
     * unit's compile target; the hybrid path is called when the CPU
     * doesn't support PSHUFB and reduces to the scalar byte-gather
     * loop (which avoids the expensive scalarized-__builtin_shuffle
     * codegen that GCC would otherwise produce on SSE2 x86). */
#ifdef HAVE_VECTOR_EXTENSIONS
  #if defined(__x86_64__) || defined(__i386__)
    if (__builtin_cpu_supports("ssse3")) {
        guac_image_bgrx_to_rgb_row_shuffle(dst, src, width);
        return;
    }
  #elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    guac_image_bgrx_to_rgb_row_shuffle(dst, src, width);
    return;
  #endif
#endif

    guac_image_bgrx_to_rgb_row_scalar(dst, src, width);

}
