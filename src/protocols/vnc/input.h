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

#ifndef GUAC_VNC_INPUT_H
#define GUAC_VNC_INPUT_H

#include <guacamole/user.h>

/* Forward declaration to avoid including vnc.h, which pulls in
 * rfbclient.h and other heavy dependencies. The full definition lives
 * in vnc.h and is visible to the few source files that need it. */
struct guac_vnc_client;

/**
 * All event types supported by the guac_vnc_input_event structure.
 *
 * Despite the "input" naming, this enum covers every message the writer
 * thread sends to the VNC server: user-driven pointer/key events, plus
 * protocol-level messages (FramebufferUpdateRequest) that were
 * previously sent inline by libvncclient at the tail of
 * HandleRFBServerMessage. Routing the FUR through the same FIFO the
 * input events travel on is what gives strict ordering - any pointer
 * or key event enqueued before a FUR is guaranteed to reach the VNC
 * server before that FUR, so the server's response frame reflects the
 * input rather than requiring an additional round trip.
 */
typedef enum guac_vnc_input_event_type {

    /**
     * A mouse event: motion or press/release of a mouse button.
     */
    GUAC_VNC_INPUT_EVENT_MOUSE,

    /**
     * A key event: press/release of a keyboard key.
     */
    GUAC_VNC_INPUT_EVENT_KEY,

    /**
     * A request for the VNC server to send any pixels that have changed
     * since the client's previous FramebufferUpdateRequest. Enqueued by
     * the reader thread after each successful HandleRFBServerMessage.
     * Libvncclient's built-in auto-FUR is suppressed at connection
     * setup (see guac_vnc_client_thread); this event is what drives
     * subsequent frame delivery instead.
     */
    GUAC_VNC_INPUT_EVENT_FUR_INCREMENTAL,

    /**
     * A non-incremental FramebufferUpdateRequest over a specific
     * rectangle (see details.fur). Used when the client-initiated or
     * server-initiated display size changes: the newly-resized region
     * must be fully re-sent, not just its deltas from the old geometry.
     */
    GUAC_VNC_INPUT_EVENT_FUR_FULL

} guac_vnc_input_event_type;

/**
 * Event details specific to GUAC_VNC_INPUT_EVENT_MOUSE events.
 */
typedef struct guac_vnc_input_event_mouse_details {

    /**
     * The X coordinate of the mouse pointer, in pixels. Not guaranteed to
     * lie within the bounds of the display area.
     */
    int x;

    /**
     * The Y coordinate of the mouse pointer, in pixels. Not guaranteed to
     * lie within the bounds of the display area.
     */
    int y;

    /**
     * Bitmask representing the current state of each mouse button.
     *
     * @see GUAC_CLIENT_MOUSE_LEFT
     * @see GUAC_CLIENT_MOUSE_MIDDLE
     * @see GUAC_CLIENT_MOUSE_RIGHT
     * @see GUAC_CLIENT_MOUSE_SCROLL_UP
     * @see GUAC_CLIENT_MOUSE_SCROLL_DOWN
     */
    int mask;

} guac_vnc_input_event_mouse_details;

/**
 * Event details specific to GUAC_VNC_INPUT_EVENT_KEY events.
 */
typedef struct guac_vnc_input_event_key_details {

    /**
     * The X11 keysym of the key that was pressed or released.
     */
    int keysym;

    /**
     * Non-zero if the key was pressed, zero if the key was released.
     */
    int pressed;

} guac_vnc_input_event_key_details;

/**
 * Event details specific to GUAC_VNC_INPUT_EVENT_FUR_FULL events.
 * GUAC_VNC_INPUT_EVENT_FUR_INCREMENTAL events carry no details - they
 * always request deltas over the full current framebuffer.
 */
typedef struct guac_vnc_input_event_fur_details {

    /**
     * Upper-left X coordinate of the rectangle being requested, in pixels.
     */
    int x;

    /**
     * Upper-left Y coordinate of the rectangle being requested, in pixels.
     */
    int y;

    /**
     * Width of the rectangle being requested, in pixels.
     */
    int w;

    /**
     * Height of the rectangle being requested, in pixels.
     */
    int h;

} guac_vnc_input_event_fur_details;

/**
 * Generic VNC input event, tagged with its event type. Which member of the
 * details union is valid is dictated by the type field.
 */
typedef struct guac_vnc_input_event {

    /**
     * The type of this event. Dictates which union member is valid.
     */
    guac_vnc_input_event_type type;

    /**
     * The user that originated this event. NOTE: this pointer is not
     * guaranteed to still be valid when the event is drained; it must be
     * validated before dereferencing.
     */
    guac_user* user;

    /**
     * Event details, type-specific.
     */
    union {
        guac_vnc_input_event_mouse_details mouse;
        guac_vnc_input_event_key_details key;
        guac_vnc_input_event_fur_details fur;
    } details;

} guac_vnc_input_event;

/**
 * Adds an input event to the VNC client's input-event queue, signalling the
 * main VNC thread to wake (if currently blocked on the VNC socket) so the
 * event can be forwarded to the VNC server. Returns as soon as the event
 * has been enqueued; does not wait for the event to be processed.
 *
 * This enqueue operation performs no I/O to the VNC server and takes no
 * display-related lock. Completes in microseconds, keeping the Guacamole-
 * client-side user input thread free to handle subsequent instructions
 * without waiting for the VNC server's acknowledgement of prior events -
 * which is the primary purpose of the queue.
 *
 * @param vnc_client
 *     The VNC client instance whose queue should receive the event.
 *
 * @param input_event
 *     The event to enqueue. Its contents are copied; the caller may
 *     discard the source struct after this function returns.
 */
void guac_vnc_input_event_enqueue(struct guac_vnc_client* vnc_client,
        const guac_vnc_input_event* input_event);

/**
 * Thread entry point for the VNC input-drain thread. Blocks on the
 * per-connection input_events FIFO (via guac_fifo_dequeue_and_lock)
 * and forwards each dequeued event to the VNC server under
 * message_lock. Exits cleanly once the FIFO is invalidated during
 * client cleanup.
 *
 * Only one such thread exists per VNC connection, so libvncclient's
 * un-serialized socket writes are safe: all input-driven sends happen
 * on this one thread, and coordination with other server-side writers
 * (resize, clipboard, ...) is handled via message_lock.
 *
 * @param data
 *     Pointer to the guac_vnc_client whose input_events FIFO this
 *     thread should drain.
 *
 * @return
 *     Always NULL.
 */
void* guac_vnc_input_drain_thread(void* data);

/**
 * Handler for Guacamole user mouse events.
 */
guac_user_mouse_handler guac_vnc_user_mouse_handler;

/**
 * Handler for Guacamole user key events.
 */
guac_user_key_handler guac_vnc_user_key_handler;

/**
 * Handler for Guacamole user resize events.
 */
guac_user_size_handler guac_vnc_user_size_handler;

#endif // GUAC_VNC_INPUT_H
