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

#ifndef GUAC_VNC_VNC_H
#define GUAC_VNC_VNC_H

#include "common/clipboard.h"
#include "common/iconv.h"
#include "display.h"
#include "input.h"
#include "settings.h"

#include <guacamole/client.h>
#include <guacamole/display.h>
#include <guacamole/fifo.h>
#include <guacamole/layer.h>
#include <rfb/rfbclient.h>

#ifdef ENABLE_PULSE
#include "pulse/pulse.h"
#endif

#ifdef ENABLE_COMMON_SSH
#include "common-ssh/sftp.h"
#include "common-ssh/ssh.h"
#include "common-ssh/user.h"
#endif

#include <guacamole/recording.h>

#include <pthread.h>

/**
 * The ID of the RFB client screen. If multi-screen support is added, more than
 * one ID will be needed as well.
 */
#define GUAC_VNC_SCREEN_ID 1

/**
 * Capacity of the per-VNC-client input event queue (see guac_vnc_client's
 * input_events field). Sized generously so that multiple simultaneous users
 * each generating high-rate mouse events over several tens of milliseconds
 * cannot overflow the queue and block their input threads. At ~60 events
 * per user per second, 1024 slots accommodates several users' worth of
 * input over a full second - far more than the VNC main thread should
 * ever need to drain at once.
 */
#define GUAC_VNC_INPUT_EVENT_QUEUE_SIZE 1024

/**
 * VNC-specific client data.
 */
typedef struct guac_vnc_client {

    /**
     * The VNC client thread.
     */
    pthread_t client_thread;

#ifdef ENABLE_VNC_TLS_LOCKING
    /**
     * The TLS mutex lock for the client.
     */
    pthread_mutex_t tls_lock;
#endif

    /**
     * Lock which synchronizes messages sent to VNC server. Held by any
     * path that writes to the VNC server socket - the writer thread
     * draining input_events (see below), and the infrequent resize /
     * server-input-toggle paths on the main thread. Libvncclient's
     * HandleRFBServerMessage itself writes nothing to the socket in
     * this build because the rfbFramebufferUpdateRequest support bit
     * is cleared at connection setup (see guac_vnc_get_client), so the
     * main thread's decode can run concurrently with input dispatch on
     * the writer thread without either blocking the other.
     */
    pthread_mutex_t message_lock;

    /**
     * FIFO of pending outbound messages consumed by a dedicated writer
     * thread (see input_drain_thread below). Despite the historical
     * "input" naming, this FIFO carries every server-bound message
     * whose ordering matters for input responsiveness: user-driven
     * mouse/key events produced by user input threads and FUR markers
     * produced by the reader thread's FinishedFrameBufferUpdate
     * callback and by the MallocFrameBuffer callback on server-
     * initiated resize. Routing both classes of message through the
     * same FIFO gives strict enqueue-order delivery - any mouse/key
     * event that was queued before a FUR will land on the server
     * ahead of that FUR, so the server's framebuffer response
     * reflects the input rather than pre-input state. Decoding on the
     * reader thread does not hold any lock, so a slow VNC server or
     * slow decode cannot stall user-input dispatch.
     */
    guac_fifo input_events;

    /**
     * Backing storage for the input_events FIFO.
     */
    guac_vnc_input_event input_events_items[GUAC_VNC_INPUT_EVENT_QUEUE_SIZE];

    /**
     * Writer thread that blocks on the input_events FIFO's internal
     * signalling (via guac_fifo_dequeue_and_lock) and dispatches each
     * dequeued outbound message to the VNC server under message_lock -
     * pointer/key events via libvncclient's SendPointerEvent /
     * SendKeyEvent, FUR markers via the internal guac_vnc_send_fur
     * helper (which bypasses the disabled supportedMessages check).
     * Started during client init; exits when the FIFO is invalidated
     * during client cleanup.
     */
    pthread_t input_drain_thread;

    /**
     * Whether input_drain_thread has been successfully started. The
     * cleanup path joins the thread only if this is set, so that a
     * failure to start the thread during init does not cause a hang at
     * shutdown waiting for a nonexistent thread.
     */
    int input_drain_thread_running;

    /**
     * The underlying VNC client.
     */
    rfbClient* rfb_client;

    /**
     * The original framebuffer malloc procedure provided by the initialized
     * rfbClient.
     */
    MallocFrameBufferProc rfb_MallocFrameBuffer;

    /**
     * The original CopyRect processing procedure provided by the initialized
     * rfbClient.
     */
    GotCopyRectProc rfb_GotCopyRect;

    /**
     * Whether copyrect  was used to produce the latest update received
     * by the VNC server.
     */
    int copy_rect_used;

    /**
     * Client settings, parsed from args.
     */
    guac_vnc_settings* settings;

    /**
     * The current display state.
     */
    guac_display* display;

    /**
     * The context of the current drawing (update) operation, if any. If no
     * operation is in progress, this will be NULL.
     */
    guac_display_layer_raw_context* current_context;

    /**
     * The current instance of the guac_display render thread. If the thread
     * has not yet been started, this will be NULL.
     */
    guac_display_render_thread* render_thread;

    /**
     * Internal clipboard.
     */
    guac_common_clipboard* clipboard;

#ifdef ENABLE_PULSE
    /**
     * PulseAudio output, if any.
     */
    guac_pa_stream* audio;
#endif

#ifdef ENABLE_COMMON_SSH
    /**
     * The user and credentials used to authenticate for SFTP.
     */
    guac_common_ssh_user* sftp_user;

    /**
     * The SSH session used for SFTP.
     */
    guac_common_ssh_session* sftp_session;

    /**
     * An SFTP-based filesystem.
     */
    guac_common_ssh_sftp_filesystem* sftp_filesystem;
#endif

    /**
     * The in-progress session recording, or NULL if no recording is in
     * progress.
     */
    guac_recording* recording;

    /**
     * Clipboard encoding-specific reader.
     */
    guac_iconv_read* clipboard_reader;

    /**
     * Clipboard encoding-specific writer.
     */
    guac_iconv_write* clipboard_writer;

#ifdef LIBVNC_HAS_RESIZE_SUPPORT
    /**
     * Whether or not the server has sent the required message to initialize
     * the screen data in the client.
     */
    bool rfb_screen_initialized;

    /**
     * Whether or not the client has sent it's starting size to the server.
     */
    bool rfb_initial_resize;
#endif

} guac_vnc_client;

/**
 * Allocates a new rfbClient instance given the parameters stored within the
 * client, returning NULL on failure.
 *
 * @param client
 *     The guac_client associated with the settings of the desired VNC
 *     connection.
 *
 * @return
 *     A new rfbClient instance allocated and connected according to the
 *     parameters stored within the given client, or NULL if connecting to the
 *     VNC server fails.
 */
rfbClient* guac_vnc_get_client(guac_client* client);

/**
 * VNC client thread. This thread initiates the VNC connection and ultimately
 * runs throughout the duration of the client, existing as a single instance,
 * shared by all users.
 *
 * @param data
 *     The guac_client instance associated with the requested VNC connection.
 *
 * @return
 *     Always NULL.
 */
void* guac_vnc_client_thread(void* data);

/**
 * Key which can be used with the rfbClientGetClientData function to return
 * the associated guac_client.
 */
extern char* GUAC_VNC_CLIENT_KEY;

#endif

