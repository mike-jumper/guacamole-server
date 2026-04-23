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

#ifndef GUAC_DISPLAY_ATOMIC_H
#define GUAC_DISPLAY_ATOMIC_H

/**
 * Relaxed-memory-order atomic primitives used by guac_display to coordinate
 * concurrent access to the per-plan ops_by_hash table and its associated
 * occupancy bitmap from multiple worker threads.
 *
 * Implemented directly on top of C11 <stdatomic.h>. Configure probes for
 * header availability and errors out if it is missing, so this translation
 * unit is only ever compiled in an environment known to support C11
 * atomics. The per-frame concurrent access patterns exercised by the
 * search and index phases cannot be supported by a slower fallback (such
 * as a global mutex) without a severe performance regression, so no such
 * fallback is provided.
 *
 * Relaxed memory ordering is used throughout. Stronger ordering is not
 * required because the phases that produce and consume ops_by_hash entries
 * - the index phase and the search phase respectively - are separated by a
 * parallel-for barrier, which supplies any needed release/acquire
 * synchronization.
 *
 * NOTE: The wrappers take plain (non-_Atomic) pointers. They internally
 * cast the target address to an _Atomic-qualified pointer when invoking
 * the C11 atomic primitives. This is technically implementation-defined
 * under C11 (the standard reserves operations on non-_Atomic types), but
 * is well-defined and portable across every mainstream C11 implementation:
 * _Atomic T has the same object representation as T for all lock-free
 * integer and pointer types on every supported architecture. Using plain
 * field types keeps callers free to perform bulk operations (e.g. memset
 * for table reset) that would otherwise require per-element atomic_init.
 *
 * @file display-atomic.h
 */

#include <stdatomic.h>
#include <stdint.h>

static inline uint8_t guac_display_atomic_load_u8(const uint8_t* ptr) {
    return atomic_load_explicit(
            (const _Atomic uint8_t*) ptr, memory_order_relaxed);
}

static inline void guac_display_atomic_or_u8(uint8_t* ptr, uint8_t mask) {
    (void) atomic_fetch_or_explicit(
            (_Atomic uint8_t*) ptr, mask, memory_order_relaxed);
}

static inline void guac_display_atomic_and_u8(uint8_t* ptr, uint8_t mask) {
    (void) atomic_fetch_and_explicit(
            (_Atomic uint8_t*) ptr, mask, memory_order_relaxed);
}

static inline uint64_t guac_display_atomic_load_u64(const uint64_t* ptr) {
    return atomic_load_explicit(
            (const _Atomic uint64_t*) ptr, memory_order_relaxed);
}

static inline void* guac_display_atomic_load_ptr(void* const* ptr) {
    return atomic_load_explicit(
            (const _Atomic(void*)*) ptr, memory_order_relaxed);
}

static inline void* guac_display_atomic_exchange_ptr(void** ptr, void* desired) {
    return atomic_exchange_explicit(
            (_Atomic(void*)*) ptr, desired, memory_order_relaxed);
}

/**
 * Attempts to atomically set *ptr to `desired` if and only if it currently
 * equals *expected. On success, returns non-zero. On failure, returns zero
 * and stores the current value of *ptr into *expected.
 */
static inline int guac_display_atomic_cas_ptr(void** ptr,
        void** expected, void* desired) {
    return atomic_compare_exchange_strong_explicit(
            (_Atomic(void*)*) ptr, expected, desired,
            memory_order_relaxed, memory_order_relaxed);
}

/**
 * Attempts to atomically set *ptr to `desired` if and only if it currently
 * equals *expected, where *ptr is an int-sized integer or enum value.
 * Used by the copy-search phase to race hash-path and extension-path
 * workers on a single operation's type field.
 *
 * The caller is responsible for ensuring the target has the same size and
 * representation as int (typically enforced via _Static_assert at the
 * call site for enum types).
 */
static inline int guac_display_atomic_cas_int(int* ptr,
        int* expected, int desired) {
    return atomic_compare_exchange_strong_explicit(
            (_Atomic int*) ptr, expected, desired,
            memory_order_relaxed, memory_order_relaxed);
}

#endif
