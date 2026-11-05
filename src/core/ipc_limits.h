/* SPDX-License-Identifier: MIT */
#ifndef RAY_IPC_LIMITS_H
#define RAY_IPC_LIMITS_H

#include "store/serde.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Protocol body and decoded-body budget. Compression does not raise it. */
#define RAY_IPC_BODY_MAX_BYTES (256LL * 1024LL * 1024LL)
/* Separate per-frame queue budget, not the configurable backlog limit:
 * the complete frame, including its header.
 * Direct responses obey the body budget, not this stricter queue budget. */
#define RAY_IPC_FRAME_MAX_BYTES (256LL * 1024LL * 1024LL)

static inline bool ray_ipc_body_size_ok(int64_t size) {
    return size > 0 && size <= RAY_IPC_BODY_MAX_BYTES;
}

static inline bool ray_ipc_queue_body_size_ok(size_t size) {
    return size > 0 && size <= (size_t)RAY_IPC_FRAME_MAX_BYTES - sizeof(ray_ipc_header_t);
}

/* A compression-eligible boundary value may fit after compression. The
 * finished wire body must still pass ray_ipc_queue_body_size_ok. */
static inline bool ray_ipc_queue_size_preflight(int64_t size, size_t threshold) {
    if (!ray_ipc_body_size_ok(size)) return false;
    return (size_t)size > threshold || ray_ipc_queue_body_size_ok((size_t)size);
}

#endif
