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
#include "guacamole/flag.h"
#include "guacamole/mem.h"
#include "guacamole/protocol.h"
#include "guacamole/rect.h"
#include "guacamole/rwlock.h"
#include "guacamole/timestamp.h"
#include "guacamole/user.h"

#include <stdatomic.h>
#include <string.h>

/**
 * Begins a section related to an optimization phase that should be tracked for
 * performance at the "trace" log level.
 */
#define GUAC_DISPLAY_PLAN_BEGIN_PHASE()                                       \
    do {                                                                      \
        guac_timestamp phase_start = guac_timestamp_current();

/**
 * Ends a section related to an optimization phase that should be tracked for
 * performance at the "trace" log level. The emitted log line reports the
 * phase's start and end as offsets (in ms) from display->frame_start, so
 * an operator reading consecutive phase lines for a single frame can see
 * whether the phases chain cleanly (next phase's start == prior phase's
 * end) or leave idle gaps that indicate queueing or scheduling delays.
 *
 * @param display
 *     The guac_display related to the optimizations being performed.
 *
 * @param phase
 *     A human-readable name for the optimization phase being tracked.
 *
 * @param n
 *     The ordinal number of this phase relative to other phases, where the
 *     first phase is phase 1.
 *
 * @param total
 *     The total number of optimization phases.
 */
#define GUAC_DISPLAY_PLAN_END_PHASE(display, phase, n, total)                 \
        guac_timestamp phase_end = guac_timestamp_current();                  \
        guac_client_log(display->client, GUAC_LOG_TRACE, "Render planning "   \
                "phase %i/%i (%s): start=+%ims end=+%ims dur=%ims",           \
                n, total, phase,                                              \
                (int) (phase_start - display->frame_start),                   \
                (int) (phase_end - display->frame_start),                     \
                (int) (phase_end - phase_start));                             \
    } while (0)

void guac_display_end_frame(guac_display* display) {
    guac_display_end_multiple_frames(display, 0);
}

/**
 * Callback for guac_client_foreach_user() which sends the current cursor
 * position and button state to any given user except the user that moved the
 * cursor last.
 *
 * @param data
 *     A pointer to the guac_display whose cursor state should be broadcast to
 *     all users except the user that moved the cursor last.
 *
 * @return
 *     Always NULL.
 */
static void* LFR_guac_display_broadcast_cursor_state(guac_user* user, void* data) {

    guac_display* display = (guac_display*) data;

    /* Send cursor state only if the user is not moving the cursor */
    if (user != display->last_frame.cursor_user) {
        guac_protocol_send_mouse(user->socket,
                display->last_frame.cursor_x, display->last_frame.cursor_y,
                display->last_frame.cursor_mask, display->last_frame.timestamp);
        guac_socket_flush(user->socket);
    }

    return NULL;

}

/**
 * Inline-threshold for parallelizing the pending-to-last dirty-rect
 * memcpy via guac_display_parallel_for: if the dirty rect is at most
 * this many rows tall, the memcpy runs on the calling thread rather
 * than being dispatched to the worker pool. Each row is ~8 KB of
 * memcpy for a 1080p-width rect (< 1 μs at ~10 GB/s) and ~15 KB
 * (~1.5 μs) for 4K width, so 8 rows is 6-12 μs of total work - well
 * above the ~1 μs per-dispatch overhead.
 *
 * Above this threshold, guac_display_parallel_for distributes the
 * rows across all available workers with no chunk-size floor, so
 * wider core counts directly translate to more concurrent memcpy
 * streams and better effective memory bandwidth.
 */
#define GUAC_DISPLAY_COMMIT_COPY_MIN_CHUNK 8

/**
 * Shared context for a single layer's pending-to-last dirty-rect memcpy
 * parallel-for dispatch. The rows [0, dirty.bottom - dirty.top) in the
 * dispatched index space each correspond to one row within the dirty rect
 * of the layer being committed.
 */
typedef struct commit_copy_context {

    /**
     * Pointer to the first byte of the dirty rect in the pending-frame
     * buffer (i.e. already offset by dirty.top rows and dirty.left pixels).
     */
    const unsigned char* pending_base;

    /**
     * Pointer to the first byte of the dirty rect in the last-frame buffer
     * (offset identically to pending_base).
     */
    unsigned char* last_base;

    /**
     * The number of bytes to copy per row - the width of the dirty rect
     * multiplied by GUAC_DISPLAY_LAYER_RAW_BPP.
     */
    size_t row_length;

    /**
     * The stride (bytes per row) of the pending-frame buffer.
     */
    size_t pending_stride;

    /**
     * The stride (bytes per row) of the last-frame buffer.
     */
    size_t last_stride;

} commit_copy_context;

/**
 * Parallel-for chunk callback for the commit-phase dirty-rect memcpy.
 * Copies rows [start, end) (indexed from 0 relative to the top of the dirty
 * rect) from pending-frame to last-frame for a single layer. Concurrent
 * chunks touch disjoint row ranges and therefore disjoint cache lines, so
 * no synchronization is required.
 */
static void commit_copy_task(void* context, int start, int end) {

    const commit_copy_context* ctx = (const commit_copy_context*) context;

    const unsigned char* pending = ctx->pending_base + (size_t) start * ctx->pending_stride;
    unsigned char* last = ctx->last_base + (size_t) start * ctx->last_stride;

    for (int i = start; i < end; i++) {
        memcpy(last, pending, ctx->row_length);
        pending += ctx->pending_stride;
        last += ctx->last_stride;
    }

}

/**
 * First half of frame-complete: emits layer property protocol
 * (size/shade/move/touches), mirrors non-buffer state from pending_frame
 * to last_frame (layer list head, per-layer list pointers, cursor
 * state, timestamp, frame count), and emits the mouse cursor update
 * if the cursor position changed.
 *
 * Separated from guac_display_frame_complete_buffer so the commit
 * handler can sandwich the inline COPY/RECT emission of the plan
 * between the two halves - the pending-to-last pixel memcpy in the
 * buffer half then overlaps with the COPY/RECT protocol bytes draining
 * to the network via the kernel TCP buffers.
 *
 * @param display
 *     The display whose pending-frame properties should be committed
 *     into last_frame.
 *
 * @return
 *     Non-zero if any layer property or cursor protocol emission took
 *     place that should contribute to the frame being considered non-
 *     empty, zero otherwise.
 */
static int PFW_LFW_guac_display_frame_complete_properties(guac_display* display) {

    guac_client* client = display->client;
    int retval = 0;

    display->last_frame.layers = display->pending_frame.layers;
    guac_display_layer* current = display->pending_frame.layers;
    while (current != NULL) {

        /* Skip processing any layers whose buffers have been replaced with
         * NULL (this is intentionally allowed to ensure references to external
         * buffers can be safely removed if necessary, even before guac_display
         * is freed) */
        if (current->pending_frame.buffer == NULL) {
            GUAC_ASSERT(current->pending_frame.buffer_is_external);
            continue;
        }

        /* Commit any change in layer size */
        if (current->pending_frame.width != current->last_frame.width
                || current->pending_frame.height != current->last_frame.height) {

            guac_protocol_send_size(client->socket, current->layer,
                    current->pending_frame.width, current->pending_frame.height);

            current->last_frame.width = current->pending_frame.width;
            current->last_frame.height = current->pending_frame.height;

            retval = 1;

        }

        /* Commit any change in layer opacity */
        if (current->pending_frame.opacity != current->last_frame.opacity) {

            guac_protocol_send_shade(client->socket, current->layer,
                    current->pending_frame.opacity);

            current->last_frame.opacity = current->pending_frame.opacity;

            retval = 1;

        }

        /* Commit any change in layer location/hierarchy */
        if (current->pending_frame.x != current->last_frame.x
                || current->pending_frame.y != current->last_frame.y
                || current->pending_frame.z != current->last_frame.z
                || current->pending_frame.parent != current->last_frame.parent) {

            guac_protocol_send_move(client->socket, current->layer,
                    current->pending_frame.parent,
                    current->pending_frame.x,
                    current->pending_frame.y,
                    current->pending_frame.z);

            current->last_frame.x = current->pending_frame.x;
            current->last_frame.y = current->pending_frame.y;
            current->last_frame.z = current->pending_frame.z;
            current->last_frame.parent = current->pending_frame.parent;

            retval = 1;

        }

        /* Commit any change in layer multitouch support */
        if (current->pending_frame.touches != current->last_frame.touches) {
            guac_protocol_send_set_int(client->socket, current->layer,
                    GUAC_PROTOCOL_LAYER_PARAMETER_MULTI_TOUCH,
                    current->pending_frame.touches);
            current->last_frame.touches = current->pending_frame.touches;
        }

        /* Commit any hinting regarding scroll/copy optimization (NOTE: While
         * this value is copied for consistency, it will already have taken
         * effect in the context of the pending frame due to the scroll/copy
         * optimization pass having occurred prior to the call to this
         * function) */
        current->last_frame.search_for_copies = current->pending_frame.search_for_copies;
        current->pending_frame.search_for_copies = 0;

        /* Commit any change in lossless setting (no need to synchronize this
         * to the client - it affects only how last_frame is interpreted) */
        current->last_frame.lossless = current->pending_frame.lossless;

        /* Duplicate layers from pending frame to last frame */
        current->last_frame.prev = current->pending_frame.prev;
        current->last_frame.next = current->pending_frame.next;
        current = current->pending_frame.next;

    }

    display->last_frame.timestamp = display->pending_frame.timestamp;
    display->last_frame.frames = display->pending_frame.frames;

    display->pending_frame.frames = 0;
    display->pending_frame_dirty_excluding_mouse = 0;

    /* Commit cursor hotspot */
    display->last_frame.cursor_hotspot_x = display->pending_frame.cursor_hotspot_x;
    display->last_frame.cursor_hotspot_y = display->pending_frame.cursor_hotspot_y;

    /* Commit mouse cursor location and notify all other users of change in
     * cursor state */
    if (display->pending_frame.cursor_x != display->last_frame.cursor_x
            || display->pending_frame.cursor_y != display->last_frame.cursor_y
            || display->pending_frame.cursor_mask != display->last_frame.cursor_mask) {

        display->last_frame.cursor_user = display->pending_frame.cursor_user;
        display->last_frame.cursor_x = display->pending_frame.cursor_x;
        display->last_frame.cursor_y = display->pending_frame.cursor_y;
        display->last_frame.cursor_mask = display->pending_frame.cursor_mask;
        guac_client_foreach_user(client, LFR_guac_display_broadcast_cursor_state, display);

        /* NOTE: We DO NOT set retval here, as flushing a frame due purely to
         * mouse position changes can cause slowdowns apparently from the sheer
         * quantity of frames */

    }

    return retval;

}

/**
 * Second half of frame-complete: runs the pending-to-last pixel copy
 * for each layer. When a layer's backing buffer dimensions changed,
 * the last_frame buffer is reallocated and the entire pending buffer
 * is copied over; otherwise only the pending_frame.dirty rect is
 * copied (with memcpy parallelized across rows via parallel_for).
 *
 * Must be invoked after guac_display_plan_apply_emit() so the COPY/
 * RECT protocol bytes for this frame are already sitting in the
 * socket's TCP send buffer - the kernel drains those bytes to the
 * client over the network while this function does its memcpy,
 * giving the commit stage thread effective overlap between CPU
 * memcpy work and network transmission.
 *
 * @param display
 *     The display whose pending-frame pixel state should be committed
 *     into last_frame.
 *
 * @return
 *     Non-zero if any layer's buffer was actually resized or had its
 *     dirty rect copied, zero otherwise.
 */
static int PFW_LFW_guac_display_frame_complete_buffer(guac_display* display) {

    int retval = 0;

    guac_display_layer* current = display->pending_frame.layers;
    while (current != NULL) {

        if (current->pending_frame.buffer == NULL) {
            GUAC_ASSERT(current->pending_frame.buffer_is_external);
            continue;
        }

        /* Always resize the last_frame buffer to match the pending_frame prior
         * to copying over any changes (this is particularly important given
         * that the pending_frame buffer can be replaced with an external
         * buffer). Since this involves copying over all data from the
         * pending frame, we can skip the later pending frame copy based on
         * whether the pending frame is dirty. */
        if (current->last_frame.buffer_stride != current->pending_frame.buffer_stride
                || current->last_frame.buffer_width != current->pending_frame.buffer_width
                || current->last_frame.buffer_height != current->pending_frame.buffer_height) {

            size_t buffer_size = guac_mem_ckd_mul_or_die(current->pending_frame.buffer_height,
                    current->pending_frame.buffer_stride);

            guac_mem_free(current->last_frame.buffer);
            current->last_frame.buffer = guac_mem_zalloc(buffer_size);
            memcpy(current->last_frame.buffer, current->pending_frame.buffer, buffer_size);

            current->last_frame.buffer_stride = current->pending_frame.buffer_stride;
            current->last_frame.buffer_width = current->pending_frame.buffer_width;
            current->last_frame.buffer_height = current->pending_frame.buffer_height;

            current->last_frame.dirty = current->pending_frame.dirty;
            current->pending_frame.dirty = (guac_rect) { 0 };

            retval = 1;

        }

        /* Copy over pending frame contents if actually changed (this is not
         * necessary if the last_frame buffer was resized to match
         * pending_frame, as a copy from pending_frame to last_frame is
         * inherently part of that). Only the dirty rect needs to be copied,
         * as last_frame already matches pending_frame outside that region. */
        else if (!guac_rect_is_empty(&current->pending_frame.dirty)) {

            const guac_rect* dirty = &current->pending_frame.dirty;

            commit_copy_context ctx = {
                .pending_base = GUAC_DISPLAY_LAYER_STATE_MUTABLE_BUFFER(current->pending_frame, *dirty),
                .last_base = GUAC_DISPLAY_LAYER_STATE_MUTABLE_BUFFER(current->last_frame, *dirty),
                .row_length = guac_mem_ckd_mul_or_die(guac_rect_width(dirty),
                        GUAC_DISPLAY_LAYER_RAW_BPP),
                .pending_stride = current->pending_frame.buffer_stride,
                .last_stride = current->last_frame.buffer_stride,
            };

            /* Parallelize across row ranges. memcpy is memory-bandwidth
             * bound and a single core typically cannot saturate the memory
             * subsystem on its own - multiple concurrent memcpy streams
             * across disjoint row ranges roughly double or triple effective
             * bandwidth on systems with multiple memory channels. For full
             * 4K refreshes (33 MB pending-to-last copy), this can cut the
             * commit phase from ~10 ms to a few ms. */
            guac_display_parallel_for(display, dirty->bottom - dirty->top,
                    GUAC_DISPLAY_COMMIT_COPY_MIN_CHUNK,
                    commit_copy_task, &ctx);

            current->last_frame.dirty = *dirty;
            current->pending_frame.dirty = (guac_rect) { 0 };

            retval = 1;

        }

        /* Even if nothing has changed in the pending frame, we have to at
         * least flush that fact to the last frame (otherwise, the last frame
         * may contain stale dirty rects) */
        else
            current->last_frame.dirty = (guac_rect) { 0 };

        current = current->pending_frame.next;

    }

    return retval;

}

void guac_display_end_mouse_frame(guac_display* display) {

    /* Sample pending_frame_dirty_excluding_mouse under the pending flag
     * and release before calling end_multiple_frames, so we never hold
     * the flag's mutex across a stage-pipeline handoff. A brief race
     * between release and the conditional flush is harmless: if
     * dirty_excluding_mouse becomes true in that window, the inner
     * flush simply picks up both the mouse update and the newly
     * reported change in one pass. */
    guac_flag_wait_and_lock(&display->pending_state, GUAC_DISPLAY_PENDING_WRITABLE);
    int dirty_excluding_mouse = display->pending_frame_dirty_excluding_mouse;
    guac_flag_unlock(&display->pending_state);

    if (!dirty_excluding_mouse)
        guac_display_end_multiple_frames(display, 0);

}

/**
 * Logs the phase-timing trace line for the given fan_out using
 * its captured phase_start and phase metadata, and frees the
 * fan_out. The caller must have already consumed any data it
 * still needs from the fan_out (in particular, fan_out->forward
 * must have been enqueued or copied before this is called).
 *
 * Start and end are logged as offsets from display->frame_start
 * using the same format as GUAC_DISPLAY_PLAN_END_PHASE so the
 * two log types can be read together on one timeline.
 */
static void guac_display_fan_out_finish(guac_display* display,
        guac_display_plan_fan_out* fan_out) {

    guac_timestamp phase_end = guac_timestamp_current();
    guac_client_log(display->client, GUAC_LOG_TRACE, "Render planning "
            "phase %i/%i (%s): start=+%ims end=+%ims dur=%ims",
            fan_out->phase_number, fan_out->phase_total,
            fan_out->phase_name,
            (int) (fan_out->phase_start - display->frame_start),
            (int) (phase_end - display->frame_start),
            (int) (phase_end - fan_out->phase_start));

    guac_mem_free(fan_out->search_chunks);
    guac_mem_free(fan_out);

}

/**
 * Dispatches the copy-search phase for the given completed plan by
 * building the (layer, sub_rect) chunk list and enqueuing one search
 * chunk item per strip onto search_stage.fifo. If no chunks qualify
 * (no layer has search_for_copies set, or no layer has a large enough
 * dirty region), the plan_work is enqueued directly to commit_stage
 * instead. Called by the last-finishing rects chunk.
 */
static void guac_display_dispatch_search(guac_display* display,
        guac_display_plan_work forward) {

    guac_display_plan* plan = forward.plan;

    int strips_per_layer = display->search_stage.thread_count;
    int chunk_count = 0;
    guac_display_plan_search_rect* chunks =
            PFR_guac_display_plan_build_search_chunks(plan, strips_per_layer,
                    &chunk_count);

    /* No search work to do - skip straight to commit. */
    if (chunk_count == 0) {
        guac_fifo_enqueue(&display->commit_stage.fifo, &forward);
        return;
    }

    guac_display_plan_fan_out* fan_out =
            guac_mem_alloc(sizeof(guac_display_plan_fan_out));
    atomic_init(&fan_out->remaining, chunk_count);
    fan_out->forward = forward;
    fan_out->phase_start = guac_timestamp_current();
    fan_out->phase_name = "search";
    fan_out->phase_number = 3;
    fan_out->phase_total = 5;
    fan_out->search_chunks = chunks;

    for (int i = 0; i < chunk_count; i++) {
        guac_display_plan_search_chunk chunk = {
            .plan = plan,
            .layer = chunks[i].layer,
            .sub_rect = chunks[i].sub_rect,
            .fan_out = fan_out,
        };
        guac_fifo_enqueue(&display->search_stage.fifo, &chunk);
    }

}

void guac_display_draft_handler(void* item, void* user_data) {

    guac_display* display = (guac_display*) user_data;
    guac_display_plan_work* work = (guac_display_plan_work*) item;

    /* PASS 1/5: Detect dirty cells and allocate plan. plan_create
     * runs pass 1 (per-layer parallel_for refining the dirty rect
     * cell-by-cell), then allocates plan->ops sized to the total
     * dirty cell count but leaves the ops unpopulated. The build
     * stage chunks that follow fill those ops in parallel. */
    guac_rwlock_acquire_read_lock(&display->last_frame_lock);
    GUAC_DISPLAY_PLAN_BEGIN_PHASE();
    work->plan = PFW_LFR_guac_display_plan_create(display);
    GUAC_DISPLAY_PLAN_END_PHASE(display, "draft", 1, 5);
    guac_rwlock_release_lock(&display->last_frame_lock);

    /* Stamp the frame-end timestamp onto pending_frame so frame_complete
     * in the commit stage can propagate it into last_frame. Pending is
     * still claimed (PENDING_WRITABLE clear) so drawers cannot race. */
    if (work->plan != NULL)
        display->pending_frame.timestamp = work->plan->frame_end;

    /* No plan - nothing for build or search to do; skip directly to
     * commit, which still has to run frame_complete for layer property
     * changes and release pending ownership back to drawers. */
    if (work->plan == NULL) {
        guac_fifo_enqueue(&display->commit_stage.fifo, work);
        return;
    }

    /* Fan out one chunk per (dirty layer, cell-row-sub-range) pair.
     * Each layer is split into at most worker_thread_count chunks
     * across its cell-rows. Only layers that had any dirty cells at
     * all during pass 1 (plan->dirty_layers) are considered, so
     * frames that touch a single layer don't spray no-op chunks to
     * every other layer on the display. */
    int workers = display->build_stage.thread_count;
    if (workers < 1)
        workers = 1;

    /* Compute total chunk count up front so the fan_out counter is
     * set correctly before any chunk is enqueued - an early-finishing
     * chunk could otherwise observe a counter of 0 and falsely
     * believe it is the last one. */
    int total_chunks = 0;
    for (int l = 0; l < work->plan->dirty_layer_count; l++) {
        guac_display_layer* layer = work->plan->dirty_layers[l];
        int cells_h = (int) layer->pending_frame_cells_height;
        int chunks = workers;
        if (chunks > cells_h)
            chunks = cells_h;
        if (chunks < 1)
            chunks = 1;
        total_chunks += chunks;
    }

    guac_display_plan_fan_out* fan_out =
            guac_mem_alloc(sizeof(guac_display_plan_fan_out));
    atomic_init(&fan_out->remaining, total_chunks);
    fan_out->forward = *work;
    fan_out->phase_start = guac_timestamp_current();
    fan_out->phase_name = "build";
    fan_out->phase_number = 2;
    fan_out->phase_total = 5;
    fan_out->search_chunks = NULL;

    for (int l = 0; l < work->plan->dirty_layer_count; l++) {

        guac_display_layer* layer = work->plan->dirty_layers[l];
        int cells_h = (int) layer->pending_frame_cells_height;

        int chunks = workers;
        if (chunks > cells_h)
            chunks = cells_h;
        if (chunks < 1)
            chunks = 1;

        int chunk_size = (cells_h + chunks - 1) / chunks;

        for (int i = 0; i < chunks; i++) {

            int start = i * chunk_size;
            int end = start + chunk_size;
            if (end > cells_h)
                end = cells_h;

            guac_display_plan_build_chunk chunk = {
                .plan = work->plan,
                .layer = layer,
                .cell_row_start = start,
                .cell_row_end = end,
                .fan_out = fan_out,
            };

            guac_fifo_enqueue(&display->build_stage.fifo, &chunk);

        }

    }

}

void guac_display_build_chunk_handler(void* item, void* user_data) {

    guac_display* display = (guac_display*) user_data;
    guac_display_plan_build_chunk* chunk = (guac_display_plan_build_chunk*) item;
    guac_display_plan_fan_out* fan_out = chunk->fan_out;
    guac_display_plan* plan = chunk->plan;
    guac_display_layer* layer = chunk->layer;

    /* PASS 2/5: For each dirty cell in this chunk's (layer, cell-row)
     * range, claim an output slot in plan->ops via atomic fetch_add,
     * write the op, collapse to RECT if possible, and hash-index the
     * op for the upcoming copy search. Clean cells have their
     * related_op pointer reset to NULL so the combine phase can
     * shortcut past them without dereferencing stale pointers from
     * prior frames. */
    guac_rwlock_acquire_read_lock(&display->last_frame_lock);

    size_t cells_w = layer->pending_frame_cells_width;
    guac_display_layer_cell* row_start =
            layer->pending_frame_cells + (size_t) chunk->cell_row_start * cells_w;

    for (int y = chunk->cell_row_start; y < chunk->cell_row_end; y++) {

        guac_display_layer_cell* cell = row_start;
        for (size_t x = 0; x < cells_w; x++) {

            if (cell->dirty_size) {

                /* Atomically claim this cell's output slot in plan->ops.
                 * Ops end up in whatever order workers reach their
                 * dirty cells, which is fine - nothing downstream
                 * depends on plan->ops order. */
                size_t idx = atomic_fetch_add_explicit(&plan->next_op_index,
                        1, memory_order_relaxed);
                guac_display_plan_operation* op = &plan->ops[idx];

                op->layer = layer;
                op->type = GUAC_DISPLAY_PLAN_OPERATION_IMG;
                op->dest = cell->dirty;
                op->dirty_size = cell->dirty_size;
                op->last_frame = cell->last_frame;
                op->current_frame = plan->frame_end;

                cell->related_op = op;
                cell->dirty_size = 0;
                cell->last_frame = plan->frame_end;

                /* Rect-detect and hash-index immediately on this op
                 * while its fields are still warm in cache. Both
                 * passes are per-op independent; no cross-cell or
                 * cross-chunk ordering required. */
                PFR_guac_display_plan_rewrite_op_as_rect(op);
                PFR_guac_display_plan_index_op(plan, op);

            }
            else
                cell->related_op = NULL;

            cell++;

        }

        row_start += cells_w;

    }

    guac_rwlock_release_lock(&display->last_frame_lock);

    /* Last-finishing chunk: aggregate per-layer pending_frame.dirty
     * rects (skipped per-chunk to avoid cross-chunk contention on
     * the layer dirty rect) and dispatch search. Using acq_rel on
     * the decrement pairs the release-side of every chunk's op
     * writes with the acquire-side of whichever thread reads plan
     * state next. */
    int remaining = atomic_fetch_sub_explicit(&fan_out->remaining, 1,
            memory_order_acq_rel);
    if (remaining == 1) {

        /* Rebuild each dirty layer's pending_frame.dirty rect from
         * the ops the build chunks produced. Single-threaded here,
         * so no locking on the extend. */
        size_t total_ops = atomic_load_explicit(&plan->next_op_index,
                memory_order_relaxed);
        GUAC_ASSERT(total_ops == plan->length);
        for (size_t i = 0; i < total_ops; i++) {
            guac_display_plan_operation* op = &plan->ops[i];
            guac_rect_extend(&op->layer->pending_frame.dirty, &op->dest);
        }

        guac_display_plan_work forward = fan_out->forward;
        guac_display_fan_out_finish(display, fan_out);
        guac_display_dispatch_search(display, forward);

    }

}

void guac_display_search_chunk_handler(void* item, void* user_data) {

    guac_display* display = (guac_display*) user_data;
    guac_display_plan_search_chunk* chunk = (guac_display_plan_search_chunk*) item;
    guac_display_plan_fan_out* fan_out = chunk->fan_out;

    /* PASS 3/5: Rewrite IMG ops as COPY ops where scroll-detection
     * finds matching content in last_frame for this chunk's strip.
     * The search is preemptible: any worker detecting contention on
     * pending_state before any copies have been found flips
     * plan->search_aborted, draining the remaining search chunks. */
    guac_rwlock_acquire_read_lock(&display->last_frame_lock);
    PFR_LFR_guac_display_plan_search_chunk(chunk->plan, chunk->layer,
            &chunk->sub_rect);
    guac_rwlock_release_lock(&display->last_frame_lock);

    int remaining = atomic_fetch_sub_explicit(&fan_out->remaining, 1,
            memory_order_acq_rel);
    if (remaining == 1) {

        if (atomic_load_explicit(&chunk->plan->search_aborted,
                    memory_order_relaxed))
            guac_client_log(display->client, GUAC_LOG_TRACE,
                    "Render planning phase 3/5 (search): aborted early "
                    "(contention on pending_state, no copies found)");

        guac_display_plan_work forward = fan_out->forward;
        guac_display_fan_out_finish(display, fan_out);
        guac_fifo_enqueue(&display->commit_stage.fifo, &forward);

    }

}

void guac_display_commit_handler(void* item, void* user_data) {

    guac_display* display = (guac_display*) user_data;
    guac_display_plan_work* work = (guac_display_plan_work*) item;

    /* PASS 4/5: Combine adjacent operations in horizontal and vertical
     * directions where that improves encoding/decoding efficiency
     * without defeating worker parallelism. */
    if (work->plan != NULL) {
        GUAC_DISPLAY_PLAN_BEGIN_PHASE();
        PFW_guac_display_plan_combine_horizontally(work->plan);
        PFW_guac_display_plan_combine_vertically(work->plan);
        GUAC_DISPLAY_PLAN_END_PHASE(display, "combine", 4, 5);
    }

    /* PASS 5/5: Commit - in four interleaved sub-steps, all under the
     * last_frame write lock (the only phase that writes last_frame):
     *
     *   (a) frame_complete_properties: emit size/shade/move/touches
     *       and cursor protocol, mirror non-buffer state from pending
     *       into last_frame. Must happen before (b) so the client
     *       sees a resized layer before receiving drawing ops that
     *       land in any newly-available area.
     *
     *   (b) plan_apply_emit: stamp lossy_quality/target_bytes on every
     *       op and emit COPY/RECT protocol inline. Pushes their bytes
     *       into the socket TCP buffer so the kernel can start
     *       draining them to the client over the network.
     *
     *   (c) frame_complete_buffer: pending-to-last pixel memcpy. The
     *       CPU does this while the kernel transmits the COPY/RECT
     *       bytes queued in (b), so the two overlap at the I/O layer
     *       even though both (b) and (c) run on this commit thread.
     *
     *   (d) release pending: drawers of the next frame can now touch
     *       pending_frame. Must happen after (c) since (c) reads
     *       pending buffers.
     *
     *   (e) plan_apply_enqueue_img (below, outside the lock): push
     *       IMG ops onto encode_stage.fifo so encode workers wake
     *       up. Must be after the last_frame write lock is released
     *       so workers can take the read lock for encoding.
     */
    int properties_nonempty = 0;
    int buffer_nonempty = 0;
    guac_rwlock_acquire_write_lock(&display->last_frame_lock);
    GUAC_DISPLAY_PLAN_BEGIN_PHASE();
    properties_nonempty = PFW_LFW_guac_display_frame_complete_properties(display);
    if (work->plan != NULL)
        guac_display_plan_apply_emit(work->plan);
    buffer_nonempty = PFW_LFW_guac_display_frame_complete_buffer(display);
    GUAC_DISPLAY_PLAN_END_PHASE(display, "commit", 5, 5);
    guac_rwlock_release_lock(&display->last_frame_lock);

    work->frame_nonempty = properties_nonempty || buffer_nonempty;

    /* Release pending ownership now that frame_complete_buffer has
     * finished copying pending into last_frame. Drawers of the next
     * frame may now touch pending_frame while the encode stage
     * finishes processing this one. */
    guac_flag_set(&display->pending_state, GUAC_DISPLAY_PENDING_WRITABLE);

    /* Stamp the encode-phase start timestamp before any enqueue onto
     * encode_stage.fifo. The last encode worker reads this field at
     * end-of-frame to compute the encode-phase duration. Also reset
     * the per-op accounting atomics so the end-of-frame breakdown
     * reflects only this frame's encode work. */
    display->encode_start = guac_timestamp_current();
    atomic_store_explicit(&display->encode_img_us, 0, memory_order_relaxed);
    atomic_store_explicit(&display->encode_codec_us, 0, memory_order_relaxed);

    /* Enqueue IMG ops onto the encode stage's FIFO. COPY/RECT bytes
     * were already emitted to the socket by plan_apply_emit above;
     * this step exclusively handles the IMG ops that require worker
     * encoding. Ownership of work->plan passes to this call and to
     * the free below. */
    int worker_ops_enqueued = 0;
    if (work->plan != NULL) {
        worker_ops_enqueued = guac_display_plan_apply_enqueue_img(work->plan);
        guac_display_plan_free(work->plan);
    }

    /* Log the encode-phase start + op count while the count is in
     * hand. Paired with the "Frame encode: end=..." log emitted by
     * the last encode worker. Split across two lines (rather than
     * snapshotted onto the display struct for the worker to read) so
     * we don't stash information for end-of-frame that already exists
     * in the return value of plan_apply_enqueue_img. */
    guac_client_log(display->client, GUAC_LOG_TRACE,
            "Frame encode: start=+%ims ops=%i",
            (int) (display->encode_start - display->frame_start),
            worker_ops_enqueued);

    /* Not all frames are graphical, and not all frames result in
     * operations reaching the encode workers. If we end up with a
     * frame containing nothing but layer property changes or nothing
     * but non-image updates, we must still send at least one operation
     * to awaken the encode workers, flush any layer changes, and mark
     * the end of the frame with a "sync", even though there is no
     * display plan to optimize. */
    if (work->frame_nonempty) {
        guac_display_plan_operation end_frame_op = {
            .type = GUAC_DISPLAY_PLAN_OPERATION_NOP
        };
        guac_fifo_enqueue(&display->encode_stage.fifo, &end_frame_op);
        worker_ops_enqueued++;
    }

    /* If nothing was enqueued into the encode FIFO, no worker will wake
     * up to run the end-of-frame path that normally clears flush_state.
     * Clear it here so the next frame is not spuriously deferred
     * forever. */
    if (worker_ops_enqueued == 0)
        guac_flag_clear(&display->flush_state, GUAC_DISPLAY_FLUSH_IN_PROGRESS);

}

void guac_display_end_multiple_frames(guac_display* display, int frames) {

    /* Claim the pending frame for this flush. This blocks while a
     * drawer is mid-transaction (holds the flag's mutex) and while
     * another flush is between here and its "release pending" step
     * (PENDING_WRITABLE bit clear). Once we've taken the mutex with
     * the bit set, we clear the bit to block subsequent drawers and
     * release the mutex so the plan stage can later release ownership
     * back to drawers after commit. */
    guac_flag_wait_and_lock(&display->pending_state, GUAC_DISPLAY_PENDING_WRITABLE);
    display->pending_frame.frames += frames;
    guac_flag_clear(&display->pending_state, GUAC_DISPLAY_PENDING_WRITABLE);
    guac_flag_unlock(&display->pending_state);

    /* Defer rendering of further frames until after any in-progress frame has
     * finished. Graphical changes will meanwhile continue being accumulated in
     * the pending frame. A single flush_state flag covers the entire flush
     * (from here through last-worker end-of-frame), so we need only check
     * its value to decide whether another flush is already active; the last
     * worker observes display->frame_deferred and re-fires
     * guac_display_end_multiple_frames() to pick up whatever accumulated. */

    guac_flag_lock(&display->flush_state);
    int defer_frame = display->frame_deferred =
        (display->flush_state.value & GUAC_DISPLAY_FLUSH_IN_PROGRESS) != 0;
    if (!defer_frame)
        guac_flag_set(&display->flush_state, GUAC_DISPLAY_FLUSH_IN_PROGRESS);
    guac_flag_unlock(&display->flush_state);

    /* If deferring, we never claimed pending ownership past the initial
     * accumulate step. Release it back to drawers and return; the last
     * encode worker will observe frame_deferred and re-fire this
     * function once it finishes end-of-frame processing. */
    if (defer_frame) {
        guac_flag_set(&display->pending_state, GUAC_DISPLAY_PENDING_WRITABLE);
        return;
    }

    /* Stamp the frame's baseline timestamp for phase-timing logs and
     * for the overall "Frame processed" duration log emitted by the
     * last encode worker. Must be set before enqueuing into the draft
     * FIFO so the draft worker sees the write via the FIFO's enqueue/
     * dequeue happens-before. */
    display->frame_start = guac_timestamp_current();

    /* Hand the plan pipeline off to the draft stage. The render thread
     * returns here without blocking on any plan phase; the draft worker
     * takes over with pending ownership already claimed (PENDING_WRITABLE
     * bit cleared above). The pipeline flows draft → rects → search →
     * commit, with commit releasing pending back to drawers once the
     * pending-to-last buffer copy is done. */
    guac_display_plan_work work = { .plan = NULL, .frame_nonempty = 0 };
    guac_fifo_enqueue(&display->draft_stage.fifo, &work);

}
