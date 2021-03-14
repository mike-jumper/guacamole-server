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

#ifndef GUAC_VIDEO_PRIVATE_H
#define GUAC_VIDEO_PRIVATE_H

#include "guacamole/socket.h"
#include "guacamole/stream.h"
#include "guacamole/timestamp.h"

#include <libavcodec/avcodec.h>

#include <stdint.h>

struct guac_video {

    /**
     * The socket that should receive Guacamole instructions related to the
     * output video stream.
     */
    guac_socket* socket;

    /**
     * The output video stream.
     */
    guac_stream* stream;

    /**
     * The open encoding context from libavcodec, created for the codec
     * specified when this guac_video was created.
     */
    AVCodecContext* context;

    /**
     * The width of the video, in pixels.
     */
    int width;

    /**
     * The height of the video, in pixels.
     */
    int height;

    /**
     * The desired output bitrate of the video, in bits per second.
     */
    int bitrate;

    /**
     * An image data area containing the next frame to be written, encoded as
     * YCbCr image data in the format required by avcodec_encode_video2(), for
     * use and re-use as frames are rendered.
     */
    AVFrame* next_frame;

    /**
     * The presentation timestamp that should be used for the next frame. This
     * is equivalent to the frame number.
     */
    int64_t next_pts;

    /**
     * The timestamp associated with the last frame, or 0 if no frames have yet
     * been added.
     */
    guac_timestamp last_timestamp;

};

#endif

