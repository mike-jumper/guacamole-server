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


#ifndef GUAC_SCROLL_H
#define GUAC_SCROLL_H

/**
 * Provides a fast scroll detection implementation, allowing the difference
 * between two images to be determined.
 *
 * @file scroll.h
 */

#include <cairo/cairo.h>

/**
 * Given two surfaces representing the current and future state of an image,
 * determines whether the latter represents a scrolled version of the former,
 * calculating the rectangle which is identical between the two surfaces. If
 * there is no such common rectangle, zero is returned.
 *
 * Currently, both surfaces must be identical in size, and both surfaces must
 * be at least 64x64, or no search will be performed. As the search algorithm
 * relies on hashing to achieve useful speed and avoid repeated comparisons, it
 * is possible for this search to return false negatives. It is not possible
 * for this search to return false positives.
 *
 * @param a
 *     The surface representing the current state of the image.
 *
 * @param ax
 *     A pointer to the int which should receive the X coordinate of the
 *     upper-left corner of the rectangle within the first surface which is
 *     also present within the second surface.
 *
 * @param ay
 *     A pointer to the int which should receive the Y coordinate of the
 *     upper-left corner of the rectangle within the first surface which is
 *     also present within the second surface.
 *
 * @param width
 *     A pointer to the int which should receive the width of the rectangle
 *     which is present in both surfaces.
 *
 * @param height 
 *     A pointer to the int which should receive the height of the rectangle
 *     which is present in both surfaces.
 *
 * @param b
 *     The surface representing the future state of the image.
 *
 * @param bx
 *     A pointer to the int which should receive the X coordinate of the
 *     upper-left corner of the rectangle within the second surface which is
 *     also present within the first surface.
 *
 * @param by
 *     A pointer to the int which should receive the Y coordinate of the
 *     upper-left corner of the rectangle within the second surface which is
 *     also present within the first surface.
 *
 * @return
 *     Non-zero if the second surface is a scrolled version of the first
 *     surface and the details of the common rectangle of image data have been
 *     stored in the pointers provided, zero otherwise.
 */
int guac_scroll_find_common_rect(cairo_surface_t* a, int* ax, int* ay,
        int* width, int* height, cairo_surface_t* b, int* bx, int* by);

#endif

