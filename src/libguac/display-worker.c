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
#include "guacamole/protocol-types.h"
#include "guacamole/protocol.h"
#include "guacamole/rect.h"
#include "guacamole/rwlock.h"
#include "guacamole/socket.h"
#include "guacamole/timestamp.h"

#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <cairo/cairo.h>
#include <pthread.h>

/**
 * Returns a wall-clock timestamp in microseconds from a monotonic
 * source. Used for sub-millisecond timing of the encode phase's
 * per-op work. CLOCK_MONOTONIC skips wall-clock adjustments (NTP,
 * manual settimeofday, etc.) so differences computed from two calls
 * are always non-negative and represent elapsed real time.
 */
static int64_t guac_display_monotonic_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000LL + (int64_t) ts.tv_nsec / 1000;
}

/**
 * Returns a new Cairo surface representing the contents of the given dirty
 * rectangle from the given layer. The returned surface must eventually be
 * freed with a call to cairo_surface_destroy(). The graphical contents will be
 * referenced from the layer's last_frame buffer. If sending the contents of a
 * pending frame, that pending frame must have been copied over to the
 * last_frame buffer before calling this function.
 *
 * @param display_layer
 *     The layer whose data should be referenced by the returned Cairo surface.
 *
 * @param dirty
 *     The region of the layer that should be referenced by the returned Cairo
 *     surface.
 *
 * @return
 *     A new Cairo surface that points to the given rectangle of image data
 *     from the last_frame buffer of the given layer. This surface must
 *     eventually be freed with a call to cairo_surface_destroy().
 */
static cairo_surface_t* LFR_guac_display_layer_cairo_rect(guac_display_layer* display_layer,
        guac_rect* dirty) {

    /* Get Cairo surface covering dirty rect */
    unsigned char* buffer = GUAC_DISPLAY_LAYER_STATE_MUTABLE_BUFFER(display_layer->last_frame, *dirty);
    cairo_surface_t* rect;

    /* Use RGB24 if the image is fully opaque */
    if (display_layer->opaque)
        rect = cairo_image_surface_create_for_data(buffer,
                CAIRO_FORMAT_RGB24, guac_rect_width(dirty),
                guac_rect_height(dirty), display_layer->last_frame.buffer_stride);

    /* Otherwise ARGB32 is needed, and the destination must be cleared */
    else
        rect = cairo_image_surface_create_for_data(buffer,
                CAIRO_FORMAT_ARGB32, guac_rect_width(dirty),
                guac_rect_height(dirty), display_layer->last_frame.buffer_stride);

    return rect;

}

/**
 * Sends instructions over the Guacamole connection to clear the given
 * rectangle of the given layer if that layer is non-opaque. This is necessary
 * prior to sending image data to layers with alpha transparency, as image data
 * from multiple updates will otherwise be composited together.
 *
 * @param display_layer
 *     The layer that should possibly be cleared in preparation for a future
 *     drawing operation.
 *
 * @param dirty
 *     The rectangular region of the drawing operation.
 */
static void guac_display_layer_clear_non_opaque(guac_display_layer* display_layer,
        guac_rect* dirty) {

    guac_display* display = display_layer->display;
    const guac_layer* layer = display_layer->layer;

    guac_client* client = display->client;
    guac_socket* socket = client->socket;

    /* Clear destination region only if necessary due to the relevant layer
     * being non-opaque. The rect+cfill pair must be sent atomically to
     * prevent interleaving with concurrent workers operating on the same
     * layer. */
    if (!display_layer->opaque) {

        pthread_mutex_lock(&display_layer->path_lock);

        guac_protocol_send_rect(socket, layer, dirty->left, dirty->top,
                guac_rect_width(dirty), guac_rect_height(dirty));

        guac_protocol_send_cfill(socket, GUAC_COMP_ROUT, layer,
                0x00, 0x00, 0x00, 0xFF);

        pthread_mutex_unlock(&display_layer->path_lock);

    }

}

/**
 * Guesses whether a rectangle within a particular layer would be better
 * compressed as PNG or using a lossy format like JPEG. Positive values
 * indicate PNG is likely to be superior, while negative values indicate the
 * opposite.
 *
 * @param layer
 *     The layer containing the image data to check.
 *
 * @param rect
 *     The rect to check within the given layer.
 *
 * @return
 *     Positive values if PNG compression is likely to perform better than
 *     lossy alternatives, or negative values if PNG is likely to perform
 *     worse.
 */
/* Compile this function with the full -O3 vectorizer pipeline so the
 * per-row counter loop below auto-vectorizes even at a -O2 build.
 * GCC <12 leaves loop vectorization off at -O2; GCC >=12 enables the
 * baseline loop vectorizer but still needs -O3's if-conversion and
 * peeling passes to widen a loop whose body contains a conditional
 * increment like `num_same += (curr == prev)`. The loop is in
 * canonical reduction-accumulator shape, which every modern auto-
 * vectorizer widens to the build target's native vector length:
 * paired 128-bit PCMPEQD on SSE2, 256-bit VPCMPEQD on AVX2, 512-bit
 * on AVX-512, VCEQ on NEON, scalable-vector compares on SVE / RVV. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("O3")
#endif
static int LFR_guac_display_layer_png_optimality(guac_display_layer* layer,
        const guac_rect* rect) {

    int num_same = 0;

    /* Get buffer from layer */
    size_t stride = layer->last_frame.buffer_stride;
    const unsigned char* buffer = GUAC_DISPLAY_LAYER_STATE_CONST_BUFFER(layer->last_frame, *rect);

    int width = rect->right - rect->left;
    int height = rect->bottom - rect->top;

    /* Image must be at least 1x1 */
    if (width < 1 || height < 1)
        return 0;

    /* For each row, count adjacent-pixel matches. Comparisons per row are
     * (width - 1); total comparisons across the rect are height*(width-1).
     * Cross-row comparisons are intentionally excluded - the scanline-at-
     * a-time counting here matches PNG's horizontal-filter model, where
     * adjacent pixels within a scanline are the primary compression
     * signal for DEFLATE.
     *
     * The inner counter loop is deliberately written in the canonical
     * reduction shape `acc += (a[i] == b[i])` rather than the previous
     * hand-SIMD overlapping-load pattern. The two formulations count
     * the same set of same-pixel pairs per row, but the canonical one
     * auto-vectorizes to the build target's native vector width
     * (including AVX-512 and SVE/RVV) while the hand-SIMD version was
     * pinned at 128 bit. The OR with 0xFF000000 normalizes Cairo's
     * undefined RGB24 "x" byte so pixels differing only in that
     * channel are not counted as different. */
    for (int y = 0; y < height; y++) {

        const uint32_t* restrict row = (const uint32_t*) buffer;
        for (int x = 1; x < width; x++) {
            uint32_t prev = row[x - 1] | 0xFF000000u;
            uint32_t curr = row[x]     | 0xFF000000u;
            num_same += (curr == prev);
        }

        buffer += stride;

    }

    /* Derive mismatches from total comparisons so the ratio below stays
     * identical to the scalar implementation. +1 preserves the original
     * division-by-zero guard where num_different was initialized to 1. */
    int num_different = height * (width - 1) - num_same + 1;

    /* Return rough approximation of optimality for PNG compression. As PNG
     * leverages lossless DEFLATE compression (which works by reducing the
     * number of bytes required to represent repeated data), an approximation
     * of the amount of repeated image data within the image is a reasonable
     * approximation for how well an image will compress. */
    return 0x100 * num_same / num_different - 0x400;

}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif

/**
 * The possible image encodings that may be selected for a given dirty region.
 */
typedef enum guac_display_encoding {
    GUAC_DISPLAY_ENCODING_PNG,
    GUAC_DISPLAY_ENCODING_JPEG,
    GUAC_DISPLAY_ENCODING_WEBP
} guac_display_encoding;

/**
 * Minimum bytes-per-pixel we assume a lossless encoding of arbitrary
 * image data will need. Even highly compressible screen content (UI,
 * text) averages around 1 byte per pixel after PNG/WebP-lossless
 * DEFLATE, and photographic content is much higher. Using 1 bpp as the
 * "will lossless fit?" check is conservative - it occasionally
 * under-estimates for extremely repetitive content - but the
 * consequence of over-estimating (staying lossless when budget is
 * tight) is much worse for overall responsiveness than the
 * consequence of under-estimating (going lossy when lossless would
 * have squeaked in).
 */
#define GUAC_DISPLAY_LOSSLESS_BPP_ESTIMATE 1

/**
 * Selects the optimal image encoding for the given rectangle of the given
 * layer, along with the effective lossless flag the encoder should use.
 * This single function replaces the prior separate should_use_jpeg() and
 * should_use_webp() helpers, avoiding duplicate scans of image data
 * needed to approximate PNG optimality.
 *
 * The layer's lossless flag takes absolute priority: a layer marked
 * lossless is always encoded losslessly, regardless of content
 * characteristics or byte budget. Only when a layer permits lossy
 * encoding does the actual lossless-vs-lossy choice come into play,
 * where it's made based on two signals:
 *
 *   - Content compressibility (via PNG optimality): repetitive image
 *     data compresses well with DEFLATE and is better served by
 *     lossless encoding regardless of budget.
 *
 *   - Per-op byte budget: even if content is PNG-friendly, a lossless
 *     encoding (roughly 1 byte/pixel at best) may still exceed the
 *     target_bytes budget. In that case, lossy encoding is preferred
 *     because its quality knob (and WebP's direct target_size) can be
 *     tuned to hit the budget, whereas lossless output size is
 *     whatever the data produces.
 *
 * @param layer
 *     The layer containing the image data to check.
 *
 * @param rect
 *     The rectangular region within the given layer to check.
 *
 * @param framerate
 *     The rate that the region covered by the given rectangle has historically
 *     been being updated within the given layer, in frames per second.
 *
 * @param target_bytes
 *     Per-op byte budget from the client-throughput estimator, or zero if
 *     no budget is available. Zero (or any value on a lossless-only layer)
 *     leaves the decision entirely to the content/framerate heuristics.
 *
 * @param lossless_out
 *     Output: the effective lossless flag the encoder should use. This
 *     mirrors the layer's lossless flag - the budget override switches
 *     the chosen encoding from lossless (PNG) to lossy (WebP/JPEG) but
 *     never forces a lossless-required layer into lossy mode.
 *
 * @param hint_out
 *     Output: a content-nature hint derived from the same optimality
 *     scan that drives the PNG-vs-lossy decision. The PNG encoder uses
 *     this hint (via guac_client_stream_png_hinted) to pick zlib
 *     strategy and PNG filter settings appropriate for the content.
 *     The lossy encoders ignore the value.
 *
 * @return
 *     The encoding that should be used for the given rectangle.
 */
static guac_display_encoding LFR_guac_display_layer_select_encoding(
        guac_display_layer* layer, const guac_rect* rect, int framerate,
        int target_bytes, int* lossless_out, guac_image_hint* hint_out) {

    /* The layer's lossless flag is authoritative. The budget-based
     * override below can only switch the encoding between lossless and
     * lossy options on layers that already permit lossy - it never
     * overrides a layer's lossless-only requirement. */
    int lossless = layer->last_frame.lossless;
    *lossless_out = lossless;

    /* Compute PNG optimality up front so both the encoding decision and
     * the content-nature hint (used by the PNG encoder to pick zlib
     * strategy and filter) are derived from the same signal. The scan
     * is cheap relative to actually encoding a frame - typically
     * sub-microsecond for the kind of per-cell rect sizes the encoder
     * sees - so running it unconditionally is not a measurable cost. */
    int png_optimal =
            LFR_guac_display_layer_png_optimality(layer, rect) >= 0;
    *hint_out = png_optimal ? GUAC_IMAGE_HINT_SYNTHETIC
                            : GUAC_IMAGE_HINT_PHOTOGRAPHIC;

    int webp_supported = guac_client_supports_webp(layer->display->client);

    /* "Lossless choice" helper: for any case where we've decided to
     * use a lossless encoding, ARGB32 (non-opaque) content goes to
     * WebP lossless when the client supports it, and everything else
     * goes to PNG. Benchmarks under benchmark/png/ show WebP
     * lossless is ~2x faster and ~55% smaller than the libpng-direct
     * ARGB32 path; palette-feasible RGB24 content is ~4x faster on
     * PNG's palette path than on WebP lossless, which is why the
     * RGB24 case stays with PNG.
     *
     * *lossless_out is forced to 1 on the WebP route so the encoder
     * actually produces lossless WebP output regardless of the
     * layer's own lossless flag. */
#define GUAC_DISPLAY_PICK_LOSSLESS() do {                        \
        if (!layer->opaque && webp_supported) {                  \
            *lossless_out = 1;                                   \
            return GUAC_DISPLAY_ENCODING_WEBP;                   \
        }                                                        \
        return GUAC_DISPLAY_ENCODING_PNG;                        \
    } while (0)

    /* Neither JPEG nor WebP-lossy is a worthwhile choice if the
     * framerate is low, as the cost of repeated lossy re-encoding
     * for a mostly-static region is not offset by improved
     * compression. */
    if (framerate < GUAC_DISPLAY_JPEG_FRAMERATE)
        GUAC_DISPLAY_PICK_LOSSLESS();

    /* Determine if JPEG is a candidate based on layer/size properties.
     * Note that JPEG cannot be used for non-opaque layers nor for
     * lossless layers. */
    int rect_width = rect->right - rect->left;
    int rect_height = rect->bottom - rect->top;
    int rect_size = rect_width * rect_height;
    int jpeg_candidate = layer->opaque && !lossless
        && rect_size > GUAC_DISPLAY_JPEG_MIN_BITMAP_SIZE;

    /* If neither lossy format is a candidate, lossless is the only
     * option. */
    if (!webp_supported && !jpeg_candidate)
        GUAC_DISPLAY_PICK_LOSSLESS();

    /* Budget-aware lossless-vs-lossy refinement - only ever applies to
     * layers that permit lossy encoding (!lossless). For such a layer,
     * even PNG-friendly content can exceed the per-op byte budget,
     * because DEFLATE's output size is dictated by the data and can't
     * be tuned downward. When that's the case, prefer a lossy encoder
     * whose quality / target_size can be adjusted to fit.
     *
     * Lossless-only layers skip this override entirely - their
     * lossless requirement wins over the budget. */
    if (!lossless && png_optimal && target_bytes > 0) {
        int estimated_lossless_bytes =
                rect_size * GUAC_DISPLAY_LOSSLESS_BPP_ESTIMATE;
        if (estimated_lossless_bytes > target_bytes)
            png_optimal = 0;
    }

    if (png_optimal)
        GUAC_DISPLAY_PICK_LOSSLESS();

#undef GUAC_DISPLAY_PICK_LOSSLESS

    /* Prefer WebP when it is supported. WebP is superior to JPEG for both
     * lossy and lossless use cases. */
    if (webp_supported)
        return GUAC_DISPLAY_ENCODING_WEBP;

    if (jpeg_candidate)
        return GUAC_DISPLAY_ENCODING_JPEG;

    return GUAC_DISPLAY_ENCODING_PNG;

}

void* guac_display_worker_thread(void* data) {

    int framerate;
    int has_outstanding_frames = 0;

    /* The generic pipeline_stage_init passes the stage itself as the
     * pthread arg; our guac_display context is stashed on stage->user_data. */
    guac_display_pipeline_stage* stage = (guac_display_pipeline_stage*) data;
    guac_display* display = (guac_display*) stage->user_data;
    guac_client* client = display->client;
    guac_socket* socket = client->socket;

    guac_display_plan_operation op;
    while (guac_fifo_dequeue_and_lock(&display->encode_stage.fifo, &op)) {

        /* Notify any watchers of render_state that a frame is now in progress */
        guac_flag_set_and_lock(&display->render_state, GUAC_DISPLAY_RENDER_STATE_FRAME_IN_PROGRESS);
        guac_flag_clear(&display->render_state, GUAC_DISPLAY_RENDER_STATE_FRAME_NOT_IN_PROGRESS);
        guac_flag_unlock(&display->render_state);

        /* NOTE: Any thread that locks the operation queue can know that there
         * are no pending operations in progress if the queue is empty and
         * there are no active workers */
        display->active_workers++;
        guac_fifo_unlock(&display->encode_stage.fifo);

        guac_rwlock_acquire_read_lock(&display->last_frame_lock);
        guac_display_layer* display_layer = op.layer;
        switch (op.type) {

            case GUAC_DISPLAY_PLAN_OPERATION_IMG: {

                /* Stamp the IMG-total start before any per-op work so
                 * everything - cairo setup, clear, select, stream call,
                 * cairo destroy - is attributed to this op's budget. */
                int64_t t_img_start = guac_display_monotonic_us();

                framerate = INT_MAX;
                if (op.current_frame > op.last_frame)
                    framerate = 1000 / (op.current_frame - op.last_frame);

                guac_rect* dirty = &op.dest;

                /* TODO: Stream PNG/WebP/JPEG using progressive encoding such
                 * that a frame that is currently being encoded can be
                 * preempted by the next frame, with the connected client then
                 * simply receiving a lower-quality intermediate frame. If
                 * necessary, progressive encoding can be achieved by manually
                 * dividing images into multiple reduced-resolution stages,
                 * such that each image streamed is actually only one quarter
                 * the size of the original image. Compositing via Guacamole
                 * protocol instructions can reassemble those stages. */

                cairo_surface_t* rect = LFR_guac_display_layer_cairo_rect(display_layer, dirty);
                const guac_layer* layer = display_layer->layer;

                /* Clear relevant rect of destination layer if necessary to
                 * ensure fresh data is not drawn on top of old data for layers
                 * with alpha transparency */
                guac_display_layer_clear_non_opaque(display_layer, dirty);

                /* Compute this op's dynamic share of the remaining
                 * per-frame byte budget before picking an encoding.
                 * The plan stamped op.lossy_quality / op.target_bytes
                 * with initial frame-level values; here we replace
                 * them with fresh values computed from the live pool,
                 * so any overage or surplus from already-encoded ops
                 * tightens or loosens this op's quality accordingly.
                 *
                 * Quality is the only encoder knob we use (target_size
                 * costs too much encode time to drive via
                 * WebPConfig::pass, per benchmark/webp/); target_bytes
                 * stays on the op because select_encoding uses it to
                 * gate the lossless → lossy override on PNG-optimal
                 * content whose lossless output would exceed budget.
                 *
                 * Both atomics are relaxed - strict ordering across
                 * workers isn't required, only a consistent-enough
                 * view that each op sees a monotonically-shrinking
                 * pool. Concurrent readers-before-writers produce
                 * slightly-loose shares for the early ops of a
                 * batch; that self-corrects as more ops complete
                 * and the pool shrinks. */
                int rem_bytes = atomic_load_explicit(
                        &display->remaining_frame_bytes,
                        memory_order_relaxed);
                int rem_pixels = atomic_load_explicit(
                        &display->remaining_frame_pixels,
                        memory_order_relaxed);
                int op_pixels = guac_rect_width(dirty)
                        * guac_rect_height(dirty);

                if (rem_bytes > 0 && rem_pixels > 0) {
                    /* int64 intermediate avoids overflow on big
                     * frames (rem_bytes up to ~1 MB, op_pixels up
                     * to ~512k - their product fits in 40 bits). */
                    int64_t share = ((int64_t) rem_bytes
                            * (int64_t) op_pixels) / (int64_t) rem_pixels;
                    if (share < 1) share = 1;
                    op.target_bytes = (int) share;
                    op.lossy_quality = guac_display_quality_for_budget(
                            (double) rem_bytes / (double) rem_pixels);
                }
                else {
                    /* Budget exhausted or never set - fall through
                     * to quality-only mode with the plan's initial
                     * quality stamp (no adjustment from here). */
                    op.target_bytes = 0;
                }

                /* Select best encoding once (avoids repeated per-op calls to
                 * guac_client_get_processing_lag() and duplicate pixel scans
                 * for PNG optimality). select_encoding may override the
                 * layer's lossless preference based on this op's byte
                 * budget: if a lossless encoding wouldn't fit within
                 * op.target_bytes, it returns a lossy encoding choice
                 * (WebP with lossless=0, or JPEG) and sets effective_
                 * lossless accordingly. The content-nature hint is
                 * populated from the same optimality scan and is
                 * forwarded to the PNG encoder below via
                 * guac_client_stream_png_hinted so the encoder can
                 * pick zlib strategy and filter appropriately. */
                int effective_lossless;
                guac_image_hint png_hint;
                guac_display_encoding encoding = LFR_guac_display_layer_select_encoding(
                        display_layer, dirty, framerate,
                        op.target_bytes, &effective_lossless, &png_hint);

                /* Stamp the codec-call start right before the stream_*
                 * dispatch so the codec_us atomic isolates actual
                 * PNG/WebP/JPEG encode + socket-write time from the
                 * surrounding cairo setup, optimality scan, clear, etc. */
                int64_t t_codec_start = guac_display_monotonic_us();

                /* Snapshot socket bytes around the stream call so the
                 * per-op log can report actual bytes emitted against
                 * the budget (op.target_bytes). The broadcast socket
                 * is shared across workers, but per-op deltas are
                 * still meaningful as long as we read before and
                 * after THIS op's stream call with no intervening
                 * writes from the same thread - which is the case,
                 * since the only write between these two snapshots
                 * is the stream_*() dispatched below. Other workers
                 * may write concurrently; those bytes add noise to
                 * the delta but keep the overall order-of-magnitude
                 * meaningful for debugging. */
                size_t bytes_pre = guac_socket_bytes_written(socket);

                switch (encoding) {

                    case GUAC_DISPLAY_ENCODING_WEBP:
                        guac_client_stream_webp_hinted(client, socket, GUAC_COMP_OVER, layer,
                                dirty->left, dirty->top, rect,
                                op.lossy_quality,
                                op.target_bytes,
                                effective_lossless,
                                png_hint);
                        break;

                    case GUAC_DISPLAY_ENCODING_JPEG:
                        guac_client_stream_jpeg(client, socket, GUAC_COMP_OVER, layer,
                                dirty->left, dirty->top, rect,
                                op.lossy_quality);
                        break;

                    case GUAC_DISPLAY_ENCODING_PNG:
                    default:
                        guac_client_stream_png_hinted(client, socket, GUAC_COMP_OVER,
                                layer, dirty->left, dirty->top, rect, png_hint);
                        break;

                }

                size_t bytes_post = guac_socket_bytes_written(socket);
                size_t bytes_actual = bytes_post > bytes_pre
                        ? bytes_post - bytes_pre : 0;

                /* Amortise this op's actual emit onto the remaining
                 * per-frame budget: subtract the bytes we just wrote
                 * from remaining_frame_bytes and this op's pixels
                 * from remaining_frame_pixels. Subsequent ops will
                 * compute a tighter share if we overshot, a looser
                 * share if we undershot. */
                atomic_fetch_sub_explicit(&display->remaining_frame_bytes,
                        (int) bytes_actual, memory_order_relaxed);
                atomic_fetch_sub_explicit(&display->remaining_frame_pixels,
                        op_pixels, memory_order_relaxed);

                int op_w = dirty->right - dirty->left;
                int op_h = dirty->bottom - dirty->top;
                const char* encoding_name =
                        encoding == GUAC_DISPLAY_ENCODING_WEBP ? "WEBP"
                      : encoding == GUAC_DISPLAY_ENCODING_JPEG ? "JPEG"
                      : "PNG";
                guac_client_log(client, GUAC_LOG_TRACE,
                        "IMG op: encoding=%s %ix%i lossless=%i "
                        "quality=%i target_bytes=%i actual_bytes=%zu",
                        encoding_name, op_w, op_h, effective_lossless,
                        op.lossy_quality, op.target_bytes, bytes_actual);

                int64_t t_codec_end = guac_display_monotonic_us();

                cairo_surface_destroy(rect);

                int64_t t_img_end = guac_display_monotonic_us();

                /* Publish per-op contributions to this frame's encode
                 * accounting atomics. Relaxed ordering is fine - the
                 * end-of-frame branch's encode_stage.fifo lock
                 * establishes happens-before for the summary read. */
                atomic_fetch_add_explicit(&display->encode_img_us,
                        (size_t) (t_img_end - t_img_start),
                        memory_order_relaxed);
                atomic_fetch_add_explicit(&display->encode_codec_us,
                        (size_t) (t_codec_end - t_codec_start),
                        memory_order_relaxed);

                break;
            }

            case GUAC_DISPLAY_PLAN_OPERATION_COPY:
            case GUAC_DISPLAY_PLAN_OPERATION_RECT:
                guac_client_log(client, GUAC_LOG_DEBUG, "Operation type %i "
                        "should NOT be present in the set of operations given "
                        "to guac_display worker thread. All operations except "
                        "IMG and NOP are handled during the initial, "
                        "single-threaded flush step. This is likely a bug.",
                        op.type);
                break;

            case GUAC_DISPLAY_PLAN_OPERATION_NOP:
                /* Do nothing */
                break;

        }

        guac_fifo_lock(&display->encode_stage.fifo);

        /* If we're the only active worker and there are no further operations
         * pending, we've reached the end of the frame, and this is the worker
         * that will be sending that boundary to connected users */
        if (!(display->encode_stage.fifo.state.value & GUAC_FIFO_STATE_NONEMPTY) && display->active_workers == 1) {

            /* Update the mouse cursor if it's been changed since the
             * last frame */
            guac_display_layer* cursor = display->cursor_buffer;
            if (!guac_rect_is_empty(&cursor->last_frame.dirty)) {
                guac_protocol_send_cursor(client->socket,
                        display->last_frame.cursor_hotspot_x,
                        display->last_frame.cursor_hotspot_y,
                        cursor->layer, 0, 0,
                        cursor->last_frame.width,
                        cursor->last_frame.height);
            }

            /* Pass this frame's server-side active-pipeline
             * duration on to guac_client_end_multiple_frames via a
             * client-level side channel. __record_sync_emit reads
             * this field and stamps it into each user's
             * sync_emit_history entry, where the ack handler uses
             * it directly as the throughput-sample denominator -
             * avoiding the ack-interval-based denominator that
             * would include pre-pipeline idle time (outer wait,
             * modification accumulation, pacing) and bias the EMA.
             *
             * display->frame_start was stamped by the render thread
             * inside guac_display_end_multiple_frames() right after
             * it set FLUSH_IN_PROGRESS; the current time here is
             * effectively the sync emit time (since the sync is
             * about to be written). The one-flush-at-a-time gate
             * guarantees no other flush is between here and the
             * foreach_user call inside end_multiple_frames, so a
             * plain store without synchronization is safe. */
            client->current_frame_render_time_ms =
                    (int) (guac_timestamp_current() - display->frame_start);

            /* Allow connected clients to move forward with rendering */
            guac_client_end_multiple_frames(client, display->last_frame.frames);

            /* While connected clients moves forward with rendering,
             * commit any changed contents to client-side backing buffer */
            guac_display_layer* current = display->last_frame.layers;
            while (current != NULL) {

                /* Save a copy of the changed region if the layer has
                 * been modified since the last frame */
                guac_rect* dirty = &current->last_frame.dirty;
                if (!guac_rect_is_empty(dirty)) {

                    int x = dirty->left;
                    int y = dirty->top;
                    int width = guac_rect_width(dirty);
                    int height = guac_rect_height(dirty);

                    /* Ensure destination region is cleared out first if the alpha channel need be considered,
                     * as GUAC_COMP_OVER is significantly faster than GUAC_COMP_SRC on the browser side */
                    if (!current->opaque) {
                        guac_protocol_send_rect(client->socket, current->last_frame_buffer, x, y, width, height);
                        guac_protocol_send_cfill(client->socket, GUAC_COMP_RATOP, current->last_frame_buffer,
                                0x00, 0x00, 0x00, 0x00);
                    }

                    guac_protocol_send_copy(client->socket,
                            current->layer, x, y, width, height,
                            GUAC_COMP_OVER, current->last_frame_buffer, x, y);

                }

                current = current->last_frame.next;

            }

            /* This is now absolutely everything for the current frame,
             * and it's safe to flush any outstanding data */
            guac_socket_flush(client->socket);

            /* Notify any watchers of render_state that a frame is no longer in progress */
            guac_flag_set_and_lock(&display->render_state, GUAC_DISPLAY_RENDER_STATE_FRAME_NOT_IN_PROGRESS);
            guac_flag_clear(&display->render_state, GUAC_DISPLAY_RENDER_STATE_FRAME_IN_PROGRESS);
            guac_flag_unlock(&display->render_state);

            /* Emit encode-phase end and overall-frame-duration logs.
             * Pairs with the "Frame encode: start=+Xms ops=N" log
             * emitted by the commit handler right after it finished
             * pushing IMG ops onto encode_stage.fifo.
             *
             * The breakdown line reports:
             *   - worker_sum: total wall time spent by workers on IMG
             *     handling (sum across workers). Divided by the wall
             *     dur of the encode phase, this gives the effective
             *     parallelism of the encode pool. A value close to
             *     encode_stage.thread_count means workers were busy
             *     the whole phase; a smaller value means some workers
             *     idled (either too few ops for the pool, uneven op
             *     cost distribution, or serialization somewhere).
             *   - codec_sum: time spent strictly inside guac_client_
             *     stream_png/webp/jpeg calls, summed across workers.
             *     The remainder (worker_sum - codec_sum) is per-op
             *     overhead: cairo surface setup/destroy, the PNG
             *     optimality scan, clear_non_opaque, protocol bytes
             *     that surround the stream_* call, FIFO and lock
             *     traffic. */
            guac_timestamp frame_end = guac_timestamp_current();
            int dur_ms = (int) (frame_end - display->encode_start);
            size_t img_us = atomic_load_explicit(&display->encode_img_us,
                    memory_order_relaxed);
            size_t codec_us = atomic_load_explicit(&display->encode_codec_us,
                    memory_order_relaxed);

            guac_client_log(client, GUAC_LOG_TRACE,
                    "Frame encode: end=+%ims dur=%ims",
                    (int) (frame_end - display->frame_start),
                    dur_ms);
            guac_client_log(client, GUAC_LOG_TRACE,
                    "Frame encode breakdown: worker_sum=%" PRIu64 "us "
                    "codec_sum=%" PRIu64 "us "
                    "(codec_fraction=%d%%, parallelism=%d.%02dx/%d)",
                    (uint64_t) img_us, (uint64_t) codec_us,
                    img_us > 0 ? (int) ((codec_us * 100) / img_us) : 0,
                    dur_ms > 0 ? (int) (img_us / ((size_t) dur_ms * 1000)) : 0,
                    dur_ms > 0 ? (int) (((img_us * 100) / ((size_t) dur_ms * 1000)) % 100) : 0,
                    display->encode_stage.thread_count);
            guac_client_log(client, GUAC_LOG_TRACE,
                    "Frame processed: total=%ims",
                    (int) (frame_end - display->frame_start));

            /* Release the flush gate so guac_display_end_multiple_frames()
             * will stop deferring subsequent frames. */
            guac_flag_clear(&display->flush_state, GUAC_DISPLAY_FLUSH_IN_PROGRESS);

            has_outstanding_frames = display->frame_deferred;

        }

        display->active_workers--;
        guac_fifo_unlock(&display->encode_stage.fifo);

        guac_rwlock_release_lock(&display->last_frame_lock);

        /* Trigger additional flush if frames were completed while we were
         * still processing the previous frame */
        if (has_outstanding_frames) {
            guac_display_end_multiple_frames(display, 0);
            has_outstanding_frames = 0;
        }

    }

    return NULL;

}
