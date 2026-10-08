/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#if defined(__linux__)
#  define _GNU_SOURCE
#endif
#include "symimp.h"
#include "ops/hash.h"   /* ray_hash_bytes: position 0 is "" */

#if defined(RAY_OS_LINUX) || defined(RAY_OS_MACOS) || defined(__linux__) || defined(__APPLE__)

#include "core/platform.h"
#include "mem/sys.h"
#include "store/fileio.h"   /* ray_file_sync_dir */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define SI_MAGIC     0x4C525453U          /* "STRL", the symbol file's magic */
#define SI_HEAD      12                   /* magic + count */
#define SI_SHARD_LOG 10
#define SI_SHARDS    (1 << SI_SHARD_LOG)  /* by the hash's top bits */
#define SI_TAB0      1024                 /* initial slots per shard */
#define SI_OFF_LOG   20                   /* offsets in chunks of 2^20 */
#define SI_OFF_CHUNKS 4096                /* 2^32 entries at most */
#define SI_GROW_MIN  ((int64_t)1 << 20)
#define SI_GROW_MAX  ((int64_t)1 << 30)

#define SI_BATCH     8192                 /* strings deduplicated together */

/* An index entry is (hash << 32) | (position + 1); 0 is an empty slot. */
typedef struct si_tab_s {
    uint64_t         mask;
    struct si_tab_s* next;         /* retired: the next one retired with it */
    _Atomic(uint64_t) e[];
} si_tab_t;
typedef struct {
    _Atomic(si_tab_t*) tab;
    int64_t            used;      /* under lock */
} si_shard_t;

struct ray_symimp_s {
    int      fd;
    char*    path;
    uint8_t* map;                  /* the file, mapped over `reserve` bytes */
    size_t   reserve;
    pthread_mutex_t alock;         /* every change: positions, records, the file
                                    * size and the shard tables */
    int64_t  count;
    int64_t  tail;                 /* bytes of records + header */
    int64_t  fsize;                /* the file's current length */
    int64_t  synced;               /* bytes whose writeback was started */
    _Atomic(int64_t)* count_out;
    /* A replaced shard table may still be read by a lock-free lookup that
     * loaded it.  Lookups register in the parity of the epoch they start
     * in; a table retired in epoch e is freed once the epoch has moved
     * past e + 1, which it only does when no lookup of the other parity
     * is left (si_reclaim). */
    _Atomic(uint64_t) epoch;
    _Atomic(int64_t)  active[2];
    si_tab_t*        retired[2];  /* under alock, by the parity retired in */
    uint64_t* offc[SI_OFF_CHUNKS]; /* record offset of each position */
    si_shard_t shards[SI_SHARDS];
};

static si_tab_t* si_tab_new(uint64_t cap) {
    si_tab_t* t = (si_tab_t*)ray_sys_alloc(sizeof(si_tab_t) + (size_t)cap * sizeof(uint64_t));
    if (!t) return NULL;
    t->mask = cap - 1;
    t->next = NULL;
    memset((void*)t->e, 0, (size_t)cap * sizeof(uint64_t));
    return t;
}

static inline uint64_t si_off(const ray_symimp_t* m, int64_t pos) {
    return m->offc[pos >> SI_OFF_LOG][pos & ((1 << SI_OFF_LOG) - 1)];
}

static inline bool si_eq(const ray_symimp_t* m, int64_t pos, const char* s, size_t len) {
    const uint8_t* r = m->map + si_off(m, pos);
    uint32_t l;
    memcpy(&l, r, 4);
    return l == len && (len == 0 || memcmp(r + 4, s, len) == 0);
}

static int64_t si_find(const ray_symimp_t* m, const si_tab_t* t, uint32_t h,
                       const char* s, size_t len) {
    uint64_t slot = h & t->mask;
    for (;;) {
        uint64_t e = atomic_load_explicit(&t->e[slot], memory_order_acquire);
        if (!e) return -1;
        if ((uint32_t)(e >> 32) == h) {
            int64_t pos = (int64_t)(uint32_t)e - 1;
            if (si_eq(m, pos, s, len)) return pos;
        }
        slot = (slot + 1) & t->mask;
    }
}

static void si_put(si_tab_t* t, uint64_t e) {
    uint64_t slot = (uint32_t)(e >> 32) & t->mask;
    while (atomic_load_explicit(&t->e[slot], memory_order_relaxed)) slot = (slot + 1) & t->mask;
    atomic_store_explicit(&t->e[slot], e, memory_order_release);
}

/* The file grown to `want` bytes.  The blocks are allocated, where the
 * filesystem can, so a full disk fails here rather than as a fault on a
 * later write through the mapping. */
static bool si_grow(ray_symimp_t* m, int64_t want) {
#if defined(__linux__)
    if (fallocate(m->fd, 0, (off_t)m->fsize, (off_t)(want - m->fsize)) == 0) return true;
    if (errno != EOPNOTSUPP) return false;
#endif
    return ftruncate(m->fd, (off_t)want) == 0;
}

/* Room for `bytes` more record bytes: the next position and its offset.
 * The file grows ahead of the tail (the mapping covers it already).
 * Under alock. */
static bool si_reserve(ray_symimp_t* m, size_t bytes, int64_t* pos, int64_t* off) {
    if (m->count >= (int64_t)UINT32_MAX - 1) return false;
    int64_t p = m->count, chunk = p >> SI_OFF_LOG;
    if (!m->offc[chunk]) {
        m->offc[chunk] = (uint64_t*)ray_sys_alloc(((size_t)1 << SI_OFF_LOG) * sizeof(uint64_t));
        if (!m->offc[chunk]) return false;
    }
    if (m->tail + (int64_t)bytes > m->fsize) {
        int64_t grow = m->fsize < SI_GROW_MIN ? SI_GROW_MIN : m->fsize > SI_GROW_MAX ? SI_GROW_MAX : m->fsize;
        int64_t need = m->tail + (int64_t)bytes, want = need + grow;
        if ((size_t)want > m->reserve) want = (int64_t)m->reserve;
        if (want < need || !si_grow(m, want)) return false;
        m->fsize = want;
    }
    *pos = p; *off = m->tail;
    m->offc[chunk][p & ((1 << SI_OFF_LOG) - 1)] = (uint64_t)m->tail;
    m->tail += (int64_t)bytes;
    m->count = p + 1;
    atomic_store_explicit(m->count_out, p + 1, memory_order_release);
    return true;
}

/* Under alock: the string is not in the shard; append it. */
static int64_t si_add(ray_symimp_t* m, si_shard_t* sh, uint32_t h, const char* s, size_t len) {
    si_tab_t* t = atomic_load_explicit(&sh->tab, memory_order_relaxed);
    if ((uint64_t)(sh->used + 1) * 2 > t->mask + 1) {
        si_tab_t* nt = si_tab_new((t->mask + 1) * 2);
        if (!nt) return -1;
        for (uint64_t i = 0; i <= t->mask; i++) {
            uint64_t e = atomic_load_explicit(&t->e[i], memory_order_relaxed);
            if (e) si_put(nt, e);
        }
        atomic_store_explicit(&sh->tab, nt, memory_order_seq_cst);
        uint64_t ep = atomic_load_explicit(&m->epoch, memory_order_relaxed) & 1;
        t->next = m->retired[ep]; m->retired[ep] = t;
        t = nt;
    }
    int64_t pos, off;
    if (!si_reserve(m, 4 + len, &pos, &off)) return -1;
    uint32_t l32 = (uint32_t)len;
    memcpy(m->map + off, &l32, 4);
    if (len) memcpy(m->map + off + 4, s, len);
    si_put(t, ((uint64_t)h << 32) | (uint64_t)(pos + 1));
    sh->used++;
    return pos;
}

/* Under alock: free the tables no lookup can still hold and move the
 * epoch on, unless a lookup of the other parity is still running. */
static void si_reclaim(ray_symimp_t* m) {
    uint64_t e = atomic_load_explicit(&m->epoch, memory_order_relaxed);
    if (atomic_load_explicit(&m->active[(e + 1) & 1], memory_order_seq_cst) != 0) return;
    for (si_tab_t* t = m->retired[(e + 1) & 1]; t;) { si_tab_t* nx = t->next; ray_sys_free(t); t = nx; }
    m->retired[(e + 1) & 1] = NULL;
    atomic_store_explicit(&m->epoch, e + 1, memory_order_seq_cst);
}

/* Up to SI_BATCH strings.  Equal strings of the batch are looked up once
 * (a column's neighbouring rows repeat), each against the batch's own
 * bytes rather than a record of the file.  Hits are found without a lock.
 * The batch's misses are added together under alock: the lock is taken
 * once per batch rather than per string, and the new strings get
 * neighbouring positions, as the ordinary domain's batch commit gives
 * them (a column's rows then index nearby positions, which its lookups
 * over the vocabulary feel). */
static bool si_batch(ray_symimp_t* m, int64_t n, const char* const* strs,
                     const size_t* lens, const uint32_t* hashes, int64_t* out_pos) {
    uint16_t loc[2 * SI_BATCH];    /* batch slot -> first such string + 1 */
    uint16_t first[SI_BATCH];
    memset(loc, 0, sizeof(loc));
    for (int64_t i = 0; i < n; i++) {
        size_t len = strs[i] ? lens[i] : 0;
        if (len > UINT32_MAX - 4) return false;
        const char* s = strs[i] ? strs[i] : "";
        uint32_t slot = hashes[i] & (2 * SI_BATCH - 1);
        for (;;) {
            uint16_t j1 = loc[slot];
            if (!j1) { loc[slot] = (uint16_t)(i + 1); first[i] = (uint16_t)i; break; }
            int64_t j = j1 - 1;
            size_t jl = strs[j] ? lens[j] : 0;
            if (hashes[j] == hashes[i] && jl == len &&
                (len == 0 || memcmp(strs[j], s, len) == 0)) { first[i] = (uint16_t)j; break; }
            slot = (slot + 1) & (2 * SI_BATCH - 1);
        }
    }
    int64_t miss = 0;
    uint64_t e;
    for (;;) {
        e = atomic_load_explicit(&m->epoch, memory_order_seq_cst);
        atomic_fetch_add_explicit(&m->active[e & 1], 1, memory_order_seq_cst);
        if (atomic_load_explicit(&m->epoch, memory_order_seq_cst) == e) break;
        atomic_fetch_sub_explicit(&m->active[e & 1], 1, memory_order_release);
    }
    for (int64_t i = 0; i < n; i++) {
        if (first[i] != i) continue;
        uint32_t h = hashes[i];
        si_shard_t* sh = &m->shards[h >> (32 - SI_SHARD_LOG)];
        out_pos[i] = si_find(m, atomic_load_explicit(&sh->tab, memory_order_acquire), h,
                             strs[i] ? strs[i] : "", strs[i] ? lens[i] : 0);
        if (out_pos[i] < 0) miss++;
    }
    atomic_fetch_sub_explicit(&m->active[e & 1], 1, memory_order_release);
    bool ok = true;
    if (miss) {
        pthread_mutex_lock(&m->alock);
        for (int64_t i = 0; i < n && ok; i++) {
            if (first[i] != i || out_pos[i] >= 0) continue;
            uint32_t h = hashes[i];
            const char* s = strs[i] ? strs[i] : "";
            size_t len = strs[i] ? lens[i] : 0;
            si_shard_t* sh = &m->shards[h >> (32 - SI_SHARD_LOG)];
            int64_t pos = si_find(m, atomic_load_explicit(&sh->tab, memory_order_relaxed), h, s, len);
            if (pos < 0) pos = si_add(m, sh, h, s, len);
            if (pos < 0) ok = false;
            else out_pos[i] = pos;
        }
        si_reclaim(m);
        pthread_mutex_unlock(&m->alock);
    }
    for (int64_t i = 0; ok && i < n; i++) if (first[i] != i) out_pos[i] = out_pos[first[i]];
    return ok;
}

bool ray_symimp_intern_batch(ray_symimp_t* m, int64_t n, const char* const* strs,
                             const size_t* lens, const uint32_t* hashes,
                             int64_t* out_pos) {
    for (int64_t o = 0; o < n; o += SI_BATCH)
        if (!si_batch(m, n - o < SI_BATCH ? n - o : SI_BATCH, strs + o, lens + o, hashes + o, out_pos + o))
            return false;
    return true;
}

ray_symimp_t* ray_symimp_create(const char* path, _Atomic(int64_t)* count) {
    if (!path || !count) return NULL;
    ray_symimp_t* m = (ray_symimp_t*)ray_sys_alloc(sizeof(*m));
    if (!m) return NULL;
    memset(m, 0, sizeof(*m));
    atomic_init(&m->epoch, 0);
    atomic_init(&m->active[0], 0);
    atomic_init(&m->active[1], 0);
    m->fd = -1;
    m->count_out = count;
    size_t pl = strlen(path);
    m->path = (char*)ray_sys_alloc(pl + 1);
    if (!m->path) goto fail;
    memcpy(m->path, path, pl + 1);
    m->fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (m->fd < 0) goto fail;
    /* Address space for the whole file up front: it only grows by ftruncate. */
    for (m->reserve = (size_t)1 << 40; m->reserve >= ((size_t)1 << 30); m->reserve >>= 2) {
        void* p = mmap(NULL, m->reserve, PROT_READ | PROT_WRITE, MAP_SHARED, m->fd, 0);
        if (p != MAP_FAILED) { m->map = (uint8_t*)p; break; }
    }
    if (!m->map) goto fail;
    /* lookups read one record each, anywhere in the file: no read-around */
    madvise(m->map, m->reserve, MADV_RANDOM);
    if (pthread_mutex_init(&m->alock, NULL) != 0) goto fail;
    for (int i = 0; i < SI_SHARDS; i++) {
        si_tab_t* t = si_tab_new(SI_TAB0);
        if (!t) goto fail;
        atomic_init(&m->shards[i].tab, t);
    }
    m->tail = SI_HEAD;
    if (!si_grow(m, SI_GROW_MIN)) goto fail;
    m->fsize = SI_GROW_MIN;
    uint32_t magic = SI_MAGIC;
    memcpy(m->map, &magic, 4);
    memset(m->map + 4, 0, 8);
    /* position 0 is the empty string, as in every symbol file */
    {
        const char* e = "";
        size_t z = 0;
        uint32_t h0 = (uint32_t)ray_hash_bytes("", 0);
        int64_t p0 = -1;
        if (!ray_symimp_intern_batch(m, 1, &e, &z, &h0, &p0) || p0 != 0) goto fail;
    }
    return m;
fail:
    /* the caller falls back to the ordinary domain, which must not find
     * this file half made */
    if (m->fd >= 0) unlink(m->path);
    ray_symimp_free(m);
    return NULL;
}

ray_err_t ray_symimp_sync(ray_symimp_t* m, bool durable) {
    if (!m) return RAY_ERR_TYPE;
    pthread_mutex_lock(&m->alock);
    int64_t count = m->count, tail = m->tail, from = m->synced;
    memcpy(m->map + 4, &count, 8);
    m->synced = tail;
    ray_err_t err = RAY_OK;
    if (durable) {
        if (ftruncate(m->fd, (off_t)tail) != 0) err = RAY_ERR_IO;
        else m->fsize = tail;
    }
    pthread_mutex_unlock(&m->alock);
    if (err != RAY_OK) return err;
    if (!durable) {
        ray_file_writeback_start(m->fd, from, tail - from);
        return RAY_OK;
    }
    if (msync(m->map, (size_t)tail, MS_SYNC) != 0 || fsync(m->fd) != 0) return RAY_ERR_IO;
    return ray_file_sync_dir(m->path);
}

void ray_symimp_free(ray_symimp_t* m) {
    if (!m) return;
    for (int i = 0; i < SI_SHARDS; i++) {
        si_shard_t* sh = &m->shards[i];
        ray_sys_free(atomic_load_explicit(&sh->tab, memory_order_relaxed));
    }
    for (int k = 0; k < 2; k++)
        for (si_tab_t* t = m->retired[k]; t;) { si_tab_t* nx = t->next; ray_sys_free(t); t = nx; }
    for (int c = 0; c < SI_OFF_CHUNKS; c++) ray_sys_free(m->offc[c]);
    if (m->map) munmap(m->map, m->reserve);
    if (m->fd >= 0) close(m->fd);
    ray_sys_free(m->path);
    ray_sys_free(m);
}

#else  /* no import dictionary: callers keep the ordinary domain */

ray_symimp_t* ray_symimp_create(const char* path, _Atomic(int64_t)* count) { (void)path; (void)count; return NULL; }
bool ray_symimp_intern_batch(ray_symimp_t* m, int64_t n, const char* const* strs,
                             const size_t* lens, const uint32_t* hashes, int64_t* out_pos) {
    (void)m; (void)n; (void)strs; (void)lens; (void)hashes; (void)out_pos; return false;
}
ray_err_t ray_symimp_sync(ray_symimp_t* m, bool durable) { (void)m; (void)durable; return RAY_ERR_IO; }
void ray_symimp_free(ray_symimp_t* m) { (void)m; }

#endif
