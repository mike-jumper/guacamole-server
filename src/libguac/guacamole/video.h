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

#ifndef GUAC_VIDEO_H
#define GUAC_VIDEO_H

#include "socket-types.h"
#include "stream-types.h"
#include "timestamp-types.h"
#include "video-types.h"

#include <cairo/cairo.h>

/**
 * Allocates a new guac_video which encodes video according to the given
 * specifications, saving the output in the given file. If the output file
 * already exists, encoding will be aborted, and the original file contents
 * will be preserved. Frames will be scaled up or down as necessary to fit the
 * given width and height.
 *
 * @param path
 *     The full path to the file in which encoded video should be written.
 *
 * @param codec_name
 *     The name of the codec to use for the video encoding, as defined by
 *     ffmpeg / libavcodec.
 *
 * @param width
 *     The width of the desired video, in pixels.
 *
 * @param height
 *     The height of the desired video, in pixels.
 *
 * @param bitrate
 *     The desired overall bitrate of the resulting encoded video, in bits per
 *     second.
 */
guac_video* guac_video_alloc(guac_socket* socket, guac_stream* stream,
        const char* codec_name, int width, int height, int bitrate);

/**
 * Advances the timeline of the encoding process to the given timestamp, such
 * that frames added via guac_video_prepare_frame() will be encoded at the
 * proper frame boundaries within the video. Duplicate frames will be encoded
 * as necessary to ensure that the output is correctly timed with respect to
 * the given timestamp. This is particularly important as Guacamole does not
 * have a framerate per se, and the time between each Guacamole "frame" will
 * vary significantly.
 *
 * This function MUST be called prior to invoking guac_video_prepare_frame()
 * to ensure the prepared frame will be encoded at the correct point in time.
 *
 * @param video
 *     The video whose timeline should be adjusted.
 *
 * @param timestamp
 *     The Guacamole timestamp denoting the point in time that the video
 *     timeline should be advanced to, as dictated by a parsed "sync"
 *     instruction.
 *
 * @return
 *     Zero if the timeline was adjusted successfully, non-zero if an error
 *     occurs (such as during the encoding of duplicate frames).
 */
int guac_video_advance_timeline(guac_video* video, guac_timestamp timestamp);

/**
 * Stores the given buffer within the given video structure such that it will
 * be written if it falls within proper frame boundaries. If the timeline of
 * the video (as dictated by guac_video_advance_timeline()) is not at a
 * frame boundary with respect to the video framerate (it occurs between frame
 * boundaries), the prepared frame will only be written if another frame is not
 * prepared within the same pair of frame boundaries). The prepared frame will
 * not be written until it is implicitly flushed through updates to the video
 * timeline or through reaching the end of the encoding process
 * (guac_video_free()).
 *
 * @param video
 *     The video in which the given buffer should be queued for possible
 *     writing (depending on timing vs. video framerate).
 *
 * @param buffer
 *     The guacenc_buffer representing the image data of the frame that should
 *     be queued.
 */
void guac_video_prepare_frame(guac_video* video, cairo_surface_t* surface);

/**
 * Frees all resources associated with the given video, finalizing the encoding
 * process. Any buffered frames which have not yet been written will be written
 * at this point.
 *
 * @return
 *     Zero if the video was successfully written and freed, non-zero if the
 *     video could not be written due to an error.
 */
int guac_video_free(guac_video* video);

#endif

