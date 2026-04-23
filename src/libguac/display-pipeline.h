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

#ifndef GUAC_DISPLAY_PIPELINE_H
#define GUAC_DISPLAY_PIPELINE_H

#include "guacamole/fifo.h"

#include <pthread.h>
#include <stddef.h>

/**
 * A pipeline stage: a FIFO paired with a fixed number of worker threads
 * that dequeue and process items from it. Every asynchronous stage of the
 * guac_display processing chain - the existing image-encoding pool, and
 * every future per-phase plan-processing stage - is an instance of this
 * pattern. The existing encoding pool is the "many workers, one shared
 * FIFO" case; per-phase plan stages are the "single worker, one dedicated
 * FIFO" case.
 *
 * The stage owns its FIFO (initialized in place at stage init time and
 * destroyed at stage shutdown) and its worker threads. The item-storage
 * backing the FIFO is still caller-provided, since it must be inline in a
 * larger struct with compile-time size.
 *
 * Stages may be initialized in one of two modes:
 *
 *   - "Generic worker" mode (guac_display_pipeline_stage_init): the caller
 *     supplies a pthread entry function and keeps full control of the
 *     dequeue loop and per-op lock lifecycle. Used by the encoding stage,
 *     which needs dequeue_and_lock semantics to coordinate active-worker
 *     accounting and end-of-frame detection.
 *
 *   - "Per-item handler" mode (guac_display_pipeline_stage_init_handler):
 *     the stage provides the dequeue loop itself and invokes a caller-
 *     supplied handler once per dequeued item. The right choice for simple
 *     stages that don't need lock-held-across-dequeue semantics. Avoids
 *     the duplicated while-loop boilerplate that would otherwise appear in
 *     every stage's worker entry.
 */
typedef struct guac_display_pipeline_stage guac_display_pipeline_stage;

/**
 * Per-item handler invoked by the built-in worker loop in
 * guac_display_pipeline_stage_init_handler() mode. Called once for each
 * item dequeued from the stage's FIFO, with FIFO and any other locks
 * fully released.
 *
 * @param item
 *     Pointer to the dequeued item (stage->fifo.item_size bytes).
 *
 * @param user_data
 *     The opaque user-data pointer registered at stage init time.
 */
typedef void guac_display_pipeline_stage_item_handler(void* item,
        void* user_data);

struct guac_display_pipeline_stage {

    /**
     * Human-readable name for this stage, used only for diagnostic logging.
     * Storage is owned by the caller and must outlive the stage.
     */
    const char* name;

    /**
     * FIFO owned by this stage. Initialized in guac_display_pipeline_stage_
     * init() over caller-provided item storage, destroyed in
     * guac_display_pipeline_stage_shutdown().
     */
    guac_fifo fifo;

    /**
     * Heap-allocated array of worker thread handles, one per worker.
     */
    pthread_t* threads;

    /**
     * The number of worker threads in this stage. Zero after shutdown.
     */
    int thread_count;

    /**
     * Opaque user data, passed by the stage infrastructure to the worker
     * entry or per-item handler. For generic-worker stages, this is also
     * what the worker_entry receives as the single pthread arg via
     * stage->user_data.
     */
    void* user_data;

    /**
     * Per-item handler function, populated only by
     * guac_display_pipeline_stage_init_handler(). NULL for stages
     * initialized via guac_display_pipeline_stage_init().
     */
    guac_display_pipeline_stage_item_handler* item_handler;

};

/**
 * Initializes a pipeline stage in "generic worker" mode, spawning
 * thread_count worker threads that each run the given entry function. The
 * stage owns its FIFO (initialized here over the caller-provided items
 * array) and its worker threads. The pthread arg passed to worker_entry
 * is the stage itself - the worker can access stage->fifo, stage->user_
 * data, etc. directly.
 *
 * @param stage
 *     The stage to initialize.
 *
 * @param name
 *     Human-readable name for this stage (must outlive the stage).
 *
 * @param items
 *     Caller-provided storage for FIFO items. Must be sized max_items *
 *     item_size bytes. Must outlive the stage.
 *
 * @param max_items
 *     Maximum number of items that may be enqueued at once.
 *
 * @param item_size
 *     Size in bytes of each FIFO item.
 *
 * @param thread_count
 *     Number of worker threads to spawn. Must be positive.
 *
 * @param worker_entry
 *     The pthread_create() entry function for each worker. Receives the
 *     stage pointer as its single argument; worker should access
 *     stage->fifo directly and retrieve its context via stage->user_data.
 *
 * @param user_data
 *     Opaque context pointer accessible to the worker via stage->user_data.
 */
void guac_display_pipeline_stage_init(guac_display_pipeline_stage* stage,
        const char* name, void* items, size_t max_items, size_t item_size,
        int thread_count,
        void* (*worker_entry)(void*), void* user_data);

/**
 * Initializes a pipeline stage in "per-item handler" mode. The stage
 * spawns thread_count workers that each run a built-in dequeue loop,
 * invoking handler(item, user_data) once per dequeued item. Stage setup
 * otherwise mirrors guac_display_pipeline_stage_init().
 *
 * @param stage
 *     The stage to initialize.
 *
 * @param name
 *     Human-readable name for this stage (must outlive the stage).
 *
 * @param items
 *     Caller-provided storage for FIFO items.
 *
 * @param max_items
 *     Maximum number of items that may be enqueued at once.
 *
 * @param item_size
 *     Size in bytes of each FIFO item.
 *
 * @param thread_count
 *     Number of worker threads to spawn. Must be positive.
 *
 * @param handler
 *     Function to invoke per dequeued item. Called with FIFO unlocked.
 *
 * @param user_data
 *     Opaque context pointer passed to the handler on each call.
 */
void guac_display_pipeline_stage_init_handler(
        guac_display_pipeline_stage* stage,
        const char* name, void* items, size_t max_items, size_t item_size,
        int thread_count,
        guac_display_pipeline_stage_item_handler* handler, void* user_data);

/**
 * Signals all workers in the stage to terminate (by invalidating the
 * stage's FIFO) and joins the worker threads. Then destroys the stage's
 * FIFO. After return the stage is no longer usable.
 *
 * Safe to call more than once: the second call observes thread_count == 0
 * and returns without further work.
 *
 * @param stage
 *     The stage to shut down.
 */
void guac_display_pipeline_stage_shutdown(guac_display_pipeline_stage* stage);

#endif
