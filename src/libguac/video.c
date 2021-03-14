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

#include "config.h"
#include "ffmpeg-compat.h"
#include "guacamole/client.h"
#include "guacamole/protocol.h"
#include "guacamole/timestamp.h"
#include "guacamole/video.h"
#include "video-private.h"

#include <cairo/cairo.h>
#include <libavcodec/avcodec.h>
#include <libavutil/common.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/**
 * The framerate at which video should be encoded, in frames per second.
 */
#define GUACENC_VIDEO_FRAMERATE 25

guac_video* guac_video_alloc(guac_socket* socket, guac_stream* stream,
        const char* codec_name, int width, int height, int bitrate) {

    /* Pull codec based on name */
    AVCodec* codec = avcodec_find_encoder_by_name(codec_name);
    if (codec == NULL) {
        /* guacenc_log(GUAC_LOG_ERROR, "Failed to locate codec \"%s\".",
                codec_name); */
        goto fail_codec;
    }

    /* Retrieve encoding context */
    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (context == NULL) {
        /* guacenc_log(GUAC_LOG_ERROR, "Failed to allocate context for "
                "codec \"%s\".", codec_name); */
        goto fail_context;
    }

    /* Init context with encoding parameters */
    context->bit_rate = bitrate;
    context->width = width;
    context->height = height;
    context->time_base = (AVRational) { 1, GUACENC_VIDEO_FRAMERATE };
    context->max_b_frames = 0;
    context->pix_fmt = AV_PIX_FMT_YUV420P;

    if (av_opt_set(context->priv_data, "preset", "ultrafast", 0))
        fprintf(stderr, "Failed to set preset\n");

    if (av_opt_set(context->priv_data, "tune", "zerolatency", 0))
        fprintf(stderr, "Failed to set tune\n");

    /* -pass 1 -coder 0 -bf 0 -flags -loop -wpredp 0 */
    if (av_opt_set(context->priv_data, "pass", "1", AV_OPT_SEARCH_CHILDREN))
        fprintf(stderr, "Failed to set pass\n");

    if (av_opt_set(context->priv_data, "coder", "0", 0))
        fprintf(stderr, "Failed to set coder\n");

    if (av_opt_set(context->priv_data, "flags", "-loop", 0))
        fprintf(stderr, "Failed to set flags\n");

    /*
    if (av_opt_set(context->priv_data, "bsf", "h264_mp4toannexb", 0))
        fprintf(stderr, "Failed to set bsf\n");
    */

    if (av_opt_set(context->priv_data, "wpredp", "0", 0))
        fprintf(stderr, "Failed to set wpredp\n");

    av_opt_set(context->priv_data, "quality", "realtime", AV_OPT_SEARCH_CHILDREN);

    /* Open codec for use */
    if (avcodec_open2(context, codec, NULL) < 0) {
        /* guacenc_log(GUAC_LOG_ERROR, "Failed to open codec \"%s\".", codec_name); */
        goto fail_codec_open;
    }

    /* Allocate corresponding frame */
    AVFrame* frame = av_frame_alloc();
    if (frame == NULL) {
        goto fail_frame;
    }

    /* Copy necessary data for frame from context */
    frame->format = context->pix_fmt;
    frame->width = context->width;
    frame->height = context->height;

    /* Allocate actual backing data for frame */
    if (av_image_alloc(frame->data, frame->linesize, frame->width,
                frame->height, frame->format, 32) < 0) {
        goto fail_frame_data;
    }

    /* Allocate video structure */
    guac_video* video = malloc(sizeof(guac_video));
    if (video == NULL) {
        goto fail_video;
    }

    /* Init properties of video */
    video->socket = socket;
    video->stream = stream;
    video->context = context;
    video->next_frame = frame;
    video->width = width;
    video->height = height;
    video->bitrate = bitrate;

    /* No frames have been written or prepared yet */
    video->last_timestamp = 0;
    video->next_pts = 0;

    return video;

    /* Free all allocated data in case of failure */
fail_video:
    av_freep(&frame->data[0]);

fail_frame_data:
    av_frame_free(&frame);

fail_frame:
fail_codec_open:
    avcodec_free_context(&context);

fail_context:
fail_codec:
    return NULL;

}

/**
 * Flushes the specied frame as a new frame of video, updating the internal
 * video timestamp by one frame's worth of time. The pts member of the given
 * frame structure will be updated with the current presentation timestamp of
 * the video. If pending frames of the video are being flushed, the given frame
 * may be NULL (as required by avcodec_encode_video2()).
 *
 * @param video
 *     The video to write the given frame to.
 *
 * @param frame
 *     The frame to write to the video, or NULL if previously-written frames
 *     are being flushed.
 *
 * @return
 *     A positive value if the frame was successfully written, zero if the
 *     frame has been saved for later writing / reordering, negative if an
 *     error occurs.
 */
static int guac_video_write_frame(guac_video* video, AVFrame* frame) {

    /* Set timestamp of frame, if frame given */
    if (frame != NULL)
        frame->pts = video->next_pts;

    /* Write frame to video */
    int got_data = guac_avcodec_encode_video(video, frame);
    if (got_data < 0)
        return -1;

    /* Update presentation timestamp for next frame */
    video->next_pts++;

    /* Write was successful */
    return got_data;

}

/**
 * Flushes the frame previously specified by guac_video_prepare_frame() as a
 * new frame of video, updating the internal video timestamp by one frame's
 * worth of time.
 *
 * @param video
 *     The video to flush.
 *
 * @return
 *     Zero if flushing was successful, non-zero if an error occurs.
 */
static int guac_video_flush_frame(guac_video* video) {

    /* Write frame to video */
    return guac_video_write_frame(video, video->next_frame) < 0;

}

int guac_video_advance_timeline(guac_video* video,
        guac_timestamp timestamp) {

    guac_timestamp next_timestamp = timestamp;

    /* Flush frames as necessary if previously updated */
    if (video->last_timestamp != 0) {

        /* Calculate the number of frames that should have been written */
        int elapsed = (timestamp - video->last_timestamp)
                    * GUACENC_VIDEO_FRAMERATE / 1000;

        /* Keep previous timestamp if insufficient time has elapsed */
        if (elapsed == 0)
            return 0;

        /* Use frame time as last_timestamp */
        next_timestamp = video->last_timestamp
                        + elapsed * 1000 / GUACENC_VIDEO_FRAMERATE;

        /* Flush frames to bring timeline in sync, duplicating if necessary */
        do {
            if (guac_video_flush_frame(video)) {
                /* guacenc_log(GUAC_LOG_ERROR, "Unable to flush frame to video "
                        "stream."); */
                return 1;
            }
        } while (--elapsed != 0);

    }

    /* Update timestamp */
    video->last_timestamp = next_timestamp;
    return 0;

}

/**
 * Converts the given raw image data into an AVFrame object usable by FFMpeg.
 *
 * @param src_data
 *     Raw image data in RGB32 format, with the lowest-order byte being the
 *     blue component and the highest-order byte being ignored (identical to
 *     Cairo's CAIRO_FORMAT_RGB24 format).
 *
 * @param width
 *     The width of the image data, in pixels.
 *
 * @param height
 *     The height of the image data, in pixels.
 *
 * @param src_stride
 *     The number of bytes between each row of image data within src_data.
 *
 * @return
 *     A newly-allocated AVFrame containing an identical copy of the provided
 *     image data.
 */
static AVFrame* guac_video_convert_frame(unsigned char* src_data, int width,
        int height, int src_stride) {

    /* Prepare source frame for buffer */
    AVFrame* frame = av_frame_alloc();
    if (frame == NULL)
        return NULL;

    /* Copy buffer properties to frame */
    frame->format = AV_PIX_FMT_RGB32;
    frame->width = width;
    frame->height = height;

    /* Allocate actual backing data for frame */
    if (av_image_alloc(frame->data, frame->linesize, frame->width,
                frame->height, frame->format, 32) < 0) {
        av_frame_free(&frame);
        return NULL;
    }

    /* Get pointer to destination image data */
    unsigned char* dst_data = frame->data[0];
    int dst_stride = frame->linesize[0];

    /* Copy all data from source buffer to destination frame */
    while (height > 0) {

        memcpy(dst_data, src_data, width * 4);

        dst_data += dst_stride;
        src_data += src_stride;

        height--;

    }

    /* Frame converted */
    return frame;

}

void guac_video_prepare_frame(guac_video* video, cairo_surface_t* surface) {

    int width = cairo_image_surface_get_width(surface);
    int height = cairo_image_surface_get_height(surface);
    int stride = cairo_image_surface_get_stride(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);

    /* Obtain destination frame */
    AVFrame* dst = video->next_frame;

    /* Prepare scaling context */
    struct SwsContext* sws = sws_getContext(width, height,
            AV_PIX_FMT_RGB32, dst->width, dst->height, AV_PIX_FMT_YUV420P,
            SWS_BICUBIC, NULL, NULL, NULL);

    /* Abort if scaling context could not be created */
    if (sws == NULL) {
        /* guacenc_log(GUAC_LOG_WARNING, "Failed to allocate software scaling "
                "context. Frame dropped."); */
        return;
    }

    /* Flush pending operations to surface */
    cairo_surface_flush(surface);

    /* Prepare source frame for surface */
    AVFrame* src = guac_video_convert_frame(data, width, height, stride);
    if (src == NULL) {
        /* guacenc_log(GUAC_LOG_WARNING, "Failed to allocate source frame. "
                "Frame dropped."); */
        return;
    }

    /* Apply scaling, copying the source frame to the destination */
    sws_scale(sws, (const uint8_t* const*) src->data, src->linesize,
            0, height, dst->data, dst->linesize);

    /* Free scaling context */
    sws_freeContext(sws);

    /* Free source frame */
    av_freep(&src->data[0]);
    av_frame_free(&src);

    guac_video_flush_frame(video);

    guac_protocol_send_sync(video->socket, video->last_timestamp, 1);
    guac_socket_flush(video->socket);

}

int guac_video_free(guac_video* video) {

    /* Ignore NULL video */
    if (video == NULL)
        return 0;

    /* Write final frame */
    guac_video_flush_frame(video);

    /* Init video packet for final flush of encoded data */
    AVPacket packet;
    av_init_packet(&packet);

    /* Flush any unwritten frames */
    int retval;
    do {
        retval = guac_video_write_frame(video, NULL);
    } while (retval > 0);

    /* Video is now completely written */
    guac_protocol_send_end(video->socket, video->stream);

    /* Free frame encoding data */
    av_freep(&video->next_frame->data[0]);
    av_frame_free(&video->next_frame);

    /* Clean up encoding context */
    avcodec_close(video->context);
    avcodec_free_context(&(video->context));

    free(video);
    return 0;

}

