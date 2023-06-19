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

#include <pthread.h>
#include <stdlib.h>

#include "guacamole/client.h"
#include "guacamole/layer.h"
#include "guacamole/protocol.h"
#include "guacamole/socket.h"
#include "guacamole/surface.h"
#include "surface-priv.h"
#include "surface-tile.h"

guac_surface* guac_surface_alloc(guac_client* client, guac_socket* socket,
        const guac_layer* layer, int width, int height) {

    guac_surface* surface = malloc(sizeof(guac_surface));
    surface->client = client;
    surface->socket = socket;
    surface->layer = layer;

    surface->content_dirty = 0;
    surface->size_dirty = 1;
    surface->width = width;
    surface->height = height;

    surface->rows = (height + GUAC_SURFACE_TILE_HEIGHT - 1) / GUAC_SURFACE_TILE_HEIGHT;
    surface->columns = (width + GUAC_SURFACE_TILE_WIDTH - 1) / GUAC_SURFACE_TILE_WIDTH;
    surface->tiles = malloc(sizeof(guac_surface_tile*) * surface->rows * surface->columns);

    guac_surface_tile** current = surface->tiles;
    for (int row = 0; row < surface->rows; row++) {
        for (int column = 0; column < surface->columns; column++) {
            *(current++) = guac_surface_tile_alloc(
                column * GUAC_SURFACE_TILE_WIDTH,
                row * GUAC_SURFACE_TILE_HEIGHT
            );
        }
    }

    pthread_mutex_init(&(surface->lock), NULL);

    return surface;

}

void guac_surface_free(guac_surface* surface) {

    pthread_mutex_destroy(&(surface->lock));

    guac_surface_tile** current = surface->tiles;
    for (int row = 0; row < surface->rows; row++) {
        for (int column = 0; column < surface->columns; column++) {
            guac_surface_tile_free(*(current++));
        }
    }

    free(surface->tiles);
    free(surface);

}

int guac_surface_resize(guac_surface* surface, int width, int height) {

    if (width > GUAC_SURFACE_MAX_WIDTH || height > GUAC_SURFACE_MAX_HEIGHT)
        return 1;

    pthread_mutex_lock(&(surface->lock));

    int new_rows = (height + GUAC_SURFACE_TILE_HEIGHT - 1) / GUAC_SURFACE_TILE_HEIGHT;
    int new_columns = (width + GUAC_SURFACE_TILE_WIDTH - 1) / GUAC_SURFACE_TILE_WIDTH;
    guac_surface_tile** new_tiles = malloc(sizeof(guac_surface_tile*) * new_rows * new_columns);

    int max_rows = new_rows;
    if (surface->rows > max_rows)
        max_rows = surface->rows;

    int max_columns = new_columns;
    if (surface->columns > max_columns)
        max_columns = surface->columns;

    guac_surface_tile** old_tile = surface->tiles;
    guac_surface_tile** new_tile = new_tiles;

    for (int row = 0; row < max_rows; row++) {
        for (int column = 0; column < max_columns; column++) {

            /* Any tile within the bounds of the newly-allocated tile space
             * must either be freshly allocated or copied from the previous set
             * of tiles */
            if (row < new_rows && column < new_columns) {
                if (row < surface->rows && column < surface->columns) {
                    *(new_tile++) = *(old_tile++);
                }
                else
                    *(new_tile++) = guac_surface_tile_alloc(
                    column * GUAC_SURFACE_TILE_WIDTH,
                    row * GUAC_SURFACE_TILE_HEIGHT
                );
            }

            /* Any tile NOT within the bounds of the newly-allocated tile space
             * must only be within the bounds of the old surface and is no
             * longer needed */
            else
                guac_surface_tile_free(*(old_tile++));

        }
    }

    free(surface->tiles);

    surface->tiles = new_tiles;
    surface->rows = new_rows;
    surface->columns = new_columns;

    surface->width = width;
    surface->height = height;
    surface->size_dirty = 1;

    pthread_mutex_unlock(&(surface->lock));

    return 0;

}

void guac_surface_draw(guac_surface* surface, int x, int y,
        cairo_surface_t* src) {

    pthread_mutex_lock(&(surface->lock));

    /* Ignore draws out of bounds */
    if (x >= surface->width || y >= surface->height) {
        pthread_mutex_unlock(&(surface->lock));
        return;
    }

    unsigned char* buffer = cairo_image_surface_get_data(src);
    /*cairo_format_t format = cairo_image_surface_get_format(src);*/
    int stride = cairo_image_surface_get_stride(src);
    int width = cairo_image_surface_get_width(src);
    int height = cairo_image_surface_get_height(src);

    /* TODO: Bound source buffer width/height by surface width/height */

    int row = y / GUAC_SURFACE_TILE_HEIGHT;
    int column = x / GUAC_SURFACE_TILE_WIDTH;
    int last_row = (y + height - 1) / GUAC_SURFACE_TILE_HEIGHT;
    int last_column = (x + width - 1) / GUAC_SURFACE_TILE_WIDTH;

    guac_surface_tile** current_row = surface->tiles + (surface->columns * row) + column;
    for (int cur_row = row; cur_row <= last_row; cur_row++) {

        guac_surface_tile** current = current_row;
        current_row += surface->columns;

        for (int cur_column = column; cur_column <= last_column; cur_column++) {
            guac_surface_tile_put(*current, x, y, buffer, width, height, stride);
            current++;
        }

    }

    surface->content_dirty = 1;

    pthread_mutex_unlock(&(surface->lock));

}

void guac_surface_paint(guac_surface* surface, int x, int y,
        cairo_surface_t* src, int red, int green, int blue) {
    /* STUB */
    guac_client_log(surface->client, GUAC_LOG_INFO, "STUB: %s", __func__);
}

void guac_surface_copy(guac_surface* src, int sx, int sy, int width, int height,
        guac_surface* dst, int dx, int dy) {
    /* TODO */
    guac_client_log(src->client, GUAC_LOG_INFO, "TODO: %s", __func__);
}

void guac_surface_transfer(guac_surface* src, int sx, int sy, int width, int height,
        guac_transfer_function op, guac_surface* dst, int dx, int dy) {
    /* STUB */
    guac_client_log(dst->client, GUAC_LOG_INFO, "STUB: %s", __func__);
}

void guac_surface_set(guac_surface* surface, int x, int y,
        int w, int h, int red, int green, int blue, int alpha) {
    /* STUB */
    guac_client_log(surface->client, GUAC_LOG_INFO, "STUB: %s", __func__);
}

void guac_surface_clip(guac_surface* surface, int x, int y,
        int width, int height) {
    /* STUB */
    guac_client_log(surface->client, GUAC_LOG_INFO, "STUB: %s", __func__);
}

void guac_surface_reset_clip(guac_surface* surface) {
    /* STUB */
    guac_client_log(surface->client, GUAC_LOG_INFO, "STUB: %s", __func__);
}

void guac_surface_move(guac_surface* surface, int x, int y) {
    /* STUB */
    guac_client_log(surface->client, GUAC_LOG_INFO, "STUB: %s", __func__);
}

void guac_surface_stack(guac_surface* surface, int z) {
    /* STUB */
    guac_client_log(surface->client, GUAC_LOG_INFO, "STUB: %s", __func__);
}

void guac_surface_set_parent(guac_surface* surface, const guac_layer* parent) {
    /* STUB */
    guac_client_log(surface->client, GUAC_LOG_INFO, "STUB: %s", __func__);
}

void guac_surface_set_opacity(guac_surface* surface, int opacity) {
    /* STUB */
    guac_client_log(surface->client, GUAC_LOG_INFO, "STUB: %s", __func__);
}

void guac_surface_flush(guac_surface* surface) {

    pthread_mutex_lock(&(surface->lock));

    if (surface->size_dirty) {
        guac_protocol_send_size(surface->socket, surface->layer,
                surface->width, surface->height);
        surface->size_dirty = 0;
    }

    if (surface->content_dirty) {

        guac_surface_tile** current = surface->tiles;
        for (int row = 0; row < surface->rows; row++) {
            for (int column = 0; column < surface->columns; column++) {
                guac_surface_tile_flush(*(current++), surface->client, surface->socket, surface->layer);
            }
        }

        surface->content_dirty = 0;

    }

    pthread_mutex_unlock(&(surface->lock));

}

void guac_surface_dup(guac_surface* surface, guac_user* user,
        guac_socket* socket) {
    /* STUB */
    guac_client_log(surface->client, GUAC_LOG_INFO, "STUB: %s", __func__);
}

void guac_surface_set_multitouch(guac_surface* surface, int touches) {
    /* STUB */
    guac_client_log(surface->client, GUAC_LOG_INFO, "STUB: %s", __func__);
}

void guac_surface_set_lossless(guac_surface* surface, int lossless) {
    /* STUB */
    guac_client_log(surface->client, GUAC_LOG_INFO, "STUB: %s", __func__);
}

