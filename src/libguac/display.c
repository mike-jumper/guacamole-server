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
#include "guacamole/client.h"
#include "guacamole/display.h"
#include "guacamole/fifo.h"
#include "guacamole/layer.h"
#include "guacamole/mem.h"
#include "guacamole/protocol.h"
#include "guacamole/rect.h"
#include "guacamole/rwlock.h"
#include "guacamole/socket.h"
#include "guacamole/timestamp.h"
#include "guacamole/user.h"

#ifdef __MINGW32__
#include <winbase.h>
#endif

#include <cairo/cairo.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

/**
 * The number of worker threads to create per processor.
 */
#define GUAC_DISPLAY_CPU_THREAD_FACTOR 1

/**
 * Logs an informational message describing the SIMD code path currently in
 * effect for guac_display's 2D rolling-hash inner loop - which is
 * responsible for most of the search- and index-phase cost when generating
 * display plans. The message distinguishes among the three cases that can
 * meaningfully differ at runtime:
 *
 *   1. Vector extensions available + x86 runtime dispatch: the target_clones
 *      ifunc resolver chose at load time among AVX-512F, AVX2, SSE4.2, and
 *      default (SSE2, 128-bit) variants based on CPU feature detection.
 *      The chosen tier is reported via __builtin_cpu_supports(), with
 *      higher tiers taking priority.
 *
 *   2. Vector extensions available, no runtime dispatch: a single
 *      compile-time variant was emitted, matching the build's target ISA
 *      (e.g. NEON on AArch64, baseline SSE2 on x86-64 without target_clones).
 *
 *   3. Vector extensions not available at build time: the loop runs as pure
 *      scalar code. This is logged at WARNING rather than INFO so that
 *      builders notice an unexpectedly-slow configuration.
 *
 * @param client
 *     The guac_client to log the informational message against.
 */
static void guac_display_log_simd(guac_client* client) {
#ifdef HAVE_VECTOR_EXTENSIONS
#if defined(HAVE_ATTRIBUTE_TARGET_CLONES) && (defined(__x86_64__) || defined(__i386__))
    /* Runtime-dispatched on x86: query the CPU to report which variant
     * of the target_clones feature list the ifunc resolver picked. The
     * report reflects the highest tier the CPU supports - matching the
     * resolver's selection priority. */
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx512f"))
        guac_client_log(client, GUAC_LOG_INFO,
                "Display SIMD: AVX-512F (EVEX-encoded 256-bit vectors) via "
                "runtime dispatch.");
    else if (__builtin_cpu_supports("avx2"))
        guac_client_log(client, GUAC_LOG_INFO,
                "Display SIMD: AVX2 (256-bit vectors, 4 lanes) via "
                "runtime dispatch.");
    else if (__builtin_cpu_supports("sse4.2"))
        guac_client_log(client, GUAC_LOG_INFO,
                "Display SIMD: SSE4.2 (128-bit vectors with native "
                "integer min/max) via runtime dispatch.");
    else
        guac_client_log(client, GUAC_LOG_INFO,
                "Display SIMD: SSE2 (128-bit vectors, 2 lanes per "
                "256-bit operation) via runtime dispatch; CPU lacks "
                "AVX2/SSE4.2.");
#elif defined(__aarch64__) || defined(__arm__)
    guac_client_log(client, GUAC_LOG_INFO,
            "Display SIMD: NEON (128-bit vectors, 2 lanes per 256-bit "
            "operation).");
#elif defined(__x86_64__) || defined(__i386__)
    guac_client_log(client, GUAC_LOG_INFO,
            "Display SIMD: compile-time x86 target (single variant, no "
            "runtime dispatch).");
#else
    guac_client_log(client, GUAC_LOG_INFO,
            "Display SIMD: GCC vector extensions (target-dependent "
            "native width).");
#endif
#else
    guac_client_log(client, GUAC_LOG_WARNING,
            "Display SIMD: disabled (scalar fallback; compiler lacks "
            "GCC vector extensions). Expect significantly reduced "
            "performance on the display hash loop.");
#endif
}

/**
 * Returns the number of processors available to this process. If possible,
 * limits on otherwise available processors like CPU affinity will be taken
 * into account. If the number of available processors cannot be determined,
 * zero is returned.
 *
 * @return
 *     The number of available processors, or zero if this value cannot be
 *     determined for any reason.
 */
static unsigned long guac_display_nproc(void) {

#if defined(HAVE_SCHED_GETAFFINITY)

    /* Linux, etc. implementation leveraging sched_getaffinity() (this is
     * specific to glibc and MUSL libc and is non-portable) */

    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);

    if (sched_getaffinity(0, sizeof(cpu_set), &cpu_set) == 0) {
        long cpu_count = CPU_COUNT(&cpu_set);
        if (cpu_count > 0)
            return cpu_count;
    }

#elif defined(_SC_NPROCESSORS_ONLN)

    /* Linux, etc. implementation leveraging sysconf() and _SC_NPROCESSORS_ONLN
     * (which is also non-portable) */

    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu_count > 0)
        return cpu_count;

#elif defined(__MINGW32__)

    /* Windows-specific implementation (clearly also non-portable) */

    unsigned long cpu_count = 0;
    DWORD_PTR process_mask, system_mask;
    for (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask);
            process_mask != 0; process_mask >>= 1) {

        if (process_mask & 1)
                cpu_count++;

    }

    if (cpu_count > 0)
        return cpu_count;

#else

    /* Fallback implementation that does not query the number of CPUs available
     * at all, returning an error code (as portable as it gets) */

    long cpu_count = 0;

#endif

    return 0;

}

guac_display* guac_display_alloc(guac_client* client) {

    /* Allocate and init core properties (really just the client pointer) */
    guac_display* display = guac_mem_zalloc(sizeof(guac_display));
    display->client = client;

    /* Init last frame and pending frame tracking. Only last_frame needs
     * an rwlock; pending_frame is guarded by display->pending_state
     * (initialized further below) rather than a thread-bound rwlock. */
    guac_rwlock_init(&display->last_frame_lock);
    display->last_frame.timestamp = display->pending_frame.timestamp = guac_timestamp_current();

    /* Init flag used to notify threads that need to monitor whether a frame is
     * currently being rendered */
    guac_flag_init(&display->render_state);
    guac_flag_set(&display->render_state, GUAC_DISPLAY_RENDER_STATE_FRAME_NOT_IN_PROGRESS);

    /* Init flag used by guac_display_end_multiple_frames() to gate the
     * deferred-frame decision. Starts clear (no flush in progress). */
    guac_flag_init(&display->flush_state);

    /* Init flag guarding pending-frame access. Starts with
     * PENDING_WRITABLE set so drawers can freely draw until the first
     * flush claims pending ownership. This MUST be initialized before the
     * add_layer/alloc_buffer calls below, which acquire pending_state to
     * serialize their layer-list mutations against future drawers and
     * flushes. */
    guac_flag_init(&display->pending_state);
    guac_flag_set(&display->pending_state, GUAC_DISPLAY_PENDING_WRITABLE);

    /* It's safe to discard const of the default layer here, as
     * guac_display_free_layer() function is specifically written to consider
     * the default layer as const */
    display->default_layer = guac_display_add_layer(display, (guac_layer*) GUAC_DEFAULT_LAYER, 1);
    display->cursor_buffer = guac_display_alloc_buffer(display, 0);

    /* Encode and apply stage FIFOs live inside their respective stage
     * structs; they're initialized as part of stage init below. */

    int cpu_count = guac_display_nproc();
    if (cpu_count <= 0) {
        guac_client_log(client, GUAC_LOG_WARNING, "Number of available "
                "processors could not be determined. Assuming single-processor.");
        cpu_count = 1;
    }
    else {
        guac_client_log(client, GUAC_LOG_INFO, "Local system reports %i "
                "processor(s) are available.", cpu_count);
    }

    int worker_thread_count = cpu_count * GUAC_DISPLAY_CPU_THREAD_FACTOR;
    guac_client_log(client, GUAC_LOG_INFO, "Graphical updates will be encoded "
            "using %i worker thread(s).", worker_thread_count);

    /* Report which SIMD code path is in effect for the display hash loop.
     * On x86, the target_clones ifunc resolver has already chosen the
     * AVX2 or default variant at load time; this call queries which
     * choice was made so builders can verify they are getting the
     * expected performance on the target CPU. */
    guac_display_log_simd(client);

    /* Now that the core of the display has been fully initialized, it's safe
     * to start the worker threads. Each worker is then pinned to one
     * CPU in the calling process's cpu-affinity set, distributed round-
     * robin so that on machines where our cpu_count (above) equals the
     * worker_thread_count, every worker gets a distinct core. Pinning
     * gives each worker a sticky L1/L2 cache for the fraction of the
     * per-layer buffers it repeatedly touches (hashing, blend detect,
     * memcpy, encode scanlining), and it keeps the kernel from
     * migrating workers away from warm caches mid-frame.
     *
     * The pin is best-effort: pthread_setaffinity_np failures (e.g.,
     * on Windows builds, or on systems with no HAVE_SCHED_GETAFFINITY)
     * leave the thread unpinned and the scheduler handles placement.
     * The enumeration below walks the process's cpu_set to build a
     * list of actually-available CPU indices rather than assuming
     * CPUs 0..cpu_count-1, so this also works correctly inside
     * containers with non-contiguous cgroup CPU allocations. */
#if defined(HAVE_SCHED_GETAFFINITY)
    cpu_set_t available_cpus;
    CPU_ZERO(&available_cpus);
    int available_cpu_indices[CPU_SETSIZE];
    int available_cpu_count = 0;
    if (sched_getaffinity(0, sizeof(available_cpus), &available_cpus) == 0) {
        for (int c = 0; c < CPU_SETSIZE
                && available_cpu_count < CPU_SETSIZE; c++) {
            if (CPU_ISSET(c, &available_cpus))
                available_cpu_indices[available_cpu_count++] = c;
        }
    }
#endif

    guac_display_pipeline_stage_init(&display->encode_stage, "encode",
            display->encode_stage_items,
            GUAC_DISPLAY_WORKER_FIFO_SIZE, sizeof(guac_display_plan_operation),
            worker_thread_count,
            guac_display_worker_thread, display);

    /* Start the parallel-task stage that backs guac_display_parallel_for().
     * Sized to the same worker count as encode_stage so a single
     * parallel_for can fan out across the same number of CPUs the
     * encode pool would. Per-item handler mode - the work item is a
     * self-contained guac_display_plan_task. */
    guac_display_pipeline_stage_init_handler(&display->parallel_stage,
            "parallel",
            display->parallel_stage_items,
            GUAC_DISPLAY_PARALLEL_FIFO_SIZE, sizeof(guac_display_plan_task),
            worker_thread_count,
            guac_display_parallel_task_handler, display);

    /* Start the four plan pipeline stages: draft → rects → search →
     * commit. Downstream stages must exist before upstream stages
     * enqueue to them, so they're started in reverse-of-data-flow
     * order.
     *
     * draft and commit are single-worker per-frame stages - plan
     * creation and final combine/commit are each intrinsically serial.
     *
     * rects and search are multi-worker chunked stages: the upstream
     * stage's last-finishing handler fans out N chunks into the
     * downstream stage's FIFO, each chunk carries a pointer to a
     * shared fan_out completion counter, and the worker that
     * decrements the counter to zero is responsible for the fan-out's
     * post-processing and downstream handoff. */
    guac_display_pipeline_stage_init_handler(&display->commit_stage, "commit",
            display->commit_stage_items,
            GUAC_DISPLAY_PLAN_FIFO_SIZE, sizeof(guac_display_plan_work),
            1,
            guac_display_commit_handler, display);
    guac_display_pipeline_stage_init_handler(&display->search_stage, "search",
            display->search_stage_items,
            GUAC_DISPLAY_SEARCH_FIFO_SIZE,
            sizeof(guac_display_plan_search_chunk),
            worker_thread_count,
            guac_display_search_chunk_handler, display);
    guac_display_pipeline_stage_init_handler(&display->build_stage, "build",
            display->build_stage_items,
            GUAC_DISPLAY_BUILD_FIFO_SIZE,
            sizeof(guac_display_plan_build_chunk),
            worker_thread_count,
            guac_display_build_chunk_handler, display);
    guac_display_pipeline_stage_init_handler(&display->draft_stage, "draft",
            display->draft_stage_items,
            GUAC_DISPLAY_PLAN_FIFO_SIZE, sizeof(guac_display_plan_work),
            1,
            guac_display_draft_handler, display);

#if defined(HAVE_SCHED_GETAFFINITY)
    for (int i = 0; i < display->encode_stage.thread_count; i++) {
        if (available_cpu_count > 0) {
            int target_cpu = available_cpu_indices[i % available_cpu_count];
            cpu_set_t single_cpu;
            CPU_ZERO(&single_cpu);
            CPU_SET(target_cpu, &single_cpu);
            /* Failure here is non-fatal: the worker still runs, just
             * without a sticky-core preference. */
            pthread_setaffinity_np(display->encode_stage.threads[i],
                    sizeof(single_cpu), &single_cpu);
        }
    }
#endif

    return display;

}

void guac_display_stop(guac_display* display) {

    /* Ensure only one of any number of concurrent calls to guac_display_stop()
     * will actually start terminating the worker threads. The encode
     * stage's FIFO doubles as our "already stopping" latch: whichever
     * caller observes it as still valid is the one responsible for the
     * teardown below. */
    guac_fifo_lock(&display->encode_stage.fifo);

    /* Stop and clean up worker threads if the display is not already being
     * stopped (we don't use the GUAC_DISPLAY_RENDER_STATE_STOPPED flag here,
     * as we must consider the case that guac_display_stop() has already been
     * called in a different thread but has not yet finished) */
    if (guac_fifo_is_valid(&display->encode_stage.fifo)) {

        /* Invalidate the encode FIFO under the lock we're already
         * holding so concurrent guac_display_stop() callers see the
         * "already stopping" state atomically. The stage shutdown
         * below will also invalidate if called on a valid FIFO, but
         * doing it here first is what gates the other callers. */
        guac_fifo_invalidate(&display->encode_stage.fifo);
        guac_fifo_unlock(&display->encode_stage.fifo);

        /* Shut down stages in data-flow order: draft feeds rects, rects
         * feeds search, search feeds commit, commit feeds encode. Each
         * shutdown drains its own FIFO before joining workers, so
         * tearing down the upstream producer first ensures no stage
         * tries to enqueue into an invalidated downstream FIFO
         * mid-processing. The parallel stage is fed by parallel_for
         * calls inside the plan stages, so it goes after them but
         * before encode. */
        guac_display_pipeline_stage_shutdown(&display->draft_stage);
        guac_display_pipeline_stage_shutdown(&display->build_stage);
        guac_display_pipeline_stage_shutdown(&display->search_stage);
        guac_display_pipeline_stage_shutdown(&display->commit_stage);
        guac_display_pipeline_stage_shutdown(&display->parallel_stage);

        /* Wait for all encode worker threads to terminate (they should
         * nearly immediately terminate following invalidation of the
         * FIFO) and free the thread array. */
        guac_display_pipeline_stage_shutdown(&display->encode_stage);

        /* Notify other calls to guac_display_stop() that the display is now
         * officially stopped */
        guac_flag_set(&display->render_state, GUAC_DISPLAY_RENDER_STATE_STOPPED);

    }

    /* Even if it isn't this particular call to guac_display_stop() that
     * terminates and waits on all the worker threads, ensure that we only
     * return after all threads are known to have been stopped */
    else {

        guac_fifo_unlock(&display->encode_stage.fifo);

        guac_flag_wait_and_lock(&display->render_state, GUAC_DISPLAY_RENDER_STATE_STOPPED);
        guac_flag_unlock(&display->render_state);

    }

}

void guac_display_free(guac_display* display) {

    guac_display_stop(display);

    /* Stage FIFOs were destroyed by guac_display_stop() via the stage
     * shutdown path. Only flags need explicit destruction here. */
    guac_flag_destroy(&display->render_state);
    guac_flag_destroy(&display->flush_state);
    guac_flag_destroy(&display->pending_state);

    /* Free all layers within the pending_frame list (NOTE: This will also free
     * those layers from the last_frame list) */
    while (display->pending_frame.layers != NULL)
        guac_display_free_layer(display->pending_frame.layers);

    /* Free any remaining layers that were present only on the last_frame list
     * and not on the pending_frame list */
    while (display->last_frame.layers != NULL)
        guac_display_free_layer(display->last_frame.layers);

    guac_rwlock_destroy(&display->last_frame_lock);

    guac_mem_free(display);

}

void guac_display_dup(guac_display* display, guac_socket* socket) {

    guac_client* client = display->client;
    guac_rwlock_acquire_read_lock(&display->last_frame_lock);

    /* Wait for any pending frame to finish being sent to established users of
     * the connection before syncing any new users (doing otherwise could
     * result in trailing instructions of that pending frame getting sent to
     * new users after they finish joining, even though they are already in
     * sync with that frame, and those trailing instructions may not have the
     * intended meaning in context of the new users' remote displays) */
    guac_flag_wait_and_lock(&display->render_state,
            GUAC_DISPLAY_RENDER_STATE_FRAME_NOT_IN_PROGRESS);

    /* Sync the state of all layers/buffers */
    guac_display_layer* current = display->last_frame.layers;
    while (current != NULL) {

        const guac_layer* layer = current->layer;

        guac_rect layer_bounds;
        guac_display_layer_get_bounds(current, &layer_bounds);

        int width = guac_rect_width(&layer_bounds);
        int height = guac_rect_height(&layer_bounds);
        guac_protocol_send_size(socket, layer, width, height);

        if (width > 0 && height > 0) {

            /* Get Cairo surface covering layer bounds */
            unsigned char* buffer = GUAC_DISPLAY_LAYER_STATE_MUTABLE_BUFFER(current->last_frame, layer_bounds);
            cairo_surface_t* rect = cairo_image_surface_create_for_data(buffer,
                        current->opaque ? CAIRO_FORMAT_RGB24 : CAIRO_FORMAT_ARGB32,
                        width, height, current->last_frame.buffer_stride);

            /* Send PNG for rect */
            guac_client_stream_png(client, socket, GUAC_COMP_OVER, layer, 0, 0, rect);

            /* Resync copy of previous frame */
            guac_protocol_send_copy(socket,
                    layer, 0, 0, width, height,
                    GUAC_COMP_OVER, current->last_frame_buffer, 0, 0);

            cairo_surface_destroy(rect);

        }

        /* Resync any properties that are specific to non-buffer layers */
        if (current->layer->index > 0) {

            /* Resync layer opacity */
            guac_protocol_send_shade(socket, current->layer,
                    current->last_frame.opacity);

            /* Resync layer position/hierarchy */
            guac_protocol_send_move(socket, current->layer,
                    current->last_frame.parent,
                    current->last_frame.x,
                    current->last_frame.y,
                    current->last_frame.z);

        }

        /* Resync multitouch support */
        if (current->layer->index >= 0) {
            guac_protocol_send_set_int(socket, current->layer,
                    GUAC_PROTOCOL_LAYER_PARAMETER_MULTI_TOUCH,
                    current->last_frame.touches);
        }

        current = current->last_frame.next;

    }

    /* Synchronize mouse cursor */
    guac_display_layer* cursor = display->cursor_buffer;
    guac_protocol_send_cursor(socket,
            display->last_frame.cursor_hotspot_x,
            display->last_frame.cursor_hotspot_y,
            cursor->layer, 0, 0,
            cursor->last_frame.width,
            cursor->last_frame.height);

    /* Synchronize mouse location */
    guac_protocol_send_mouse(socket, display->last_frame.cursor_x, display->last_frame.cursor_y,
            display->last_frame.cursor_mask, client->last_sent_timestamp);

    /* The initial frame synchronizing the newly-joined users is now complete */
    guac_protocol_send_sync(socket, client->last_sent_timestamp, display->last_frame.frames);

    /* Further rendering for the current connection can now safely continue */
    guac_flag_unlock(&display->render_state);
    guac_rwlock_release_lock(&display->last_frame_lock);

    guac_socket_flush(socket);

}

void guac_display_notify_user_left(guac_display* display, guac_user* user) {
    guac_flag_wait_and_lock(&display->pending_state, GUAC_DISPLAY_PENDING_WRITABLE);

    /* Update to reflect leaving user, if necessary */
    if (display->pending_frame.cursor_user == user)
        display->pending_frame.cursor_user = NULL;

    guac_flag_unlock(&display->pending_state);
}

void guac_display_notify_user_moved_mouse(guac_display* display, guac_user* user, int x, int y, int mask) {

    guac_flag_wait_and_lock(&display->pending_state, GUAC_DISPLAY_PENDING_WRITABLE);
    display->pending_frame.cursor_user = user;
    display->pending_frame.cursor_x = x;
    display->pending_frame.cursor_y = y;
    display->pending_frame.cursor_mask = mask;
    guac_flag_unlock(&display->pending_state);

    guac_display_end_mouse_frame(display);

}

guac_display_layer* guac_display_default_layer(guac_display* display) {
    return display->default_layer;
}

guac_display_layer* guac_display_alloc_layer(guac_display* display, int opaque) {
    return guac_display_add_layer(display, guac_client_alloc_layer(display->client), opaque);
}

guac_display_layer* guac_display_alloc_buffer(guac_display* display, int opaque) {
    return guac_display_add_layer(display, guac_client_alloc_buffer(display->client), opaque);
}

void guac_display_free_layer(guac_display_layer* display_layer) {

    guac_display* display = display_layer->display;
    const guac_layer* layer = display_layer->layer;

    guac_display_remove_layer(display_layer);

    if (layer->index != 0) {

        guac_client* client = display->client;
        guac_protocol_send_dispose(client->socket, layer);

        /* As long as this isn't the display layer, it's safe to cast away the
         * constness and free the underlying layer/buffer. Only the default
         * layer (layer #0) is truly const. */
        if (layer->index > 0)
            guac_client_free_layer(client, (guac_layer*) layer);
        else
            guac_client_free_buffer(client, (guac_layer*) layer);

    }

}

void guac_display_parallel_task_handler(void* item, void* user_data) {

    (void) user_data;
    guac_display_plan_task* task = (guac_display_plan_task*) item;

    /* Run the chunk's user-supplied callback over its assigned range. */
    task->func(task->context, task->start, task->end);

    /* Decrement the shared remaining-chunks counter. The worker that
     * observes it drop to zero wakes the dispatching thread waiting in
     * guac_display_parallel_for(). */
    guac_display_parallel_task_state* state = task->state;
    pthread_mutex_lock(&state->lock);
    int now_done = (--state->remaining == 0);
    pthread_mutex_unlock(&state->lock);

    if (now_done)
        guac_flag_set(&state->done, 1);

}

void guac_display_parallel_for(guac_display* display, int count,
        int min_chunk_size, void (*func)(void* context, int start, int end),
        void* context) {

    /* Nothing to do for an empty range */
    if (count <= 0)
        return;

    int workers = display->parallel_stage.thread_count;

    /* Fallback to direct invocation on the calling thread if there's no
     * parallelism to exploit. This covers: single-threaded worker pool,
     * shutdown-in-progress, and work too small to be worth dispatching.
     * min_chunk_size plays the role of inline-threshold here: per-phase
     * callers pick it so that the total work of count items stays
     * substantially above the fixed dispatch overhead (~1 µs per
     * chunk, FIFO enqueue + state mutex round-trip). */
    if (workers <= 1 || count <= min_chunk_size) {
        func(context, 0, count);
        return;
    }

    /* Aim for one chunk per worker. chunk_size = ceil(count / workers)
     * so the last chunk rounds down rather than spills over. When
     * count < workers the computed chunk_size is 1 and num_chunks
     * = count, naturally keeping num_chunks <= workers.
     *
     * min_chunk_size deliberately does NOT floor chunk_size here:
     * doing so would cap parallelism at count/min_chunk_size on
     * many-core systems (e.g., a frame with 50 dirty cells on a
     * 64-core host could only use 50/16 = 3 workers under the old
     * behavior). Since the per-item work for every caller of
     * parallel_for is comfortably larger than the per-chunk dispatch
     * overhead, fragmenting down to 1-item chunks when count < workers
     * costs only a few percent of additional dispatch time while
     * unlocking full concurrency - and the inline fast path above
     * already catches the cases where total work is too small for any
     * dispatch to be worthwhile. */
    int chunk_size = (count + workers - 1) / workers;
    int num_chunks = (count + chunk_size - 1) / chunk_size;

    /* A single chunk means there's no actual parallelism, so dispatch
     * overhead is pure waste. Run directly on the calling thread. */
    if (num_chunks <= 1) {
        func(context, 0, count);
        return;
    }

    /* Initialize shared completion state on the dispatching thread's stack.
     * The state must outlive the last worker that decrements remaining. We
     * accomplish this by blocking below until the "done" flag is raised, at
     * which point all workers are finished touching the state. */
    guac_display_parallel_task_state state;
    pthread_mutex_init(&state.lock, NULL);
    state.remaining = num_chunks;
    guac_flag_init(&state.done);

    /* Enqueue each chunk as a guac_display_plan_task onto the parallel
     * stage's FIFO. Its workers pull tasks via the per-item handler
     * pattern; the encoding stage no longer participates in
     * parallel_for dispatch at all. */
    for (int i = 0; i < num_chunks; i++) {

        int start = i * chunk_size;
        int end = start + chunk_size;
        if (end > count)
            end = count;

        guac_display_plan_task task = {
            .func = func,
            .context = context,
            .start = start,
            .end = end,
            .state = &state,
        };

        guac_fifo_enqueue(&display->parallel_stage.fifo, &task);

    }

    /* Block until the last chunk has been completed by a worker */
    guac_flag_wait_and_lock(&state.done, 1);
    guac_flag_unlock(&state.done);

    guac_flag_destroy(&state.done);
    pthread_mutex_destroy(&state.lock);

}
