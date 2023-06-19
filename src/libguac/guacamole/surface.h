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

#ifndef GUAC_SURFACE_H
#define GUAC_SURFACE_H

/**
 * Provides initial data structurs and functions for manupulating
 * statically-sized tiles. Each tile is intended to be used alongside other
 * tiles to build a surface.
 *
 * @file surface.h
 */

#include "client-types.h"
#include "socket-types.h"
#include "surface-constants.h"
#include "surface-types.h"

/**
 * Allocates a new guac_surface, assigning it to the given layer.
 *
 * @param client
 *     The client associated with the given layer.
 *
 * @param socket
 *     The socket to send instructions on when flushing.
 *
 * @param layer
 *     The layer to associate with the new surface.
 *
 * @param width
 *     The width of the surface.
 *
 * @param height
 *     The height of the surface.
 *
 * @return
 *     A newly-allocated guac_surface.
 */
guac_surface* guac_surface_alloc(guac_client* client, guac_socket* socket,
        const guac_layer* layer, int width, int height);

/**
 * Frees the given guac_surface. Beware that this will NOT free any
 * associated layers, which must be freed manually.
 *
 * @param surface The surface to free.
 */
void guac_surface_free(guac_surface* surface);

 /**
 * Resizes the given surface to the given size.
 *
 * @param surface The surface to resize.
 * @param width The width of the surface.
 * @param height The height of the surface.
 */
int guac_surface_resize(guac_surface* surface, int width, int height);

/**
 * Draws the given data to the given guac_surface. If the source surface
 * is ARGB, the draw operation will be performed using the Porter-Duff "over"
 * composite operator. If the source surface is RGB (no alpha channel), no
 * compositing is performed and destination pixels are ignored.
 *
 * @param surface
 *     The surface to draw to.
 *
 * @param x
 *     The X coordinate of the draw location.
 *
 * @param y
 *     The Y coordinate of the draw location.
 *
 * @param src
 *     The Cairo surface to retrieve data from.
 */
void guac_surface_draw(guac_surface* surface, int x, int y,
        cairo_surface_t* src);

/**
 * Paints to the given guac_surface using the given data as a stencil,
 * filling opaque regions with the specified color, and leaving transparent
 * regions untouched.
 *
 * @param surface The surface to draw to.
 * @param x The X coordinate of the draw location.
 * @param y The Y coordinate of the draw location.
 * @param src The Cairo surface to retrieve data from.
 * @param red The red component of the fill color.
 * @param green The green component of the fill color.
 * @param blue The blue component of the fill color.
 */
void guac_surface_paint(guac_surface* surface, int x, int y,
        cairo_surface_t* src, int red, int green, int blue);

/**
 * Copies a rectangle of data between two surfaces.
 *
 * @param src The source surface.
 * @param sx The X coordinate of the upper-left corner of the source rect.
 * @param sy The Y coordinate of the upper-left corner of the source rect.
 * @param width The width of the source rect.
 * @param height The height of the source rect.
 * @param dst The destination surface.
 * @param dx The X coordinate of the upper-left corner of the destination rect.
 * @param dy The Y coordinate of the upper-left corner of the destination rect.
 */
void guac_surface_copy(guac_surface* src, int sx, int sy, int width, int height,
        guac_surface* dst, int dx, int dy);

/**
 * Transfers a rectangle of data between two surfaces.
 *
 * @param src The source surface.
 * @param sx The X coordinate of the upper-left corner of the source rect.
 * @param sy The Y coordinate of the upper-left corner of the source rect.
 * @param width The width of the source rect.
 * @param height The height of the source rect.
 * @param op The transfer function.
 * @param dst The destination surface.
 * @param dx The X coordinate of the upper-left corner of the destination rect.
 * @param dy The Y coordinate of the upper-left corner of the destination rect.
 */
void guac_surface_transfer(guac_surface* src, int sx, int sy, int width, int height,
        guac_transfer_function op, guac_surface* dst, int dx, int dy);

/**
 * Assigns the given value to all pixels within a rectangle of the given
 * surface. The color of all pixels within the rectangle, including the alpha
 * component, is entirely replaced.
 *
 * @param surface
 *     The surface to draw upon.
 *
 * @param x
 *     The X coordinate of the upper-left corner of the rectangle.
 *
 * @param y
 *     The Y coordinate of the upper-left corner of the rectangle.
 *
 * @param w
 *     The width of the rectangle.
 *
 * @param h
 *     The height of the rectangle.
 *
 * @param red
 *     The red component of the color value to assign to all pixels within the
 *     rectangle.
 *
 * @param green
 *     The green component of the color value to assign to all pixels within
 *     the rectangle.
 *
 * @param blue 
 *     The blue component of the color value to assign to all pixels within the
 *     rectangle.
 *
 * @param alpha 
 *     The alpha component of the color value to assign to all pixels within
 *     the rectangle.
 */
void guac_surface_set(guac_surface* surface, int x, int y,
        int w, int h, int red, int green, int blue, int alpha);

/**
 * Given the coordinates and dimensions of a rectangle, clips all future
 * operations within that rectangle.
 *
 * @param surface
 *     The surface whose clipping rectangle should be changed.
 *
 * @param x
 *     The X coordinate of the upper-left corner of the bounding rectangle.
 *
 * @param y
 *     The Y coordinate of the upper-left corner of the bounding rectangle.
 *
 * @param width
 *     The width of the bounding rectangle.
 *
 * @param hidth
 *     The height of the bounding rectangle.
 */
void guac_surface_clip(guac_surface* surface, int x, int y,
        int width, int height);

/**
 * Resets the clipping rectangle, allowing drawing operations throughout the
 * entire surface.
 *
 * @param surface
 *     The surface whose clipping rectangle should be reset.
 */
void guac_surface_reset_clip(guac_surface* surface);

/**
 * Changes the location of the surface relative to its parent layer. If the
 * surface does not represent a non-default visible layer, then this function
 * has no effect.
 *
 * @param surface
 *     The surface to move relative to its parent layer.
 *
 * @param x
 *     The new X coordinate for the upper-left corner of the layer, in pixels.
 *
 * @param y
 *     The new Y coordinate for the upper-left corner of the layer, in pixels.
 */
void guac_surface_move(guac_surface* surface, int x, int y);

/**
 * Changes the stacking order of the surface relative to other surfaces within
 * the same parent layer. If the surface does not represent a non-default
 * visible layer, then this function has no effect.
 *
 * @param surface
 *     The surface to reorder relative to sibling layers.
 *
 * @param z
 *     The new Z-order for this layer, relative to sibling layers.
 */
void guac_surface_stack(guac_surface* surface, int z);

/**
 * Changes the parent layer of ths given surface. By default, layers will be
 * children of the default layer. If the surface does not represent a
 * non-default visible layer, then this function has no effect.
 *
 * @param surface
 *     The surface whose parent layer should be changed.
 *
 * @param parent
 *     The layer which should be set as the new parent of the given surface.
 */
void guac_surface_set_parent(guac_surface* surface, const guac_layer* parent);

/**
 * Sets the opacity of the surface. If the surface does not represent a
 * non-default visible layer, then this function has no effect.
 *
 * @param surface
 *     The surface whose opacity should be changed.
 *
 * @param opacity
 *     The level of opacity applied to this surface, where fully opaque is 255,
 *     and fully transparent is 0.
 */
void guac_surface_set_opacity(guac_surface* surface, int opacity);

/**
 * Flushes the given surface, including any applicable properties, drawing any
 * pending operations on the remote display.
 *
 * @param surface
 *     The surface to flush.
 */
void guac_surface_flush(guac_surface* surface);

/**
 * Duplicates the contents of the current surface to the given socket. Pending
 * changes are not flushed.
 *
 * @param surface
 *     The surface to duplicate.
 *
 * @param user
 *     The user receiving the surface.
 *
 * @param socket
 *     The socket over which the surface contents should be sent.
 */
void guac_surface_dup(guac_surface* surface, guac_user* user,
        guac_socket* socket);

/**
 * Declares that the given surface should receive touch events. By default,
 * surfaces are assumed to not expect touch events. This value is advisory, and
 * the client is not required to honor the declared level of touch support.
 * Implementations are expected to safely handle or ignore any received touch
 * events, regardless of the level of touch support declared.  regardless of
 * the level of touch support declared.
 *
 * @param surface
 *     The surface to modify.
 *
 * @param touches
 *     The number of simultaneous touches that this surface can accept, where 0
 *     indicates that the surface does not support touch events at all.
 */
void guac_surface_set_multitouch(guac_surface* surface, int touches);

/**
 * Sets the lossless compression policy of the given surface to the given
 * value. By default, newly-created surfaces will use lossy compression for
 * graphical updates when heuristics determine that doing so is appropriate.
 * Specifying a non-zero value here will force all graphical updates to always
 * use lossless compression, whereas specifying zero will restore the default
 * policy.
 *
 * @param surface
 *     The surface to modify.
 *
 * @param lossless
 *     Non-zero if all graphical updates for this surface should use lossless
 *     compression, 0 otherwise.
 */
void guac_surface_set_lossless(guac_surface* surface, int lossless);

#endif

