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

#include "display-plan.h"
#include "display-priv.h"
#include "guacamole/display.h"
#include "guacamole/mem.h"
#include "guacamole/rect.h"

#include <limits.h>
#include <string.h>
#include <stdint.h>

#ifdef HAVE_VECTOR_EXTENSIONS
/**
 * 8-wide vectors of uint32_t / int32_t, used to parallelize the translucent
 * overlay detection's per-channel unblend + range tracking. Matches the
 * AVX2 register width (256 bits = 32 bytes = 8 uint32_t lanes) on x86 when
 * AVX2 is available, and compiles to paired 128-bit SSE2 / NEON ops on
 * older targets and other architectures. The vector_size attribute is a
 * core GCC/Clang extension and requires no header.
 *
 * Extracting RGB channels from packed ARGB pixels uses bitwise shift +
 * mask, which compiles to VPSRLD / VPAND (or the equivalent). Per-channel
 * arithmetic - (255·p - (255-α)·l) - keeps everything in int32 to
 * accommodate the full signed range (max |value| ≈ 65025). Integer min/
 * max uses the GCC-vector ternary `(a < b) ? a : b` which compiles to
 * VPMINSD / VPMAXSD on AVX2.
 */
typedef uint32_t guac_v8u32 __attribute__((vector_size(32)));
typedef int32_t guac_v8i32 __attribute__((vector_size(32)));

/*
 * Per-lane signed min / max of two guac_v8i32 vectors.
 *
 * Implemented as statement-expression macros rather than inline
 * functions for two reasons: (a) C mode does not permit the vector
 * ternary (?:) with a vector condition - only C++ does - and (b)
 * passing a 32-byte vector through a function signature triggers a
 * -Wpsabi warning on builds without AVX enabled globally, even when
 * the function is static inline. Macros sidestep both issues: the
 * comparison mask is a regular vector subexpression, and no call
 * boundary exists for the ABI check to fire on.
 *
 * The bitwise-blend pattern GCC reliably folds to native SIMD min/max
 * instructions (VPMINSD / VPMAXSD on AVX2 and SSE4.1, VMIN / VMAX on
 * NEON) when the active ISA provides one.
 */
#define GUAC_V8I32_MIN(a, b) __extension__ ({                                \
        guac_v8i32 _ga = (a), _gb = (b);                                     \
        guac_v8i32 _gm = _ga < _gb;                                          \
        (guac_v8i32) ((_ga & _gm) | (_gb & ~_gm));                           \
    })

#define GUAC_V8I32_MAX(a, b) __extension__ ({                                \
        guac_v8i32 _ga = (a), _gb = (b);                                     \
        guac_v8i32 _gm = _ga > _gb;                                          \
        (guac_v8i32) ((_ga & _gm) | (_gb & ~_gm));                           \
    })

#if defined(HAVE_ATTRIBUTE_TARGET_CLONES) && (defined(__x86_64__) || defined(__i386__))

/* 8-wide signed-int32 vector matching the layout the AVX2 intrinsic
 * builtins expect (their internal __v8si type). Defining this locally
 * avoids needing <immintrin.h>, whose outer _mm256_* wrappers are only
 * declared when AVX2 is enabled at preprocessor time - while the
 * underlying __builtin_ia32_* intrinsics are part of the compiler's
 * builtin table and are usable in any function whose target attribute
 * permits them. */
typedef int guac_v8si __attribute__((vector_size(32)));

/*
 * AVX2-only single-instruction min / max helpers. Both GCC and Clang
 * accept __attribute__((target("avx2"))) to enable AVX2 codegen for
 * the helper's body regardless of the file's command-line target, and
 * both accept the __builtin_ia32_pminsd256 / __builtin_ia32_pmaxsd256
 * builtins these wrap - so no compiler-specific pragmas or vendor
 * headers are needed.
 *
 * The pointer-based signature is deliberate. Returning or accepting a
 * 32-byte vector by value would require AVX in the caller's ABI,
 * which the default target_clones variant of the dispatching function
 * doesn't have - that mismatch would otherwise raise -Wpsabi at the
 * call site (a warning we'd then have to silence with a compiler-
 * specific diagnostic pragma). Passing pointers keeps the call
 * boundary in scalar territory; the ymm register traffic happens
 * entirely inside the AVX2-targeted body, after the dereferences.
 */
__attribute__((target("avx2")))
static inline void guac_v8i32_min_avx2(guac_v8i32* dst, const guac_v8i32* src) {
    *dst = (guac_v8i32) __builtin_ia32_pminsd256((guac_v8si) *dst, (guac_v8si) *src);
}

__attribute__((target("avx2")))
static inline void guac_v8i32_max_avx2(guac_v8i32* dst, const guac_v8i32* src) {
    *dst = (guac_v8i32) __builtin_ia32_pmaxsd256((guac_v8si) *dst, (guac_v8si) *src);
}

#endif

#endif

/**
 * Rounds the given value down to the nearest power of two.
 *
 * @param value
 *     The value to round.
 *
 * @return
 *     The power of two that is closest to the given value without exceeding
 *     that value.
 */
static size_t guac_display_plan_round_pot(size_t value) {

    if (value <= 2)
        return value;

    size_t rounded = 1;
    while (value >>= 1)
        rounded <<= 1;

    return rounded;

}

/**
 * Returns whether the given buffer consists entirely of the same 32-bit
 * quantity (ie: a single ARGB pixel), repeated throughout the buffer.
 *
 * This function attempts to perform a fast comparison leveraging memcmp() to
 * reduce the search space, rather than simply looping through each pixel one
 * at a time. Basic benchmarks show this approach to be roughly twice as fast
 * as a simple loop for arbitrary buffer lengths and four times as fast for
 * buffer lengths that are powers of two.
 *
 * @param buffer
 *     The buffer to check.
 *
 * @param length
 *     The number of bytes in the buffer.
 *
 * @param color
 *     A pointer to a uint32_t to receive the value of the 32-bit quantity that
 *     is repeated, if applicable.
 *
 * @return
 *     Non-zero if the same 32-bit quantity is repeated throughout the buffer,
 *     zero otherwise. If the same value is indeed repeated throughout the
 *     buffer, that value is stored in the variable pointed to by the "color"
 *     pointer. If the value is not repeated, the variable pointed to by the
 *     "color" pointer is left untouched.
 */
static int guac_display_plan_is_single_color(const unsigned char* restrict buffer,
        size_t length, uint32_t* restrict color) {

    /* It is vacuously true that all the 32-bit quantities in an empty buffer
     * are the same */
    if (length == 0) {
        *color = 0x00000000;
        return 1;
    }

    /* A single 32-bit value is the same as itself */
    if (length == 4) {
        *color = ((const uint32_t*) buffer)[0];
        return 1;
    }

    /* Simply directly compare if there are only two values */
    if (length == 8) {
        uint32_t a = ((const uint32_t*) buffer)[0];
        uint32_t b = ((const uint32_t*) buffer)[1];
        if (a == b) {
            *color = a;
            return 1;
        }
    }

    /* For all other lengths, avoid comparing if finding a match is impossible.
     * A buffer can consist entirely of the same 32-bit (4-byte) quantity
     * repeated throughout the buffer only if that buffer's length is a
     * multiple of 4. */
    if ((length % 4) != 0)
        return 0;

    /* A buffer consists entirely of the same 32-bit quantity repeated
     * throughout if (1) the two halves of the buffer are the same and (2) one
     * of those halves is known to consist entirely of the same 32-bit quantity
     * repeated throughout. */

    size_t pot_length = guac_display_plan_round_pot(guac_mem_ckd_sub_or_die(length, 1));
    size_t remaining_length = guac_mem_ckd_sub_or_die(length, pot_length);

    /* Easiest recursive case: the buffer is already a power of two and can be
     * split into two very easy-to-compare halves */
    if (pot_length == remaining_length) {
        return !memcmp(buffer, buffer + pot_length, pot_length)
            && guac_display_plan_is_single_color(buffer, pot_length, color);
    }

    /* For buffers that can't be split into two power-of-two halves, decide
     * based on one easy power-of-two case and one not-so-easy case of whatever
     * remains */
    uint32_t color_a = 0, color_b = 0;
    if (guac_display_plan_is_single_color(buffer, pot_length, &color_a)
        && guac_display_plan_is_single_color(buffer + pot_length, remaining_length, &color_b)
        && color_a == color_b) {

        *color = color_a;
        return 1;

    }

    return 0;

}

/**
 * Returns whether the given rectangle within given buffer consists entirely of
 * the same 32-bit quantity (ie: a single ARGB pixel), repeated throughout the
 * rectangular region.
 *
 * This function attempts to perform a fast comparison leveraging memcmp() to
 * reduce the search space, rather than simply looping through each pixel one
 * at a time. Basic benchmarks show this approach to be roughly twice as fast
 * as a simple loop for arbitrary buffer lengths and four times as fast for
 * buffer lengths that are powers of two.
 *
 * @param buffer
 *     The buffer to check.
 *
 * @param stride
 *     The number of bytes in each row of image data within the buffer.
 *
 * @param rect
 *     The rectangle representing the region to be checked within the buffer.
 *
 * @param color
 *     A pointer to a uint32_t to receive the value of the 32-bit quantity that
 *     is repeated, if applicable.
 *
 * @return
 *     Non-zero if the same 32-bit quantity is repeated throughout the
 *     rectangular region, zero otherwise. If the same value is indeed repeated
 *     throughout the rectangle, that value is stored in the variable pointed
 *     to by the "color" pointer. If the value is not repeated, the variable
 *     pointed to by the "color" pointer is left untouched.
 */
static int guac_display_plan_is_rect_single_color(const unsigned char* restrict buffer,
        size_t stride, const guac_rect* restrict rect, uint32_t* restrict color) {

    size_t row_length = guac_mem_ckd_mul_or_die(guac_rect_width(rect), GUAC_DISPLAY_LAYER_RAW_BPP);
    buffer = GUAC_RECT_CONST_BUFFER(*rect, buffer, stride, GUAC_DISPLAY_LAYER_RAW_BPP);

    /* Verify that the first row consists of a single color */
    uint32_t first_color = 0x00000000;
    if (!guac_display_plan_is_single_color(buffer, row_length, &first_color))
        return 0;

    /* The whole rectangle consists of a single color if each row is identical
     * and it's already known that one of those rows consists of the a single
     * color */
    const unsigned char* previous = buffer;
    for (int y = rect->top + 1; y < rect->bottom; y++) {

        const unsigned char* current = previous + stride;
        if (memcmp(previous, current, row_length))
            return 0;

        previous = current;

    }

    *color = first_color;
    return 1;

}

/**
 * Minimum last-frame channel delta required between the reference pixel and
 * the second sample pixel before alpha derivation is considered precise
 * enough to be useful. The derived alpha has resolution of approximately
 * 255 / delta_last; a delta of 16 limits the alpha error to about ±16, which
 * is enough to keep the unblended source estimates from drifting wildly
 * across pixels for typical UI alphas (50-75%).
 */
#define GUAC_DISPLAY_PLAN_TRANSLUCENT_MIN_DELTA 16

/**
 * Threshold above which the alpha derivation is considered "good enough" and
 * the search for the best sample pair short-circuits. Larger deltas give
 * slightly better precision, but the gain rapidly tapers off.
 */
#define GUAC_DISPLAY_PLAN_TRANSLUCENT_GOOD_DELTA 64

/**
 * Returns whether the given rectangle within the pending-frame buffer can be
 * expressed as a single uniform translucent rect (one source color and one
 * source alpha) composited over the corresponding region of the last-frame
 * buffer using source-over blending.
 *
 * The detection works in three steps:
 *
 *   1. Find two pixels in the rect with distinct last-frame values (in
 *      some channel) and derive the source alpha from them. From the
 *      source-over identity p[i] = α·src + (255-α)·last[i] (rounded), the
 *      difference across two pixels gives
 *      255 - α = 255·(p_a - p_b) / (l_a - l_b), solvable via integer
 *      division on whichever channel has the largest last-frame delta.
 *
 *   2. Unblend every pixel via
 *      src_est[i] = (255·p[i] - (255-α)·l[i]) / α
 *      and track running min/max per channel.
 *
 *   3. If the per-channel spread stays within an α-scaled tolerance, the
 *      rect IS a uniform translucent overlay: emit the midpoint of the
 *      min/max range (per channel) as the recovered src color, with the
 *      derived alpha in the high byte.
 *
 * Why min/max rather than strict-equality check on a masked scratch buffer:
 * the blend/unblend cycle produces src estimates that differ from the true
 * src by up to ±127.5/α (round-to-nearest blender). For α < 200 this
 * exceeds ±1 routinely, and for α ≤ 64 exceeds ±2. A quantization mask
 * could absorb these, but whenever the true src lands near a mask bucket
 * boundary the noise straddles the boundary and strict equality fails - a
 * structural blind spot that makes the check inconsistent across src
 * values. A simple running min/max with α-scaled tolerance has no such
 * blind spots and rejects genuine non-uniform content via the same range
 * check.
 *
 * @param pending_buffer
 *     Pointer to the start of the pending-frame buffer (top-left of layer,
 *     not of rect).
 *
 * @param pending_stride
 *     Number of bytes per row in the pending-frame buffer.
 *
 * @param last_buffer
 *     Pointer to the start of the last-frame buffer. Must be non-NULL.
 *
 * @param last_stride
 *     Number of bytes per row in the last-frame buffer.
 *
 * @param last_width
 *     Width of last_buffer in pixels.
 *
 * @param last_height
 *     Height of last_buffer in pixels.
 *
 * @param rect
 *     The rectangle within pending_buffer to check.
 *
 * @param result
 *     Receives the detected translucent rect's derived color (with alpha
 *     in the high byte) AND the two sample pixels (top-left and the
 *     maximum-channel-delta partner) used in the derivation. The retained
 *     samples are what the combine phase later uses to consider whether
 *     adjacent translucent rects can be merged. Untouched on failure.
 *
 * @return
 *     Non-zero if the rect can be expressed as a uniform translucent
 *     overlay, zero otherwise.
 */
#if defined(HAVE_ATTRIBUTE_TARGET_CLONES) && (defined(__x86_64__) || defined(__i386__))
/* Emit a variant per listed x86 feature level so the libc ifunc resolver
 * picks the best-specialized codegen for the running CPU at load time
 * without forcing the whole project to be built with -march=native. The
 * actual list is chosen by configure (GUAC_DISPLAY_TARGET_CLONES_X86) to
 * the widest the toolchain accepts. See the matching comment in
 * display-plan-search.c for the per-variant rationale. */
__attribute__((target_clones(GUAC_DISPLAY_TARGET_CLONES_X86)))
#endif
static int guac_display_plan_is_rect_translucent_overlay(
        const unsigned char* restrict pending_buffer, size_t pending_stride,
        const unsigned char* restrict last_buffer, size_t last_stride,
        int last_width, int last_height,
        const guac_rect* restrict rect,
        guac_display_plan_rect_data* restrict result) {

    /* Bail if the rect extends past last_frame's bounds (e.g., pending was
     * just resized larger and last_frame hasn't been re-snapshotted yet). */
    if (rect->left < 0 || rect->top < 0
            || rect->right > last_width || rect->bottom > last_height)
        return 0;

    int width = guac_rect_width(rect);
    int height = guac_rect_height(rect);

    /* A 1-pixel rect trivially "matches" any (src, α) pair, so the
     * detection result would be uninformative. Skip it. */
    if (width < 2 && height < 2)
        return 0;

    const uint32_t* pending = (const uint32_t*) GUAC_RECT_CONST_BUFFER(
            *rect, pending_buffer, pending_stride, GUAC_DISPLAY_LAYER_RAW_BPP);
    const uint32_t* last = (const uint32_t*) GUAC_RECT_CONST_BUFFER(
            *rect, last_buffer, last_stride, GUAC_DISPLAY_LAYER_RAW_BPP);

    size_t pending_pixel_stride = pending_stride / GUAC_DISPLAY_LAYER_RAW_BPP;
    size_t last_pixel_stride = last_stride / GUAC_DISPLAY_LAYER_RAW_BPP;

    /* Reference pixel: top-left of the rect. */
    uint32_t l0 = last[0];
    uint32_t p0 = pending[0];

    /* Scan for a second sample pixel "B" with a large channel delta in
     * last, picking the channel that produces the most precision in the
     * alpha derivation. Stop early once a sufficiently large delta is
     * found - beyond that point, additional precision is wasted.
     *
     * The full ARGB of B (best_p, best_l) is captured so that the
     * src-derivation step below can use B's data in every channel, not
     * just the channel that recorded the best delta. */
    int best_delta = 0;
    int best_dl = 0;
    int best_dp = 0;
    uint32_t best_p = 0;
    uint32_t best_l = 0;

    for (int y = 0; y < height; y++) {
        const uint32_t* last_row = last + y * last_pixel_stride;
        const uint32_t* pending_row = pending + y * pending_pixel_stride;
        for (int x = 0; x < width; x++) {

            uint32_t l = last_row[x];
            if (l == l0)
                continue;

            uint32_t p = pending_row[x];

            /* Try R, G, B channels (skip alpha; we operate on opaque
             * destinations where alpha is constant). */
            for (int shift = 0; shift <= 16; shift += 8) {

                int dl = (int)((l >> shift) & 0xFF) - (int)((l0 >> shift) & 0xFF);
                int adl = dl < 0 ? -dl : dl;
                if (adl <= best_delta)
                    continue;

                int dp = (int)((p >> shift) & 0xFF) - (int)((p0 >> shift) & 0xFF);

                /* dl and dp must share a sign for a valid source-over
                 * blend, since 255 - α >= 0. Otherwise this channel is
                 * inconsistent with any uniform translucent overlay. */
                if ((dl < 0) != (dp < 0))
                    continue;

                best_delta = adl;
                best_dl = dl;
                best_dp = dp;
                best_p = p;
                best_l = l;

            }

            if (best_delta >= GUAC_DISPLAY_PLAN_TRANSLUCENT_GOOD_DELTA)
                goto sample_done;

        }
    }
sample_done:

    /* Insufficient last-frame variation to derive alpha precisely. Either
     * the rect's last-frame region is uniform (in which case the existing
     * single-color check on pending would have caught a uniform pending,
     * and a non-uniform pending with uniform last cannot be a single
     * translucent overlay), or the variation is too small for the integer
     * arithmetic below to extract a meaningful alpha. */
    if (best_delta < GUAC_DISPLAY_PLAN_TRANSLUCENT_MIN_DELTA)
        return 0;

    /* Derive alpha. Both deltas have already been normalized to share a
     * sign in the loop above, so abs() works directly. Using round-to-
     * nearest (rather than truncation) division here halves the worst-
     * case derivation error - critical for the verification step below,
     * since α-derivation error propagates linearly into the predicted
     * blend at every pixel. */
    int abs_dl = best_dl < 0 ? -best_dl : best_dl;
    int abs_dp = best_dp < 0 ? -best_dp : best_dp;

    int neg_alpha = (255 * abs_dp + abs_dl / 2) / abs_dl;
    if (neg_alpha > 255)
        return 0;
    int alpha = 255 - neg_alpha;

    /* α = 0 means "no change at all" - just last_frame; the IMG path
     * would already drop such cells before we reach here. α very close to
     * 255 means a nearly-opaque rect, which the existing single-color
     * check would already catch. Below a minimum α, the source-over
     * equation barely depends on src - small noise in pending swings
     * the derived src wildly. */
    if (alpha < 8)
        return 0;

    /* Solve the per-channel source-over system of equations
     *
     *   255·p_A = α·src + (255-α)·l_A
     *   255·p_B = α·src + (255-α)·l_B
     *
     * for src, using both sample pixels (pixel A = top-left, pixel B =
     * the maximum-delta pixel found above). Adding the two equations
     * gives
     *
     *   src = (255·(p_A + p_B) - (255-α)·(l_A + l_B)) / (2·α)
     *
     * Averaging the two equations rather than solving from a single
     * pixel halves the variance of the per-channel rounding noise -
     * this is what makes adjacent rects with the same true (src, α)
     * derive the same recovered src despite different sample pixel
     * choices, eliminating the mid-rect blocking artifacts the
     * midpoint-of-min/max approach introduced. */
    int p_sum_r = (int)((p0 >> 16) & 0xFF) + (int)((best_p >> 16) & 0xFF);
    int p_sum_g = (int)((p0 >>  8) & 0xFF) + (int)((best_p >>  8) & 0xFF);
    int p_sum_b = (int)( p0        & 0xFF) + (int)( best_p        & 0xFF);
    int l_sum_r = (int)((l0 >> 16) & 0xFF) + (int)((best_l >> 16) & 0xFF);
    int l_sum_g = (int)((l0 >>  8) & 0xFF) + (int)((best_l >>  8) & 0xFF);
    int l_sum_b = (int)( l0        & 0xFF) + (int)( best_l        & 0xFF);

    int two_alpha = 2 * alpha;
    int src_r = (255 * p_sum_r - neg_alpha * l_sum_r + alpha) / two_alpha;
    int src_g = (255 * p_sum_g - neg_alpha * l_sum_g + alpha) / two_alpha;
    int src_b = (255 * p_sum_b - neg_alpha * l_sum_b + alpha) / two_alpha;

    if (src_r < 0) src_r = 0; else if (src_r > 255) src_r = 255;
    if (src_g < 0) src_g = 0; else if (src_g > 255) src_g = 255;
    if (src_b < 0) src_b = 0; else if (src_b > 255) src_b = 255;

    /* Per-channel scaled targets c.ch = α·src.ch. Verification below
     * checks every pixel's scaled_src is within ±T of these. */
    int c_r = alpha * src_r;
    int c_g = alpha * src_g;
    int c_b = alpha * src_b;

    /* Per-pixel tolerance bound (in scaled space).
     *
     * Per-pixel scaled_src deviates from α·src_true by at most ±127.5
     * (the original blender's ±0.5 rounding scaled by 255). Add a
     * derivation-noise budget of roughly α (for ≤±1 src error) plus
     * 255 (for ≤±1 α error scaled by the maximum |last - src| range).
     * The result is generous enough to admit any genuine source-over
     * overlay while still rejecting the ~half-screen-width spreads
     * non-blend image content produces. */
    int scaled_tolerance = 384 + alpha;

    /* Running min/max per channel, tracked in "scaled" space (i.e. not
     * divided by α). Per-pixel deviation from α·src is bounded; tracking
     * min/max lets us early-exit at row boundaries the moment the
     * observed range falls outside the (c - T, c + T) acceptance window. */
    int min_sr = INT_MAX, max_sr = INT_MIN;
    int min_sg = INT_MAX, max_sg = INT_MIN;
    int min_sb = INT_MAX, max_sb = INT_MIN;

    for (int y = 0; y < height; y++) {

        const uint32_t* last_row = last + y * last_pixel_stride;
        const uint32_t* pending_row = pending + y * pending_pixel_stride;

        int x = 0;

#ifdef HAVE_VECTOR_EXTENSIONS
        /* SIMD body: 8 pixels per iteration.
         *
         *   - Loads pending and last as 8-wide uint32 vectors (one
         *     unaligned 256-bit load each via memcpy; aligned or not,
         *     modern x86/ARM handle both at similar throughput).
         *   - Extracts per-channel uint8 values into 8-wide int32
         *     vectors via shift + mask.
         *   - Computes 255·p - (255-α)·l in int32 per lane - all
         *     SIMD-native operations, no divides.
         *   - Tracks per-channel running vector min/max via
         *     branchless ternary, which compiles to VPMINSD / VPMAXSD
         *     (AVX2), paired VPMINSD / VPMAXSD (SSE4.1), or VMINS /
         *     VMAXS (NEON).
         *
         * The per-pixel tolerance check is deferred to the end of each
         * row so the SIMD body stays completely branch-free. A rejected
         * rect exits after at most one row, which for the typical 64-
         * wide cell is 8 SIMD iterations - still far fewer than the
         * scalar path's full-pass cost on failure. */
        guac_v8u32 mask_ff = {
            0xFFu, 0xFFu, 0xFFu, 0xFFu,
            0xFFu, 0xFFu, 0xFFu, 0xFFu
        };

        guac_v8i32 vmin_sr = {
            INT_MAX, INT_MAX, INT_MAX, INT_MAX,
            INT_MAX, INT_MAX, INT_MAX, INT_MAX
        };
        guac_v8i32 vmax_sr = {
            INT_MIN, INT_MIN, INT_MIN, INT_MIN,
            INT_MIN, INT_MIN, INT_MIN, INT_MIN
        };
        guac_v8i32 vmin_sg = vmin_sr, vmax_sg = vmax_sr;
        guac_v8i32 vmin_sb = vmin_sr, vmax_sb = vmax_sr;

        int have_vec_data = 0;

        for (; x + 8 <= width; x += 8) {

            guac_v8u32 p_vec;
            guac_v8u32 l_vec;
            memcpy(&p_vec, pending_row + x, sizeof(guac_v8u32));
            memcpy(&l_vec, last_row + x,    sizeof(guac_v8u32));

            guac_v8i32 p_r = (guac_v8i32) ((p_vec >> 16) & mask_ff);
            guac_v8i32 p_g = (guac_v8i32) ((p_vec >>  8) & mask_ff);
            guac_v8i32 p_b = (guac_v8i32) ( p_vec        & mask_ff);

            guac_v8i32 l_r = (guac_v8i32) ((l_vec >> 16) & mask_ff);
            guac_v8i32 l_g = (guac_v8i32) ((l_vec >>  8) & mask_ff);
            guac_v8i32 l_b = (guac_v8i32) ( l_vec        & mask_ff);

            guac_v8i32 sr = 255 * p_r - neg_alpha * l_r;
            guac_v8i32 sg = 255 * p_g - neg_alpha * l_g;
            guac_v8i32 sb = 255 * p_b - neg_alpha * l_b;

            /* On x86 with AVX2 available at runtime, use the
             * single-instruction VPMINSD/VPMAXSD-on-ymm intrinsics
             * (one instruction per min or max) instead of the
             * portable bitwise-blend macro (which compiles to a 4-
             * instruction VPCMPGTD + VPAND + VPANDN + VPOR sequence
             * because GCC doesn't pattern-recognize the blend back
             * to native min/max for ymm-width vectors). The helpers
             * take pointers rather than vectors-by-value so the
             * call boundary uses a scalar (pointer) ABI even from
             * the default target_clones variant - no -Wpsabi
             * suppression needed. In the AVX2 variant the compiler
             * inlines them cleanly to a single ymm instruction; in
             * the default variant the call sits in dead code (the
             * runtime check always fails on non-AVX2 hardware). */
#if defined(HAVE_ATTRIBUTE_TARGET_CLONES) && (defined(__x86_64__) || defined(__i386__))
            if (__builtin_cpu_supports("avx2")) {
                guac_v8i32_min_avx2(&vmin_sr, &sr);
                guac_v8i32_max_avx2(&vmax_sr, &sr);
                guac_v8i32_min_avx2(&vmin_sg, &sg);
                guac_v8i32_max_avx2(&vmax_sg, &sg);
                guac_v8i32_min_avx2(&vmin_sb, &sb);
                guac_v8i32_max_avx2(&vmax_sb, &sb);
            }
            else
#endif
            {
                vmin_sr = GUAC_V8I32_MIN(sr, vmin_sr);
                vmax_sr = GUAC_V8I32_MAX(sr, vmax_sr);
                vmin_sg = GUAC_V8I32_MIN(sg, vmin_sg);
                vmax_sg = GUAC_V8I32_MAX(sg, vmax_sg);
                vmin_sb = GUAC_V8I32_MIN(sb, vmin_sb);
                vmax_sb = GUAC_V8I32_MAX(sb, vmax_sb);
            }

            have_vec_data = 1;

        }

        /* Reduce vector lanes to scalar and merge into the running
         * per-row min/max. The element loop is tiny and auto-vectorizes
         * back into a horizontal min/max on most targets, but either way
         * it runs at most once per row - insignificant compared to the
         * vector body above. */
        if (have_vec_data) {
            for (int k = 0; k < 8; k++) {
                if (vmin_sr[k] < min_sr) min_sr = vmin_sr[k];
                if (vmax_sr[k] > max_sr) max_sr = vmax_sr[k];
                if (vmin_sg[k] < min_sg) min_sg = vmin_sg[k];
                if (vmax_sg[k] > max_sg) max_sg = vmax_sg[k];
                if (vmin_sb[k] < min_sb) min_sb = vmin_sb[k];
                if (vmax_sb[k] > max_sb) max_sb = vmax_sb[k];
            }
        }
#endif

        /* Scalar body. When HAVE_VECTOR_EXTENSIONS is defined this
         * handles the 0 to 7 leftover pixels the SIMD body didn't cover;
         * otherwise it handles every pixel. */
        for (; x < width; x++) {

            uint32_t l = last_row[x];
            uint32_t p = pending_row[x];

            int sr = 255 * (int)((p >> 16) & 0xFF) - neg_alpha * (int)((l >> 16) & 0xFF);
            int sg = 255 * (int)((p >>  8) & 0xFF) - neg_alpha * (int)((l >>  8) & 0xFF);
            int sb = 255 * (int)( p        & 0xFF) - neg_alpha * (int)( l        & 0xFF);

            if (sr < min_sr) min_sr = sr;
            if (sr > max_sr) max_sr = sr;
            if (sg < min_sg) min_sg = sg;
            if (sg > max_sg) max_sg = sg;
            if (sb < min_sb) min_sb = sb;
            if (sb > max_sb) max_sb = sb;

        }

        /* End-of-row early exit. The check is per-pixel - every pixel's
         * scaled_src must be within ±scaled_tolerance of the equation-
         * derived c.ch - which translates to "min ≥ c - T AND max ≤ c +
         * T" on the running min/max we've been tracking. Either bound
         * being violated proves at least one pixel disagrees with the
         * uniform-(α, src) hypothesis. */
        if (min_sr < c_r - scaled_tolerance || max_sr > c_r + scaled_tolerance
                || min_sg < c_g - scaled_tolerance || max_sg > c_g + scaled_tolerance
                || min_sb < c_b - scaled_tolerance || max_sb > c_b + scaled_tolerance)
            return 0;

    }

    /* All pixels confirmed within ±scaled_tolerance of the equation-
     * derived (α, src). Emit those derived values directly - no midpoint
     * recovery, no biasing by which pixels happened to fall in the rect.
     * Also retain both sample pixels so the combine phase can later use
     * them to consider merging this rect with an adjacent translucent
     * rect that shares a similar (α, src). */
    result->color     = ((uint32_t) alpha << 24)
                      | ((uint32_t) src_r << 16)
                      | ((uint32_t) src_g <<  8)
                      |  (uint32_t) src_b;
    result->pending_a = p0;
    result->last_a    = l0;
    result->pending_b = best_p;
    result->last_b    = best_l;
    return 1;

}

void PFR_guac_display_plan_rewrite_op_as_rect(guac_display_plan_operation* op) {

    /* Only IMG ops may collapse to RECT - other types already have
     * their final representation. */
    if (op->type != GUAC_DISPLAY_PLAN_OPERATION_IMG)
        return;

    guac_display_layer* layer = op->layer;
    size_t stride = layer->pending_frame.buffer_stride;
    const unsigned char* buffer = layer->pending_frame.buffer;

    /* NOTE: Processing of operations referring to layers whose buffers
     * have been replaced with NULL is intentionally allowed to ensure
     * references to external buffers can be safely removed if
     * necessary, even before guac_display is freed */
    if (buffer == NULL)
        return;

    uint32_t color = 0x00000000;
    if (guac_display_plan_is_rect_single_color(buffer, stride, &op->dest, &color)) {

        /* Ignore alpha channel for opaque layers */
        if (layer->opaque)
            color |= 0xFF000000;

        op->type = GUAC_DISPLAY_PLAN_OPERATION_RECT;
        op->src.rect.color = color;

        /* Solid rects don't have sample pixels - the combine phase
         * keys translucent-merge eligibility on alpha < 0xFF, so
         * these zero values are never read. Initializing them
         * here keeps the op state defined. */
        op->src.rect.pending_a = 0;
        op->src.rect.last_a    = 0;
        op->src.rect.pending_b = 0;
        op->src.rect.last_b    = 0;

    }

    /* Pending didn't reduce to a single color, but on opaque layers
     * a non-uniform pending may still be expressible as a single
     * translucent rect blended (source-over) over last_frame. Catch
     * the common UI cases - modal backdrops, hover tints, selection
     * highlights - where a constant source color and constant alpha
     * produce a varying pending purely because the underlying
     * background varies. */
    else if (layer->opaque
            && layer->last_frame.buffer != NULL
            && guac_display_plan_is_rect_translucent_overlay(
                    buffer, stride,
                    layer->last_frame.buffer,
                    layer->last_frame.buffer_stride,
                    layer->last_frame.width,
                    layer->last_frame.height,
                    &op->dest, &op->src.rect)) {

        op->type = GUAC_DISPLAY_PLAN_OPERATION_RECT;

    }

}
