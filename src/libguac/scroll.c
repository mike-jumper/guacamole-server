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
#include <stdlib.h>

static int guac_scroll_estimate_delta(unsigned char* data_a, int width_a,
        int height_a, int stride_a, unsigned char* data_b, int width_b,
        int height_b, int stride_b, int* delta_x, int* delta_y) {

    int found_x;
    int found_y;

    /* Not a scrolled version if not same size */
    if (width_a != width_b || height_a != height_b)
        return 0;

    /* Cannot search if B is too small */
    if (width_a < 64 || height_a < 64)
        return 0;

    /* Restrict search area to centered 512x512 square */
    if (width_a > 512) {
        int left_a = (width_a - 512) / 2;
        data_a += left_a * 4;
        data_b += left_a * 4;
        width_a = 512;
    }

    if (height_a > 512) {
        int top_a = (height_a - 512) / 2;
        data_a += top_a * stride_a;
        data_b += top_a * stride_b;
        height_a = 512;
    }

    /* Search for 64x64 rectangle centered within B */
    int previous_x = (width_a - 64) / 2;
    int previous_y = (height_a - 64) / 2;

    /* Offset B data by upper-left corner of rect */
    data_b += (previous_x * 4) + (previous_y * stride_b);

    /* Search for centered 64x64 rectangle from B within A */
    if (guac_hash_search_image(data_a, width_a, height_a, stride_a,
                data_b, 64, 64, stride_b, &found_x, &found_y)) {
        *delta_x = previous_x - found_x;
        *delta_y = previous_y - found_y;
        return 1;
    }

    /* Not scrolled */
    return 0;

}

int guac_scroll_find_common_rect(cairo_surface_t* a, int* ax, int* ay,
        int* width, int* height, cairo_surface_t* b, int* bx, int* by) {

    int delta_x;
    int delta_y;

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

    if (guac_scroll_estimate_delta(data_a, width_a, height_a, stride_a,
                data_b, width_b, height_b, stride_b, &delta_x, &delta_y)) {

        /* Translate relative X to absolute X coordinates and width */
        if (delta_x < 0) {
            *width = width_b + delta_x;
            *ax = -delta_x;
            *bx = 0;
        }
        else {
            *width = width_b - delta_x;
            *ax = 0;
            *bx = delta_x;
        }

        /* Translate relative Y to absolute Y coordinates and height */
        if (delta_y < 0) {
            *height = height_b + delta_y;
            *ay = -delta_y;
            *by = 0;
        }
        else {
            *height = height_b - delta_y;
            *ay = 0;
            *by = delta_y;
        }

        data_a += (*ax * 4) + (*ay * stride_a);
        data_b += (*bx * 4) + (*by * stride_b);

        /* Scrolled if rectangle does indeed match */
        return !guac_image_cmp(data_a, *width, *height, stride_a,
                data_b, *width, *height, stride_b);

    }

    /* Not scrolled */
    return 0;

}

