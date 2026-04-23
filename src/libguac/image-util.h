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

#ifndef GUAC_IMAGE_UTIL_H
#define GUAC_IMAGE_UTIL_H

/**
 * @file image-util.h
 *
 * Pixel-format conversion helpers shared between libguac's image
 * encoders. Each helper isolates a single per-pixel transform that
 * would otherwise be a scalar bottleneck between Cairo (which stores
 * pixels in BGRx) and whatever layout the underlying image library
 * expects.
 *
 * The implementations live in image-util.c and runtime-dispatch between
 * a PSHUFB/TBL-based fast path (on SSSE3-capable x86 or any NEON-
 * capable ARM) and a scalar fallback path. The dispatch happens once
 * per call at the row granularity, so the overhead is negligible
 * relative to per-pixel work even on slow targets.
 */

#include <stdint.h>

/**
 * Converts one row of pixel data from Cairo's RGB24 format (BGRx; blue,
 * green, red, undefined, little-endian uint32) to 8-bit packed RGB for
 * consumption by libpng's png_write_row() or standard libjpeg's
 * jpeg_write_scanlines() when compiled without libjpeg-turbo's
 * JCS_EXT_BGRX extension. Internally dispatches at runtime between a
 * shuffle-based SIMD path (4 pixels/iteration via PSHUFB on x86 SSSE3+
 * or TBL on AArch64 NEON) and a scalar fallback.
 *
 * @param dst
 *     Destination buffer of at least width*3 bytes to receive packed RGB.
 *
 * @param src
 *     Source buffer of at least width*4 bytes in Cairo BGRx order.
 *
 * @param width
 *     The number of pixels in the row.
 */
void guac_image_bgrx_to_rgb_row(unsigned char* restrict dst,
        const unsigned char* restrict src, int width);

/**
 * Copies one row of 32-bit BGRx pixels and forces each pixel's high
 * byte (Cairo's undefined "x" channel) to 0xFF, producing well-formed
 * ARGB/BGRA data with alpha = 1.0. Used to bridge Cairo's RGB24
 * surfaces to downstream encoders (libwebp, etc.) that require a
 * valid alpha channel.
 *
 * Written as a plain counter loop so the auto-vectorizer can scale
 * the store width to whatever the build target supports - baseline
 * SSE2 emits 128-bit paired ops, AVX2 emits 256-bit, AVX-512 emits
 * 512-bit, and NEON/SVE/RVV get their widest native width. The
 * function carries a per-function tree-loop-vectorize optimize
 * attribute so this still happens at -O2 on GCC <12 (which leaves
 * loop vectorization off by default at that level).
 *
 * @param dst
 *     Destination buffer of at least width*4 bytes. May alias src.
 *
 * @param src
 *     Source buffer of at least width*4 bytes in 32-bit pixel layout.
 *
 * @param width
 *     The number of pixels in the row.
 */
void guac_image_force_alpha_row(uint32_t* restrict dst,
        const uint32_t* restrict src, int width);

#endif

