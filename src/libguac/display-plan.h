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

#ifndef GUAC_DISPLAY_PLAN_H
#define GUAC_DISPLAY_PLAN_H

#include "guacamole/display.h"
#include "guacamole/flag.h"
#include "guacamole/rect.h"
#include "guacamole/timestamp.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>

/**
 * The width of an update which should be considered negible and thus
 * trivial overhead compared to the cost of two updates.
 */
#define GUAC_DISPLAY_NEGLIGIBLE_WIDTH 64

/**
 * The height of an update which should be considered negible and thus
 * trivial overhead compared to the cost of two updates.
 */
#define GUAC_DISPLAY_NEGLIGIBLE_HEIGHT 64

/**
 * The proportional increase in cost contributed by transfer and processing of
 * image data, compared to processing an equivalent amount of client-side
 * data.
 */
#define GUAC_DISPLAY_DATA_FACTOR 128

/**
 * The maximum width or height to allow when combining any pair of rendering
 * operations into a single operation, in pixels, as the exponent of a power of
 * two. This value is intended to be large enough to avoid unnecessarily
 * increasing the number of drawing operations, yet also small enough to allow
 * larger updates to be easily parallelized via the worker threads.
 *
 * The current value of 9 means that each encoded image will be no larger than
 * 512x512 pixels.
 */
#define GUAC_DISPLAY_MAX_COMBINED_SIZE 9

/**
 * The base cost of every update. Each update should be considered to have
 * this starting cost, plus any additional cost estimated from its
 * content.
 */
#define GUAC_DISPLAY_BASE_COST 4096

/**
 * An increase in cost is negligible if it is less than
 * 1/GUAC_DISPLAY_NEGLIGIBLE_INCREASE of the old cost.
 */
#define GUAC_DISPLAY_NEGLIGIBLE_INCREASE 4

/**
 * The framerate which, if exceeded, indicates that JPEG is preferred.
 */
#define GUAC_DISPLAY_JPEG_FRAMERATE 3

/**
 * Minimum JPEG bitmap size (area). If the bitmap is smaller than this threshold,
 * it should be compressed as a PNG image to avoid the JPEG compression tax.
 */
#define GUAC_DISPLAY_JPEG_MIN_BITMAP_SIZE 4096

/**
 * The JPEG compression min block size, as the exponent of a power of two. This
 * defines the optimal rectangle block size factor for JPEG compression.
 * Usually 8x8 would suffice, but we use 16x16 here to reduce the occurrence of
 * ringing artifacts further.
 */
#define GUAC_SURFACE_JPEG_BLOCK_SIZE 4

/**
 * The WebP compression min block size, as the exponent of a power of two. This
 * defines the optimal rectangle block size factor for WebP compression. WebP
 * does utilize variable block size, but ensuring a block size factor reduces
 * any noise on the image edges.
 */
#define GUAC_SURFACE_WEBP_BLOCK_SIZE 3

/**
 * The number of hash buckets within each guac_display_plan.
 */
#define GUAC_DISPLAY_PLAN_OPERATION_INDEX_SIZE 0x10000

/**
 * Hash function which hashes a larger, 64-bit hash into a 16-bit hash that
 * will fit within GUAC_DISPLAY_PLAN_OPERATION_INDEX_SIZE. Note that the random
 * distribution of this hash relies entirely on the random distribution of the
 * value being hashed.
 */
#define GUAC_DISPLAY_PLAN_OPERATION_HASH(hash) (\
          ( hash        & 0xFFFF)               \
        ^ ((hash >> 16) & 0xFFFF)               \
        ^ ((hash >> 32) & 0xFFFF)               \
        ^ ((hash >> 48) & 0xFFFF)               \
        )

/**
 * The type of a graphical operation that may be part of a guac_display_plan.
 */
typedef enum guac_display_plan_operation_type {

    /**
     * Do nothing (no-op).
     */
    GUAC_DISPLAY_PLAN_OPERATION_NOP = 0,

    /**
     * Copy image data from the associated source rect to the destination rect.
     * The source and destination layers are not necessarily the same.
     */
    GUAC_DISPLAY_PLAN_OPERATION_COPY,

    /**
     * Fill a rectangular region of the destination layer with the source
     * color.
     */
    GUAC_DISPLAY_PLAN_OPERATION_RECT,

    /**
     * Draw arbitrary image data to the destination rect.
     */
    GUAC_DISPLAY_PLAN_OPERATION_IMG

} guac_display_plan_operation_type;

/**
 * Completion state shared by all chunks of a single parallel-for dispatch.
 * The structure is created on the stack of the dispatching thread, referenced
 * by each chunk's guac_display_plan_task carried on parallel_stage.fifo, and
 * destroyed after the dispatching thread observes that all chunks have
 * finished.
 */
typedef struct guac_display_parallel_task_state {

    /**
     * Mutex guarding access to the remaining-chunks counter.
     */
    pthread_mutex_t lock;

    /**
     * The number of chunks that have NOT yet been completed. Decremented
     * under lock by each worker as it finishes its chunk.
     */
    int remaining;

    /**
     * Flag raised by the worker that completes the final chunk. The
     * dispatching thread waits on this flag before returning.
     */
    guac_flag done;

} guac_display_parallel_task_state;

/**
 * Descriptor of a single parallel-for chunk. Each item enqueued onto the
 * parallel_stage's FIFO is one of these, describing the half-open index
 * range [start, end) that the parallel worker dequeuing it should process.
 */
typedef struct guac_display_plan_task {

    /**
     * The function to invoke on the given index range.
     */
    void (*func)(void* context, int start, int end);

    /**
     * Opaque context pointer passed through to the invoked function. The
     * dispatching thread is responsible for ensuring the pointed-to data
     * outlives the parallel-for dispatch.
     */
    void* context;

    /**
     * The inclusive starting index of the chunk assigned to this op.
     */
    int start;

    /**
     * The exclusive ending index of the chunk assigned to this op.
     */
    int end;

    /**
     * Shared completion state for all chunks of the parallel-for dispatch.
     */
    guac_display_parallel_task_state* state;

} guac_display_plan_task;

/**
 * Per-RECT-op metadata: the recovered color and (for translucent rects)
 * the pair of sample pixels from which the (α, src) derivation was
 * computed. The samples are retained so that the combine phase can
 * re-derive a merged (α, src) when considering whether two adjacent
 * translucent rects can be combined into one - the per-rect derivations
 * are otherwise final, and there's no way to recover them from the
 * packed color alone.
 */
typedef struct guac_display_plan_rect_data {

    /**
     * Packed ARGB color (alpha in the high byte). For solid rects (the
     * existing single-uniform-color path), alpha is 0xFF. For translucent
     * overlay rects, alpha < 0xFF and the rect is composited source-over
     * with the existing destination pixels.
     */
    uint32_t color;

    /**
     * Pending- and last-frame ARGB values of sample pixel A for the
     * translucent overlay derivation - the top-left pixel of the rect at
     * the time of detection. Only meaningful when alpha < 0xFF; left as
     * zero on solid rects.
     */
    uint32_t pending_a;
    uint32_t last_a;

    /**
     * Pending- and last-frame ARGB values of sample pixel B - the
     * maximum-channel-delta partner of A within the rect, chosen to
     * maximize α-derivation precision. Only meaningful when alpha <
     * 0xFF; left as zero on solid rects.
     */
    uint32_t pending_b;
    uint32_t last_b;

} guac_display_plan_rect_data;

/**
 * A reference to a rectangular region of image data within a layer of the
 * remote Guacamole display.
 */
typedef struct guac_display_plan_layer_rect {

    /**
     * The rectangular region that should serve as source data for an
     * operation.
     */
    guac_rect rect;

    /**
     * The layer that the source data is coming from.
     */
    const guac_layer* layer;

} guac_display_plan_layer_rect;

/**
 * Any one of several operations that may be contained in a guac_display_plan.
 */
typedef struct guac_display_plan_operation {

    /**
     * The destination layer (recipient of graphical output/changes).
     */
    guac_display_layer* layer;

    /**
     * The operation being performed on the destination layer.
     */
    guac_display_plan_operation_type type;

    /**
     * The location within the destination layer that will receive these
     * changes.
     */
    guac_rect dest;

    /**
     * The approximate number of pixels that have actually changed as a result
     * of this operation. This value will not necessarily be the same as the
     * area of the destination rect if some pixels remain unchanged.
     */
    size_t dirty_size;

    /**
     * The lossy compression quality (between 0 and 100 inclusive) that should
     * be used for this operation if lossy encoding is selected. This is
     * computed once per frame during guac_display_plan_apply() to avoid having
     * each worker thread repeatedly acquire the user rwlock and iterate
     * connected users to compute client-side processing lag.
     */
    int lossy_quality;

    /**
     * The target encoded size in bytes for this operation when the chosen
     * encoder supports direct size targeting (currently WebP, via
     * libwebp's WebPConfig::target_size). Derived from the per-frame byte
     * budget proportionally to this op's pixel area, so larger updates
     * get a proportionally larger slice of the budget. A value of 0
     * disables size targeting and falls back to pure quality-based
     * encoding - this is the state when no throughput data is available
     * (e.g. the first few frames of a connection).
     *
     * Computed once per frame during guac_display_plan_apply(), alongside
     * lossy_quality, and attached to every IMG op before worker threads
     * pick them up.
     */
    int target_bytes;

    /**
     * The timestamp of the last frame that made any change within the
     * destination rect of the destination layer.
     */
    guac_timestamp last_frame;

    /**
     * The timestamp of the change being made. This will be the timestamp of
     * the frame at the time the frame was ended, not the timestamp of the
     * server at the time this operation was added to the plan.
     */
    guac_timestamp current_frame;

    union {

        /**
         * The color that should be used to fill the destination rect, plus
         * (for translucent rects) the sample pixels retained for the
         * combine phase to use when considering whether to merge with
         * adjacent translucent rects. Applies only to
         * GUAC_DISPLAY_PLAN_OPERATION_RECT operations.
         */
        guac_display_plan_rect_data rect;

        /**
         * The rectangle that should be copied to the destination rect. This
         * value applies only to GUAC_DISPLAY_PLAN_OPERATION_COPY operations.
         */
        guac_display_plan_layer_rect layer_rect;

    } src;

} guac_display_plan_operation;

/**
 * A guac_display_plan_operation that has been hashed and stored within a
 * guac_display_plan.
 */
typedef struct guac_display_plan_indexed_operation {

    /**
     * The operation.
     */
    guac_display_plan_operation* op;

    /**
     * The hash value associated with the operation. This hash value is derived
     * from the actual image contents of the region that was changed, using the
     * new contents of that region. The intent of this hash is to allow
     * operations to be quickly located based on the output they will produce,
     * such that image draw operations can be automatically replaced with
     * simple copies if they reuse data from elsewhere in a layer.
     */
    uint64_t hash;

} guac_display_plan_indexed_operation;

/**
 * The set of operations required to transform the display state from what each
 * user currently sees (the previous frame) to the current state of the
 * guac_display (the current frame). The operations within a plan are quickly
 * generated based on simple image comparisons, and are then refined by an
 * optimizer based on estimated costs.
 */
typedef struct guac_display_plan {

    /**
     * The display that this plan was created for.
     */
    guac_display* display;

    /**
     * The time that the frame ended.
     */
    guac_timestamp frame_end;

    /**
     * Array of all operations that should be applied, in order. The operations
     * in this array do not overlap nor depend on each other. They may be
     * safely reordered without any impact on the image that results from
     * applying those operations.
     */
    guac_display_plan_operation* ops;

    /**
     * The number of operations stored in the ops array.
     */
    size_t length;

    /**
     * Index of operations in the plan by their image contents. Only operations
     * that can be easily stored without collisions will be represented here.
     */
    guac_display_plan_indexed_operation ops_by_hash[GUAC_DISPLAY_PLAN_OPERATION_INDEX_SIZE];

    /**
     * Compact occupancy bitmap parallel to ops_by_hash: one bit per bucket,
     * set by the index phase when a bucket is populated and read by the
     * search phase as a fast-path filter.
     *
     * With 65536 buckets and a typical plan filling ~2000 of them, 97%+ of
     * search-phase hash lookups miss. Each miss would otherwise cost an
     * atomic load from the 1 MB ops_by_hash table (L2/L3 latency). The 8 KB
     * bitmap fits in L1 cache per-core, so the fast-path check on empty
     * buckets stays L1-resident and avoids touching the large table at all.
     *
     * Bit index `i` lives at `ops_by_hash_occupancy[i >> 3] & (1 << (i & 7))`.
     */
    uint8_t ops_by_hash_occupancy[GUAC_DISPLAY_PLAN_OPERATION_INDEX_SIZE / 8];

    /**
     * Set to non-zero by guac_display_plan_find_copies() the first time it
     * successfully transitions an IMG op to a COPY op. Read by the
     * abort-check in guac_hash_foreach_image_rect() to determine whether
     * the search has produced any matches yet - if it has not, and another
     * thread is blocked waiting on the display's pending_frame lock, the
     * search is aborted so the waiting thread can proceed sooner.
     *
     * Concurrently written by worker threads during the search phase, so
     * must be atomically accessed.
     */
    _Atomic int copies_found;

    /**
     * Set to non-zero by any worker thread in the search phase that
     * observes both (a) no copies found yet and (b) another thread blocked
     * on the display's pending_frame lock. Once set, all workers still
     * iterating guac_hash_foreach_image_rect() return immediately at the
     * next abort-check point. This is the single coordinated "bail out
     * now" signal; the contention check only has to fire once per search
     * before every worker drains.
     *
     * Reset to zero at the start of each search (see
     * guac_display_plan_run_search() in display-flush.c).
     */
    _Atomic int search_aborted;

    /**
     * Upper bound (in pixels) on the area of any single IMG op produced
     * by the combine phase. Computed once at the start of
     * PFW_guac_display_plan_combine_horizontally() as
     * max(floor, total_img_area / encode_workers) and then read by
     * guac_display_plan_should_combine() to reject IMG+IMG combines
     * whose combined rect area would exceed the target. The effect is
     * to keep per-op encode work roughly balanced across the encode
     * worker pool: on large-area frames the existing 512x512
     * crosses-boundary cap dominates; on small frames with many
     * workers this target shrinks below the cap and prevents
     * collapsing the plan down to fewer ops than workers can
     * process in parallel.
     */
    size_t img_combine_target_area;

    /**
     * Atomic cursor used by the build stage workers to claim unique
     * slots in the ops array as they walk their cell-row ranges.
     * Each dirty cell corresponds to exactly one op; the worker that
     * encounters the cell does a single fetch_add to get its output
     * index, then writes the op there. After all build chunks finish,
     * next_op_index equals plan->length.
     *
     * Ops land in plan->ops in whatever order workers happen to claim
     * them (not cell-scan order). This is correct because the
     * consumers of plan->ops - combine (iterates cells, not ops),
     * search (hash-indexed), plan_apply (order-independent because
     * ops are non-overlapping regions) - don't depend on ops being
     * in any particular order.
     */
    _Atomic size_t next_op_index;

    /**
     * Array of pointers to the layers that had dirty cells at the
     * time plan_create ran pass 1. Used by the draft handler to fan
     * out build chunks only to layers that actually have work -
     * otherwise every frame that touches a single layer would still
     * fan out useless chunks across every connected display layer.
     *
     * Allocated in plan_create sized to the number of dirty layers;
     * freed by plan_free.
     */
    guac_display_layer** dirty_layers;

    /**
     * Number of entries in dirty_layers.
     */
    int dirty_layer_count;

} guac_display_plan;

/**
 * Creates a new guac_display_plan representing the changes necessary to
 * transform the current remote display state seen by each connected user (the
 * previous frame) to the current local display state represented by the
 * guac_display (the current frame). The actual operations within the plan are
 * chosen based on the result of passing the naive set of operations through an
 * optimizer.
 *
 * There are cases where no plan will be generated. If no changes have occurred
 * since the last frame, or if the last frame is still being encoded by the
 * guac_display, NULL is returned. In the event that NULL is returned but
 * changes have been made, those changes will eventually be automatically
 * picked up after the currently-pending frame has finished encoded.
 *
 * The returned guac_display_plan must eventually be manually freed by a call
 * to guac_display_plan_free().
 *
 * IMPORTANT: The calling thread must already hold the write lock for the
 * display's pending_state, and must at least hold the read lock for the
 * display's last_frame_lock.
 *
 * @param display
 *     The guac_display to create a plan for.
 *
 * @return
 *     A newly-allocated guac_display_plan representing the changes necessary
 *     to transform the current remote display state to that of the local
 *     guac_display, or NULL if no plan could be created. If non-NULL, this
 *     value must eventually be freed by a call to guac_display_plan_free().
 */
guac_display_plan* PFW_LFR_guac_display_plan_create(guac_display* display);

/**
 * Frees all memory associated with the given guac_display_plan.
 *
 * @param plan
 *     The plan to free.
 */
void guac_display_plan_free(guac_display_plan* plan);

/**
 * Examines a single plan operation and, if it is an IMG op whose
 * destination region contains only a single uniform color (solid rect)
 * or can be expressed as a single translucent rect composited over
 * last_frame, rewrites it in place to a GUAC_DISPLAY_PLAN_OPERATION_RECT
 * op. Ops of any other type, or IMG ops that do not collapse to a
 * single-color representation, are left unchanged.
 *
 * Safe to call concurrently on different ops (each call touches only
 * the op passed to it and the pending/last frame buffers of op->layer,
 * which are read-only here).
 *
 * @param op
 *     The operation to examine and possibly rewrite.
 */
void PFR_guac_display_plan_rewrite_op_as_rect(guac_display_plan_operation* op);

/**
 * Inserts the given op's 64x64 cell hash into the plan's ops_by_hash
 * table (and its occupancy bitmap). No-op if the op is not an IMG op,
 * if the op's destination cell does not fit within the pending_frame
 * bounds, or if the cell is not exactly GUAC_DISPLAY_CELL_SIZE in each
 * dimension (hashing only applies to full 64x64 cells).
 *
 * Concurrent writes to the shared ops_by_hash table are coordinated by
 * the atomic CAS inside guac_display_plan_store_indexed_op(), so it is
 * safe to invoke this function from multiple threads on different ops.
 *
 * The caller is responsible for ensuring ops_by_hash and
 * ops_by_hash_occupancy are zero-initialized before the first call for
 * the plan (guac_display_plan_create() zeroes the whole plan struct
 * at allocation time, so this is a no-op for the normal path). Any
 * reader of the index (copy search) must not run until every call to
 * this function for every op of the plan has completed, as a partial
 * index can only produce missed copy detections, not incorrect ones.
 *
 * @param plan
 *     The plan whose ops_by_hash table should receive this op.
 *
 * @param op
 *     The operation whose hash should be inserted.
 */
void PFR_guac_display_plan_index_op(guac_display_plan* plan,
        guac_display_plan_operation* op);

/**
 * Scans a single (layer, sub_rect) slice of the scroll/copy search, looking
 * up each 64x64 hash in the plan's already-populated ops_by_hash table and
 * rewriting any matching IMG ops as COPY ops. The caller guarantees that
 * every index chunk (see guac_display_plan_index_dirty_cells) has completed
 * before this function is invoked.
 *
 * Workers coordinate ownership of matched ops via the CAS inside
 * guac_display_plan_remove_indexed_op(), so concurrent invocations on
 * disjoint (layer, sub_rect) pairs require no external synchronization.
 *
 * @param plan
 *     The guac_display_plan being refined.
 *
 * @param layer
 *     The layer whose last_frame buffer is being searched. The layer's
 *     last_frame contents are read; matched ops from its pending_frame
 *     are rewritten.
 *
 * @param sub_rect
 *     The sub-rectangle of last_frame to scan, including the 63-pixel
 *     right-hand halo required to let the rolling 2D hash slide into
 *     the final emit column of this strip.
 */
void PFR_LFR_guac_display_plan_search_chunk(guac_display_plan* plan,
        guac_display_layer* layer, const guac_rect* sub_rect);

/**
 * Descriptor of a single (layer, sub_rect) chunk of the copy-rewrite
 * search phase. Emitted by guac_display_plan_build_search_chunks() and
 * consumed by PFR_LFR_guac_display_plan_search_chunk().
 */
typedef struct guac_display_plan_search_rect {

    /**
     * The layer whose last_frame buffer is being searched.
     */
    guac_display_layer* layer;

    /**
     * The sub-rect to scan. The left edge is the first emit column of
     * this strip; the right edge includes GUAC_DISPLAY_CELL_SIZE - 1
     * pixels of halo so the rolling 2D hash can slide into the final
     * emit column.
     */
    guac_rect sub_rect;

} guac_display_plan_search_rect;

/**
 * Builds the list of (layer, sub_rect) chunks that the copy-search phase
 * should process for the given plan. Only layers with pending_frame.
 * search_for_copies set are considered, and each qualifying layer is
 * split into up to strips_per_layer horizontal strips. This function
 * must only be called after the pending-to-last commit has happened, as
 * it reads layer->last_frame (width/height) for the search region.
 *
 * @param plan
 *     The guac_display_plan whose search-phase chunks should be built.
 *
 * @param strips_per_layer
 *     The target upper bound on horizontal strips per layer. Should
 *     typically match the number of workers that will process the
 *     returned chunks.
 *
 * @param count_out
 *     Output: the number of chunks written to the returned array.
 *
 * @return
 *     A newly-allocated array of chunks of length *count_out, or NULL
 *     if no chunks could be produced. The caller owns the array and
 *     must eventually free it with guac_mem_free().
 */
guac_display_plan_search_rect* PFR_guac_display_plan_build_search_chunks(
        guac_display_plan* plan, int strips_per_layer, int* count_out);

/**
 * Walks through all operations currently in the given guac_display_plan,
 * combining horizontally-adjacent operations wherever doing so appears to be
 * more efficient than performing those operations separately.
 *
 * @param plan
 *     The guac_display_plan to modify.
 */
void PFW_guac_display_plan_combine_horizontally(guac_display_plan* plan);

/**
 * Walks through all operations currently in the given guac_display_plan,
 * combining vertically-adjacent operations wherever doing so appears to be
 * more efficient than performing those operations separately.
 *
 * @param plan
 *     The guac_display_plan to modify.
 */
void PFW_guac_display_plan_combine_vertically(guac_display_plan* plan);

/**
 * Stamps lossy_quality and target_bytes onto every op in the given plan
 * and immediately emits protocol instructions for all COPY and RECT ops.
 * IMG ops are stamped but NOT enqueued here - they're queued onto the
 * encode stage FIFO by guac_display_plan_apply_enqueue_img() after the
 * pending-to-last pixel commit has finished.
 *
 * Emitting COPY/RECT protocol at this point - before the expensive
 * pending-to-last memcpy - lets the kernel drain their bytes to the
 * client over the network while the commit handler runs the memcpy on
 * the CPU. The commit stage thread does one or the other at a time, but
 * the NIC transmits asynchronously.
 *
 * The caller must have already run guac_display_plan_combine_*() on
 * the plan so op merging is final before protocol is emitted.
 *
 * @param plan
 *     The guac_display_plan whose COPY/RECT operations should be emitted
 *     inline and whose ops should be stamped with encoding parameters.
 */
void guac_display_plan_apply_emit(guac_display_plan* plan);

/**
 * Maps a bytes-per-pixel budget onto a WebP / JPEG quality setting
 * (30-90) via a log-linear mapping calibrated against the WebP
 * rate-distortion curve - doubling the byte budget adds roughly
 * 18 quality points. Callers use this to pick a quality that's
 * likely to produce output near their target byte count WITHOUT
 * needing the encoder's own target_size machinery (which requires
 * WebPConfig::pass > 1 and costs proportional encode time).
 *
 * Used at plan level (frame-wide quality stamp) and at worker
 * level (per-op quality re-derivation against the remaining per-
 * frame byte pool).
 *
 * @param bpp
 *     The target bytes-per-pixel budget, real-valued.
 *
 * @return
 *     A quality setting in [30, 90].
 */
int guac_display_quality_for_budget(double bpp);

/**
 * Enqueues all IMG operations from the given plan onto the encode stage
 * FIFO. The caller must have previously invoked
 * guac_display_plan_apply_emit() so each op carries its lossy_quality
 * and target_bytes values, and must have finished the pending-to-last
 * buffer commit so encode workers can safely read layer->last_frame.
 * buffer.
 *
 * The encode_stage FIFO is locked across the enqueue loop so workers
 * cannot begin processing IMG ops until every IMG op is queued - this
 * keeps IMG protocol output from interleaving with the COPY/RECT
 * protocol emitted earlier by guac_display_plan_apply_emit().
 *
 * @param plan
 *     The guac_display_plan whose IMG ops should be enqueued onto the
 *     encode stage FIFO.
 *
 * @return
 *     The number of operations enqueued. Callers can use a zero return
 *     to determine that no encode worker will fire as a result of this
 *     plan and that any pipeline flag normally cleared by the end-of-
 *     frame worker must be cleared explicitly.
 */
int guac_display_plan_apply_enqueue_img(guac_display_plan* plan);

#endif
