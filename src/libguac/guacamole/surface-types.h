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

#ifndef GUAC_SURFACE_TYPES_H
#define GUAC_SURFACE_TYPES_H

/**
 * Provides initial data structurs and functions for manupulating
 * statically-sized tiles. Each tile is intended to be used alongside other
 * tiles to build a surface.
 *
 * @file surface-tile.h
 */

/**
 * A graphical surface that abstracts away internals of the Guacamole protocol,
 * automatically combining and optimizing drawing operations for transmission
 * over a network.
 */
typedef struct guac_surface guac_surface;

#endif

