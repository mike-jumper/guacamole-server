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

#include "display-pipeline.h"

#include "guacamole/fifo.h"
#include "guacamole/mem.h"

#include <pthread.h>
#include <stddef.h>

void guac_display_pipeline_stage_init(guac_display_pipeline_stage* stage,
        const char* name, void* items, size_t max_items, size_t item_size,
        int thread_count,
        void* (*worker_entry)(void*), void* user_data) {

    stage->name = name;
    stage->thread_count = thread_count;
    stage->threads = guac_mem_alloc(thread_count, sizeof(pthread_t));
    stage->user_data = user_data;
    stage->item_handler = NULL;

    guac_fifo_init(&stage->fifo, items, max_items, item_size);

    for (int i = 0; i < thread_count; i++)
        pthread_create(&stage->threads[i], NULL, worker_entry, stage);

}

/**
 * Built-in worker entry used by stages initialized via
 * guac_display_pipeline_stage_init_handler(). Allocates a per-thread item
 * buffer of the FIFO's item_size, then loops: dequeue-and-handle until
 * the FIFO is invalidated.
 */
static void* guac_display_pipeline_stage_handler_worker(void* data) {

    guac_display_pipeline_stage* stage = (guac_display_pipeline_stage*) data;

    /* One malloc per worker lifetime: avoids VLA stack concerns and keeps
     * the hot dequeue loop tight. */
    void* buffer = guac_mem_alloc(stage->fifo.item_size);

    while (guac_fifo_dequeue(&stage->fifo, buffer))
        stage->item_handler(buffer, stage->user_data);

    guac_mem_free(buffer);
    return NULL;

}

void guac_display_pipeline_stage_init_handler(
        guac_display_pipeline_stage* stage,
        const char* name, void* items, size_t max_items, size_t item_size,
        int thread_count,
        guac_display_pipeline_stage_item_handler* handler, void* user_data) {

    /* Defer to the generic init; the item_handler field is assigned here
     * (overriding the NULL left by the generic init) so the built-in
     * worker loop can find it. */
    guac_display_pipeline_stage_init(stage, name, items, max_items,
            item_size, thread_count,
            guac_display_pipeline_stage_handler_worker, user_data);
    stage->item_handler = handler;

}

void guac_display_pipeline_stage_shutdown(guac_display_pipeline_stage* stage) {

    /* Idempotent: second shutdown observes an already-released thread array
     * and returns. */
    if (stage->threads == NULL)
        return;

    /* Signal any workers blocked on guac_fifo_dequeue_and_lock() to exit
     * their dequeue loop by invalidating the FIFO. If the FIFO was already
     * invalidated by the caller (e.g. as part of a broader shutdown
     * sequence), the invalidate call is a no-op. */
    if (guac_fifo_is_valid(&stage->fifo))
        guac_fifo_invalidate(&stage->fifo);

    for (int i = 0; i < stage->thread_count; i++)
        pthread_join(stage->threads[i], NULL);

    guac_mem_free(stage->threads);
    stage->threads = NULL;
    stage->thread_count = 0;

    guac_fifo_destroy(&stage->fifo);

}
