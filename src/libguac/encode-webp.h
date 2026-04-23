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

#ifndef GUAC_ENCODE_WEBP_H
#define GUAC_ENCODE_WEBP_H

#include "guacamole/protocol-types.h"
#include "guacamole/socket.h"
#include "guacamole/stream.h"

#include <cairo/cairo.h>

/**
 * Encodes the given surface as a WebP, and sends the resulting data over the
 * given stream and socket as blobs.
 *
 * @param socket
 *     The socket to send WebP blobs over.
 *
 * @param stream
 *     The stream to associate with each blob.
 *
 * @param surface
 *     The Cairo surface to write to the given stream and socket as PNG blobs.
 *
 * @param quality
 *     The WebP image quality to use. For lossy images, larger values indicate
 *     improving quality at the expense of larger file size. For lossless
 *     images, this dictates the quality of compression, with larger values
 *     producing smaller files at the expense of speed.
 *
 * @param target_bytes
 *     If greater than zero, the encoder targets this byte count directly
 *     (via libwebp's WebPConfig::target_size) and quality is treated only
 *     as an upper bound. If zero, the encoder runs in pure quality-only
 *     mode and emits whatever size the given quality produces. Ignored
 *     when lossless is nonzero, as lossless WebP has no notion of a
 *     target size.
 *
 * @param lossless
 *     Zero for a lossy image, non-zero for lossless.
 *
 * @param hint
 *     Advisory content-nature hint, for encoder parameter selection
 *     consistent with guac_png_write's signature. Currently unused by
 *     the WebP encoder - per-content parameter sweeps at benchmark/
 *     webp/ show the production setting (method=0) is universally
 *     fastest across content types, so no hint-driven branching is
 *     warranted today. The parameter is plumbed through anyway so the
 *     caller interface stays symmetric with PNG and future hint-
 *     driven tuning (e.g. adaptive method for bandwidth-constrained
 *     lossy encoding) can land without another signature change.
 *
 * @return
 *     Zero if the encoding operation is successful, non-zero otherwise.
 */
int guac_webp_write(guac_socket* socket, guac_stream* stream,
        cairo_surface_t* surface, int quality, int target_bytes,
        int lossless, guac_image_hint hint);

#endif
