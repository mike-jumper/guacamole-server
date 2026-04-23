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

#include "display-atomic.h"
#include "display-plan.h"
#include "display-priv.h"
#include "guacamole/display.h"
#include "guacamole/rect.h"

#include <string.h>
#include <stdint.h>

#ifdef HAVE_VECTOR_EXTENSIONS
/**
 * 4-wide vector of uint64_t, used to SIMD-parallelize the 2D rolling hash's
 * cell_hash update loop when the compiler supports the GCC/Clang
 * __attribute__((vector_size(N))) extension (detected at configure time as
 * HAVE_VECTOR_EXTENSIONS). Operations on this type compile to AVX2
 * instructions when available (256-bit vectors, 4x uint64 per register),
 * and fall back to pairs of 128-bit SSE2 operations on older targets or to
 * scalar code on non-x86 architectures. No header is required for this
 * attribute; it is a core compiler extension.
 *
 * When HAVE_VECTOR_EXTENSIONS is not defined, the hash loop uses a purely
 * scalar implementation that remains strictly C99-compliant.
 *
 * The hash loop benefits from this because:
 *
 *   - cell_hash[x] updates for different x values are independent within a
 *     single row (each column has its own accumulator), so 4 columns can be
 *     updated per SIMD iteration.
 *
 *   - The per-update arithmetic - "cell_hash * 62 + row_hash" - reduces to
 *     shifts and additions (62 = 64 - 2, so `ch * 62 = (ch << 6) - (ch << 1)`),
 *     which are natively supported on all SIMD ISAs. AVX2 lacks a native
 *     64-bit multiply (only AVX-512 has _mm512_mullo_epi64), so using shifts
 *     sidesteps that limitation while still producing the correct result
 *     modulo 2^64.
 */
typedef uint64_t guac_v4u64 __attribute__((vector_size(32)));
#endif

/**
 * Tests whether the given bucket in the plan's occupancy bitmap is set.
 * The bitmap acts as an L1-resident fast-path filter that avoids touching
 * the much larger (1 MB) ops_by_hash table for the overwhelming majority of
 * search-phase queries that miss.
 *
 * @param plan
 *     The plan whose bitmap is being read.
 *
 * @param index
 *     The bucket index (already reduced via GUAC_DISPLAY_PLAN_OPERATION_HASH).
 *
 * @return
 *     Non-zero if the bucket has been marked occupied, zero otherwise.
 */
static inline int guac_display_plan_occupancy_test(
        const guac_display_plan* plan, size_t index) {
    uint8_t byte = guac_display_atomic_load_u8(
            &plan->ops_by_hash_occupancy[index >> 3]);
    return byte & (1 << (index & 7));
}

/**
 * Sets the given bucket as occupied in the plan's occupancy bitmap, using
 * an atomic OR to remain safe under concurrent updates from multiple
 * index-phase workers.
 *
 * @param plan
 *     The plan whose bitmap is being modified.
 *
 * @param index
 *     The bucket index (already reduced via GUAC_DISPLAY_PLAN_OPERATION_HASH).
 */
static inline void guac_display_plan_occupancy_set(
        guac_display_plan* plan, size_t index) {
    guac_display_atomic_or_u8(&plan->ops_by_hash_occupancy[index >> 3],
            (uint8_t) (1 << (index & 7)));
}

/**
 * Clears the given bucket in the plan's occupancy bitmap, using an atomic
 * AND to remain safe under concurrent updates from multiple search-phase
 * workers. Called by the search phase after successfully claiming an op,
 * so that subsequent queries against the (now-empty) bucket take the
 * L1-resident fast path in guac_hash_foreach_image_rect() and never enter
 * the callback at all.
 *
 * @param plan
 *     The plan whose bitmap is being modified.
 *
 * @param index
 *     The bucket index (already reduced via GUAC_DISPLAY_PLAN_OPERATION_HASH).
 */
static inline void guac_display_plan_occupancy_clear(
        guac_display_plan* plan, size_t index) {
    guac_display_atomic_and_u8(&plan->ops_by_hash_occupancy[index >> 3],
            (uint8_t) ~(1 << (index & 7)));
}

/**
 * Stores the given operation within the ops_by_hash table of the given display
 * plan based on the given hash value. The hash function applied for storing
 * the operation is GUAC_DISPLAY_PLAN_OPERATION_HASH(). If another operation is
 * already stored at the same location within ops_by_hash, this store is
 * silently dropped (the existing entry remains).
 *
 * This function is safe to invoke concurrently from multiple threads: it uses
 * an atomic compare-and-swap to claim an empty slot, and the hash field is
 * only written by the thread that wins the CAS. Ordering between the index
 * phase (which populates the table) and the subsequent search phase (which
 * reads it) is established externally by the parallel-for barrier, so no
 * memory fences stronger than relaxed are required here.
 *
 * @param plan
 *     The plan to store the operation within.
 *
 * @param hash
 *     The hash value to use to calculate the storage location. This value will
 *     be further hashed with GUAC_DISPLAY_PLAN_OPERATION_HASH().
 *
 * @param op
 *     The operation to store.
 */
static void guac_display_plan_store_indexed_op(guac_display_plan* plan, uint64_t hash,
        guac_display_plan_operation* op) {

    size_t index = GUAC_DISPLAY_PLAN_OPERATION_HASH(hash);
    guac_display_plan_indexed_operation* entry = &(plan->ops_by_hash[index]);

    /* Atomically claim the slot by transitioning entry->op from NULL to op.
     * Only the winning thread writes the hash; losers drop their entry. */
    void* expected = NULL;
    if (guac_display_atomic_cas_ptr((void**) &entry->op, &expected, op)) {
        entry->hash = hash;

        /* Mark the bucket as occupied so search-phase lookups can take the
         * L1-resident fast path. */
        guac_display_plan_occupancy_set(plan, index);
    }

}

/**
 * Removes and returns a pointer to the matching operation stored within the
 * ops_by_hash table of the given display plan, if any. If no such operation is
 * stored, NULL is returned.
 *
 * This function is safe to invoke concurrently from multiple threads: the op
 * pointer is extracted via atomic exchange, so only one caller ever observes
 * a non-NULL result for any given slot. Concurrent callers that lose the
 * exchange race observe NULL and treat the entry as absent. Ordering with
 * the index phase is established externally by the parallel-for barrier.
 *
 * @param plan
 *     The plan to retrieve the operation from.
 *
 * @param hash
 *     The hash value to use to calculate the storage location. This value will
 *     be further hashed with GUAC_DISPLAY_PLAN_OPERATION_HASH().
 *
 * @return
 *     The operation that was stored under the given hash, if any, or NULL if
 *     no such operation was found or was already claimed by another caller.
 */
static guac_display_plan_operation* guac_display_plan_remove_indexed_op(guac_display_plan* plan, uint64_t hash) {

    size_t index = GUAC_DISPLAY_PLAN_OPERATION_HASH(hash);

    /* Fast path: the occupancy bitmap is 8 KB and fits entirely in L1 per
     * core, whereas ops_by_hash is 1 MB and will typically be out of L2 for
     * random-pattern access. Skipping the ops_by_hash load for empty buckets
     * (the ~97% case for typical plans) eliminates the cost that dominates
     * the search phase. */
    if (!guac_display_plan_occupancy_test(plan, index))
        return NULL;

    guac_display_plan_indexed_operation* entry = &(plan->ops_by_hash[index]);

    /* NOTE: We verify the hash value here because the lookup performed is
     * actually a hash of a hash. There's an additional chance of collisions
     * between hash values at this second level of hashing.
     *
     * An atomic load is used here to ensure portable, non-torn reads of the
     * 64-bit hash value on platforms where aligned uint64 reads are not
     * naturally atomic. Relaxed ordering is sufficient because the hash field
     * was committed by the index phase, which is separated from the search
     * phase by a parallel-for barrier providing release/acquire semantics. */
    if (guac_display_atomic_load_u64(&entry->hash) != hash)
        return NULL;

    /* Guard the RMW with a cheap shared-state load. When a region of the
     * pending/last frame contains many 64x64 windows that all hash to the
     * same bucket (e.g. a large expanse of a single color, where every such
     * window produces an identical hash), the first worker to reach the
     * bucket wins the exchange and the op pointer transitions to NULL for
     * all subsequent accesses. Without this guard, every subsequent access
     * would still perform the exchange - a full read-modify-write that takes
     * exclusive ownership of the cache line and invalidates it in every
     * other worker's cache.
     *
     * A plain atomic load, by contrast, keeps the cache line in shared state
     * across all readers. Workers that observe NULL here exit via the cheap
     * path and never contend for ownership of the line. */
    if (guac_display_atomic_load_ptr((void* const*) &entry->op) == NULL)
        return NULL;

    /* Atomic exchange ensures at most one caller ever extracts a non-NULL op
     * for any given slot, even when multiple workers race to claim the same
     * hash match. Losers get NULL back and treat the slot as empty. */
    guac_display_plan_operation* op = (guac_display_plan_operation*)
            guac_display_atomic_exchange_ptr((void**) &entry->op, NULL);
    if (op == NULL)
        return NULL;

    /* We won the race to claim this op. Clear the bucket's occupancy bit
     * so future queries hashing to this same bucket short-circuit at the
     * inlined bitmap check in guac_hash_foreach_image_rect() and never
     * enter the callback at all. This is critical for regions where many
     * 64x64 windows produce the same hash (e.g. solid-color expanses):
     * without this clear, every subsequent such window would still pay
     * the function-call and hash/op-load cost (~15-30 ns each), which for
     * a large region aggregates to tens of milliseconds. With the clear,
     * every subsequent window's entire cost is an L1-resident bitmap test
     * (~4 ns) inside the hot loop. */
    guac_display_plan_occupancy_clear(plan, index);
    return op;

}

/**
 * Callback invoked by guac_hash_foreach_image_rect() for each 64x64 rectangle
 * of image data.
 *
 * @param plan
 *     The display plan related to the call to guac_hash_foreach_image_rect().
 *
 * @param x
 *     The X coordinate of the upper-left corner of the current 64x64 rectangle
 *     within the search region.
 *
 * @param y
 *     The Y coordinate of the upper-left corner of the current 64x64 rectangle
 *     within the search region.
 *
 * @param hash
 *     The hash value that applies to the current 64x64 rectangle.
 *
 * @param closure
 *     The closure value that was originally provided to the call to 
 *     guac_hash_foreach_image_rect().
 */
typedef void guac_hash_callback(guac_display_plan* plan, int x, int y, uint64_t hash, void* closure);

/**
 * Iterates through each 64x64 subrectangle within the given rectangular region
 * of the underlying buffer of the given layer state, invoking the given
 * callback for each such subrectangle. Each 64x64 subrectangle within the
 * rectangular region is evaluated by sliding a 64x64 window over each pixel of
 * the region such that every 64x64 subrectangle in the region is eventually
 * covered.
 *
 * If an occupancy filter is supplied, the callback is invoked only for hashes
 * whose corresponding bit in the filter is set. This allows the search phase
 * to skip the function call entirely for the overwhelming majority of
 * positions where no indexed op can possibly match, keeping the inner loop
 * L1-resident.
 *
 * @param plan
 *     The display plan related to the search/indexing operation being
 *     performed.
 *
 * @param layer_state
 *     The layer state containing the image buffer to hash.
 *
 * @param rect
 *     The rectangular region within the image buffer that should be hashed.
 *
 * @param callback
 *     The callback to invoke for each 64x64 subrectangle of the given region.
 *
 * @param closure
 *     The arbitrary value to pass the given callback each time it is invoked
 *     through this function call.
 *
 * @param occupancy_filter
 *     Optional pointer to an occupancy bitmap of size
 *     GUAC_DISPLAY_PLAN_OPERATION_INDEX_SIZE / 8 bytes. When non-NULL, the
 *     callback is invoked only if the bit at position
 *     GUAC_DISPLAY_PLAN_OPERATION_HASH(hash) in the bitmap is set. When NULL,
 *     the callback is invoked for every hash (the original behavior).
 */
#if defined(HAVE_ATTRIBUTE_TARGET_CLONES) && (defined(__x86_64__) || defined(__i386__))
/* Compile this function with GCC/Clang function multiversioning so that
 * separate variants are emitted for each listed x86 feature level in
 * addition to the default (SSE2 baseline) variant. The libc ifunc
 * resolver picks the most capable variant the running CPU supports at
 * load time.
 *
 * Possible tiers (the actual list is chosen by configure via
 * GUAC_DISPLAY_TARGET_CLONES_X86, whichever is the widest the toolchain
 * accepts - older compilers may support only a subset):
 *
 *   default    - baseline x86-64 (SSE2). Minimum target.
 *   sse4.2     - Nehalem+ (2008); brings SSE4.1 which GCC recognizes as
 *                native min/max for xmm-width vector blends.
 *   avx2       - Haswell+ (2013); 256-bit integer ops on ymm.
 *   avx512f    - Skylake-X+ (2017); EVEX encoding and extra register
 *                pressure relief even for our 256-bit vector_size(32)
 *                types, plus better scheduling on capable cores.
 *
 * Each listed variant specializes instruction selection for its feature
 * set. Callers need not know which feature levels exist - the ifunc
 * resolver handles dispatch at first call, and the set can be expanded
 * in future without touching call sites (just configure.ac's probe list).
 *
 * When HAVE_ATTRIBUTE_TARGET_CLONES is not defined (e.g., toolchains
 * lacking target_clones support entirely, or configure probe failure for
 * all candidate lists), only a single variant is emitted matching the
 * build's target ISA. Users may still opt into a specific feature level
 * by adding -march=... to their build flags. */
__attribute__((target_clones(GUAC_DISPLAY_TARGET_CLONES_X86)))
#endif
static int guac_hash_foreach_image_rect(guac_display_plan* plan,
        const guac_display_layer_state* layer_state, const guac_rect* rect,
        guac_hash_callback* callback, void* closure,
        const uint8_t* occupancy_filter) {

    size_t stride = layer_state->buffer_stride;
    const unsigned char* data = GUAC_DISPLAY_LAYER_STATE_CONST_BUFFER(*layer_state, *rect);

    int x, y;

    /* Align cell_hash to the AVX2 vector width so the SIMD main loop below
     * can emit aligned 256-bit loads/stores. Without this, GCC falls back
     * to vmovdqu (unaligned), which on Haswell-and-earlier costs extra
     * cycles whenever a load crosses a 16-byte boundary, and on newer
     * cores still forgoes some peephole optimizations the aligned form
     * enables. The array is indexed by SIMD group from position zero,
     * with current_cell_hash advancing by 4 uint64 (32 bytes) per SIMD
     * iteration, so every SIMD access is 32-byte aligned when the base
     * is. _Alignas is a C11 keyword and needs no additional header. */
    _Alignas(32) uint64_t cell_hash[GUAC_DISPLAY_MAX_WIDTH] = { 0 };

    /* NOTE: Because the hash value of the sliding 64x64 window is available
     * only upon reaching the bottom-right corner of that window, we offset the
     * coordinates here by the relative location of the bottom-right corner
     * (GUAC_DISPLAY_CELL_SIZE - 1) so that we have easy access to the
     * coordinates of the upper-left corner of the sliding window, as required
     * by the callback being invoked.
     *
     * This also allows us to easily determine when the hash is valid and it's
     * safe to invoke the callback. Once the coordinates are within the given
     * rect, we have evaluated a full 64x64 rectangle and have a valid hash. */

    int start_x = rect->left   - GUAC_DISPLAY_CELL_SIZE + 1;
    int end_x   = rect->right  - GUAC_DISPLAY_CELL_SIZE + 1;
    int start_y = rect->top    - GUAC_DISPLAY_CELL_SIZE + 1;
    int end_y   = rect->bottom - GUAC_DISPLAY_CELL_SIZE + 1;

    /* Granularity of the preemption check: consult the shared plan state
     * (and the display-wide waiter counter) every N rows of this sweep.
     * Each row is on the order of a few hundred cells' worth of hash
     * work (tens of microseconds on 4K widths), so 16 rows is roughly
     * the latency with which this worker will notice and cooperate with
     * a contending thread - well under typical inter-frame pacing, but
     * far enough apart that the atomic loads are negligible next to the
     * hash arithmetic. */
    const int ABORT_CHECK_INTERVAL = 16;

    for (y = start_y; y < end_y; y++) {

        /* Preemption check: if another worker has already signalled that
         * the search should abort, stop immediately. Otherwise, check
         * whether a drawing thread is waiting to acquire the display's
         * pending_frame lock (display->draw_pending) and flip the shared
         * abort flag if the search hasn't yet converted an IMG op to a
         * COPY - the cost we've incurred so far is, by that point,
         * unproductive, and draining is preferable to making the drawing
         * thread wait out the rest of the sweep. */
        if (((y - start_y) & (ABORT_CHECK_INTERVAL - 1)) == 0) {
            if (atomic_load_explicit(&plan->search_aborted,
                    memory_order_relaxed))
                return 0;
            if (!atomic_load_explicit(&plan->copies_found,
                        memory_order_relaxed)
                    && atomic_load_explicit(&plan->display->draw_pending,
                        memory_order_relaxed)) {
                atomic_store_explicit(&plan->search_aborted, 1,
                        memory_order_relaxed);
                return 0;
            }
        }

        uint64_t* current_cell_hash = cell_hash;

        /* Get current row */
        uint32_t* row = (uint32_t*) data;
        data += stride;

        /* Calculate row segment hashes for entire row.
         *
         * When HAVE_VECTOR_EXTENSIONS is defined (configure has confirmed
         * that the compiler supports __attribute__((vector_size(N)))),
         * the inner x loop is split into a SIMD main body and a scalar
         * tail. In the SIMD body, four x positions are processed per
         * iteration: row_hash updates remain serial due to the inherent
         * accumulator dependency chain, but the four cell_hash updates
         * (one per column) are independent and are performed as a single
         * vector operation. Bitmap filtering and callback invocation
         * remain scalar because the bitmap access is random and the
         * callback is inherently sequential.
         *
         * The arithmetic identity used by the cell_hash update is
         * 'ch * 62 == (ch << 6) - (ch << 1)', which avoids the 64-bit
         * multiply that AVX2 does not provide natively (only AVX-512
         * does, via vpmullq). Using shifts and subtraction produces
         * exactly the same modular result as the scalar path.
         *
         * When HAVE_VECTOR_EXTENSIONS is not defined, only the scalar tail
         * loop runs, processing every x sequentially. */
        uint64_t row_hash = 0;
        x = start_x;

#ifdef HAVE_VECTOR_EXTENSIONS
        for (; x + 4 <= end_x; x += 4, row += 4, current_cell_hash += 4) {

            /* Serial row_hash updates for the 4 positions. The dependency
             * chain on row_hash makes this unparallelizable within a row,
             * but having the 4 updates explicitly sequenced in a tight
             * block gives the compiler maximum freedom to schedule the
             * multiply-add chain. */
            uint64_t rh[4];
            row_hash = ((row_hash * 31) << 1) + row[0]; rh[0] = row_hash;
            row_hash = ((row_hash * 31) << 1) + row[1]; rh[1] = row_hash;
            row_hash = ((row_hash * 31) << 1) + row[2]; rh[2] = row_hash;
            row_hash = ((row_hash * 31) << 1) + row[3]; rh[3] = row_hash;

            /* SIMD cell_hash update: new_ch = old_ch * 62 + rh, applied
             * to 4 columns in parallel. memcpy is used for the vector
             * load/store so the compiler generates an unaligned vector
             * move (vmovdqu or equivalent) without aliasing concerns. */
            guac_v4u64 ch_v;
            guac_v4u64 rh_v;
            memcpy(&ch_v, current_cell_hash, sizeof(guac_v4u64));
            memcpy(&rh_v, rh, sizeof(guac_v4u64));

            guac_v4u64 new_ch = ((ch_v << 6) - (ch_v << 1)) + rh_v;
            memcpy(current_cell_hash, &new_ch, sizeof(guac_v4u64));

            /* Fire callbacks for whichever of the four x positions are
             * in-range and pass the occupancy filter. The callback path
             * is scalar because:
             *
             *   - The bitmap access pattern is random (hash-indexed), so
             *     SIMD gather would be slower than 4 scalar byte loads on
             *     most CPUs.
             *
             *   - The callback itself is sequential (transforms one op
             *     per invocation) and early-exits via the filter on the
             *     common no-match path. */
            if (y >= rect->top && x + 3 >= rect->left) {
                int k;
                for (k = 0; k < 4; k++) {
                    int cur_x = x + k;
                    if (cur_x < rect->left)
                        continue;
                    uint64_t ch_val = current_cell_hash[k];
                    if (occupancy_filter != NULL) {
                        size_t idx = GUAC_DISPLAY_PLAN_OPERATION_HASH(ch_val);
                        if (!(occupancy_filter[idx >> 3] & (1 << (idx & 7))))
                            continue;
                    }
                    callback(plan, cur_x, y, ch_val, closure);
                }
            }

        }
#endif

        /* Scalar tail. With HAVE_VECTOR_EXTENSIONS this handles the 0 to 3
         * leftover positions that don't form a complete SIMD group; without
         * it, this processes every x in the row. */
        for (; x < end_x; x++) {

            /* Get current pixel */
            uint32_t pixel = *(row++);

            /* Update hash value for current row segment */
            row_hash = ((row_hash * 31) << 1) + pixel;

            /* Incorporate row hash value into overall cell hash */
            uint64_t cell_hash = ((*current_cell_hash * 31) << 1) + row_hash;
            *(current_cell_hash++) = cell_hash;

            if (y >= rect->top && x >= rect->left) {
                if (occupancy_filter != NULL) {
                    size_t idx = GUAC_DISPLAY_PLAN_OPERATION_HASH(cell_hash);
                    if (!(occupancy_filter[idx >> 3] & (1 << (idx & 7))))
                        continue;
                }
                callback(plan, x, y, cell_hash, closure);
            }

        }

    } /* end for each row */

    return 0;

}

/**
 * Initializes the given rectangle with the bounds of the pending frame cell
 * containing the given coordinate.
 *
 * @param rect
 *     The rectangle to initialize.
 *
 * @param x
 *     The X coordinate of the point that the rectangle must contain.
 *
 * @param y
 *     The Y coordinate of the point that the rectangle must contain.
 */
static void guac_display_cell_init_rect(guac_rect* rect, int x, int y) {
    x = (x / GUAC_DISPLAY_CELL_SIZE) * GUAC_DISPLAY_CELL_SIZE;
    y = (y / GUAC_DISPLAY_CELL_SIZE) * GUAC_DISPLAY_CELL_SIZE;
    guac_rect_init(rect, x, y, GUAC_DISPLAY_CELL_SIZE, GUAC_DISPLAY_CELL_SIZE);
}

/**
 * Callback for guac_hash_foreach_image_rect() which stores the given operation
 * in the ops_by_hash table of the given display plan.
 *
 * @param plan
 *     The display plan to store the given operation in.
 *
 * @param x
 *     The X coordinate of the upper-left corner of the 64x64 rectangle
 *     modified by the given operation.
 *
 * @param y
 *     The Y coordinate of the upper-left corner of the 64x64 rectangle
 *     modified by the given operation.
 *
 * @param hash
 *     The hash value that applies to the 64x64 rectangle at the given
 *     coordinates.
 *
 * @param closure
 *     A pointer to the guac_display_plan_operation that should be stored
 *     within the ops_by_hash table of the given display plan.
 */
static void guac_display_plan_index_op_for_cell(guac_display_plan* plan, int x, int y, uint64_t hash, void* closure) {
    guac_display_plan_store_indexed_op(plan, hash, (guac_display_plan_operation*) closure);
}

void PFR_guac_display_plan_index_op(guac_display_plan* plan,
        guac_display_plan_operation* op) {

    if (op->type != GUAC_DISPLAY_PLAN_OPERATION_IMG)
        return;

    guac_display_layer* layer = op->layer;

    /* Access layer bounds directly rather than via
     * guac_display_layer_get_bounds(), which would attempt to
     * reacquire the pending_frame read lock from a worker thread. */
    guac_rect layer_bounds = {
        .left = 0,
        .top = 0,
        .right = layer->pending_frame.width,
        .bottom = layer->pending_frame.height
    };

    guac_rect cell;
    guac_display_cell_init_rect(&cell, op->dest.left, op->dest.top);

    guac_rect_constrain(&cell, &layer_bounds);
    if (guac_rect_width(&cell) == GUAC_DISPLAY_CELL_SIZE
            && guac_rect_height(&cell) == GUAC_DISPLAY_CELL_SIZE) {
        /* Pass NULL for the occupancy filter - the index phase must
         * call the storing callback unconditionally. */
        guac_hash_foreach_image_rect(plan, &layer->pending_frame,
                &cell, guac_display_plan_index_op_for_cell, op,
                NULL);
    }

}

/**
 * Compares two rectangular regions of two arbitrary buffers, returning whether
 * those regions contain identical data.
 *
 * @param data_a
 *     A pointer to the first byte of image data within the first region being
 *     compared.
 *
 * @param width_a
 *     The width of the first region, in pixels.
 *
 * @param height_a
 *     The height of the first region, in pixels.
 *
 * @param stride_a
 *     The number of bytes in each row of image data in the first region.
 *
 * @param data_b
 *     A pointer to the first byte of image data within the second region being
 *     compared.
 *
 * @param width_b
 *     The width of the second region, in pixels.
 *
 * @param height_b
 *     The height of the second region, in pixels.
 *
 * @param stride_b
 *     The number of bytes in each row of image data in the first region.
 *
 * @return
 *     Non-zero if the regions contain at least one differing pixel, zero
 *     otherwise.
 */
static int guac_image_cmp(const unsigned char* restrict data_a, int width_a, int height_a,
        int stride_a, const unsigned char* restrict data_b, int width_b, int height_b,
        int stride_b) {

    int y;

    /* If core dimensions differ, just compare those. Done. */
    if (width_a != width_b) return width_a - width_b;
    if (height_a != height_b) return height_a - height_b;

    size_t length = guac_mem_ckd_mul_or_die(width_a, GUAC_DISPLAY_LAYER_RAW_BPP);

    for (y = 0; y < height_a; y++) {

        /* Compare row. If different, use that result. */
        int cmp_result = memcmp(data_a, data_b, length);
        if (cmp_result != 0)
            return cmp_result;

        /* Next row */
        data_a += stride_a;
        data_b += stride_b;

    }

    /* Otherwise, same. */
    return 0;

}

/* The CAS in try_extend_copy_match and in the hash-path callback below
 * reinterprets &op->type as int*, so the operation type enum must have the
 * same object representation as int. This holds on every mainstream C
 * toolchain we support (enums default to int storage absent -fshort-enums),
 * but asserting it here turns any violation into a compile-time error
 * rather than a silent miscompile. */
_Static_assert(sizeof(guac_display_plan_operation_type) == sizeof(int),
        "guac_display_plan_operation_type must be int-sized for atomic CAS");

/**
 * Attempts to extend a previously-matched copy into a single cell-aligned
 * neighbor. Compares the full 64x64 source window at (src_x, src_y) within
 * copy_from_layer->last_frame against the 64x64 pending_frame cell at
 * (dst_x, dst_y) within copy_to_layer, and atomically transforms the
 * neighbor's op from IMG to COPY if the contents are bit-identical.
 *
 * Bypassing the rolling-hash lookup is the point: when a scroll vector has
 * already been confirmed at the initial match, adjacent cells are
 * overwhelmingly likely to match the same vector, so checking them
 * directly is faster than the full per-cell hash/lookup dance. Cells that
 * don't match bail out early on the first differing row inside
 * guac_image_cmp().
 *
 * Extension-path workers race against hash-path workers (and against
 * extensions from other initial matches) on the neighbor's op->type.
 * Serialization is provided by an atomic compare-and-swap: whichever
 * thread transitions op->type from IMG to COPY first wins, and losers
 * return 0 without touching the op.
 *
 * @param copy_from_layer
 *     The layer whose last_frame provides the source image data.
 *
 * @param copy_to_layer
 *     The layer whose pending_frame cell is to be transformed.
 *
 * @param src_x
 *     The X coordinate of the upper-left corner of the 64x64 source rect
 *     within copy_from_layer->last_frame. Does not need to be cell-aligned.
 *
 * @param src_y
 *     The Y coordinate of the upper-left corner of the 64x64 source rect.
 *
 * @param dst_x
 *     The X coordinate of the upper-left corner of the 64x64 destination
 *     cell within copy_to_layer->pending_frame. Must be a multiple of
 *     GUAC_DISPLAY_CELL_SIZE.
 *
 * @param dst_y
 *     The Y coordinate of the upper-left corner of the 64x64 destination
 *     cell within copy_to_layer->pending_frame. Must be a multiple of
 *     GUAC_DISPLAY_CELL_SIZE.
 *
 * @return
 *     Non-zero if the neighbor was verified and atomically claimed as a
 *     COPY, zero otherwise (including when the neighbor was out of bounds,
 *     had no associated op, was already transformed by another worker, or
 *     did not bit-match the source window).
 */
static int try_extend_copy_match(guac_display_layer* copy_from_layer,
        guac_display_layer* copy_to_layer,
        int src_x, int src_y, int dst_x, int dst_y) {

    /* Source 64x64 window must fit entirely within last_frame */
    if (src_x < 0 || src_y < 0
            || src_x + GUAC_DISPLAY_CELL_SIZE > copy_from_layer->last_frame.width
            || src_y + GUAC_DISPLAY_CELL_SIZE > copy_from_layer->last_frame.height)
        return 0;

    /* Destination 64x64 window must fit entirely within the pending_frame
     * pixel buffer. The cell-grid check below is NOT sufficient on its own
     * because pending_frame_cells_height is ceil(height / CELL_SIZE), which
     * exceeds the logical pixel height when the height isn't a multiple of
     * 64. For internally-allocated buffers that mismatch is absorbed by
     * XFW_guac_display_layer_buffer_resize()'s rounding of the underlying
     * allocation up to GUAC_DISPLAY_RESIZE_FACTOR (also 64) - the extra
     * padding rows are actually mapped. But externally-owned buffers
     * (RDP/VNC framebuffers, etc.) are not padded, so a cell at the bottom
     * cell-row would have guac_image_cmp() read past the end of the
     * buffer into unmapped memory. Validating against pending_frame.width/
     * height guarantees every row we touch is backed by real storage
     * regardless of buffer origin. */
    if (dst_x < 0 || dst_y < 0
            || dst_x + GUAC_DISPLAY_CELL_SIZE > copy_to_layer->pending_frame.width
            || dst_y + GUAC_DISPLAY_CELL_SIZE > copy_to_layer->pending_frame.height)
        return 0;

    /* Destination cell must also be within the cell grid so the op-lookup
     * below indexes validly. For internal buffers the pixel-bounds check
     * above implies this (both use the same logical dimensions); for
     * external buffers the pixel check is strictly tighter. Still
     * explicit here so the invariant is visible at the call site. */
    int cell_x = dst_x / GUAC_DISPLAY_CELL_SIZE;
    int cell_y = dst_y / GUAC_DISPLAY_CELL_SIZE;
    if ((size_t) cell_x >= copy_to_layer->pending_frame_cells_width
            || (size_t) cell_y >= copy_to_layer->pending_frame_cells_height)
        return 0;

    /* Cell must be dirty: non-dirty cells have no associated op and thus
     * nothing for an extension to transform. */
    guac_display_layer_cell* cell = &copy_to_layer->pending_frame_cells[
            cell_y * copy_to_layer->pending_frame_cells_width + cell_x];
    guac_display_plan_operation* op = cell->related_op;
    if (op == NULL)
        return 0;

    /* Cheap non-atomic fast-path check. If another worker already
     * transformed this op (either via its own hash-path claim or via a
     * separate extension from another initial match), bail out before
     * paying for image_cmp. This read may race with a concurrent
     * transformer's write of op->type, but is safe: the CAS below is
     * the real serialization point, and observing a stale IMG here
     * just means we'll fall through to image_cmp and lose the CAS. */
    if (op->type != GUAC_DISPLAY_PLAN_OPERATION_IMG)
        return 0;

    /* Verify pixel-exact match across the full 64x64 window. Comparing
     * the full cell is safe even when the op's original dirty subrect
     * covered only part of the cell: a full-cell match subsumes any
     * subrect match, and this transformation will overwrite op->dest
     * with a full-cell rect anyway. */
    guac_rect src_rect;
    guac_rect_init(&src_rect, src_x, src_y,
            GUAC_DISPLAY_CELL_SIZE, GUAC_DISPLAY_CELL_SIZE);
    guac_rect dst_rect;
    guac_rect_init(&dst_rect, dst_x, dst_y,
            GUAC_DISPLAY_CELL_SIZE, GUAC_DISPLAY_CELL_SIZE);

    const unsigned char* copy_from = GUAC_DISPLAY_LAYER_STATE_CONST_BUFFER(
            copy_from_layer->last_frame, src_rect);
    const unsigned char* copy_to = GUAC_DISPLAY_LAYER_STATE_CONST_BUFFER(
            copy_to_layer->pending_frame, dst_rect);

    if (guac_image_cmp(
                copy_from, GUAC_DISPLAY_CELL_SIZE, GUAC_DISPLAY_CELL_SIZE,
                copy_from_layer->last_frame.buffer_stride,
                copy_to, GUAC_DISPLAY_CELL_SIZE, GUAC_DISPLAY_CELL_SIZE,
                copy_to_layer->pending_frame.buffer_stride) != 0)
        return 0;

    /* Atomically transition op->type from IMG to COPY. If this CAS fails,
     * a concurrent hash-path or extension worker has already claimed the
     * op; yielding to them is correct. Relaxed ordering is sufficient
     * because the only concurrent writers of op->type are other CAS
     * operations here (or in the hash-path callback below), and no reader
     * of op->src / op->dest runs concurrently with the post-CAS writes
     * (combine/apply phases run only after the parallel-for barrier). */
    int expected = GUAC_DISPLAY_PLAN_OPERATION_IMG;
    if (!guac_display_atomic_cas_int((int*) &op->type, &expected,
                GUAC_DISPLAY_PLAN_OPERATION_COPY))
        return 0;

    /* We own this op exclusively now. No other worker will read or write
     * op->src or op->dest, since the CAS has flipped the type and any
     * subsequent pre-check in another worker observes COPY. */
    op->src.layer_rect.layer = copy_from_layer->last_frame_buffer;
    op->src.layer_rect.rect = src_rect;
    op->dest = dst_rect;

    return 1;

}

/**
 * Callback for guac_hash_foreach_image_rect() which searches the ops_by_hash
 * table of the given display plan for occurrences of the given hash, replacing
 * the matching operation with a copy operation if a match is found.
 *
 * Once an initial match is confirmed, the match is extended vertically (down
 * and up) one cell at a time as long as each adjacent cell trivially matches
 * the same scroll vector. Each extended cell bypasses the rolling-hash
 * lookup entirely: it's verified directly against its pending_frame neighbor
 * via guac_image_cmp(). This catches scrolled regions that the hash-based
 * search may have missed (e.g. cells whose hashes collided out of
 * ops_by_hash during indexing) and converts vertical runs of IMG ops along
 * the same scroll vector into COPY ops in one pass.
 *
 * Extension is limited to the vertical axis because the search itself is
 * parallelized into vertical strips by last_frame X range (see
 * PFR_LFR_guac_display_plan_rewrite_as_copies). A vertical neighbor
 * (op->dest.x unchanged, op->dest.y shifted by 64) has its source at the
 * same last_frame X as the initial match, and so falls within this same
 * worker's strip - the contention against any racing claim is between this
 * worker's extension and its own future hash iteration, both running on the
 * same core. A horizontal neighbor would shift the source X by 64, often
 * landing it in another worker's strip and inviting cross-core CAS
 * contention on the op cache line. The horizontal axis is left to the
 * combine phase (combine_horizontally), which merges adjacent COPY columns
 * sharing the same scroll vector into wider rectangles.
 *
 * NOTE: While this function will search for and optimize operations that copy
 * existing data, it can only do so for distinct image data. Multiple
 * operations that copy the same exact data (like a region tiled with multiple
 * copies of some pattern) can only be stored in the table once, and therefore
 * will only match once via the hash path. The extension pass above catches
 * the duplicates that the hash lookup cannot.
 *
 * @param plan
 *     The display plan to update with any copies found.
 *
 * @param x
 *     The X coordinate of the upper-left corner of the 64x64 region currently
 *     being checked.
 *
 * @param y
 *     The Y coordinate of the upper-left corner of the 64x64 region currently
 *     being checked.
 *
 * @param hash
 *     The hash value that applies to the 64x64 rectangle at the given
 *     coordinates.
 *
 * @param closure
 *     A pointer to the guac_display_layer that is being searched.
 */
static void PFR_LFR_guac_display_plan_find_copies(guac_display_plan* plan,
        int x, int y, uint64_t hash, void* closure) {

    guac_display_layer* copy_from_layer = (guac_display_layer*) closure;

    /* Extract the candidate op from ops_by_hash. The extraction is atomic:
     * at most one worker ever receives a non-NULL pointer for a given
     * bucket, so no two hash-path workers can claim the same op. */
    guac_display_plan_operation* op = guac_display_plan_remove_indexed_op(plan, hash);
    if (op == NULL)
        return;

    guac_display_layer* copy_to_layer = op->layer;

    guac_rect src_rect;
    guac_rect_init(&src_rect, x, y, GUAC_DISPLAY_CELL_SIZE, GUAC_DISPLAY_CELL_SIZE);

    guac_rect dst_rect;
    guac_display_cell_init_rect(&dst_rect, op->dest.left, op->dest.top);

    const unsigned char* copy_from = GUAC_DISPLAY_LAYER_STATE_CONST_BUFFER(copy_from_layer->last_frame, src_rect);
    const unsigned char* copy_to = GUAC_DISPLAY_LAYER_STATE_CONST_BUFFER(copy_to_layer->pending_frame, dst_rect);

    /* Only transform into a copy if the image data is truly identical (not a collision) */
    if (guac_image_cmp(copy_from, GUAC_DISPLAY_CELL_SIZE, GUAC_DISPLAY_CELL_SIZE,
                copy_from_layer->last_frame.buffer_stride,
                copy_to, GUAC_DISPLAY_CELL_SIZE, GUAC_DISPLAY_CELL_SIZE,
                copy_to_layer->pending_frame.buffer_stride) != 0)
        return;

    /* Atomically transition op->type from IMG to COPY. Extracting op from
     * ops_by_hash above already prevents other hash-path workers from
     * reaching this point for the same op, but an extension-path worker
     * (see try_extend_copy_match) may have already claimed this op from
     * a neighboring initial match. The CAS resolves that race: if it
     * fails, the extension already transformed the op and we yield. */
    int expected = GUAC_DISPLAY_PLAN_OPERATION_IMG;
    if (!guac_display_atomic_cas_int((int*) &op->type, &expected,
                GUAC_DISPLAY_PLAN_OPERATION_COPY))
        return;

    op->src.layer_rect.layer = copy_from_layer->last_frame_buffer;
    op->src.layer_rect.rect = src_rect;
    op->dest = dst_rect;

    /* Signal to the abort-check in guac_hash_foreach_image_rect() that the
     * search has now justified its cost. Relaxed is sufficient - the flag
     * is used only by the "is anyone waiting?" heuristic and neither
     * synchronizes data across threads nor requires prompt visibility. */
    atomic_store_explicit(&plan->copies_found, 1, memory_order_relaxed);

    /* Walk vertically away from the confirmed match along the scroll
     * vector, claiming each cell-adjacent neighbor that trivially matches
     * via guac_image_cmp(). Each loop terminates as soon as a neighbor
     * fails to match (out of bounds, already transformed by another
     * worker, or scroll vector no longer holds). The horizontal axis is
     * deliberately left to combine_horizontally - extending here would
     * shift the source X across worker strip boundaries and cause
     * cross-core CAS contention on op cache lines. */
    int src_origin_x = x;
    int src_origin_y = y;
    int dst_origin_x = dst_rect.left;
    int dst_origin_y = dst_rect.top;

    for (int j = 1; ; j++) {
        int offset = j * GUAC_DISPLAY_CELL_SIZE;
        if (!try_extend_copy_match(copy_from_layer, copy_to_layer,
                    src_origin_x, src_origin_y + offset,
                    dst_origin_x, dst_origin_y + offset))
            break;
    }

    for (int j = 1; ; j++) {
        int offset = j * GUAC_DISPLAY_CELL_SIZE;
        if (!try_extend_copy_match(copy_from_layer, copy_to_layer,
                    src_origin_x, src_origin_y - offset,
                    dst_origin_x, dst_origin_y - offset))
            break;
    }

}

void PFR_LFR_guac_display_plan_search_chunk(guac_display_plan* plan,
        guac_display_layer* layer, const guac_rect* sub_rect) {

    /* Pass the occupancy bitmap as a filter so the inner loop can skip
     * the find_copies call entirely for empty buckets. With a typical
     * bucket fill ratio around 3% for a full 4K frame, this eliminates
     * ~97% of function calls and the L3-latency atomic load they would
     * otherwise incur.
     *
     * The hash algorithm used by guac_hash_foreach_image_rect() is a
     * correct 64x64 sliding window hash: pixel/row contributions older
     * than 64 positions vanish automatically because 62^64 is zero
     * modulo 2^64 (62 = 2 * 31, 2^64 is the hash's modulus). Each
     * strip can therefore start with row_hash = 0 and cell_hash[] = 0
     * at its leftmost column / topmost row; after 64 iterations in
     * each dimension the rolling-away of the initial zeros leaves the
     * hash identical to what the sequential algorithm would produce at
     * the same (x, y) position. Concurrent invocations on disjoint
     * (layer, sub_rect) chunks therefore require no coordination
     * beyond the atomic claim inside
     * guac_display_plan_remove_indexed_op(). */
    guac_hash_foreach_image_rect(plan, &layer->last_frame,
            sub_rect, PFR_LFR_guac_display_plan_find_copies, layer,
            plan->ops_by_hash_occupancy);

}

guac_display_plan_search_rect* PFR_guac_display_plan_build_search_chunks(
        guac_display_plan* plan, int strips_per_layer, int* count_out) {

    *count_out = 0;
    if (strips_per_layer < 1)
        strips_per_layer = 1;

    guac_display* display = plan->display;

    /* Count layers with search_for_copies to size the chunk list */
    int layer_count = 0;
    for (guac_display_layer* current = display->last_frame.layers;
            current != NULL; current = current->last_frame.next) {
        if (current->pending_frame.search_for_copies)
            layer_count++;
    }

    if (layer_count == 0)
        return NULL;

    int max_chunks = layer_count * strips_per_layer;
    guac_display_plan_search_rect* chunks = guac_mem_alloc(max_chunks,
            sizeof(guac_display_plan_search_rect));
    int chunk_count = 0;

    for (guac_display_layer* current = display->last_frame.layers;
            current != NULL; current = current->last_frame.next) {

        if (!current->pending_frame.search_for_copies)
            continue;

        guac_rect search_region;
        guac_rect_init(&search_region, 0, 0,
                current->last_frame.width, current->last_frame.height);

        /* Avoid excessive computation by restricting the search region to
         * only the area that was changed in the upcoming frame (in the
         * case of scrolling, absolutely all data relevant to the scroll
         * will have been modified). */
        guac_rect_constrain(&search_region, &current->pending_frame.dirty);

        int width = search_region.right - search_region.left;
        int height = search_region.bottom - search_region.top;

        /* A valid 64x64 window cannot exist in a search region smaller
         * than GUAC_DISPLAY_CELL_SIZE in either dimension. Such a region
         * yields zero emit positions, so skip it entirely. */
        if (width < GUAC_DISPLAY_CELL_SIZE || height < GUAC_DISPLAY_CELL_SIZE)
            continue;

        /* The number of emit columns this search will produce. This is
         * also the total work this layer contributes, measured in "emit
         * positions per row". */
        int emit_width = width - GUAC_DISPLAY_CELL_SIZE + 1;

        /* Choose the number of strips for this layer. Avoid splitting
         * into chunks so narrow that the per-strip halo dominates the
         * useful work: a strip should be at least GUAC_DISPLAY_CELL_SIZE
         * wide (giving halo-to-useful-work ratio of ~1:1) before further
         * subdivision is worthwhile. */
        int max_strips_by_width = emit_width / GUAC_DISPLAY_CELL_SIZE;
        if (max_strips_by_width < 1)
            max_strips_by_width = 1;

        int layer_strips = strips_per_layer;
        if (layer_strips > max_strips_by_width)
            layer_strips = max_strips_by_width;

        int strip_emit_width = (emit_width + layer_strips - 1) / layer_strips;

        for (int s = 0; s < layer_strips; s++) {

            int emit_left = search_region.left + s * strip_emit_width;
            int emit_right = emit_left + strip_emit_width;
            if (emit_right > search_region.left + emit_width)
                emit_right = search_region.left + emit_width;

            if (emit_right <= emit_left)
                continue;

            guac_display_plan_search_rect* chunk = &chunks[chunk_count++];
            chunk->layer = current;
            chunk->sub_rect.top = search_region.top;
            chunk->sub_rect.bottom = search_region.bottom;
            chunk->sub_rect.left = emit_left;

            /* Right edge includes 63 pixels of halo so the last emit
             * column can complete its row_hash. Clamp to the search
             * region's right edge - at the rightmost strip of a layer,
             * the halo naturally fits within the search region because
             * emit_right already accounts for CELL_SIZE - 1 of padding
             * (emit_width = width - CELL_SIZE + 1). */
            chunk->sub_rect.right = emit_right + GUAC_DISPLAY_CELL_SIZE - 1;
            if (chunk->sub_rect.right > search_region.right)
                chunk->sub_rect.right = search_region.right;

        }

    }

    if (chunk_count == 0) {
        guac_mem_free(chunks);
        return NULL;
    }

    *count_out = chunk_count;
    return chunks;

}
