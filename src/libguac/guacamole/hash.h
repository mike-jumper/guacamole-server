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


#ifndef _GUAC_HASH_H
#define _GUAC_HASH_H

/**
 * Provides functions and structures for producing likely-to-be-unique hash
 * values for images.
 *
 * @file hash.h
 */

#include <cairo/cairo.h>

#include <stdint.h>

typedef struct guac_hash_search_state {

    int x;

    int y;

    uint64_t value;

} guac_hash_search_state;

typedef int guac_hash_callback(int x, int y, uint64_t hash, void* closure);

/**
 * Produces a 24-bit hash value from all pixels of the given surface. The
 * surface provided must be RGB or ARGB with each pixel stored in 32 bits.
 * The hashing algorithm used is a variant of the cyclic polynomial rolling
 * hash.
 *
 * @param surface The Cairo surface to hash.
 * @return An arbitrary 24-bit unsigned integer value intended to be well
 *         distributed across different images.
 */
unsigned int guac_hash_surface(cairo_surface_t* surface);

int guac_hash_foreach_surface_rect(cairo_surface_t* surface, int rect_width,
        int rect_height, guac_hash_callback* callback, void* closure);

int guac_hash_foreach_image_rect(unsigned char* data, int width, int height,
        int stride, int rect_width, int rect_height,
        guac_hash_callback* callback, void* closure);

int guac_hash_search_image(unsigned char* haystack_data, int haystack_width,
        int haystack_height, int haystack_stride, unsigned char* needle_data,
        int needle_width, int needle_height, int needle_stride,
        int* found_x, int* found_y);

int guac_hash_search_surface(cairo_surface_t* haystack, cairo_surface_t* needle,
        int* found_x, int* found_y);

int guac_image_cmp(unsigned char* data_a, int width_a, int height_a,
        int stride_a, unsigned char* data_b, int width_b, int height_b,
        int stride_b);

/**
 * Given two Cairo surfaces, returns zero if the data contained within each
 * is identical, and a positive or negative value if the value of the first
 * is found to be lexically greater or less than the second respectively.
 *
 * @param a The first Cairo surface to compare.
 * @param b The Cairo surface to compare the first surface against.
 * @return Zero if the data contained within each is identical, and a positive
 *         or negative value if the value of the first is found to be lexically
 *         greater or less than the second respectively.
 */
int guac_surface_cmp(cairo_surface_t* a, cairo_surface_t* b);

#endif

