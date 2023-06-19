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

#ifndef GUAC_SURFACE_TILE_H
#define GUAC_SURFACE_TILE_H

/**
 * Provides initial data structurs and functions for manupulating
 * statically-sized tiles. Each tile is intended to be used alongside other
 * tiles to build a surface.
 *
 * @file surface-tile.h
 */

#include "guacamole/client.h"
#include "guacamole/layer.h"
#include "guacamole/socket.h"

#include <stdint.h>

/**
 * The width of each tile, in pixels.
 */
#define GUAC_SURFACE_TILE_WIDTH 64

/**
 * The height of each tile, in pixels.
 */
#define GUAC_SURFACE_TILE_HEIGHT 64

/**
 * The number of bytes in each row of image data.
 */
#define GUAC_SURFACE_TILE_ROW_SIZE (GUAC_SURFACE_TILE_WIDTH*4)

/**
 * The number of bytes separating adjacent rows of image data within the same
 * page of the tile.
 */
#define GUAC_SURFACE_TILE_STRIDE (GUAC_SURFACE_TILE_ROW_SIZE*2)

typedef struct guac_surface_tile {

    /**
     * The X coordinate of the upper-left corner of this tile within its
     * containing surface, in pixels.
     */
    int x;

    /**
     * The Y coordinate of the upper-left corner of this tile within its
     * containing surface, in pixels.
     */
    int y;

    /**
     * Whether this tile has been modified at all since the associated surface
     * was last flushed.
     */
    int dirty;

    int current_page;

    cairo_surface_t* pages[2];

    uint32_t buffer[GUAC_SURFACE_TILE_WIDTH * GUAC_SURFACE_TILE_HEIGHT * 2];

} guac_surface_tile;

guac_surface_tile* guac_surface_tile_alloc(int x, int y);

void guac_surface_tile_free(guac_surface_tile* tile);

void guac_surface_tile_flush(guac_surface_tile* tile, guac_client* client,
        guac_socket* socket, const guac_layer* layer);

void guac_surface_tile_dup(guac_surface_tile* tile, guac_user* user,
        guac_socket* socket, const guac_layer* layer);

void guac_surface_tile_put(guac_surface_tile* tile, int x, int y,
        unsigned char* buffer, int width, int height, int stride);

#endif
