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

#ifndef _GUAC_USER_CONSTANTS_H
#define _GUAC_USER_CONSTANTS_H

/**
 * Constants related to the Guacamole user structure, guac_user.
 *
 * @file user-constants.h
 */

/**
 * The character prefix which identifies a user ID.
 */
#define GUAC_USER_ID_PREFIX '@'

/**
 * The maximum number of inbound or outbound streams supported by any one
 * guac_user.
 */
#define GUAC_USER_MAX_STREAMS 512

/**
 * The index of a closed stream.
 */
#define GUAC_USER_CLOSED_STREAM_INDEX -1

/**
 * The maximum number of objects supported by any one guac_client.
 */
#define GUAC_USER_MAX_OBJECTS 64

/**
 * The index of an object which has not been defined.
 */
#define GUAC_USER_UNDEFINED_OBJECT_INDEX -1

/**
 * The stream name reserved for the root of a Guacamole protocol object.
 */
#define GUAC_USER_OBJECT_ROOT_NAME "/"

/**
 * The mimetype of a stream containing a map of available stream names to their
 * corresponding mimetypes. The root of a Guacamole protocol object is
 * guaranteed to have this type.
 */
#define GUAC_USER_STREAM_INDEX_MIMETYPE "application/vnd.glyptodon.guacamole.stream-index+json"

/**
 * Maximum number of outstanding (sent but not yet acknowledged) server
 * syncs tracked per-user for throughput-calculation purposes. Each
 * emitted sync adds a record carrying the cumulative byte count on
 * the user's socket at emit time; matching sync acks look the record
 * up by timestamp to anchor the throughput sample's upper byte bound.
 *
 * MUST be a power of two - the history is a ring buffer whose
 * physical slot is derived from a monotonic index via bitmask-and
 * with GUAC_USER_SYNC_EMIT_HISTORY_MASK.
 *
 * Sized for ~1 second of in-flight syncs at a 60 Hz frame rate with a
 * generous margin - enough to cover any realistic RTT plus client
 * processing delay without losing samples. When the history is full a
 * newly-emitted sync displaces the oldest un-acked entry; acks that
 * would have matched the displaced entry simply skip the throughput
 * sample (one bad or missing sample is strictly better than a
 * hang or an incorrect calculation).
 */
#define GUAC_USER_SYNC_EMIT_HISTORY_SIZE 64

/**
 * Bitmask that reduces a monotonic
 * guac_user::sync_emit_write_index / sync_emit_read_index value to
 * the corresponding physical slot within
 * guac_user::sync_emit_history. Derived from
 * GUAC_USER_SYNC_EMIT_HISTORY_SIZE, which must be a power of two.
 */
#define GUAC_USER_SYNC_EMIT_HISTORY_MASK (GUAC_USER_SYNC_EMIT_HISTORY_SIZE - 1)

#endif

