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
#include "guacamole/hash.h"

#include <cairo/cairo.h>

#include <stdint.h>
#include <string.h>

typedef struct guac_hash_search_state {

    unsigned char* haystack_data;

    int haystack_stride;

    unsigned char* needle_data;

    int needle_stride;

    int needle_width;

    int needle_height;

    int x;

    int y;

    uint64_t value;

} guac_hash_search_state;

/*
 * Arbitrary hash function whhich maps ALL 32-bit numbers onto 24-bit numbers
 * evenly, while guaranteeing that all 24-bit numbers are mapped onto
 * themselves.
 */
unsigned int _guac_hash_32to24(unsigned int value) {

    /* Grab highest-order byte */
    unsigned int upper = value & 0xFF000000;

    /* XOR upper with lower three bytes, truncate to 24-bit */
    return
          (value & 0xFFFFFF)
        ^ (upper >> 8)
        ^ (upper >> 16)
        ^ (upper >> 24);

}

/**
 * Rotates a given 32-bit integer by N bits.
 *
 * NOTE: We probably should check for available bitops.h macros first.
 */
unsigned int _guac_rotate(unsigned int value, int amount) {

    /* amount = amount % 32 */
    amount &= 0x1F; 

    /* Return rotated amount */
    return (value >> amount) | (value << (32 - amount));

}

unsigned int guac_hash_surface(cairo_surface_t* surface) {

    /* Init to zero */
    unsigned int hash_value = 0;

    int x, y;

    /* Get image data and metrics */
    unsigned char* data = cairo_image_surface_get_data(surface);
    int width = cairo_image_surface_get_width(surface);
    int height = cairo_image_surface_get_height(surface);
    int stride = cairo_image_surface_get_stride(surface);

    for (y=0; y<height; y++) {

        /* Get current row */
        uint32_t* row = (uint32_t*) data;
        data += stride;

        for (x=0; x<width; x++) {

            /* Get color at current pixel */
            unsigned int color = *row;
            row++;

            /* Compute next hash */
            hash_value =
                _guac_rotate(hash_value, 1) ^ color ^ 0x1B872E69;

        }

    } /* end for each row */

    /* Done */
    return _guac_hash_32to24(hash_value);

}

static int guac_hash_assign_value(int x, int y, uint64_t hash, void* closure) {
    *((uint64_t*) closure) = hash;
    return 1;
}

static int guac_hash_find_value(int x, int y, uint64_t hash, void* closure) {

    guac_hash_search_state* state = (guac_hash_search_state*) closure;

    /* Verify possibly-matching rectangle if hash matches */
    if (state->value == hash) {

        unsigned char* current = state->haystack_data + (x * 4)
            + (y * state->haystack_stride);

        int width = state->needle_width;
        int height = state->needle_height;

        /* Store coordinates and stop if rectangle contents are correct */
        if (!guac_image_cmp(current, width, height, state->haystack_stride,
                    state->needle_data, width, height, state->needle_stride)) {
            state->x = x;
            state->y = y;
            return 1;
        }

    }

    /* Hash does not currently match */
    return 0;

}

int guac_hash_foreach_image_rect(unsigned char* data, int width, int height,
        int stride, int rect_width, int rect_height,
        guac_hash_callback* callback, void* closure) {

    /* Only 64x64 is currently supported */
    if (rect_width != 64 || rect_height != 64)
        return 0;

    int x, y;
    uint64_t cell_hash[4096] = { 0 };

    for (y = 0; y < height; y++) {

        uint64_t* current_cell_hash = cell_hash;

        /* Get current row */
        uint32_t* row = (uint32_t*) data;
        data += stride;

        /* Calculate row segment hashes for entire row */
        uint64_t row_hash = 0;
        for (x = 0; x < width; x++) {

            /* Get current pixel */
            uint32_t pixel = *(row++);

            /* Update hash value for current row segment */
            row_hash = ((row_hash * 31) << 1) + pixel;

            /* Incorporate row hash value into overall cell hash */
            uint64_t cell_hash = ((*current_cell_hash * 31) << 1) + row_hash;
            *(current_cell_hash++) = cell_hash;

            /* Invoke callback for every hash generated, breaking out early if
             * requested */
            if (y >= rect_height - 1 && x >= rect_width - 1) {
                int result = callback(x - rect_width + 1, y - rect_height + 1,
                        cell_hash, closure);
                if (result)
                    return result;
            }

        }

    } /* end for each row */

    return 0;

}

int guac_hash_foreach_surface_rect(cairo_surface_t* surface, int rect_width,
        int rect_height, guac_hash_callback* callback, void* closure) {

    /* Surface metrics */
    unsigned char* data = cairo_image_surface_get_data(surface);
    int width = cairo_image_surface_get_width(surface);
    int height = cairo_image_surface_get_height(surface);
    int stride = cairo_image_surface_get_stride(surface);

    return guac_hash_foreach_image_rect(data, width, height, stride,
            rect_width, rect_height, callback, closure);

}

int guac_hash_search_image(unsigned char* haystack_data, int haystack_width,
        int haystack_height, int haystack_stride, unsigned char* needle_data,
        int needle_width, int needle_height, int needle_stride,
        int* found_x, int* found_y) {

    /* If there isn't room for the needle, it can't possibly be present */
    if (haystack_width < needle_width || haystack_height < needle_height)
        return 0;

    guac_hash_search_state state = {

        .haystack_data   = haystack_data,
        .haystack_stride = haystack_stride,

        .needle_data   = needle_data,
        .needle_stride = needle_stride,
        .needle_width  = needle_width,
        .needle_height = needle_height

    };

    /* Calculate hash value of needle */
    guac_hash_foreach_image_rect(needle_data, needle_width, needle_height,
            needle_stride, needle_width, needle_height,
            guac_hash_assign_value, &state.value);

    /* Search for needle in haystack */
    if (guac_hash_foreach_image_rect(haystack_data, haystack_width,
                haystack_height, haystack_stride, needle_width, needle_height,
                guac_hash_find_value, &state)) {
        *found_x = state.x;
        *found_y = state.y;
        return 1;
    }

    /* Failed to find needle */
    return 0;

}

int guac_hash_search_surface(cairo_surface_t* haystack, cairo_surface_t* needle,
        int* found_x, int* found_y) {

    /* Haystack metrics */
    unsigned char* haystack_data = cairo_image_surface_get_data(haystack);
    int haystack_width = cairo_image_surface_get_width(haystack);
    int haystack_height = cairo_image_surface_get_height(haystack);
    int haystack_stride = cairo_image_surface_get_stride(haystack);

    /* Needle metrics */
    unsigned char* needle_data = cairo_image_surface_get_data(needle);
    int needle_width = cairo_image_surface_get_width(needle);
    int needle_height = cairo_image_surface_get_height(needle);
    int needle_stride = cairo_image_surface_get_stride(needle);

    return guac_hash_search_image(haystack_data, haystack_width,
            haystack_height, haystack_stride, needle_data, needle_width,
            needle_height, needle_stride, found_x, found_y);

}

int guac_image_cmp(unsigned char* data_a, int width_a, int height_a,
        int stride_a, unsigned char* data_b, int width_b, int height_b,
        int stride_b) {

    int y;

    /* If core dimensions differ, just compare those. Done. */
    if (width_a != width_b) return width_a - width_b;
    if (height_a != height_b) return height_a - height_b;

    for (y = 0; y < height_a; y++) {

        /* Compare row. If different, use that result. */
        int cmp_result = memcmp(data_a, data_b, width_a * 4);
        if (cmp_result != 0)
            return cmp_result;

        /* Next row */
        data_a += stride_a;
        data_b += stride_b;

    }

    /* Otherwise, same. */
    return 0;

}

int guac_surface_cmp(cairo_surface_t* a, cairo_surface_t* b) {

    /* Surface A metrics */
    unsigned char* data_a = cairo_image_surface_get_data(a);
    int width_a = cairo_image_surface_get_width(a);
    int height_a = cairo_image_surface_get_height(a);
    int stride_a = cairo_image_surface_get_stride(a);

    /* Surface B metrics */
    unsigned char* data_b = cairo_image_surface_get_data(b);
    int width_b = cairo_image_surface_get_width(b);
    int height_b = cairo_image_surface_get_height(b);
    int stride_b = cairo_image_surface_get_stride(b);

    return guac_image_cmp(data_a, width_a, height_a, stride_a,
            data_b, width_b, height_b, stride_b);

}
