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
#include "guacamole/error.h"
#include "guacamole/protocol.h"
#include "guacamole/socket.h"
#include "guacamole/timestamp.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

char __guac_socket_BASE64_CHARACTERS[64] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
    'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd',
    'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's',
    't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', '+', '/'
};

#ifdef HAVE_VECTOR_EXTENSIONS
/**
 * 16-wide signed byte vector. 16 bytes is the SSE2 register width and the
 * natural NEON quadword width, so one vector holds the 16 6-bit indices
 * produced by encoding 12 input bytes in parallel (four base64 quartets).
 */
typedef signed char guac_v16i8 __attribute__((vector_size(16)));

/**
 * 4-wide uint32 vector, an alias over the same 16-byte storage as
 * guac_v16i8. Used to drive the four per-dword masked shifts that
 * extract the four 6-bit indices from each 3-byte group after the pshufb
 * spread in __guac_socket_encode_base64_12.
 */
typedef uint32_t guac_v4u32 __attribute__((vector_size(16)));

/**
 * pshufb control mask that spreads 12 input bytes [B0..B11] into 16
 * output bytes arranged as four 4-byte groups, where each group holds one
 * base64 quartet's source bytes rearranged for in-register index
 * extraction. For the first group [B0, B1, B2], the output bytes are
 * [B0, B2, B1, B0] (little-endian positions 0..3), which - read as a
 * 32-bit value - yields v = B0 | (B2<<8) | (B1<<16) | (B0<<24). The
 * filler byte at position 0 of each group is an arbitrary input byte
 * (we reuse B0 for simplicity); it never contributes to any of the four
 * extracted indices because every extraction mask targets bits 8..31 of
 * the dword.
 *
 * Used only by the shuffle-based encode path, which is gated at runtime
 * on __builtin_cpu_supports("ssse3") (or statically enabled on targets
 * where a byte-shuffle is unconditionally available, e.g. AArch64
 * NEON). On x86 the shuffle-based functions carry a target("ssse3")
 * attribute so the compiler emits PSHUFB even in baseline-x86-64
 * builds.
 */
static const guac_v16i8 __guac_socket_base64_spread_mask = {
    0, 2, 1, 0,     3, 5, 4, 3,     6, 8, 7, 6,     9, 11, 10, 9
};

/**
 * 16-wide unsigned byte vector. Paired with guac_v16i8 for the alphabet
 * translation below: the comparisons use the signed view (so they lower
 * to native SSE2 pcmpgtb / NEON cmgt s8 without the unsigned-to-signed
 * XOR-with-0x80 emulation that GCC emits for unsigned-compare on SSE2),
 * and the add / and steps use the unsigned view so the modular
 * arithmetic is language-defined rather than relying on the overflow
 * behavior of signed char addition (which UBSan flags and which GCC's
 * optimizer is legally free to assume doesn't happen).
 */
typedef unsigned char guac_v16u8 __attribute__((vector_size(16)));

/**
 * Translates 16 6-bit base64 indices (each in [0, 63]) to their
 * corresponding base64 alphabet characters using range-based arithmetic
 * offsets, eliminating the per-index 64-byte table load of the scalar
 * path.
 *
 * The standard base64 alphabet consists of five contiguous ASCII
 * subranges, each reached from 'A'+idx by adding a cumulative delta as
 * the index crosses each range boundary:
 *
 *    idx range  | character range  | cumulative delta from ('A' + idx)
 *    [ 0, 25]   | 'A'..'Z' (65..90)|    0
 *    [26, 51]   | 'a'..'z' (97..122)|  +6
 *    [52, 61]   | '0'..'9' (48..57)|  -69
 *    [62]       | '+'      (43)    |  -84
 *    [63]       | '/'      (47)    |  -81
 *
 * Each step uses a vector cmpgt against the threshold to produce a -1
 * mask where the threshold is exceeded, ANDs the mask with the delta, and
 * adds the result in-lane. Only cmpgt/and/add on bytes are used, which
 * are SSE2-baseline (pcmpgtb/pand/paddb) and compile to
 * vcgtq_s8/vandq_s8/vaddq_s8 on NEON. All add/and operations are carried
 * out on the unsigned view of the byte vector so the modular-wraparound
 * sums like 63+65=128 (which overflow signed char) remain standards-
 * defined rather than implementation-quirk UB.
 */
static inline guac_v16u8 __guac_socket_translate_base64_alphabet(
        guac_v16u8 idx) {

    const guac_v16u8 base    = { 'A','A','A','A','A','A','A','A',
                                 'A','A','A','A','A','A','A','A' };

    /* Thresholds are stored as signed for the cmpgt. Index values are
     * in [0, 63] and thresholds are 25, 51, 61, 62 - all within the
     * positive int8 range, so signed and unsigned comparison agree
     * here but signed maps to a single native SSE2 pcmpgtb. */
    const guac_v16i8 t25     = { 25,25,25,25,25,25,25,25,
                                 25,25,25,25,25,25,25,25 };
    const guac_v16i8 t51     = { 51,51,51,51,51,51,51,51,
                                 51,51,51,51,51,51,51,51 };
    const guac_v16i8 t61     = { 61,61,61,61,61,61,61,61,
                                 61,61,61,61,61,61,61,61 };
    const guac_v16i8 t62     = { 62,62,62,62,62,62,62,62,
                                 62,62,62,62,62,62,62,62 };

    /* Deltas as raw byte patterns. -75 is 181 (0xB5), -15 is 241 (0xF1).
     * We add them in unsigned-byte arithmetic where +NN and +(NN mod 256)
     * are the same operation. */
    const guac_v16u8 d_lower = {   6,   6,   6,   6,   6,   6,   6,   6,
                                   6,   6,   6,   6,   6,   6,   6,   6 };
    const guac_v16u8 d_digit = { 181, 181, 181, 181, 181, 181, 181, 181,
                                 181, 181, 181, 181, 181, 181, 181, 181 };
    const guac_v16u8 d_plus  = { 241, 241, 241, 241, 241, 241, 241, 241,
                                 241, 241, 241, 241, 241, 241, 241, 241 };
    const guac_v16u8 d_slash = {   3,   3,   3,   3,   3,   3,   3,   3,
                                   3,   3,   3,   3,   3,   3,   3,   3 };

    guac_v16i8 idx_s = (guac_v16i8) idx;
    guac_v16u8 result = idx + base;
    result += (guac_v16u8) (idx_s > t25) & d_lower;
    result += (guac_v16u8) (idx_s > t51) & d_digit;
    result += (guac_v16u8) (idx_s > t61) & d_plus;
    result += (guac_v16u8) (idx_s > t62) & d_slash;
    return result;

}

/**
 * Hybrid encoder: scalar bit extraction + SIMD alphabet translation.
 * Used on strict SSE2 baseline (no SSSE3 at runtime) and on any target
 * without vector_size support. The 12 input bytes are unrolled into 16
 * 6-bit indices scalar-wise, then the alphabet translation runs as a
 * single vector cmpgt/and/add sequence against the idx vector. The
 * alphabet step is SSE2-baseline and never needs runtime dispatch.
 *
 * On SSE2-only x86, __builtin_shuffle would scalarize to 16 sequential
 * byte moves via the stack - measurably slower than extracting the
 * indices scalar-wise to begin with - so this hybrid remains the
 * preferred path when PSHUFB is unavailable.
 */
static inline void __guac_socket_encode_base64_12_hybrid(
        const unsigned char* src, char* dst) {

    unsigned char indices[16];
    for (int g = 0; g < 4; g++) {
        unsigned char a = src[0];
        unsigned char b = src[1];
        unsigned char c = src[2];
        indices[4*g + 0] = (a & 0xFC) >> 2;
        indices[4*g + 1] = ((a & 0x03) << 4) | ((b & 0xF0) >> 4);
        indices[4*g + 2] = ((b & 0x0F) << 2) | ((c & 0xC0) >> 6);
        indices[4*g + 3] = c & 0x3F;
        src += 3;
    }

    guac_v16u8 idx;
    memcpy(&idx, indices, sizeof(idx));

    guac_v16u8 chars = __guac_socket_translate_base64_alphabet(idx);
    memcpy(dst, &chars, sizeof(chars));

}

/**
 * Transforms a vector holding 12 source bytes in lanes 0..11 (lanes 12..15
 * are don't-care) into the 16 base64 characters that encode those bytes,
 * writing the result to dst. Splitting the load out of the transform lets
 * the hot fast path in guac_socket_write_base64 issue a single 16-byte
 * movdqu against the caller's buffer instead of the two-part 8+4 byte
 * load that GCC otherwise emits when memcpying exactly 12 bytes into a
 * 16-byte vector (used only for the final 12..15 byte iteration).
 *
 * After the spread each 32-bit lane holds
 *
 *    v = (B0 << 24) | (B1 << 16) | (B2 << 8) | filler
 *
 * with filler = B0 (unreferenced below). The four indices are packed
 * into their destination bytes by four disjoint masked shifts:
 *
 *    I0 at byte 0:  v >> 26                 (top 6 of B0)
 *    I1 at byte 1: (v >> 12) & 0x00003F00   (B0[1..0] B1[7..4])
 *    I2 at byte 2: (v <<  2) & 0x003F0000   (B1[3..0] B2[7..6])
 *    I3 at byte 3: (v << 16) & 0x3F000000   (B2[5..0])
 *
 * The high 2 bits of every output lane are cleared by the masks, so
 * __guac_socket_translate_base64_alphabet sees signed bytes already in
 * [0, 63].
 *
 * On x86 this function is compiled with target("ssse3") so __builtin_shuffle
 * lowers to PSHUFB even in a baseline-x86-64 build - callers gate their
 * invocation on __builtin_cpu_supports("ssse3") (see
 * guac_socket_write_base64 below) so the function is only ever reached
 * on CPUs that actually support the instruction. On AArch64 NEON the
 * shuffle lowers to TBL natively and no target attribute is required.
 * The static inline declaration is kept so the compiler can inline this
 * into the SSSE3-targeted hot loop in guac_socket_write_base64 (same-
 * target inlining is permitted).
 */
#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("ssse3")))
#endif
static inline void __guac_socket_transform_base64_12_shuffle(
        guac_v16i8 v, char* dst) {

    guac_v16i8 spread =
        __builtin_shuffle(v, __guac_socket_base64_spread_mask);

    guac_v4u32 v32;
    memcpy(&v32, &spread, sizeof(v32));

    guac_v4u32 indices = (v32 >> 26)
                       | ((v32 >> 12) & 0x00003F00u)
                       | ((v32 <<  2) & 0x003F0000u)
                       | ((v32 << 16) & 0x3F000000u);

    guac_v16u8 idx;
    memcpy(&idx, &indices, sizeof(idx));

    guac_v16u8 chars = __guac_socket_translate_base64_alphabet(idx);
    memcpy(dst, &chars, sizeof(chars));

}

/**
 * Encodes 12 input bytes at src to 16 base64 characters at dst via the
 * shuffle-based path. Safe to call when only exactly 12 bytes past src
 * are readable - the load is a two-part 8+4 byte memcpy that never
 * reads past the 12th byte. Used for the final 12..15 byte iteration
 * of the SSSE3 fast loop in guac_socket_write_base64; bulk iterations
 * issue a single 16-byte load inline rather than go through this
 * helper.
 */
#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("ssse3")))
#endif
static inline void __guac_socket_encode_base64_12_shuffle(
        const unsigned char* src, char* dst) {

    guac_v16i8 v;
    memcpy(&v, src, 12);
    __guac_socket_transform_base64_12_shuffle(v, dst);

}
#endif

static void* __guac_socket_keep_alive_thread(void* data) {

    int old_cancelstate;

    /* Socket keep-alive loop */
    guac_socket* socket = (guac_socket*) data;
    while (socket->state == GUAC_SOCKET_OPEN) {

        /* Send NOP keep-alive if it's been a while since the last output */
        guac_timestamp timestamp = guac_timestamp_current();
        if (timestamp - socket->last_write_timestamp >
                GUAC_SOCKET_KEEP_ALIVE_INTERVAL) {

            /* Send NOP */
            if (guac_protocol_send_nop(socket)
                || guac_socket_flush(socket))
                break;

        }

        /* Calculate sleep interval every loop as nanosleep updates it
           with the remaining time interval */
        struct timespec interval;
        interval.tv_sec  =  GUAC_SOCKET_KEEP_ALIVE_INTERVAL / 1000;
        interval.tv_nsec = (GUAC_SOCKET_KEEP_ALIVE_INTERVAL % 1000) * 1000000L;

        /* Sleep until next keep-alive check, but allow thread cancellation
         * during that sleep */
        int sleep_result;
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &old_cancelstate);
        GUAC_RETRY_EINTR(sleep_result, nanosleep(&interval, &interval));
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancelstate);

    }

    return NULL;

}

static ssize_t __guac_socket_write(guac_socket* socket,
        const void* buf, size_t count) {

    /* Update timestamp of last write */
    socket->last_write_timestamp = guac_timestamp_current();

    /* If handler defined, call it. */
    ssize_t written;
    if (socket->write_handler)
        written = socket->write_handler(socket, buf, count);
    else
        /* Otherwise, pretend everything was written. */
        written = count;

    /* Accumulate throughput-measurement counter. The increment runs under
     * whatever serialization already guards writes to this socket (the
     * instruction lock for concurrent writers, single-threaded elsewhere),
     * so a plain load-modify-store suffices here. Readers use
     * __atomic_load_n() in guac_socket_bytes_written() for cross-thread
     * visibility without contending on a lock. */
    if (written > 0)
        __atomic_store_n(&socket->total_bytes_written,
                socket->total_bytes_written + (size_t) written,
                __ATOMIC_RELAXED);

    return written;

}

ssize_t guac_socket_write(guac_socket* socket,
        const void* buf, size_t count) {

    const char* buffer = buf;

    /* Write until completely written */
    while (count > 0) {

        /* Attempt to write, return on error */
        int written = __guac_socket_write(socket, buffer, count);
        if (written == -1)
            return 1;

        /* Advance buffer as data written */
        buffer += written;
        count  -= written;

    }

    return 0;

}

ssize_t guac_socket_read(guac_socket* socket, void* buf, size_t count) {

    /* If handler defined, call it. */
    if (socket->read_handler)
        return socket->read_handler(socket, buf, count);

    /* Otherwise, pretend nothing was read. */
    return 0;

}

int guac_socket_select(guac_socket* socket, int usec_timeout) {

    /* Call select handler if defined */
    if (socket->select_handler)
        return socket->select_handler(socket, usec_timeout);

    /* Otherwise, assume ready. */
    return 1;

}

guac_socket* guac_socket_alloc(void) {

    guac_socket* socket = guac_mem_alloc(sizeof(guac_socket));

    /* If no memory available, return with error */
    if (socket == NULL) {
        guac_error = GUAC_STATUS_NO_MEMORY;
        guac_error_message = "Could not allocate memory for socket";
        return NULL;
    }

    socket->__ready = 0;
    socket->__encoded = 0;
    socket->data = NULL;
    socket->state = GUAC_SOCKET_OPEN;
    socket->last_write_timestamp = guac_timestamp_current();
    socket->total_bytes_written = 0;

    /* No keep alive ping by default */
    socket->__keep_alive_enabled = 0;

    /* No handlers yet */
    socket->read_handler   = NULL;
    socket->write_handler  = NULL;
    socket->select_handler = NULL;
    socket->free_handler   = NULL;
    socket->flush_handler  = NULL;
    socket->lock_handler   = NULL;
    socket->unlock_handler = NULL;

    return socket;

}

void guac_socket_require_keep_alive(guac_socket* socket) {

    /* Start keep-alive thread */
    socket->__keep_alive_enabled = 1;
    pthread_create(&(socket->__keep_alive_thread), NULL,
                __guac_socket_keep_alive_thread, (void*) socket);

}

size_t guac_socket_bytes_written(guac_socket* socket) {
    return __atomic_load_n(&socket->total_bytes_written, __ATOMIC_RELAXED);
}

void guac_socket_instruction_begin(guac_socket* socket) {

    /* Call instruction begin handler if defined */
    if (socket->lock_handler)
        socket->lock_handler(socket);

}

void guac_socket_instruction_end(guac_socket* socket) {

    /* Call instruction end handler if defined */
    if (socket->unlock_handler)
        socket->unlock_handler(socket);

}

void guac_socket_free(guac_socket* socket) {

    guac_socket_flush(socket);

    /* Call free handler if defined */
    if (socket->free_handler)
        socket->free_handler(socket);

    /* Mark as closed */
    socket->state = GUAC_SOCKET_CLOSED;

    /* Stop keep-alive thread, if enabled */
    if (socket->__keep_alive_enabled) {
        pthread_cancel(socket->__keep_alive_thread);
        pthread_join(socket->__keep_alive_thread, NULL);
    }

    guac_mem_free(socket);
}

ssize_t guac_socket_write_int(guac_socket* socket, int64_t i) {

    char buffer[128];
    int length;

    /* Write provided integer as a string */
    length = snprintf(buffer, sizeof(buffer), "%"PRIi64, i);
    return guac_socket_write(socket, buffer, length);

}

ssize_t guac_socket_write_string(guac_socket* socket, const char* str) {

    /* Write contents of string */
    if (guac_socket_write(socket, str, strlen(str)))
        return 1;

    return 0;

}

/**
 * Encodes one to three bytes of data as four characters in base64 encoding.
 *
 * This function takes int arguments even though it's working with bytes to
 * allow -1 to be used as distinct sentinel value for missing data.
 *
 * @param a An int holding the first byte of data to encode. Only the
 *          least-significant byte will be used. This will always be inserted
 *          into the output.
 *
 * @param b An int holding the second byte of data to encode. Only the
 *          least-significant byte will be used. If this is less than zero,
 *          the second and third bytes will be ignored and the last two
 *          characters of the output will be '=' padding characters.
 *
 * @param c An int holding the third byte of data to encode. Only the
 *          least-significant byte will be used. If this is less than zero,
 *          the third byte will be ignored and the last character of the
 *          output will be a '=' padding character.
 *
 * @param output Character buffer to hold the output. Exactly four characters
 *               will be written to the buffer starting at this location.
 *
 * @return Returns zero.
 */
ssize_t __guac_socket_encode_base64(int a, int b, int c, char* output) {

    /* Byte 0:[AAAAAA] AABBBB BBBBCC CCCCCC */
    output[0] = __guac_socket_BASE64_CHARACTERS[(a & 0xFC) >> 2];

    if (b >= 0) {

        /* Byte 1: AAAAAA [AABBBB] BBBBCC CCCCCC */
        output[1] = __guac_socket_BASE64_CHARACTERS[((a & 0x03) << 4) | ((b & 0xF0) >> 4)];

        /*
         * Bytes 2 and 3, zero characters of padding:
         *
         * AAAAAA  AABBBB [BBBBCC] CCCCCC
         * AAAAAA  AABBBB  BBBBCC [CCCCCC]
         */
        if (c >= 0) {
            output[2] = __guac_socket_BASE64_CHARACTERS[((b & 0x0F) << 2) | ((c & 0xC0) >> 6)];
            output[3] = __guac_socket_BASE64_CHARACTERS[c & 0x3F];
        }

        /*
         * Bytes 2 and 3, one character of padding:
         *
         * AAAAAA  AABBBB [BBBB--] ------
         * AAAAAA  AABBBB  BBBB-- [------]
         */
        else {
            output[2] = __guac_socket_BASE64_CHARACTERS[((b & 0x0F) << 2)];
            output[3] = '=';
        }
    }

    /*
     * Bytes 1, 2, and 3, two characters of padding:
     *
     * AAAAAA [AA----] ------  ------
     * AAAAAA  AA---- [------] ------
     * AAAAAA  AA----  ------ [------]
     */
    else {
        output[1] = __guac_socket_BASE64_CHARACTERS[((a & 0x03) << 4)];
        output[2] = '=';
        output[3] = '=';
    }

    return 0;
}

/**
 * Flushes any complete base64 characters currently held in the socket's
 * __encoded_buf out to the underlying socket via guac_socket_write(),
 * then resets __encoded to 0.
 *
 * @param socket
 *     The socket whose encoded-output buffer should be drained.
 *
 * @return
 *     Zero on success, non-zero if guac_socket_write() fails.
 */
static ssize_t __guac_socket_flush_encoded(guac_socket* socket) {

    if (socket->__encoded == 0)
        return 0;

    ssize_t retval = guac_socket_write(socket,
            socket->__encoded_buf, socket->__encoded);
    socket->__encoded = 0;
    return retval;

}

/**
 * Appends a block of base64 characters to the socket's __encoded_buf,
 * flushing to the underlying socket first if the new data would exceed
 * the buffer's capacity. The caller-supplied len must not exceed the
 * buffer capacity (and in practice is always 4 or 16 here).
 *
 * @param socket
 *     The socket to append to.
 *
 * @param data
 *     The base64 characters to append.
 *
 * @param len
 *     The number of characters to append.
 *
 * @return
 *     Zero on success, non-zero if a required flush to the socket fails.
 */
static ssize_t __guac_socket_append_encoded(guac_socket* socket,
        const char* data, int len) {

    if (socket->__encoded + len > GUAC_SOCKET_BASE64_ENCODED_BUFFER_SIZE) {
        ssize_t retval = __guac_socket_flush_encoded(socket);
        if (retval)
            return retval;
    }

    memcpy(socket->__encoded_buf + socket->__encoded, data, len);
    socket->__encoded += len;
    return 0;

}

ssize_t guac_socket_flush_base64(guac_socket* socket) {

    /* Encode any held 1- or 2-byte straggler as a padded quartet, then
     * drain the entire accumulated encoded buffer to the socket. After
     * this call __ready is 0 and __encoded is 0; the next write starts
     * from a clean state. */
    if (socket->__ready > 0) {

        char out[4];
        if (socket->__ready == 2)
            __guac_socket_encode_base64(
                    socket->__ready_buf[0],
                    socket->__ready_buf[1], -1, out);
        else
            __guac_socket_encode_base64(
                    socket->__ready_buf[0], -1, -1, out);

        ssize_t retval = __guac_socket_append_encoded(socket, out, 4);
        if (retval)
            return retval;

        socket->__ready = 0;

    }

    return __guac_socket_flush_encoded(socket);

}

ssize_t guac_socket_write_base64(guac_socket* socket,
        const void* buf, size_t count) {

    const unsigned char* src = (const unsigned char*) buf;
    ssize_t retval;

    /* If a partial 1- or 2-byte group is held from a previous call,
     * consume just enough bytes from the caller's buffer to complete a
     * full 3-byte group and encode it. If the caller doesn't have
     * enough, absorb what's available into the held tail and return
     * without encoding - nothing can be emitted yet. */
    if (socket->__ready > 0) {

        size_t need = 3 - socket->__ready;
        if (count < need) {
            memcpy(socket->__ready_buf + socket->__ready, src, count);
            socket->__ready += count;
            return 0;
        }

        memcpy(socket->__ready_buf + socket->__ready, src, need);
        src += need;
        count -= need;

        char out[4];
        __guac_socket_encode_base64(
                socket->__ready_buf[0],
                socket->__ready_buf[1],
                socket->__ready_buf[2], out);
        retval = __guac_socket_append_encoded(socket, out, 4);
        if (retval)
            return retval;
        socket->__ready = 0;

    }

    /* Bulk path: encode 12 input bytes -> 16 base64 chars per iteration
     * straight from the caller's buffer into __encoded_buf. Flush to the
     * socket whenever __encoded_buf can't fit another 16-char chunk,
     * preserving the batching behavior of the pre-refactor design
     * without copying through __ready_buf first. When vector extensions
     * aren't available at build time, both loops are skipped and the
     * scalar 3-byte loop below handles the entire payload.
     *
     * Two shapes of bulk loop are emitted, selected at runtime:
     *
     *   - The "shuffle" loop uses PSHUFB (x86 SSSE3+) or TBL (AArch64
     *     NEON) to spread 12 input bytes across a 16-byte vector, then
     *     extracts four 6-bit indices per source triplet with four
     *     masked 32-bit SIMD shifts. Gated at runtime on
     *     __builtin_cpu_supports("ssse3") on x86 so that a baseline-
     *     x86-64 build (the Autoconf default) still lights up the fast
     *     path on any post-2008 CPU. The transform functions carry a
     *     target("ssse3") attribute so the compiler emits PSHUFB even
     *     when the translation unit itself was compiled for SSE2.
     *
     *   - The "hybrid" loop does scalar bit extraction into a 16-byte
     *     array, then runs the SSE2-baseline alphabet-translation
     *     vector op. Used on strict SSE2 CPUs (runtime-detected) and
     *     on every non-x86-non-NEON target. Slower than the shuffle
     *     loop but faster than scalarized-shuffle, because on true
     *     SSE2 __builtin_shuffle would expand to 16 byte moves via
     *     the stack.
     *
     * The fast loop issues a single 16-byte movdqu per chunk; a final
     * iteration for any remaining 12..15 bytes uses the (slower) 12-
     * byte memcpy load. Confining the slow load to at most one
     * iteration amortizes it to zero in the typical large-write case. */
#ifdef HAVE_VECTOR_EXTENSIONS

    /* Decide once whether the shuffle-based fast path is usable on
     * this CPU. On ARM with NEON the shuffle always lowers to TBL and
     * is unconditionally available; on x86 we consult the CPU feature
     * table populated by glibc at load time. Cached in a local so the
     * check runs once per guac_socket_write_base64 call rather than
     * once per encoded chunk. */
    int guac_socket_use_shuffle;
#if defined(__x86_64__) || defined(__i386__)
    guac_socket_use_shuffle = __builtin_cpu_supports("ssse3");
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    guac_socket_use_shuffle = 1;
#else
    guac_socket_use_shuffle = 0;
#endif

    if (guac_socket_use_shuffle) {

        while (count >= 16) {

            if (socket->__encoded + 16 > GUAC_SOCKET_BASE64_ENCODED_BUFFER_SIZE) {
                retval = __guac_socket_flush_encoded(socket);
                if (retval)
                    return retval;
            }

            guac_v16i8 v;
            memcpy(&v, src, sizeof(v));
            __guac_socket_transform_base64_12_shuffle(v,
                    socket->__encoded_buf + socket->__encoded);

            socket->__encoded += 16;
            src += 12;
            count -= 12;

        }

        if (count >= 12) {

            if (socket->__encoded + 16 > GUAC_SOCKET_BASE64_ENCODED_BUFFER_SIZE) {
                retval = __guac_socket_flush_encoded(socket);
                if (retval)
                    return retval;
            }

            __guac_socket_encode_base64_12_shuffle(src,
                    socket->__encoded_buf + socket->__encoded);
            socket->__encoded += 16;
            src += 12;
            count -= 12;

        }

    }
    else {

        while (count >= 12) {

            if (socket->__encoded + 16 > GUAC_SOCKET_BASE64_ENCODED_BUFFER_SIZE) {
                retval = __guac_socket_flush_encoded(socket);
                if (retval)
                    return retval;
            }

            __guac_socket_encode_base64_12_hybrid(src,
                    socket->__encoded_buf + socket->__encoded);
            socket->__encoded += 16;
            src += 12;
            count -= 12;

        }

    }
#endif

    /* Scalar tail for whatever the SIMD body didn't cover (0-11 bytes
     * when the SIMD path ran, or the full input in the no-vector-ext
     * build). Only encodes complete 3-byte groups here; 0, 1, or 2
     * trailing bytes are handed to __ready_buf for the next call. */
    while (count >= 3) {

        char out[4];
        __guac_socket_encode_base64(src[0], src[1], src[2], out);
        retval = __guac_socket_append_encoded(socket, out, 4);
        if (retval)
            return retval;

        src += 3;
        count -= 3;

    }

    if (count > 0) {
        memcpy(socket->__ready_buf, src, count);
        socket->__ready = count;
    }

    return 0;

}

ssize_t guac_socket_flush(guac_socket* socket) {

    /* If handler defined, call it. */
    if (socket->flush_handler)
        return socket->flush_handler(socket);

    /* Otherwise, do nothing */
    return 0;

}
