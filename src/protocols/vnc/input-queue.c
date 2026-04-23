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

#include "input.h"
#include "vnc.h"

#include <guacamole/fifo.h>
#include <rfb/rfbclient.h>

#include <pthread.h>
#include <string.h>

void guac_vnc_input_event_enqueue(guac_vnc_client* vnc_client,
        const guac_vnc_input_event* input_event) {

    /* The fifo's internal guac_flag signals any thread blocked in
     * guac_fifo_dequeue_and_lock(), so the drain thread wakes up as
     * soon as this enqueue completes. No additional signalling
     * mechanism is required. */
    guac_fifo_enqueue(&vnc_client->input_events, input_event);

}

/**
 * Writes a FramebufferUpdateRequest message directly to the VNC server,
 * bypassing libvncclient's SendFramebufferUpdateRequest. The built-in
 * wrapper short-circuits when rfbFramebufferUpdateRequest is cleared in
 * supportedMessages.client2server, which is exactly what we do at
 * connection setup to suppress libvncclient's internal auto-FUR at the
 * tail of HandleRFBServerMessage. The writer thread still needs to
 * actually emit FURs, so this helper serializes the 10-byte request
 * message onto the socket without consulting that flag.
 *
 * @param rfb_client
 *     The libvncclient client whose server-side socket should receive
 *     the request.
 *
 * @param x, y, w, h
 *     The upper-left corner and dimensions of the rectangle whose
 *     updated contents are being requested, in pixels.
 *
 * @param incremental
 *     Non-zero to request only pixels that have changed since the last
 *     FramebufferUpdateRequest the server received, zero to request the
 *     full contents of the rectangle regardless of what the server has
 *     previously sent.
 */
static rfbBool guac_vnc_send_fur(rfbClient* client,
        int x, int y, int w, int h, rfbBool incremental) {

    /* The parameter must be named "client" because rfbClientSwap16IfLE
     * is a macro that dereferences a variable named "client" from the
     * enclosing scope (for its endian-test byte). */
    rfbFramebufferUpdateRequestMsg fur;
    fur.type = rfbFramebufferUpdateRequest;
    fur.incremental = incremental ? 1 : 0;
    fur.x = rfbClientSwap16IfLE(x);
    fur.y = rfbClientSwap16IfLE(y);
    fur.w = rfbClientSwap16IfLE(w);
    fur.h = rfbClientSwap16IfLE(h);
    return WriteToRFBServer(client, (char*) &fur,
            sz_rfbFramebufferUpdateRequestMsg);

}

/**
 * Forwards a single event to the VNC server, acquiring message_lock to
 * ensure the underlying socket write does not interleave with other
 * VNC-server-bound traffic (resize requests, clipboard sends, etc.).
 */
static void guac_vnc_dispatch_input_event(guac_vnc_client* vnc_client,
        const guac_vnc_input_event* input_event) {

    rfbClient* rfb_client = vnc_client->rfb_client;

    /* Silently drop events that arrive before the rfbClient exists or
     * after it has been destroyed. In the steady state this branch is
     * never taken; it matters only during the brief windows at the
     * beginning and end of a VNC connection. */
    if (rfb_client == NULL)
        return;

    pthread_mutex_lock(&(vnc_client->message_lock));

    switch (input_event->type) {

        case GUAC_VNC_INPUT_EVENT_MOUSE:
            SendPointerEvent(rfb_client,
                    input_event->details.mouse.x,
                    input_event->details.mouse.y,
                    input_event->details.mouse.mask);
            break;

        case GUAC_VNC_INPUT_EVENT_KEY:
            SendKeyEvent(rfb_client,
                    input_event->details.key.keysym,
                    input_event->details.key.pressed);
            break;

        case GUAC_VNC_INPUT_EVENT_FUR_INCREMENTAL:
            /* Request deltas over the full current framebuffer. Using
             * the tracked rfb_client->width / rfb_client->height here
             * means no additional state needs to travel through the
             * FIFO for the common case. */
            guac_vnc_send_fur(rfb_client, 0, 0,
                    rfb_client->width, rfb_client->height, TRUE);
            break;

        case GUAC_VNC_INPUT_EVENT_FUR_FULL:
            guac_vnc_send_fur(rfb_client,
                    input_event->details.fur.x,
                    input_event->details.fur.y,
                    input_event->details.fur.w,
                    input_event->details.fur.h, FALSE);
            break;

    }

    pthread_mutex_unlock(&(vnc_client->message_lock));

}

void* guac_vnc_input_drain_thread(void* data) {

    guac_vnc_client* vnc_client = (guac_vnc_client*) data;

    /* Loop indefinitely, blocking on the fifo's own signalling for new
     * events. guac_fifo_dequeue_and_lock returns zero only once the
     * fifo has been invalidated (see guac_fifo_invalidate in the
     * client free handler), which is how we cleanly unblock and exit
     * at shutdown. */
    guac_vnc_input_event input_event;
    while (guac_fifo_dequeue_and_lock(&vnc_client->input_events, &input_event)) {

        /* Release the fifo lock immediately - dispatching must not hold
         * it, because blocking on message_lock here would stall
         * producers (user input threads trying to enqueue new events). */
        guac_fifo_unlock(&vnc_client->input_events);

        guac_vnc_dispatch_input_event(vnc_client, &input_event);

    }

    return NULL;

}
