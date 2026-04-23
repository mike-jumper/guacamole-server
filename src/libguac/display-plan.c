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

#include "display-plan.h"
#include "display-priv.h"
#include "guacamole/assert.h"
#include "guacamole/client.h"
#include "guacamole/display.h"
#include "guacamole/fifo.h"
#include "guacamole/mem.h"
#include "guacamole/protocol.h"
#include "guacamole/socket.h"
#include "guacamole/timestamp.h"

#include <math.h>

#include <string.h>
#include <cairo/cairo.h>

/**
 * Updates the dirty rect in the given cell to note that a horizontal line of
 * image data at the given location and having the given width has changed
 * since the last frame. A provided counter of the overall number of changed
 * cells is updated accordingly.
 *
 * @param layer
 *     The layer that changed.
 *
 * @param cell
 *     The cell containing the line of image data that changed.
 *
 * @param count
 *     A pointer to a counter that contains the current number of cells that
 *     have been marked as having changed since the last frame.
 *
 * @param x
 *     The X coordinate of the leftmost pixel of the horizontal line.
 *
 * @param y
 *     The Y coordinate of the leftmost pixel of the horizontal line.
 *
 * @param width
 *     The width of the line, in pixels.
 */
static inline void guac_display_plan_mark_dirty(guac_display_layer* layer,
        guac_display_layer_cell* cell, size_t* count, int x, int y,
        int width) {

    if (!cell->dirty_size) {
        guac_rect_init(&cell->dirty, x, y, width, 1);
        cell->dirty_size = width;
        (*count)++;
    }

    else {
        guac_rect dirty;
        guac_rect_init(&dirty, x, y, width, 1);
        guac_rect_extend(&cell->dirty, &dirty);
        cell->dirty_size += width;
    }

}

/**
 * Variant of memcmp() which specifically compares series of 32-bit quantities
 * and determines the overall location and length of the differences in the two
 * provided buffers. The length and location determined are the length and
 * location of the smallest contiguous series of 32-bit quantities that differ
 * between the buffers.
 *
 * Declared `inline` so the compiler can propagate constants from hot call
 * sites - in the common case, this function is called with count ==
 * GUAC_DISPLAY_CELL_SIZE, which allows GCC/Clang to emit a straight-line
 * SIMD compare of a fixed 256-byte block rather than an out-of-line call
 * into libc's memcmp.
 *
 * @param buffer_a
 *     The first buffer to compare.
 *
 * @param buffer_b
 *     The buffer to compare with buffer_a.
 *
 * @param count
 *     The number of 32-bit quantities in each buffer.
 *
 * @param pos
 *     A pointer to a size_t that should receive the offset of the difference,
 *     if the two buffers turn out to contain different data. The value of the
 *     size_t will only be modified if at least one difference is found.
 *
 * @return
 *     The number of 32-bit quantities after and including the offset returned
 *     via pos that are different between buffer_a and buffer_b, or zero if
 *     there are no such differences.
 */
static inline size_t guac_display_memcmp(const uint32_t* restrict buffer_a,
        const uint32_t* restrict buffer_b, size_t count, size_t* pos) {

    /* Fast path: detect the common case of identical rows via libc's
     * memcmp, which is always SIMD-vectorized by glibc and inlined by
     * compilers for compile-time-constant sizes. Most rows of a frame are
     * unchanged even within a cell that has been partially modified, so
     * this short-circuit avoids the bulk of the scalar scan below. */
    if (count == 0 || memcmp(buffer_a, buffer_b, count * sizeof(uint32_t)) == 0)
        return 0;

    /* Locate first difference between the buffers. memcmp has already
     * confirmed at least one exists, so no end-of-buffer bounds check is
     * needed to terminate the loop. */
    size_t first = 0;
    while (buffer_a[first] == buffer_b[first])
        first++;

    /* Search backwards from the end for the last difference. Because the
     * first difference is already known, this loop is bounded by 'first'. */
    size_t last = count - 1;
    while (last > first && buffer_a[last] == buffer_b[last])
        last--;

    /* Final difference found - provide caller with the starting offset and
     * length (in 32-bit quantities) of differences */
    *pos = first;
    return last - first + 1;

}

/**
 * Context shared by all chunks of a single plan_create parallel-for dispatch.
 * Each chunk works on a disjoint range of cell-rows within the layer's
 * aligned dirty rect and accumulates its partial op-count into the shared
 * total under op_count_lock.
 */
typedef struct guac_plan_create_context {

    /**
     * The layer whose cells are being updated by the dispatched chunks.
     */
    guac_display_layer* layer;

    /**
     * The cell-aligned dirty rect for the layer, already constrained to the
     * pending frame bounds.
     */
    guac_rect dirty;

    /**
     * Protects concurrent updates to op_count.
     */
    pthread_mutex_t op_count_lock;

    /**
     * Sum of per-chunk op counts across all dispatched chunks of this
     * layer's dispatch.
     */
    size_t op_count;

} guac_plan_create_context;

/**
 * Processes a contiguous range of cell-rows within the given layer's
 * aligned dirty rect, refining each touched cell's per-cell dirty rect and
 * dirty-pixel count to reflect the actual per-pixel differences between the
 * pending and last frames. Returns the number of cells that transitioned
 * from clean to dirty as a result of this call.
 *
 * @param current
 *     The layer to process.
 *
 * @param dirty
 *     The cell-aligned dirty rect for the layer as a whole.
 *
 * @param cell_row_start
 *     The zero-based index of the first cell-row (within dirty) to process.
 *
 * @param cell_row_end
 *     The zero-based index of the cell-row after the last cell-row to
 *     process. Together with cell_row_start, this forms a half-open range.
 *
 * @return
 *     The number of cells that transitioned from clean to dirty.
 */
static size_t plan_create_process_cell_rows(guac_display_layer* current,
        const guac_rect* dirty, int cell_row_start, int cell_row_end) {

    size_t local_op_count = 0;

    int sub_top = dirty->top + cell_row_start * GUAC_DISPLAY_CELL_SIZE;
    int sub_bottom = dirty->top + cell_row_end * GUAC_DISPLAY_CELL_SIZE;
    if (sub_bottom > dirty->bottom)
        sub_bottom = dirty->bottom;
    if (sub_top >= sub_bottom)
        return 0;

    guac_rect sub_dirty = {
        .left = dirty->left,
        .top = sub_top,
        .right = dirty->right,
        .bottom = sub_bottom
    };

    const unsigned char* flushed_row = GUAC_DISPLAY_LAYER_STATE_CONST_BUFFER(current->last_frame, sub_dirty);
    unsigned char* buffer_row = GUAC_DISPLAY_LAYER_STATE_MUTABLE_BUFFER(current->pending_frame, sub_dirty);

    guac_display_layer_cell* cell_row = current->pending_frame_cells
        + guac_mem_ckd_mul_or_die(sub_dirty.top / GUAC_DISPLAY_CELL_SIZE, current->pending_frame_cells_width)
        + sub_dirty.left / GUAC_DISPLAY_CELL_SIZE;

    for (int corner_y = sub_dirty.top; corner_y < sub_dirty.bottom; corner_y += GUAC_DISPLAY_CELL_SIZE) {

        int height = GUAC_DISPLAY_CELL_SIZE;
        if (corner_y + height > sub_dirty.bottom)
            height = sub_dirty.bottom - corner_y;

        /* Iteration through the pending_frame_cells array and the image
         * buffer is a bit complex here, as the pending_frame_cells array
         * contains cells that represent 64x64 regions, while the image
         * buffers contain absolutely all pixels. The outer loop goes
         * through just the pending cells, while the following loop goes
         * through the Y coordinates that make up that cell. */

        for (int y_off = 0; y_off < height; y_off++) {

            /* At this point, we need to loop through the horizontal
             * dimension, comparing the 64-pixel rows of image data in the
             * current line (corner_y + y_off) that are in each applicable
             * cell. We jump forward by one cell for each comparison. */

            int y = corner_y + y_off;

            guac_display_layer_cell* current_cell = cell_row;
            uint32_t* current_flushed = (uint32_t*) flushed_row;
            uint32_t* current_buffer = (uint32_t*) buffer_row;
            for (int corner_x = sub_dirty.left; corner_x < sub_dirty.right; corner_x += GUAC_DISPLAY_CELL_SIZE) {

                int width = GUAC_DISPLAY_CELL_SIZE;
                if (corner_x + width > sub_dirty.right)
                    width = sub_dirty.right - corner_x;

                /* This SHOULD be impossible, as corner_x would need to
                 * somehow be outside the bounds of the dirty rect, which
                 * would have failed the loop condition earlier) */
                GUAC_ASSERT(width >= 0);

                /* NOTE: Extension of the layer-level pending_frame.dirty
                 * rect is hoisted out of this per-row detection loop and
                 * performed once per dirty cell further below, as the
                 * extend operation is idempotent within a single cell. */

                /* Any line that is completely outside the bounds of the
                 * previous frame is dirty (nothing to compare against) */
                if (y >= current->last_frame.height || corner_x >= current->last_frame.width) {
                    guac_display_plan_mark_dirty(current, current_cell, &local_op_count, corner_x, y, width);
                }

                /* All other regions must be processed further to determine
                 * what portion is dirty */
                else {

                    /* Only the pixels that are within the bounds of BOTH
                     * the last_frame and pending_frame are directly
                     * comparable. Others are inherently dirty by virtue of
                     * being outside the bounds of last_frame */
                    int comparable_width = width;
                    if (corner_x + comparable_width > current->last_frame.width)
                        comparable_width = current->last_frame.width - corner_x;

                    /* It is impossible for this value to be negative
                     * because of the last_frame bounds checks that occur
                     * in the if block prior to this else block */
                    GUAC_ASSERT(comparable_width >= 0);

                    /* Any region outside the right edge of the previous frame is dirty */
                    if (width > comparable_width) {
                        guac_display_plan_mark_dirty(current, current_cell, &local_op_count, corner_x + comparable_width, y, width - comparable_width);
                    }

                    /* Mark the relevant region of the cell as dirty if the
                     * current 64-pixel line has changed in any way */
                    size_t length, pos;
                    if ((length = guac_display_memcmp(current_buffer, current_flushed, comparable_width, &pos)) != 0) {
                        guac_display_plan_mark_dirty(current, current_cell, &local_op_count, corner_x + pos, y, length);
                    }

                }

                current_flushed += GUAC_DISPLAY_CELL_SIZE;
                current_buffer += GUAC_DISPLAY_CELL_SIZE;
                current_cell++;

            }

            flushed_row += current->last_frame.buffer_stride;
            buffer_row += current->pending_frame.buffer_stride;

        }

        cell_row += current->pending_frame_cells_width;

    }

    return local_op_count;

}

/**
 * Parallel-for chunk callback. Processes the cell-row range [start, end) of
 * the layer referenced by the shared context and accumulates the partial
 * op-count into the shared total.
 */
static void plan_create_task(void* context, int start, int end) {
    guac_plan_create_context* ctx = (guac_plan_create_context*) context;
    size_t local = plan_create_process_cell_rows(ctx->layer, &ctx->dirty, start, end);

    pthread_mutex_lock(&ctx->op_count_lock);
    ctx->op_count += local;
    pthread_mutex_unlock(&ctx->op_count_lock);
}

/**
 * Minimum number of cell-rows per parallel-for chunk when parallelizing the
 * detection pass of plan_create. Chosen to keep dispatch overhead a small
 * fraction of the per-chunk work. Each cell-row represents 64 image rows.
 */
#define GUAC_DISPLAY_PLAN_CREATE_MIN_CHUNK 2

guac_display_plan* PFW_LFR_guac_display_plan_create(guac_display* display) {

    guac_display_layer* current;
    guac_timestamp frame_end = guac_timestamp_current();

    /* Pre-count pending-frame layers to size the dirty_layers scratch
     * array. One walk here is cheap (tens of layers at most for any
     * reasonable session) and simpler than reallocating as we go. */
    int total_layers = 0;
    for (current = display->pending_frame.layers; current != NULL;
            current = current->pending_frame.next)
        total_layers++;

    guac_display_layer** dirty_layers = NULL;
    int dirty_layer_count = 0;
    if (total_layers > 0)
        dirty_layers = guac_mem_alloc(total_layers, sizeof(guac_display_layer*));

    size_t op_count = 0;

    /* Pass 1: per-layer dirty-cell detection. For each layer that has
     * a non-empty pending_frame.dirty rect and a non-NULL buffer, run
     * the cell-row parallel_for to refine the dirty rect cell-by-cell
     * and count dirty cells. The layer is recorded in dirty_layers
     * for the build stage to later fan out chunks only to layers that
     * actually have work. */
    current = display->pending_frame.layers;
    while (current != NULL) {

        /* Skip processing any layers whose buffers have been replaced with
         * NULL (this is intentionally allowed to ensure references to external
         * buffers can be safely removed if necessary, even before guac_display
         * is freed) */
        if (current->pending_frame.buffer == NULL) {
            GUAC_ASSERT(current->pending_frame.buffer_is_external);
            current = current->pending_frame.next;
            continue;
        }

        /* Check only within layer dirty region, skipping the layer if
         * unmodified. This pass should reset and refine that region, but
         * otherwise rely on proper reporting of modified regions by callers of
         * the open/close layer functions. */
        guac_rect dirty = current->pending_frame.dirty;
        if (guac_rect_is_empty(&dirty)) {
            current = current->pending_frame.next;
            continue;
        }

        /* Flush any outstanding Cairo operations before directly accessing buffer */
        guac_display_layer_cairo_context* cairo_context = &(current->pending_frame_cairo_context);
        if (cairo_context->surface != NULL)
            cairo_surface_flush(cairo_context->surface);

        /* Re-align the dirty rect with nearest multiple of 64 to ensure each
         * step of the dirty rect refinement loop starts at the topmost
         * boundary of a cell */
        guac_rect_align(&dirty, GUAC_DISPLAY_CELL_SIZE_EXPONENT);

        guac_rect pending_frame_bounds = {
            .left = 0,
            .top = 0,
            .right = current->pending_frame.width,
            .bottom = current->pending_frame.height
        };

        /* Limit size of dirty rect by bounds of backing surface for pending
         * frame ONLY (bounds checks against the last frame are performed
         * within the loop such that everything outside the bounds of the last
         * frame is considered dirty) */
        guac_rect_constrain(&dirty, &pending_frame_bounds);

        /* Reset the layer-level dirty rect before the per-cell refinement;
         * it will be rebuilt in the build stage's final aggregation
         * step (the last-finishing build chunk walks plan->ops and
         * extends each op's layer dirty rect). */
        current->pending_frame.dirty = (guac_rect) { 0 };

        /* Partition the dirty rect into cell-row chunks and dispatch to
         * worker threads via the parallel-for primitive. Cell-rows are a
         * natural partition boundary: workers write only into cell state
         * within their assigned rows, and cells in distinct cell-rows are
         * on disjoint cache lines, so there is no false sharing. For
         * small dirty regions or when running with a single worker,
         * guac_display_parallel_for falls back to direct sequential
         * execution on the calling thread. */
        int total_cell_rows = (dirty.bottom - dirty.top + GUAC_DISPLAY_CELL_SIZE - 1)
                / GUAC_DISPLAY_CELL_SIZE;

        guac_plan_create_context ctx = {
            .layer = current,
            .dirty = dirty,
            .op_count = 0,
        };
        pthread_mutex_init(&ctx.op_count_lock, NULL);

        guac_display_parallel_for(display, total_cell_rows,
                GUAC_DISPLAY_PLAN_CREATE_MIN_CHUNK,
                plan_create_task, &ctx);

        pthread_mutex_destroy(&ctx.op_count_lock);

        /* Only record the layer if it actually has dirty cells. A
         * layer may have had a non-empty pending_frame.dirty rect (which
         * is why we ran pass 1 on it) yet produce zero dirty cells if
         * its pixels happen to match last_frame exactly within the
         * claimed region. */
        if (ctx.op_count > 0) {
            dirty_layers[dirty_layer_count++] = current;
            op_count += ctx.op_count;
        }

        current = current->pending_frame.next;

    }

    /* If no layer produced any dirty cells, there is nothing for the
     * build stage to do. Discard the dirty_layers scratch and return
     * NULL so the draft handler skips straight to commit. */
    if (!op_count) {
        guac_mem_free(dirty_layers);
        return NULL;
    }

    /* zalloc, not alloc: ops_by_hash and ops_by_hash_occupancy must be
     * zero-initialized before the index phase, and doing so at alloc
     * time lets that phase run per-chunk without either a shared
     * preamble memset or a synchronization step before workers start. */
    guac_display_plan* plan = guac_mem_zalloc(sizeof(guac_display_plan));
    plan->display = display;
    plan->frame_end = frame_end;
    plan->length = op_count;
    plan->ops = guac_mem_alloc(plan->length, sizeof(guac_display_plan_operation));

    /* Per-frame scratch: dirty_layers array sized to actual count.
     * Freed by plan_free. */
    plan->dirty_layers = dirty_layers;
    plan->dirty_layer_count = dirty_layer_count;

    /* Search-phase preemption state. Reset per plan so the first abort
     * check sees a clean slate; workers will flip these during the
     * scroll-detection search (phase 3). */
    atomic_init(&plan->copies_found, 0);
    atomic_init(&plan->search_aborted, 0);

    /* Build-stage output cursor. Each build chunk does a fetch_add on
     * this to claim its op slot, so ops may appear in plan->ops in
     * any order. No downstream pass (combine, search, plan_apply)
     * depends on plan->ops being in a particular order. */
    atomic_init(&plan->next_op_index, 0);

    /* Ops are NOT populated here - the build stage's chunks walk each
     * dirty layer's cells, claim slots in plan->ops via next_op_index,
     * and fill in the op fields along with the rect and index passes
     * in a single per-cell step. Returning here gives the caller an
     * allocated but unpopulated plan ready for the build stage. */
    return plan;

}

void guac_display_plan_free(guac_display_plan* plan) {
    guac_mem_free(plan->dirty_layers);
    guac_mem_free(plan->ops);
    guac_mem_free(plan);
}

/**
 * Target server-side frame interval (in milliseconds) used to derive a
 * per-frame byte budget from the estimated client throughput. At 33ms
 * (≈30fps), a throughput of 250 bytes/ms corresponds to ~8kB per frame,
 * which is roughly where JPEG quality 70 lands for typical UI content.
 * This target doesn't cap the actual frame rate - it just sets the
 * "budget window" that quality decisions are measured against.
 */
#define GUAC_DISPLAY_TARGET_FRAME_INTERVAL_MS 33

/**
 * Maps a bytes-per-pixel budget (in bytes-per-pixel, real-valued) to a
 * JPEG/WebP quality setting on a continuous logarithmic scale.
 *
 * The scale is anchored at two calibration points:
 *
 *   0.10 bpp  →  quality 30   (minimum we'll emit)
 *   1.00 bpp  →  quality 90   (maximum we'll emit)
 *
 * and runs logarithmically between them, i.e. doubling the budget
 * adds roughly 18 quality points. A log mapping matches the shape of
 * the JPEG/WebP rate-distortion curve - perceptual quality improves
 * roughly linearly with log(bitrate), not with bitrate itself - so a
 * log-scaled quality tracks actual visual fidelity much better than
 * a linear or stepped mapping.
 *
 * Values outside the calibration range clamp at the endpoints (30 and
 * 90), so pathologically slow or fast connections don't drive quality
 * past useful extremes.
 */
int guac_display_quality_for_budget(double bpp) {

    if (bpp <= 0.0)
        return 30;

    /* quality = 30 + 60 · (log10(bpp) - log10(0.1)) / (log10(1.0) - log10(0.1))
     *         = 30 + 60 · (log10(bpp) + 1)
     *         = 90 + 60 · log10(bpp)
     *
     * Equivalent forms follow from log10(1.0)=0 and log10(0.1)=-1. The
     * closed form avoids a division and keeps precision consistent
     * across the range. */
    double quality_d = 90.0 + 60.0 * log10(bpp);
    int quality = (int) (quality_d + 0.5);

    if (quality < 30) quality = 30;
    if (quality > 90) quality = 90;

    return quality;

}

void guac_display_plan_apply_emit(guac_display_plan* plan) {

    guac_display* display = plan->display;
    guac_client* client = display->client;

    /* Derive the per-op encoding parameters for this frame. The
     * strategy depends on whether we have throughput data:
     *
     *   - With throughput: compute a per-pixel byte budget from
     *     (target_interval × slowest-user's bytes/ms) / total-frame-
     *     pixels. Each IMG op then gets a continuous quality value
     *     (logarithmic in bpp, since compression is logarithmic in
     *     bitrate) and a target_bytes = (per-pixel budget × op
     *     pixels). The WebP encoder uses target_bytes directly via
     *     libwebp's WebPConfig::target_size; the JPEG encoder uses
     *     the quality value.
     *
     *   - Without throughput (first frames of a connection, or no
     *     sync yet): fall back to the original lag-based quality
     *     heuristic, target_bytes stays 0 (WebP falls back to
     *     quality-only).
     *
     * One derivation per plan avoids each worker thread independently
     * iterating the user list under rwlock. */
    int throughput = guac_client_get_throughput(client);
    int quality;
    double per_pixel_budget = 0.0;
    int total_pixels = 0;
    int img_ops = 0;
    int byte_budget = 0;

    /* Sum pixel area and count of IMG ops regardless of whether we'll
     * use them for budget - the totals are also useful in the log
     * summary below when throughput data isn't available. COPY/RECT/
     * NOP ops carry no significant byte cost and are excluded. */
    for (int i = 0; i < plan->length; i++) {
        if (plan->ops[i].type == GUAC_DISPLAY_PLAN_OPERATION_IMG) {
            int w = guac_rect_width(&plan->ops[i].dest);
            int h = guac_rect_height(&plan->ops[i].dest);
            total_pixels += w * h;
            img_ops++;
        }
    }

    if (throughput > 0) {

        if (total_pixels > 0) {
            byte_budget =
                GUAC_DISPLAY_TARGET_FRAME_INTERVAL_MS * throughput;
            per_pixel_budget = (double) byte_budget / (double) total_pixels;
            quality = guac_display_quality_for_budget(per_pixel_budget);
        }
        else
            quality = 90;

    }
    else {

        /* No throughput data yet - use the historical lag-based
         * approximation. Preserved so the first frame or two of a
         * connection (or a connection where no sync has been returned
         * yet) still get sensible quality. */
        int lag = guac_client_get_processing_lag(client);
        quality = 90 - (lag - 20);

    }

    if (quality > 90) quality = 90;
    else if (quality < 30) quality = 30;

    /* Plan-level budget/quality decision, one line per frame. Logs
     * the inputs (throughput, total pixels / number of IMG ops) and
     * the outputs (per-frame byte budget, per-pixel budget in bpp
     * milli-units, chosen quality). bpp is reported in milli-bytes-
     * per-pixel to keep an integer format; e.g. 500 = 0.5 bpp. When
     * throughput is zero (no samples yet), per_pixel_budget stays 0
     * and only quality is meaningful. */
    guac_client_log(client, GUAC_LOG_TRACE,
            "Frame encode plan: throughput=%ikB/s img_ops=%i "
            "total_pixels=%i byte_budget=%i bpp=%i quality=%i",
            throughput, img_ops, total_pixels, byte_budget,
            (int) (per_pixel_budget * 1000.0 + 0.5), quality);

    /* Seed the dynamic per-frame byte/pixel budget on the display
     * itself (not the plan - workers pull ops via display->encode_
     * stage.fifo with no plan back-reference, but they all share
     * the display). Workers read these atomics before encoding to
     * compute their share of whatever's left, then subtract their
     * actual bytes / pixels after encoding, so a WebP op that
     * overshoots its initial fair-share naturally tightens the
     * budget for later ops, and an op that undershoots leaves
     * more room for them. */
    atomic_store_explicit(&display->remaining_frame_bytes, byte_budget,
            memory_order_relaxed);
    atomic_store_explicit(&display->remaining_frame_pixels, total_pixels,
            memory_order_relaxed);

    /* Stamp quality/target_bytes onto every op and emit COPY/RECT
     * protocol inline. IMG ops are stamped here but NOT enqueued to
     * encode_stage.fifo yet; they're picked up in _enqueue_img after
     * frame_complete_buffer has finished pending-to-last memcpy.
     * Emitting COPY/RECT this early lets the kernel drain their bytes
     * to the client over the network while the commit handler runs
     * the pixel memcpy. */
    guac_display_plan_operation* op = plan->ops;
    for (int i = 0; i < plan->length; i++) {

        op->lossy_quality = quality;

        /* Proportionally allocate the per-frame byte budget to each IMG
         * op based on its pixel area. Workers hand this to encoders
         * that support direct size targeting (currently WebP via
         * libwebp's WebPConfig::target_size). When no per-pixel budget
         * exists (no throughput data yet), target_bytes stays 0 and
         * encoders fall back to quality-only mode. */
        if (per_pixel_budget > 0.0
                && op->type == GUAC_DISPLAY_PLAN_OPERATION_IMG) {
            int pixels = guac_rect_width(&op->dest)
                       * guac_rect_height(&op->dest);
            op->target_bytes = (int) (per_pixel_budget * pixels + 0.5);
        }
        else
            op->target_bytes = 0;

        guac_display_layer* display_layer = op->layer;
        switch (op->type) {

            case GUAC_DISPLAY_PLAN_OPERATION_COPY:
                guac_protocol_send_copy(client->socket, op->src.layer_rect.layer,
                        op->src.layer_rect.rect.left, op->src.layer_rect.rect.top,
                        guac_rect_width(&op->src.layer_rect.rect), guac_rect_height(&op->src.layer_rect.rect),
                        GUAC_COMP_OVER, display_layer->layer, op->dest.left, op->dest.top);
                break;

            case GUAC_DISPLAY_PLAN_OPERATION_RECT: {

                guac_protocol_send_rect(client->socket, display_layer->layer,
                        op->dest.left, op->dest.top, guac_rect_width(&op->dest), guac_rect_height(&op->dest));

                int alpha = (op->src.rect.color & 0xFF000000) >> 24;
                int red   = (op->src.rect.color & 0x00FF0000) >> 16;
                int green = (op->src.rect.color & 0x0000FF00) >> 8;
                int blue  = (op->src.rect.color & 0x000000FF);

                /* Clear before drawing if layer is not opaque (transparency
                 * will not be copied correctly otherwise) */
                if (!display_layer->opaque) {
                    guac_protocol_send_cfill(client->socket, GUAC_COMP_ROUT, display_layer->layer, 0x00, 0x00, 0x00, 0xFF);
                    guac_protocol_send_cfill(client->socket, GUAC_COMP_OVER, display_layer->layer, red, green, blue, alpha);
                }

                /* Honor whatever alpha was stored in op->src.rect.color: the
                 * rects pass sets it to 0xFF for fully-opaque single-color
                 * rects (preserving the prior behavior) and to a derived
                 * value < 0xFF for translucent-overlay rects, which
                 * COMP_OVER then source-over-blends with the existing
                 * pixels of the opaque destination - exactly the behavior
                 * such overlays require. */
                else
                    guac_protocol_send_cfill(client->socket, GUAC_COMP_OVER, display_layer->layer, red, green, blue, alpha);

                break;
            }

            /* NOP and IMG are handled in _enqueue_img (NOP drops there,
             * IMG goes to encode_stage.fifo). */
            default:
                break;

        }

        op++;

    }

}

int guac_display_plan_apply_enqueue_img(guac_display_plan* plan) {

    guac_display* display = plan->display;
    int enqueued = 0;

    /* The encode_stage.fifo lock is held across the whole enqueue loop
     * so workers cannot interleave IMG protocol emission between our
     * enqueued ops. Since plan_apply_emit has already pushed all COPY/
     * RECT protocol ahead of this point, the resulting on-wire order
     * is: properties (from frame_complete_properties) -> COPY/RECT
     * (from plan_apply_emit) -> IMG (from encode workers) -> sync
     * (from last encode worker). */
    guac_fifo_lock(&display->encode_stage.fifo);

    guac_display_plan_operation* op = plan->ops;
    for (int i = 0; i < plan->length; i++) {

        if (op->type == GUAC_DISPLAY_PLAN_OPERATION_IMG) {
            guac_fifo_enqueue(&display->encode_stage.fifo, op);
            enqueued++;
        }

        op++;

    }

    guac_fifo_unlock(&display->encode_stage.fifo);

    return enqueued;

}
