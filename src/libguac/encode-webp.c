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

#include "encode-webp.h"
#include "guacamole/error.h"
#include "guacamole/protocol.h"
#include "guacamole/stream.h"
#include "image-util.h"
#include "palette.h"

#include <cairo/cairo.h>
#include <webp/encode.h>

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * libwebp "method" setting - the encoder's internal compression effort
 * knob. The valid range is 0-6 with the following meaning:
 *
 *    - 0: fastest encode, largest output.
 *    - 6: slowest encode, smallest output.
 *
 * Per-frame encode-phase profiling on typical display workloads shows
 * the codec accounts for ~99% of worker-active time; per-op encode
 * time is therefore the dominant factor in the display pipeline's
 * end-to-end frame latency. Dropping method from 2 to 0 empirically
 * reduces per-op encode time by ~3-4x (e.g. 30-40ms -> ~10ms on heavy
 * frames) at the cost of ~10-20% larger WebP output bytes. On
 * networks where bandwidth is not the binding constraint, this
 * tradeoff is heavily favorable - the saved encode time turns into
 * higher end-to-end framerate.
 *
 * If a future workload lands where bandwidth IS the binding
 * constraint (tight target_bytes budgets, slow client link), a higher
 * method here produces smaller output at the cost of encode time.
 * The existing target_bytes-driven quantizer loop can make up some
 * of the size difference at method=0, so method=0 remains a
 * reasonable default even under byte budgeting; if profiling shows
 * otherwise, adaptive method selection here based on target_bytes
 * vs image area is a straightforward follow-up.
 */
#define GUAC_WEBP_METHOD 0

/**
 * Structure which describes the current state of the WebP image writer.
 */
typedef struct guac_webp_stream_writer {

    /**
     * The socket over which all WebP blobs will be written.
     */
    guac_socket* socket;

    /**
     * The Guacamole stream to associate with each WebP blob.
     */
    guac_stream* stream;

    /**
     * Buffer of pending WebP data.
     */
    char buffer[GUAC_PROTOCOL_BLOB_MAX_LENGTH];

    /**
     * The number of bytes currently stored in the buffer.
     */
    int buffer_size;

} guac_webp_stream_writer;

/**
 * Writes the contents of the WebP stream writer as a blob to its associated
 * socket.
 *
 * @param writer
 *     The writer structure to flush.
 */
static void guac_webp_flush_data(guac_webp_stream_writer* writer) {

    /* Send blob */
    guac_protocol_send_blob(writer->socket, writer->stream,
            writer->buffer, writer->buffer_size);

    /* Clear buffer */
    writer->buffer_size = 0;

}

/**
 * Configures the given stream writer object to use the given Guacamole stream
 * object for WebP output.
 *
 * @param writer
 *     The Guacamole WebP stream writer structure to configure.
 *
 * @param socket
 *     The Guacamole socket to use when sending blob instructions.
 *
 * @param stream
 *     The stream over which WebP-encoded blobs of image data should be sent.
 */
static void guac_webp_stream_writer_init(guac_webp_stream_writer* writer,
        guac_socket* socket, guac_stream* stream) {

    writer->buffer_size = 0;

    /* Store Guacamole-specific objects */
    writer->socket = socket;
    writer->stream = stream;

}

/**
 * WebP output function which appends the given WebP data to the internal
 * buffer of the Guacamole stream writer structure, automatically flushing the
 * writer as necessary.
 *
 * @param data
 *     The segment of data to write.
 *
 * @param data_size
 *     The size of segment of data to write.
 *
 * @param picture
 *     The WebP picture associated with this write operation. Provides access to
 *     picture->custom_ptr which contains the Guacamole stream writer structure.
 *
 * @return
 *     Non-zero if writing was successful, zero on failure.
 */
static int guac_webp_stream_write(const uint8_t* data, size_t data_size,
        const WebPPicture* picture) {

    guac_webp_stream_writer* const writer =
        (guac_webp_stream_writer*) picture->custom_ptr;
    assert(writer != NULL);

    const unsigned char* current = data;
    int length = data_size;

    /* Append all data given */
    while (length > 0) {

        /* Calculate space remaining */
        int remaining = sizeof(writer->buffer) - writer->buffer_size;

        /* If no space remains, flush buffer to make room */
        if (remaining == 0) {
            guac_webp_flush_data(writer);
            remaining = sizeof(writer->buffer);
        }

        /* Calculate size of next block of data to append */
        int block_size = remaining;
        if (block_size > length)
            block_size = length;

        /* Append block */
        memcpy(writer->buffer + writer->buffer_size,
               current, block_size);

        /* Next block */
        current += block_size;
        writer->buffer_size += block_size;
        length -= block_size;

    }

    return 1;
}

int guac_webp_write(guac_socket* socket, guac_stream* stream,
        cairo_surface_t* surface, int quality, int target_bytes,
        int lossless, guac_image_hint hint) {

    /* The hint is accepted for API symmetry with guac_png_write but
     * not currently acted upon - the libwebp parameter benchmark at
     * benchmark/webp/ found method=0 uniformly fastest across all
     * tested content types, so there is no content-dependent branch
     * that the hint would drive today. Mark as explicitly unused to
     * satisfy -Werror=unused-parameter in the interim; remove the
     * cast if future benchmarks motivate actual hint-driven tuning. */
    (void) hint;

    guac_webp_stream_writer writer;
    WebPPicture picture;
    uint32_t* argb_output;

    int y;

    int width = cairo_image_surface_get_width(surface);
    int height = cairo_image_surface_get_height(surface);
    int stride = cairo_image_surface_get_stride(surface);
    cairo_format_t format = cairo_image_surface_get_format(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);

    if (format != CAIRO_FORMAT_RGB24 && format != CAIRO_FORMAT_ARGB32) {
        guac_error = GUAC_STATUS_INTERNAL_ERROR;
        guac_error_message = "Invalid Cairo image format. Unable to create WebP.";
        return -1;
    }

    /* Flush pending operations to surface */
    cairo_surface_flush(surface);

    /* Configure WebP compression bits */
    WebPConfig config;
    if (!WebPConfigPreset(&config, WEBP_PRESET_DEFAULT, quality))
        return -1;

    /* Add additional tuning */
    config.lossless = lossless;
    config.quality = quality;
    config.thread_level = 0; /* NOT multi-threaded (threading results in unnecessary overhead vs. the worker threads used by guac_display) */
    config.method = GUAC_WEBP_METHOD;

    /* target_bytes is accepted but intentionally unused as a
     * direct WebPConfig::target_size. libwebp only iterates the
     * quantizer toward target_size when WebPConfig::pass > 1, and
     * the extra passes roughly multiply encode time (pass=5 ≈ 2×,
     * pass=10 ≈ 3×, per benchmark/webp/benchmark-target-size.c).
     * That encoding-time cost directly regresses our server frame
     * rate. Instead, the caller is expected to have already mapped
     * its byte budget onto the WebPConfig::quality setting via a
     * bpp → quality heuristic (guac_display_quality_for_budget in
     * display-plan.c); we stay at pass=1 (libwebp's default) and
     * trust that mapping. Frames that overshoot naturally tighten
     * later frames' budgets through the dynamic per-op
     * remaining_frame_bytes / remaining_frame_pixels atomics, and
     * a server that's consistently overshooting will be throttled
     * by the frame-pacing wait - both of which respond faster
     * than a 3× slowdown in the per-op codec path. */
    (void) target_bytes;

    /* Validate configuration */
    if (!WebPValidateConfig(&config)) {
        return -1;
    }

    /* Set up WebP picture */
    if (!WebPPictureInit(&picture)) {
        return -1;
    }
    picture.use_argb = 1;
    picture.width = width;
    picture.height = height;

    /* Allocate and init writer */
    if (!WebPPictureAlloc(&picture)) {
        return -1;
    }
    picture.writer = guac_webp_stream_write;
    picture.custom_ptr = &writer;
    guac_webp_stream_writer_init(&writer, socket, stream);

    /* Copy image data into WebP picture. The Cairo-to-libwebp-argb
     * transfer is the only per-pixel work between libguac and libwebp,
     * so we hoist the format check out of the per-pixel loop to isolate
     * two clean block-transfer paths:
     *
     *   - CAIRO_FORMAT_ARGB32: bytes are already in libwebp's expected
     *     layout; a per-row memcpy (libc's vectorized DEFLATE-free
     *     block copy) is sufficient. libwebp's internal argb buffer is
     *     aligned and its argb_stride accommodates any padding, so we
     *     just copy width × 4 bytes per row and let the stride offsets
     *     skip any internal padding rows.
     *
     *   - CAIRO_FORMAT_RGB24: Cairo stores BGRx with an undefined upper
     *     byte; libwebp needs the alpha channel forced to 0xFF. We
     *     broadcast 0xFF000000 into an 8-wide vector once, then OR it
     *     into 8 pixels at a time (compiles to VPOR on AVX2, paired
     *     POR on SSE2). The scalar tail handles 0-7 leftover pixels. */
    argb_output = picture.argb;
    int row_bytes = width * (int) sizeof(uint32_t);

    if (format == CAIRO_FORMAT_ARGB32) {

        for (y = 0; y < height; y++) {
            memcpy(argb_output, data, row_bytes);
            data += stride;
            argb_output += picture.argb_stride;
        }

    }
    else {

        /* Per-row BGRx → ARGB (alpha=0xFF) copy. Delegated to a shared
         * auto-vectorized helper in image-util so this path scales
         * with the build target's native vector width (128-bit SSE2
         * through 512-bit AVX-512, NEON quadword through SVE, RVV)
         * instead of the hand-picked 256-bit width we used to pin it
         * at. */
        for (y = 0; y < height; y++) {
            guac_image_force_alpha_row(argb_output, (uint32_t*) data, width);
            data += stride;
            argb_output += picture.argb_stride;
        }

    }

    /* Encode image */
    const int result = WebPEncode(&config, &picture) ? 0 : -1;

    /* Free picture */
    WebPPictureFree(&picture);

    /* Ensure all data is written */
    guac_webp_flush_data(&writer);

    return result;

}

