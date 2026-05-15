/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#ifndef RAY_SYM_H
#define RAY_SYM_H

/*
 * sym.h -- Global symbol intern table.
 *
 * Sequential mode: simple hash map + array. wyhash (truncated to 32-bit),
 * open addressing with linear probing. Stores (hash32 << 32) | (id + 1)
 * so that 0 means empty bucket.
 */

#include <rayforce.h>
#include "core/types.h"
#include <stdbool.h>

/* Symbol width encoding (lower 2 bits of attrs when type == RAY_SYM).
 * RAY_SYM_W{8,16,32,64} are now declared in <rayforce.h> for embedders. */
#define RAY_SYM_W_MASK   0x03
#ifndef RAY_SYM_W8
#define RAY_SYM_W8       0x00
#define RAY_SYM_W16      0x01
#define RAY_SYM_W32      0x02
#define RAY_SYM_W64      0x03
#endif

/* Helper macros */
#define RAY_IS_SYM(t)         ((t) == RAY_SYM)
#define RAY_SYM_ELEM(attrs)   (1u << ((attrs) & RAY_SYM_W_MASK))  /* 1,2,4,8 */

/* Determine optimal SYM width for a given dictionary size */
static inline uint8_t ray_sym_dict_width(int64_t dict_size) {
    if (dict_size <= 255)        return RAY_SYM_W8;
    if (dict_size <= 65535)      return RAY_SYM_W16;
    if (dict_size <= 4294967295) return RAY_SYM_W32;
    return RAY_SYM_W64;
}

/* SYM-aware element size: returns adaptive width for RAY_SYM columns */
static inline uint8_t ray_sym_elem_size(int8_t type, uint8_t attrs) {
    if (type == RAY_SYM) return (uint8_t)RAY_SYM_ELEM(attrs);
    return ray_elem_size(type);
}

/* Read a dictionary index from a RAY_SYM column (adaptive width) */
static inline int64_t ray_read_sym(const void* data, int64_t row, int8_t type, uint8_t attrs) {
    (void)type; /* only RAY_SYM now */
    switch (attrs & RAY_SYM_W_MASK) {
        case RAY_SYM_W8:  return ((const uint8_t*)data)[row];
        case RAY_SYM_W16: return ((const uint16_t*)data)[row];
        case RAY_SYM_W32: return ((const uint32_t*)data)[row];
        case RAY_SYM_W64: return ((const int64_t*)data)[row];
    }
    return 0;
}

/* Write a dictionary index into a RAY_SYM column (adaptive width) */
static inline void ray_write_sym(void* data, int64_t row, uint64_t val, int8_t type, uint8_t attrs) {
    (void)type; /* only RAY_SYM now */
    switch (attrs & RAY_SYM_W_MASK) {
        case RAY_SYM_W8:  ((uint8_t*)data)[row]  = (uint8_t)val;  break;
        case RAY_SYM_W16: ((uint16_t*)data)[row] = (uint16_t)val; break;
        case RAY_SYM_W32: ((uint32_t*)data)[row] = (uint32_t)val; break;
        case RAY_SYM_W64: ((int64_t*)data)[row]  = (int64_t)val;  break;
    }
}

/* Intern with pre-computed wyhash, no lock.
 * Caller must guarantee single-threaded access. */
int64_t ray_sym_intern_prehashed(uint32_t hash, const char* str, size_t len);

/* ---- Dotted name resolution (namespace paths) ---------------------------
 * A symbol whose name contains one or more '.' is a *dotted* sym.  At intern
 * time we memchr once, split the name on '.', intern each segment, and cache
 * the resulting segment sym_ids.  `ray_sym_is_dotted` is cheap (one bitmap
 * load) and gates the slow path in env lookup/set. */
bool ray_sym_is_dotted(int64_t sym_id);

/* Returns segment count (>=2 if dotted, 0 otherwise).  *out_segs is set to
 * an interned sym_id array of length `nsegs` (valid for the lifetime of the
 * sym table). */
int ray_sym_segs(int64_t sym_id, const int64_t** out_segs);

/* Bulk-intern variant that does NOT sub-intern segments.  Used only by
 * persistence paths (ray_sym_load, ray_sym_save merge phase) where the
 * disk-position==sym_id invariant would be broken by segment sub-interning
 * appending entries mid-sequence.  Callers MUST follow a batch of these
 * with ray_sym_rebuild_segments to populate the dotted cache. */
int64_t ray_sym_intern_no_split(const char* str, size_t len);
int64_t ray_sym_intern_no_split_unlocked(const char* str, size_t len);

/* Walk the intern table and cache segment sym_ids for any dotted name
 * that hasn't been cached yet.  Idempotent — safe to call multiple times.
 * Needed after bulk loads that used ray_sym_intern_no_split.  Returns
 * RAY_ERR_OOM on the first allocation/sub-intern failure so persistence
 * paths can abort instead of leaving dotted names silently un-cached. */
ray_err_t ray_sym_rebuild_segments(void);

/* Number of symbols loaded from or saved to the current on-disk dictionary.
 * Runtime-only interned symbols may exist above this prefix. */
uint32_t ray_sym_persisted_count(void);

/* Save the same on-disk symbol format as ray_sym_save, but skip durability
 * syncs.  Intended for generated bulk-import caches where throughput matters
 * more than crash recovery of a half-written target. */
ray_err_t ray_sym_save_bulk(const char* path);

/* Upper bound on the arena bytes that sym_str_arena consumes for a name
 * of the given length.  Used by the three-phase atomic intern to pre-
 * reserve arena capacity, so the commit phase cannot fail partway.
 *
 * Short path (<7 bytes): ray_arena_alloc(_, 0) charges ARENA_ALIGN_UP(32)
 * = 32 bytes.  Long path: sym_str_arena computes chars_block =
 * ALIGN(32 + len + 1) and calls ray_arena_alloc(_, chars_block), which
 * charges ARENA_ALIGN_UP(32 + chars_block) = 32 + chars_block because
 * chars_block is 32-aligned.  The +32 term is the crucial one — omitting
 * it under-reserves by exactly 32 bytes per long sym.
 *
 * Exposed as inline so tests can verify the bound against actual arena
 * consumption for every length in a range. */
static inline size_t ray_sym_bytes_upper(size_t len) {
    if (len < 7) return 32;
    size_t chars_block = ((size_t)32 + len + 1 + 31) & ~(size_t)31;
    return 32 + chars_block;
}

#endif /* RAY_SYM_H */
