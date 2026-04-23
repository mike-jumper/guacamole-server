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

#include "display.h"
#include "input.h"
#include "vnc.h"

#include <guacamole/client.h>
#include <guacamole/display.h>
#include <guacamole/recording.h>
#include <guacamole/user.h>
#include <rfb/rfbclient.h>

int guac_vnc_user_mouse_handler(guac_user* user, int x, int y, int mask) {

    guac_client* client = user->client;
    guac_vnc_client* vnc_client = (guac_vnc_client*) client->data;

    /* Store current mouse location/state. Non-blocking - only flips a flag
     * in the render thread's state so the next emitted frame includes the
     * latest cursor position. */
    guac_display_render_thread_notify_user_moved_mouse(vnc_client->render_thread, user, x, y, mask);

    /* Report mouse position within recording, if active. Writes to the
     * recording file's buffered socket; does not block on the VNC server. */
    if (vnc_client->recording != NULL)
        guac_recording_report_mouse(vnc_client->recording, x, y, mask);

    /* Queue the event for the drain thread to forward to the VNC
     * server. The user input thread does not wait for the VNC server
     * to acknowledge receipt, so a slow VNC server or slow upstream
     * network cannot stall Guacamole-side input processing. */
    guac_vnc_input_event mouse_event = {
        .type = GUAC_VNC_INPUT_EVENT_MOUSE,
        .user = user,
        .details.mouse = {
            .x = x,
            .y = y,
            .mask = mask
        }
    };
    guac_vnc_input_event_enqueue(vnc_client, &mouse_event);

    return 0;
}

int guac_vnc_user_key_handler(guac_user* user, int keysym, int pressed) {

    guac_vnc_client* vnc_client = (guac_vnc_client*) user->client->data;

    /* Report key state within recording, if active. */
    if (vnc_client->recording != NULL)
        guac_recording_report_key(vnc_client->recording, keysym, pressed);

    /* Queue the event for the drain thread to forward to the VNC
     * server. See guac_vnc_user_mouse_handler for rationale. */
    guac_vnc_input_event key_event = {
        .type = GUAC_VNC_INPUT_EVENT_KEY,
        .user = user,
        .details.key = {
            .keysym = keysym,
            .pressed = pressed
        }
    };
    guac_vnc_input_event_enqueue(vnc_client, &key_event);

    return 0;
}

#ifdef LIBVNC_HAS_RESIZE_SUPPORT
int guac_vnc_user_size_handler(guac_user* user, int width, int height) {

    guac_user_log(user, GUAC_LOG_TRACE, "Running user size handler.");

    /* Get the Guacamole VNC client */
    guac_vnc_client* vnc_client = (guac_vnc_client*) user->client->data;

    /* Send display update */
    guac_vnc_display_set_size(vnc_client->rfb_client, width, height);

    return 0;

}
#endif // LIBVNC_HAS_RESIZE_SUPPORT
