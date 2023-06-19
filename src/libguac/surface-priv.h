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

#ifndef GUAC_SURFACE_PRIV_H
#define GUAC_SURFACE_PRIV_H

/*
 * Surface implementation leveraging tiles to ensure spacial locality during
 * image processing. Each surface is split into logical tiles of 64x64 pixels,
 * with those tiles actually consisting of two 64x64 images whose rows are
 * interleaved. One of the two 64x64 images represents the current/old state,
 * while the other represents the new state. Each time a tile is flushed, fast
 * internal comparisons are made to determine what parts of the tile need to be
 * encoded and sent, and an internal reference noting which image is old vs.
 * new is swapped.
 */

/**
 * Provides initial data structurs and functions for manupulating
 * statically-sized tiles. Each tile is intended to be used alongside other
 * tiles to build a surface.
 *
 * @file surface-tile.h
 */

#include "config.h"
#include "guacamole/client.h"
#include "guacamole/surface.h"
#include "surface-tile.h"

#include <pthread.h>

struct guac_surface {

    /**
     * The guac_client related to all graphical operations that will be
     * performed on this surface.
     */
    guac_client* client;

    /**
     * The guac_socket that all instructions should be flushed to.
     */
    guac_socket* socket;

    /**
     * The guac_layer related to all graphical operations that will be
     * performed on this surface.
     */
    const guac_layer* layer;

    /**
     * Mutex that must be acquired before any property of this surface is read
     * or modified, with the exception of the guac_client, guac_socket, and
     * guac_layer pointers that are set only when this structure is first
     * allocated.
     */
    pthread_mutex_t lock;

    /**
     * Whether the graphical content of at least one tile of this surface has
     * been modified since last flush.
     */
    int content_dirty;

    /**
     * Whether the width and/or height properties of this guac_surface have
     * been modified since last flush.
     */
    int size_dirty;

    /**
     * The width of this surface, in pixels.
     */
    int width;

    /**
     * The height of this surface, in pixels.
     */
    int height;

    /**
     * The number of rows of tiles allocated within the tiles array. The tiles
     * array may contain more rows than strictly necessary to contain the
     * height of the surface.
     */
    int rows;

    /**
     * The number of columns of tiles allocated within the tiles array. The
     * tiles array may contain more columns than strictly necessary to contain
     * the width of the surface.
     */
    int columns;

    /**
     * All tiles making up the graphical content of this surface, as a
     * dynamically-allocated, two-dimensional array of tiles in row-major
     * order. The number of rows and columns in this array are stored in the
     * "rows" and "columns" members of this guac_surface.
     */
    guac_surface_tile** tiles;

};

#endif

