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

/*
 * domain.c -- Symbol-resolution domains (see domain.h).
 *
 * The FILE-domain reader is INDEPENDENT of g_sym/sym.c: it shares only
 * the on-disk STRL format knowledge (4-byte "STRL" magic, int64 count,
 * then count x [u32 len | bytes] records).  The ONLY touchpoint with
 * the global intern table is the runtime-id LUT build (sanctioned,
 * sequential) and the RAY_SYM_AUDIT failure path.
 *
 * Post-flip (Task 7b) a FILE domain is base + tail: the mmapped file at
 * open time plus in-memory appended symbols (ray_sym_domain_intern).
 * Append-only; positions are permanent; ray_sym_domain_flush persists
 * base+tail atomically under the symfile's flock.
 *
 * Concurrency: one global spinlock guards the cache list, FILE-domain
 * refcounts, appends, the lazily built reverse index and the runtime-id
 * LUT build.  Domain opens, interns (save merges / live inserts — all
 * inside sequential eval) and first-find index builds are rare.  The
 * two hot read paths are LOCK-FREE: ray_sym_domain_str reads the
 * atomically published (count, atoms) pair — appends publish a slot
 * write with a release store of the count, growth REPLACES the array
 * (old arrays are retired, never freed in place, so a stale pointer is
 * always valid memory covering every position < its publish count) —
 * and ray_sym_domain_runtime_lut reads an atomically published array
 * (invalidated and rebuilt across appends; retired likewise).  The
 * runtime singleton never touches the lock (immortal, delegates to
 * sym.c's own locking).
 */

#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
#  define _GNU_SOURCE       /* realpath */
#endif

#include "domain.h"
#include "core/platform.h"  /* ray_vm_map_file / ray_vm_unmap_file */
#include "mem/heap.h"
#include "mem/sys.h"   /* domain buffers are process-global → mmap-backed, not buddy */
#include "mem/arena.h"   /* ray_arena_t / ray_arena_str — domain atom storage */
#include "store/fileio.h"   /* flock + tmp/rename protocol for flush */
#include "ops/hash.h"       /* ray_hash_bytes (same hash family as g_sym) */
#include <stdatomic.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <libgen.h>
#include <sys/stat.h>
#include <limits.h>
#ifndef PATH_MAX
#  define PATH_MAX 4096
#endif

/* Same on-disk format constant as src/table/sym.c (private there). */
#define DOMAIN_STRL_MAGIC 0x4C525453U /* "STRL" */

typedef enum {
    DOM_RUNTIME = 0,
    DOM_FILE    = 1,
} dom_kind_t;

/* ---- per-domain string-atom arena ----------------------------------------
 *
 * String atoms for a FILE domain are LAZILY materialized on first access and
 * allocated from a per-domain ray_arena_t (mem/arena.c — mmap-backed via
 * ray_sys_alloc, the same arena the global sym table uses) instead of the
 * per-thread buddy heap.  Two reasons it must NOT be the buddy heap:
 *   - Lazy materialization runs on ray_pool_dispatch WORKER threads during
 *     query execution; each worker owns a distinct buddy heap.  An atom built
 *     with ray_str() there would land on a worker heap that the process-global
 *     domain cache outlives → use-after-free when that heap tears down.  The
 *     arena's chunks are thread-independent and freed only at dom_destroy.
 *   - ray_arena_str atoms carry RAY_ATTR_ARENA: ray_free / retain / release all
 *     no-op on them, so they are refcount-free and bulk-freed by the arena. */

/* Retired allocations (replaced atom arrays / invalidated LUTs): kept
 * alive until domain destroy so lock-free readers holding a stale
 * pointer never touch freed memory. */
typedef struct dom_retired_s {
    void* ptr;
    struct dom_retired_s* next;
} dom_retired_t;
typedef struct dom_retired_map_s {
    void*  map;
    size_t size;
    struct dom_retired_map_s* next;
} dom_retired_map_t;
typedef struct dom_raw_snap_s {
    const unsigned char* map;
    const size_t*        offsets;
    int64_t              count;
} dom_raw_snap_t;

struct ray_sym_domain_s {
    uint8_t   kind;        /* dom_kind_t */
    bool      immortal;    /* runtime singleton: retain/release skip */
    uint32_t  rc;          /* FILE: open/attach refs (guarded by lock) */
    char*     path;        /* FILE: malloc'd resolved path; NULL for runtime */

    /* FILE: base mapping of the symfile as of open (NULL for a freshly
     * created domain with no file yet). */
    void*     map;
    size_t    map_size;
    int64_t   base_count;  /* entries that came from the mapping */

    /* FILE: persisted state — how much of the vocabulary the file holds
     * (updated at open / extend / flush) and the exact byte size of the
     * file at that point (cache-hit revalidation key). */
    int64_t   disk_count;
    size_t    disk_size;

    /* FILE: published vocabulary — (atoms, count) pair.  atoms[i] is the
     * string atom for position i (borrowed by callers), LAZILY materialized
     * on first access from the arena.  A slot is NULL until materialized;
     * the per-slot publication is an atomic release store.  count is
     * release-published AFTER the slot write (for appends); growth REPLACES
     * the array via atomic store and retires the old one.  Lock-free readers
     * load count (acquire) then atoms (acquire) then the slot (acquire). */
    _Atomic(int64_t)  count;
    _Atomic(ray_t**)  atoms;
    int64_t   atoms_cap;   /* guarded by g_dom_lock */

    /* FILE: per-domain ray_arena_t backing every materialized string atom
     * (lazy file slots + runtime-appended interns).  Atoms carry
     * RAY_ATTR_ARENA (refcount-free); ray_arena_destroy bulk-frees them.
     * Guarded by g_dom_lock (only ever touched under it). */
    ray_arena_t* arena;

    /* FILE: byte offset within `map` of each file entry's record (the u32
     * length prefix).  base_count entries, covering the file prefix
     * [0, base_count) only.  Built in one pass at open (no per-entry alloc);
     * used to materialize atoms[pos] on demand from the mmap bytes.
     * malloc'd; NULL when base_count == 0. */
    size_t*   offsets;
    /* Published raw snapshot (see ray_sym_domain_raw_pin): map, offsets and
     * the file-prefix count as one immutable record.  Replaced whole on
     * extend; the previous record, its offsets and its mapping are retired
     * (retired / retired_maps) rather than freed, so a reader holding the
     * old snapshot stays valid until the domain is destroyed. */
    _Atomic(struct dom_raw_snap_s*) raw_snap;
    struct dom_retired_map_s* retired_maps;

    /* FILE, lazy: position → runtime intern id.  Built under g_dom_lock
     * (the build INTERNS the vocabulary into the global table —
     * sequential by contract, see the header), published with release
     * semantics; reads are lock-free.  Appends invalidate it (retire +
     * NULL); the next request rebuilds over the grown vocabulary. */
    _Atomic(int64_t*) runtime_lut;

    /* FILE, lazy: reverse index — (hash32 << 32) | (pos + 1), 0 = empty.
     * Same bucket encoding as g_sym (sym.c).  Invariant: when buckets
     * exist they cover EVERY published entry (lazy build is full; intern
     * inserts incrementally; growth rebuilds).  Guarded by g_dom_lock. */
    uint64_t* buckets;
    uint64_t  bucket_mask; /* cap - 1; 0 = not built yet */

    dom_retired_t* retired; /* replaced atom arrays + old LUTs */

    struct ray_sym_domain_s* next; /* cache chain */
};

/* ---- runtime singleton --------------------------------------------------- */

static struct ray_sym_domain_s g_runtime_domain = {
    .kind     = DOM_RUNTIME,
    .immortal = true,
};

ray_sym_domain_t* ray_sym_runtime_domain(void) {
    return &g_runtime_domain;
}

/* ---- cache + lock --------------------------------------------------------- */

static ray_sym_domain_t* g_domains = NULL;

static _Atomic(int) g_dom_lock = 0;
static inline void dom_lock(void) {
    while (atomic_exchange_explicit(&g_dom_lock, 1, memory_order_acquire)) {
        RAY_CPU_RELAX();
    }
}
static inline void dom_unlock(void) {
    atomic_store_explicit(&g_dom_lock, 0, memory_order_release);
}

/* ---- FILE domain construction / destruction ------------------------------- */

/* Resolved cache key for `path`.  realpath of the file when it exists;
 * for to-be-created symfiles, realpath of the parent + "/" + basename
 * (the parent must exist).  malloc'd. */
static char* dom_resolve_path(const char* path) {
    /* Fixed-buffer realpath (POSIX form) + ray_alloc_raw for the result — avoids
     * realpath(NULL)/strdup's libc-malloc'd buffers, so the returned key is
     * uniformly buddy-allocated and the caller releases it with ray_free_raw. */
    char resolved[PATH_MAX];
    if (realpath(path, resolved)) {
        size_t n = strlen(resolved);
        char* out = (char*)ray_sys_alloc(n + 1);
        if (out) memcpy(out, resolved, n + 1);
        return out;
    }

    char tmp[1024];
    size_t plen = strlen(path);
    if (plen == 0 || plen >= sizeof(tmp)) return NULL;
    memcpy(tmp, path, plen + 1);
    char* base = basename(tmp);
    char bbuf[1024];
    size_t blen = strlen(base);
    if (blen >= sizeof(bbuf)) return NULL;
    memcpy(bbuf, base, blen + 1);
    /* basename may have modified tmp; recompute dirname on a fresh copy */
    memcpy(tmp, path, plen + 1);
    char* dir = dirname(tmp);
    char rdir[PATH_MAX];
    if (!realpath(dir, rdir)) return NULL;
    size_t dlen = strlen(rdir);
    char* out = (char*)ray_sys_alloc(dlen + 1 + blen + 1);
    if (!out) return NULL;
    memcpy(out, rdir, dlen);
    out[dlen] = '/';
    memcpy(out + dlen + 1, bbuf, blen + 1);
    return out;
}

/* Retire an allocation that lock-free readers may still hold. */
static bool dom_retire(ray_sym_domain_t* d, void* p) {
    if (!p) return true;
    dom_retired_t* r = (dom_retired_t*)ray_sys_alloc(sizeof(*r));
    if (!r) return false;
    r->ptr = p;
    r->next = d->retired;
    d->retired = r;
    return true;
}
/* Publish (map, offsets, count) as the domain's raw snapshot; the previous
 * record is retired.  Called with the file's current image in place. */
static bool dom_publish_raw_snap(ray_sym_domain_t* d) {
    dom_raw_snap_t* snap = NULL;
    if (d->map && d->offsets && d->base_count > 0) {
        snap = (dom_raw_snap_t*)ray_sys_alloc(sizeof(*snap));
        if (!snap) return false;
        snap->map = (const unsigned char*)d->map;
        snap->offsets = d->offsets;
        snap->count = d->base_count;
    }
    dom_raw_snap_t* old = atomic_exchange_explicit(&d->raw_snap, snap, memory_order_acq_rel);
    if (old) (void)dom_retire(d, old);
    return true;
}

/* Free a FILE domain after the last reference dropped.  Caller has
 * already unlinked it from the cache. */
static void dom_destroy(ray_sym_domain_t* d) {
    /* Atoms are RAY_ATTR_ARENA — never refcounted, never individually freed.
     * The arena bulk-frees them; we only free the pointer ARRAY here. */
    ray_t** atoms = atomic_load_explicit(&d->atoms, memory_order_relaxed);
    ray_sys_free(atoms);
    if (d->arena) ray_arena_destroy(d->arena);
    ray_sys_free(d->offsets);
    ray_sys_free(atomic_load_explicit(&d->runtime_lut, memory_order_relaxed));
    for (dom_retired_t* r = d->retired; r;) {
        dom_retired_t* nxt = r->next;
        ray_sys_free(r->ptr);
        ray_sys_free(r);
        r = nxt;
    }
    ray_sys_free(d->buckets);
    ray_sys_free(atomic_load_explicit(&d->raw_snap, memory_order_relaxed));
    for (dom_retired_map_t* rm = d->retired_maps; rm;) {
        dom_retired_map_t* nxt = rm->next;
        ray_vm_unmap_file(rm->map, rm->size);
        ray_sys_free(rm);
        rm = nxt;
    }
    if (d->map) ray_vm_unmap_file(d->map, d->map_size);
    ray_sys_free(d->path);
    ray_sys_free(d);
}

/* Index a mapped STRL image in one pass WITHOUT materializing any atom:
 * validate magic + count, then record each record's byte offset (the start
 * of its u32 length prefix) into a fresh offsets array.  atoms[] are all
 * NULL — every slot is materialized lazily on first access.  Returns false
 * on structural inconsistency (bad magic, truncated record, trailing bytes)
 * or OOM.  On success *out_atoms (calloc'd NULLs) and *out_offsets (malloc'd
 * byte offsets), each exactly *out_count entries; both NULL when empty. */
static bool dom_parse_strl_atoms(const void* map, size_t map_size,
                                 ray_t*** out_atoms, size_t** out_offsets,
                                 int64_t* out_count) {
    const uint8_t* base = (const uint8_t*)map;
    if (map_size < 12) return false;

    uint32_t magic;
    memcpy(&magic, base, 4);
    if (magic != DOMAIN_STRL_MAGIC) return false;

    int64_t count;
    memcpy(&count, base + 4, 8);
    if (count < 0 || count > UINT32_MAX) return false;

    ray_t** atoms = NULL;
    size_t* offsets = NULL;
    if (count > 0) {
        atoms = (ray_t**)ray_sys_alloc((size_t)((size_t)count) * (sizeof(ray_t*)));
        if (!atoms) return false;
        offsets = (size_t*)ray_sys_alloc((size_t)count * sizeof(size_t));
        if (!offsets) { ray_sys_free(atoms); return false; }
    }

    size_t off = 12;
    size_t remaining = map_size - 12;
    for (int64_t i = 0; i < count; i++) {
        offsets[i] = off;          /* start of this record's u32 len prefix */
        uint32_t slen;
        if (remaining < 4) goto fail;
        memcpy(&slen, base + off, 4);
        off += 4; remaining -= 4;
        if ((size_t)slen > remaining) goto fail;
        off += slen; remaining -= slen;
    }
    if (remaining != 0) goto fail;

    *out_atoms = atoms;
    *out_offsets = offsets;
    *out_count = count;
    return true;

fail:
    ray_sys_free(atoms);
    ray_sys_free(offsets);
    return false;
}

/* Materialize atoms[pos] from the file bytes at offsets[pos] into the arena
 * and publish it.  CALLER MUST HOLD g_dom_lock.  Idempotent: returns an
 * already-published slot without re-materializing.  pos must be a valid file
 * position (0 <= pos < base_count); runtime-appended slots (pos >=
 * base_count) are materialized at append time and are never NULL.  Returns
 * NULL on arena OOM. */
static ray_t* dom_atom_at_locked(ray_sym_domain_t* d, int64_t pos) {
    ray_t** atoms = atomic_load_explicit(&d->atoms, memory_order_relaxed);
    ray_t* a = atomic_load_explicit((_Atomic(ray_t*)*)&atoms[pos],
                                    memory_order_relaxed);
    if (a) return a;

    /* Lazy file slot: read [u32 len | bytes] at offsets[pos] from the map. */
    const uint8_t* base = (const uint8_t*)d->map;
    size_t off = d->offsets[pos];
    uint32_t slen;
    memcpy(&slen, base + off, 4);
    a = ray_arena_str(d->arena, (const char*)base + off + 4, slen);
    if (!a) return NULL;
    atomic_store_explicit((_Atomic(ray_t*)*)&atoms[pos], a, memory_order_release);
    return a;
}

/* Cache-hit revalidation: the file changed size since this object last
 * touched it.  Append-only external growth (a reader process catching
 * up with a writer) EXTENDS the object in place; anything else —
 * shrunk/rewritten file, or growth while this process holds unflushed
 * appends (a second writer) — fails.  Called under g_dom_lock with the
 * file's current size in st_size. */
static bool dom_extend_from_file_locked(ray_sym_domain_t* d, size_t st_size) {
    int64_t count = atomic_load_explicit(&d->count, memory_order_relaxed);
    if (st_size < d->disk_size) return false;       /* shrunk: rewritten */
    if (count != d->disk_count) return false;       /* our tail is dirty */

    size_t map_size = 0;
    void* map = ray_vm_map_file(d->path, &map_size);
    if (!map) return false;

    ray_t** fresh = NULL;       /* all-NULL (lazy) slot array for the new image */
    size_t* fresh_offsets = NULL;
    int64_t fresh_count = 0;
    if (!dom_parse_strl_atoms(map, map_size, &fresh, &fresh_offsets,
                              &fresh_count)) {
        ray_vm_unmap_file(map, map_size);
        return false;
    }

    bool ok = fresh_count >= count;

    const uint8_t* nbase = (const uint8_t*)map;     /* new image bytes */
    const uint8_t* obase = (const uint8_t*)d->map;  /* old image bytes (count > 0) */

    /* Read a file record's [len, ptr] at byte offset `off` within `b`. */
    #define DOM_REC(b, off, lp, pp) do {                       \
        uint32_t _l; memcpy(&_l, (b) + (off), 4);              \
        (lp) = _l; (pp) = (const char*)((b) + (off) + 4);      \
    } while (0)

    /* Position-0 reservation also holds for externally grown files: the
     * first record of a non-empty file must be the empty string. */
    if (ok && count == 0 && fresh_count > 0) {
        uint32_t l0; const char* p0; (void)p0;
        DOM_REC(nbase, fresh_offsets[0], l0, p0);
        if (l0 != 0) ok = false;
    }

    /* Append-only prefix check: every position we already published must be
     * byte-identical between the old and new file images (compare mmap bytes
     * directly via the old/new offsets — no materialization needed). */
    for (int64_t i = 0; ok && i < count; i++) {
        uint32_t ol, nl; const char *op, *np;
        DOM_REC(obase, d->offsets[i], ol, op);
        DOM_REC(nbase, fresh_offsets[i], nl, np);
        if (ol != nl || (ol > 0 && memcmp(op, np, ol) != 0)) ok = false;
    }
    #undef DOM_REC

    if (ok && fresh_count > count) {
        /* Publish the grown vocabulary.  Build a NEW slot array (replace,
         * never realloc — lock-free readers may hold the old one): carry the
         * already-materialized prefix atoms (arena-owned, still valid; long
         * atoms hold their own copy, so swapping the map is safe), new file
         * slots start NULL (lazy). */
        ray_t** atoms = atomic_load_explicit(&d->atoms, memory_order_relaxed);
        ray_t** narr = (ray_t**)ray_sys_alloc((size_t)fresh_count * sizeof(ray_t*));
        if (!narr) {
            ok = false;
        } else if (!dom_retire(d, atoms)) {
            ray_sys_free(narr);
            ok = false;
        } else {
            if (count > 0) memcpy(narr, atoms, (size_t)count * sizeof(ray_t*));
            for (int64_t i = count; i < fresh_count; i++) narr[i] = NULL;
            ray_sys_free(fresh);  /* fresh slot array unused — narr carries the prefix */

            /* Swap in the new offsets + map.  Prefix atoms are arena
             * copies, but a raw snapshot may still point at the old
             * offsets and mapping: retire both instead of freeing. */
            (void)dom_retire(d, d->offsets);
            d->offsets = fresh_offsets;
            void* old_map = d->map;
            size_t old_map_size = d->map_size;
            d->map = map;
            d->map_size = map_size;
            d->base_count = fresh_count;

            atomic_store_explicit(&d->atoms, narr, memory_order_release);
            d->atoms_cap = fresh_count;
            atomic_store_explicit(&d->count, fresh_count, memory_order_release);
            d->disk_count = fresh_count;
            d->disk_size = st_size;

            if (old_map) {
                dom_retired_map_t* rm = (dom_retired_map_t*)ray_sys_alloc(sizeof(*rm));
                if (rm) { rm->map = old_map; rm->size = old_map_size; rm->next = d->retired_maps; d->retired_maps = rm; }
                /* no rm: the old mapping stays mapped for the process
                 * lifetime — never unmapped under a live snapshot */
            }
            (void)dom_publish_raw_snap(d);

            /* Reverse index + LUT now cover a stale prefix: drop both.
             * A retire-OOM here would keep the stale LUT published over
             * the grown count (OOB reads for lock-free consumers) — the
             * same corner dom_append_locked hits; mirror its loud abort
             * (the count is already published, there is no clean undo). */
            if (d->buckets) { ray_sys_free(d->buckets); d->buckets = NULL; d->bucket_mask = 0; }
            int64_t* lut = atomic_load_explicit(&d->runtime_lut, memory_order_relaxed);
            if (lut) {
                if (!dom_retire(d, lut)) {
                    fprintf(stderr, "rayforce: sym domain '%s': OOM retiring "
                                    "runtime LUT after external extend — cannot "
                                    "keep id translation consistent\n",
                            d->path ? d->path : "?");
                    abort();
                }
                atomic_store_explicit(&d->runtime_lut, NULL, memory_order_release);
            }
            return true;
        }
    }

    /* Failure, or nothing actually new (size-only change): drop the fresh
     * parse and the new mapping; keep the existing image. */
    if (ok && fresh_count == count) d->disk_size = st_size;
    ray_sys_free(fresh);
    ray_sys_free(fresh_offsets);
    ray_vm_unmap_file(map, map_size);
    return ok;
}

static ray_sym_domain_t* dom_open_impl(const char* path, bool create) {
    if (!path) return NULL;

    char* rpath = dom_resolve_path(path);
    if (!rpath) return NULL;

    struct stat st;
    bool exists = (stat(rpath, &st) == 0);
    if (!exists && !create) { ray_sys_free(rpath); return NULL; }

    dom_lock();
    for (ray_sym_domain_t* d = g_domains; d; d = d->next) {
        if (strcmp(d->path, rpath) == 0) {
            /* Revalidate: external append-only growth extends in place;
             * any other divergence is loud (NULL). */
            size_t cur_size = exists ? (size_t)st.st_size : 0;
            if (cur_size != d->disk_size &&
                !dom_extend_from_file_locked(d, cur_size)) {
                dom_unlock();
                ray_sys_free(rpath);
                return NULL;
            }
            d->rc++;
            dom_unlock();
            ray_sys_free(rpath);
            return d;
        }
    }
    dom_unlock();

    /* Not cached: build outside the lock (mapping + parse can be slow). */
    ray_sym_domain_t* d = (ray_sym_domain_t*)ray_sys_alloc((size_t)(1) * (sizeof(*d)));
    if (!d) { ray_sys_free(rpath); return NULL; }
    d->kind = DOM_FILE;
    d->rc = 1;
    atomic_store_explicit(&d->raw_snap, NULL, memory_order_relaxed);
    d->retired_maps = NULL;
    d->path = rpath;
    /* String atoms (lazy file slots + runtime-appended interns) live here,
     * off the per-thread buddy heap.  64 KB chunks. */
    d->arena = ray_arena_new(64 * 1024);
    if (!d->arena) { ray_sys_free(d); ray_sys_free(rpath); return NULL; }

    if (exists) {
        d->map = ray_vm_map_file(rpath, &d->map_size);
        ray_t** atoms = NULL;
        size_t* offsets = NULL;
        int64_t count = 0;
        if (!d->map ||
            !dom_parse_strl_atoms(d->map, d->map_size, &atoms, &offsets, &count)) {
            dom_destroy(d);
            return NULL;
        }
        atomic_store_explicit(&d->atoms, atoms, memory_order_relaxed);
        atomic_store_explicit(&d->count, count, memory_order_relaxed);
        d->offsets = offsets;
        d->atoms_cap = count;
        d->base_count = count;
        (void)dom_publish_raw_snap(d);
        d->disk_count = count;
        d->disk_size = d->map_size;
        /* Position-0 reservation: every non-empty symfile must carry the
         * empty string at position 0 (mirrors global id 0 — the SYM
         * null; group kernels and null conventions treat id 0 as "" /
         * null).  A vocabulary that starts with anything else is
         * structurally corrupt for domain use.  Empty files are fine.
         * Check the file bytes directly (slen at offsets[0] must be 0) —
         * no atom materialization needed. */
        if (count > 0) {
            uint32_t l0;
            memcpy(&l0, (const uint8_t*)d->map + offsets[0], 4);
            if (l0 != 0) {
                dom_destroy(d);
                return NULL;
            }
        }
    }
    /* create && !exists: empty domain — vocabulary appears via intern
     * ("" seeded at position 0) and reaches disk on first flush. */

    /* Insert, unless another thread raced us to the same path — then keep
     * the winner (pointer equality must hold for one resolved path). */
    dom_lock();
    for (ray_sym_domain_t* e = g_domains; e; e = e->next) {
        if (strcmp(e->path, d->path) == 0) {
            e->rc++;
            dom_unlock();
            dom_destroy(d);
            return e;
        }
    }
    d->next = g_domains;
    g_domains = d;
    dom_unlock();
    return d;
}

ray_sym_domain_t* ray_sym_domain_open(const char* path) {
    return dom_open_impl(path, false);
}

ray_sym_domain_t* ray_sym_domain_open_or_create(const char* path) {
    return dom_open_impl(path, true);
}

void ray_sym_domain_retain(ray_sym_domain_t* dom) {
    if (!dom || dom->immortal) return;
    dom_lock();
    dom->rc++;
    dom_unlock();
}

void ray_sym_domain_release(ray_sym_domain_t* dom) {
    if (!dom || dom->immortal) return;
    dom_lock();
    if (--dom->rc > 0) {
        dom_unlock();
        return;
    }
    /* Last ref: unlink the cache entry, then free outside the lock.
     * A subsequent open of the same path builds a fresh mapping. */
    ray_sym_domain_t** pp = &g_domains;
    while (*pp && *pp != dom) pp = &(*pp)->next;
    if (*pp) *pp = dom->next;
    dom_unlock();
    dom_destroy(dom);
}

/* ---- resolution ------------------------------------------------------------ */

ray_t* ray_sym_domain_str(ray_sym_domain_t* dom, int64_t pos) {
    if (!dom) return NULL;
    if (dom->kind == DOM_RUNTIME) return ray_sym_str(pos);

    /* Lock-free fast path: acquire the published count, then the array, then
     * the slot.  Appends release-publish the count after the slot write;
     * array replacements are release-published before the count bump and old
     * arrays are retired (valid memory) — a reader can never index a
     * published position into memory that lacks it.  A NULL slot is an
     * unmaterialized lazy file entry: fall to the lock-held materialize. */
    int64_t count = atomic_load_explicit(&dom->count, memory_order_acquire);
    if (pos < 0 || pos >= count) return NULL;
    ray_t** atoms = atomic_load_explicit(&dom->atoms, memory_order_acquire);
    ray_t* a = atomic_load_explicit((_Atomic(ray_t*)*)&atoms[pos],
                                    memory_order_acquire);
    if (a) return a;

    dom_lock();
    a = dom_atom_at_locked(dom, pos);
    dom_unlock();
    return a;
}

bool ray_sym_domain_raw_pin(ray_sym_domain_t* dom, ray_sym_domain_raw_t* out) {
    if (!dom || !out || dom->kind == DOM_RUNTIME) return false;
    dom_raw_snap_t* snap = atomic_load_explicit(&dom->raw_snap, memory_order_acquire);
    if (!snap) return false;
    out->map = snap->map;
    out->offsets = snap->offsets;
    out->count = snap->count;
    return true;
}
void ray_sym_domain_raw_unpin(ray_sym_domain_t* dom) {
    (void)dom;   /* the snapshot outlives its readers by construction */
}
/* Empty-vocabulary FILE domains get a distinct non-NULL LUT so callers
 * can branch on NULL == "runtime domain, ids pass through".  Never
 * indexed: every position is out of range when count == 0. */
static const int64_t g_empty_lut[1] = { -1 };

const int64_t* ray_sym_domain_runtime_lut(ray_sym_domain_t* dom) {
    if (!dom || dom->kind == DOM_RUNTIME) return NULL;

    int64_t* lut = atomic_load_explicit(&dom->runtime_lut, memory_order_acquire);
    if (lut) return lut;
    if (atomic_load_explicit(&dom->count, memory_order_acquire) == 0)
        return g_empty_lut;

    dom_lock();
    lut = atomic_load_explicit(&dom->runtime_lut, memory_order_relaxed);
    if (!lut) {
        int64_t count = atomic_load_explicit(&dom->count, memory_order_relaxed);
        lut = (int64_t*)ray_sys_alloc((size_t)count * sizeof(int64_t));
        if (!lut) { dom_unlock(); return NULL; }
        /* THE sanctioned interning of a file vocabulary into the global
         * table: one sequential pass over the VOCABULARY (never the
         * rows).  Callers that hand ids to ray_pool_dispatch workers
         * must request this LUT during sequential setup — interning
         * inside a worker violates sym.c's frozen-table rule. */
        for (int64_t i = 0; i < count; i++) {
            ray_t* a = dom_atom_at_locked(dom, i);
            if (!a) { ray_sys_free(lut); dom_unlock(); return NULL; }
            lut[i] = ray_sym_intern(ray_str_ptr(a), ray_str_len(a));
            if (lut[i] < 0) {
                /* 7a-review hardening: never publish a LUT carrying -1
                 * intern failures — a silent -1 makes cross-domain keys
                 * spuriously equal.  Loud NULL instead. */
                ray_sys_free(lut);
                dom_unlock();
                return NULL;
            }
        }
        atomic_store_explicit(&dom->runtime_lut, lut, memory_order_release);
    }
    dom_unlock();
    return lut;
}

/* (Re)build the reverse index over every published entry.  Called under
 * the lock.  `extra` reserves headroom for entries about to be added. */
static bool dom_build_index_locked(ray_sym_domain_t* d, int64_t extra) {
    int64_t count = atomic_load_explicit(&d->count, memory_order_relaxed);
    uint64_t need = (uint64_t)count + (uint64_t)extra;
    uint64_t cap = 16;
    while ((double)need > 0.7 * (double)cap) {
        if (cap > (UINT64_MAX >> 1)) return false;
        cap <<= 1;
    }
    uint64_t* buckets = (uint64_t*)ray_sys_alloc((size_t)((size_t)cap) * (sizeof(uint64_t)));
    if (!buckets) return false;

    uint64_t mask = cap - 1;
    for (int64_t i = 0; i < count; i++) {
        ray_t* a = dom_atom_at_locked(d, i);
        if (!a) { ray_sys_free(buckets); return false; }
        uint32_t h = (uint32_t)ray_hash_bytes(ray_str_ptr(a), ray_str_len(a));
        uint64_t slot = h & mask;
        while (buckets[slot] != 0) slot = (slot + 1) & mask;
        buckets[slot] = ((uint64_t)h << 32) | ((uint64_t)(uint32_t)i + 1);
    }
    ray_sys_free(d->buckets);
    d->buckets = buckets;
    d->bucket_mask = mask;
    return true;
}

/* Probe the reverse index.  Called under the lock with buckets built. */
static int64_t dom_probe_locked(ray_sym_domain_t* d, uint32_t h,
                                const char* str, size_t len) {
    uint64_t mask = d->bucket_mask;
    uint64_t slot = h & mask;
    while (d->buckets[slot] != 0) {
        uint64_t e = d->buckets[slot];
        if ((uint32_t)(e >> 32) == h) {
            int64_t pos = (int64_t)(uint32_t)e - 1;
            ray_t* a = dom_atom_at_locked(d, pos);
            if (a && ray_str_len(a) == len &&
                (len == 0 || memcmp(ray_str_ptr(a), str, len) == 0))
                return pos;
        }
        slot = (slot + 1) & mask;
    }
    return -1;
}

int64_t ray_sym_domain_find(ray_sym_domain_t* dom, const char* str, size_t len) {
    if (!dom || !str) return -1;
    if (dom->kind == DOM_RUNTIME) return ray_sym_find(str, len);

    if (atomic_load_explicit(&dom->count, memory_order_acquire) == 0)
        return -1;

    dom_lock();
    /* Sym ID 0 is reserved for the empty string (sym.c reserves it at init) and
     * a FILE domain's positions are written in id order, so position 0 is "".
     * VERIFY it — materialising ONLY atom 0, no full index build — rather than
     * assume, then short-circuit `<> ''` / `= ''` in O(1) instead of building the
     * whole reverse index just to locate "".  Falls through to the normal find if
     * a symfile was ever written some other way (then "" is found by probe). */
    if (len == 0) {
        ray_t* a0 = dom_atom_at_locked(dom, 0);
        if (a0 && ray_str_len(a0) == 0) { dom_unlock(); return 0; }
    }
    if (!dom->buckets && !dom_build_index_locked(dom, 0)) {
        dom_unlock();
        return -1;
    }
    uint32_t h = (uint32_t)ray_hash_bytes(str, len);
    int64_t found = dom_probe_locked(dom, h, str, len);
    dom_unlock();
    return found;
}

/* Append one symbol.  Called under the lock with buckets built and NOT
 * containing the symbol.  Publishes (atoms, count) atomically; retires
 * a replaced atom array; invalidates the runtime LUT.  Returns the new
 * position or -1 on OOM (no state change on failure). */
static int64_t dom_append_locked(ray_sym_domain_t* d, uint32_t h,
                                 const char* str, size_t len) {
    int64_t count = atomic_load_explicit(&d->count, memory_order_relaxed);
    if (count >= (int64_t)UINT32_MAX) return -1;

    /* Grow the published array by REPLACING it — never realloc in place:
     * lock-free readers may hold the old pointer. */
    if (count >= d->atoms_cap) {
        int64_t ncap = d->atoms_cap < 8 ? 8 : d->atoms_cap * 2;
        ray_t** narr = (ray_t**)ray_sys_alloc((size_t)ncap * sizeof(ray_t*));
        if (!narr) return -1;
        ray_t** old = atomic_load_explicit(&d->atoms, memory_order_relaxed);
        if (count > 0) memcpy(narr, old, (size_t)count * sizeof(ray_t*));
        if (!dom_retire(d, old)) { ray_sys_free(narr); return -1; }
        atomic_store_explicit(&d->atoms, narr, memory_order_release);
        d->atoms_cap = ncap;
    }

    /* Bucket headroom (rebuild covers everything published so far). */
    uint64_t cap = d->bucket_mask + 1;
    if ((double)(count + 1) > 0.7 * (double)cap &&
        !dom_build_index_locked(d, 1))
        return -1;

    /* Runtime-appended interns have NO file offset; materialize eagerly into
     * the arena here so the slot is never NULL (lazy materialization covers
     * only the file prefix [0, base_count)). */
    ray_t* s = ray_arena_str(d->arena, str, len);
    if (!s) return -1;

    ray_t** atoms = atomic_load_explicit(&d->atoms, memory_order_relaxed);
    atoms[count] = s;
    atomic_store_explicit(&d->count, count + 1, memory_order_release);

    uint64_t mask = d->bucket_mask;
    uint64_t slot = h & mask;
    while (d->buckets[slot] != 0) slot = (slot + 1) & mask;
    d->buckets[slot] = ((uint64_t)h << 32) | ((uint64_t)(uint32_t)count + 1);

    /* The published LUT (if any) no longer covers the vocabulary: retire
     * it; the next ray_sym_domain_runtime_lut rebuilds over the grown
     * set.  Retire failure (OOM) keeps the stale LUT published — that
     * would be a correctness hole, so treat it as append failure...
     * except the append IS already published.  Instead leak-by-keeping:
     * dom_retire only fails on a 16-byte malloc after we already did
     * larger ones — practically unreachable; fall back to NOT clearing
     * and accept the stale LUT being rebuilt on next miss is impossible,
     * so abort loudly in that corner. */
    int64_t* lut = atomic_load_explicit(&d->runtime_lut, memory_order_relaxed);
    if (lut) {
        if (!dom_retire(d, lut)) {
            fprintf(stderr, "rayforce: sym domain '%s': OOM retiring runtime "
                            "LUT after append — cannot keep id translation "
                            "consistent\n", d->path ? d->path : "?");
            abort();
        }
        atomic_store_explicit(&d->runtime_lut, NULL, memory_order_release);
    }

    return count;
}

int64_t ray_sym_domain_intern(ray_sym_domain_t* dom, const char* str, size_t len) {
    if (!dom || !str) return -1;
    if (dom->kind == DOM_RUNTIME) return ray_sym_intern(str, len);

    dom_lock();
    if (!dom->buckets && !dom_build_index_locked(dom, 1)) {
        dom_unlock();
        return -1;
    }

    /* Position-0 reservation: the first symbol of a brand-new domain is
     * always "" (the SYM null), even when the caller interns something
     * else first. */
    if (atomic_load_explicit(&dom->count, memory_order_relaxed) == 0 &&
        len != 0) {
        uint32_t h0 = (uint32_t)ray_hash_bytes("", 0);
        if (dom_append_locked(dom, h0, "", 0) != 0) {
            dom_unlock();
            return -1;
        }
    }

    uint32_t h = (uint32_t)ray_hash_bytes(str, len);
    int64_t pos = dom_probe_locked(dom, h, str, len);
    if (pos < 0) pos = dom_append_locked(dom, h, str, len);
    dom_unlock();
    return pos;
}

int64_t ray_sym_domain_count(ray_sym_domain_t* dom) {
    if (!dom) return 0;
    if (dom->kind == DOM_RUNTIME) return (int64_t)ray_sym_count();
    return atomic_load_explicit(&dom->count, memory_order_acquire);
}

const char* ray_sym_domain_path(ray_sym_domain_t* dom) {
    if (!dom || dom->kind == DOM_RUNTIME) return NULL;
    return dom->path;
}

/* ---- flush ------------------------------------------------------------------ */

ray_err_t ray_sym_domain_flush(ray_sym_domain_t* dom, bool durable) {
    if (!dom) return RAY_ERR_TYPE;
    if (dom->kind == DOM_RUNTIME) return RAY_OK; /* sym.c owns its own files */

    dom_lock();
    int64_t count = atomic_load_explicit(&dom->count, memory_order_relaxed);
    int64_t disk_count = dom->disk_count;
    size_t  disk_size  = dom->disk_size;
    /* Materialize every slot under the lock so the lock-free snapshot below
     * has no NULL (lazy) entries to write.  The atoms are arena-owned, so
     * once materialized they stay valid for the rest of this call. */
    if (count != disk_count) {
        for (int64_t i = 0; i < count; i++) {
            if (!dom_atom_at_locked(dom, i)) { dom_unlock(); return RAY_ERR_OOM; }
        }
    }
    ray_t** atoms = atomic_load_explicit(&dom->atoms, memory_order_relaxed);
    dom_unlock();

    if (count == disk_count) return RAY_OK; /* nothing new */

    char lock_path[1024];
    char tmp_path[1024];
    if (snprintf(lock_path, sizeof(lock_path), "%s.lk", dom->path) >= (int)sizeof(lock_path))
        return RAY_ERR_IO;
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", dom->path) >= (int)sizeof(tmp_path))
        return RAY_ERR_IO;

    ray_fd_t lock_fd = ray_file_open(lock_path,
                                     RAY_OPEN_READ | RAY_OPEN_WRITE | RAY_OPEN_CREATE);
    if (lock_fd == RAY_FD_INVALID) return RAY_ERR_IO;
    ray_err_t err = ray_file_lock_ex(lock_fd);
    if (err != RAY_OK) { ray_file_close(lock_fd); return err; }

    /* Verify-base-unchanged: the file on disk must be EXACTLY what this
     * object last persisted (size + header count).  In-memory interning
     * assigns positions speculatively, so any divergence means a second
     * writer raced us — hard error, never a silent remap. */
    {
        struct stat st;
        bool exists = (stat(dom->path, &st) == 0);
        if (disk_count == 0) {
            if (exists && st.st_size > 0) {
                ray_file_unlock(lock_fd);
                ray_file_close(lock_fd);
                return RAY_ERR_CORRUPT; /* concurrent writer created it */
            }
        } else {
            bool ok = exists && (size_t)st.st_size == disk_size;
            if (ok) {
                FILE* vf = fopen(dom->path, "rb");
                uint32_t magic = 0;
                int64_t fcount = -1;
                if (!vf || fread(&magic, 4, 1, vf) != 1 ||
                    fread(&fcount, 8, 1, vf) != 1 ||
                    magic != DOMAIN_STRL_MAGIC || fcount != disk_count)
                    ok = false;
                if (vf) fclose(vf);
            }
            if (!ok) {
                ray_file_unlock(lock_fd);
                ray_file_close(lock_fd);
                return RAY_ERR_CORRUPT; /* concurrent writer */
            }
        }
    }

    /* Write base + tail to tmp. */
    size_t written_size = 0;
    {
        FILE* f = fopen(tmp_path, "wb");
        if (!f) {
            ray_file_unlock(lock_fd);
            ray_file_close(lock_fd);
            return RAY_ERR_IO;
        }
        uint32_t magic = DOMAIN_STRL_MAGIC;
        err = RAY_OK;
        if (fwrite(&magic, 4, 1, f) != 1 || fwrite(&count, 8, 1, f) != 1)
            err = RAY_ERR_IO;
        written_size = 12;
        for (int64_t i = 0; err == RAY_OK && i < count; i++) {
            ray_t* s = atoms[i];
            size_t slen = ray_str_len(s);
            if (slen > UINT32_MAX) { err = RAY_ERR_RANGE; break; }
            uint32_t len32 = (uint32_t)slen;
            if (fwrite(&len32, 4, 1, f) != 1 ||
                (slen > 0 && fwrite(ray_str_ptr(s), 1, slen, f) != slen)) {
                err = RAY_ERR_IO;
                break;
            }
            written_size += 4 + slen;
        }
        if (fclose(f) != 0 && err == RAY_OK) err = RAY_ERR_IO;
    }
    if (err != RAY_OK) goto fail_tmp;

    if (durable) {
        ray_fd_t tmp_fd = ray_file_open(tmp_path, RAY_OPEN_READ | RAY_OPEN_WRITE);
        if (tmp_fd == RAY_FD_INVALID) { err = RAY_ERR_IO; goto fail_tmp; }
        err = ray_file_sync(tmp_fd);
        ray_file_close(tmp_fd);
        if (err != RAY_OK) goto fail_tmp;
    }

    err = ray_file_rename(tmp_path, dom->path);
    if (err != RAY_OK) goto fail_tmp;

    if (durable) {
        err = ray_file_sync_dir(dom->path);
        if (err != RAY_OK) {
            ray_file_unlock(lock_fd);
            ray_file_close(lock_fd);
            return err;
        }
    }

    dom_lock();
    dom->disk_count = count;
    dom->disk_size = written_size;
    dom_unlock();

    ray_file_unlock(lock_fd);
    ray_file_close(lock_fd);
    return RAY_OK;

fail_tmp:
    remove(tmp_path);
    ray_file_unlock(lock_fd);
    ray_file_close(lock_fd);
    return err;
}

/* ---- RAY_SYM_AUDIT ----------------------------------------------------------- */

/* Cached at ray_sym_init (table/sym.c) from the RAY_SYM_AUDIT env var. */
uint8_t ray_g_sym_audit = 0;

void ray_sym_audit_cell(ray_t* vec, int64_t row, int64_t pos, ray_t* resolved) {
    ray_sym_domain_t* dom = ray_sym_vec_domain(vec);
    int64_t count = ray_sym_domain_count(dom);
    if (resolved != NULL && pos >= 0 && pos < count) return;

    fprintf(stderr,
            "rayforce: RAY_SYM_AUDIT violation: vec=%p type=%d attrs=0x%02x "
            "len=%lld row=%lld pos=%lld domain=%p (%s) count=%lld resolved=%p\n",
            (void*)vec, (int)vec->type, (unsigned)vec->attrs,
            (long long)vec->len, (long long)row, (long long)pos,
            (void*)dom,
            dom == ray_sym_runtime_domain() ? "runtime"
                : (ray_sym_domain_path(dom) ? ray_sym_domain_path(dom) : "file"),
            (long long)count, (void*)resolved);
    abort();
}
