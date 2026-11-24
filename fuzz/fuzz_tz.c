/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Fuzz target: the TZif time zone file parser (src/core/tz.c).
 *
 *   The files it reads in production are the ones built into the binary,
 *   but ray_tz_parse takes any buffer, so it is held to the usual standard:
 *   a malformed file is refused, never read out of bounds.  A file that
 *   parses must answer a table whose changes ascend, which every lookup
 *   relies on, and a lookup anywhere in the TIMESTAMP range must land
 *   inside the table.
 */

#include "common.h"
#include "core/tz.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    ray_fuzz_init();   /* the table is allocated from the runtime's heap */

    (void)ray_tz_name_ok((const char*)data, size);

    ray_tz_t* z = ray_tz_parse(data, size);
    if (!z) return 0;
    for (int32_t i = 1; i < z->n; i++)
        if (z->at[i - 1] >= z->at[i]) __builtin_trap();
    volatile int64_t sink = z->off[ray_tz_find(z, INT64_MIN)] + z->off[ray_tz_find(z, INT64_MAX)];
    for (int32_t i = 0; i < z->n; i++) sink += z->off[ray_tz_find(z, z->at[i])];
    (void)sink;
    ray_tz_free(z);
    return 0;
}
