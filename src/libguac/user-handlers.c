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

#include "guacamole/mem.h"
#include "guacamole/client.h"
#include "guacamole/object.h"
#include "guacamole/protocol.h"
#include "guacamole/socket.h"
#include "guacamole/stream.h"
#include "guacamole/string.h"
#include "guacamole/timestamp.h"
#include "guacamole/user.h"
#include "user-handlers.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Guacamole instruction handler map */

__guac_instruction_handler_mapping __guac_instruction_handler_map[] = {
   {"sync",       __guac_handle_sync},
   {"touch",      __guac_handle_touch},
   {"mouse",      __guac_handle_mouse},
   {"key",        __guac_handle_key},
   {"clipboard",  __guac_handle_clipboard},
   {"disconnect", __guac_handle_disconnect},
   {"size",       __guac_handle_size},
   {"file",       __guac_handle_file},
   {"pipe",       __guac_handle_pipe},
   {"ack",        __guac_handle_ack},
   {"blob",       __guac_handle_blob},
   {"end",        __guac_handle_end},
   {"get",        __guac_handle_get},
   {"put",        __guac_handle_put},
   {"audio",      __guac_handle_audio},
   {"argv",       __guac_handle_argv},
   {"nop",        __guac_handle_nop},
   {NULL,         NULL}
};

/* Guacamole handshake handler map */

__guac_instruction_handler_mapping __guac_handshake_handler_map[] = {
    {"size",     __guac_handshake_size_handler},
    {"audio",    __guac_handshake_audio_handler},
    {"video",    __guac_handshake_video_handler},
    {"image",    __guac_handshake_image_handler},
    {"timezone", __guac_handshake_timezone_handler},
    {"name",     __guac_handshake_name_handler},
    {NULL,       NULL}
};

/**
 * Parses a 64-bit integer from the given string. It is assumed that the string
 * will contain only decimal digits, with an optional leading minus sign.
 * The result of parsing a string which does not conform to this pattern is
 * undefined.
 *
 * @param str
 *     The string to parse, which must contain only decimal digits and an
 *     optional leading minus sign.
 *
 * @return
 *     The 64-bit integer value represented by the given string.
 */
static int64_t __guac_parse_int(const char* str) {

    int sign = 1;
    int64_t num = 0;

    for (; *str != '\0'; str++) {

        if (*str == '-')
            sign = -sign;
        else
            num = num * 10 + (*str - '0');

    }

    return num * sign;

}

/* Guacamole instruction handlers */

int __guac_handle_sync(guac_user* user, int argc, char** argv) {

    int frame_duration;

    guac_timestamp current = guac_timestamp_current();
    guac_timestamp timestamp = __guac_parse_int(argv[0]);

    /* Error if timestamp is in future */
    if (timestamp > user->client->last_sent_timestamp)
        return -1;

    /* Only update lag calculations if timestamp is sane */
    if (timestamp >= user->last_received_timestamp) {

        /* Look up the ring entry for the sync this ack is for, and
         * accumulate the per-frame byte counts and per-frame
         * render times from the oldest live entry through the match
         * (inclusive). The byte sum is the numerator for the
         * network-throughput EMA; the render-time sum is the
         * denominator - server-side active pipeline duration
         * (encode + socket-write), which TCP backpressure naturally
         * extends on a bandwidth-bound link. This replaces an
         * earlier ack-interval denominator that included pre-
         * pipeline idle time (outer wait, modification
         * accumulation, pacing wait) and fed a feedback loop with
         * pacing decisions.
         *
         * guac_client_end_multiple_frames appends one history entry
         * per emit (strictly-ascending timestamps within the live
         * range [read_index, write_index)); a successful match
         * advances read_index past the match, implicitly dropping
         * all older entries (acks are monotonic, so no future ack
         * will need them).
         *
         * Linear scan with early-exit is the right choice here over
         * bsearch: in steady-state lockstep pacing the match is at
         * read_index (the oldest outstanding sync is the next one
         * to get acked), so the scan terminates on its first
         * compare - vs bsearch's unconditional log2(N) probes. The
         * ascending-timestamp invariant also lets us early-exit on
         * the first entry past the ack's timestamp, which handles
         * the overflow and never-tracked cases cheaply. */
        size_t frame_bytes_sum = 0;
        int render_time_sum_ms = 0;
        int have_sample = 0;

        /* "Consecutive" here means: this ack matches the FIRST entry
         * in the live range (match_n == 0), i.e. no previously-
         * emitted sync was skipped between the last ack we processed
         * and this one. When the client batches multiple syncs into
         * one ack - it acks only the latest sync timestamp it has
         * seen - the match lands further into the live range and
         * the byte sum spans multiple frames while the per-frame
         * client-processing-lag denominator reflects only one, so
         * we don't produce a processing-throughput sample for that
         * case. The network throughput sample stays valid:
         * bytes/ack_interval averages correctly over any window. */
        int have_consecutive_sample = 0;

        pthread_mutex_lock(&user->sync_emit_lock);

        /* Walk the live range [read_index, write_index) by length,
         * not by "i != write_index". A modular-index loop is fragile
         * near uint64 wrap - if the counters were close to UINT64_MAX
         * and wrapped across the boundary, i would wrap to 0 before
         * reaching write_index and the loop would spin for up to
         * ~UINT64_MAX extra iterations. Computing the count via
         * unsigned subtraction first gives a bounded small integer
         * (<= GUAC_USER_SYNC_EMIT_HISTORY_SIZE by invariant) and a
         * termination condition that doesn't depend on wrap behaviour.
         *
         * Physical slot access still uses (idx & MASK), where idx
         * can mathematically wrap without issue - only the low bits
         * matter for addressing. */
        uint64_t read_idx = user->sync_emit_read_index;
        uint64_t count = user->sync_emit_write_index - read_idx;

        for (uint64_t n = 0; n < count; n++) {

            uint64_t idx = read_idx + n;
            guac_user_sync_emit_record* rec =
                    &user->sync_emit_history[idx & GUAC_USER_SYNC_EMIT_HISTORY_MASK];

            /* Accumulate per-frame bytes and per-frame render times
             * as we walk so a match at position n yields totals
             * across all frames from the last ack through this one.
             * Entries past the match (future syncs not yet acked)
             * are left in the live range for subsequent acks. */
            frame_bytes_sum += rec->bytes;
            render_time_sum_ms += rec->render_time_ms;

            if (rec->timestamp == timestamp) {
                have_sample = 1;
                have_consecutive_sample = (n == 0);
                /* Advance read_index past the match. The matched
                 * entry's slot (and any intervening older entries
                 * for out-of-order acks) stays in place physically
                 * but is now outside the live range and will be
                 * overwritten by future emits. Subtract the walked
                 * entries from the cached live-range sum since
                 * they're now out-of-range. */
                user->sync_emit_read_index = idx + 1;
                user->sum_ring_bytes -= frame_bytes_sum;
                break;
            }

            /* Ascending invariant: once we see a timestamp past the
             * ack's, the ack's sync isn't in the live range
             * (displaced by overflow, or never tracked). Abort. */
            if (rec->timestamp > timestamp)
                break;

        }

        pthread_mutex_unlock(&user->sync_emit_lock);

        /* Update stored timestamp */
        user->last_received_timestamp = timestamp;

        /* Calculate length of frame, including network and processing lag */
        frame_duration = current - timestamp;

        /* Calculate processing lag portion of length of frame */
        int frame_processing_lag = 0;
        if (user->last_frame_duration != 0) {

            /* Calculate lag using the previous frame as a baseline */
            frame_processing_lag = frame_duration - user->last_frame_duration;

            /* Adjust back to zero if cumulative error leads to a negative
             * value */
            if (frame_processing_lag < 0)
                frame_processing_lag = 0;

        }

        /* Record baseline duration of frame by excluding lag (this is the
         * network round-trip time) */
        int estimated_rtt = frame_duration - frame_processing_lag;
        user->last_frame_duration = estimated_rtt;

        /* Calculate cumulative accumulated processing lag relative to server timeline */
        int processing_lag = current - user->last_received_timestamp - estimated_rtt;
        if (processing_lag < 0)
            processing_lag = 0;

        user->processing_lag = processing_lag;

        /* Update network-throughput EMA. The sample approximates
         * the effective emit rate - the rate at which the server
         * can actually push frame bytes onto the wire:
         *
         *     throughput = frame_bytes_sum / render_time_sum_ms
         *
         *   - frame_bytes_sum is the sum of per-frame byte counts
         *     from the ring entries between (and including) the
         *     previous acked sync and this one. Those bytes are
         *     exactly what flowed between the two syncs: acked
         *     content, no more, no less.
         *
         *   - render_time_sum_ms is the sum of server-side active
         *     pipeline durations for those same frames. Each
         *     render_time is measured from flush-start (render
         *     thread calling guac_display_end_multiple_frames) to
         *     last-worker sync emit - pure encode + socket-write
         *     time, excluding all pre-pipeline idle (outer wait,
         *     modification accumulation, pre-encoding pacing wait).
         *
         * Why NOT the ack-to-ack interval? Because it includes the
         * pre-pipeline idle, which then flows into the pacing-wait
         * decision for the next frame, which extends the next ack-
         * to-ack interval, which further depresses the sample - a
         * self-reinforcing feedback loop that collapses the EMA
         * toward the min-throughput floor over a few dozen frames.
         *
         * Why this works:
         *
         *   - Bandwidth-bound link: TCP backpressure extends
         *     socket_write time, so render_time_sum_ms tracks
         *     frame_bytes_sum / net_rate and the EMA converges to
         *     actual net_rate.
         *
         *   - Fast link: render_time_sum_ms is encode-CPU-bound,
         *     so the EMA reflects server encode rate - the useful
         *     upper bound for pacing in that regime (we can't
         *     drive the client faster than we can encode anyway).
         *
         * First sample seeds the EMA directly; subsequent samples
         * fold in with a smoothing factor of 1/4.
         *
         * Samples are skipped when: (a) we can't match the ack's
         * timestamp to a tracked sync emit (have_sample == 0), or
         * (b) render_time_sum_ms is not positive (no display-
         * pipeline record available for this sync; shouldn't
         * happen in display-driven emission but guarded
         * defensively). */
        if (have_sample && render_time_sum_ms > 0) {

            int sample = (int) (frame_bytes_sum
                    / (size_t) render_time_sum_ms);

            if (user->bytes_per_ms == 0)
                user->bytes_per_ms = sample;
            else
                user->bytes_per_ms =
                    (3 * user->bytes_per_ms + sample) / 4;

        }

        /* Update processing-throughput EMA. This sample measures
         * how fast the client can decode and render a single
         * frame's payload:
         *
         *     processing_throughput = frame_bytes
         *                             / frame_processing_lag
         *
         *   - frame_bytes is the byte count of this ack's single
         *     acked frame (the sum collapses to one entry when the
         *     match is at read_index).
         *
         *   - frame_processing_lag is the portion of the
         *     round-trip spent on the client after the sync
         *     arrived, estimated above as frame_duration minus the
         *     rolling RTT baseline. It tracks client-side
         *     decode+render time up to smoothing noise.
         *
         * Only produced when have_consecutive_sample is set: if
         * the client batches multiple syncs into one ack, the byte
         * sum spans multiple frames but frame_processing_lag
         * reflects only the last one, so the ratio would be
         * meaningless. Batched acks still produce valid
         * network-throughput samples above (bytes/ack_interval
         * averages correctly over any window); the skip only
         * affects the processing EMA.
         *
         * First sample seeds the EMA directly; subsequent samples
         * fold in with a smoothing factor of 1/4. */
        if (have_consecutive_sample
                && user->last_acked_sync_receipt_time != 0
                && frame_processing_lag > 0) {

            int proc_sample =
                    (int) (frame_bytes_sum / (size_t) frame_processing_lag);

            if (user->processing_bytes_per_ms == 0)
                user->processing_bytes_per_ms = proc_sample;
            else
                user->processing_bytes_per_ms =
                    (3 * user->processing_bytes_per_ms + proc_sample) / 4;

        }

        /* Advance the acked receipt-time baseline regardless of
         * whether a sample was produced - the next sample measures
         * bytes and interval relative to this ack, even if this ack
         * itself didn't contribute a sample. Skipping the update
         * when no sample is produced would cause the next sample's
         * interval to span multiple frames and misrepresent the
         * rate. */
        if (have_sample)
            user->last_acked_sync_receipt_time = current;

    }

    /* Log received timestamp and calculated lag (at TRACE level only) */
    guac_user_log(user, GUAC_LOG_TRACE,
            "User confirmation of frame %" PRIu64 "ms received "
            "at %" PRIu64 "ms (processing_lag=%ims, estimated_rtt=%ims, "
            "net_throughput=%ikB/s, proc_throughput=%ikB/s)",
            timestamp, current, user->processing_lag, user->last_frame_duration,
            user->bytes_per_ms, user->processing_bytes_per_ms);

    /* Signal sync receipt so any rate-limiting consumer timed-waiting
     * on the client-level sync_state flag wakes up and re-measures
     * with the fresh processing_lag / throughput numbers just written
     * above. The flag is a pure event - consumers clear it before
     * each wait, so setting it here unconditionally is correct even
     * across back-to-back syncs. */
    guac_flag_set(&user->client->sync_state, GUAC_CLIENT_SYNC_RECEIVED);

    if (user->sync_handler)
        return user->sync_handler(user, timestamp);
    return 0;
}

int __guac_handle_touch(guac_user* user, int argc, char** argv) {
    if (user->touch_handler)
        return user->touch_handler(
            user,
            atoi(argv[0]), /* id */
            atoi(argv[1]), /* x */
            atoi(argv[2]), /* y */
            atoi(argv[3]), /* x_radius */
            atoi(argv[4]), /* y_radius */
            atof(argv[5]), /* angle */
            atof(argv[6])  /* force */
        );
    return 0;
}

int __guac_handle_mouse(guac_user* user, int argc, char** argv) {
    if (user->mouse_handler)
        return user->mouse_handler(
            user,
            atoi(argv[0]), /* x */
            atoi(argv[1]), /* y */
            atoi(argv[2])  /* mask */
        );
    return 0;
}

int __guac_handle_key(guac_user* user, int argc, char** argv) {
    if (user->key_handler)
        return user->key_handler(
            user,
            atoi(argv[0]), /* keysym */
            atoi(argv[1])  /* pressed */
        );
    return 0;
}

/**
 * Retrieves the existing user-level input stream having the given index. These
 * will be streams which were created by the remotely-connected user. If the
 * index is invalid or too large, this function will automatically respond with
 * an "ack" instruction containing an appropriate error code.
 *
 * @param user
 *     The user associated with the stream being retrieved.
 *
 * @param stream_index
 *     The index of the stream to retrieve.
 *
 * @return
 *     The stream associated with the given user and having the given index,
 *     or NULL if the index is invalid.
 */
static guac_stream* __get_input_stream(guac_user* user, int stream_index) {

    /* Validate stream index */
    if (stream_index < 0 || stream_index >= GUAC_USER_MAX_STREAMS) {

        guac_stream dummy_stream;
        dummy_stream.index = stream_index;

        guac_protocol_send_ack(user->socket, &dummy_stream,
                "Invalid stream index", GUAC_PROTOCOL_STATUS_CLIENT_BAD_REQUEST);
        return NULL;
    }

    return &(user->__input_streams[stream_index]);

}

/**
 * Retrieves the existing, in-progress (open) user-level input stream having
 * the given index. These will be streams which were created by the
 * remotely-connected user. If the index is invalid, too large, or the stream
 * is closed, this function will automatically respond with an "ack"
 * instruction containing an appropriate error code.
 *
 * @param user
 *     The user associated with the stream being retrieved.
 *
 * @param stream_index
 *     The index of the stream to retrieve.
 *
 * @return
 *     The in-progress (open)stream associated with the given user and having
 *     the given index, or NULL if the index is invalid or the stream is
 *     closed.
 */
static guac_stream* __get_open_input_stream(guac_user* user, int stream_index) {

    guac_stream* stream = __get_input_stream(user, stream_index);

    /* Fail if no such stream */
    if (stream == NULL)
        return NULL;

    /* Validate initialization of stream */
    if (stream->index == GUAC_USER_CLOSED_STREAM_INDEX) {

        guac_stream dummy_stream;
        dummy_stream.index = stream_index;

        guac_protocol_send_ack(user->socket, &dummy_stream,
                "Invalid stream index", GUAC_PROTOCOL_STATUS_CLIENT_BAD_REQUEST);
        return NULL;
    }

    return stream;

}

/**
 * Initializes and returns a new user-level input stream having the given
 * index, clearing any values that may have been assigned by a past use of the
 * underlying stream object storage. If the stream was already open, it will
 * first be closed and its end handlers invoked as if explicitly closed by the
 * user.
 *
 * @param user
 *     The user associated with the stream being initialized.
 *
 * @param stream_index
 *     The index of the stream to initialized.
 *
 * @return
 *     A new initialized user-level input stream having the given index, or
 *     NULL if the index is invalid.
 */
static guac_stream* __init_input_stream(guac_user* user, int stream_index) {

    guac_stream* stream = __get_input_stream(user, stream_index);

    /* Fail if no such stream */
    if (stream == NULL)
        return NULL;

    /* Force end of previous stream if open */
    if (stream->index != GUAC_USER_CLOSED_STREAM_INDEX) {

        /* Call stream handler if defined */
        if (stream->end_handler)
            stream->end_handler(user, stream);

        /* Fall back to global handler if defined */
        else if (user->end_handler)
            user->end_handler(user, stream);

    }

    /* Initialize stream */
    stream->index = stream_index;
    stream->data = NULL;
    stream->ack_handler = NULL;
    stream->blob_handler = NULL;
    stream->end_handler = NULL;

    return stream;

}

int __guac_handle_audio(guac_user* user, int argc, char** argv) {

    /* Pull corresponding stream */
    int stream_index = atoi(argv[0]);
    guac_stream* stream = __init_input_stream(user, stream_index);
    if (stream == NULL)
        return 0;

    /* If supported, call handler */
    if (user->audio_handler)
        return user->audio_handler(
            user,
            stream,
            argv[1] /* mimetype */
        );

    /* Otherwise, abort */
    guac_protocol_send_ack(user->socket, stream,
            "Audio input unsupported", GUAC_PROTOCOL_STATUS_UNSUPPORTED);
    return 0;

}

int __guac_handle_clipboard(guac_user* user, int argc, char** argv) {

    /* Pull corresponding stream */
    int stream_index = atoi(argv[0]);
    guac_stream* stream = __init_input_stream(user, stream_index);
    if (stream == NULL)
        return 0;

    /* If supported, call handler */
    if (user->clipboard_handler)
        return user->clipboard_handler(
            user,
            stream,
            argv[1] /* mimetype */
        );

    /* Otherwise, abort */
    guac_protocol_send_ack(user->socket, stream,
            "Clipboard unsupported", GUAC_PROTOCOL_STATUS_UNSUPPORTED);
    return 0;

}

int __guac_handle_size(guac_user* user, int argc, char** argv) {
    if (user->size_handler)
        return user->size_handler(
            user,
            atoi(argv[0]), /* width */
            atoi(argv[1])  /* height */
        );
    return 0;
}

int __guac_handle_file(guac_user* user, int argc, char** argv) {

    /* Pull corresponding stream */
    int stream_index = atoi(argv[0]);
    guac_stream* stream = __init_input_stream(user, stream_index);
    if (stream == NULL)
        return 0;

    /* If supported, call handler */
    if (user->file_handler)
        return user->file_handler(
            user,
            stream,
            argv[1], /* mimetype */
            argv[2]  /* filename */
        );

    /* Otherwise, abort */
    guac_protocol_send_ack(user->socket, stream,
            "File transfer unsupported", GUAC_PROTOCOL_STATUS_UNSUPPORTED);
    return 0;
}

int __guac_handle_pipe(guac_user* user, int argc, char** argv) {

    /* Pull corresponding stream */
    int stream_index = atoi(argv[0]);
    guac_stream* stream = __init_input_stream(user, stream_index);
    if (stream == NULL)
        return 0;

    /* If supported, call handler */
    if (user->pipe_handler)
        return user->pipe_handler(
            user,
            stream,
            argv[1], /* mimetype */
            argv[2]  /* name */
        );

    /* Otherwise, abort */
    guac_protocol_send_ack(user->socket, stream,
            "Named pipes unsupported", GUAC_PROTOCOL_STATUS_UNSUPPORTED);
    return 0;
}

int __guac_handle_argv(guac_user* user, int argc, char** argv) {

    /* Pull corresponding stream */
    int stream_index = atoi(argv[0]);
    guac_stream* stream = __init_input_stream(user, stream_index);
    if (stream == NULL)
        return 0;

    /* If supported, call handler */
    if (user->argv_handler)
        return user->argv_handler(
            user,
            stream,
            argv[1], /* mimetype */
            argv[2]  /* name */
        );

    /* Otherwise, abort */
    guac_protocol_send_ack(user->socket, stream,
            "Reconfiguring in-progress connections unsupported",
            GUAC_PROTOCOL_STATUS_UNSUPPORTED);
    return 0;
}

int __guac_handle_ack(guac_user* user, int argc, char** argv) {

    guac_stream* stream;

    /* Parse stream index */
    int stream_index = atoi(argv[0]);

    /* Ignore indices of client-level streams */
    if (stream_index % 2 != 0)
        return 0;

    /* Determine index within user-level array of streams */
    stream_index /= 2;

    /* Validate stream index */
    if (stream_index < 0 || stream_index >= GUAC_USER_MAX_STREAMS)
        return 0;

    stream = &(user->__output_streams[stream_index]);

    /* Validate initialization of stream */
    if (stream->index == GUAC_USER_CLOSED_STREAM_INDEX)
        return 0;

    /* Call stream handler if defined */
    if (stream->ack_handler)
        return stream->ack_handler(user, stream, argv[1],
                atoi(argv[2]));

    /* Fall back to global handler if defined */
    if (user->ack_handler)
        return user->ack_handler(user, stream, argv[1],
                atoi(argv[2]));

    return 0;
}

int __guac_handle_blob(guac_user* user, int argc, char** argv) {

    int stream_index = atoi(argv[0]);
    guac_stream* stream = __get_open_input_stream(user, stream_index);

    /* Fail if no such stream */
    if (stream == NULL)
        return 0;

    /* Call stream handler if defined */
    if (stream->blob_handler) {
        int length = guac_protocol_decode_base64(argv[1]);
        return stream->blob_handler(user, stream, argv[1],
            length);
    }

    /* Fall back to global handler if defined */
    if (user->blob_handler) {
        int length = guac_protocol_decode_base64(argv[1]);
        return user->blob_handler(user, stream, argv[1],
            length);
    }

    guac_protocol_send_ack(user->socket, stream,
            "File transfer unsupported", GUAC_PROTOCOL_STATUS_UNSUPPORTED);
    return 0;
}

int __guac_handle_end(guac_user* user, int argc, char** argv) {

    int result = 0;
    int stream_index = atoi(argv[0]);
    guac_stream* stream = __get_open_input_stream(user, stream_index);

    /* Fail if no such stream */
    if (stream == NULL)
        return 0;

    /* Call stream handler if defined */
    if (stream->end_handler)
        result = stream->end_handler(user, stream);

    /* Fall back to global handler if defined */
    else if (user->end_handler)
        result = user->end_handler(user, stream);

    /* Mark stream as closed */
    stream->index = GUAC_USER_CLOSED_STREAM_INDEX;
    return result;
}

int __guac_handle_get(guac_user* user, int argc, char** argv) {

    guac_object* object;

    /* Validate object index */
    int object_index = atoi(argv[0]);
    if (object_index < 0 || object_index >= GUAC_USER_MAX_OBJECTS)
        return 0;

    object = &(user->__objects[object_index]);

    /* Validate initialization of object */
    if (object->index == GUAC_USER_UNDEFINED_OBJECT_INDEX)
        return 0;

    /* Call object handler if defined */
    if (object->get_handler)
        return object->get_handler(
            user,
            object,
            argv[1] /* name */
        );

    /* Fall back to global handler if defined */
    if (user->get_handler)
        return user->get_handler(
            user,
            object,
            argv[1] /* name */
        );

    return 0;
}

int __guac_handle_put(guac_user* user, int argc, char** argv) {

    guac_object* object;

    /* Validate object index */
    int object_index = atoi(argv[0]);
    if (object_index < 0 || object_index >= GUAC_USER_MAX_OBJECTS)
        return 0;

    object = &(user->__objects[object_index]);

    /* Validate initialization of object */
    if (object->index == GUAC_USER_UNDEFINED_OBJECT_INDEX)
        return 0;

    /* Pull corresponding stream */
    int stream_index = atoi(argv[1]);
    guac_stream* stream = __init_input_stream(user, stream_index);
    if (stream == NULL)
        return 0;

    /* Call object handler if defined */
    if (object->put_handler)
        return object->put_handler(
            user,
            object, 
            stream,
            argv[2], /* mimetype */
            argv[3]  /* name */
        );

    /* Fall back to global handler if defined */
    if (user->put_handler)
        return user->put_handler(
            user,
            object,
            stream,
            argv[2], /* mimetype */
            argv[3]  /* name */
        );

    /* Otherwise, abort */
    guac_protocol_send_ack(user->socket, stream,
            "Object write unsupported", GUAC_PROTOCOL_STATUS_UNSUPPORTED);
    return 0;
}

int __guac_handle_nop(guac_user* user, int argc, char** argv) {
    guac_user_log(user, GUAC_LOG_TRACE,
            "Received nop instruction");
    return 0;
}

int __guac_handle_disconnect(guac_user* user, int argc, char** argv) {
    guac_user_stop(user);
    return 0;
}

/* Guacamole handshake handler functions. */

int __guac_handshake_size_handler(guac_user* user, int argc, char** argv) {
    
    /* Validate size of instruction. */
    if (argc < 2) {
        guac_user_log(user, GUAC_LOG_ERROR, "Received \"size\" "
                "instruction lacked required arguments.");
        return 1;
    }
    
    /* Parse optimal screen dimensions from size instruction */
    user->info.optimal_width  = atoi(argv[0]);
    user->info.optimal_height = atoi(argv[1]);

    /* If DPI given, set the user resolution */
    if (argc >= 3)
        user->info.optimal_resolution = atoi(argv[2]);

    /* Otherwise, use a safe default for rough backwards compatibility */
    else
        user->info.optimal_resolution = 96;
    
    return 0;
    
}

int __guac_handshake_audio_handler(guac_user* user, int argc, char** argv) {

    guac_free_mimetypes((char **) user->info.audio_mimetypes);
    
    /* Store audio mimetypes */
    user->info.audio_mimetypes = (const char**) guac_copy_mimetypes(argv, argc);
    
    return 0;
    
}

int __guac_handshake_video_handler(guac_user* user, int argc, char** argv) {

    guac_free_mimetypes((char **) user->info.video_mimetypes);
    
    /* Store video mimetypes */
    user->info.video_mimetypes = (const char**) guac_copy_mimetypes(argv, argc);
    
    return 0;
    
}

int __guac_handshake_image_handler(guac_user* user, int argc, char** argv) {
    
    guac_free_mimetypes((char **) user->info.image_mimetypes);
    
    /* Store image mimetypes */
    user->info.image_mimetypes = (const char**) guac_copy_mimetypes(argv, argc);
    
    return 0;
    
}

int __guac_handshake_name_handler(guac_user* user, int argc, char** argv) {

    /* Free any past value for the user's name */
    guac_mem_free_const(user->info.name);

    /* If a value is provided for the name, copy it into guac_user. */
    if (argc > 0 && strcmp(argv[0], ""))
        user->info.name = (const char*) guac_strdup(argv[0]);

    /* No or empty value was provided, so make sure this is NULLed out. */
    else
        user->info.name = NULL;

    return 0;

}

int __guac_handshake_timezone_handler(guac_user* user, int argc, char** argv) {
    
    /* Free any past value */
    guac_mem_free_const(user->info.timezone);
    
    /* Store timezone, if present */
    if (argc > 0 && strcmp(argv[0], ""))
        user->info.timezone = (const char*) guac_strdup(argv[0]);
    
    else
        user->info.timezone = NULL;
    
    return 0;
    
}

char** guac_copy_mimetypes(char** mimetypes, int count) {

    int i;

    /* Allocate sufficient space for NULL-terminated array of mimetypes */
    char** mimetypes_copy = guac_mem_alloc(sizeof(char*),
            guac_mem_ckd_add_or_die(count, 1));

    /* Copy each provided mimetype */
    for (i = 0; i < count; i++)
        mimetypes_copy[i] = guac_strdup(mimetypes[i]);

    /* Terminate with NULL */
    mimetypes_copy[count] = NULL;

    return mimetypes_copy;

}

void guac_free_mimetypes(char** mimetypes) {

    if (mimetypes == NULL)
        return;
    
    char** current_mimetype = mimetypes;

    /* Free all strings within NULL-terminated mimetype array */
    while (*current_mimetype != NULL) {
        guac_mem_free(*current_mimetype);
        current_mimetype++;
    }

    /* Free the array itself, now that its contents have been freed */
    guac_mem_free(mimetypes);

}

int __guac_user_call_opcode_handler(__guac_instruction_handler_mapping* map,
        guac_user* user, const char* opcode, int argc, char** argv) {

    /* For each defined instruction */
    __guac_instruction_handler_mapping* current = map;
    while (current->opcode != NULL) {

        /* If recognized, call handler */
        if (strcmp(opcode, current->opcode) == 0)
            return current->handler(user, argc, argv);

        current++;
    }

    /* If unrecognized, log and ignore */
    guac_user_log(user, GUAC_LOG_DEBUG, "Handler not found for \"%s\"",
            opcode);
    return 0;

}

