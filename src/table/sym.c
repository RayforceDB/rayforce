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

#include "sym.h"
#include "core/platform.h"
#include "store/col.h"
#include "store/fileio.h"
#include "mem/heap.h"
#include "mem/sys.h"
#include "mem/arena.h"
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <errno.h>
#include "ops/hash.h"

/* --------------------------------------------------------------------------
 * Symbol table structure (static global, sequential mode only).
 * NOT thread-safe: all interning must happen before ray_parallel_begin().
 * -------------------------------------------------------------------------- */

#define SYM_INIT_CAP     256
#define SYM_LOAD_FACTOR  0.7

/* Cached segment list for a dotted sym: nsegs sym_ids that together make up
 * the dotted path.  segs is arena-allocated (same lifetime as sym table). */
typedef struct {
    uint8_t  nsegs;
    int64_t* segs;   /* length nsegs; NULL for non-dotted entries */
} sym_segs_t;

typedef struct {
    /* Hash table: each bucket stores (hash32 << 32) | (id + 1), 0 = empty */
    uint64_t*  buckets;
    uint32_t   bucket_cap;   /* always power of 2 */

    /* String array: strings[id] = ray_t* string atom */
    ray_t**     strings;
    uint32_t   str_count;
    uint32_t   str_cap;

    /* Per-sym dotted-path metadata, parallel to strings[].
     * `dotted` is a bitmap (1 bit per sym_id); bit set = name is dotted
     *   and segment sym_ids are cached in `segments`.
     * `scanned` is a bitmap; bit set = sym_cache_segments has settled this
     *   sym (either cached successfully, or decided it is a plain name).
     *   Unset = needs to be (re-)scanned on the next intern call, which is
     *   how we recover from a transient cache OOM on first intern: the
     *   bit stays clear, so future interns of the same name retry.
     * `segments` holds cached segment sym_ids; segs == NULL when dotted
     *   bit is clear. */
    uint64_t*   dotted;       /* (str_cap + 63) / 64 words */
    uint64_t*   scanned;      /* (str_cap + 63) / 64 words */
    sym_segs_t* segments;     /* length str_cap */

    /* Persistence: entries [0..persisted_count-1] are known on disk */
    uint32_t   persisted_count;

    /* Arena for string atoms — avoids per-string buddy allocator calls */
    ray_arena_t*  arena;
} sym_table_t;

static sym_table_t g_sym;
static _Atomic(bool) g_sym_inited = false;

/* Spinlock protecting g_sym mutations in ray_sym_intern */
static _Atomic(int) g_sym_lock = 0;
static inline void sym_lock(void) {
    while (atomic_exchange_explicit(&g_sym_lock, 1, memory_order_acquire)) {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#endif
    }
}
static inline void sym_unlock(void) {
    atomic_store_explicit(&g_sym_lock, 0, memory_order_release);
}

/* Arena-backed ray_str equivalent. Same logic as ray_str() in atom.c
 * but allocates from the sym arena instead of the buddy allocator. */
static ray_t* sym_str_arena(ray_arena_t* arena, const char* s, size_t len) {
    if (len < 7) {
        /* SSO path: inline in header */
        ray_t* v = ray_arena_alloc(arena, 0);
        if (!v) return NULL;
        v->type = -RAY_STR;
        v->slen = (uint8_t)len;
        if (len > 0) memcpy(v->sdata, s, len);
        v->sdata[len] = '\0';
        return v;
    }
    /* Long string: fused single allocation for U8 vector + STR header.
     * Layout: [CHAR ray_t header (32B) | string data (len+1) | padding | STR ray_t header (32B)]
     * This halves arena_alloc calls for long strings. */
    size_t data_size = len + 1;
    size_t chars_block = ((32 + data_size) + 31) & ~(size_t)31;  /* align up to 32 */
    ray_t* chars = ray_arena_alloc(arena, chars_block + 32 - 32);  /* chars_block - 32 (header) + 32 (str header) */
    if (!chars) return NULL;
    chars->type = RAY_U8;
    chars->len = (int64_t)len;
    memcpy(ray_data(chars), s, len);
    ((char*)ray_data(chars))[len] = '\0';

    /* STR header sits right after the CHAR block */
    ray_t* v = (ray_t*)((char*)chars + chars_block);
    memset(v, 0, 32);
    v->attrs = RAY_ATTR_ARENA;
    ray_atomic_store(&v->rc, 1);
    v->type = -RAY_STR;
    v->obj = chars;
    return v;
}

/* --------------------------------------------------------------------------
 * ray_sym_init
 * -------------------------------------------------------------------------- */

ray_err_t ray_sym_init(void) {
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&g_sym_inited, &expected, true,
            memory_order_acq_rel, memory_order_acquire))
        return RAY_OK; /* already initialized by another thread */

    g_sym.bucket_cap = SYM_INIT_CAP;
    /* ray_sys_alloc uses mmap(MAP_ANONYMOUS) which zero-initializes. */
    g_sym.buckets = (uint64_t*)ray_sys_alloc(g_sym.bucket_cap * sizeof(uint64_t));
    if (!g_sym.buckets) {
        atomic_store_explicit(&g_sym_inited, false, memory_order_release);
        return RAY_ERR_OOM;
    }

    g_sym.str_cap = SYM_INIT_CAP;
    g_sym.str_count = 0;
    g_sym.strings = (ray_t**)ray_sys_alloc(g_sym.str_cap * sizeof(ray_t*));
    if (!g_sym.strings) {
        ray_sys_free(g_sym.buckets);
        g_sym.buckets = NULL;
        atomic_store_explicit(&g_sym_inited, false, memory_order_release);
        return RAY_ERR_OOM;
    }

    g_sym.arena = ray_arena_new(1024 * 1024);  /* 1MB chunks */
    if (!g_sym.arena) {
        ray_sys_free(g_sym.strings);
        ray_sys_free(g_sym.buckets);
        g_sym.strings = NULL;
        g_sym.buckets = NULL;
        atomic_store_explicit(&g_sym_inited, false, memory_order_release);
        return RAY_ERR_OOM;
    }

    /* Dotted-path sidecars sized to str_cap.  ray_sys_alloc is MAP_ANONYMOUS
     * so memory is zero-initialised — bitmaps start all-zero, segments[i]
     * structs start {nsegs:0, segs:NULL}.  Failures free prior allocations
     * and roll the sym table back to uninitialised. */
    uint32_t bm_words = (g_sym.str_cap + 63) / 64;
    g_sym.dotted = (uint64_t*)ray_sys_alloc((size_t)bm_words * sizeof(uint64_t));
    g_sym.scanned = (uint64_t*)ray_sys_alloc((size_t)bm_words * sizeof(uint64_t));
    g_sym.segments = (sym_segs_t*)ray_sys_alloc((size_t)g_sym.str_cap * sizeof(sym_segs_t));
    if (!g_sym.dotted || !g_sym.scanned || !g_sym.segments) {
        if (g_sym.dotted) ray_sys_free(g_sym.dotted);
        if (g_sym.scanned) ray_sys_free(g_sym.scanned);
        if (g_sym.segments) ray_sys_free(g_sym.segments);
        g_sym.dotted = NULL;
        g_sym.scanned = NULL;
        g_sym.segments = NULL;
        ray_arena_destroy(g_sym.arena);
        g_sym.arena = NULL;
        ray_sys_free(g_sym.strings);
        ray_sys_free(g_sym.buckets);
        g_sym.strings = NULL;
        g_sym.buckets = NULL;
        atomic_store_explicit(&g_sym_inited, false, memory_order_release);
        return RAY_ERR_OOM;
    }

    /* g_sym_inited already set to true by CAS above */
    return RAY_OK;
}

/* --------------------------------------------------------------------------
 * ray_sym_destroy
 * -------------------------------------------------------------------------- */

void ray_sym_destroy(void) {
    if (!atomic_load_explicit(&g_sym_inited, memory_order_acquire)) return;

    /* Arena-backed strings: ray_release is a no-op (RAY_ATTR_ARENA).
     * Destroy the arena to free all string atoms at once.
     * segments[i].segs pointers are arena-allocated too, freed with it. */
    if (g_sym.arena) {
        ray_arena_destroy(g_sym.arena);
        g_sym.arena = NULL;
    }

    if (g_sym.segments) ray_sys_free(g_sym.segments);
    if (g_sym.scanned)  ray_sys_free(g_sym.scanned);
    if (g_sym.dotted)   ray_sys_free(g_sym.dotted);
    ray_sys_free(g_sym.strings);
    ray_sys_free(g_sym.buckets);

    memset(&g_sym, 0, sizeof(g_sym));
    atomic_store_explicit(&g_sym_inited, false, memory_order_release);
}

/* --------------------------------------------------------------------------
 * Hash table helpers
 * -------------------------------------------------------------------------- */

static void ht_insert(uint64_t* buckets, uint32_t cap, uint32_t hash, uint32_t id) {
    uint32_t mask = cap - 1;
    uint32_t slot = hash & mask;
    uint64_t entry = ((uint64_t)hash << 32) | ((uint64_t)(id + 1));

    for (;;) {
        if (buckets[slot] == 0) {
            buckets[slot] = entry;
            return;
        }
        slot = (slot + 1) & mask;
    }
}

/* Grow hash table to new_cap (must be power of 2 and > current cap). */
static bool ht_grow_to(uint32_t new_cap) {
    uint64_t* new_buckets = (uint64_t*)ray_sys_alloc((size_t)new_cap * sizeof(uint64_t));
    if (!new_buckets) return false;

    /* Re-insert all existing entries */
    for (uint32_t i = 0; i < g_sym.bucket_cap; i++) {
        uint64_t e = g_sym.buckets[i];
        if (e == 0) continue;
        uint32_t h = (uint32_t)(e >> 32);
        uint32_t id = (uint32_t)(e & 0xFFFFFFFF) - 1;
        ht_insert(new_buckets, new_cap, h, id);
    }

    ray_sys_free(g_sym.buckets);
    g_sym.buckets = new_buckets;
    g_sym.bucket_cap = new_cap;
    return true;
}

static bool ht_grow(void) {
    /* Overflow guard: bucket_cap is always power of 2.
     * At 2^31, doubling overflows uint32_t. */
    if (g_sym.bucket_cap >= (UINT32_MAX / 2 + 1)) return false;
    return ht_grow_to(g_sym.bucket_cap * 2);
}

/* --------------------------------------------------------------------------
 * sym_grow_str_cap — grow strings[], dotted[] bitmap, and segments[] array
 * to hold at least new_cap entries.  Must be called with sym_lock held
 * (or from within single-threaded prehashed intern).  Zero-fills the new
 * portion of segments[] explicitly (realloc of a mapped region may return
 * pages that weren't touched but we don't want to rely on virgin mmap).
 * -------------------------------------------------------------------------- */
static bool sym_grow_str_cap(uint32_t new_cap) {
    uint32_t old_cap = g_sym.str_cap;
    if (new_cap <= old_cap) return true;

    ray_t** new_strings = (ray_t**)ray_sys_realloc(g_sym.strings,
                                                   (size_t)new_cap * sizeof(ray_t*));
    if (!new_strings) return false;
    g_sym.strings = new_strings;

    uint32_t old_bm_words = (old_cap + 63) / 64;
    uint32_t new_bm_words = (new_cap + 63) / 64;
    if (new_bm_words > old_bm_words) {
        uint64_t* new_dotted = (uint64_t*)ray_sys_realloc(g_sym.dotted,
                                                          (size_t)new_bm_words * sizeof(uint64_t));
        if (!new_dotted) return false;
        memset(new_dotted + old_bm_words, 0,
               (size_t)(new_bm_words - old_bm_words) * sizeof(uint64_t));
        g_sym.dotted = new_dotted;

        uint64_t* new_scanned = (uint64_t*)ray_sys_realloc(g_sym.scanned,
                                                           (size_t)new_bm_words * sizeof(uint64_t));
        if (!new_scanned) return false;
        memset(new_scanned + old_bm_words, 0,
               (size_t)(new_bm_words - old_bm_words) * sizeof(uint64_t));
        g_sym.scanned = new_scanned;
    }

    sym_segs_t* new_segments = (sym_segs_t*)ray_sys_realloc(g_sym.segments,
                                                            (size_t)new_cap * sizeof(sym_segs_t));
    if (!new_segments) return false;
    memset(new_segments + old_cap, 0,
           (size_t)(new_cap - old_cap) * sizeof(sym_segs_t));
    g_sym.segments = new_segments;

    g_sym.str_cap = new_cap;
    return true;
}

/* Forward declarations — sym_cache_segments (below) needs these helpers
 * that are defined further down in the file.  ray_sym_bytes_upper is
 * declared in sym.h as a public inline so both the intern path and the
 * test suite can refer to the same formula. */
static int64_t sym_intern_nolock(uint32_t hash, const char* str, size_t len);
static int64_t sym_probe(uint32_t hash, const char* str, size_t len);
static int64_t sym_commit_new(uint32_t hash, const char* str, size_t len);
static bool    sym_reserve_capacity(uint32_t new_sym_count, size_t arena_bytes);

/* --------------------------------------------------------------------------
 * sym_cache_segments — idempotent cache-and-apply for an EXISTING sym.
 * Used by ray_sym_rebuild_segments (after bulk persistence loads) and by
 * the probe-found branch of sym_intern_nolock (a prior intern via
 * ray_sym_intern_no_split may have committed the sym without ever
 * running the cache prep).
 *
 * Atomic: same inspect + reserve + commit pattern as sym_intern_nolock,
 * so a failure here leaves no orphan segment syms and no half-applied
 * cache state.  Returns false only on real OOM — scanned stays clear
 * in that case so future retries pick up where we left off.
 * -------------------------------------------------------------------------- */
static bool sym_cache_segments(uint32_t new_id, const char* str, size_t len) {
    uint64_t bit = (uint64_t)1 << (new_id & 63);
    uint32_t word = new_id >> 6;
    if (g_sym.scanned[word] & bit) return true;

    const char* first_dot = (const char*)memchr(str, '.', len);
    if (!first_dot) {
        /* Plain — mark settled. */
        g_sym.scanned[word] |= bit;
        return true;
    }

    /* Validate structure.  Trailing dot → not dotted.  Leading `.` is
     * allowed ONLY when followed by another dot (e.g. `.sys.gc`) —
     * in that case segment 0 includes the leading dot (`.sys`), so
     * reserved-namespace names resolve against their root dict via
     * the regular segment walk. */
    if (str[len - 1] == '.') {
        g_sym.scanned[word] |= bit;
        return true;
    }
    bool leading_dot = (str[0] == '.');
    if (leading_dot) {
        /* `.sys` alone (no second dot) is a plain name. */
        const char* second = (const char*)memchr(str + 1, '.', len - 1);
        if (!second) { g_sym.scanned[word] |= bit; return true; }
    }
    size_t sep_dots = 0;
    for (size_t i = (leading_dot ? 1 : 0); i < len; i++)
        if (str[i] == '.') sep_dots++;
    if (sep_dots + 1 > 255) {
        g_sym.scanned[word] |= bit;
        return true;
    }
    uint8_t nsegs = (uint8_t)(sep_dots + 1);

    struct { const char* p; size_t len; uint32_t hash; int64_t id; } descs[256];
    uint32_t new_seg_count = 0;
    size_t   new_seg_bytes = 0;
    {
        const char* p = str;
        size_t remaining = len;
        uint8_t i = 0;
        while (remaining && i < nsegs) {
            /* Segment 0 starts at str[0] but skips the leading `.` when
             * searching for the segment-terminating dot — so seg 0 of
             * `.sys.gc` is `.sys`, not `` (empty). */
            size_t skip = (i == 0 && leading_dot) ? 1 : 0;
            const char* dot = remaining > skip
                ? (const char*)memchr(p + skip, '.', remaining - skip)
                : NULL;
            size_t seg_len = dot ? (size_t)(dot - p) : remaining;
            if (seg_len == 0) { g_sym.scanned[word] |= bit; return true; }
            uint32_t h = (uint32_t)ray_hash_bytes(p, seg_len);
            descs[i].p    = p;
            descs[i].len  = seg_len;
            descs[i].hash = h;
            descs[i].id   = sym_probe(h, p, seg_len);
            if (descs[i].id < 0) {
                new_seg_count++;
                new_seg_bytes += ray_sym_bytes_upper(seg_len);
            }
            i++;
            if (!dot) break;
            remaining -= (seg_len + 1);
            p = dot + 1;
        }
    }

    /* Reserve capacity for new segments + segs array. */
    size_t segs_payload = (size_t)nsegs * sizeof(int64_t);
    size_t arena_bytes  = new_seg_bytes +
                          (((size_t)32 + segs_payload + 31) & ~(size_t)31);
    if (!sym_reserve_capacity(new_seg_count, arena_bytes)) return false;

    /* Commit.  Allocations covered by reservation above. */
    for (uint8_t i = 0; i < nsegs; i++) {
        if (descs[i].id < 0) {
            int64_t sid = sym_commit_new(descs[i].hash, descs[i].p, descs[i].len);
            if (sid < 0) return false;   /* reservation should have prevented */
            descs[i].id = sid;
            g_sym.scanned[sid >> 6] |= ((uint64_t)1 << (sid & 63));
        }
    }

    int64_t* segs = (int64_t*)ray_arena_alloc(g_sym.arena, segs_payload);
    if (!segs) return false;             /* reservation should have prevented */
    for (uint8_t i = 0; i < nsegs; i++) segs[i] = descs[i].id;

    g_sym.segments[new_id].nsegs = nsegs;
    g_sym.segments[new_id].segs  = segs;
    g_sym.dotted[word]  |= bit;
    g_sym.scanned[word] |= bit;
    return true;
}

/* --------------------------------------------------------------------------
 * sym_probe — hash-table lookup only.  Returns sym_id for an existing
 * entry or -1 if not present.  No side effects.
 * -------------------------------------------------------------------------- */
static int64_t sym_probe(uint32_t hash, const char* str, size_t len) {
    uint32_t mask = g_sym.bucket_cap - 1;
    uint32_t slot = hash & mask;
    for (;;) {
        uint64_t e = g_sym.buckets[slot];
        if (e == 0) return -1;
        uint32_t e_hash = (uint32_t)(e >> 32);
        if (e_hash == hash) {
            uint32_t e_id = (uint32_t)(e & 0xFFFFFFFF) - 1;
            ray_t* existing = g_sym.strings[e_id];
            if (ray_str_len(existing) == len &&
                memcmp(ray_str_ptr(existing), str, len) == 0) {
                return (int64_t)e_id;
            }
        }
        slot = (slot + 1) & mask;
    }
}

/* --------------------------------------------------------------------------
 * sym_commit_new — insert a NEW sym (caller must have confirmed it does
 * not already exist).  Grows the hash/strings tables as needed, allocates
 * the string atom from the arena, inserts into the hash table.  Returns
 * new sym_id or -1 on OOM.  No cache side effect.
 * -------------------------------------------------------------------------- */
static int64_t sym_commit_new(uint32_t hash, const char* str, size_t len) {
    /* Grow hash table if load factor exceeds threshold, or if critically
     * full.  Attempt grow before refusing insert.
     * Cast to uint64_t to prevent overflow when bucket_cap >= 2^26. */
    if ((uint64_t)g_sym.str_count * 100 >= (uint64_t)g_sym.bucket_cap * 70) {
        if (!ht_grow()) {
            /* If critically full even after failed grow, refuse insert
             * to prevent infinite probe loops. */
            if ((uint64_t)g_sym.str_count * 100 >= (uint64_t)g_sym.bucket_cap * 95) {
                return -1;
            }
        }
    }

    uint32_t new_id = g_sym.str_count;

    if (new_id >= g_sym.str_cap) {
        if (g_sym.str_cap >= UINT32_MAX / 2) return -1;
        if (!sym_grow_str_cap(g_sym.str_cap * 2)) return -1;
    }

    /* Create string atom from arena — avoids buddy allocator overhead.
     * Arena blocks have rc=1 and RAY_ATTR_ARENA set. */
    ray_t* s = sym_str_arena(g_sym.arena, str, len);
    if (!s) return -1;
    g_sym.strings[new_id] = s;
    g_sym.str_count++;

    /* Insert into hash table.
     * Note: ht_insert probes from hash & mask to find an empty slot,
     * so it works correctly even if ht_grow changed the bucket array. */
    ht_insert(g_sym.buckets, g_sym.bucket_cap, hash, new_id);

    return (int64_t)new_id;
}

/* --------------------------------------------------------------------------
 * sym_intern_nolock_noseg — intern WITHOUT the segment-caching side
 * effect.  Persistence paths (ray_sym_load, ray_sym_save's merge phase)
 * use this variant because segment sub-interning during load would
 * append new ids mid-sequence and break the disk-position==sym_id
 * invariant.  After the bulk op, call ray_sym_rebuild_segments to
 * populate the dotted bitmap + segments cache.  Assumes caller holds
 * sym_lock (or is in the single-threaded prehashed caller contract).
 * -------------------------------------------------------------------------- */
static int64_t sym_intern_nolock_noseg(uint32_t hash, const char* str, size_t len) {
    int64_t existing = sym_probe(hash, str, len);
    if (existing >= 0) return existing;
    return sym_commit_new(hash, str, len);
}

/* Reserve hash-table, strings-array, and arena capacity for `new_sym_count`
 * new syms plus `arena_bytes` of additional arena usage (for the segs array
 * if we're interning a dotted name).  Returns true on success; on failure
 * returns false with no commits made. */
static bool sym_reserve_capacity(uint32_t new_sym_count, size_t arena_bytes) {
    /* Hash table — grow if adding new_sym_count entries would exceed 70%. */
    uint64_t new_count = (uint64_t)g_sym.str_count + new_sym_count;
    uint32_t target = g_sym.bucket_cap;
    while (new_count * 100 >= (uint64_t)target * 70) {
        if (target >= (UINT32_MAX / 2 + 1)) return false;
        target *= 2;
    }
    if (target > g_sym.bucket_cap) {
        if (!ht_grow_to(target)) return false;
    }

    /* Strings and sidecars. */
    if (new_count > g_sym.str_cap) {
        uint32_t str_target = g_sym.str_cap;
        while (str_target < new_count) {
            if (str_target >= UINT32_MAX / 2) return false;
            str_target *= 2;
        }
        if (!sym_grow_str_cap(str_target)) return false;
    }

    /* Arena — reserve one chunk large enough for every forthcoming alloc. */
    if (arena_bytes && !ray_arena_reserve(g_sym.arena, arena_bytes)) return false;

    return true;
}

/* --------------------------------------------------------------------------
 * sym_intern_nolock — fully atomic intern.
 *
 * Three phases:
 *   A. Inspect: probe the main name, validate its dotted shape, probe
 *      every segment.  No side effects.
 *   B. Reserve: pre-grow hash/strings/arena to accommodate everything
 *      we might need to commit.  Can fail → return -1 with no state
 *      change (no orphan segment syms, no cache fragments).
 *   C. Commit: all allocations in this phase are guaranteed by the
 *      reservations above, so they cannot fail.  Creates any new
 *      segment syms, creates the main sym, fills the segs cache, sets
 *      scanned + dotted bits.
 *
 * This closes two prior traps:
 *  - A committed main sym whose dotted bit disagrees with its name's
 *    structure (env silently routing dotted-path writes/reads through
 *    the flat path).
 *  - Orphan segment syms persisting when the main-sym commit fails.
 *
 * For an existing sym found in phase A, we still opportunistically try
 * the cache — that path is the lazy fallback for ray_sym_intern_no_split,
 * which commits the main sym without a cache on purpose.  A cache-OOM
 * there is tolerated (scanned bit stays clear → future interns retry).
 * -------------------------------------------------------------------------- */
static int64_t sym_intern_nolock(uint32_t hash, const char* str, size_t len) {
    /* Phase A.1: probe main. */
    int64_t existing = sym_probe(hash, str, len);
    if (existing >= 0) {
        (void)sym_cache_segments((uint32_t)existing, str, len);
        return existing;
    }

    /* Phase A.2: structural validation + per-segment probe. */
    struct { const char* p; size_t len; uint32_t hash; int64_t id; } descs[256];
    uint8_t  nsegs = 0;
    uint32_t new_seg_count = 0;
    size_t   new_seg_bytes = 0;
    bool     is_dotted = false;

    const char* first_dot = (const char*)memchr(str, '.', len);
    if (first_dot) {
        /* Dotted-name rules (parallel to sym_cache_segments):
         *   - Trailing dot            → plain (not dotted).
         *   - Leading dot alone       → plain (`.sys` with no inner dot).
         *   - Leading dot + inner dot → segment 0 is `.<head>` including
         *                                the leading dot.  This is how
         *                                reserved-namespace names like
         *                                `.sys.gc` resolve against the
         *                                `.sys` root dict. */
        bool valid = str[len - 1] != '.';
        bool leading_dot = (str[0] == '.');
        if (valid && leading_dot) {
            const char* second = (const char*)memchr(str + 1, '.', len - 1);
            if (!second) valid = false;
        }
        size_t sep_dots = 0;
        if (valid) {
            for (size_t i = (leading_dot ? 1 : 0); i < len; i++)
                if (str[i] == '.') sep_dots++;
            if (sep_dots + 1 > 255) valid = false;
        }
        if (valid) {
            nsegs = (uint8_t)(sep_dots + 1);
            const char* p = str;
            size_t remaining = len;
            uint8_t i = 0;
            while (remaining && i < nsegs) {
                size_t skip = (i == 0 && leading_dot) ? 1 : 0;
                const char* dot = remaining > skip
                    ? (const char*)memchr(p + skip, '.', remaining - skip)
                    : NULL;
                size_t seg_len = dot ? (size_t)(dot - p) : remaining;
                if (seg_len == 0) { valid = false; break; }
                uint32_t seg_hash = (uint32_t)ray_hash_bytes(p, seg_len);
                descs[i].p    = p;
                descs[i].len  = seg_len;
                descs[i].hash = seg_hash;
                descs[i].id   = sym_probe(seg_hash, p, seg_len);
                if (descs[i].id < 0) {
                    new_seg_count++;
                    new_seg_bytes += ray_sym_bytes_upper(seg_len);
                }
                i++;
                if (!dot) break;
                remaining -= (seg_len + 1);
                p = dot + 1;
            }
            if (valid) is_dotted = true;
        }
    }

    /* Phase B: reserve capacity for main + new segments + segs array. */
    size_t arena_bytes = ray_sym_bytes_upper(len);
    if (is_dotted) {
        arena_bytes += new_seg_bytes;
        /* segs array is arena-allocated via ray_arena_alloc(_, nsegs*8). */
        size_t segs_payload = (size_t)nsegs * sizeof(int64_t);
        arena_bytes += ((size_t)32 + segs_payload + 31) & ~(size_t)31;
    }
    if (!sym_reserve_capacity(1 + new_seg_count, arena_bytes)) return -1;

    /* Phase C: commit.  Every allocation below is covered by the
     * reservation above, so nothing here can fail. */
    if (is_dotted) {
        for (uint8_t i = 0; i < nsegs; i++) {
            if (descs[i].id < 0) {
                int64_t sid = sym_commit_new(descs[i].hash, descs[i].p, descs[i].len);
                /* Reservation guarantees success; defensive check kept. */
                if (sid < 0) return -1;
                descs[i].id = sid;
                /* Segment is itself a plain name (no dot inside). */
                g_sym.scanned[sid >> 6] |= ((uint64_t)1 << (sid & 63));
            }
        }
    }

    int64_t main_id = sym_commit_new(hash, str, len);
    if (main_id < 0) return -1;

    if (is_dotted) {
        int64_t* segs = (int64_t*)ray_arena_alloc(g_sym.arena,
                                                  (size_t)nsegs * sizeof(int64_t));
        if (!segs) return main_id;   /* reservation should have prevented this */
        for (uint8_t i = 0; i < nsegs; i++) segs[i] = descs[i].id;
        g_sym.segments[main_id].nsegs = nsegs;
        g_sym.segments[main_id].segs  = segs;
        g_sym.dotted[main_id >> 6] |= ((uint64_t)1 << (main_id & 63));
    }
    g_sym.scanned[main_id >> 6] |= ((uint64_t)1 << (main_id & 63));

    return main_id;
}

/* --------------------------------------------------------------------------
 * ray_sym_intern — locked public API
 * -------------------------------------------------------------------------- */

int64_t ray_sym_intern(const char* str, size_t len) {
    if (!atomic_load_explicit(&g_sym_inited, memory_order_acquire)) return -1;
    uint32_t hash = (uint32_t)ray_hash_bytes(str, len);
    sym_lock();
    int64_t id = sym_intern_nolock(hash, str, len);
    sym_unlock();
    return id;
}

/* --------------------------------------------------------------------------
 * ray_sym_intern_prehashed -- intern with pre-computed hash, no lock.
 *
 * CALLER CONTRACT: must only be called when no other thread is interning
 * (e.g., after ray_pool_dispatch returns during CSV merge).
 * -------------------------------------------------------------------------- */

int64_t ray_sym_intern_prehashed(uint32_t hash, const char* str, size_t len) {
    if (!atomic_load_explicit(&g_sym_inited, memory_order_acquire)) return -1;
    return sym_intern_nolock(hash, str, len);
}

/* --------------------------------------------------------------------------
 * ray_sym_intern_no_split — persistence-only bulk intern
 * -------------------------------------------------------------------------- */

int64_t ray_sym_intern_no_split(const char* str, size_t len) {
    if (!atomic_load_explicit(&g_sym_inited, memory_order_acquire)) return -1;
    uint32_t hash = (uint32_t)ray_hash_bytes(str, len);
    sym_lock();
    int64_t id = sym_intern_nolock_noseg(hash, str, len);
    sym_unlock();
    return id;
}

/* --------------------------------------------------------------------------
 * ray_sym_rebuild_segments — populate dotted cache for any not-yet-cached
 * entries.  Must follow a batch of ray_sym_intern_no_split calls.
 *
 * Propagates the first allocation/sub-intern failure as RAY_ERR_OOM so
 * persistence callers (ray_sym_load / ray_sym_save merge) can abort
 * cleanly rather than silently leaving dotted names un-cached — that
 * would degrade them to flat-sym semantics and break env lookup for any
 * name the user wrote with a '.' in it.
 * -------------------------------------------------------------------------- */

ray_err_t ray_sym_rebuild_segments(void) {
    if (!atomic_load_explicit(&g_sym_inited, memory_order_acquire)) return RAY_ERR_IO;
    sym_lock();
    /* Snapshot upper bound — sym_cache_segments may append segment entries
     * beyond the original range, but those new entries themselves are
     * non-dotted segment names and so produce no further work.  Use the
     * scanned bitmap to skip: anything already settled (plain or dotted)
     * avoids even the memchr inside sym_cache_segments. */
    uint32_t count = g_sym.str_count;
    for (uint32_t i = 0; i < count; i++) {
        if (g_sym.scanned[i >> 6] & ((uint64_t)1 << (i & 63))) continue;
        ray_t* s = g_sym.strings[i];
        if (!s) continue;
        if (!sym_cache_segments(i, ray_str_ptr(s), ray_str_len(s))) {
            sym_unlock();
            return RAY_ERR_OOM;
        }
    }
    sym_unlock();
    return RAY_OK;
}

/* --------------------------------------------------------------------------
 * Dotted-name accessors
 * -------------------------------------------------------------------------- */

bool ray_sym_is_dotted(int64_t sym_id) {
    if (!atomic_load_explicit(&g_sym_inited, memory_order_acquire)) return false;
    if (sym_id < 0 || (uint32_t)sym_id >= g_sym.str_count) return false;
    uint64_t word = g_sym.dotted[(uint32_t)sym_id >> 6];
    return (word >> ((uint32_t)sym_id & 63)) & 1;
}

int ray_sym_segs(int64_t sym_id, const int64_t** out_segs) {
    if (!atomic_load_explicit(&g_sym_inited, memory_order_acquire)) return 0;
    if (sym_id < 0 || (uint32_t)sym_id >= g_sym.str_count) return 0;
    sym_segs_t s = g_sym.segments[sym_id];
    if (s.nsegs == 0 || !s.segs) return 0;
    if (out_segs) *out_segs = s.segs;
    return (int)s.nsegs;
}

/* --------------------------------------------------------------------------
 * ray_sym_find
 * -------------------------------------------------------------------------- */

int64_t ray_sym_find(const char* str, size_t len) {
    if (!atomic_load_explicit(&g_sym_inited, memory_order_acquire)) return -1;

    /* Lock required: concurrent ray_sym_intern may trigger ht_grow which
     * frees and replaces g_sym.buckets -- reading without lock is UAF. */
    sym_lock();

    uint32_t hash = (uint32_t)ray_hash_bytes(str, len);
    uint32_t mask = g_sym.bucket_cap - 1;
    uint32_t slot = hash & mask;

    for (;;) {
        uint64_t e = g_sym.buckets[slot];
        if (e == 0) { sym_unlock(); return -1; }  /* empty -- not found */

        uint32_t e_hash = (uint32_t)(e >> 32);
        if (e_hash == hash) {
            uint32_t e_id = (uint32_t)(e & 0xFFFFFFFF) - 1;
            ray_t* existing = g_sym.strings[e_id];
            if (ray_str_len(existing) == len &&
                memcmp(ray_str_ptr(existing), str, len) == 0) {
                sym_unlock();
                return (int64_t)e_id;
            }
        }
        slot = (slot + 1) & mask;
    }
}

/* --------------------------------------------------------------------------
 * ray_sym_str
 * -------------------------------------------------------------------------- */

/* Returned pointer is valid only while no concurrent ray_sym_intern occurs.
 * Safe during read-only execution phase (after all interning is complete).
 * Caller must not store the pointer across sym table mutations (ht_grow
 * or strings realloc). */
ray_t* ray_sym_str(int64_t id) {
    if (!atomic_load_explicit(&g_sym_inited, memory_order_acquire)) return NULL;

    /* Lock required: concurrent ray_sym_intern may realloc g_sym.strings. */
    sym_lock();
    if (id < 0 || (uint32_t)id >= g_sym.str_count) { sym_unlock(); return NULL; }
    ray_t* s = g_sym.strings[id];
    sym_unlock();
    return s;
}

/* --------------------------------------------------------------------------
 * ray_sym_count
 * -------------------------------------------------------------------------- */

uint32_t ray_sym_count(void) {
    if (!atomic_load_explicit(&g_sym_inited, memory_order_acquire)) return 0;

    /* Lock required: concurrent ray_sym_intern may modify str_count. */
    sym_lock();
    uint32_t count = g_sym.str_count;
    sym_unlock();
    return count;
}

/* --------------------------------------------------------------------------
 * ray_sym_ensure_cap -- pre-grow hash table and strings array
 *
 * Ensures the symbol table can hold at least `needed` total symbols without
 * rehashing.  Call before bulk interning (e.g., CSV merge) to prevent
 * mid-insert OOM that silently drops symbols.
 * -------------------------------------------------------------------------- */

bool ray_sym_ensure_cap(uint32_t needed) {
    if (!atomic_load_explicit(&g_sym_inited, memory_order_acquire)) return false;

    sym_lock();

    /* Grow strings array (and sidecars) if needed */
    while (g_sym.str_cap < needed) {
        if (g_sym.str_cap >= UINT32_MAX / 2) { sym_unlock(); return false; }
        uint32_t new_str_cap = g_sym.str_cap * 2;
        if (new_str_cap < needed) { /* jump directly to needed */
            new_str_cap = needed;
            /* Round up to power of 2 */
            new_str_cap--;
            new_str_cap |= new_str_cap >> 1;
            new_str_cap |= new_str_cap >> 2;
            new_str_cap |= new_str_cap >> 4;
            new_str_cap |= new_str_cap >> 8;
            new_str_cap |= new_str_cap >> 16;
            new_str_cap++;
            if (new_str_cap == 0) { sym_unlock(); return false; }
        }
        if (!sym_grow_str_cap(new_str_cap)) { sym_unlock(); return false; }
    }

    /* Grow hash table so load factor stays below threshold after filling */
    double raw_buckets = (double)needed / SYM_LOAD_FACTOR + 1.0;
    if (raw_buckets > (double)UINT32_MAX) { sym_unlock(); return false; }
    uint32_t needed_buckets = (uint32_t)raw_buckets;
    /* Round up to power of 2 */
    needed_buckets--;
    needed_buckets |= needed_buckets >> 1;
    needed_buckets |= needed_buckets >> 2;
    needed_buckets |= needed_buckets >> 4;
    needed_buckets |= needed_buckets >> 8;
    needed_buckets |= needed_buckets >> 16;
    needed_buckets++;

    if (needed_buckets > g_sym.bucket_cap) {
        if (!ht_grow_to(needed_buckets)) { sym_unlock(); return false; }
    }

    sym_unlock();
    return true;
}

/* --------------------------------------------------------------------------
 * ray_sym_save -- serialize symbol table as RAY_LIST of -RAY_STR
 *
 * Uses ray_col_save (STRL format), file locking for concurrent writers,
 * and fsync + atomic rename for crash safety.  Append-only: skips save
 * when persisted_count == str_count.
 * -------------------------------------------------------------------------- */

ray_err_t ray_sym_save(const char* path) {
    if (!path) return RAY_ERR_IO;
    if (!atomic_load_explicit(&g_sym_inited, memory_order_acquire)) return RAY_ERR_IO;

    /* Quick check: nothing new to persist? */
    sym_lock();
    if (g_sym.persisted_count == g_sym.str_count) {
        sym_unlock();
        return RAY_OK;
    }
    sym_unlock();

    /* Build lock and temp paths */
    char lock_path[1024];
    char tmp_path[1024];
    if (snprintf(lock_path, sizeof(lock_path), "%s.lk", path) >= (int)sizeof(lock_path))
        return RAY_ERR_IO;
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >= (int)sizeof(tmp_path))
        return RAY_ERR_IO;

    /* Acquire cross-process exclusive lock */
    ray_fd_t lock_fd = ray_file_open(lock_path, RAY_OPEN_READ | RAY_OPEN_WRITE | RAY_OPEN_CREATE);
    if (lock_fd == RAY_FD_INVALID) return RAY_ERR_IO;
    ray_err_t err = ray_file_lock_ex(lock_fd);
    if (err != RAY_OK) { ray_file_close(lock_fd); return err; }

    /* If file exists, load and merge (pick up entries from other writers).
     * Distinguish "file not found" (proceed with full save) from real I/O
     * errors (abort to avoid overwriting a file we couldn't read). */
    {
        ray_t* existing = ray_col_load(path);
        if (existing && !RAY_IS_ERR(existing)) {
            if (existing->type != RAY_LIST) {
                ray_release(existing);
                ray_file_unlock(lock_fd);
                ray_file_close(lock_fd);
                return RAY_ERR_CORRUPT;
            }
            /* Intern any new entries from disk (idempotent).
             * Verify each entry's in-memory ID matches its disk position:
             * if a local symbol already occupies a slot that disk expects,
             * the tables have diverged and merging would silently reorder
             * symbol IDs, corrupting previously written RAY_SYM columns. */
            ray_t** slots = (ray_t**)ray_data(existing);
            for (int64_t i = 0; i < existing->len; i++) {
                ray_t* s = slots[i];
                if (!s || RAY_IS_ERR(s) || s->type != -RAY_STR) {
                    ray_release(existing);
                    ray_file_unlock(lock_fd);
                    ray_file_close(lock_fd);
                    return RAY_ERR_CORRUPT;
                }
                /* Use the no-split variant: sub-interning segments mid-loop
                 * would shift subsequent disk positions and spuriously trip
                 * the id==i check below. */
                int64_t id = ray_sym_intern_no_split(ray_str_ptr(s), ray_str_len(s));
                if (id < 0) {
                    ray_release(existing);
                    ray_file_unlock(lock_fd);
                    ray_file_close(lock_fd);
                    return RAY_ERR_OOM;
                }
                if (id != i) {
                    /* Divergent symbol tables: disk position i maps to
                     * in-memory id != i.  A local symbol occupies the
                     * slot, so merging would reorder IDs and corrupt
                     * any RAY_SYM columns written by the other writer. */
                    ray_release(existing);
                    ray_file_unlock(lock_fd);
                    ray_file_close(lock_fd);
                    return RAY_ERR_CORRUPT;
                }
            }
            ray_release(existing);
            /* Populate dotted cache for names just merged in.  An OOM
             * here would leave some loaded dotted names without a segment
             * cache, silently degrading their env-lookup semantics — we
             * must not proceed to write the file as if the merge fully
             * succeeded. */
            ray_err_t rebuild_err = ray_sym_rebuild_segments();
            if (rebuild_err != RAY_OK) {
                ray_file_unlock(lock_fd);
                ray_file_close(lock_fd);
                return rebuild_err;
            }
        } else {
            /* ray_col_load failed — check if the file actually exists.
             * If it does, the failure is a real I/O/corruption error;
             * do not overwrite the file with a potentially incomplete
             * in-memory snapshot. */
            ray_fd_t probe_fd = ray_file_open(path, RAY_OPEN_READ);
            if (probe_fd != RAY_FD_INVALID) {
                /* File exists and is readable but ray_col_load failed —
                 * corruption or format error; do not overwrite. */
                ray_file_close(probe_fd);
                ray_file_unlock(lock_fd);
                ray_file_close(lock_fd);
                return RAY_IS_ERR(existing) ? ray_err_from_obj(existing) : RAY_ERR_IO;
            }
            if (errno != ENOENT) {
                /* File may exist but we can't open it (EACCES, EMFILE,
                 * EIO, etc.) — do not overwrite, report I/O error. */
                ray_file_unlock(lock_fd);
                ray_file_close(lock_fd);
                return RAY_ERR_IO;
            }
            /* File does not exist (ENOENT) — proceed with full save */
        }
    }

    /* Snapshot string pointers under sym_lock, then build list without it.
     * Strings are append-only and never freed, so pointers remain valid. */
    sym_lock();
    uint32_t count = g_sym.str_count;
    size_t snap_sz = count * sizeof(ray_t*);
    ray_t* snap_block = ray_alloc(snap_sz);
    if (!snap_block || RAY_IS_ERR(snap_block)) {
        sym_unlock();
        ray_file_unlock(lock_fd);
        ray_file_close(lock_fd);
        return RAY_ERR_OOM;
    }
    ray_t** snap = (ray_t**)ray_data(snap_block);
    memcpy(snap, g_sym.strings, snap_sz);
    sym_unlock();

    /* Build RAY_LIST of -RAY_STR from snapshot */
    ray_t* list = ray_list_new((int64_t)count);
    if (!list || RAY_IS_ERR(list)) {
        ray_free(snap_block);
        ray_file_unlock(lock_fd);
        ray_file_close(lock_fd);
        return RAY_ERR_OOM;
    }

    for (uint32_t i = 0; i < count; i++) {
        list = ray_list_append(list, snap[i]);
        if (!list || RAY_IS_ERR(list)) {
            ray_free(snap_block);
            ray_file_unlock(lock_fd);
            ray_file_close(lock_fd);
            return RAY_ERR_OOM;
        }
    }
    ray_free(snap_block);

    /* Save to temp file via ray_col_save (writes STRL format) */
    err = ray_col_save(list, tmp_path);
    ray_release(list);
    if (err != RAY_OK) {
        remove(tmp_path);
        ray_file_unlock(lock_fd);
        ray_file_close(lock_fd);
        return err;
    }

    /* Fsync temp file for durability */
    ray_fd_t tmp_fd = ray_file_open(tmp_path, RAY_OPEN_READ | RAY_OPEN_WRITE);
    if (tmp_fd == RAY_FD_INVALID) {
        remove(tmp_path);
        ray_file_unlock(lock_fd);
        ray_file_close(lock_fd);
        return RAY_ERR_IO;
    }
    err = ray_file_sync(tmp_fd);
    ray_file_close(tmp_fd);
    if (err != RAY_OK) {
        remove(tmp_path);
        ray_file_unlock(lock_fd);
        ray_file_close(lock_fd);
        return err;
    }

    /* Atomic rename: tmp -> final path */
    err = ray_file_rename(tmp_path, path);
    if (err != RAY_OK) {
        remove(tmp_path);
        ray_file_unlock(lock_fd);
        ray_file_close(lock_fd);
        return err;
    }

    /* Fsync parent directory so the new directory entry is durable.
     * Without this, a crash after rename can lose the new file. */
    err = ray_file_sync_dir(path);
    if (err != RAY_OK) {
        ray_file_unlock(lock_fd);
        ray_file_close(lock_fd);
        return err;
    }

    /* Update persisted count */
    sym_lock();
    g_sym.persisted_count = count;
    sym_unlock();

    ray_file_unlock(lock_fd);
    ray_file_close(lock_fd);
    return RAY_OK;
}

/* --------------------------------------------------------------------------
 * ray_sym_load -- load symbol table from RAY_LIST file (STRL format)
 *
 * Uses ray_col_load to read the list, then interns entries beyond what's
 * already in memory.  File locking prevents reading a partial write.
 * -------------------------------------------------------------------------- */

ray_err_t ray_sym_load(const char* path) {
    if (!path) return RAY_ERR_IO;
    if (!atomic_load_explicit(&g_sym_inited, memory_order_acquire)) return RAY_ERR_IO;

    /* Acquire cross-process shared lock.
     * Try read-only open first so that read-only users (snapshots, read-only
     * mounts) can load without write permission on the directory.  Fall back
     * to read-write+create if the lock file doesn't exist yet.  If both fail,
     * only proceed without locking on read-only filesystem (EROFS) — other
     * errors (EMFILE, ENFILE, EACCES on writable fs, etc.) are real failures
     * that would silently drop the shared-lock guarantee. */
    char lock_path[1024];
    if (snprintf(lock_path, sizeof(lock_path), "%s.lk", path) >= (int)sizeof(lock_path))
        return RAY_ERR_IO;
    ray_fd_t lock_fd = ray_file_open(lock_path, RAY_OPEN_READ);
    if (lock_fd == RAY_FD_INVALID) {
        int saved_errno = errno;
        lock_fd = ray_file_open(lock_path, RAY_OPEN_READ | RAY_OPEN_WRITE | RAY_OPEN_CREATE);
        if (lock_fd == RAY_FD_INVALID) {
            /* Only proceed unlocked on read-only filesystem (EROFS) where
             * concurrent writes are impossible.  All other failures are
             * real errors that should not be silently ignored. */
            if (saved_errno != EROFS && errno != EROFS)
                return RAY_ERR_IO;
        }
    }
    if (lock_fd != RAY_FD_INVALID) {
        ray_err_t err = ray_file_lock_sh(lock_fd);
        if (err != RAY_OK) { ray_file_close(lock_fd); return err; }
    }

    /* Load the sym file as a RAY_LIST of -RAY_STR */
    ray_t* list = ray_col_load(path);
    if (!list || RAY_IS_ERR(list)) {
        ray_err_t code = RAY_IS_ERR(list) ? ray_err_from_obj(list) : RAY_ERR_IO;
        ray_file_unlock(lock_fd);
        ray_file_close(lock_fd);
        return code;
    }

    if (list->type != RAY_LIST || list->len > UINT32_MAX) {
        ray_release(list);
        ray_file_unlock(lock_fd);
        ray_file_close(lock_fd);
        return RAY_ERR_CORRUPT;
    }

    /* Validate existing entries match, then intern remaining.
     * Use persisted_count (not str_count) as the already-loaded prefix:
     * runtime code may ray_sym_intern transient names that were never
     * persisted, and those must not participate in prefix validation
     * or affect the intern start offset. */
    sym_lock();
    uint32_t already = g_sym.persisted_count;
    sym_unlock();
    ray_t** slots = (ray_t**)ray_data(list);

    /* Reject stale/truncated sym file: if disk has fewer entries than what
     * we previously loaded from disk, the file is outdated or truncated. */
    if (already > 0 && list->len < (int64_t)already) {
        ray_release(list);
        ray_file_unlock(lock_fd);
        ray_file_close(lock_fd);
        return RAY_ERR_CORRUPT;
    }

    /* Validate entries [0..already-1] match the persisted prefix */
    for (int64_t i = 0; i < (int64_t)already && i < list->len; i++) {
        ray_t* s = slots[i];
        if (!s || RAY_IS_ERR(s) || s->type != -RAY_STR) {
            ray_release(list);
            ray_file_unlock(lock_fd);
            ray_file_close(lock_fd);
            return RAY_ERR_CORRUPT;
        }
        ray_t* mem_s = ray_sym_str(i);
        if (!mem_s || ray_str_len(mem_s) != ray_str_len(s) ||
            memcmp(ray_str_ptr(mem_s), ray_str_ptr(s), ray_str_len(s)) != 0) {
            ray_release(list);
            ray_file_unlock(lock_fd);
            ray_file_close(lock_fd);
            return RAY_ERR_CORRUPT;
        }
    }

    /* Intern entries beyond what's already in memory.
     * Verify each entry's in-memory ID matches its disk position:
     * if transient runtime-interned symbols already occupy these
     * slots, the disk entries would get wrong IDs, causing RAY_SYM
     * columns to resolve the wrong strings. */
    for (int64_t i = (int64_t)already; i < list->len; i++) {
        ray_t* s = slots[i];
        if (!s || RAY_IS_ERR(s) || s->type != -RAY_STR) {
            ray_release(list);
            ray_file_unlock(lock_fd);
            ray_file_close(lock_fd);
            return RAY_ERR_CORRUPT;
        }
        /* Bulk load MUST use the no-split variant so that loading a disk
         * entry like "user.name" doesn't recursively intern "user" + "name"
         * mid-loop and shift subsequent disk positions — that would break
         * the id==i contract below.  Segment cache is populated in one
         * pass after the loop finishes. */
        int64_t id = ray_sym_intern_no_split(ray_str_ptr(s), ray_str_len(s));
        if (id < 0) {
            ray_release(list);
            ray_file_unlock(lock_fd);
            ray_file_close(lock_fd);
            return RAY_ERR_OOM;
        }
        if (id != i) {
            /* ID mismatch: disk position i was assigned in-memory
             * id != i, meaning a transient symbol occupies the slot.
             * The sym table has diverged from disk; continuing would
             * cause RAY_SYM columns to resolve wrong strings. */
            ray_release(list);
            ray_file_unlock(lock_fd);
            ray_file_close(lock_fd);
            return RAY_ERR_CORRUPT;
        }
    }

    /* Populate dotted cache for every loaded (and previously-loaded) sym.
     * Idempotent — already-cached entries are skipped.  Runs once per load.
     * An OOM here must surface: leaving dotted names un-cached would make
     * env lookup silently resolve them as flat syms, quietly losing
     * namespace semantics on anything the user stored with a '.' in it. */
    ray_err_t rebuild_err = ray_sym_rebuild_segments();
    if (rebuild_err != RAY_OK) {
        ray_release(list);
        ray_file_unlock(lock_fd);
        ray_file_close(lock_fd);
        return rebuild_err;
    }

    /* Update persisted count to reflect what is actually on disk.
     * Use list->len (not str_count) because transient runtime-interned
     * symbols may exist beyond the persisted prefix. */
    sym_lock();
    g_sym.persisted_count = (uint32_t)list->len;
    sym_unlock();

    ray_release(list);
    ray_file_unlock(lock_fd);
    ray_file_close(lock_fd);
    return RAY_OK;
}
