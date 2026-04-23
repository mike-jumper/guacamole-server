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

#include "encode-png.h"
#include "guacamole/mem.h"
#include "guacamole/error.h"
#include "guacamole/protocol.h"
#include "guacamole/stream.h"
#include "image-util.h"
#include "palette.h"

#include <png.h>
#include <cairo/cairo.h>
#include <zlib.h>

#ifdef HAVE_PNGSTRUCT_H
#include <pngstruct.h>
#endif

#include <inttypes.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * zlib compression level for the palette-PNG encode path. See the
 * benchmark at benchmark/png/ for the full sweep behind this choice.
 * Level 3 produces output within ~3-5% of level 6's size on palette
 * content while running ~45% faster, which is the right tradeoff for
 * a latency-critical interactive encoder. Level 9 costs 5-10x more
 * time for an additional 1-3% size reduction and is never worth it
 * here.
 */
#define GUAC_PNG_PALETTE_COMPRESSION_LEVEL 3

/**
 * Applies encoding-hint-derived zlib strategy and PNG filter settings
 * to the libpng write struct. Used by both the RGB24-direct and
 * ARGB32-direct encoders - the palette path has its own separately
 * optimized settings and does not call this helper.
 *
 * Hints:
 *   - GUAC_IMAGE_HINT_SYNTHETIC: Z_DEFAULT_STRATEGY with PNG_FILTER_UP.
 *     Default strategy keeps LZ77 matching enabled so long runs of
 *     identical or near-identical pixels (typical of text, UI, icons)
 *     compress tightly. PNG_FILTER_UP predicts each pixel from the
 *     pixel directly above it, which gives near-zero residuals on
 *     vertically-repeating scanlines such as flat background regions.
 *     Measured ~28% faster than libpng defaults at similar size.
 *
 *   - GUAC_IMAGE_HINT_PHOTOGRAPHIC: Z_HUFFMAN_ONLY with PNG_FILTER_UP.
 *     Huffman-only skips the LZ77 matching pass entirely, which is
 *     the dominant cost at level>=1 and which produces near-zero
 *     benefit on photographic content (adjacent pixels rarely match
 *     exactly). PNG_FILTER_UP is still the best single filter here
 *     because photograph-like content has strong vertical coherence.
 *     Measured ~4.5-5x faster than libpng defaults on
 *     photograph-like content, with about 1% larger output.
 *
 *   - GUAC_IMAGE_HINT_UNKNOWN (or any unexpected value): leave
 *     libpng's built-in defaults (level 6, default strategy, adaptive
 *     filter) in place. This is conservative - any caller that has
 *     no opinion on content falls back to the general-purpose
 *     tradeoff libpng ships with.
 */
static void guac_png_apply_hint(png_structp png, guac_image_hint hint) {
    switch (hint) {

        case GUAC_IMAGE_HINT_SYNTHETIC:
            png_set_compression_strategy(png, Z_DEFAULT_STRATEGY);
            png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_UP);
            break;

        case GUAC_IMAGE_HINT_PHOTOGRAPHIC:
            png_set_compression_strategy(png, Z_HUFFMAN_ONLY);
            png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_UP);
            break;

        case GUAC_IMAGE_HINT_UNKNOWN:
        default:
            /* Leave libpng's built-in defaults unchanged. */
            break;

    }
}

/**
 * Data describing the current write state of PNG data.
 */
typedef struct guac_png_write_state {

    /**
     * The socket over which all PNG blobs will be written.
     */
    guac_socket* socket;

    /**
     * The Guacamole stream to associate with each PNG blob.
     */
    guac_stream* stream;

    /**
     * Buffer of pending PNG data.
     */
    char buffer[GUAC_PROTOCOL_BLOB_MAX_LENGTH];

    /**
     * The number of bytes currently stored in the buffer.
     */
    int buffer_size;

} guac_png_write_state;

/**
 * Writes the contents of the PNG write state as a blob to its associated
 * socket.
 *
 * @param write_state
 *     The write state to flush.
 */
static void guac_png_flush_data(guac_png_write_state* write_state) {

    /* Send blob */
    guac_protocol_send_blob(write_state->socket, write_state->stream,
            write_state->buffer, write_state->buffer_size);

    /* Clear buffer */
    write_state->buffer_size = 0;

}

/**
 * Appends the given PNG data to the internal buffer of the given write state,
 * automatically flushing the write state as necessary.
 * socket.
 *
 * @param write_state
 *     The write state to append the given data to.
 *
 * @param data
 *     The PNG data to append.
 *
 * @param length
 *     The size of the given PNG data, in bytes.
 */
static void guac_png_write_data(guac_png_write_state* write_state,
        const void* data, int length) {

    const unsigned char* current = data;

    /* Append all data given */
    while (length > 0) {

        /* Calculate space remaining */
        int remaining = sizeof(write_state->buffer) - write_state->buffer_size;

        /* If no space remains, flush buffer to make room */
        if (remaining == 0) {
            guac_png_flush_data(write_state);
            remaining = sizeof(write_state->buffer);
        }

        /* Calculate size of next block of data to append */
        int block_size = remaining;
        if (block_size > length)
            block_size = length;

        /* Append block */
        memcpy(write_state->buffer + write_state->buffer_size,
                current, block_size);

        /* Next block */
        current += block_size;
        write_state->buffer_size += block_size;
        length -= block_size;

    }

}

/**
 * Writes the given buffer of PNG data to the buffer of the given write state,
 * flushing that buffer to blob instructions if necessary. This handler is
 * called by Cairo when writing PNG data via
 * cairo_surface_write_to_png_stream().
 *
 * @param closure
 *     Pointer to arbitrary data passed to cairo_surface_write_to_png_stream().
 *     In the case of this handler, this data will be the guac_png_write_state.
 *
 * @param data
 *     The buffer of PNG data to write.
 * 
 * @param length
 *     The size of the given buffer, in bytes.
 *
 * @return
 *     A Cairo status code indicating whether the write operation succeeded.
 *     In the case of this handler, this will always be CAIRO_STATUS_SUCCESS.
 */
static cairo_status_t guac_png_cairo_write_handler(void* closure,
        const unsigned char* data, unsigned int length) {

    guac_png_write_state* write_state = (guac_png_write_state*) closure;

    /* Append data to buffer, writing as necessary */
    guac_png_write_data(write_state, data, length);

    return CAIRO_STATUS_SUCCESS;

}

/**
 * Implementation of guac_png_write() which uses Cairo's own PNG encoder to
 * write PNG data, rather than using libpng directly.
 *
 * @param socket
 *     The socket to send PNG blobs over.
 *
 * @param stream
 *     The stream to associate with each blob.
 *
 * @param surface
 *     The Cairo surface to write to the given stream and socket as PNG blobs.
 *
 * @return
 *     Zero if the encoding operation is successful, non-zero otherwise.
 */
/* Forward declarations - the libpng write/flush callback handlers are
 * defined further down (they're shared with the palette path) but both
 * guac_png_rgb24_direct_write() below and the existing palette path
 * reference them via png_set_write_fn(). */
static void guac_png_write_handler(png_structp png, png_bytep data,
        png_size_t length);
static void guac_png_flush_handler(png_structp png);

/**
 * Encodes an RGB24 Cairo surface as a PNG using libpng directly, feeding
 * each row through guac_image_bgrx_to_rgb_row() to convert from Cairo's
 * BGRx layout to libpng's packed RGB without the per-pixel scalar copy
 * Cairo would otherwise perform internally. Callable for surfaces where
 * cairo_image_surface_get_format() == CAIRO_FORMAT_RGB24; for any other
 * format, the caller should fall back to Cairo's own PNG writer (for
 * formats other than ARGB32) or to guac_png_argb32_direct_write() (for
 * ARGB32).
 *
 * @param socket
 *     The socket to send PNG blobs over.
 *
 * @param stream
 *     The stream to associate with each blob.
 *
 * @param surface
 *     The RGB24 Cairo surface to encode.
 *
 * @param hint
 *     Advisory hint describing the nature of the image content, used to
 *     pick zlib strategy and filter. See guac_png_apply_hint().
 *
 * @return
 *     Zero on success, non-zero on failure.
 */
static int guac_png_rgb24_direct_write(guac_socket* socket,
        guac_stream* stream, cairo_surface_t* surface, guac_image_hint hint) {

    png_structp png = NULL;
    png_infop png_info = NULL;
    guac_png_write_state write_state;
    unsigned char* row_buffer = NULL;

    int width = cairo_image_surface_get_width(surface);
    int height = cairo_image_surface_get_height(surface);
    int stride = cairo_image_surface_get_stride(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);

    /* Create libpng write struct */
    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png == NULL) {
        guac_error = GUAC_STATUS_INTERNAL_ERROR;
        guac_error_message = "libpng failed to create write structure";
        return -1;
    }

    png_info = png_create_info_struct(png);
    if (png_info == NULL) {
        png_destroy_write_struct(&png, NULL);
        guac_error = GUAC_STATUS_INTERNAL_ERROR;
        guac_error_message = "libpng failed to create info structure";
        return -1;
    }

    /* Error handler returns via longjmp */
    if (setjmp(png_jmpbuf(png))) {
        guac_mem_free(row_buffer);
        png_destroy_write_struct(&png, &png_info);
        guac_error = GUAC_STATUS_IO_ERROR;
        guac_error_message = "libpng output error";
        return -1;
    }

    /* Init write state + routing to Guacamole blob stream */
    write_state.socket = socket;
    write_state.stream = stream;
    write_state.buffer_size = 0;
    png_set_write_fn(png, &write_state,
            guac_png_write_handler, guac_png_flush_handler);

    /* Hint-driven strategy + filter selection; no-op for UNKNOWN. */
    guac_png_apply_hint(png, hint);

    /* 8-bit RGB output (alpha channel from Cairo's BGRx is undefined and
     * deliberately stripped). */
    png_set_IHDR(png, png_info, width, height, 8,
            PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
            PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, png_info);

    /* Allocate a single row buffer to hold converted RGB data (3 bytes
     * per pixel). libpng copies the row into its compression pipeline
     * before png_write_row() returns, so one buffer reused across all
     * rows is fine. */
    row_buffer = (unsigned char*) guac_mem_alloc(width, 3);

    for (int y = 0; y < height; y++) {
        guac_image_bgrx_to_rgb_row(row_buffer, data, width);
        png_write_row(png, row_buffer);
        data += stride;
    }

    png_write_end(png, png_info);

    guac_mem_free(row_buffer);
    png_destroy_write_struct(&png, &png_info);

    /* Ensure any buffered blob data is flushed */
    guac_png_flush_data(&write_state);
    return 0;

}

/**
 * Reciprocal-alpha lookup table used by guac_png_bgra_premult_to_rgba_row
 * to un-premultiply the color channels of Cairo's ARGB32 pixels when
 * emitting PNG's straight-alpha RGBA rows. Index a holds
 * floor((255 * 65536 + a/2) / a) so the per-channel division reduces to
 * a table lookup plus one multiply and a shift; entry 0 is a sentinel
 * never used (the per-pixel code short-circuits the fully-transparent
 * case to all-zero output).
 *
 * Populated on first use by guac_png_init_alpha_recip().
 */
static uint32_t guac_png_alpha_recip[256];

static void guac_png_init_alpha_recip(void) {
    /* Double-init is harmless - the computed values are deterministic
     * and writes are idempotent. Any minor redundant work here is
     * well below the cost of a pthread_once barrier. */
    static int inited = 0;
    if (inited)
        return;
    guac_png_alpha_recip[0] = 0;
    for (int a = 1; a < 256; a++)
        guac_png_alpha_recip[a] =
                (255u * 65536u + (unsigned) a / 2u) / (unsigned) a;
    inited = 1;
}

/**
 * Converts a single pixel from Cairo ARGB32 (premultiplied BGRA) to
 * PNG's RGBA straight-alpha layout. Extracted so both the scalar
 * fallback and the SIMD path below can share the same per-pixel
 * slow-path logic; the SIMD path only beats this helper when it can
 * skip the un-premultiplication entirely via the opaque fast-path.
 */
static inline void guac_png_bgra_premult_to_rgba_pixel(
        unsigned char* dst, uint32_t p) {

    unsigned a = (p >> 24) & 0xFF;
    unsigned r = (p >> 16) & 0xFF;
    unsigned g = (p >> 8)  & 0xFF;
    unsigned b =  p        & 0xFF;

    if (a == 255) {
        /* Opaque - premultiplied and straight are identical. */
    }
    else if (a == 0) {
        r = g = b = 0;
    }
    else {
        uint32_t inv = guac_png_alpha_recip[a];
        r = (r * inv + 32768u) >> 16;
        g = (g * inv + 32768u) >> 16;
        b = (b * inv + 32768u) >> 16;
        /* Clamp against rounding overshoot (can go slightly over 255
         * when a premultiplied channel equals alpha exactly). */
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
    }

    dst[0] = (unsigned char) r;
    dst[1] = (unsigned char) g;
    dst[2] = (unsigned char) b;
    dst[3] = (unsigned char) a;

}

#ifdef HAVE_VECTOR_EXTENSIONS
/**
 * 16-byte byte-vector used to batch-shuffle 4 BGRA pixels into RGBA
 * layout in one __builtin_shuffle. Matches the pattern that
 * image-util.c uses for BGRx->RGB row conversion; defined locally
 * because encode-png.c already has its own vector typedef for the
 * palette path and keeping per-file typedefs simplifies target
 * dispatch rules.
 */
typedef unsigned char guac_png_v16u8 __attribute__((vector_size(16)));

/**
 * Control mask for BGRA -> RGBA shuffle. Input is 4 pixels of Cairo
 * ARGB32 premultiplied data, which on little-endian sits in memory
 * as bytes [B, G, R, A, B, G, R, A, ...]. Output is PNG's straight-
 * alpha layout [R, G, B, A, R, G, B, A, ...], i.e. the B and R bytes
 * swapped per pixel. Since the SIMD path only fires for all-opaque
 * groups, the "straight-alpha" requirement collapses to just this
 * swap.
 */
static const guac_png_v16u8 guac_png_bgra_to_rgba_shuffle = {
     2,  1,  0,  3,  /* pixel 0: R G B A (from input B G R A) */
     6,  5,  4,  7,  /* pixel 1 */
    10,  9,  8, 11,  /* pixel 2 */
    14, 13, 12, 15,  /* pixel 3 */
};
#endif

/**
 * SIMD-accelerated per-row BGRA-premult -> RGBA-straight conversion.
 * Processes 4 pixels at a time: if all 4 have alpha == 0xFF, the
 * premultiplied channels equal the straight channels and the
 * conversion reduces to a per-pixel byte swap (B <-> R), which
 * lowers to one PSHUFB on x86 SSSE3+ and one TBL on AArch64 NEON.
 * When any pixel in the group of 4 has partial alpha, that group
 * falls through to the per-pixel scalar un-premultiplication helper.
 *
 * The opaque fast-path captures the common case in guac_display
 * ARGB32 content: cursor and overlay bitmaps with an opaque
 * interior and thin anti-aliased borders. Fully-translucent
 * imagery (e.g. a semi-transparent screenshot overlay) gets no
 * speedup here - the table lookup needed for un-premult is a
 * gather, which won't auto-vectorize and isn't worth an explicit
 * intrinsic implementation for that rare case.
 */
#ifdef HAVE_VECTOR_EXTENSIONS
#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("ssse3")))
#endif
static void guac_png_bgra_premult_to_rgba_row_shuffle(unsigned char* dst,
        const unsigned char* src, int width) {

    const uint32_t* pixels = (const uint32_t*) src;
    int x = 0;

    /* 4 pixels per iteration. */
    for (; x + 4 <= width; x += 4) {

        guac_png_v16u8 v;
        memcpy(&v, src + x * 4, sizeof(v));

        /* Alpha bytes sit at positions 3, 7, 11, 15 within the group.
         * A single all-opaque AND-reduce keeps the check branch-
         * predictable: the common cursor/overlay shape has long runs
         * of all-opaque groups punctuated by runs of mixed-alpha
         * groups on the anti-aliased border, so this branch is
         * consistently taken one way or the other over many
         * iterations. */
        if (v[3] == 0xFF && v[7] == 0xFF && v[11] == 0xFF && v[15] == 0xFF) {
            v = __builtin_shuffle(v, guac_png_bgra_to_rgba_shuffle);
            memcpy(dst + x * 4, &v, sizeof(v));
        }
        else {
            for (int i = 0; i < 4; i++)
                guac_png_bgra_premult_to_rgba_pixel(dst + (x + i) * 4,
                        pixels[x + i]);
        }

    }

    /* Scalar tail for 0-3 leftover pixels. */
    for (; x < width; x++)
        guac_png_bgra_premult_to_rgba_pixel(dst + x * 4, pixels[x]);

}
#endif

/**
 * Converts one row of Cairo ARGB32 (premultiplied BGRA in memory as
 * uint32_t 0xAARRGGBB on little-endian) into PNG's straight-alpha RGBA
 * layout. Dispatches to the SIMD-accelerated row converter on CPUs
 * that support the needed shuffle instruction (SSSE3 on x86 via
 * runtime check; NEON unconditionally on AArch64/ARM builds), and
 * falls back to the per-pixel scalar helper otherwise. PNG has no
 * premultiplied-alpha mode, so this conversion is mandatory when
 * handing Cairo ARGB32 data directly to libpng - skipping it produces
 * output that displays darker than intended on clients that assume
 * straight alpha.
 */
static void guac_png_bgra_premult_to_rgba_row(unsigned char* dst,
        const unsigned char* src, int width) {

#ifdef HAVE_VECTOR_EXTENSIONS
  #if defined(__x86_64__) || defined(__i386__)
    if (__builtin_cpu_supports("ssse3")) {
        guac_png_bgra_premult_to_rgba_row_shuffle(dst, src, width);
        return;
    }
  #elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    guac_png_bgra_premult_to_rgba_row_shuffle(dst, src, width);
    return;
  #endif
#endif

    const uint32_t* pixels = (const uint32_t*) src;
    for (int x = 0; x < width; x++)
        guac_png_bgra_premult_to_rgba_pixel(dst + x * 4, pixels[x]);

}

/**
 * Encodes an ARGB32 Cairo surface as a PNG using libpng directly,
 * un-premultiplying alpha per row before handing pixels to libpng's
 * compression pipeline. Benchmarks under benchmark/png/ show this
 * path to be 1.7-5x faster than routing ARGB32 content through
 * Cairo's built-in PNG writer, depending on content type and hint
 * (see guac_png_apply_hint for the per-hint tradeoffs).
 *
 * Only callable when cairo_image_surface_get_format() ==
 * CAIRO_FORMAT_ARGB32; for any other alpha-bearing format, the caller
 * should fall back to Cairo's writer.
 *
 * @param socket
 *     The socket to send PNG blobs over.
 *
 * @param stream
 *     The stream to associate with each blob.
 *
 * @param surface
 *     The ARGB32 Cairo surface to encode.
 *
 * @param hint
 *     Advisory hint describing the nature of the image content.
 *
 * @return
 *     Zero on success, non-zero on failure.
 */
static int guac_png_argb32_direct_write(guac_socket* socket,
        guac_stream* stream, cairo_surface_t* surface, guac_image_hint hint) {

    png_structp png = NULL;
    png_infop png_info = NULL;
    guac_png_write_state write_state;
    unsigned char* row_buffer = NULL;

    int width = cairo_image_surface_get_width(surface);
    int height = cairo_image_surface_get_height(surface);
    int stride = cairo_image_surface_get_stride(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);

    /* Populate the un-premultiplication reciprocal table if this is
     * the first ARGB32 direct encode to run. */
    guac_png_init_alpha_recip();

    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png == NULL) {
        guac_error = GUAC_STATUS_INTERNAL_ERROR;
        guac_error_message = "libpng failed to create write structure";
        return -1;
    }

    png_info = png_create_info_struct(png);
    if (png_info == NULL) {
        png_destroy_write_struct(&png, NULL);
        guac_error = GUAC_STATUS_INTERNAL_ERROR;
        guac_error_message = "libpng failed to create info structure";
        return -1;
    }

    if (setjmp(png_jmpbuf(png))) {
        guac_mem_free(row_buffer);
        png_destroy_write_struct(&png, &png_info);
        guac_error = GUAC_STATUS_IO_ERROR;
        guac_error_message = "libpng output error";
        return -1;
    }

    write_state.socket = socket;
    write_state.stream = stream;
    write_state.buffer_size = 0;
    png_set_write_fn(png, &write_state,
            guac_png_write_handler, guac_png_flush_handler);

    guac_png_apply_hint(png, hint);

    /* 8-bit RGBA output. Unlike the RGB24 path this preserves the alpha
     * channel, un-premultiplied per-row below. */
    png_set_IHDR(png, png_info, width, height, 8,
            PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
            PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, png_info);

    /* 4 bytes per pixel; reused across all rows. */
    row_buffer = (unsigned char*) guac_mem_alloc(width, 4);

    for (int y = 0; y < height; y++) {
        guac_png_bgra_premult_to_rgba_row(row_buffer, data, width);
        png_write_row(png, row_buffer);
        data += stride;
    }

    png_write_end(png, png_info);

    guac_mem_free(row_buffer);
    png_destroy_write_struct(&png, &png_info);

    guac_png_flush_data(&write_state);
    return 0;

}

#ifdef HAVE_VECTOR_EXTENSIONS
/**
 * 4-wide uint32 vector used to scan forward for the end of a same-color
 * pixel run in guac_png_bgrx_to_palette_row(). One xmm register (or NEON
 * quadword) holds 4 BGRx pixels; wider vectors buy nothing on an all-
 * equal inner scan that terminates on first difference.
 */
typedef uint32_t guac_png_v4u32 __attribute__((vector_size(16)));
#endif

/**
 * Converts a row of Cairo BGRx pixels to libpng palette indices, writing
 * exactly width bytes to dst. Uses a run-based strategy: at each
 * position, look up the palette index once, then scan forward
 * (vectorized 4 pixels at a time when HAVE_VECTOR_EXTENSIONS is
 * available, scalar otherwise) to find the end of the same-color run
 * and fill dst[run_start..run_end) with that index via memset.
 *
 * This is the right shape for the palette-PNG workload: the path only
 * runs when a surface has <=256 unique colors, which in practice means
 * screenshots of text, UI chrome, icons, and synthetic graphics. Those
 * images contain long horizontal same-color runs (typically tens to
 * hundreds of pixels of background between foreground strokes), so
 * palette_find is amortized across entire runs and the actual
 * per-pixel work reduces to a SIMD compare + a memset, both
 * memory-bandwidth operations.
 *
 * A secondary 1-entry direct-mapped cache (prev_color / prev_index)
 * eliminates palette_find for the alternation pattern that text produces
 * - lots of short runs of background color interleaved with foreground
 * strokes that always return to the same background index.
 *
 * @param dst
 *     Destination buffer of at least width bytes to receive one palette
 *     index byte per pixel.
 *
 * @param src
 *     Source buffer of at least width*4 bytes in Cairo BGRx order.
 *
 * @param width
 *     The number of pixels in the row.
 *
 * @param palette
 *     The populated palette against which colors should be looked up.
 */
static void guac_png_bgrx_to_palette_row(unsigned char* restrict dst,
        const unsigned char* restrict src, int width,
        guac_palette* palette) {

    const uint32_t* pixels = (const uint32_t*) src;

    /* Adjacent-run cache. Sentinel 0xFFFFFFFF can't appear here because
     * we mask source pixels to 24 bits, guaranteeing a miss on the
     * first pixel of each row. */
    uint32_t prev_color = 0xFFFFFFFFu;
    unsigned char prev_index = 0;

    int x = 0;
    while (x < width) {

        uint32_t color = pixels[x] & 0x00FFFFFFu;

        unsigned char index;
        if (color == prev_color)
            index = prev_index;
        else {
            index = (unsigned char) guac_palette_find(palette, (int) color);
            prev_color = color;
            prev_index = index;
        }

        int run_start = x;
        x++;

#ifdef HAVE_VECTOR_EXTENSIONS
        /* Scan 4 pixels per iteration looking for the end of this run.
         * Each iteration masks to 24 bits and compares all four lanes
         * against a broadcast of the run's color; if all match, advance
         * the x cursor by 4 and continue, otherwise fall through to the
         * scalar tail which narrows down to the exact first-different
         * pixel. Equality yields -1 per matching lane, so "all match"
         * is a product / AND-reduction of the lanes. */
        guac_png_v4u32 target = { color, color, color, color };
        guac_png_v4u32 mask   = { 0x00FFFFFFu, 0x00FFFFFFu,
                                  0x00FFFFFFu, 0x00FFFFFFu };
        for (; x + 4 <= width; x += 4) {
            guac_png_v4u32 v;
            memcpy(&v, pixels + x, sizeof(v));
            v &= mask;
            guac_png_v4u32 eq = (v == target);
            /* All lanes match iff every lane is -1. AND-reduce. */
            if ((eq[0] & eq[1] & eq[2] & eq[3]) != 0xFFFFFFFFu)
                break;
        }
#endif

        /* Scalar tail narrows to the exact first differing pixel. Runs
         * when HAVE_VECTOR_EXTENSIONS is unset (every pixel), when width
         * - x < 4 (up to 3 leftover pixels), or when the SIMD iteration
         * hit a partial match (up to 3 pixels still within the run
         * inside the broken vector). */
        while (x < width && (pixels[x] & 0x00FFFFFFu) == color)
            x++;

        memset(dst + run_start, index, x - run_start);

    }

}

static int guac_png_cairo_write(guac_socket* socket, guac_stream* stream,
        cairo_surface_t* surface, guac_image_hint hint) {

    cairo_format_t format = cairo_image_surface_get_format(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);

    /* RGB24 surfaces take the direct libpng path with our SIMD-shuffled
     * BGRx->RGB row conversion, bypassing Cairo's scalar internal
     * conversion. */
    if (format == CAIRO_FORMAT_RGB24 && data != NULL)
        return guac_png_rgb24_direct_write(socket, stream, surface, hint);

    /* ARGB32 surfaces take the direct libpng ARGB32 path, which does
     * its own alpha un-premultiplication per row. This replaces the
     * earlier always-Cairo fallback for alpha-bearing content and
     * picks up the hint-driven compression strategy selection. */
    if (format == CAIRO_FORMAT_ARGB32 && data != NULL)
        return guac_png_argb32_direct_write(socket, stream, surface, hint);

    /* Any other Cairo format (A8, A1, RGB16, etc.) is uncommon in
     * guac_display's output and falls through to Cairo's own PNG
     * writer, which handles the full Cairo format matrix correctly. */
    guac_png_write_state write_state;

    /* Init write state */
    write_state.socket = socket;
    write_state.stream = stream;
    write_state.buffer_size = 0;

    /* Write surface as PNG */
    if (cairo_surface_write_to_png_stream(surface,
                guac_png_cairo_write_handler,
                &write_state) != CAIRO_STATUS_SUCCESS) {
        guac_error = GUAC_STATUS_INTERNAL_ERROR;
        guac_error_message = "Cairo PNG backend failed";
        return -1;
    }

    /* Flush remaining PNG data */
    guac_png_flush_data(&write_state);
    return 0;

}

/**
 * Writes the given buffer of PNG data to the buffer of the given write state,
 * flushing that buffer to blob instructions if necessary. This handler is
 * called by libpng when writing PNG data via png_write_png().
 *
 * @param png
 *     The PNG compression state structure associated with the write operation.
 *     The pointer to arbitrary data will have been set to the
 *     guac_png_write_state by png_set_write_fn(), and will be accessible via
 *     png->io_ptr or png_get_io_ptr(png), depending on the version of libpng.
 *
 * @param data
 *     The buffer of PNG data to write.
 * 
 * @param length
 *     The size of the given buffer, in bytes.
 */
static void guac_png_write_handler(png_structp png, png_bytep data,
        png_size_t length) {

    /* Get png buffer structure */
    guac_png_write_state* write_state;
#ifdef HAVE_PNG_GET_IO_PTR
    write_state = (guac_png_write_state*) png_get_io_ptr(png);
#else
    write_state = (guac_png_write_state*) png->io_ptr;
#endif

    /* Append data to buffer, writing as necessary */
    guac_png_write_data(write_state, data, length);

}

/**
 * Flushes any PNG data within the buffer of the given write state as a blob
 * instruction. If no data is within the buffer, this function has no effect.
 * This handler is called by libpng when it has finished writing PNG data via
 * png_write_png().
 *
 * @param png
 *     The PNG compression state structure associated with the write operation.
 *     The pointer to arbitrary data will have been set to the
 *     guac_png_write_state by png_set_write_fn(), and will be accessible via
 *     png->io_ptr or png_get_io_ptr(png), depending on the version of libpng.
 */
static void guac_png_flush_handler(png_structp png) {

    /* Get png buffer structure */
    guac_png_write_state* write_state;
#ifdef HAVE_PNG_GET_IO_PTR
    write_state = (guac_png_write_state*) png_get_io_ptr(png);
#else
    write_state = (guac_png_write_state*) png->io_ptr;
#endif

    /* Flush buffer */
    guac_png_flush_data(write_state);

}

int guac_png_write(guac_socket* socket, guac_stream* stream,
        cairo_surface_t* surface, guac_image_hint hint) {

    png_structp png;
    png_infop png_info;
    unsigned char* row_buffer = NULL;
    int bpp;

    guac_png_write_state write_state;

    /* Get image surface properties and data */
    cairo_format_t format = cairo_image_surface_get_format(surface);
    int width = cairo_image_surface_get_width(surface);
    int height = cairo_image_surface_get_height(surface);
    int stride = cairo_image_surface_get_stride(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);

    /* If not RGB24, hand off to the cairo-path dispatcher which routes
     * ARGB32 to its own libpng-direct encoder and any remaining formats
     * to Cairo's built-in writer. */
    if (format != CAIRO_FORMAT_RGB24 || data == NULL)
        return guac_png_cairo_write(socket, stream, surface, hint);

    /* Flush pending operations to surface */
    cairo_surface_flush(surface);

    /* Attempt to build palette */
    guac_palette* palette = guac_palette_alloc(surface);

    /* Palette doesn't fit - fall back to the RGB24 direct encoder via
     * the cairo-path dispatcher (which, for RGB24 surfaces, routes
     * straight to guac_png_rgb24_direct_write without touching Cairo). */
    if (palette == NULL)
        return guac_png_cairo_write(socket, stream, surface, hint);

    /* Calculate BPP from palette size */
    if      (palette->size <= 2)  bpp = 1;
    else if (palette->size <= 4)  bpp = 2;
    else if (palette->size <= 16) bpp = 4;
    else                          bpp = 8;

    /* Set up PNG writer */
    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        guac_palette_free(palette);
        guac_error = GUAC_STATUS_INTERNAL_ERROR;
        guac_error_message = "libpng failed to create write structure";
        return -1;
    }

    png_info = png_create_info_struct(png);
    if (!png_info) {
        png_destroy_write_struct(&png, NULL);
        guac_palette_free(palette);
        guac_error = GUAC_STATUS_INTERNAL_ERROR;
        guac_error_message = "libpng failed to create info structure";
        return -1;
    }

    /* Set error handler */
    if (setjmp(png_jmpbuf(png))) {
        guac_mem_free(row_buffer);
        png_destroy_write_struct(&png, &png_info);
        guac_palette_free(palette);
        guac_error = GUAC_STATUS_IO_ERROR;
        guac_error_message = "libpng output error";
        return -1;
    }

    /* Init write state */
    write_state.socket = socket;
    write_state.stream = stream;
    write_state.buffer_size = 0;

    /* Set up writer */
    png_set_write_fn(png, &write_state,
            guac_png_write_handler,
            guac_png_flush_handler);

    /* Override libpng's default compression knobs. On palette content
     * (long horizontal runs of identical indices) the adaptive filter
     * picker that libpng enables by default is pure overhead: it
     * tries SUB/UP/AVG/PAETH on every row only to find that NONE
     * produces the best-compressing input. Forcing NONE here skips
     * that per-row filter selection entirely. Level 3 gives up only
     * a few percent of compression vs level 6 while encoding ~45%
     * faster on typical palette content. */
    png_set_compression_level(png, GUAC_PNG_PALETTE_COMPRESSION_LEVEL);
    png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_NONE);

    /* Write image info */
    png_set_IHDR(
        png,
        png_info,
        width,
        height,
        bpp,
        PNG_COLOR_TYPE_PALETTE,
        PNG_INTERLACE_NONE,
        PNG_COMPRESSION_TYPE_DEFAULT,
        PNG_FILTER_TYPE_DEFAULT
    );

    /* Write palette and header */
    png_set_PLTE(png, png_info, palette->colors, palette->size);
    png_write_info(png, png_info);

    /* png_set_packing is the streaming equivalent of the libpng-level
     * PNG_TRANSFORM_PACKING that the old all-rows-at-once png_write_png
     * path used: we feed libpng one-byte-per-pixel rows (regardless of
     * output bit depth) and libpng packs to 1/2/4-bit as it writes.
     * Combined with streaming png_write_row below, this avoids the
     * height*malloc + height*free churn the old path did to hold every
     * row in memory before a single png_write_png call. */
    if (bpp < 8)
        png_set_packing(png);

    /* Reuse a single row buffer across all rows. Palette-index bytes are
     * generated on-the-fly by guac_png_bgrx_to_palette_row() per row and
     * handed straight to png_write_row(), which copies into libpng's
     * internal DEFLATE pipeline before returning. */
    row_buffer = (unsigned char*) guac_mem_alloc(width);

    for (int y = 0; y < height; y++) {
        guac_png_bgrx_to_palette_row(row_buffer, data, width, palette);
        png_write_row(png, row_buffer);
        data += stride;
    }

    png_write_end(png, png_info);

    guac_mem_free(row_buffer);
    png_destroy_write_struct(&png, &png_info);
    guac_palette_free(palette);

    /* Ensure all data is written */
    guac_png_flush_data(&write_state);
    return 0;

}

