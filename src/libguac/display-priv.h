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

#ifndef GUAC_DISPLAY_PRIV_H
#define GUAC_DISPLAY_PRIV_H

#include "display-pipeline.h"
#include "display-plan.h"
#include "guacamole/client.h"
#include "guacamole/display.h"
#include "guacamole/fifo.h"
#include "guacamole/rect.h"
#include "guacamole/socket.h"

#include <pthread.h>
#include <stdatomic.h>

/**
 * The maximum amount of time to wait after flushing a frame when compensating
 * for client-side processing delays, in milliseconds. If a connected client is
 * taking longer than this amount of additional time to process a received
 * frame, processing lag compensation will be only partial (to avoid delaying
 * further processing without bound for extremely slow clients).
 */
#define GUAC_DISPLAY_MAX_LAG_COMPENSATION 500

/*
 * IMPORTANT: All functions defined within the internals of guac_display that
 * DO NOT acquire locks on their own are given prefixes based on whether they
 * access or modify the pending frame, last frame, or both. It is the
 * responsibility of the caller of such functions to ensure that the required
 * locks are either held or not relevant.
 *
 * The prefixes that may be added to function names are:
 *
 *   "PFR_"
 *     The function reads (but does not write) the state of the pending frame.
 *     This prefix and "PFW_" are mutually-exclusive.
 *
 *   "PFW_"
 *     The function writes (and possibly reads) the state of the pending frame.
 *     This prefix and "PFW_" are mutually-exclusive.
 *
 *   "LFR_"
 *     The function reads (but does not write) the state of the last frame.
 *     This prefix and "LFW_" are mutually-exclusive.
 *
 *   "LFW_"
 *     The function writes (and possibly reads) the state of the last frame.
 *     This prefix and "LFR_" are mutually-exclusive.
 *
 *   "XFR_"
 *     The function reads (but does not write) the state of a frame, and
 *     whether that frame is the pending frame or the last frame depends on
 *     which frame is provided via function parameters. This prefix and "XFW_"
 *     are mutually-exclusive.
 *
 *   "XFW_"
 *     The function writes (but does not read) the state of a frame, and
 *     whether that frame is the pending frame or the last frame depends on
 *     which frame is provided via function parameters. This prefix and "XFR_"
 *     are mutually-exclusive.
 *
 * Any functions lacking these prefixes either do not access last/pending
 * frames in any way or take care of acquiring/releasing locks entirely on
 * their own.
 *
 * These conventions are used for all functions in the internals of
 * guac_display, not just those defined in this header.
 */

/*
 * IMPORTANT: In cases where a single thread must acquire multiple locks used
 * by guac_display, proper acquisition order must be observed to avoid
 * deadlock. The correct order is:
 *
 * 1) pending_state (flag; hold via guac_flag_wait_and_lock or guac_flag_lock)
 * 2) last_frame_lock
 * 3) encode_stage.fifo / apply_stage.fifo / plan_stage.fifo
 * 4) render_state / flush_state
 *
 * Acquiring these locks in any other order risks deadlock. Don't do it.
 */

/**
 * The size of the image tiles (cells) that will be used to track changes to
 * each layer, including gathering framerate statistics and performing indexing
 * based on contents. Each side of each cell will consist of this many pixels.
 *
 * IMPORTANT: The hashing algorithm used to search the previous frame for
 * content in the pending frame that has been reused (ie: scrolling) strongly
 * depends on this value being 64. Any adjustment to this value will require
 * corresponding and careful changes to the hashing algorithm.
 */
#define GUAC_DISPLAY_CELL_SIZE 64

/**
 * The exponent of the power-of-two value that dictates the size of the image
 * tiles (cells) that will be used to track changes to each layer
 * (GUAC_DISPLAY_CELL_SIZE).
 */
#define GUAC_DISPLAY_CELL_SIZE_EXPONENT 6

/**
 * The amount that the width/height of internal storage for graphical data
 * should be rounded up to avoid unnecessary reallocations and copying.
 */
#define GUAC_DISPLAY_RESIZE_FACTOR 64

/**
 * Given the width (or height) of a layer in pixels, calculates the width (or
 * height) of that layer's pending_frame_cells array in cells.
 *
 * NOTE: It is not necessary to recalculate these values except when resizing a
 * layer. In all other cases, the width/height of a layer in cells can be found
 * in the pending_frame_cells_width and pending_frame_cells_height members
 * respectively.
 *
 * @param pixels
 *     The width or height of the layer, in pixels.
 *
 * @return
 *     The width or height of that layer's pending_frame_cells array, in cells.
 */
#define GUAC_DISPLAY_CELL_DIMENSION(pixels) \
    ((pixels + GUAC_DISPLAY_CELL_SIZE - 1) / GUAC_DISPLAY_CELL_SIZE)

/**
 * The size of the operation FIFO read by the display worker threads. This
 * value is the number of operation slots in the FIFO, not bytes. The amount of
 * space currently specified here is roughly sufficient 8 worst-case frames
 * worth of outstanding operations.
 */
#define GUAC_DISPLAY_WORKER_FIFO_SIZE (                                       \
            GUAC_DISPLAY_MAX_WIDTH * GUAC_DISPLAY_MAX_HEIGHT                  \
                / GUAC_DISPLAY_CELL_SIZE                                      \
                / GUAC_DISPLAY_CELL_SIZE                                      \
                * 8)

/**
 * The size of the plan-stage FIFO. In steady state the
 * GUAC_DISPLAY_FLUSH_IN_PROGRESS flag ensures this FIFO contains at most
 * one pending frame at a time; the extra slots absorb brief scheduling
 * hiccups without ever blocking the render thread's enqueue.
 */
#define GUAC_DISPLAY_PLAN_FIFO_SIZE 4

/**
 * The size of the parallel-stage FIFO read by the worker pool that
 * services guac_display_parallel_for() chunk dispatches. A single
 * parallel_for call enqueues at most worker_thread_count chunks; this
 * size is comfortably above the largest plausible worker count, so an
 * enqueue never blocks waiting for a slot.
 */
#define GUAC_DISPLAY_PARALLEL_FIFO_SIZE 128

/**
 * The size of the build-stage FIFO. The draft stage fans out up to
 * (worker_thread_count chunks per layer) for each layer that has any
 * dirty cells. FLUSH_IN_PROGRESS serializes to one frame at a time,
 * so this size only has to comfortably accommodate the fan-out of a
 * single frame. 128 slots covers up to ~16 layers at a typical
 * worker count of 8.
 */
#define GUAC_DISPLAY_BUILD_FIFO_SIZE 128

/**
 * The size of the search-stage FIFO. The last rects chunk to finish
 * fans out up to layer_count * strips_per_layer chunks (one per
 * (layer, sub_rect) strip of the copy-search). The cap here is a
 * pragmatic upper bound - more layers than this on one frame is not
 * expected in practice, and FLUSH_IN_PROGRESS gates one frame at a
 * time so chunks never overlap across frames.
 */
#define GUAC_DISPLAY_SEARCH_FIFO_SIZE 256

/**
 * Returns the memory address of the given rectangle within the mutable image
 * buffer of the given guac_display_layer_state, where the upper-left corner of
 * the given buffer is (0, 0). If the memory address cannot be calculated
 * because doing so would overflow the maximum value of a size_t, execution of
 * the current process is automatically aborted.
 *
 * IMPORTANT: No checks are performed on whether the rectangle extends beyond
 * the bounds of the buffer, including considering whether the left/top
 * position of the rectangle is negative. If the rectangle has not already been
 * contrained to be within the bounds of the buffer, such checks must be
 * performed before dereferencing the value returned by this macro.
 *
 * @param layer_state
 *     The guac_display_layer_state associated with the image buffer within
 *     which the address of the given rectangle should be determined.
 *
 * @param rect
 *     The rectangle to determine the offset of.
 *
 * @return
 *     The memory address of the given rectangle within the buffer of the given
 *     layer state.
 */
#define GUAC_DISPLAY_LAYER_STATE_MUTABLE_BUFFER(layer_state, rect) \
    GUAC_RECT_MUTABLE_BUFFER(rect, (layer_state).buffer, (layer_state).buffer_stride, GUAC_DISPLAY_LAYER_RAW_BPP)

/**
 * Returns the memory address of the given rectangle within the immutable
 * (const) image buffer of the given guac_display_layer_state, where the
 * upper-left corner of the given buffer is (0, 0). If the memory address
 * cannot be calculated because doing so would overflow the maximum value of a
 * size_t, execution of the current process is automatically aborted.
 *
 * IMPORTANT: No checks are performed on whether the rectangle extends beyond
 * the bounds of the buffer, including considering whether the left/top
 * position of the rectangle is negative. If the rectangle has not already been
 * contrained to be within the bounds of the buffer, such checks must be
 * performed before dereferencing the value returned by this macro.
 *
 * @param layer_state
 *     The guac_display_layer_state associated with the image buffer within
 *     which the address of the given rectangle should be determined.
 *
 * @param rect
 *     The rectangle to determine the offset of.
 *
 * @return
 *     The memory address of the given rectangle within the buffer of the given
 *     layer state.
 */
#define GUAC_DISPLAY_LAYER_STATE_CONST_BUFFER(layer_state, rect) \
    GUAC_RECT_CONST_BUFFER(rect, (layer_state).buffer, (layer_state).buffer_stride, GUAC_DISPLAY_LAYER_RAW_BPP)

/**
 * Bitwise flag set on the render_state flag in guac_display when rendering of
 * a pending frame is in progress (Guacamole instructions that draw the pending
 * frame are being sent to connected users).
 */
#define GUAC_DISPLAY_RENDER_STATE_FRAME_IN_PROGRESS 1

/**
 * Bitwise flag set on the render_state flag in guac_display when rendering of
 * a pending frame is NOT in progress (Guacamole instructions that draw the
 * pending frame are NOT being sent to connected users).
 */
#define GUAC_DISPLAY_RENDER_STATE_FRAME_NOT_IN_PROGRESS 2

/**
 * Bitwise flag set on the render_state flag in guac_display when the
 * guac_display has been stopped and all worker threads have terminated (no
 * further frames will render). This flag is set when guac_display_stop() has
 * been invoked, including as part of guac_display_free().
 */
#define GUAC_DISPLAY_RENDER_STATE_STOPPED 4

/**
 * Bitwise flag set on the flush_state flag in guac_display while a flush is
 * active - from the point where guac_display_end_multiple_frames() passes the
 * deferred-frame check and dispatches work to workers, through the last
 * worker clearing it at end-of-frame. Callers entering
 * guac_display_end_multiple_frames() observe this flag to decide whether to
 * defer the frame: if it is set, a prior flush's workers are still running
 * and the new frame accumulates into pending state until the last worker
 * picks it up via display->frame_deferred.
 *
 * This single flag replaces the historical (FIFO non-empty OR active_workers
 * > 0) defer check, which only worked because there was exactly one FIFO and
 * one worker pool. As more pipeline stages are introduced, each with its own
 * FIFO, a single flag covering the entire flush remains a correct and
 * economical signal.
 */
#define GUAC_DISPLAY_FLUSH_IN_PROGRESS 1

/**
 * Bitwise flag set on the pending_state flag in guac_display whenever the
 * pending frame may be safely modified by drawing threads. Cleared by the
 * flush pipeline at the start of a frame flush - from that point until the
 * pipeline re-sets the bit, drawers calling
 * guac_flag_wait_and_lock(pending_state, PENDING_WRITABLE) block until the
 * flush is done reading and modifying pending_frame state.
 *
 * This flag replaces the rwlock that previously guarded pending_frame: an
 * rwlock is thread-bound and cannot be safely handed between the pipeline
 * stage threads that now run draft/rects/search/combine/commit. With the
 * flag, any stage thread can set or clear PENDING_WRITABLE regardless of
 * which thread cleared it, making cross-stage handoff of pending ownership
 * possible without explicit lock migration.
 *
 * Usage conventions:
 *
 *   - Drawers acquire via guac_flag_wait_and_lock(pending_state,
 *     PENDING_WRITABLE); this blocks if the bit is clear (flush in
 *     progress) or if another drawer holds the flag's mutex. Release via
 *     guac_flag_unlock().
 *
 *   - The flush's first stage acquires via guac_flag_wait_and_lock(...,
 *     PENDING_WRITABLE), clears the bit, and unlocks. Subsequent stages
 *     read/modify pending_frame without holding the flag's mutex - the
 *     cleared bit guarantees no drawer thread will be executing
 *     concurrently. The flush's last pending-using stage re-locks, sets
 *     the bit, unlocks, releasing pending ownership back to drawers.
 */
#define GUAC_DISPLAY_PENDING_WRITABLE 1

/**
 * Bitwise flag that is set on the state of a guac_display_render_thread when
 * the thread should be stopped.
 */
#define GUAC_DISPLAY_RENDER_THREAD_STATE_STOPPING 1

/**
 * Bitwise flag that is set on the state of a guac_display_render_thread when
 * visible, graphical changes have been made.
 */
#define GUAC_DISPLAY_RENDER_THREAD_STATE_FRAME_MODIFIED 2

/**
 * Bitwise flag that is set on the state of a guac_display_render_thread when
 * a frame boundary has been reached.
 */
#define GUAC_DISPLAY_RENDER_THREAD_STATE_FRAME_READY 4

/**
 * The state of the mouse cursor, as independently tracked by the render
 * thread. The mouse cursor state may be reported by
 * guac_display_render_thread_notify_user_moved_mouse() to avoid unnecessarily
 * locking the display within instruction handlers (which can otherwise result
 * in delays in handling critical instructions like "sync").
 */
typedef struct guac_display_render_thread_cursor_state {

    /**
     * The user that moved or clicked the mouse.
     *
     * NOTE: This user is NOT guaranteed to still exist in memory. This may be
     * a dangling pointer and must be validated before deferencing.
     */
    guac_user* user;

    /**
     * The X coordinate of the mouse cursor.
     */
    int x;

    /**
     * The Y coordinate of the mouse cursor.
     */
    int y;

    /**
     * The mask representing the states of all mouse buttons.
     */
    int mask;

} guac_display_render_thread_cursor_state;

struct guac_display_render_thread {

    /**
     * The display this render thread should render to.
     */
    guac_display* display;

    /**
     * The actual underlying POSIX thread.
     */
    pthread_t thread;

    /**
     * Flag representing render state. This flag is used to store whether the
     * render thread is stopping and whether the current frame has been
     * modified or is ready.
     *
     * @see GUAC_DISPLAY_RENDER_THREAD_STATE_STOPPING
     * @see GUAC_DISPLAY_RENDER_THREAD_FRAME_MODIFIED
     * @see GUAC_DISPLAY_RENDER_THREAD_FRAME_READY
     */
    guac_flag state;

    /**
     * The current mouse cursor state, as reported by
     * guac_display_render_thread_notify_user_moved_mouse().
     */
    guac_display_render_thread_cursor_state cursor_state;

    /**
     * The number of frames that have been explicitly marked as ready since the
     * last frame sent. This will be zero if explicit frame boundaries are not
     * currently being used.
     */
    unsigned int frames;

};

/**
 * Approximation of how often a region of a layer is modified, as well as what
 * changes have been made to that region since the last frame. This information
 * is used to help advise future optimizations, such as whether lossy
 * compression is appropriate and whether parts of the layer can be copied from
 * other regions rather than resend image data.
 */
typedef struct guac_display_layer_cell {

    /**
     * The last time this particular cell was part of a frame (used to
     * calculate framerate).
     */
    guac_timestamp last_frame;

    /**
     * The region of this cell that has been modified since the last frame was
     * flushed. If the cell has not been modified at all, this will be an empty
     * rect.
     */
    guac_rect dirty;

    /**
     * The rough number of pixels in the dirty rect that have been modified. If
     * the cell has not been modified at all, this will be zero.
     */
    size_t dirty_size;

    /**
     * The display plan operation that is associated with this cell. If a
     * display plan is not currently being created or optimized, this will be
     * NULL.
     */
    guac_display_plan_operation* related_op;

} guac_display_layer_cell;

/**
 * The state of a Guacamole layer or buffer at some point in time. Within
 * guac_display_layer, copies of this structure are used to represent the
 * previous frame and the current, in-progress frame. The previous and
 * in-progress frames are compared during flush to determine what graphical
 * operations need to be sent to connected clients to efficiently transform the
 * remote display from its previous state to the now-current state.
 *
 * IMPORTANT: The lock of the corresponding guac_display_state must be acquired
 * before reading or modifying the values of any member of this structure.
 */
typedef struct guac_display_layer_state {

    /**
     * The width of this layer in pixels.
     */
    int width;

    /**
     * The height of this layer in pixels.
     */
    int height;

    /**
     * The layer which contains this layer. This is only applicable to visible
     * (non-buffer) layers which are not the default layer.
     */
    const guac_layer* parent;

    /**
     * The X coordinate of the upper-left corner of this layer, in pixels,
     * relative to its parent layer. This is only applicable to visible
     * (non-buffer) layers which are not the default layer.
     */
    int x;

    /**
     * The Y coordinate of the upper-left corner of this layer, in pixels,
     * relative to its parent layer. This is only applicable to visible
     * (non-buffer) layers which are not the default layer.
     */
    int y;

    /**
     * The Z-order of this layer, relative to sibling layers. This is only
     * applicable to visible (non-buffer) layers which are not the default
     * layer.
     */
    int z;

    /**
     * The level of opacity applied to this layer. Fully opaque is 255, while
     * fully transparent is 0. This is only applicable to visible (non-buffer)
     * layers which are not the default layer.
     */
    int opacity;

    /**
     * The number of simultaneous touches that this surface can accept, where 0
     * indicates that the surface does not support touch events at all.
     */
    int touches;

    /**
     * Non-zero if all graphical updates for this surface should use lossless
     * compression, 0 otherwise. By default, newly-created surfaces will use
     * lossy compression when heuristics determine it is appropriate.
     */
    int lossless;

    /**
     * The raw, 32-bit buffer of ARGB image data. If the layer was allocated as
     * opaque, the alpha channel of each ARGB pixel will not be considered when
     * compositing or when encoding images.
     *
     * So that large regions of image data can be easily compared, a consistent
     * value for the alpha channel SHOULD be provided so that each 32-bit pixel
     * can be compared without having to separately masking the channel.
     * Optimizations within guac_display, including scroll detection, may
     * assume that the alpha channel can always be considered when comparing
     * pixel values for equivalence.
     */
    unsigned char* buffer;

    /**
     * The width of the image data, in pixels. This is not necessarily the same
     * as the width of the layer.
     */
    int buffer_width;

    /**
     * The height of the image data, in pixels. This is not necessarily the
     * same as the height of the layer.
     */
    int buffer_height;

    /**
     * The number of bytes in each row of image data. This is not necessarily
     * equivalent to 4 * width.
     */
    size_t buffer_stride;

    /**
     * Non-zero if the image data referenced by the buffer pointer was
     * allocated externally and should not be automatically freed or managed by
     * guac_display, zero otherwise.
     */
    int buffer_is_external;

    /**
     * The approximate rectangular region containing all pixels within this
     * layer that have been modified since the frame that occurred before this
     * frame. If the layer was not modified, this will be an empty rect (zero
     * width or zero height).
     */
    guac_rect dirty;

    /**
     * Whether this layer should be searched for possible scroll/copy
     * optimizations.
     */
    int search_for_copies;

    /* ---------------- LAYER LIST POINTERS ---------------- */

    /**
     * The layer immediately prior to this layer within the list containing
     * this layer, or NULL if this is the first layer/buffer in the list.
     */
    guac_display_layer* prev;

    /**
     * The layer immediately following this layer within the list containing
     * this layer, or NULL if this is the last layer/buffer in the list.
     */
    guac_display_layer* next;

} guac_display_layer_state;

struct guac_display_layer {

    /**
     * The guac_display instance that allocated this layer/buffer.
     */
    guac_display* display;

    /**
     * The Guacamole layer (or buffer) that this guac_display_layer will draw
     * to when flushing a frame.
     *
     * NOTE: This value is set only during allocation and may safely be
     * accessed without acquiring the overall layer lock.
     */
    const guac_layer* layer;

    /**
     * Whether the graphical data that will be written to this layer/buffer
     * will only ever be opaque (no alpha channel). Compositing of graphical
     * updates can be faster when no alpha channel need be considered.
     */
    int opaque;

    /**
     * Lock used to ensure sequences of Guacamole protocol path instructions
     * for this layer are not interleaved when sent from concurrent threads.
     */
    pthread_mutex_t path_lock;

    /* ---------------- LAYER PREVIOUS FRAME STATE ---------------- */

    /**
     * The state of this layer when the last frame was flushed to connected clients.
     *
     * IMPORTANT: The display-level last_frame_lock MUST be acquired before
     * modifying or reading this member.
     */
    guac_display_layer_state last_frame;

    /**
     * Off-screen buffer storing the contents of the previously-rendered frame
     * for later use. If graphical updates are recognized as reusing data from
     * a previous frame, that data will be copied from this buffer. Doing this
     * simplifies the copy operation (there is no longer any need to perform
     * those copies in a specific order) and ensures the copies are efficient
     * on the client side (copying from one part of a graphical surface to
     * another part of the same surface can be inefficient, particularly if the
     * regions overlap). In practice, there is ample time between frames for
     * the client to copy a layer's current contents to an off-screen buffer
     * while awaiting the next frame.
     *
     * NOTE: This value is set only during allocation and may safely be
     * accessed without acquiring the display-level last_frame_lock.
     */
    guac_layer* last_frame_buffer;

    /* ---------------- LAYER PENDING FRAME STATE ---------------- */

    /**
     * The upcoming state of this layer when the current, in-progress frame is
     * flushed to connected clients.
     *
     * IMPORTANT: The display-level pending_state MUST be acquired before
     * modifying or reading this member.
     */
    guac_display_layer_state pending_frame;

    /**
     * The Cairo context and surface containing the graphical data of the
     * pending frame. The actual underlying buffer and details of the graphical
     * surface are also available via pending_frame_raw_context.
     *
     * IMPORTANT: The display-level pending_state MUST be acquired before
     * modifying or reading this member.
     */
    guac_display_layer_cairo_context pending_frame_cairo_context;

    /**
     * The raw underlying buffer and details of the surface containing the
     * graphical data of the pending frame. A Cairo context and surface backed
     * by this buffer are also available via pending_frame_cairo_context.
     *
     * IMPORTANT: The display-level pending_state MUST be acquired before
     * modifying or reading this member.
     */
    guac_display_layer_raw_context pending_frame_raw_context;

    /**
     * A two-dimensional array of square tiles representing the nature of
     * changes made to corresponding regions of the display. This is used both
     * to track how frequently certain regions are being updated (to help
     * inform whether lossy compression is appropriate), to track what parts of
     * the frame have actually changed, and to aid in determining whether
     * adjacent updated regions should be combined into a single update.
     *
     * IMPORTANT: The display-level pending_state MUST be acquired before
     * modifying or reading this member.
     */
    guac_display_layer_cell* pending_frame_cells;

    /**
     * The width of the pending_frame_cells array, in cells.
     *
     * IMPORTANT: The display-level pending_state MUST be acquired before
     * modifying or reading this member.
     */
    size_t pending_frame_cells_width;

    /**
     * The height of the pending_frame_cells array, in cells.
     *
     * IMPORTANT: The display-level pending_state MUST be acquired before
     * modifying or reading this member.
     */
    size_t pending_frame_cells_height;

};

typedef struct guac_display_state {

    /**
     * The specific point in time that this guac_display_state represents.
     *
     * Synchronization for guac_display_state members is NOT provided by
     * this struct. For pending_frame, callers must hold pending_state
     * (see guac_display::pending_state and GUAC_DISPLAY_PENDING_WRITABLE).
     * For last_frame, callers must hold guac_display::last_frame_lock
     * (read or write as appropriate).
     */
    guac_timestamp timestamp;

    /**
     * All layers and buffers that were part of the display at the time that
     * the frame/snapshot represented by this guac_display_state was updated.
     * 
     * NOTE: For each guac_display, there are two distinct lists of layers: the
     * last frame layer list and the pending frame layer list:
     *
     * LAST FRAME LAYER LIST
     *
     *  - HEAD: display->last_frame.layers
     *  - NEXT: layer->last_frame.next
     *  - PREV: layer->last_frame.prev
     *
     * PENDING LAYER LIST
     *
     *  - HEAD: display->pending_frame.layers
     *  - NEXT: layer->pending_frame.next
     *  - PREV: layer->pending_frame.prev
     *
     * Existing layers are deleted only at the time a frame is flushed when a
     * layer in the last frame layer list is found to no longer exist in the
     * pending frame layer list. The same goes for the addition of new layers:
     * they are added only during flush when a layer that was not present in
     * the last frame layer list is found to be present in the pending frame
     * layer list.
     */
    guac_display_layer* layers;

    /**
     * The X coordinate of the hotspot of the mouse cursor. The cursor image is
     * stored/updated via the cursor_buffer member of guac_display.
     */
    int cursor_hotspot_x;

    /**
     * The Y coordinate of the hotspot of the mouse cursor. The cursor image is
     * stored/updated via the cursor_buffer member of guac_display.
     */
    int cursor_hotspot_y;

    /**
     * The user that moved or clicked the mouse. This is used to ensure we
     * don't attempt to synchronize an out-of-date mouse position to the user
     * that is actively moving the mouse.
     *
     * NOTE: This user is NOT guaranteed to still exist in memory. This may be
     * a dangling pointer and must be validated before deferencing.
     */
    guac_user* cursor_user;

    /**
     * The X coordinate of the mouse cursor.
     */
    int cursor_x;

    /**
     * The Y coordinate of the mouse cursor.
     */
    int cursor_y;

    /**
     * The mask representing the states of all mouse buttons.
     */
    int cursor_mask;

    /**
     * The number of logical frames that have been rendered to this display
     * state since the previous display state.
     */
    unsigned int frames;

} guac_display_state;

/**
 * Work item that flows through the four plan pipeline stages - draft,
 * rects, search, and commit - one frame at a time. The draft stage is
 * entered by guac_display_end_multiple_frames() enqueuing an empty
 * work item. Each subsequent stage's last finishing chunk enqueues
 * the work item downstream. The commit stage terminates the chain,
 * releasing pending ownership and invoking plan_apply inline before
 * returning to its dequeue loop.
 */
typedef struct guac_display_plan_work {

    /**
     * The plan being shepherded through the pipeline. NULL before the
     * draft stage runs; set to the result of
     * PFW_LFR_guac_display_plan_create() by the draft handler. May
     * remain NULL across all downstream stages if no dirty regions
     * exist to produce a plan - in that case, downstream handlers skip
     * their plan-dependent work and enqueue straight through to commit.
     */
    guac_display_plan* plan;

    /**
     * Non-zero if the commit phase observed any committable change at
     * all (the return value of guac_display_frame_complete()). Set by
     * the commit handler; read by the same handler when deciding
     * whether to enqueue an end-of-frame NOP and whether the encode
     * stage will fire at all.
     */
    int frame_nonempty;

} guac_display_plan_work;

/**
 * Shared completion state for a fan-out of chunks within a single
 * pipeline stage (currently rects or search). One instance is heap-
 * allocated by the handler that fans out the chunks, referenced by each
 * chunk carried on the stage's FIFO, and freed by the worker that
 * processes the last-finishing chunk (the one that decrements
 * remaining to zero).
 *
 * The last-finishing worker is also responsible for logging the phase's
 * timing, enqueuing work downstream, and (in the search stage's case)
 * freeing any auxiliary chunk-list buffer attached via search_chunks.
 */
typedef struct guac_display_plan_fan_out {

    /**
     * Number of chunks that have NOT yet been completed. Each chunk
     * worker atomically decrements this on completion. The worker that
     * observes the decrement result of zero is the "last" worker and
     * owns the post-fan-out work (logging, downstream enqueue, free).
     */
    _Atomic int remaining;

    /**
     * The plan_work to forward downstream once the fan-out completes.
     * Carried here rather than in each chunk so we don't pay the
     * per-chunk size - chunk items on the FIFO should stay small to
     * keep FIFO storage compact.
     */
    guac_display_plan_work forward;

    /**
     * The guac_timestamp at which the fan-out began, captured by the
     * handler that dispatched it. The last-finishing worker subtracts
     * this from the current time to log the phase duration.
     */
    guac_timestamp phase_start;

    /**
     * Human-readable name of the phase being timed (e.g. "rects",
     * "search"). Storage is owned by the caller that set up the fan-
     * out and must outlive it - in practice a static string literal.
     */
    const char* phase_name;

    /**
     * The 1-based ordinal number of this phase within the full plan
     * pipeline, used in the phase-timing log line. "rects" is 2/5,
     * "search" is 3/5, etc.
     */
    int phase_number;

    /**
     * The total number of plan phases, used in the phase-timing log line.
     */
    int phase_total;

    /**
     * Auxiliary buffer of (layer, sub_rect) search chunks backing the
     * entries enqueued onto search_stage.fifo. Populated only when
     * this fan_out is driving the search phase; NULL for fan_outs
     * driving the rects phase. Freed by the last-finishing search
     * chunk along with the fan_out itself.
     */
    guac_display_plan_search_rect* search_chunks;

} guac_display_plan_fan_out;

/**
 * Per-chunk work item for build_stage. One of these is enqueued for
 * each (layer, cell-row-range) pair that a build chunk should process.
 * The handler walks that cell-row range of the given layer, for each
 * dirty cell claiming an output slot in plan->ops via atomic
 * fetch_add on plan->next_op_index, writing the op, running rect-
 * detect on it, and hash-indexing it. The handler then decrements
 * fan_out->remaining.
 */
typedef struct guac_display_plan_build_chunk {

    /**
     * The plan whose ops this chunk writes into. Duplicated here
     * (rather than pulled from fan_out->forward.plan) so the handler
     * can access it without dereferencing the fan_out until the
     * final decrement.
     */
    guac_display_plan* plan;

    /**
     * The layer whose cells this chunk covers. Only dirty cells
     * within layer->pending_frame_cells[cell_row_start ..
     * cell_row_end) produce ops.
     */
    guac_display_layer* layer;

    /**
     * The inclusive starting cell-row index (not pixel row) of the
     * range this chunk covers within layer->pending_frame_cells.
     */
    int cell_row_start;

    /**
     * The exclusive ending cell-row index (not pixel row) of the
     * range this chunk covers.
     */
    int cell_row_end;

    /**
     * Shared fan-out completion state for the whole build dispatch.
     * All build chunks for a given frame share the same fan_out,
     * regardless of which layer they cover.
     */
    guac_display_plan_fan_out* fan_out;

} guac_display_plan_build_chunk;

/**
 * Per-chunk work item for search_stage. One of these is enqueued for
 * each (layer, sub_rect) strip of the copy-detection search. The
 * handler calls guac_display_plan_search_chunk on the strip and
 * decrements fan_out->remaining.
 */
typedef struct guac_display_plan_search_chunk {

    /**
     * The plan being refined. Carried per-chunk to avoid an extra
     * fan_out dereference in the hot path.
     */
    guac_display_plan* plan;

    /**
     * The layer whose last_frame is being searched for this chunk.
     */
    guac_display_layer* layer;

    /**
     * The sub-rectangle of last_frame to scan (includes the right-hand
     * halo required by the rolling 2D hash).
     */
    guac_rect sub_rect;

    /**
     * Shared fan-out completion state for the whole search dispatch.
     */
    guac_display_plan_fan_out* fan_out;

} guac_display_plan_search_chunk;

struct guac_display {

    /* NOTE: Any member of this structure that requires protection against
     * concurrent access is protected by its own lock. The overall display does
     * not have nor need a top-level lock. */

    /**
     * The client associated with this display.
     */
    guac_client* client;

    /* ---------------- DISPLAY FRAME STATES ---------------- */

    /**
     * The state of this display at the time the last frame was sent to
     * connected users.
     */
    guac_display_state last_frame;

    /**
     * Reader-writer lock guarding concurrent access to the members of
     * last_frame and any layer->last_frame substructure. Encoding workers
     * hold the read lock while processing IMG ops; the plan stage's
     * frame_complete holds the write lock while committing pending state
     * into last_frame.
     */
    guac_rwlock last_frame_lock;

    /**
     * The pending state of this display that will become the next frame once
     * it is sent to connected users.
     */
    guac_display_state pending_frame;

    /**
     * Whether the pending frame has been modified in any way outside of
     * changing the mouse cursor or moving the mouse. This is used to help
     * inform whether a frame should be flushed to update connected clients
     * with respect to mouse cursor changes, or whether those changes can be
     * safely assumed to be part of a larger frame containing general graphical
     * updates.
     *
     * IMPORTANT: The display-level pending_state MUST be acquired before
     * modifying or reading this member.
     */
    int pending_frame_dirty_excluding_mouse;

    /* ---------------- WELL-KNOWN LAYERS / BUFFERS ---------------- */

    /**
     * The default layer of the client display.
     */
    guac_display_layer* default_layer;

    /**
     * The buffer storing the current mouse cursor. The hotspot position within
     * the cursor is stored within cursor_hotspot_x and cursor_hotspot_y of
     * guac_display_state.
     */
    guac_display_layer* cursor_buffer;

    /* ---------------- FRAME ENCODING WORKER THREADS ---------------- */

    /**
     * Pipeline stage that pulls graphical operations from its FIFO and
     * applies them by emitting Guacamole instructions to connected clients.
     * This stage runs with many worker threads (sized to cpu_count *
     * GUAC_DISPLAY_CPU_THREAD_FACTOR at alloc time) sharing a single FIFO -
     * the general "FIFO + N workers" case of the guac_display_pipeline_stage
     * pattern. The FIFO lives inside the stage (encode_stage.fifo); items
     * are stored in encode_stage_items[].
     */
    guac_display_pipeline_stage encode_stage;

    /**
     * Storage backing encode_stage.fifo: an array of display-plan operations
     * sized to hold roughly eight worst-case frames of outstanding ops.
     */
    guac_display_plan_operation encode_stage_items[GUAC_DISPLAY_WORKER_FIFO_SIZE];

    /**
     * Pipeline stage that services guac_display_parallel_for() chunk
     * dispatches. Plan stages call parallel_for to fan work-loops out
     * across this stage's workers; the workers each pull a chunk
     * (guac_display_plan_task) from parallel_stage.fifo, run it, and
     * decrement the dispatch's shared completion counter. Sized to
     * cpu_count * GUAC_DISPLAY_CPU_THREAD_FACTOR at alloc time.
     *
     * Separating this from encode_stage means encode workers no longer
     * need to switch on item type, and TASK ops never appear on
     * encode_stage.fifo. The cost is doubling the worker thread count
     * (parallel + encode), which is acceptable because the two pools
     * are sequential under the current FLUSH_IN_PROGRESS gating: only
     * one is busy at a time.
     */
    guac_display_pipeline_stage parallel_stage;

    /**
     * Storage backing parallel_stage.fifo.
     */
    guac_display_plan_task parallel_stage_items[GUAC_DISPLAY_PARALLEL_FIFO_SIZE];

    /**
     * Draft stage - first phase of the plan pipeline. Runs plan_create
     * (PFW_LFR): diffs pending against last to refine per-cell dirty
     * state and count dirty cells, then allocates plan->ops sized to
     * that count. The plan's ops are not yet populated; the build
     * stage fills them in parallel across layers and cell rows.
     * Enqueues build chunks onto build_stage on completion.
     */
    guac_display_pipeline_stage draft_stage;

    /** Storage backing draft_stage.fifo. */
    guac_display_plan_work draft_stage_items[GUAC_DISPLAY_PLAN_FIFO_SIZE];

    /**
     * Build stage - for each dirty layer, walks the layer's
     * pending_frame_cells in chunked cell-row ranges and, for each
     * dirty cell, claims a slot in plan->ops via atomic fetch_add,
     * writes the op, runs rect-detect, and hash-indexes the op.
     * This merges what were previously draft pass 2, the rects
     * phase, and the index phase into one pass of work per cell-
     * row chunk.
     *
     * Chunk items are enqueued by the draft handler (one per layer
     * per chunk) and dispatched to worker_thread_count workers. The
     * last-finishing chunk walks plan->ops to aggregate per-layer
     * pending_frame.dirty rects, then builds the search chunk list
     * and fans out to search_stage (or enqueues to commit if no
     * search work qualifies).
     */
    guac_display_pipeline_stage build_stage;

    /** Storage backing build_stage.fifo. */
    guac_display_plan_build_chunk build_stage_items[GUAC_DISPLAY_BUILD_FIFO_SIZE];

    /**
     * Search stage - runs per-chunk copy-detection on (layer,
     * sub_rect) strips. Chunk items are enqueued by the last-finishing
     * rects chunk (fan-out) and dispatched to worker_thread_count
     * workers. The last-finishing chunk enqueues to commit_stage.
     */
    guac_display_pipeline_stage search_stage;

    /** Storage backing search_stage.fifo. */
    guac_display_plan_search_chunk search_stage_items[GUAC_DISPLAY_SEARCH_FIFO_SIZE];

    /**
     * Commit stage - final phase of the plan pipeline. Runs combine
     * (horizontal + vertical op merging), then frame_complete to copy
     * pending into last_frame. Releases pending_state back to drawers,
     * then invokes guac_display_plan_apply() inline to emit COPY/RECT
     * protocol and enqueue IMG ops onto encode_stage.fifo. Terminates
     * the plan pipeline chain.
     */
    guac_display_pipeline_stage commit_stage;

    /** Storage backing commit_stage.fifo. */
    guac_display_plan_work commit_stage_items[GUAC_DISPLAY_PLAN_FIFO_SIZE];

    /**
     * The current number of active worker threads.
     *
     * IMPORTANT: This member must only be accessed or modified while the ops
     * FIFO is locked.
     */
    unsigned int active_workers;

    /**
     * Whether least one pending frame has been deferred due to the encoding
     * process being underway for a previous frame at the time it was
     * completed.
     *
     * IMPORTANT: This member must only be accessed or modified while the ops
     * FIFO is locked.
     */
    int frame_deferred;

    /**
     * The current state of the rendering process. Code that needs to be aware
     * of whether a frame is currently in the process of being rendered can
     * monitor the state of this flag, watching for either the
     * GUAC_DISPLAY_RENDER_STATE_FRAME_IN_PROGRESS or
     * GUAC_DISPLAY_RENDER_STATE_FRAME_NOT_IN_PROGRESS values.
     */
    guac_flag render_state;

    /**
     * Single-bit flag (GUAC_DISPLAY_FLUSH_IN_PROGRESS) indicating whether a
     * frame flush is currently in progress - i.e. whether any stage of the
     * pipeline still has outstanding work for the previous frame. Set by
     * guac_display_end_multiple_frames() when it dispatches work to the
     * worker pool, cleared by the last worker during end-of-frame
     * processing. The presence of this flag is what drives the
     * deferred-frame decision in guac_display_end_multiple_frames(),
     * replacing the older check of the ops FIFO and active_workers counter.
     */
    guac_flag flush_state;

    /**
     * Single-bit flag (GUAC_DISPLAY_PENDING_WRITABLE) guarding access to
     * pending_frame state. Drawers wait on this flag (via
     * guac_flag_wait_and_lock) to serialize among themselves and against
     * the flush pipeline. Replaces the former pending_state rwlock,
     * which was thread-bound and therefore incompatible with cross-stage
     * pending ownership handoff.
     *
     * Initialized with PENDING_WRITABLE set (drawers may freely draw).
     * See the GUAC_DISPLAY_PENDING_WRITABLE docstring for the full
     * acquire/release protocol.
     */
    guac_flag pending_state;

    /**
     * Wall-clock timestamp captured at the moment the current frame
     * began its pass through the plan pipeline - specifically, in
     * guac_display_end_multiple_frames() right after it observed that
     * no prior flush was in progress and set FLUSH_IN_PROGRESS. Used
     * as the baseline from which each plan-phase log reports its
     * start and end offsets so an operator reading the trace can see,
     * frame by frame, where time is going and whether the phases
     * chain cleanly or leave gaps between hand-offs.
     *
     * Read at the end of the frame by the last encode worker to
     * compute the overall frame duration for the "Frame processed"
     * log line. Memory ordering is handled by the pipeline stage
     * FIFOs' locks: the write happens before an enqueue onto
     * draft_stage.fifo, the read happens after a dequeue from
     * encode_stage.fifo, and each stage's FIFO handoff establishes
     * happens-before.
     */
    guac_timestamp frame_start;

    /**
     * Wall-clock timestamp captured in the commit handler immediately
     * before plan_apply_enqueue_img (or the frame-empty NOP enqueue)
     * pushes work onto encode_stage.fifo. Used by the last encode
     * worker to compute and log the encode-phase duration. Together
     * with frame_start this lets an operator reading the trace see
     * how much of the total frame time was spent inside the encode
     * stage vs. the plan pipeline preceding it - typically most of
     * the per-frame cost for non-trivial frames sits in encode.
     *
     * Memory ordering: written by commit_handler before any enqueue
     * onto encode_stage.fifo, read by the last encode worker after
     * dequeuing from the same FIFO. The FIFO's internal lock
     * establishes happens-before.
     */
    guac_timestamp encode_start;

    /**
     * Sum (in microseconds) of per-op IMG-handling time across all
     * encode workers for the current frame. Each worker adds its
     * op's elapsed time once per IMG op processed. At end-of-frame,
     * dividing this by the encode-phase wall duration gives the
     * effective parallelism of the encode pool: a value equal to
     * encode_stage.thread_count means all workers were fully busy
     * the entire encode phase; a smaller value means workers were
     * idling (either the op count was too small or dispatch was
     * uneven).
     *
     * Reset to zero in commit_handler before any op is enqueued for
     * the current frame. Updated via relaxed atomic fetch_add by
     * each worker; the end-of-frame branch's FIFO lock provides the
     * happens-before needed to read the final sum.
     */
    _Atomic size_t encode_img_us;

    /**
     * Sum (in microseconds) of time spent inside the actual codec
     * call (guac_client_stream_png/webp/jpeg) across all encode
     * workers for the current frame. Compared against encode_img_us
     * this tells an operator what fraction of encode-phase worker
     * time was real codec work versus overhead (select_encoding
     * optimality scan, cairo surface setup/destroy, clear_non_opaque,
     * protocol writes outside the stream_* call, FIFO/lock traffic).
     *
     * Reset and updated with the same semantics as encode_img_us.
     */
    _Atomic size_t encode_codec_us;

    /**
     * Dynamic per-frame wire-byte budget shared across encode
     * workers. Initialised at guac_display_plan_apply_emit() to
     * the per-frame budget derived from the slowest user's
     * throughput estimate, and depleted by each IMG op as it
     * emits bytes to the socket.
     *
     * Each worker, before encoding its op, reads this and
     * remaining_frame_pixels to compute its share of the remaining
     * budget via (remaining_frame_bytes × op_pixels /
     * remaining_frame_pixels); after encoding, it subtracts the
     * bytes it actually wrote from remaining_frame_bytes and its
     * op_pixels from remaining_frame_pixels. This amortises any
     * overage or surplus from already-encoded ops onto the still-
     * to-encode ones - if an earlier op's WebP output overshot its
     * fair share, the later ops see a tighter slice and tighten
     * their own target_size accordingly.
     *
     * May go negative on overage - signed int, readers must treat
     * values <= 0 as "budget exhausted" and skip target_size
     * targeting (letting the quality ceiling bound output) rather
     * than feeding a negative number into libwebp.
     */
    _Atomic int remaining_frame_bytes;

    /**
     * Sum of pixel areas across IMG ops that haven't been encoded
     * yet for the current frame. Paired with remaining_frame_bytes
     * to compute a fair per-op byte share on-the-fly. See
     * remaining_frame_bytes for the accounting protocol.
     *
     * May go to zero or below if ops overshoot the bookkeeping
     * (shouldn't happen in practice); readers guard the division.
     */
    _Atomic int remaining_frame_pixels;

    /**
     * Non-zero if a drawing context (guac_display_layer_open_raw() or
     * guac_display_layer_open_cairo()) has been opened and is either
     * currently blocked waiting to acquire pending_state or has just
     * acquired it. Set by the drawing thread immediately before it attempts
     * the (possibly blocking) lock acquire, and cleared immediately after
     * the acquire returns.
     *
     * Polled by the flush's scroll-detection search (display-plan phase 3)
     * to detect that a drawing thread is waiting to write to the pending
     * frame. If the search has not yet converted any IMG op to COPY, it
     * will drain its workers and abort so the waiting thread can proceed
     * sooner. If the search has already found matches, it is already
     * paying its way and runs to completion.
     *
     * Only drawing entry points bump this flag - lock acquires for layer
     * property changes, cursor updates, etc. do not. Those acquires are
     * infrequent, not correlated with scroll-heavy workloads, and not worth
     * plumbing through the full abort decision.
     */
    _Atomic int draw_pending;

};

/**
 * Allocates and inserts a new element into the given linked list of display
 * layers, associating it with the given layer and surface.
 *
 * @param head
 *     A pointer to the head pointer of the list of layers. The head pointer
 *     will be updated by this function to point to the newly-allocated
 *     display layer.
 *
 * @param layer
 *     The Guacamole layer to associated with the new display layer.
 *
 * @param opaque
 *     Non-zero if the new layer will only ever contain opaque image contents
 *     (the alpha channel should be ignored), zero otherwise.
 *
 * @return
 *     The newly-allocated display layer, which has been associated with the
 *     provided layer and surface.
 */
guac_display_layer* guac_display_add_layer(guac_display* display, guac_layer* layer, int opaque);

/**
 * Removes the given layer from all linked lists containing that layer and
 * frees all associated memory.
 *
 * @param display_layer
 *     The layer to remove.
 */
void guac_display_remove_layer(guac_display_layer* display_layer);

/**
 * Resizes the given layer to the given dimensions, including any underlying
 * image buffers.
 *
 * @param layer
 *     The layer to resize.
 *
 * @param width
 *     The new width, in pixels.
 *
 * @param height
 *     The new height, in pixels.
 */
void PFW_guac_display_layer_resize(guac_display_layer* layer,
        int width, int height);

/**
 * Worker thread that continuously pulls operations from the operation FIFO of
 * the given guac_display, applying those operations by seding corresponding
 * instructions to connected clients.
 *
 * @param data
 *     A pointer to the guac_display.
 *
 * @return
 *     Always NULL.
 */
void* guac_display_worker_thread(void* data);

/**
 * Per-item handler for parallel_stage. Invoked once per dequeued
 * guac_display_plan_task: runs task->func with task->context over the
 * task's [start, end) range, then decrements task->state->remaining
 * and signals state->done when the dispatching parallel_for has
 * received completion notifications for all of its chunks.
 *
 * @param item
 *     A pointer to the dequeued guac_display_plan_task item.
 *
 * @param user_data
 *     Unused (parallel_stage workers reach all needed state through
 *     the task itself).
 */
void guac_display_parallel_task_handler(void* item, void* user_data);

/**
 * Per-item handler for draft_stage. Runs plan_create (dirty-cell
 * detection + plan allocation) and stamps the resulting plan onto
 * the work item, then fans out build chunks onto build_stage. If
 * plan_create returns NULL (no dirty cells) the handler forwards
 * the work directly to commit_stage instead.
 *
 * @param item
 *     A pointer to the dequeued guac_display_plan_work item. On entry
 *     work->plan is NULL; on successful return it is either the newly-
 *     built plan or NULL if no layer is dirty.
 *
 * @param user_data
 *     A pointer to the guac_display.
 */
void guac_display_draft_handler(void* item, void* user_data);

/**
 * Per-item handler for build_stage. Walks the chunk's cell-row range
 * within the chunk's layer. For each dirty cell, the handler claims
 * an op slot in plan->ops via atomic fetch_add on plan->next_op_index,
 * writes the op, runs rewrite_op_as_rect on it, and indexes it into
 * plan->ops_by_hash. Clean cells within the range have their
 * related_op pointer reset to NULL so the combine phase can skip
 * them cleanly.
 *
 * After walking the range, the handler decrements the fan-out
 * counter. The last-finishing chunk then (a) walks plan->ops to
 * aggregate per-layer pending_frame.dirty rects (since the per-chunk
 * op writes skipped the extend to avoid cross-chunk contention on
 * the layer dirty rect), (b) builds the search chunk list and fans
 * out to search_stage (or enqueues to commit_stage directly when no
 * search chunks qualify), and (c) logs phase timing and frees the
 * build fan_out.
 *
 * @param item
 *     A pointer to the dequeued guac_display_plan_build_chunk item.
 *
 * @param user_data
 *     A pointer to the guac_display.
 */
void guac_display_build_chunk_handler(void* item, void* user_data);

/**
 * Per-item handler for search_stage. Runs
 * PFR_LFR_guac_display_plan_search_chunk on the chunk's (layer,
 * sub_rect), then decrements the fan-out counter. The last-finishing
 * chunk logs phase timing, inspects plan->search_aborted for the
 * early-abort trace, enqueues the plan_work to commit_stage, and
 * frees both the fan_out and its auxiliary search-chunks buffer.
 *
 * @param item
 *     A pointer to the dequeued guac_display_plan_search_chunk item.
 *
 * @param user_data
 *     A pointer to the guac_display.
 */
void guac_display_search_chunk_handler(void* item, void* user_data);

/**
 * Per-item handler for commit_stage. Runs combine (horizontal +
 * vertical) and frame_complete, releases pending back to drawers, then
 * invokes plan_apply inline to emit COPY/RECT protocol and enqueue IMG
 * ops onto encode_stage.fifo. Clears FLUSH_IN_PROGRESS when no
 * worker-visible work was enqueued. Terminates the plan pipeline
 * chain.
 *
 * @param item
 *     A pointer to the dequeued guac_display_plan_work item. Ownership
 *     of work->plan transfers to this handler.
 *
 * @param user_data
 *     A pointer to the guac_display.
 */
void guac_display_commit_handler(void* item, void* user_data);

/**
 * Invokes the given task function over contiguous chunks of [0, count),
 * distributing chunks across the display's existing worker threads. This
 * function may only be invoked when the display's worker threads are not
 * currently processing frame-related ops (IMG/NOP). In practice, this means
 * this function is only safe to call during plan generation, after the
 * deferred-frame check in guac_display_end_multiple_frames() has confirmed
 * the ops FIFO is empty and no workers are active.
 *
 * If count is zero, the function returns immediately. If count is small
 * enough that parallelization would not be worthwhile, or if there is only
 * a single worker thread, the task function is invoked directly on the
 * calling thread with start=0 and end=count. Otherwise, the range [0, count)
 * is split into approximately encode_stage.thread_count chunks (each at least
 * min_chunk_size in size), dispatched via the ops FIFO, and executed
 * concurrently by worker threads. The calling thread blocks until all
 * chunks have completed.
 *
 * The task function itself receives a half-open range [start, end) and
 * must operate only on data within that range. It MUST NOT modify state
 * shared with other chunks without external synchronization.
 *
 * @param display
 *     The display whose worker threads should process the given work.
 *
 * @param count
 *     The total number of indices in the range to process.
 *
 * @param min_chunk_size
 *     The minimum chunk size in indices. If the range cannot be split into
 *     multiple chunks of at least this size, the task is invoked sequentially
 *     on the calling thread.
 *
 * @param func
 *     The function to invoke for each chunk.
 *
 * @param context
 *     An opaque context pointer to pass through to each invocation of the
 *     given function. The caller must ensure this pointer remains valid
 *     until guac_display_parallel_for() returns.
 */
void guac_display_parallel_for(guac_display* display, int count,
        int min_chunk_size, void (*func)(void* context, int start, int end),
        void* context);

#endif
