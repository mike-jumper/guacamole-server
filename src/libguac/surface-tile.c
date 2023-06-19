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

#include "guacamole/client.h"
#include "guacamole/user.h"
#include "surface-tile.h"

#include <stdlib.h>
#include <string.h>

static cairo_surface_t* guac_surface_tile_page(guac_surface_tile* tile, int page) {
    return cairo_image_surface_create_for_data(
        ((unsigned char*) tile->buffer) + (page * GUAC_SURFACE_TILE_ROW_SIZE),
        CAIRO_FORMAT_RGB24,
        GUAC_SURFACE_TILE_WIDTH,
        GUAC_SURFACE_TILE_HEIGHT,
        GUAC_SURFACE_TILE_STRIDE
    );
}

guac_surface_tile* guac_surface_tile_alloc(int x, int y) {

    guac_surface_tile* tile = (guac_surface_tile*) calloc(1, sizeof(guac_surface_tile));

    tile->x = x;
    tile->y = y;

    tile->pages[0] = guac_surface_tile_page(tile, 0);
    tile->pages[1] = guac_surface_tile_page(tile, 1);

    return tile;

}

void guac_surface_tile_free(guac_surface_tile* tile) {
    cairo_surface_destroy(tile->pages[0]);
    cairo_surface_destroy(tile->pages[1]);
    free(tile);
}

void guac_surface_tile_put(guac_surface_tile* tile, int x, int y,
        unsigned char* buffer, int width, int height, int stride) {

    /* Calculate extents of tile rect */
    int left   = tile->x;
    int top    = tile->y;
    int right  = left + GUAC_SURFACE_TILE_WIDTH;
    int bottom = top  + GUAC_SURFACE_TILE_HEIGHT;

    /* Calculate extents of buffer */
    int max_left   = x;
    int max_top    = y;
    int max_right  = max_left + width;
    int max_bottom = max_top  + height;

    /* Contrain tile rect within buffer */
    if (max_left   > left)   left   = max_left;
    if (max_top    > top)    top    = max_top;
    if (max_right  < right)  right  = max_right;
    if (max_bottom < bottom) bottom = max_bottom;

    int src_x = left - x;
    int src_y = top - y;
    int dst_x = left - tile->x;
    int dst_y = top - tile->y;

    width = right - left;
    height = bottom - top;

    if (width <= 0)
        return;

    if (height <= 0)
        return;

    unsigned char* src = buffer + (src_y * stride) + (src_x * 4);

    unsigned char* dst_old = ((unsigned char*) tile->buffer)
        + ((1 - tile->current_page) * GUAC_SURFACE_TILE_ROW_SIZE)
        + (dst_y * GUAC_SURFACE_TILE_STRIDE) + (dst_x * 4);

    unsigned char* dst_new = ((unsigned char*) tile->buffer)
        + (tile->current_page * GUAC_SURFACE_TILE_ROW_SIZE)
        + (dst_y * GUAC_SURFACE_TILE_STRIDE) + (dst_x * 4);

    for (int i = 0; i < height; i++) {

        memcpy(dst_new, src, width*4);

        if (!tile->dirty)
            tile->dirty = memcmp(dst_old, dst_new, width*4);

        src += stride;
        dst_new += GUAC_SURFACE_TILE_STRIDE;
        dst_old += GUAC_SURFACE_TILE_STRIDE;

    }

    if (tile->dirty) {
        cairo_surface_mark_dirty(tile->pages[tile->current_page]);
    }

}

void guac_surface_tile_flush(guac_surface_tile* tile, guac_client* client,
        guac_socket* socket, const guac_layer* layer) {

    if (tile->dirty) {

        unsigned char* dst_old = ((unsigned char*) tile->buffer)
            + ((1 - tile->current_page) * GUAC_SURFACE_TILE_ROW_SIZE);

        unsigned char* dst_new = ((unsigned char*) tile->buffer)
            + (tile->current_page * GUAC_SURFACE_TILE_ROW_SIZE);

        for (int i = 0; i < GUAC_SURFACE_TILE_HEIGHT; i++) {
            memcpy(dst_old, dst_new, GUAC_SURFACE_TILE_WIDTH*4);
            dst_new += GUAC_SURFACE_TILE_STRIDE;
            dst_old += GUAC_SURFACE_TILE_STRIDE;
        }

        guac_client_stream_png(client, socket, GUAC_COMP_OVER, layer,
                tile->x, tile->y, tile->pages[tile->current_page]);

        tile->current_page = 1 - tile->current_page;
        tile->dirty = 0;

    }

}

void guac_surface_tile_dup(guac_surface_tile* tile, guac_user* user,
        guac_socket* socket, const guac_layer* layer) {

    guac_user_stream_png(user, socket, GUAC_COMP_OVER, layer,
            tile->x, tile->y, tile->pages[1 - tile->current_page]);

}

