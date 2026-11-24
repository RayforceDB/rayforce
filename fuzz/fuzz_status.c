/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Fuzz target: the status-stream frame decoder (pure, no runtime).
 *
 *   A reader of the stream decodes bytes a server sent it.  Whatever the
 *   bytes, the decoder must stay inside them; and a payload it accepts
 *   must encode back to the same bytes (the reserved word aside, which a
 *   server writes as zero), so decode and encode agree on the layout.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/status.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    ray_status_frame_t f;
    if (!ray_status_frame_decode(data, size, &f)) return 0;
    uint8_t out[RAY_STATUS_FRAME_MAX];
    size_t n = ray_status_frame_encode(&f, out);
    if (n != size) abort();
    if (memcmp(out, data, 92) != 0) abort();
    if (memcmp(out + RAY_STATUS_FRAME_FIXED, data + RAY_STATUS_FRAME_FIXED,
               size - RAY_STATUS_FRAME_FIXED) != 0) abort();
    return 0;
}
