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
 * Concurrency: one global mutex guards the cache list, FILE-domain
 * refcounts, appends, the lazily built reverse index and the runtime-id
 * LUT build. Bulk imports can hold it while growing a large dictionary;
 * contending workers sleep instead of spending CPU time spinning. The
 * hot read paths are LOCK-FREE: ray_sym_domain_str reads the
 * atomically published (count, atoms) pair — appends publish a slot
 * write with a release store of the count, growth REPLACES the array —
 * ray_sym_domain_raw_pin copies the published (map, checkpoints, count)
 * record, and ray_sym_domain_runtime_lut reads an atomically published
 * array (invalidated and rebuilt across appends).  The runtime singleton
 * never touches the lock (immortal, delegates to sym.c's own locking).
 *
 * What happens to a REPLACED array, record or mapping depends on who can
 * still be reading it:
 *   - append paths (dom_append_locked, the batch commit): import workers
 *     probe a snapshot outside the lock while another worker commits, so
 *     the old one is RETIRED (dom_retire) and freed with the domain; a
 *     stale pointer is always valid memory covering every position below
 *     its publish count.
 *   - external extend (dom_extend_from_file_locked: the symfile grew under
 *     another process, reopened on a cache hit): FREED at once
 *     (dom_drop_locked), unless a batch intern is in flight, then retired.
 *     Safe because a reopen runs under the lock on the calling thread
 *     between kernels: the pool is fork-join (the main thread is worker 0,
 *     a dispatch drains before it returns), the IPC loop is one thread,
 *     every reopen site runs before a dispatch, and the one lock-free
 *     reader that outlives a lock release — the batch probe — is counted
 *     in batch_inflight.  The flush paths (ray_sym_domain_flush, _append)
 *     also read `atoms` after releasing the lock, but only while
 *     count != disk_count, and an extend refuses a dirty domain, so no
 *     extend can run while they read.  Retiring here too was #717: a
 *     replica reopening
 *     on a timer never drops its last reference, so every reopen leaked a
 *     generation.  The contract this rests on is in domain.h at
 *     ray_sym_domain_raw_pin: no pin across a reopen, no reopen under a
 *     running kernel.
 */

#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
#  define _GNU_SOURCE       /* realpath */
#endif

#include "domain.h"
#include "core/profile.h"   /* phase ticks of the batch intern */
#include "core/platform.h"  /* ray_vm_map_file / ray_vm_unmap_file */
#include "mem/heap.h"
#include "mem/sys.h"   /* domain buffers are process-global → mmap-backed, not buddy */
#include "mem/arena.h"   /* ray_arena_t / ray_arena_str — domain atom storage */
#include "store/part.h"     /* discard unused partition-owned FILE domains */
#include "store/fileio.h"   /* flock + tmp/rename protocol for flush */
#include "ops/hash.h"       /* ray_hash_bytes (same hash family as g_sym) */
#include "core/pool.h"      /* batch intern: parallel read-only probe */
#include "sym.h"            /* ray_sym_intern_prehashed (runtime fallback) */
#include "symimp.h"         /* DOM_IMPORT */
#include "store/enccol.h"   /* ray_enccol_of: a generation-2 SYM column's domain */
#include <stdatomic.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <libgen.h>
#include <sys/stat.h>
#include <limits.h>
#if defined(RAY_OS_WINDOWS)
#include <windows.h>
#elif !defined(RAY_OS_WASM)
#include <pthread.h>
#endif
#include <errno.h>
#if defined(RAY_OS_WINDOWS)
#include <process.h>
#define dom_pid() ((unsigned long long)_getpid())
#else
#include <unistd.h>
#include <fcntl.h>                 /* dom_src_open: explicit reads */
#include <dirent.h>
#include <signal.h>                /* kill(pid, 0): a temporary file's writer still runs */
#define dom_pid() ((unsigned long long)getpid())
#endif
#ifndef PATH_MAX
#  define PATH_MAX 4096
#endif

/* Same on-disk format constant as src/table/sym.c (private there). */
#define DOMAIN_STRL_MAGIC 0x4C525453U /* "STRL" */

typedef enum {
    DOM_RUNTIME = 0,
    DOM_FILE    = 1,
    /* A symbol file one converter builds alone (ray_symimp_t): intern,
     * count, flush and release only. */
    DOM_IMPORT  = 2,
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
/* ---- sparse record index ---------------------------------------------------
 *
 * A symfile's records are variable-length, so finding entry `pos` needs the
 * byte offset of a record at or before it.  The domain keeps a block table:
 * blocks of DOM_IX_STRIDE (28) entries, each one cache line (64 bytes): the
 * offset of its first record (a checkpoint) and the length of each entry's
 * string in two bytes (65535: 65535 or longer, the record's own u32 then
 * says).  Entry j of block pos / 28 is found from the checkpoint by adding up
 * the records before it — from the lengths, a 65535 one from its record
 * header — so a lookup touches one cache line of the table and reads the
 * record it wants and nothing before it, and a string's length needs no
 * record at all unless it is 65535 or longer (ray_sym_domain_raw_len).
 *
 * The table is persisted next to the symfile, in `<symfile>.ix`, so that
 * opening a domain reads neither the records nor the table: only the 128-byte
 * header; the blocks are mapped and paged in as entries are read.  An index
 * is used only for the very file it was written for: the inode, the
 * modification and status-change times and the size it records match the
 * mapped file's.  Anything else (another file of the name, the file grown
 * or rewritten by a writer that left the index as it was, an earlier
 * format) is not used: the open walks every record once, reading the file
 * in large explicit reads (dom_src_t), and writes the index anew.  The
 * symfile and its index are ours: an index that names the file is read as
 * written, with no check of its blocks or of the records.
 *
 * The index file (all integers little-endian):
 *
 *   0  u32 "STRX"   4 u32 version   8 u32 stride (28)   12 u32 0
 *  16  i64 count: entries covered   24 u64 end: byte offset past their records
 *  32  u64 inode, 40 u64 stamp of the modification and status-change times
 *      of the file it was written for
 *  48  u64 checkpoint of the last block (n - 1)
 *  56  u32 0   60 u32 0   64 u32 0   68 u32 0
 *  72  u64 cap: block slots               80 .. 127: zeros
 * 128  n blocks of 64 bytes, n = ceil(count / 28), then zeros up to cap:
 *      u64 offset of the block's first record, then 28 u16 lengths (those
 *      past `count` in the last block: 0)
 *
 * The symfile is append-only, and so is the block array: a writer that
 * appended records writes the block the old entries ended in and the
 * blocks they open into the reserved slots, then the header — the cost of
 * what it appended — and writes the index whole (tmp + rename, with room
 * for more blocks) when the slots run out or the index on disk is not the
 * one it last read or wrote.  It is a cache, never the only copy of
 * anything: never synced. */
#define DOM_IX_STRIDE  ((int64_t)RAY_SYM_INDEX_STRIDE)
typedef ray_sym_ixblk_t dom_ixb_t;
#define DOM_IX_MAGIC   0x58525453U   /* "STRX" */
#define DOM_IX_VERSION 5u
#define DOM_IX_HDR     128           /* the blocks start on a cache line */
#define DOM_IX_HEAD    4096          /* an extend compares this many bytes of the first records */
#define DOM_LEN_LONG   65535         /* a length of 65535: the escape, 65535 or longer, its record header says */

/* The block table of an image of `count` entries whose records end at
 * `end`; one allocation.  The entries' lengths: this allocation's own (a
 * walk), or the mapped index file (the domain keeps the mapping until it
 * dies). */
typedef struct dom_blk_s {
    int64_t        n;
    int64_t        count;
    uint64_t       end;
    const dom_ixb_t* ib;       /* block k: starts at map + ib[k].off, its entries' lengths */
    dom_ixb_t*     own_ib;     /* ib, when this allocation holds it (a walk) */
} dom_blk_t;

typedef struct dom_raw_snap_s {
    const unsigned char* map;
    dom_blk_t*           blk;
    int64_t              count;
} dom_raw_snap_t;

/* Growable checkpoint list (sys memory: process-global, like the domain). */
typedef struct {
    uint64_t* v;
    int64_t   n, cap;
} dom_ck_t;

/* ---- atom slots --------------------------------------------------------------
 *
 * atoms[pos] is reached through a directory of fixed-size slot chunks, a
 * chunk allocated (zeroed, from the domain arena) the first time a slot in
 * it is set.  A flat array of one pointer per entry would commit 8 bytes per
 * entry of the vocabulary at open, materialized or not. */
#define DOM_SLOT_LOG 11
#define DOM_SLOTS    ((int64_t)1 << DOM_SLOT_LOG)
typedef struct {
    _Atomic(ray_t*) a[DOM_SLOTS];
} dom_slots_t;
typedef struct {
    int64_t                n;     /* chunk pointers: covers n * DOM_SLOTS slots */
    _Atomic(dom_slots_t*)  c[];
} dom_dir_t;

struct ray_sym_domain_s {
    uint8_t   kind;        /* dom_kind_t */
    bool      immortal;    /* runtime singleton: retain/release skip */
    uint32_t  rc;          /* FILE: open/attach refs (guarded by lock) */
    char*     path;        /* FILE: malloc'd resolved path; NULL for runtime */

    /* FILE: base mapping of the symfile as of open (NULL for a freshly
     * created domain with no file yet). */
    void*     map;
    size_t    map_size;
    ray_file_id_t map_id;  /* the mapped file's identity (from its descriptor) */
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
     * the array via atomic store; an append retires the old one, an external
     * extend frees it (see the concurrency note above).  Lock-free readers
     * load count (acquire) then atoms (acquire) then the slot (acquire). */
    _Atomic(int64_t)  count;
    _Atomic(dom_dir_t*) atoms;
    int64_t   atoms_cap;   /* slots the directory covers; guarded by g_dom_lock */

    /* FILE: per-domain ray_arena_t backing every materialized string atom
     * (lazy file slots + runtime-appended interns).  Atoms carry
     * RAY_ATTR_ARENA (refcount-free); ray_arena_destroy bulk-frees them.
     * Guarded by g_dom_lock (only ever touched under it). */
    ray_arena_t* arena;

    /* FILE: block table of the mapped records, the file prefix
     * [0, base_count) (see the sparse record index above): from the index
     * file or one walk at open.  Read under g_dom_lock; lock-free readers
     * take it from the raw snapshot.  NULL when base_count == 0. */
    dom_blk_t* blk;
    /* FILE: what the index did for this domain (ray_sym_domain_ix_info). */
    bool      ix_used;          /* the open took its blocks from the index */
    /* FILE: checkpoints of the entries this process flushed past the
     * mapping, positions in [base_count, disk_count) that are multiples of
     * the stride, offsets in the file as written.  Guarded by the symfile's
     * `.lk` flock (the flush paths), published under g_dom_lock. */
    dom_ck_t  wck;
    /* FILE: entries, and the byte offset past their records, the index
     * file covered when this process last read or wrote it. */
    _Atomic(int64_t)  ix_count;
    _Atomic(uint64_t) ix_end;
    int64_t   ix_covered;   /* entries the open (or extend) took from the index */
    /* Published raw snapshot (see ray_sym_domain_raw_pin): map, checkpoints
     * and the file-prefix count as one immutable record.  Replaced whole on
     * extend; the previous record, its checkpoints and its mapping are freed
     * then, or retired (retired / retired_maps) while a batch intern is
     * probing outside the lock — see the concurrency note above. */
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
    /* Batch interns currently probing a snapshot of `buckets` outside the
     * lock (guarded by g_dom_lock).  While non-zero, whatever an extend
     * or an index rebuild replaces is retired instead of freed. */
    int32_t   batch_inflight;

    dom_retired_t* retired; /* replaced directories, checkpoints, snapshot records, LUTs */

    struct ray_symimp_s* imp; /* DOM_IMPORT: the import dictionary */

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

#if defined(RAY_OS_WINDOWS)
static SRWLOCK g_dom_lock = SRWLOCK_INIT;
static inline void dom_lock(void) {
    AcquireSRWLockExclusive(&g_dom_lock);
}
static inline void dom_unlock(void) {
    ReleaseSRWLockExclusive(&g_dom_lock);
}
#elif defined(RAY_OS_WASM)
/* The WASM runtime is single-threaded. */
static inline void dom_lock(void) {}
static inline void dom_unlock(void) {}
#else
static pthread_mutex_t g_dom_lock = PTHREAD_MUTEX_INITIALIZER;
static inline void dom_lock(void) { pthread_mutex_lock(&g_dom_lock); }
static inline void dom_unlock(void) { pthread_mutex_unlock(&g_dom_lock); }
#endif

/* ---- FILE domain construction / destruction ------------------------------- */

/* realpath(3) contract: absolute path of an EXISTING file, else NULL.
 * Windows has no realpath; _fullpath only normalizes (it succeeds for
 * missing files too), so existence is checked separately. */
static char* dom_realpath(const char* path, char resolved[PATH_MAX]) {
#if defined(RAY_OS_WINDOWS)
    if (!_fullpath(resolved, path, PATH_MAX)) return NULL;
    if (GetFileAttributesA(resolved) == INVALID_FILE_ATTRIBUTES) return NULL;
    return resolved;
#else
    return realpath(path, resolved);
#endif
}

/* Do two resolved paths name the same file?  On Windows one file is
 * reachable as C:\db\sym, C:\db/sym or C:\DB\Sym (NTFS is case-insensitive
 * and _fullpath keeps the caller's separators and case), so compare with
 * '\\' == '/' and ASCII case folded — a plain strcmp would key one symfile
 * twice and open two diverging domains for it.  The paths themselves are
 * left as given: they are also used to create the file. */
static bool dom_path_eq(const char* a, const char* b) {
#if defined(RAY_OS_WINDOWS)
    for (;; a++, b++) {
        char ca = *a == '\\' ? '/' : *a;
        char cb = *b == '\\' ? '/' : *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        if (!ca) return true;
    }
#else
    return strcmp(a, b) == 0;
#endif
}

/* Resolved cache key for `path`.  realpath of the file when it exists;
 * for to-be-created symfiles, realpath of the parent + "/" + basename
 * (the parent must exist).  malloc'd. */
static char* dom_resolve_path(const char* path) {
    /* Fixed-buffer realpath (POSIX form) + ray_alloc_raw for the result — avoids
     * realpath(NULL)/strdup's libc-malloc'd buffers, so the returned key is
     * uniformly buddy-allocated and the caller releases it with ray_free_raw. */
    char resolved[PATH_MAX];
    if (dom_realpath(path, resolved)) {
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
    if (!dom_realpath(dir, rdir)) return NULL;
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

/* Drop something a lock-free reader of this domain MAY hold: freed at once
 * unless a batch intern is probing a snapshot outside the lock, then
 * retired with the domain.  Correct only where no other reader can be
 * holding it -- the reverse index (probed by batch interns alone) and the
 * generation replaced by an external extend (see the concurrency note at
 * the top of the file: a reopen runs between kernels, and a pin does not
 * survive one).  The append paths keep dom_retire: their readers are the
 * concurrent import workers. */
static bool dom_drop_locked(ray_sym_domain_t* d, void* old) {
    if (!old) return true;
    if (d->batch_inflight == 0) { ray_sys_free(old); return true; }
    return dom_retire(d, old);
}
static bool dom_drop_buckets_locked(ray_sym_domain_t* d, uint64_t* old) {
    return dom_drop_locked(d, old);
}

/* Publish (map, block table, count) as the domain's raw snapshot.  The
 * previous record is dropped (external extend: no reader holds it) or
 * retired.  Called with the file's current image in place. */
static bool dom_publish_raw_snap(ray_sym_domain_t* d, bool drop_old) {
    dom_raw_snap_t* snap = NULL;
    if (d->map && d->blk && d->base_count > 0) {
        snap = (dom_raw_snap_t*)ray_sys_alloc(sizeof(*snap));
        if (!snap) return false;
        snap->map = (const unsigned char*)d->map;
        snap->blk = d->blk;
        snap->count = d->base_count;
    }
    dom_raw_snap_t* old = atomic_exchange_explicit(&d->raw_snap, snap, memory_order_acq_rel);
    if (old) (void)(drop_old ? dom_drop_locked(d, old) : dom_retire(d, old));
    return true;
}

/* ---- atom slots ---------------------------------------------------------- */

/* A directory covering `slots` slots (whole chunks), carrying the chunks of
 * `old`.  NULL on OOM.  Fresh pointers are NULL (sys memory is zeroed). */
static dom_dir_t* dom_dir_new(int64_t slots, dom_dir_t* old) {
    int64_t n = (slots + DOM_SLOTS - 1) >> DOM_SLOT_LOG;
    if (n < 1) n = 1;
    dom_dir_t* dir = (dom_dir_t*)ray_sys_alloc(sizeof(dom_dir_t) +
                                               (size_t)n * sizeof(dom_slots_t*));
    if (!dir) return NULL;
    dir->n = n;
    int64_t keep = old ? (old->n < n ? old->n : n) : 0;
    for (int64_t k = 0; k < keep; k++)
        atomic_store_explicit(&dir->c[k],
                              atomic_load_explicit(&old->c[k], memory_order_relaxed),
                              memory_order_relaxed);
    return dir;
}

/* Slot `pos` of `dir`: the atom, or NULL (not materialized, or a chunk this
 * directory has not seen).  Lock-free. */
static inline ray_t* dom_slot_get(dom_dir_t* dir, int64_t pos) {
    if (!dir || pos < 0 || (pos >> DOM_SLOT_LOG) >= dir->n) return NULL;
    dom_slots_t* c = atomic_load_explicit(&dir->c[pos >> DOM_SLOT_LOG], memory_order_acquire);
    return c ? atomic_load_explicit(&c->a[pos & (DOM_SLOTS - 1)], memory_order_acquire)
             : NULL;
}

/* The current directory's chunk holding slot `pos` (< atoms_cap), made in
 * the arena when absent.  Under g_dom_lock.  NULL on OOM. */
static dom_slots_t* dom_chunk_locked(ray_sym_domain_t* d, int64_t pos) {
    dom_dir_t* dir = atomic_load_explicit(&d->atoms, memory_order_relaxed);
    _Atomic(dom_slots_t*)* cp = &dir->c[pos >> DOM_SLOT_LOG];
    dom_slots_t* c = atomic_load_explicit(cp, memory_order_relaxed);
    if (c) return c;
    c = (dom_slots_t*)ray_arena_alloc_raw(d->arena, sizeof(dom_slots_t));
    if (!c) return NULL;
    for (int64_t i = 0; i < DOM_SLOTS; i++)
        atomic_store_explicit(&c->a[i], NULL, memory_order_relaxed);
    atomic_store_explicit(cp, c, memory_order_release);
    return c;
}

static bool dom_slot_set_locked(ray_sym_domain_t* d, int64_t pos, ray_t* a) {
    dom_slots_t* c = dom_chunk_locked(d, pos);
    if (!c) return false;
    atomic_store_explicit(&c->a[pos & (DOM_SLOTS - 1)], a, memory_order_release);
    return true;
}

/* Cover `need` slots: a grown directory REPLACES the current one (lock-free
 * readers may hold it, so it is retired).  Under g_dom_lock. */
static bool dom_grow_locked(ray_sym_domain_t* d, int64_t need) {
    if (need <= d->atoms_cap) return true;
    int64_t ncap = d->atoms_cap < DOM_SLOTS ? DOM_SLOTS : d->atoms_cap;
    while (ncap < need) ncap *= 2;
    dom_dir_t* old = atomic_load_explicit(&d->atoms, memory_order_relaxed);
    dom_dir_t* dir = dom_dir_new(ncap, old);
    if (!dir) return false;
    if (!dom_retire(d, old)) { ray_sys_free(dir); return false; }
    atomic_store_explicit(&d->atoms, dir, memory_order_release);
    d->atoms_cap = dir->n << DOM_SLOT_LOG;
    return true;
}

/* ---- records and blocks ------------------------------------------------------ */

static bool dom_ck_reserve(dom_ck_t* ck, int64_t want) {
    if (want <= ck->cap) return true;
    uint64_t* nv = (uint64_t*)ray_sys_alloc((size_t)want * sizeof(uint64_t));
    if (!nv) return false;
    if (ck->n) memcpy(nv, ck->v, (size_t)ck->n * sizeof(uint64_t));
    ray_sys_free(ck->v);
    ck->v = nv;
    ck->cap = want;
    return true;
}

static bool dom_ck_push(dom_ck_t* ck, uint64_t off) {
    if (ck->n == ck->cap && !dom_ck_reserve(ck, ck->cap < 64 ? 64 : ck->cap * 2))
        return false;
    ck->v[ck->n++] = off;
    return true;
}

static void dom_ck_free(dom_ck_t* ck) {
    ray_sys_free(ck->v);
    ck->v = NULL;
    ck->n = ck->cap = 0;
}

static inline int64_t dom_ck_count(int64_t entries) {
    return (entries + DOM_IX_STRIDE - 1) / DOM_IX_STRIDE;
}

static inline uint32_t dom_rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t dom_rd64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* A block table for `count` entries; with room for its own checkpoints
 * and lengths when `own` (else both are the index file's, set by the
 * caller).  NULL on OOM. */
static dom_blk_t* dom_blk_new(int64_t count, bool own) {
    int64_t n = dom_ck_count(count);
    size_t head = (sizeof(dom_blk_t) + 7) & ~(size_t)7;
    size_t ibb = own ? (size_t)n * sizeof(dom_ixb_t) : 0;
    dom_blk_t* b = (dom_blk_t*)ray_sys_alloc(head + ibb);
    if (!b) return NULL;
    uint8_t* p = (uint8_t*)b + head;
    b->n = n;
    b->count = count;
    b->end = 0;
    b->own_ib = own ? (dom_ixb_t*)p : NULL;
    b->ib = b->own_ib;
    if (own) memset(b->own_ib, 0, ibb);   /* unused lengths of the last block: 0 */
    return b;
}

static inline uint64_t dom_blk_lim(const dom_blk_t* b, int64_t k) {
    return k + 1 < b->n ? b->ib[k + 1].off : b->end;
}

/* Where a walk reads the records: the mapping, or the file read
 * with explicit, large reads into a buffer (dom_src_open).  A walk over
 * much of the file through the mapping faults it in a page cluster at a
 * time (some 128 KiB a request) and leaves the pages mapped — kept by the
 * reclaim as referenced, so a walk over a file larger than the memory
 * thrashes its own read-ahead; read() into a buffer takes it in 4 MiB
 * requests, each page read once. */
#define DOM_RD_CHUNK ((size_t)4 << 20)
typedef struct {
    const uint8_t* map;      /* the mapping (no reads), or NULL */
    int            fd;
    uint8_t*       buf;
    size_t         cap;
    uint64_t       lo, hi;   /* the file bytes buf holds */
    uint64_t       size;     /* the image's bytes */
} dom_src_t;

static inline dom_src_t dom_src_map(const uint8_t* map, uint64_t size) {
    dom_src_t s;
    memset(&s, 0, sizeof(s));
    s.map = map;
    s.fd = -1;
    s.size = size;
    return s;
}

/* Explicit reads of the file `path` — only if it is still the file of
 * identity `id` that is mapped (size `size`).  False where it cannot be:
 * the caller reads through the mapping. */
static bool dom_src_open(dom_src_t* s, const char* path, const ray_file_id_t* id, uint64_t size) {
    memset(s, 0, sizeof(*s));
    s->fd = -1;
    s->size = size;
#if !defined(RAY_OS_WINDOWS) && !defined(RAY_OS_WASM)
    if (!path || !id || !id->valid) return false;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || (uint64_t)st.st_ino != id->ino || (uint64_t)st.st_size != size) {
        close(fd);
        return false;
    }
#if defined(POSIX_FADV_SEQUENTIAL)
    (void)posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
    s->fd = fd;
    return true;
#else
    (void)path; (void)id;
    return false;
#endif
}

static void dom_src_close(dom_src_t* s) {
#if !defined(RAY_OS_WINDOWS) && !defined(RAY_OS_WASM)
    if (s->fd >= 0) close(s->fd);
#endif
    ray_sys_free(s->buf);
    s->buf = NULL;
    s->fd = -1;
}

/* Bytes [off, off + len) of the image: in the mapping, or read into the
 * buffer (from `off`, at least a chunk).  NULL past the image or on a read
 * error. */
static const uint8_t* dom_src_get(dom_src_t* s, uint64_t off, uint64_t len) {
    if (off > s->size || len > s->size - off) return NULL;
    if (s->map) return s->map + off;
    if (off >= s->lo && off + len <= s->hi) return s->buf + (off - s->lo);
#if !defined(RAY_OS_WINDOWS) && !defined(RAY_OS_WASM)
    uint64_t n = len > DOM_RD_CHUNK ? len : DOM_RD_CHUNK;
    if (n > s->size - off) n = s->size - off;
    if (n > s->cap) {
        uint8_t* nb = (uint8_t*)ray_sys_alloc((size_t)n);
        if (!nb) return NULL;
        ray_sys_free(s->buf);
        s->buf = nb;
        s->cap = (size_t)n;
    }
    uint64_t got = 0;
    while (got < n) {
        ssize_t r = pread(s->fd, s->buf + got, (size_t)(n - got), (off_t)(off + got));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { s->lo = s->hi = 0; return NULL; }
        got += (uint64_t)r;
    }
#if defined(POSIX_FADV_WILLNEED)
    /* the next chunk is read while this one is checked; the one just
     * read is not needed again (pages the mapping holds stay) */
    if (off + n < s->size)
        (void)posix_fadvise(s->fd, (off_t)(off + n), (off_t)DOM_RD_CHUNK, POSIX_FADV_WILLNEED);
    if (s->hi > s->lo)
        (void)posix_fadvise(s->fd, (off_t)s->lo, (off_t)(s->hi - s->lo), POSIX_FADV_DONTNEED);
#endif
    s->lo = off;
    s->hi = off + n;
    return s->buf;
#else
    return NULL;
#endif
}

/* Walk entries [from, b->count) of the image, the first at byte *off: the
 * checkpoint of every block that starts on the way and, into the table's own
 * lengths, the length of every entry.  False when a read of the file fails. */
static bool dom_blk_walk_src(dom_src_t* src, dom_blk_t* b, int64_t from, size_t* off) {
    uint64_t o = *off;
    for (int64_t i = from; i < b->count; i++) {
        int64_t k = i / DOM_IX_STRIDE, j = i % DOM_IX_STRIDE;
        if (!j) b->own_ib[k].off = o;
        const uint8_t* h = dom_src_get(src, o, 4);
        if (!h) return false;
        uint32_t l = dom_rd32(h);
        o += 4 + (uint64_t)l;
        b->own_ib[k].len[j] = l < DOM_LEN_LONG ? (uint16_t)l : (uint16_t)DOM_LEN_LONG;
    }
    *off = (size_t)o;
    return true;
}

/* Entry `pos`: from its block's checkpoint past the records before it, by
 * their lengths (a 65535 one by its header). */
static inline const uint8_t* dom_blk_entry(const uint8_t* map, const dom_blk_t* b,
                                           int64_t pos, uint32_t* len) {
    int64_t k = pos / DOM_IX_STRIDE, j = pos % DOM_IX_STRIDE;
    const uint8_t* p = map + b->ib[k].off;
    for (int64_t i = 0; i < j; i++) {
        uint16_t l = b->ib[k].len[i];
        p += 4 + (l < DOM_LEN_LONG ? (size_t)l : (size_t)dom_rd32(p));
    }
    *len = dom_rd32(p);
    return p + 4;
}

static void dom_blk_save(const char* path, const uint8_t* map, size_t map_size,
                         const dom_blk_t* b, const ray_file_id_t* id);
static size_t dom_ix_min(void);

/* Entry `pos` of the mapped prefix.  Under g_dom_lock.  NULL when out of
 * range. */
static const uint8_t* dom_entry_locked(ray_sym_domain_t* d, int64_t pos, uint32_t* len) {
    if (pos < 0 || pos >= d->base_count || !d->blk) return NULL;
    return dom_blk_entry((const uint8_t*)d->map, d->blk, pos, len);
}

/* Bytes of entry `pos` in order, for a pass over the whole vocabulary
 * (pos = 0, 1, ..., *off starting at 12): the mapped records read in turn,
 * then the appended atoms.  Under g_dom_lock. */
static bool dom_next_locked(ray_sym_domain_t* d, int64_t pos, size_t* off,
                            const char** p, size_t* len) {
    if (pos < d->base_count) {
        size_t o = *off;
        uint32_t l = dom_rd32((const uint8_t*)d->map + o);
        *p = (const char*)d->map + o + 4;
        *len = l;
        *off = o + 4 + l;
        return true;
    }
    ray_t* a = dom_slot_get(atomic_load_explicit(&d->atoms, memory_order_relaxed), pos);
    if (!a) return false;
    *p = ray_str_ptr(a);
    *len = ray_str_len(a);
    return true;
}

/* Bytes of entry `pos`: its atom when there is one, else its record in the
 * mapping — nothing materialized.  Under g_dom_lock.  NULL if unresolvable. */
static const char* dom_bytes_locked(ray_sym_domain_t* d, int64_t pos, size_t* len) {
    ray_t* a = dom_slot_get(atomic_load_explicit(&d->atoms, memory_order_relaxed), pos);
    if (a) { *len = ray_str_len(a); return ray_str_ptr(a); }
    uint32_t l = 0;
    const uint8_t* p = dom_entry_locked(d, pos, &l);
    *len = l;
    return (const char*)p;
}

/* Free a FILE domain after the last reference dropped.  Caller has
 * already unlinked it from the cache. */
static void dom_destroy(ray_sym_domain_t* d) {
    if (d->imp) ray_symimp_free(d->imp);
    /* Atoms and slot chunks are arena memory — never individually freed.
     * The arena bulk-frees them; only the directory is freed here. */
    ray_sys_free(atomic_load_explicit(&d->atoms, memory_order_relaxed));
    if (d->arena) ray_arena_destroy(d->arena);
    ray_sys_free(d->blk);
    dom_ck_free(&d->wck);
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

/* ---- the index file ------------------------------------------------------- */

/* Symfiles smaller than this get no index file: walking them at open costs
 * less than reading one.  RAY_SYM_INDEX_MIN overrides it (bytes). */
static size_t dom_ix_min(void) {
    const char* e = getenv("RAY_SYM_INDEX_MIN");
    if (e && *e) return (size_t)strtoull(e, NULL, 10);
    return (size_t)1 << 20;
}

static bool dom_ix_name(char* out, size_t cap, const char* sym, const char* suffix) {
    int n = snprintf(out, cap, "%s.ix%s", sym, suffix);
    return n > 0 && (size_t)n < cap;
}

static bool dom_fseek_ix(FILE* f, int64_t off) {
#ifdef RAY_OS_WINDOWS
    return _fseeki64(f, off, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)off, SEEK_SET) == 0;
#endif
}

/* Remove the temporary index files (`<sym>.ix.<pid>.tmp`) a process that
 * no longer runs left behind. */
static void dom_ix_sweep_tmp(const char* path) {
#if !defined(RAY_OS_WINDOWS) && !defined(RAY_OS_WASM)
    char dir[PATH_MAX], pre[PATH_MAX];
    const char* slash = strrchr(path, '/');
    const char* base = slash ? slash + 1 : path;
    size_t dl = slash ? (size_t)(slash - path) : 0;
    if (dl >= sizeof(dir)) return;
    if (slash) { memcpy(dir, path, dl); dir[dl] = 0; } else { dir[0] = '.'; dir[1] = 0; }
    int prl = snprintf(pre, sizeof(pre), "%s.ix.", base);
    if (prl <= 0 || (size_t)prl >= sizeof(pre)) return;
    DIR* dh = opendir(dir);
    if (!dh) return;
    struct dirent* e;
    while ((e = readdir(dh))) {
        size_t el = strlen(e->d_name);
        if (el <= (size_t)prl + 4 || strncmp(e->d_name, pre, (size_t)prl) != 0 ||
            strcmp(e->d_name + el - 4, ".tmp") != 0) continue;
        char* stop = NULL;
        long long pid = strtoll(e->d_name + prl, &stop, 10);
        if (pid <= 0 || stop != e->d_name + el - 4) continue;
        if ((unsigned long long)pid == dom_pid()) continue;
        if (kill((pid_t)pid, 0) == 0 || errno != ESRCH) continue;
        char full[PATH_MAX];
        if (snprintf(full, sizeof(full), "%s/%s", dir, e->d_name) < (int)sizeof(full))
            (void)unlink(full);
    }
    closedir(dh);
#else
    (void)path;
#endif
}

/* The identity an index records for the file it was written for: the inode
 * and one value of the modification and status-change times (the latter
 * cannot be set back by a writer: an in-place rewrite always moves it). */
static inline uint64_t dom_id_stamp(const ray_file_id_t* id) {
    return (uint64_t)id->mtime_ns ^ ((uint64_t)id->ctime_ns * UINT64_C(0x9E3779B97F4A7C15));
}

/* The index file's header, parsed. */
typedef struct {
    int64_t  count;       /* entries covered */
    uint64_t end;         /* byte offset past their records */
    uint64_t ino, stamp;  /* the file it was written for (both 0: none claimed) */
    uint64_t last_ckpt;   /* checkpoint of the last block */
    uint64_t cap;         /* block slots */
} dom_ixh_t;



static void dom_ixh_put(uint8_t* h, const dom_ixh_t* x) {
    memset(h, 0, DOM_IX_HDR);
    uint32_t w = DOM_IX_MAGIC;   memcpy(h, &w, 4);
    w = DOM_IX_VERSION;          memcpy(h + 4, &w, 4);
    w = (uint32_t)DOM_IX_STRIDE; memcpy(h + 8, &w, 4);
    memcpy(h + 16, &x->count, 8);
    memcpy(h + 24, &x->end, 8);
    memcpy(h + 32, &x->ino, 8);
    memcpy(h + 40, &x->stamp, 8);
    memcpy(h + 48, &x->last_ckpt, 8);
    memcpy(h + 72, &x->cap, 8);
}

static bool dom_ixh_get(const uint8_t* h, dom_ixh_t* x) {
    if (dom_rd32(h) != DOM_IX_MAGIC || dom_rd32(h + 4) != DOM_IX_VERSION ||
        dom_rd32(h + 8) != (uint32_t)DOM_IX_STRIDE)
        return false;
    memcpy(&x->count, h + 16, 8);
    x->end = dom_rd64(h + 24);
    x->ino = dom_rd64(h + 32);
    x->stamp = dom_rd64(h + 40);
    x->last_ckpt = dom_rd64(h + 48);
    x->cap = dom_rd64(h + 72);
    return x->count > 0 && x->end > 12 && x->cap >= (uint64_t)dom_ck_count(x->count) &&
           x->cap <= (uint64_t)dom_ck_count(x->count) * 2 + 4096;
}

/* Keep a mapping until the domain dies (the index a table points into). */
static void dom_keep_map(ray_sym_domain_t* d, void* m, size_t sz) {
    dom_retired_map_t* rm = (dom_retired_map_t*)ray_sys_alloc(sizeof(*rm));
    if (!rm) return;   /* stays mapped for the process lifetime */
    rm->map = m;
    rm->size = sz;
    rm->next = d->retired_maps;
    d->retired_maps = rm;
}

/* The index of symfile `path` for its mapped image (`size` bytes, `count`
 * entries per its header, identity `id`): a block table pointing at its
 * checkpoints and its lengths in the index file, mapped (*ixmap /
 * *ixsize: for the caller to keep while the table lives).  Taken only when
 * written for this very file — its identity, size and count are the
 * image's — in this format, with its block sections all there.  Reads the
 * header only; the checkpoints and the lengths are paged in as entries are
 * read.  NULL otherwise (or on OOM). */
static dom_blk_t* dom_ix_load(const char* path, const uint8_t* map, size_t size, int64_t count,
                              const ray_file_id_t* id, void** ixmap, size_t* ixsize,
                              dom_ixh_t* hx) {
    char ip[PATH_MAX];
    if (count <= 0 || !path || !id || !id->valid || id->size != size ||
        !dom_ix_name(ip, sizeof(ip), path, ""))
        return NULL;
    FILE* f = fopen(ip, "rb");
    if (!f) return NULL;
    uint8_t h[DOM_IX_HDR];
    dom_ixh_t x;
    bool ok = fread(h, 1, DOM_IX_HDR, f) == DOM_IX_HDR && dom_ixh_get(h, &x) &&
              x.count == count && x.end == size && (x.ino | x.stamp) != 0 &&
              x.ino == id->ino && x.stamp == dom_id_stamp(id);
    fclose(f);
    (void)map;
    dom_blk_t* b = ok ? dom_blk_new(count, false) : NULL;
    void* m = NULL;
    size_t msz = 0;
    ray_file_id_t iid;
    if (b) {
        m = ray_vm_map_file_id(ip, &msz, &iid);
        /* the file mapped is the one read: the same header, every length */
        ok = m && msz >= DOM_IX_HDR + x.cap * sizeof(dom_ixb_t) &&
             memcmp(m, h, DOM_IX_HDR) == 0;
    }
    if (!b || !ok) {
        if (m) ray_vm_unmap_file(m, msz);
        ray_sys_free(b);
        return NULL;
    }
    b->end = size;
    b->ib = (const dom_ixb_t*)((const uint8_t*)m + DOM_IX_HDR);
    *ixmap = m;
    *ixsize = msz;
    *hx = x;
    return b;
}

/* Write `bytes` zero bytes to f. */
static bool dom_write_zeros(FILE* f, uint64_t bytes) {
    static const uint8_t z[4096];
    while (bytes) {
        size_t c = bytes < sizeof(z) ? (size_t)bytes : sizeof(z);
        if (fwrite(z, 1, c, f) != c) return false;
        bytes -= c;
    }
    return true;
}

/* Where a writer takes the entries' lengths from: those of entries below
 * `na` from the blocks `pb` (a table's own or mapped ones), the others from
 * lb[i - na]. */
typedef struct {
    const dom_ixb_t* pb;
    int64_t          na;
    const uint16_t*  lb;
} dom_lsrc_t;

static inline uint16_t dom_ls_get(const dom_lsrc_t* s, int64_t i) {
    return i < s->na ? s->pb[i / DOM_IX_STRIDE].len[i % DOM_IX_STRIDE] : s->lb[i - s->na];
}

/* Blocks [k0, k1) of an index of `count` entries — checkpoints ck[k], lengths
 * from ls (0 past the last entry) — written to f. */
static bool dom_write_blocks(FILE* f, const uint64_t* ck, const dom_lsrc_t* ls, int64_t count,
                             int64_t k0, int64_t k1) {
    enum { CHUNK = 64 };
    dom_ixb_t buf[CHUNK];
    for (int64_t k = k0; k < k1;) {
        int64_t c = k1 - k < CHUNK ? k1 - k : CHUNK;
        memset(buf, 0, sizeof(dom_ixb_t) * (size_t)c);
        for (int64_t q = 0; q < c; q++) {
            buf[q].off = ck[k + q];
            int64_t i0 = (k + q) * DOM_IX_STRIDE;
            for (int64_t j = 0; j < DOM_IX_STRIDE && i0 + j < count; j++)
                buf[q].len[j] = dom_ls_get(ls, i0 + j);
        }
        if (fwrite(buf, sizeof(dom_ixb_t), (size_t)c, f) != (size_t)c) return false;
        k += c;
    }
    return true;
}

/* Write the whole index of symfile `path`, whose `map` holds the records of
 * its first `count` entries, ending at byte `end`, with checkpoints
 * ck[0..n) and the entries' lengths (ls); claimed as the index of the file
 * of identity `id` when given (and of the size `end`); with room for a
 * quarter more blocks; to a temporary file renamed over the index.  Best
 * effort: the index is a cache. */
static void dom_ix_save(const char* path, const uint8_t* map, size_t map_size,
                        const uint64_t* ck, int64_t n, const dom_lsrc_t* ls,
                        int64_t count, uint64_t end, const ray_file_id_t* id) {
    if (!path || count <= 0 || n != dom_ck_count(count) || end > map_size || ck[0] != 12 ||
        ck[n - 1] >= end || (ls->na > 0 && !ls->pb) || (ls->na < count && !ls->lb))
        return;
    char ip[PATH_MAX], tp[PATH_MAX];
    char sfx[32];
    snprintf(sfx, sizeof(sfx), ".%llu.tmp", dom_pid());
    if (!dom_ix_name(ip, sizeof(ip), path, "") || !dom_ix_name(tp, sizeof(tp), path, sfx)) return;
    dom_ixh_t x;
    memset(&x, 0, sizeof(x));
    x.count = count;
    x.end = end;
    if (id && id->valid && id->size == end) {
        x.ino = id->ino;
        x.stamp = dom_id_stamp(id);
    }
    x.last_ckpt = ck[n - 1];
    x.cap = (uint64_t)n + (uint64_t)n / 4 + 64;
    uint8_t h[DOM_IX_HDR];
    dom_ixh_put(h, &x);
    dom_ix_sweep_tmp(path);
    FILE* f = fopen(tp, "wb");
    bool ok = f && fwrite(h, 1, DOM_IX_HDR, f) == DOM_IX_HDR &&
              dom_write_blocks(f, ck, ls, count, 0, n) &&
              dom_write_zeros(f, (x.cap - (uint64_t)n) * sizeof(dom_ixb_t));
    if (f && fclose(f) != 0) ok = false;
    if (ok) ok = ray_file_rename(tp, ip) == RAY_OK;
    if (!ok && f) remove(tp);
}

/* A writer's refresh of the index of `path` after it appended (`count`
 * entries, their records ending at `end`; checkpoints ck[0..n), lengths as
 * dom_ix_save's): when the index on disk is the one this process last read
 * or wrote (covering `ix_count` entries ending at `ix_end`) and has slots
 * for the new blocks, the block the old entries ended in and the new blocks
 * are written into the slots, then the header — the cost of what was
 * appended; otherwise the index is written whole.  The file is mapped for
 * its identity, claimed when `stamp`.  Under the symfile's `.lk` flock.  Best effort. */
static void dom_ix_refresh(const char* path, const uint64_t* ck, int64_t n,
                           const dom_lsrc_t* ls, int64_t count, uint64_t end,
                           int64_t ix_count, uint64_t ix_end, bool stamp) {
    if (end < dom_ix_min() || count <= 0 || n != dom_ck_count(count)) return;
    size_t size = 0;
    ray_file_id_t id;
    uint8_t* map = (uint8_t*)ray_vm_map_file_id(path, &size, &id);
    if (!map) return;
    char ip[PATH_MAX];
    FILE* f = ix_count > 0 && end <= size && dom_ix_name(ip, sizeof(ip), path, "")
            ? fopen(ip, "r+b") : NULL;
    uint8_t h[DOM_IX_HDR];
    dom_ixh_t x;
    bool app = f && fread(h, 1, DOM_IX_HDR, f) == DOM_IX_HDR && dom_ixh_get(h, &x) &&
               x.count == ix_count && x.end == ix_end && x.count <= count &&
               (uint64_t)n <= x.cap && ck[0] == 12 && ck[n - 1] < end;
    if (app) {
        int64_t k0 = dom_ck_count(x.count) - 1;
        x.count = count;
        x.end = end;
        x.ino = x.stamp = 0;
        if (stamp && id.valid && id.size == end) { x.ino = id.ino; x.stamp = dom_id_stamp(&id); }
        x.last_ckpt = ck[n - 1];
        dom_ixh_put(h, &x);
        app = dom_fseek_ix(f, DOM_IX_HDR + k0 * (int64_t)sizeof(dom_ixb_t)) &&
              dom_write_blocks(f, ck, ls, count, k0, n) &&
              fflush(f) == 0 && dom_fseek_ix(f, 0) && fwrite(h, 1, DOM_IX_HDR, f) == DOM_IX_HDR;
    }
    if (f && fclose(f) != 0) app = false;
    if (!app) dom_ix_save(path, map, size, ck, n, ls, count, end, stamp ? &id : NULL);
    ray_vm_unmap_file(map, size);
}

static void dom_ix_save_file(const char* path, const uint64_t* ck, int64_t n,
                             const uint16_t* lens, int64_t count, uint64_t end) {
    if (end < dom_ix_min()) return;
    size_t size = 0;
    ray_file_id_t id;
    void* map = ray_vm_map_file_id(path, &size, &id);
    if (!map) return;
    dom_lsrc_t ls = { NULL, 0, lens };
    dom_ix_save(path, (const uint8_t*)map, size, ck, n, &ls, count, end, &id);
    ray_vm_unmap_file(map, size);
}

/* The index of the table `b` (its own blocks, from a walk): written whole. */
static void dom_blk_save(const char* path, const uint8_t* map, size_t map_size,
                         const dom_blk_t* b, const ray_file_id_t* id) {
    uint64_t* ck = (uint64_t*)ray_sys_alloc((size_t)b->n * sizeof(uint64_t));
    if (!ck) return;
    for (int64_t k = 0; k < b->n; k++) ck[k] = b->ib[k].off;
    dom_lsrc_t ls = { b->ib, b->count, NULL };
    dom_ix_save(path, map, map_size, ck, b->n, &ls, b->count, b->end, id);
    ray_sys_free(ck);
}

void ray_sym_domain_index_save(const char* path, const uint64_t* ckpt, int64_t n,
                               const uint16_t* lens, int64_t count, uint64_t end) {
    if (path && ckpt && lens) dom_ix_save_file(path, ckpt, n, lens, count, end);
}

void ray_sym_domain_index_remove(const char* path) {
    char ip[PATH_MAX];
    if (path && dom_ix_name(ip, sizeof(ip), path, "")) (void)remove(ip);
}

/* The block table of the mapped image of `d` (its `count` entries per the
 * header): from the index file when it is this file's, else by one walk of
 * all the records (read with explicit reads, dom_src_t), the index then
 * written for the file.  False when a read of the file fails (or on OOM). */
static bool dom_index_image(ray_sym_domain_t* d, int64_t count) {
    const uint8_t* map = (const uint8_t*)d->map;
    size_t size = d->map_size;
    dom_ixh_t hx;
    void* ixm = NULL;
    size_t ixs = 0;
    dom_blk_t* b = dom_ix_load(d->path, map, size, count, &d->map_id, &ixm, &ixs, &hx);
    bool have = b != NULL;
    if (have) {
        dom_keep_map(d, ixm, ixs);
    } else {
        b = dom_blk_new(count, true);
        if (!b) return false;
        b->end = size;
        size_t o = 12;
        dom_src_t src;
        if (!dom_src_open(&src, d->path, &d->map_id, size)) src = dom_src_map(map, size);
        bool walked = dom_blk_walk_src(&src, b, 0, &o);
        dom_src_close(&src);
        if (!walked) { ray_sys_free(b); return false; }
    }
    d->blk = b;
    d->ix_used = have;
    d->ix_count = have ? count : 0;
    d->ix_end = have ? size : 0;
    d->ix_covered = have ? count : 0;
    if (!have && count > 0 && size >= dom_ix_min()) {
        dom_blk_save(d->path, map, size, b, &d->map_id);
        d->ix_count = count;
        d->ix_end = size;
    } else if (!have && size < dom_ix_min()) {
        ray_sym_domain_index_remove(d->path);   /* one left by a larger file of the name */
    }
    return true;
}

/* Materialize slot `pos` of the file prefix from its record into the arena
 * and publish it.  CALLER MUST HOLD g_dom_lock.  Idempotent: returns an
 * already-published slot without re-materializing.  Runtime-appended slots
 * (pos >= base_count) are set at append time and never NULL.  Returns NULL
 * on arena OOM. */
static ray_t* dom_atom_at_locked(ray_sym_domain_t* d, int64_t pos) {
    ray_t* a = dom_slot_get(atomic_load_explicit(&d->atoms, memory_order_relaxed), pos);
    if (a) return a;
    if (pos < 0 || pos >= d->base_count) return NULL;
    uint32_t slen = 0;
    const uint8_t* p = dom_entry_locked(d, pos, &slen);
    if (!p) return NULL;
    a = ray_arena_str(d->arena, (const char*)p, slen);
    if (!a || !dom_slot_set_locked(d, pos, a)) return NULL;
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
    ray_file_id_t nid;
    void* map = ray_vm_map_file_id(d->path, &map_size, &nid);
    if (!map) return false;
    const uint8_t* nbase = (const uint8_t*)map;     /* new image bytes */

    int64_t fresh_count = -1;
    bool ok = map_size >= 12 && dom_rd32(nbase) == DOMAIN_STRL_MAGIC;
    if (ok) {
        memcpy(&fresh_count, nbase + 4, 8);
        ok = fresh_count >= count && fresh_count <= (int64_t)UINT32_MAX;
    }

    /* Position-0 reservation also holds for externally grown files: the
     * first record of a non-empty file must be the empty string. */
    if (ok && count == 0 && fresh_count > 0)
        ok = map_size >= 16 && dom_rd32(nbase + 12) == 0;

    /* Append-only prefix check: every position we already published must be
     * the same between the old and the new file images.  The new image's
     * block table comes from the index the writer that grew the file left
     * (when it is that file's, as at open) or from one walk.  The mapped
     * prefix [0, base_count) is compared by its table — the same checkpoints
     * and the same lengths, so the old image's records are not read —
     * and by the bytes of its first records and of its last, partial block.
     * A flush of this process wrote the rest, [base_count, count), from
     * appended atoms (never NULL), compared record by record.  A failure
     * with the index is tried once more with a walk. */
    dom_blk_t* ob = d->blk;
    const uint8_t* obase = (const uint8_t*)d->map;
    dom_dir_t* cur = atomic_load_explicit(&d->atoms, memory_order_relaxed);
    dom_blk_t* nb = NULL;
    bool from_ix = false;
    void* nixm = NULL;
    size_t nixs = 0;
    dom_ixh_t nhx;
    memset(&nhx, 0, sizeof(nhx));
    for (int pass = 0; ok && pass < 2; pass++) {
        nb = pass == 0 ? dom_ix_load(d->path, nbase, map_size, fresh_count, &nid, &nixm, &nixs, &nhx)
                       : NULL;
        if (pass == 0 && !nb) pass = 1;
        if (!nb) {
            nb = dom_blk_new(fresh_count, true);
            if (!nb) { ok = false; break; }
            nb->end = map_size;
            size_t o = 12;
            dom_src_t src;
            if (!dom_src_open(&src, d->path, &nid, map_size)) src = dom_src_map(nbase, map_size);
            bool walked = dom_blk_walk_src(&src, nb, 0, &o);
            dom_src_close(&src);
            if (!walked) { ray_sys_free(nb); nb = NULL; ok = false; break; }
        }
        from_ix = pass == 0;
        bool good = true;
        if (d->base_count > 0) {
            int64_t full = d->base_count / DOM_IX_STRIDE;
            good = map_size >= d->map_size;
            for (int64_t k = 0; good && k < full; k++)
                good = nb->ib[k].off == ob->ib[k].off && dom_blk_lim(nb, k) == dom_blk_lim(ob, k) &&
                       memcmp(nb->ib[k].len, ob->ib[k].len, sizeof(nb->ib[k].len)) == 0;
            for (int64_t i = full * DOM_IX_STRIDE; good && i < d->base_count; i++)
                good = nb->ib[full].len[i - full * DOM_IX_STRIDE] == ob->ib[full].len[i - full * DOM_IX_STRIDE];
            if (good && full < ob->n) {
                uint64_t at = ob->ib[full].off;
                good = at <= d->map_size && at <= map_size && nb->ib[full].off == at &&
                       memcmp(nbase + at, obase + at, d->map_size - (size_t)at) == 0;
            }
            if (good) {
                size_t hb = d->map_size - 12 < DOM_IX_HEAD ? d->map_size - 12 : DOM_IX_HEAD;
                good = memcmp(nbase + 12, obase + 12, hb) == 0;
            }
        }
        /* the entries this process flushed past the mapping, in order */
        size_t ro = d->base_count > 0 ? d->map_size : 12;
        for (int64_t i = d->base_count; good && i < count; i++) {
            ray_t* a = dom_slot_get(cur, i);
            good = a && (i % DOM_IX_STRIDE || nb->ib[i / DOM_IX_STRIDE].off == ro) &&
                   map_size - ro >= 4;
            uint32_t nl = good ? dom_rd32(nbase + ro) : 0;
            good = good && (size_t)nl <= map_size - ro - 4 && nl == ray_str_len(a) &&
                   (nl == 0 || memcmp(nbase + ro + 4, ray_str_ptr(a), nl) == 0);
            ro += 4 + (size_t)nl;
        }
        if (good) break;
        ray_sys_free(nb);
        nb = NULL;
        if (nixm) { ray_vm_unmap_file(nixm, nixs); nixm = NULL; }
        if (pass == 1) ok = false;
    }

    if (ok && fresh_count > count) {
        /* Publish the grown vocabulary.  A NEW directory (replace, never
         * grow in place, so a batch intern probing outside the lock keeps a
         * valid one) carries the already-materialized prefix atoms
         * (arena-owned, still valid; long atoms hold their own copy, so
         * swapping the map is safe); new file slots start NULL (lazy). */
        dom_dir_t* narr = dom_dir_new(fresh_count, cur);
        if (!narr) {
            ok = false;
        } else {
            /* Swap in the new checkpoints + map, publish, then let go of
             * the superseded generation.  Nothing holds it: a reopen runs
             * on the calling thread between kernels, and a raw snapshot
             * does not survive one (domain.h) -- unless a batch intern is
             * probing outside the lock, in which case dom_drop_locked
             * retires instead.  Retiring until the domain dies here was
             * #717: a replica never lets its domain die. */
            dom_blk_t* old_blk = d->blk;
            d->blk = nb;
            d->map_id = nid;
            nb = NULL;
            d->ix_used = from_ix;
            d->ix_count = from_ix ? nhx.count : 0;
            d->ix_end = from_ix ? nhx.end : 0;
            d->ix_covered = from_ix ? fresh_count : 0;
            if (nixm) { dom_keep_map(d, nixm, nixs); nixm = NULL; }
            void* old_map = d->map;
            size_t old_map_size = d->map_size;
            d->map = map;
            d->map_size = map_size;
            d->base_count = fresh_count;
            /* every flushed entry is in the mapping now */
            dom_ck_free(&d->wck);

            atomic_store_explicit(&d->atoms, narr, memory_order_release);
            d->atoms_cap = narr->n << DOM_SLOT_LOG;
            atomic_store_explicit(&d->count, fresh_count, memory_order_release);
            d->disk_count = fresh_count;
            d->disk_size = st_size;
            /* The new snapshot record must be published before the old
             * generation goes.  When its allocation fails the old record
             * stays published and still points at the old checkpoints and
             * mapping, so those are retired (valid memory, covering fewer
             * positions, as before) rather than freed; the directory is
             * referenced by no record and goes either way. */
            bool snap_ok = dom_publish_raw_snap(d, true);

            (void)dom_drop_locked(d, cur);
            if (snap_ok) (void)dom_drop_locked(d, old_blk);
            else         (void)dom_retire(d, old_blk);
            if (old_map) {
                if (snap_ok && d->batch_inflight == 0) {
                    ray_vm_unmap_file(old_map, old_map_size);
                } else {
                    dom_retired_map_t* rm = (dom_retired_map_t*)ray_sys_alloc(sizeof(*rm));
                    if (rm) { rm->map = old_map; rm->size = old_map_size; rm->next = d->retired_maps; d->retired_maps = rm; }
                    /* no rm: the old mapping stays mapped for the process
                     * lifetime — never unmapped under a live snapshot */
                }
            }

            /* Reverse index + LUT now cover a stale prefix: drop both.
             * A retire-OOM here (batch in flight) would keep the stale LUT
             * published over the grown count (OOB reads for lock-free
             * consumers) — the same corner dom_append_locked hits; mirror
             * its loud abort (the count is already published, there is no
             * clean undo). */
            if (d->buckets) {
                if (!dom_drop_buckets_locked(d, d->buckets)) {
                    fprintf(stderr, "rayforce: sym domain '%s': OOM retiring "
                                    "reverse index after external extend\n",
                            d->path ? d->path : "?");
                    abort();
                }
                d->buckets = NULL; d->bucket_mask = 0;
            }
            int64_t* lut = atomic_load_explicit(&d->runtime_lut, memory_order_relaxed);
            if (lut) {
                atomic_store_explicit(&d->runtime_lut, NULL, memory_order_release);
                if (!dom_drop_locked(d, lut)) {
                    fprintf(stderr, "rayforce: sym domain '%s': OOM retiring "
                                    "runtime LUT after external extend — cannot "
                                    "keep id translation consistent\n",
                            d->path ? d->path : "?");
                    abort();
                }
            }
            return true;
        }
    }

    /* Failure, or nothing actually new (size-only change): drop the fresh
     * table and the new mapping; keep the existing image. */
    if (ok && fresh_count == count) d->disk_size = st_size;
    ray_sys_free(nb);
    if (nixm) ray_vm_unmap_file(nixm, nixs);
    ray_vm_unmap_file(map, map_size);
    return ok;
}

static ray_sym_domain_t* dom_open_impl(const char* path, bool create) {
    if (!path) return NULL;

    /* A script or IPC caller may drop a parted view without running GC.
     * Do not let its cache-only domain survive into a splayed read/write
     * of a rebuilt root. Prune before taking g_dom_lock: releasing the
     * cached SYM columns takes that lock itself. Live views keep their refs. */
    ray_parted_cache_prune();

    char* rpath = dom_resolve_path(path);
    if (!rpath) return NULL;

    struct stat st;
    bool exists = (stat(rpath, &st) == 0);
    if (!exists && !create) { ray_sys_free(rpath); return NULL; }

    dom_lock();
    for (ray_sym_domain_t* d = g_domains; d; d = d->next) {
        if (dom_path_eq(d->path, rpath)) {
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
        /* Mapped, not read: the records are reached through checkpoints
         * (the sparse record index above), from the index file when it is
         * valid, so opening reads only the index and the records after it. */
        d->map = ray_vm_map_file_id(rpath, &d->map_size, &d->map_id);
        int64_t count = -1;
        const uint8_t* m = (const uint8_t*)d->map;
        if (m && d->map_size >= 12 && dom_rd32(m) == DOMAIN_STRL_MAGIC)
            memcpy(&count, m + 4, 8);
        if (count < 0 || count > (int64_t)UINT32_MAX || !dom_index_image(d, count)) {
            dom_destroy(d);
            return NULL;
        }
        dom_dir_t* dir = count > 0 ? dom_dir_new(count, NULL) : NULL;
        if (count > 0 && !dir) { dom_destroy(d); return NULL; }
        atomic_store_explicit(&d->atoms, dir, memory_order_relaxed);
        atomic_store_explicit(&d->count, count, memory_order_relaxed);
        d->atoms_cap = dir ? dir->n << DOM_SLOT_LOG : 0;
        d->base_count = count;
        (void)dom_publish_raw_snap(d, false);
        d->disk_count = count;
        d->disk_size = d->map_size;
    }
    /* create && !exists: empty domain — vocabulary appears via intern
     * ("" seeded at position 0) and reaches disk on first flush. */

    /* Insert, unless another thread raced us to the same path — then keep
     * the winner (pointer equality must hold for one resolved path). */
    dom_lock();
    for (ray_sym_domain_t* e = g_domains; e; e = e->next) {
        if (dom_path_eq(e->path, d->path)) {
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

ray_sym_domain_t* ray_sym_domain_create_import(const char* path) {
    if (!path) return NULL;
    ray_sym_domain_t* d = (ray_sym_domain_t*)ray_sys_alloc(sizeof(*d));
    if (!d) return NULL;
    memset(d, 0, sizeof(*d));
    d->kind = DOM_IMPORT;
    d->rc = 1;
    size_t pl = strlen(path);
    d->path = (char*)ray_sys_alloc(pl + 1);
    if (d->path) memcpy(d->path, path, pl + 1);
    if (d->path) d->imp = ray_symimp_create(path, &d->count);
    if (!d->imp) { ray_sys_free(d->path); ray_sys_free(d); return NULL; }
    return d;
}

bool ray_sym_domain_import_stats(ray_sym_domain_t* dom, struct ray_symimp_stats_s* out) {
    if (!dom || dom->kind != DOM_IMPORT) return false;
    ray_symimp_stats(dom->imp, out);
    return true;
}

struct ray_symimp_s* ray_sym_domain_import(ray_sym_domain_t* dom) {
    return dom && dom->kind == DOM_IMPORT ? dom->imp : NULL;
}

ray_sym_domain_t* ray_sym_domain_new(void) {
    ray_sym_domain_t* d = ray_sys_alloc(sizeof(*d));
    if (!d) return NULL;
    d->kind = DOM_FILE;
    d->rc = 1;
    atomic_store_explicit(&d->raw_snap, NULL, memory_order_relaxed);
    d->arena = ray_arena_new(64 * 1024);
    if (!d->arena) { ray_sys_free(d); return NULL; }
    return d;
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
    if (dom->kind == DOM_IMPORT) return NULL;

    /* Lock-free fast path: acquire the published count, then the directory,
     * then the chunk and the slot.  Appends release-publish the count after
     * the slot write; directory replacements are release-published before
     * the count bump, and an append retires the old directory (valid memory)
     * — a reader can never index a published position into memory that
     * lacks it.  An external extend frees the old directory instead: no
     * reader runs during a reopen (see the concurrency note at the top of
     * the file).  A NULL slot is an unmaterialized lazy file entry (or one
     * whose chunk this directory has not seen): fall to the lock-held
     * materialize. */
    int64_t count = atomic_load_explicit(&dom->count, memory_order_acquire);
    if (pos < 0 || pos >= count) return NULL;
    ray_t* a = dom_slot_get(atomic_load_explicit(&dom->atoms, memory_order_acquire), pos);
    if (a) return a;

    dom_lock();
    a = dom_atom_at_locked(dom, pos);
    dom_unlock();
    return a;
}

bool ray_sym_domain_raw_pin(ray_sym_domain_t* dom, ray_sym_domain_raw_t* out) {
    if (!dom || !out || dom->kind != DOM_FILE) return false;
    dom_raw_snap_t* snap = atomic_load_explicit(&dom->raw_snap, memory_order_acquire);
    if (!snap) return false;
    out->map = snap->map;
    out->ib = snap->blk->ib;
    out->count = snap->count;
    out->blk = snap->blk;
    out->dom = dom;
    return true;
}

int64_t ray_sym_domain_strlen(ray_sym_domain_t* dom, int64_t pos) {
    if (!dom || pos < 0) return -1;
    ray_sym_domain_raw_t r;
    if (dom->kind == DOM_FILE && ray_sym_domain_raw_pin(dom, &r) && pos < r.count)
        return ray_sym_domain_raw_len(&r, pos);
    ray_t* s = ray_sym_domain_str(dom, pos);
    return s ? (int64_t)ray_str_len(s) : -1;
}

bool ray_sym_domain_ix_info(ray_sym_domain_t* dom, ray_sym_domain_ix_info_t* out) {
    if (!dom || !out || dom->kind != DOM_FILE) return false;
    dom_lock();
    out->from_index = dom->ix_used;
    out->lens_mapped = dom->blk && dom->blk->ib && !dom->blk->own_ib;
    out->covered = dom->ix_covered;
    dom_unlock();
    return true;
}

void ray_sym_domain_raw_unpin(ray_sym_domain_t* dom) {
    (void)dom;   /* the snapshot outlives its readers by contract: no pin
                  * across a reopen (domain.h, ray_sym_domain_raw_pin) */
}
/* Empty-vocabulary FILE domains get a distinct non-NULL LUT so callers
 * can branch on NULL == "runtime domain, ids pass through".  Never
 * indexed: every position is out of range when count == 0. */
static const int64_t g_empty_lut[1] = { -1 };

const int64_t* ray_sym_domain_runtime_lut(ray_sym_domain_t* dom) {
    if (!dom || dom->kind == DOM_RUNTIME) return NULL;
    /* never asked for: an import's columns are read back through the
     * file domain, after the import dictionary is gone */
    if (dom->kind == DOM_IMPORT) return NULL;

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
         * rows), straight from the records — no domain atom is made for
         * it.  Callers that hand ids to ray_pool_dispatch workers
         * must request this LUT during sequential setup — interning
         * inside a worker violates sym.c's frozen-table rule. */
        size_t off = 12;
        for (int64_t i = 0; i < count; i++) {
            const char* p = NULL;
            size_t l = 0;
            if (!dom_next_locked(dom, i, &off, &p, &l)) {
                ray_sys_free(lut); dom_unlock(); return NULL;
            }
            lut[i] = ray_sym_intern(p, l);
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
    if (d->buckets) {
        /* Growth: the old table covers every published entry and carries
         * the hashes — re-slot its entries, no string hashing. */
        uint64_t old_cap = d->bucket_mask + 1;
        for (uint64_t i = 0; i < old_cap; i++) {
            uint64_t e = d->buckets[i];
            if (e == 0) continue;
            uint64_t slot = (uint32_t)(e >> 32) & mask;
            while (buckets[slot] != 0) slot = (slot + 1) & mask;
            buckets[slot] = e;
        }
    } else {
        /* One pass over the records, hashed where they lie: no atom. */
        size_t off = 12;
        for (int64_t i = 0; i < count; i++) {
            const char* p = NULL;
            size_t l = 0;
            if (!dom_next_locked(d, i, &off, &p, &l)) { ray_sys_free(buckets); return false; }
            uint32_t h = (uint32_t)ray_hash_bytes(p, l);
            uint64_t slot = h & mask;
            while (buckets[slot] != 0) slot = (slot + 1) & mask;
            buckets[slot] = ((uint64_t)h << 32) | ((uint64_t)(uint32_t)i + 1);
        }
    }
    /* A batch intern may be probing a snapshot of the old table outside
     * the lock (see ray_sym_domain_intern_batch). */
    if (d->buckets && !dom_drop_buckets_locked(d, d->buckets)) {
        ray_sys_free(buckets);
        return false;
    }
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
            size_t l = 0;
            const char* p = dom_bytes_locked(d, pos, &l);
            if (p && l == len && (len == 0 || memcmp(p, str, len) == 0))
                return pos;
        }
        slot = (slot + 1) & mask;
    }
    return -1;
}

int64_t ray_sym_domain_find(ray_sym_domain_t* dom, const char* str, size_t len) {
    if (!dom || !str) return -1;
    if (dom->kind == DOM_RUNTIME) return ray_sym_find(str, len);
    if (dom->kind == DOM_IMPORT) return -1;

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

    /* Grow the published directory by REPLACING it — never in place:
     * lock-free readers may hold the old pointer. */
    if (!dom_grow_locked(d, count + 1)) return -1;
    if (!dom_chunk_locked(d, count)) return -1;

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

    (void)dom_slot_set_locked(d, count, s);   /* its chunk exists (above) */
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
    if (dom->kind == DOM_IMPORT) {
        uint64_t h = ray_hash_bytes(str, len);
        int64_t pos = -1;
        return ray_symimp_intern_batch(dom->imp, 1, &str, &len, &h, &pos) ? pos : -1;
    }

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

/* ---- batch intern ---------------------------------------------------------- */

/* Read-only probe over a snapshot of the reverse index taken under the
 * lock.  Everything the snapshot points at outlives it: replaced bucket
 * tables and atom arrays are retired, not freed, and the file prefix is
 * read through the pinned raw snapshot.  Entries appended after the
 * snapshot (pos >= count) are ignored here and resolved under the lock. */
typedef struct {
    const uint64_t*      buckets;
    uint64_t             mask;
    dom_dir_t*           atoms;
    int64_t              count;
    ray_sym_domain_raw_t raw;      /* raw.count == 0: no file prefix */
    const char* const*   strs;
    const size_t*        lens;
    const uint32_t*      hashes;
    int64_t*             out_pos;
    /* misses, grouped by hash partition (hash >> part_shift) */
    int64_t*             miss;       /* [n_miss] batch indices */
    int64_t*             uniq;       /* [n_miss] first occurrences, per partition segment */
    int64_t*             part_off;   /* [n_part + 1] */
    int64_t*             uniq_n;     /* [n_part] distinct misses per partition */
    int64_t*             tab;        /* one block: every partition's dedupe table */
    int64_t*             tab_off;    /* [n_part + 1] partition table extents in tab */
    /* the append: new strings k = 0..total-1 in batch order */
    int64_t*             ui;         /* [total] batch index of new string k */
    int64_t*             uoff;       /* [total] its byte offset in region */
    char*                region;     /* one arena region for the whole batch */
    int64_t              base;       /* position of new string 0 */
    dom_dir_t*           atoms_w;    /* current directory (fill target; chunks made) */
    _Atomic(uint8_t)*    done;       /* [n_tasks] insert task t finished (parallel commit) */
    int64_t              insert_total;
    /* Set when a probe met an entry it could not compare (no atom, no
     * raw bytes): the misses are then resolved under the lock instead. */
    _Atomic(bool)        unsure;
    int                  part_shift;
} dom_batch_ctx_t;

static void dom_batch_probe_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    const dom_batch_ctx_t* b = (const dom_batch_ctx_t*)raw;
    for (int64_t i = start; i < end; i++) {
        uint32_t h = b->hashes[i];
        size_t len = b->lens[i];
        const char* s = b->strs[i];
        uint64_t slot = h & b->mask;
        int64_t found = -1;
        for (;;) {
            uint64_t e = atomic_load_explicit((_Atomic(uint64_t)*)&b->buckets[slot],
                                              memory_order_relaxed);
            if (e == 0) break;
            if ((uint32_t)(e >> 32) == h) {
                int64_t pos = (int64_t)(uint32_t)e - 1;
                if (pos < b->count) {
                    const char* p = NULL;
                    size_t l = 0;
                    ray_t* a = dom_slot_get(b->atoms, pos);
                    if (a) { p = ray_str_ptr(a); l = ray_str_len(a); }
                    else if (pos < b->raw.count) p = ray_sym_domain_raw_str(&b->raw, pos, &l);
                    else atomic_store_explicit((_Atomic(bool)*)&b->unsure, true, memory_order_relaxed);
                    if (p && l == len && (len == 0 || memcmp(p, s, len) == 0)) {
                        found = pos;
                        break;
                    }
                }
            }
            slot = (slot + 1) & b->mask;
        }
        b->out_pos[i] = found;
    }
}

/* Dedupe the misses of one hash partition among themselves: the first
 * occurrence stays a miss (out_pos -1) and is listed in the partition's
 * segment of `uniq`; a repeat records its representative as -(rep + 2).
 * Partitions are disjoint by hash, so no two tasks ever see the same
 * string. */
static void dom_batch_dedupe_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    dom_batch_ctx_t* b = (dom_batch_ctx_t*)raw;
    for (int64_t p = start; p < end; p++) {
        int64_t lo = b->part_off[p], hi = b->part_off[p + 1];
        int64_t cnt = hi - lo;
        if (cnt == 0) { b->uniq_n[p] = 0; continue; }
        /* the partition's slice of the shared table block (sized by the
         * caller: a power of two at least twice the partition's misses) */
        uint64_t cap = (uint64_t)(b->tab_off[p + 1] - b->tab_off[p]);
        int64_t* tab = b->tab + b->tab_off[p];
        memset(tab, 0xff, (size_t)cap * sizeof(int64_t));   /* -1 = empty */
        uint64_t mask = cap - 1;
        int64_t k = 0;
        for (int64_t j = lo; j < hi; j++) {
            int64_t i = b->miss[j];
            uint32_t h = b->hashes[i];
            uint64_t slot = h & mask;
            int64_t rep = -1;
            while (tab[slot] >= 0) {
                int64_t r = tab[slot];
                if (b->hashes[r] == h && b->lens[r] == b->lens[i] &&
                    (b->lens[i] == 0 || memcmp(b->strs[r], b->strs[i], b->lens[i]) == 0)) {
                    rep = r;
                    break;
                }
                slot = (slot + 1) & mask;
            }
            if (rep >= 0) {
                b->out_pos[i] = -(rep + 2);
            } else {
                tab[slot] = i;
                b->uniq[lo + k++] = i;
            }
        }
        b->uniq_n[p] = k;
    }
}

/* Append the batch's new strings k0..k1 (positions base + k, assigned in
 * batch order by the caller): build each atom at its offset in the one
 * arena region, store it at its position (a contiguous run per task — no
 * two tasks write the same cache line of the atom array) and publish it in
 * the reverse index (CAS on the empty slot keeps the probes of the other
 * tasks correct). */
static void dom_batch_insert_fn(void* raw, uint32_t wid, int64_t k0, int64_t k1) {
    (void)wid;
    dom_batch_ctx_t* b = (dom_batch_ctx_t*)raw;
    for (int64_t k = k0; k < k1; k++) {
        int64_t i = b->ui[k];
        int64_t pos = b->base + k;
        ray_t* s = ray_arena_str_at(b->region + b->uoff[k], b->strs[i], b->lens[i]);
        dom_slots_t* c = atomic_load_explicit(&b->atoms_w->c[pos >> DOM_SLOT_LOG],
                                              memory_order_relaxed);
        atomic_store_explicit(&c->a[pos & (DOM_SLOTS - 1)], s, memory_order_relaxed);
        uint32_t h = b->hashes[i];
        uint64_t e = ((uint64_t)h << 32) | ((uint64_t)(uint32_t)pos + 1);
        uint64_t slot = h & b->mask;
        for (;;) {
            uint64_t cur = 0;
            if (atomic_compare_exchange_strong_explicit((_Atomic(uint64_t)*)&b->buckets[slot],
                    &cur, e, memory_order_relaxed, memory_order_relaxed))
                break;
            slot = (slot + 1) & b->mask;
        }
    }
}

/* The insert in tasks of DOM_INSERT_GRAIN new strings, each flagged done
 * when it finishes.  A cancelled pool dispatch skips tasks that have not
 * started but runs a started one to its end (nothing here polls), so after
 * the dispatch every task is either whole or untouched, and the caller
 * completes the untouched ones serially: the commit then always publishes
 * every string, cancel or not, without a serial pass over all of them. */
#define DOM_INSERT_GRAIN 4096
static void dom_batch_insert_task_fn(void* raw, uint32_t wid, int64_t t0, int64_t t1) {
    dom_batch_ctx_t* b = (dom_batch_ctx_t*)raw;
    for (int64_t t = t0; t < t1; t++) {
        int64_t k0 = t * DOM_INSERT_GRAIN;
        int64_t k1 = k0 + DOM_INSERT_GRAIN;
        if (k1 > b->insert_total) k1 = b->insert_total;
        dom_batch_insert_fn(raw, wid, k0, k1);
        atomic_store_explicit(&b->done[t], 1, memory_order_release);
    }
}

/* A repeated miss takes the position its first occurrence received. */
static void dom_batch_repeat_fn(void* raw, uint32_t wid, int64_t j0, int64_t j1) {
    (void)wid;
    dom_batch_ctx_t* b = (dom_batch_ctx_t*)raw;
    for (int64_t j = j0; j < j1; j++) {
        int64_t i = b->miss[j];
        int64_t v = b->out_pos[i];
        if (v < -1) b->out_pos[i] = b->out_pos[-(v + 2)];
    }
}

/* Whether a pool dispatch issued now would skip tasks: the evaluator's
 * interrupt flag, or the pool's own cancel flag (set together by
 * ray_request_interrupt; a handler on a worker thread can leave the pool's
 * set alone for a moment). */
static inline bool dom_batch_cancelled(ray_pool_t* pool) {
    if (ray_interrupted()) return true;
    return pool && atomic_load_explicit(&pool->cancelled, memory_order_relaxed);
}

#ifdef DEBUG
static _Atomic(int) g_dom_batch_cancel_phase_for_test = 0;

void ray_sym_domain_batch_cancel_for_test(int phase) {
    atomic_store_explicit(&g_dom_batch_cancel_phase_for_test, phase,
                          memory_order_relaxed);
}

static void dom_batch_cancel_for_test(int phase) {
    int expected = phase;
    if (atomic_compare_exchange_strong_explicit(
            &g_dom_batch_cancel_phase_for_test, &expected, 0,
            memory_order_relaxed, memory_order_relaxed))
        ray_request_interrupt();
}
#else
#define dom_batch_cancel_for_test(...) ((void)0)
#endif

static bool dom_batch_append_serial_locked(ray_sym_domain_t* dom, int64_t n,
                                           const char* const* strs, const size_t* lens,
                                           const uint32_t* hashes, int64_t* out_pos) {
    for (int64_t i = 0; i < n; i++) {
        if (out_pos[i] >= 0) continue;
        int64_t pos = dom_probe_locked(dom, hashes[i], strs[i], lens[i]);
        if (pos < 0) pos = dom_append_locked(dom, hashes[i], strs[i], lens[i]);
        if (pos < 0) return false;
        out_pos[i] = pos;
    }
    return true;
}

bool ray_sym_domain_intern_batch(ray_sym_domain_t* dom, int64_t n,
                                 const char* const* strs, const size_t* lens,
                                 const uint32_t* hashes, int64_t* out_pos) {
    if (!dom || n < 0) return false;
    if (n == 0) return true;
    if (ray_interrupted()) return false;
    if (dom->kind == DOM_IMPORT) {
        /* The import dictionary keys on all 64 bits of the hash, which a
         * caller of this entry point has dropped: hashed again here
         * (ray_sym_domain_intern_batch64 is the path that keeps them). */
        uint64_t h64[1024];
        for (int64_t o = 0; o < n; o += 1024) {
            int64_t k = n - o < 1024 ? n - o : 1024;
            for (int64_t i = 0; i < k; i++)
                h64[i] = strs[o + i] ? ray_hash_bytes(strs[o + i], lens[o + i]) : ray_hash_bytes("", 0);
            if (!ray_symimp_intern_batch(dom->imp, k, strs + o, lens + o, h64, out_pos + o)) return false;
        }
        return true;
    }
    if (dom->kind == DOM_RUNTIME) {
        for (int64_t i = 0; i < n; i++) {
            int64_t id = ray_sym_intern_prehashed(hashes[i], strs[i], lens[i]);
            if (id < 0) return false;
            out_pos[i] = id;
        }
        return true;
    }

    dom_batch_ctx_t b;
    memset(&b, 0, sizeof(b));

    dom_lock();
    int64_t count = atomic_load_explicit(&dom->count, memory_order_relaxed);
    /* Headroom for the whole batch up front so no rebuild happens while
     * the batch is in flight. */
    if (!dom->buckets ||
        (double)(count + n + 1) > 0.7 * (double)(dom->bucket_mask + 1)) {
        if (!dom_build_index_locked(dom, n + 1)) { dom_unlock(); return false; }
    }
    if (count == 0) {
        uint32_t h0 = (uint32_t)ray_hash_bytes("", 0);
        if (dom_append_locked(dom, h0, "", 0) != 0) { dom_unlock(); return false; }
    }
    b.buckets = dom->buckets;
    b.mask    = dom->bucket_mask;
    b.atoms   = atomic_load_explicit(&dom->atoms, memory_order_acquire);
    b.count   = atomic_load_explicit(&dom->count, memory_order_acquire);
    dom->batch_inflight++;
    dom_unlock();
    ray_profile_tick("dom batch: index ready");

    if (!ray_sym_domain_raw_pin(dom, &b.raw)) b.raw.count = 0;
    b.strs = strs; b.lens = lens; b.hashes = hashes; b.out_pos = out_pos;

    ray_pool_t* pool = ray_pool_get();
    bool par = ray_pool_par_dispatch_ok(pool, n, 4096);
    dom_batch_cancel_for_test(RAY_SYM_DOMAIN_CANCEL_PROBE);
    if (par) ray_pool_dispatch(pool, dom_batch_probe_fn, &b, n);
    else dom_batch_probe_fn(&b, 0, 0, n);
    if (dom_batch_cancelled(pool)) {
        dom_lock();
        dom->batch_inflight--;
        dom_unlock();
        return false;
    }

    /* Misses, grouped by hash partition: no more partitions than workers*4
     * or than the batch fills with a few thousand misses each.  Every
     * partition dedupes its misses in its slice of ONE table block — a
     * batch costs one page-granular allocation for the tables, not one per
     * partition (and no mapping churn on the workers). */
    int n_part = 1;
    if (par) {
        int64_t want = (int64_t)ray_pool_total_workers(pool) * 4;
        if (want > n / 4096) want = n / 4096;
        while (n_part < want && n_part < 1024) n_part <<= 1;
    }
    b.part_shift = 31;
    for (int p = n_part; p > 2; p >>= 1) b.part_shift--;
    if (n_part == 1) n_part = 2;   /* keep the shift below the type width */
    int64_t n_miss = 0;
    for (int64_t i = 0; i < n; i++) n_miss += (out_pos[i] < 0);
    if (n_miss == 0) {
        dom_lock();
        dom->batch_inflight--;
        dom_unlock();
        return true;
    }

    b.miss     = (int64_t*)ray_sys_alloc((size_t)n_miss * sizeof(int64_t));
    b.uniq     = (int64_t*)ray_sys_alloc((size_t)n_miss * sizeof(int64_t));
    b.part_off = (int64_t*)ray_sys_alloc((size_t)(n_part + 1) * sizeof(int64_t));
    b.uniq_n   = (int64_t*)ray_sys_alloc((size_t)n_part * sizeof(int64_t));
    b.tab_off  = (int64_t*)ray_sys_alloc((size_t)(n_part + 1) * sizeof(int64_t));
    bool ok = b.miss && b.uniq && b.part_off && b.uniq_n && b.tab_off;
    if (ok) {
        memset(b.part_off, 0, (size_t)(n_part + 1) * sizeof(int64_t));
        for (int64_t i = 0; i < n; i++)
            if (out_pos[i] < 0) b.part_off[(hashes[i] >> b.part_shift) + 1]++;
        for (int p = 0; p < n_part; p++) b.part_off[p + 1] += b.part_off[p];
        {
            int64_t* fill = b.uniq_n;   /* scratch cursor per partition */
            memcpy(fill, b.part_off, (size_t)n_part * sizeof(int64_t));
            for (int64_t i = 0; i < n; i++)
                if (out_pos[i] < 0) b.miss[fill[hashes[i] >> b.part_shift]++] = i;
        }
        /* table extents: a power of two at least twice the partition's misses */
        int64_t tcap = 0;
        for (int p = 0; p < n_part; p++) {
            int64_t cnt = b.part_off[p + 1] - b.part_off[p];
            int64_t cap = 0;
            if (cnt > 0) { cap = 16; while (cnt * 2 > cap) cap <<= 1; }
            b.tab_off[p] = tcap;
            tcap += cap;
        }
        b.tab_off[n_part] = tcap;
        b.tab = (int64_t*)ray_sys_alloc((size_t)(tcap > 0 ? tcap : 1) * sizeof(int64_t));
        if (!b.tab) ok = false;
        else {
            dom_batch_cancel_for_test(RAY_SYM_DOMAIN_CANCEL_DEDUPE);
            if (par) ray_pool_dispatch_n(pool, dom_batch_dedupe_fn, &b, (uint32_t)n_part);
            else dom_batch_dedupe_fn(&b, 0, 0, n_part);
            if (dom_batch_cancelled(pool)) ok = false;
        }
        ray_profile_tick("dom batch: misses deduped");
    }

    dom_lock();
    dom->batch_inflight--;   /* no probe outside the lock past this point */
    if (dom_batch_cancelled(pool)) {
        dom_unlock();
        ray_sys_free(b.miss);
        ray_sys_free(b.uniq);
        ray_sys_free(b.part_off);
        ray_sys_free(b.uniq_n);
        ray_sys_free(b.tab_off);
        ray_sys_free(b.tab);
        return false;
    }
    /* An external extend meanwhile dropped the reverse index the probe
     * read (dom_extend_from_file_locked): everything below, the serial
     * fallback included, probes the current vocabulary, so rebuild it
     * first; without one there is nothing to fall back to. */
    bool indexed = dom->buckets || dom_build_index_locked(dom, n + 1);
    if (!indexed) ok = false;
    bool unchanged = ok && dom->buckets == b.buckets &&
                     atomic_load_explicit(&dom->count, memory_order_relaxed) == b.count &&
                     !atomic_load_explicit(&b.unsure, memory_order_relaxed);
    if (ok && !unchanged) {
        /* Concurrent column chunks commonly append between the probe and
         * commit. Keep the deduplication and arena batching: recheck only
         * distinct misses against the current index, then commit the new
         * subset in bulk. Falling back to one allocation/insertion per
         * cell here makes additional import workers spend their time
         * waiting for this lock. */
        int64_t total = 0;
        for (int p = 0; p < n_part; p++) {
            int64_t lo = b.part_off[p], keep = 0;
            for (int64_t j = 0; j < b.uniq_n[p]; j++) {
                int64_t i = b.uniq[lo+j];
                int64_t pos = dom_probe_locked(dom, hashes[i], strs[i], lens[i]);
                if (pos >= 0) out_pos[i] = pos;
                else b.uniq[lo+keep++] = i;
            }
            b.uniq_n[p] = keep; total += keep;
        }
        b.count = atomic_load_explicit(&dom->count, memory_order_relaxed);
        if ((double)(b.count + total) > 0.7 * (double)(dom->bucket_mask + 1))
            ok = dom_build_index_locked(dom, total);
        b.buckets = dom->buckets; b.mask = dom->bucket_mask;
        unchanged = ok;
    }
    if (unchanged) {
        int64_t total = 0;
        for (int p = 0; p < n_part; p++) total += b.uniq_n[p];
        int64_t base = b.count;
        ok = base + total < (int64_t)UINT32_MAX;
        /* Directory: grow once by replacement (lock-free readers may hold
         * the old pointer), and make the chunks the new slots fall in, so
         * the insert tasks only store into them. */
        if (ok) ok = dom_grow_locked(dom, base + total);
        for (int64_t k = base >> DOM_SLOT_LOG;
             ok && total > 0 && k <= (base + total - 1) >> DOM_SLOT_LOG; k++)
            ok = dom_chunk_locked(dom, k << DOM_SLOT_LOG) != NULL;
        if (ok && total > 0) {
            b.ui = (int64_t*)ray_sys_alloc((size_t)total * 2 * sizeof(int64_t));
            if (!b.ui) ok = false;
            else b.uoff = b.ui + total;
        }
        if (ok) {
            /* New strings take positions in batch order (first occurrence),
             * so the symfile does not depend on how the batch was split;
             * their atoms go into ONE arena region, each at the offset the
             * strings before it leave.  Nothing is published until every
             * atom is built, so a failed reservation costs only arena
             * space. */
            int64_t pos = base;
            size_t bytes = 0;
            for (int64_t i = 0; i < n; i++) {
                if (out_pos[i] != -1) continue;
                int64_t k = pos - base;
                if (k < total) { b.ui[k] = i; b.uoff[k] = (int64_t)bytes; }
                bytes += ray_arena_str_bytes(lens[i]);
                out_pos[i] = pos++;
            }
            if (pos - base != total) ok = false;
            if (ok && bytes > 0) {
                b.region = (char*)ray_arena_alloc_raw(dom->arena, bytes);
                if (!b.region) ok = false;
            }
            if (ok) {
                b.base = base;
                b.atoms_w = atomic_load_explicit(&dom->atoms, memory_order_relaxed);
                /* The commit is not interruptible: once bucket and atom
                 * publication starts, a partial insert cannot be rolled
                 * back.  It still runs in parallel: a cancel arriving now
                 * makes the dispatch skip tasks, and every skipped task is
                 * run here afterwards (dom_batch_insert_task_fn), so the
                 * publication is whole either way.  The repeat pass is
                 * idempotent and simply re-runs serially after a cancel. */
                dom_batch_cancel_for_test(RAY_SYM_DOMAIN_CANCEL_INSERT);
                if (total > 0) {
                    int64_t n_tasks = (total + DOM_INSERT_GRAIN - 1) / DOM_INSERT_GRAIN;
                    b.insert_total = total;
                    void* done_mem = NULL;
                    if (par && ray_pool_par_dispatch_ok(pool, total, DOM_INSERT_GRAIN) &&
                        n_tasks <= RAY_POOL_MAX_TASKS)
                        done_mem = ray_sys_alloc((size_t)n_tasks);
                    b.done = done_mem;
                    if (done_mem) {
                        memset(done_mem, 0, (size_t)n_tasks);
                        ray_pool_dispatch_n(pool, dom_batch_insert_task_fn, &b, (uint32_t)n_tasks);
                        for (int64_t t = 0; t < n_tasks; t++)
                            if (!atomic_load_explicit(&b.done[t], memory_order_acquire))
                                dom_batch_insert_task_fn(&b, 0, t, t + 1);
                        ray_sys_free(done_mem);
                        b.done = NULL;
                    } else {
                        dom_batch_insert_fn(&b, 0, 0, total);
                    }
                }
                if (par && ray_pool_par_dispatch_ok(pool, n_miss, 4096)) {
                    ray_pool_dispatch(pool, dom_batch_repeat_fn, &b, n_miss);
                    if (dom_batch_cancelled(pool)) dom_batch_repeat_fn(&b, 0, 0, n_miss);
                } else {
                    dom_batch_repeat_fn(&b, 0, 0, n_miss);
                }
                ray_profile_tick("dom batch: inserted");
                atomic_store_explicit(&dom->count, pos, memory_order_release);
                /* Same invalidation as dom_append_locked: the runtime LUT
                 * no longer covers the vocabulary. */
                int64_t* lut = atomic_load_explicit(&dom->runtime_lut, memory_order_relaxed);
                if (lut) {
                    if (!dom_retire(dom, lut)) {
                        fprintf(stderr, "rayforce: sym domain '%s': OOM retiring runtime "
                                        "LUT after batch append\n", dom->path ? dom->path : "?");
                        abort();
                    }
                    atomic_store_explicit(&dom->runtime_lut, NULL, memory_order_release);
                }
            } else {
                /* Nothing published: undo the assigned positions and the
                 * repeat marks; the serial path resolves them again. */
                for (int64_t i = 0; i < n; i++)
                    if (out_pos[i] < -1 || out_pos[i] >= base) out_pos[i] = -1;
            }
        }
    } else {
        for (int64_t i = 0; i < n; i++) if (out_pos[i] < -1) out_pos[i] = -1;
    }
    if (indexed && (!unchanged || !ok))
        ok = dom_batch_append_serial_locked(dom, n, strs, lens, hashes, out_pos);
    dom_unlock();
    ray_profile_tick("dom batch: done");

    ray_sys_free(b.miss);
    ray_sys_free(b.uniq);
    ray_sys_free(b.part_off);
    ray_sys_free(b.uniq_n);
    ray_sys_free(b.tab_off);
    ray_sys_free(b.tab);
    ray_sys_free(b.ui);
    return ok;
}

bool ray_sym_domain_intern_batch64(ray_sym_domain_t* dom, int64_t n,
                                   const char* const* strs, const size_t* lens,
                                   const uint64_t* hashes, int64_t* out_pos) {
    if (!dom || n < 0) return false;
    if (dom->kind == DOM_IMPORT)
        return n == 0 || ray_symimp_intern_batch(dom->imp, n, strs, lens, hashes, out_pos);
    /* the other domains key on the low 32 bits; in slices of 4096, which
     * still take the parallel probe */
    uint32_t h32[4096];
    for (int64_t o = 0; o < n; o += 4096) {
        int64_t k = n - o < 4096 ? n - o : 4096;
        for (int64_t i = 0; i < k; i++) h32[i] = (uint32_t)hashes[o + i];
        if (!ray_sym_domain_intern_batch(dom, k, strs + o, lens + o, h32, out_pos + o)) return false;
    }
    return true;
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

/* The index file once a flush left the symfile holding `count` entries
 * whose records end at byte `end`: the mapped prefix's checkpoints and the
 * flushed ones, the prefix's lengths (its table's) and the flushed
 * entries' (their atoms'), after every flush of a file large enough —
 * appended to the index on disk, at the cost of the records flushed
 * (dom_ix_refresh), so a reader never finds an index short of the file.
 * Called under the symfile's `.lk` flock. */
static void dom_flush_index(ray_sym_domain_t* dom, int64_t count, uint64_t end) {
    dom_lock();
    bool want = end >= dom_ix_min();
    int64_t nb = dom->blk ? dom->blk->n : 0, n = nb + dom->wck.n;
    int64_t base = dom->base_count;
    const dom_ixb_t* bl = dom->blk ? dom->blk->ib : NULL;   /* stays mapped or retired, never freed */
    uint64_t* all = want && n == dom_ck_count(count) && (base == 0 || bl)
                  ? (uint64_t*)ray_sys_alloc((size_t)n * sizeof(uint64_t)) : NULL;
    uint16_t* tl = all && count > base ? (uint16_t*)ray_sys_alloc((size_t)(count - base) * 2) : NULL;
    if (all && count > base && !tl) { ray_sys_free(all); all = NULL; }
    if (all) {
        for (int64_t k = 0; k < nb; k++) all[k] = dom->blk->ib[k].off;
        if (dom->wck.n) memcpy(all + nb, dom->wck.v, (size_t)dom->wck.n * sizeof(uint64_t));
        dom_dir_t* dir = atomic_load_explicit(&dom->atoms, memory_order_relaxed);
        for (int64_t i = base; i < count; i++) {
            ray_t* a = dom_slot_get(dir, i);
            size_t l = a ? ray_str_len(a) : 0;
            tl[i - base] = l < DOM_LEN_LONG ? (uint16_t)l : (uint16_t)DOM_LEN_LONG;
        }
    }
    int64_t ixc = dom->ix_count;
    uint64_t ixe = dom->ix_end;
    dom_unlock();
    if (!all) return;
    dom_lsrc_t ls = { bl, base, tl };
    dom_ix_refresh(dom->path, all, n, &ls, count, end, ixc, ixe, true);
    ray_sys_free(all);
    ray_sys_free(tl);
    dom_lock();
    dom->ix_count = count;
    dom->ix_end = end;
    dom_unlock();
}

ray_err_t ray_sym_domain_flush(ray_sym_domain_t* dom, bool durable) {
    if (!dom) return RAY_ERR_TYPE;
    if (dom->kind == DOM_RUNTIME) return RAY_OK; /* sym.c owns its own files */
    if (dom->kind == DOM_IMPORT) return ray_symimp_sync(dom->imp, durable);
    if (!dom->path) return RAY_ERR_DOMAIN;

    dom_lock();
    bool dirty = atomic_load_explicit(&dom->count, memory_order_relaxed) != dom->disk_count;
    dom_unlock();
    if (!dirty) return RAY_OK; /* nothing new */

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

    /* What to write, read under the flock: no other flush of this file runs
     * now, and while this process holds unflushed entries no reopen
     * replaces the mapping (dom_extend_from_file_locked refuses a dirty
     * domain).  The mapped prefix is written from its records, the rest
     * from the appended atoms (never NULL). */
    dom_lock();
    int64_t count = atomic_load_explicit(&dom->count, memory_order_relaxed);
    int64_t disk_count = dom->disk_count;
    size_t  disk_size  = dom->disk_size;
    int64_t base_count = dom->base_count;
    const uint8_t* bmap = (const uint8_t*)dom->map;
    size_t  bsize = dom->map_size;
    dom_dir_t* dir = atomic_load_explicit(&dom->atoms, memory_order_relaxed);
    dom_unlock();
    if (count == disk_count) {
        ray_file_unlock(lock_fd);
        ray_file_close(lock_fd);
        return RAY_OK;
    }

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
            /* a new file: an index left by an earlier one under this name
             * describes nothing in it */
            ray_sym_domain_index_remove(dom->path);
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

    /* Write base + tail to tmp, noting the checkpoints of the tail. */
    size_t written_size = 0;
    dom_ck_t tail = {0};
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
        if (err == RAY_OK && base_count > 0) {
            if (fwrite(bmap + 12, 1, bsize - 12, f) != bsize - 12) err = RAY_ERR_IO;
            written_size = bsize;
        }
        /* Records are packed into a large buffer first: two stdio calls per
         * entry dominated the flush of a big vocabulary. */
        enum { FLUSH_BUF = 1u << 20 };
        uint8_t* wb = (uint8_t*)ray_sys_alloc(FLUSH_BUF);
        size_t wn = 0;
        if (!wb) err = RAY_ERR_OOM;
        for (int64_t i = base_count; err == RAY_OK && i < count; i++) {
            ray_t* s = dom_slot_get(dir, i);
            if (!s) { err = RAY_ERR_CORRUPT; break; }
            if (!(i % DOM_IX_STRIDE) && !dom_ck_push(&tail, written_size)) {
                err = RAY_ERR_OOM;
                break;
            }
            size_t slen = ray_str_len(s);
            if (slen > UINT32_MAX) { err = RAY_ERR_RANGE; break; }
            uint32_t len32 = (uint32_t)slen;
            if (wn + 4 + slen > FLUSH_BUF) {
                if (wn && fwrite(wb, 1, wn, f) != wn) { err = RAY_ERR_IO; break; }
                wn = 0;
            }
            if (4 + slen > FLUSH_BUF) {
                /* Oversized record: straight through. */
                if (fwrite(&len32, 4, 1, f) != 1 ||
                    fwrite(ray_str_ptr(s), 1, slen, f) != slen) {
                    err = RAY_ERR_IO;
                    break;
                }
            } else {
                memcpy(wb + wn, &len32, 4);
                if (slen) memcpy(wb + wn + 4, ray_str_ptr(s), slen);
                wn += 4 + slen;
            }
            written_size += 4 + slen;
        }
        if (err == RAY_OK && wn && fwrite(wb, 1, wn, f) != wn) err = RAY_ERR_IO;
        ray_sys_free(wb);
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

    dom_lock();
    dom->disk_count = count;
    dom->disk_size = written_size;
    dom_ck_free(&dom->wck);
    dom->wck = tail;
    dom_unlock();
    tail.v = NULL;

    if (durable) {
        err = ray_file_sync_dir(dom->path);
        if (err != RAY_OK) {
            ray_file_unlock(lock_fd);
            ray_file_close(lock_fd);
            return err;
        }
    }

    dom_flush_index(dom, count, written_size);
    ray_file_unlock(lock_fd);
    ray_file_close(lock_fd);
    return RAY_OK;

fail_tmp:
    dom_ck_free(&tail);
    remove(tmp_path);
    ray_file_unlock(lock_fd);
    ray_file_close(lock_fd);
    return err;
}

static bool dom_fseek(FILE* f, int64_t off) {
#ifdef RAY_OS_WINDOWS
    return _fseeki64(f, off, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)off, SEEK_SET) == 0;
#endif
}

ray_err_t ray_sym_domain_flush_append(ray_sym_domain_t* dom, bool durable) {
    if (!dom) return RAY_ERR_TYPE;
    if (dom->kind == DOM_RUNTIME) return RAY_OK;
    if (dom->kind == DOM_IMPORT) return ray_symimp_sync(dom->imp, durable);
    if (!dom->path) return RAY_ERR_DOMAIN;

    dom_lock();
    bool dirty = atomic_load_explicit(&dom->count, memory_order_relaxed) != dom->disk_count;
    dom_unlock();
    if (!dirty && !durable) return RAY_OK;

    char lock_path[1024];
    if (snprintf(lock_path, sizeof(lock_path), "%s.lk", dom->path) >= (int)sizeof(lock_path))
        return RAY_ERR_IO;
    ray_fd_t lock_fd = ray_file_open(lock_path,
                                     RAY_OPEN_READ | RAY_OPEN_WRITE | RAY_OPEN_CREATE);
    if (lock_fd == RAY_FD_INVALID) return RAY_ERR_IO;
    ray_err_t err = ray_file_lock_ex(lock_fd);
    if (err != RAY_OK) { ray_file_close(lock_fd); return err; }

    /* Only the entries past the persisted prefix are written: appended
     * atoms (never NULL), read under the flock as in ray_sym_domain_flush. */
    dom_lock();
    int64_t count = atomic_load_explicit(&dom->count, memory_order_relaxed);
    int64_t disk_count = dom->disk_count;
    size_t  disk_size  = dom->disk_size;
    dom_dir_t* dir = atomic_load_explicit(&dom->atoms, memory_order_relaxed);
    dom_unlock();
    if (count == disk_count && !durable) {
        ray_file_unlock(lock_fd);
        ray_file_close(lock_fd);
        return RAY_OK;
    }

    /* The file must still be the persisted size: anything else is another
     * writer. */
    struct stat st;
    bool exists = stat(dom->path, &st) == 0;
    if (disk_count == 0 ? (exists && st.st_size > 0)
                        : (!exists || (size_t)st.st_size != disk_size)) {
        ray_file_unlock(lock_fd);
        ray_file_close(lock_fd);
        return RAY_ERR_CORRUPT;
    }
    if (disk_count == 0) ray_sym_domain_index_remove(dom->path);   /* a new file */
    FILE* f = fopen(dom->path, disk_count == 0 ? "wb+" : "rb+");
    if (!f) { ray_file_unlock(lock_fd); ray_file_close(lock_fd); return RAY_ERR_IO; }
    size_t size = disk_size;
    uint32_t magic = DOMAIN_STRL_MAGIC;
    if (disk_count == 0) {
        int64_t zero = 0;
        if (fwrite(&magic, 4, 1, f) != 1 || fwrite(&zero, 8, 1, f) != 1) err = RAY_ERR_IO;
        size = 12;
    } else if (!dom_fseek(f, (int64_t)disk_size)) {
        err = RAY_ERR_IO;
    }
    dom_ck_t tail = {0};
    enum { FLUSH_BUF = 1u << 20 };
    uint8_t* wb = err == RAY_OK ? (uint8_t*)ray_sys_alloc(FLUSH_BUF) : NULL;
    size_t wn = 0;
    if (err == RAY_OK && !wb) err = RAY_ERR_OOM;
    for (int64_t i = disk_count; err == RAY_OK && i < count; i++) {
        ray_t* s = dom_slot_get(dir, i);
        if (!s) { err = RAY_ERR_CORRUPT; break; }
        if (!(i % DOM_IX_STRIDE) && !dom_ck_push(&tail, size)) { err = RAY_ERR_OOM; break; }
        size_t slen = ray_str_len(s);
        if (slen > UINT32_MAX) { err = RAY_ERR_RANGE; break; }
        uint32_t len32 = (uint32_t)slen;
        if (wn + 4 + slen > FLUSH_BUF) {
            if (wn && fwrite(wb, 1, wn, f) != wn) { err = RAY_ERR_IO; break; }
            wn = 0;
        }
        if (4 + slen > FLUSH_BUF) {
            if (fwrite(&len32, 4, 1, f) != 1 || fwrite(ray_str_ptr(s), 1, slen, f) != slen) {
                err = RAY_ERR_IO;
                break;
            }
        } else {
            memcpy(wb + wn, &len32, 4);
            if (slen) memcpy(wb + wn + 4, ray_str_ptr(s), slen);
            wn += 4 + slen;
        }
        size += 4 + slen;
    }
    if (err == RAY_OK && wn && fwrite(wb, 1, wn, f) != wn) err = RAY_ERR_IO;
    ray_sys_free(wb);
    /* the count goes in last, once the records it covers are written */
    if (err == RAY_OK && (fflush(f) != 0 || !dom_fseek(f, 4) ||
                          fwrite(&count, 8, 1, f) != 1 || fflush(f) != 0))
        err = RAY_ERR_IO;
    /* start writing the new records back now, so a later durable flush
     * waits for little */
    if (err == RAY_OK && !durable)
        ray_file_writeback_start(fileno(f), (int64_t)disk_size, (int64_t)(size - disk_size));
    if (fclose(f) != 0 && err == RAY_OK) err = RAY_ERR_IO;
    if (err == RAY_OK && durable) {
        ray_fd_t fd = ray_file_open(dom->path, RAY_OPEN_READ | RAY_OPEN_WRITE);
        if (fd == RAY_FD_INVALID) err = RAY_ERR_IO;
        else { err = ray_file_sync(fd); ray_file_close(fd); }
        if (err == RAY_OK) err = ray_file_sync_dir(dom->path);
    }
    if (err == RAY_OK) {
        dom_lock();
        dom->disk_count = count;
        dom->disk_size = size;
        for (int64_t k = 0; err == RAY_OK && k < tail.n; k++)
            if (!dom_ck_push(&dom->wck, tail.v[k])) err = RAY_ERR_OOM;
        if (err != RAY_OK) dom_ck_free(&dom->wck);   /* no index from here on */
        dom_unlock();
        if (err == RAY_OK) dom_flush_index(dom, count, size);
        err = RAY_OK;   /* the symfile is written; the index is a cache */
    }
    dom_ck_free(&tail);
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
