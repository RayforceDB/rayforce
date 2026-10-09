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

#include "symgrp.h"
#include "symimp.h"

#if defined(RAY_OS_LINUX) || defined(RAY_OS_MACOS) || defined(__linux__) || defined(__APPLE__)

#include "mem/sys.h"
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

/* ---- append-only chunk lists --------------------------------------------
 * Items of one size in chunks that double from 16 KB to 4 MB: a small list
 * stays small, a big one is made of blocks large enough to spill to a file
 * past the anon watermark (mem/sys.h), and is read back in order. */
typedef struct sg_chunk_s {
    struct sg_chunk_s* next;
    int64_t n, cap;
    _Alignas(8) uint8_t data[];
} sg_chunk_t;
typedef struct { sg_chunk_t *head, *tail; int64_t n; } sg_list_t;

#define SG_CHUNK_MIN ((int64_t)16 << 10)
#define SG_CHUNK_MAX ((int64_t)4 << 20)

static void* sg_push(sg_list_t* l, size_t isz) {
    sg_chunk_t* c = l->tail;
    if (!c || c->n == c->cap) {
        int64_t bytes = c ? c->cap * (int64_t)isz * 2 : SG_CHUNK_MIN;
        if (bytes > SG_CHUNK_MAX) bytes = SG_CHUNK_MAX;
        int64_t cap = bytes / (int64_t)isz;
        if (cap < 1) cap = 1;
        sg_chunk_t* nc = (sg_chunk_t*)ray_sys_alloc(sizeof(sg_chunk_t) + (size_t)cap * isz);
        if (!nc) return NULL;
        nc->next = NULL; nc->n = 0; nc->cap = cap;
        if (c) c->next = nc; else l->head = nc;
        l->tail = c = nc;
    }
    l->n++;
    return c->data + (size_t)(c->n++) * isz;
}
static void sg_list_free(sg_list_t* l) {
    for (sg_chunk_t* c = l->head; c;) { sg_chunk_t* nx = c->next; ray_sys_free(c); c = nx; }
    l->head = l->tail = NULL; l->n = 0;
}

/* Bytes kept for the deferred candidates: the same lists, raw. */
static const char* sg_keep(sg_list_t* l, const char* s, uint32_t len) {
    sg_chunk_t* c = l->tail;
    if (!c || c->cap - c->n < (int64_t)len) {
        int64_t cap = c ? c->cap * 2 : SG_CHUNK_MIN;
        if (cap > SG_CHUNK_MAX) cap = SG_CHUNK_MAX;
        if (cap < (int64_t)len) cap = len;
        sg_chunk_t* nc = (sg_chunk_t*)ray_sys_alloc(sizeof(sg_chunk_t) + (size_t)(cap ? cap : 1));
        if (!nc) return NULL;
        nc->next = NULL; nc->n = 0; nc->cap = cap;
        if (c) c->next = nc; else l->head = nc;
        l->tail = c = nc;
    }
    char* p = (char*)c->data + c->n;
    if (len) memcpy(p, s, len);
    c->n += len;
    return p;
}

/* Stable sort of 64-bit words by their high 32 bits (two 16-bit radix
 * passes); tmp is n words. */
static bool sg_sort_hi(uint64_t* a, uint64_t* tmp, int64_t n) {
    if (n < 2) return true;
    int64_t* cnt = (int64_t*)ray_sys_alloc(65536 * sizeof(int64_t));
    if (!cnt) return false;
    for (int pass = 0; pass < 2; pass++) {
        int sh = 32 + 16 * pass;
        memset(cnt, 0, 65536 * sizeof(int64_t));
        for (int64_t i = 0; i < n; i++) cnt[(a[i] >> sh) & 0xffff]++;
        int64_t s = 0;
        for (int d = 0; d < 65536; d++) { int64_t c = cnt[d]; cnt[d] = s; s += c; }
        for (int64_t i = 0; i < n; i++) tmp[cnt[(a[i] >> sh) & 0xffff]++] = a[i];
        memcpy(a, tmp, (size_t)n * sizeof(uint64_t));
    }
    ray_sys_free(cnt);
    return true;
}

/* ---- the index -------------------------------------------------------- */

typedef struct { uint64_t h; uint32_t len, pos; } sg_ent_t;                 /* a log entry */
typedef struct { uint64_t h; uint32_t len, t, local, count; } sg_own_t;      /* a new string */
typedef struct { int64_t t; uint64_t h; const char* s; uint32_t local, pos, len; } sg_def_t;
typedef struct { int64_t t; uint32_t local, pos; } sg_ovr_t;

/* Verdict words: kind in the top 2 bits; NEW and REF hold an owner id, OLD
 * a position. */
#define SG_KIND(w)  ((uint32_t)((w) >> 62))
#define SG_VAL(w)   ((w) & ((UINT64_C(1) << 62) - 1))
#define SG_WORD(k, v) (((uint64_t)(k) << 62) | (uint64_t)(v))

typedef struct {
    int64_t n;                 /* staged local ids */
    ray_symgrp_fp_t* fp;       /* [n], by hash group (stable: local id order in each) */
    int64_t* seg;              /* [groups + 1] */
    uint64_t* r;               /* [n] verdict words, as fp */
    uint32_t* cnt;             /* FREQ: [n + 1] rows by local id */
    uint64_t* own;             /* ROWS*: bit per local id of the new strings */
    uint64_t* again;           /* ROWS*: of them, those a later task meets too */
    uint32_t* rank;            /* ROWS*: the others before each bitmap word (positions) */
    uint32_t* arank;           /* ROWS*: those met again before it (positions) */
    int64_t owned, bytes, cand;
    int64_t nagain, abytes;    /* ROWS: the strings met again, their records' bytes */
} sg_task_t;

struct ray_symgrp_s {
    ray_symimp_t*      imp;
    ray_symgrp_order_t order;
    pthread_mutex_t    lock;              /* ray_symgrp_collide */
    sg_list_t          logs[RAY_SYMGRP_MAX];
    int64_t            entries;
    /* the pass */
    int                lg;                /* log2 of the groups */
    int64_t            ntasks, workers;
    sg_task_t*         tasks;
    int64_t            nown, pos0, off0;
    uint32_t*          pos_of;            /* [nown] */
    uint32_t*          t_of;              /* [nown] */
    int64_t*           off_of;            /* [nown]: SHARDS, FREQ */
    int64_t*           offb;              /* [2 ntasks]: ROWS*, a task's records (the
                                           * others, then those met again) */
    uint32_t*          gslots;            /* step 2's group table, sized for the largest */
    sg_ent_t*          gent;
    sg_own_t*          gnew;
    /* the window */
    uint32_t*          spos;              /* loaded positions, ascending */
    uint32_t*          slen;              /* their records' lengths */
    int64_t*           soff;              /* [sn + 1] into sbuf */
    char*              sbuf;
    int64_t            sn;
    sg_list_t*         dl;                /* [workers] deferred candidates */
    sg_list_t*         db;                /* [workers] their bytes */
    sg_ovr_t*          ovr;               /* by (t, local) */
    int64_t            novr;
    ray_symgrp_stats_t st;
    _Atomic(int64_t)   compares, cmp_bytes;
};

/* ROWS and ROWS_FLAT: positions by task and local id, no table by string. */
static inline bool sg_rows(const ray_symgrp_t* g) {
    return g->order == RAY_SYMGRP_ROWS || g->order == RAY_SYMGRP_ROWS_FLAT;
}

ray_symgrp_t* ray_symgrp_new(ray_symimp_t* imp, ray_symgrp_order_t order) {
    if (!imp) return NULL;
    ray_symgrp_t* g = (ray_symgrp_t*)ray_sys_alloc(sizeof(*g));
    if (!g) return NULL;
    memset(g, 0, sizeof(*g));
    g->imp = imp; g->order = order;
    atomic_init(&g->compares, 0); atomic_init(&g->cmp_bytes, 0);
    if (pthread_mutex_init(&g->lock, NULL) != 0) { ray_sys_free(g); return NULL; }
    return g;
}

static void sg_window_free(ray_symgrp_t* g) {
    ray_sys_free(g->spos); ray_sys_free(g->slen); ray_sys_free(g->soff); ray_sys_free(g->sbuf);
    g->spos = NULL; g->slen = NULL; g->soff = NULL; g->sbuf = NULL; g->sn = 0;
    if (g->dl) for (int64_t w = 0; w < g->workers; w++) { sg_list_free(&g->dl[w]); sg_list_free(&g->db[w]); }
}

void ray_symgrp_end(ray_symgrp_t* g) {
    if (!g) return;
    sg_window_free(g);
    ray_sys_free(g->dl); ray_sys_free(g->db); g->dl = g->db = NULL;
    ray_sys_free(g->ovr); g->ovr = NULL; g->novr = 0;
    if (g->tasks) for (int64_t t = 0; t < g->ntasks; t++) {
        sg_task_t* k = &g->tasks[t];
        ray_sys_free(k->fp); ray_sys_free(k->seg); ray_sys_free(k->r);
        ray_sys_free(k->cnt); ray_sys_free(k->own); ray_sys_free(k->rank);
        ray_sys_free(k->again); ray_sys_free(k->arank);
    }
    ray_sys_free(g->tasks); g->tasks = NULL; g->ntasks = 0;
    ray_sys_free(g->pos_of); ray_sys_free(g->t_of); ray_sys_free(g->off_of); ray_sys_free(g->offb);
    g->pos_of = NULL; g->t_of = NULL; g->off_of = NULL; g->offb = NULL; g->nown = 0;
}

void ray_symgrp_free(ray_symgrp_t* g) {
    if (!g) return;
    ray_symgrp_end(g);
    for (int i = 0; i < RAY_SYMGRP_MAX; i++) sg_list_free(&g->logs[i]);
    pthread_mutex_destroy(&g->lock);
    ray_sys_free(g);
}

bool ray_symgrp_begin(ray_symgrp_t* g, int64_t ntasks, int groups, int64_t workers) {
    ray_symgrp_end(g);
    int lg = 0;
    while ((1 << lg) < groups && lg < RAY_SYMGRP_LOG_BITS) lg++;
    g->lg = lg;
    g->ntasks = ntasks < 0 ? 0 : ntasks;
    g->workers = workers < 1 ? 1 : workers;
    g->tasks = (sg_task_t*)ray_sys_alloc((size_t)(g->ntasks ? g->ntasks : 1) * sizeof(sg_task_t));
    g->dl = (sg_list_t*)ray_sys_alloc((size_t)g->workers * sizeof(sg_list_t));
    g->db = (sg_list_t*)ray_sys_alloc((size_t)g->workers * sizeof(sg_list_t));
    if (!g->tasks || !g->dl || !g->db) { ray_symgrp_end(g); return false; }
    return true;   /* zero-filled (mem/sys.h) */
}

static inline int sg_group(const ray_symgrp_t* g, uint64_t h) {
    return g->lg ? (int)(h >> (64 - g->lg)) : 0;
}
static inline int sg_log(uint64_t h) { return (int)(h >> (64 - RAY_SYMGRP_LOG_BITS)); }

bool ray_symgrp_stage(ray_symgrp_t* g, int64_t t, int64_t n, const ray_symgrp_fp_t* fp,
                      const uint32_t* counts) {
    if (t < 0 || t >= g->ntasks || n < 0) return false;
    sg_task_t* k = &g->tasks[t];
    int ng = 1 << g->lg;
    k->n = n;
    k->seg = (int64_t*)ray_sys_alloc((size_t)(ng + 1) * sizeof(int64_t));
    k->fp = (ray_symgrp_fp_t*)ray_sys_alloc((size_t)(n ? n : 1) * sizeof(*fp));
    k->r = (uint64_t*)ray_sys_alloc((size_t)(n ? n : 1) * sizeof(uint64_t));
    if (!k->seg || !k->fp || !k->r) return false;
    for (int64_t i = 0; i < n; i++) k->seg[sg_group(g, fp[i].h) + 1]++;
    for (int i = 0; i < ng; i++) k->seg[i + 1] += k->seg[i];
    int64_t* at = (int64_t*)ray_sys_alloc((size_t)ng * sizeof(int64_t));
    if (!at) return false;
    memcpy(at, k->seg, (size_t)ng * sizeof(int64_t));
    for (int64_t i = 0; i < n; i++) k->fp[at[sg_group(g, fp[i].h)]++] = fp[i];
    ray_sys_free(at);
    if (g->order == RAY_SYMGRP_FREQ) {
        k->cnt = (uint32_t*)ray_sys_alloc((size_t)(n + 1) * sizeof(uint32_t));
        if (!k->cnt) return false;
        for (int64_t i = 0; i < n; i++) k->cnt[i + 1] = counts ? counts[i] : 1;
    }
    if (sg_rows(g)) {
        int64_t words = (n + 1 + 63) / 64;
        k->own = (uint64_t*)ray_sys_alloc((size_t)words * sizeof(uint64_t));
        k->again = (uint64_t*)ray_sys_alloc((size_t)words * sizeof(uint64_t));
        k->rank = (uint32_t*)ray_sys_alloc((size_t)words * sizeof(uint32_t));
        k->arank = (uint32_t*)ray_sys_alloc((size_t)words * sizeof(uint32_t));
        if (!k->own || !k->again || !k->rank || !k->arank) return false;
    }
    return true;
}

static uint64_t sg_group_cap(int64_t ne, int64_t ns) {
    uint64_t cap = 16;
    while (cap < (uint64_t)(ne + ns) * 2) cap <<= 1;
    return cap;
}

/* ROWS*: a new string's position, from its task's base (of the strings met
 * again or of the others) and the new strings of its kind before it among
 * the task's local ids. */
static inline uint32_t sg_rank_pos(const ray_symgrp_t* g, int64_t t, uint32_t local) {
    const sg_task_t* k = &g->tasks[t];
    uint64_t below = (UINT64_C(1) << (local & 63)) - 1, again = k->again[local >> 6];
    if (again >> (local & 63) & 1)
        return (uint32_t)(g->pos0 + k->arank[local >> 6] + (uint32_t)__builtin_popcountll(again & below));
    uint64_t word = k->own[local >> 6] & ~again & below;
    return (uint32_t)(g->pos0 + k->rank[local >> 6] + (uint32_t)__builtin_popcountll(word));
}
/* A REF's owner task and position: ROWS* keep (task, local id) in the
 * word, the other orders an owner id. */
static inline uint32_t sg_ref_task(const ray_symgrp_t* g, uint64_t v) {
    return sg_rows(g) ? (uint32_t)(v >> 32) : g->t_of[v];
}
static inline uint32_t sg_ref_pos(const ray_symgrp_t* g, uint64_t v) {
    return sg_rows(g) ? sg_rank_pos(g, (int64_t)(v >> 32), (uint32_t)v) : g->pos_of[v];
}

/* Step 2, one hash group: its log entries (the logs it spans) in a table,
 * then every task's staged strings of the group in task order. */
static bool sg_resolve_group(ray_symgrp_t* g, int gi, sg_list_t* owners, int64_t* next_id) {
    int ng = 1 << g->lg, per = RAY_SYMGRP_MAX / ng;
    int64_t ne = 0, ns = 0;
    for (int l = gi * per; l < (gi + 1) * per; l++) ne += g->logs[l].n;
    for (int64_t t = 0; t < g->ntasks; t++) ns += g->tasks[t].seg[gi + 1] - g->tasks[t].seg[gi];
    if (!ns) return true;
    uint64_t cap = sg_group_cap(ne, ns), mask = cap - 1;
    /* the pass's group buffers, sized for its largest group (sg_resolve) */
    uint32_t* slots = g->gslots; sg_ent_t* ent = g->gent; sg_own_t* nw = g->gnew;
    memset(slots, 0, (size_t)cap * sizeof(uint32_t));
    bool ok = true, rows = sg_rows(g), split = g->order == RAY_SYMGRP_ROWS;
    int64_t e = 0;
    for (int l = gi * per; ok && l < (gi + 1) * per; l++)
        for (sg_chunk_t* c = g->logs[l].head; c; c = c->next) {
            memcpy(ent + e, c->data, (size_t)c->n * sizeof(sg_ent_t));
            e += c->n;
        }
    for (int64_t i = 0; ok && i < ne; i++) {
        uint64_t s = ent[i].h & mask;
        while (slots[s]) s = (s + 1) & mask;
        slots[s] = (uint32_t)(i + 1);
    }
    int64_t nn = 0;
    for (int64_t t = 0; ok && t < g->ntasks; t++) {
        sg_task_t* k = &g->tasks[t];
        for (int64_t i = k->seg[gi]; i < k->seg[gi + 1]; i++) {
            const ray_symgrp_fp_t* f = &k->fp[i];
            uint32_t cnt = k->cnt ? k->cnt[f->local] : 1;
            uint64_t s = f->h & mask, w = 0;
            for (;;) {
                uint32_t v = slots[s];
                if (!v) break;
                if (v <= ne) {
                    const sg_ent_t* x = &ent[v - 1];
                    if (x->h == f->h && x->len == f->len) { w = SG_WORD(RAY_SYMGRP_OLD, x->pos); break; }
                } else {
                    sg_own_t* o = &nw[v - ne - 1];
                    if (o->h == f->h && o->len == f->len) {
                        o->count += cnt;
                        /* ROWS*: the owner itself (task, local id), its
                         * position computed from the tasks' bitmaps */
                        w = rows ? SG_WORD(RAY_SYMGRP_REF, ((uint64_t)o->t << 32) | o->local)
                                 : SG_WORD(RAY_SYMGRP_REF, *next_id + (int64_t)(v - ne - 1));
                        /* ROWS: met again by a later task, its record goes
                         * with the others a later window may compare with */
                        sg_task_t* ot = &g->tasks[o->t];
                        uint64_t bit = UINT64_C(1) << (o->local & 63);
                        if (split && o->t != (uint32_t)t && !(ot->again[o->local >> 6] & bit)) {
                            ot->again[o->local >> 6] |= bit;
                            ot->nagain++; ot->abytes += 4 + (int64_t)o->len;
                        }
                        break;
                    }
                }
                s = (s + 1) & mask;
            }
            if (w) {
                k->cand += 4 + (int64_t)f->len;
                if (SG_KIND(w) == RAY_SYMGRP_OLD) g->st.old++; else g->st.refs++;
            } else {
                nw[nn] = (sg_own_t){f->h, f->len, (uint32_t)t, f->local, cnt};
                slots[s] = (uint32_t)(ne + nn + 1);
                w = SG_WORD(RAY_SYMGRP_NEW, rows ? 0 : *next_id + nn);
                nn++;
                k->owned++; k->bytes += 4 + (int64_t)f->len;
                if (k->own) k->own[f->local >> 6] |= UINT64_C(1) << (f->local & 63);
            }
            k->r[i] = w;
        }
    }
    /* SHARDS, FREQ: the new strings in the order met, for their positions */
    for (int64_t i = 0; ok && !rows && i < nn; i++) {
        sg_own_t* o = (sg_own_t*)sg_push(owners, sizeof(sg_own_t));
        if (!o) ok = false; else *o = nw[i];
    }
    *next_id += nn;
    g->st.log_loaded += ne;
    return ok;
}

bool ray_symgrp_resolve(ray_symgrp_t* g) {
    int ng = 1 << g->lg, per = RAY_SYMGRP_MAX / ng;
    sg_list_t owners = {0};
    int64_t nown = 0, bytes = 0;
    bool ok = true;
    for (int64_t t = 0; t < g->ntasks; t++) g->st.staged += g->tasks[t].n;
    /* one table for every group, sized for the largest: allocated once,
     * so a group's table reuses the pages the last one warmed */
    uint64_t cap = 16; int64_t ne_max = 1, ns_max = 1;
    for (int gi = 0; gi < ng; gi++) {
        int64_t ne = 0, ns = 0;
        for (int l = gi * per; l < (gi + 1) * per; l++) ne += g->logs[l].n;
        for (int64_t t = 0; t < g->ntasks; t++) ns += g->tasks[t].seg[gi + 1] - g->tasks[t].seg[gi];
        if (sg_group_cap(ne, ns) > cap) cap = sg_group_cap(ne, ns);
        if (ne > ne_max) ne_max = ne;
        if (ns > ns_max) ns_max = ns;
    }
    g->gslots = (uint32_t*)ray_sys_alloc((size_t)cap * sizeof(uint32_t));
    g->gent = (sg_ent_t*)ray_sys_alloc((size_t)ne_max * sizeof(sg_ent_t));
    g->gnew = (sg_own_t*)ray_sys_alloc((size_t)ns_max * sizeof(sg_own_t));
    ok = g->gslots && g->gent && g->gnew;
    for (int gi = 0; ok && gi < ng; gi++) ok = sg_resolve_group(g, gi, &owners, &nown);
    ray_sys_free(g->gslots); ray_sys_free(g->gent); ray_sys_free(g->gnew);
    g->gslots = NULL; g->gent = NULL; g->gnew = NULL;
    g->st.groups += ng;
    for (int64_t t = 0; t < g->ntasks; t++) bytes += g->tasks[t].bytes;
    g->nown = nown; g->st.owners += nown;
    if (ok && nown) ok = ray_symimp_reserve(g->imp, nown, bytes, &g->pos0, &g->off0);
    if (ok && sg_rows(g)) {
        /* positions by task, then by local id: the strings later tasks meet
         * again (ROWS) first, task by task, then the others; in each a
         * task's base, then the strings of its kind before it in the task's
         * bitmaps (no table by string) */
        g->offb = (int64_t*)ray_sys_alloc((size_t)(g->ntasks ? g->ntasks : 1) * 2 * sizeof(int64_t));
        ok = g->offb != NULL;
        int64_t nagain = 0, abytes = 0;
        for (int64_t t = 0; t < g->ntasks; t++) { nagain += g->tasks[t].nagain; abytes += g->tasks[t].abytes; }
        g->st.again += nagain; g->st.again_bytes += abytes;
        int64_t abase = 0, aoff = g->off0, base = nagain, off = g->off0 + abytes;
        for (int64_t t = 0; ok && t < g->ntasks; t++) {
            sg_task_t* k = &g->tasks[t];
            g->offb[2 * t] = off; off += k->bytes - k->abytes;
            g->offb[2 * t + 1] = aoff; aoff += k->abytes;
            int64_t words = (k->n + 1 + 63) / 64;
            uint32_t r = (uint32_t)base, ra = (uint32_t)abase;
            for (int64_t w = 0; w < words; w++) {
                k->rank[w] = r; k->arank[w] = ra;
                r += (uint32_t)__builtin_popcountll(k->own[w] & ~k->again[w]);
                ra += (uint32_t)__builtin_popcountll(k->again[w]);
            }
            base += k->owned - k->nagain; abase += k->nagain;
        }
        /* the new strings join the index, task by task */
        for (int64_t t = 0; ok && t < g->ntasks; t++) {
            const sg_task_t* k = &g->tasks[t];
            for (int64_t i = 0; ok && i < k->n; i++) {
                if (SG_KIND(k->r[i]) != RAY_SYMGRP_NEW) continue;
                const ray_symgrp_fp_t* f = &k->fp[i];
                sg_ent_t* x = (sg_ent_t*)sg_push(&g->logs[sg_log(f->h)], sizeof(sg_ent_t));
                if (!x) ok = false;
                else { *x = (sg_ent_t){f->h, f->len, sg_rank_pos(g, t, f->local)}; g->entries++; }
            }
        }
        return ok;
    }
    if (ok) {
        g->pos_of = (uint32_t*)ray_sys_alloc((size_t)(nown ? nown : 1) * sizeof(uint32_t));
        g->t_of = (uint32_t*)ray_sys_alloc((size_t)(nown ? nown : 1) * sizeof(uint32_t));
        ok = g->pos_of && g->t_of;
    }
    if (ok) {
        g->off_of = (int64_t*)ray_sys_alloc((size_t)(nown ? nown : 1) * sizeof(int64_t));
        ok = g->off_of != NULL;
        uint64_t* ord = NULL; uint64_t* tmp = NULL; uint32_t* len_of = NULL;
        if (ok && g->order == RAY_SYMGRP_FREQ) {
            /* most rows first, then first occurrence: three stable passes,
             * least significant key first */
            ord = (uint64_t*)ray_sys_alloc((size_t)(nown ? nown : 1) * sizeof(uint64_t));
            tmp = (uint64_t*)ray_sys_alloc((size_t)(nown ? nown : 1) * sizeof(uint64_t));
            len_of = (uint32_t*)ray_sys_alloc((size_t)(nown ? nown : 1) * sizeof(uint32_t));
            uint32_t* cnt_of = (uint32_t*)ray_sys_alloc((size_t)(nown ? nown : 1) * sizeof(uint32_t));
            uint32_t* loc_of = (uint32_t*)ray_sys_alloc((size_t)(nown ? nown : 1) * sizeof(uint32_t));
            ok = ord && tmp && len_of && cnt_of && loc_of;
            int64_t id = 0;
            for (sg_chunk_t* c = owners.head; ok && c; c = c->next)
                for (int64_t i = 0; i < c->n; i++, id++) {
                    const sg_own_t* o = (const sg_own_t*)c->data + i;
                    len_of[id] = o->len; cnt_of[id] = o->count; loc_of[id] = o->local; g->t_of[id] = o->t;
                }
            for (int64_t i = 0; ok && i < nown; i++) ord[i] = ((uint64_t)loc_of[i] << 32) | (uint64_t)i;
            ok = ok && sg_sort_hi(ord, tmp, nown);
            for (int64_t i = 0; ok && i < nown; i++) { uint32_t x = (uint32_t)ord[i]; ord[i] = ((uint64_t)g->t_of[x] << 32) | x; }
            ok = ok && sg_sort_hi(ord, tmp, nown);
            for (int64_t i = 0; ok && i < nown; i++) { uint32_t x = (uint32_t)ord[i]; ord[i] = ((uint64_t)(UINT32_MAX - cnt_of[x]) << 32) | x; }
            ok = ok && sg_sort_hi(ord, tmp, nown);
            int64_t off = g->off0;
            for (int64_t i = 0; ok && i < nown; i++) {
                uint32_t x = (uint32_t)ord[i];
                g->pos_of[x] = (uint32_t)(g->pos0 + i);
                g->off_of[x] = off; off += 4 + (int64_t)len_of[x];
            }
            ray_sys_free(cnt_of); ray_sys_free(loc_of);
        } else if (ok) {   /* SHARDS: the order step 2 met them in */
            int64_t id = 0, off = g->off0;
            for (sg_chunk_t* c = owners.head; c; c = c->next)
                for (int64_t i = 0; i < c->n; i++, id++) {
                    const sg_own_t* o = (const sg_own_t*)c->data + i;
                    g->pos_of[id] = (uint32_t)(g->pos0 + id);
                    g->t_of[id] = o->t;
                    g->off_of[id] = off; off += 4 + (int64_t)o->len;
                }
        }
        ray_sys_free(ord); ray_sys_free(tmp); ray_sys_free(len_of);
    }
    /* the new strings join the index */
    int64_t id = 0;
    for (sg_chunk_t* c = owners.head; ok && c; c = c->next)
        for (int64_t i = 0; ok && i < c->n; i++, id++) {
            const sg_own_t* o = (const sg_own_t*)c->data + i;
            sg_ent_t* x = (sg_ent_t*)sg_push(&g->logs[sg_log(o->h)], sizeof(sg_ent_t));
            if (!x) ok = false;
            else { *x = (sg_ent_t){o->h, o->len, g->pos_of[id]}; g->entries++; }
        }
    sg_list_free(&owners);
    return ok;
}

int64_t ray_symgrp_task_size(const ray_symgrp_t* g, int64_t t) {
    return t >= 0 && t < g->ntasks ? g->tasks[t].n : 0;
}

bool ray_symgrp_task(const ray_symgrp_t* g, int64_t t, ray_symgrp_res_t* res) {
    if (t < 0 || t >= g->ntasks) return false;
    const sg_task_t* k = &g->tasks[t];
    for (int64_t i = 0; i < k->n; i++) {
        uint64_t w = k->r[i];
        uint32_t local = k->fp[i].local;
        uint64_t v = SG_VAL(w);
        switch (SG_KIND(w)) {
        case RAY_SYMGRP_NEW:
            res[local] = sg_rows(g)
                ? (ray_symgrp_res_t){sg_rank_pos(g, t, local), RAY_SYMGRP_NEW, 0,
                                     (uint32_t)(k->again[local >> 6] >> (local & 63) & 1), -1}
                : (ray_symgrp_res_t){g->pos_of[v], RAY_SYMGRP_NEW, 0, 0, g->off_of[v]};
            break;
        case RAY_SYMGRP_OLD:
            res[local] = (ray_symgrp_res_t){(uint32_t)v, RAY_SYMGRP_OLD, 0, 0, 0};
            break;
        case RAY_SYMGRP_REF:
            res[local] = (ray_symgrp_res_t){sg_ref_pos(g, v), RAY_SYMGRP_REF, sg_ref_task(g, v), 0, 0};
            break;
        default:
            return false;
        }
    }
    return true;
}

int64_t ray_symgrp_task_off(const ray_symgrp_t* g, int64_t t, bool again) {
    return g->offb && t >= 0 && t < g->ntasks ? g->offb[2 * t + (again ? 1 : 0)] : -1;
}

int64_t ray_symgrp_window(const ray_symgrp_t* g, int64_t ta, int64_t budget) {
    int64_t tb = ta, sum = 0;
    while (tb < g->ntasks && (tb == ta || sum + g->tasks[tb].cand <= budget)) sum += g->tasks[tb++].cand;
    return tb;
}

/* Records read in position order go through spans of neighbouring records
 * (gaps up to SG_SPAN_GAP read through rather than sought over), cut into
 * pieces read ahead at most SG_AHEAD bytes before the record being copied:
 * a window's spans asked for at once are evicted again, under memory
 * pressure, before they are copied, and then fault back in a page at a
 * time. */
#define SG_SPAN_GAP ((int64_t)256 << 10)
#define SG_PIECE    ((int64_t)4 << 20)
#define SG_AHEAD    ((int64_t)32 << 20)
typedef struct { int64_t lo, hi; } sg_span_t;
typedef struct {
    sg_span_t* p;
    int64_t n, cap;
    int64_t next;       /* the first piece not asked for */
    int64_t done;       /* the first piece not behind the reader */
    int64_t ahead;      /* bytes asked for, not behind */
    int64_t lo, hi;     /* the span being built (hi 0: none) */
} sg_ra_t;

static bool sg_ra_cut(sg_ra_t* ra) {
    for (int64_t a = ra->lo; a < ra->hi; a += SG_PIECE) {
        if (ra->n == ra->cap) {
            int64_t nc = ra->cap ? ra->cap * 2 : 256;
            sg_span_t* np = (sg_span_t*)ray_sys_realloc(ra->p, (size_t)nc * sizeof(sg_span_t));
            if (!np) return false;
            ra->p = np; ra->cap = nc;
        }
        ra->p[ra->n++] = (sg_span_t){a, ra->hi - a > SG_PIECE ? a + SG_PIECE : ra->hi};
    }
    return true;
}
/* A record [off, end), records in ascending order: joined to the span or
 * starting the next.  False on allocation failure. */
static bool sg_ra_add(sg_ra_t* ra, int64_t off, int64_t end, int64_t* spans) {
    if (ra->hi > 0) {
        if (off >= ra->lo && off - ra->hi <= SG_SPAN_GAP) { if (end > ra->hi) ra->hi = end; return true; }
        if (!sg_ra_cut(ra)) return false;
    }
    ra->lo = off; ra->hi = end; (*spans)++;
    return true;
}
static bool sg_ra_end(sg_ra_t* ra) {
    bool ok = ra->hi <= 0 || sg_ra_cut(ra);
    ra->lo = ra->hi = 0;
    return ok;
}
/* Before the record at `off` is read: the pieces behind it dropped from
 * the count, those ahead asked for up to SG_AHEAD; `bytes` counts them. */
static void sg_ra_at(const ray_symimp_t* imp, sg_ra_t* ra, int64_t off, int64_t* bytes) {
    for (;;) {
        while (ra->done < ra->next && ra->p[ra->done].hi <= off) {
            ra->ahead -= ra->p[ra->done].hi - ra->p[ra->done].lo;
            ra->done++;
        }
        if (ra->done == ra->next)
            while (ra->next < ra->n && ra->p[ra->next].hi <= off) { ra->next++; ra->done++; }
        if (ra->next >= ra->n || (ra->next > ra->done && ra->ahead >= SG_AHEAD)) return;
        int64_t len = ra->p[ra->next].hi - ra->p[ra->next].lo;
        ray_symimp_willneed(imp, ra->p[ra->next].lo, len);
        ra->ahead += len; *bytes += len;
        ra->next++;
    }
}

/* The records the window's candidates point at from before it: positions
 * (with the candidates' lengths, which their records share) sorted; those
 * the previous window loaded too are copied from it (a string common in the
 * column is read from the file once), the rest are read in position order
 * (sg_ra_t) and copied from the file. */
bool ray_symgrp_load(ray_symgrp_t* g, int64_t ta, int64_t tb) {
    uint32_t* opos = g->spos; uint32_t* olen = g->slen; int64_t* ooff = g->soff;
    char* obuf = g->sbuf; int64_t on = g->sn;
    g->spos = NULL; g->slen = NULL; g->soff = NULL; g->sbuf = NULL; g->sn = 0;
    sg_window_free(g);
    g->st.windows++;
    int64_t n = 0;
    for (int64_t t = ta; t < tb; t++) n += g->tasks[t].n;
    uint64_t* a = (uint64_t*)ray_sys_alloc((size_t)(n ? n : 1) * sizeof(uint64_t));
    uint64_t* tmp = (uint64_t*)ray_sys_alloc((size_t)(n ? n : 1) * sizeof(uint64_t));
    if (!a || !tmp) { ray_sys_free(a); ray_sys_free(tmp); return false; }
    int64_t m = 0;
    for (int64_t t = ta; t < tb; t++) {
        const sg_task_t* k = &g->tasks[t];
        for (int64_t i = 0; i < k->n; i++) {
            uint64_t w = k->r[i];
            uint32_t pos;
            if (SG_KIND(w) == RAY_SYMGRP_OLD) pos = (uint32_t)SG_VAL(w);
            else if (SG_KIND(w) == RAY_SYMGRP_REF && sg_ref_task(g, SG_VAL(w)) < ta) pos = sg_ref_pos(g, SG_VAL(w));
            else continue;
            a[m++] = ((uint64_t)pos << 32) | k->fp[i].len;
        }
    }
    bool ok = sg_sort_hi(a, tmp, m);
    int64_t u = 0, bytes = 0;
    for (int64_t i = 0; ok && i < m; i++)
        if (!u || (a[i] >> 32) != (a[u - 1] >> 32)) { a[u++] = a[i]; bytes += (uint32_t)a[i]; }
    g->spos = (uint32_t*)ray_sys_alloc((size_t)(u ? u : 1) * sizeof(uint32_t));
    g->slen = (uint32_t*)ray_sys_alloc((size_t)(u ? u : 1) * sizeof(uint32_t));
    g->soff = (int64_t*)ray_sys_alloc((size_t)(u + 1) * sizeof(int64_t));
    g->sbuf = (char*)ray_sys_alloc((size_t)(bytes ? bytes : 1));
    ok = ok && g->spos && g->slen && g->soff && g->sbuf;
    /* what the previous window holds (both lists ascending), marked by a
     * set top bit of the length word; the rest read span by span */
    sg_ra_t ra = {0};
    int64_t j = 0;
    for (int64_t i = 0; ok && i < u; i++) {
        uint32_t pos = (uint32_t)(a[i] >> 32);
        while (j < on && opos[j] < pos) j++;
        if (j < on && opos[j] == pos) { a[i] |= UINT64_C(1) << 31; continue; }
        int64_t off = ray_symimp_offset(g->imp, pos);
        ok = sg_ra_add(&ra, off, off + 4 + (uint32_t)a[i], &g->st.store_spans);
    }
    ok = ok && sg_ra_end(&ra);
    int64_t at = 0;
    j = 0;
    for (int64_t i = 0; ok && i < u; i++) {
        uint32_t pos = (uint32_t)(a[i] >> 32), room = (uint32_t)a[i] & 0x7fffffffu, len;
        const char* s;
        if ((uint32_t)a[i] >> 31) {
            while (opos[j] < pos) j++;
            len = olen[j]; s = obuf + ooff[j];
            g->st.store_kept++;
        } else {
            sg_ra_at(g->imp, &ra, ray_symimp_offset(g->imp, pos), &g->st.store_read);
            s = ray_symimp_get(g->imp, pos, &len);
            g->st.store_pos++; g->st.store_bytes += room;
        }
        g->spos[i] = pos;
        g->slen[i] = len;
        g->soff[i] = at;
        /* the candidates' length: a record of another length is no match */
        if (len == room) memcpy(g->sbuf + at, s, len);
        at += room;
    }
    if (ok) { g->soff[u] = at; g->sn = u; }
    ray_sys_free(ra.p);
    ray_sys_free(a); ray_sys_free(tmp);
    ray_sys_free(opos); ray_sys_free(olen); ray_sys_free(ooff); ray_sys_free(obuf);
    return ok;
}

bool ray_symgrp_same(ray_symgrp_t* g, uint32_t pos, const char* s, uint32_t len) {
    int64_t lo = 0, hi = g->sn;
    while (lo < hi) { int64_t mid = (lo + hi) / 2; if (g->spos[mid] < pos) lo = mid + 1; else hi = mid; }
    if (lo < g->sn && g->spos[lo] == pos)
        return g->slen[lo] == len && g->soff[lo + 1] - g->soff[lo] == (int64_t)len &&
               (len == 0 || memcmp(g->sbuf + g->soff[lo], s, len) == 0);
    uint32_t rl;
    const char* r = ray_symimp_get(g->imp, pos, &rl);
    return rl == len && (len == 0 || memcmp(r, s, len) == 0);
}

bool ray_symgrp_defer(ray_symgrp_t* g, uint32_t worker, int64_t t, uint32_t local, uint64_t h,
                      uint32_t pos, const char* s, uint32_t len) {
    if ((int64_t)worker >= g->workers) return false;
    const char* kept = sg_keep(&g->db[worker], s, len);
    sg_def_t* d = (sg_def_t*)sg_push(&g->dl[worker], sizeof(sg_def_t));
    if (!kept || !d) return false;
    *d = (sg_def_t){t, h, kept, local, pos, len};
    return true;
}

int64_t ray_symgrp_collide(ray_symgrp_t* g, uint64_t h, const char* s, uint32_t len) {
    pthread_mutex_lock(&g->lock);
    int64_t pos = -1;
    for (sg_chunk_t* c = g->logs[sg_log(h)].head; pos < 0 && c; c = c->next)
        for (int64_t i = 0; i < c->n; i++) {
            const sg_ent_t* x = (const sg_ent_t*)c->data + i;
            if (x->h != h || x->len != len) continue;
            uint32_t rl;
            const char* r = ray_symimp_get(g->imp, x->pos, &rl);
            if (rl == len && (len == 0 || memcmp(r, s, len) == 0)) { pos = x->pos; break; }
        }
    if (pos < 0) {
        int64_t p0, o0;
        sg_ent_t* x = NULL;
        if (ray_symimp_reserve(g->imp, 1, 4 + (int64_t)len, &p0, &o0) &&
            (x = (sg_ent_t*)sg_push(&g->logs[sg_log(h)], sizeof(sg_ent_t)))) {
            ray_symimp_put(g->imp, p0, o0, s, len);
            *x = (sg_ent_t){h, len, (uint32_t)p0};
            g->entries++;
            pos = p0;
        }
    }
    g->st.collisions++;
    pthread_mutex_unlock(&g->lock);
    return pos;
}

static int sg_ovr_cmp(const sg_ovr_t* a, int64_t t, uint32_t local) {
    return a->t != t ? (a->t < t ? -1 : 1) : a->local != local ? (a->local < local ? -1 : 1) : 0;
}

bool ray_symgrp_settle(ray_symgrp_t* g, int64_t** redo, int64_t* nredo) {
    *redo = NULL; *nredo = 0;
    ray_sys_free(g->ovr); g->ovr = NULL; g->novr = 0;
    int64_t n = 0;
    for (int64_t w = 0; w < g->workers; w++) n += g->dl[w].n;
    g->st.deferred += n;
    if (!n) return true;
    /* every deferred candidate, by position */
    sg_def_t** all = (sg_def_t**)ray_sys_alloc((size_t)n * sizeof(sg_def_t*));
    uint64_t* a = (uint64_t*)ray_sys_alloc((size_t)n * sizeof(uint64_t));
    uint64_t* tmp = (uint64_t*)ray_sys_alloc((size_t)n * sizeof(uint64_t));
    bool ok = all && a && tmp;
    int64_t k = 0;
    for (int64_t w = 0; ok && w < g->workers; w++)
        for (sg_chunk_t* c = g->dl[w].head; c; c = c->next)
            for (int64_t i = 0; i < c->n; i++, k++) {
                all[k] = (sg_def_t*)c->data + i;
                a[k] = ((uint64_t)all[k]->pos << 32) | (uint64_t)k;
            }
    ok = ok && sg_sort_hi(a, tmp, n);
    sg_ra_t ra = {0};
    int64_t spans = 0;
    for (int64_t i = 0; ok && i < n; i++) {
        const sg_def_t* d = all[(uint32_t)a[i]];
        int64_t off = ray_symimp_offset(g->imp, d->pos);
        ok = sg_ra_add(&ra, off, off + 4 + d->len, &spans);
    }
    ok = ok && sg_ra_end(&ra);
    sg_ovr_t* ovr = NULL; int64_t novr = 0, covr = 0;
    int64_t cmp = 0, cmpb = 0;
    for (int64_t i = 0; ok && i < n; i++) {
        const sg_def_t* d = all[(uint32_t)a[i]];
        uint32_t rl;
        sg_ra_at(g->imp, &ra, ray_symimp_offset(g->imp, d->pos), &g->st.settle_read);
        const char* r = ray_symimp_get(g->imp, d->pos, &rl);
        cmp++; cmpb += d->len;
        if (rl == d->len && (d->len == 0 || memcmp(r, d->s, d->len) == 0)) continue;
        int64_t p = ray_symgrp_collide(g, d->h, d->s, d->len);
        if (p < 0) { ok = false; break; }
        if (novr == covr) {
            covr = covr ? covr * 2 : 16;
            sg_ovr_t* no = (sg_ovr_t*)ray_sys_realloc(ovr, (size_t)covr * sizeof(sg_ovr_t));
            if (!no) { ok = false; break; }
            ovr = no;
        }
        ovr[novr++] = (sg_ovr_t){d->t, d->local, (uint32_t)p};
    }
    atomic_fetch_add_explicit(&g->compares, cmp, memory_order_relaxed);
    atomic_fetch_add_explicit(&g->cmp_bytes, cmpb, memory_order_relaxed);
    ray_sys_free(ra.p);
    ray_sys_free(all); ray_sys_free(a); ray_sys_free(tmp);
    for (int64_t w = 0; w < g->workers; w++) { sg_list_free(&g->dl[w]); sg_list_free(&g->db[w]); }
    if (!ok) { ray_sys_free(ovr); return false; }
    /* by (task, local id), insertion sort: mismatches are true collisions */
    for (int64_t i = 1; i < novr; i++) {
        sg_ovr_t x = ovr[i]; int64_t j = i;
        while (j > 0 && sg_ovr_cmp(&ovr[j - 1], x.t, x.local) > 0) { ovr[j] = ovr[j - 1]; j--; }
        ovr[j] = x;
    }
    g->ovr = ovr; g->novr = novr;
    if (novr) {
        int64_t* rd = (int64_t*)ray_sys_alloc((size_t)novr * sizeof(int64_t));
        if (!rd) return false;
        int64_t nr = 0;
        for (int64_t i = 0; i < novr; i++) if (!nr || rd[nr - 1] != ovr[i].t) rd[nr++] = ovr[i].t;
        *redo = rd; *nredo = nr;
        g->st.redo += nr;
    }
    return true;
}

int64_t ray_symgrp_override(const ray_symgrp_t* g, int64_t t, uint32_t local) {
    int64_t lo = 0, hi = g->novr;
    while (lo < hi) { int64_t mid = (lo + hi) / 2; if (sg_ovr_cmp(&g->ovr[mid], t, local) < 0) lo = mid + 1; else hi = mid; }
    return lo < g->novr && sg_ovr_cmp(&g->ovr[lo], t, local) == 0 ? (int64_t)g->ovr[lo].pos : -1;
}

void ray_symgrp_note(ray_symgrp_t* g, int64_t compares, int64_t bytes) {
    atomic_fetch_add_explicit(&g->compares, compares, memory_order_relaxed);
    atomic_fetch_add_explicit(&g->cmp_bytes, bytes, memory_order_relaxed);
}

void ray_symgrp_stats(ray_symgrp_t* g, ray_symgrp_stats_t* out) {
    *out = g->st;
    out->compares = atomic_exchange_explicit(&g->compares, 0, memory_order_relaxed);
    out->cmp_bytes = atomic_exchange_explicit(&g->cmp_bytes, 0, memory_order_relaxed);
    memset(&g->st, 0, sizeof(g->st));
}

int64_t ray_symgrp_entries(const ray_symgrp_t* g) { return g->entries; }
ray_symgrp_order_t ray_symgrp_order(const ray_symgrp_t* g) { return g->order; }

#else  /* POSIX only, as the import dictionary */

ray_symgrp_t* ray_symgrp_new(struct ray_symimp_s* imp, ray_symgrp_order_t order) { (void)imp; (void)order; return NULL; }
void ray_symgrp_free(ray_symgrp_t* g) { (void)g; }
bool ray_symgrp_begin(ray_symgrp_t* g, int64_t ntasks, int groups, int64_t workers) { (void)g; (void)ntasks; (void)groups; (void)workers; return false; }
bool ray_symgrp_stage(ray_symgrp_t* g, int64_t t, int64_t n, const ray_symgrp_fp_t* fp, const uint32_t* counts) { (void)g; (void)t; (void)n; (void)fp; (void)counts; return false; }
bool ray_symgrp_resolve(ray_symgrp_t* g) { (void)g; return false; }
int64_t ray_symgrp_task_size(const ray_symgrp_t* g, int64_t t) { (void)g; (void)t; return 0; }
bool ray_symgrp_task(const ray_symgrp_t* g, int64_t t, ray_symgrp_res_t* res) { (void)g; (void)t; (void)res; return false; }
int64_t ray_symgrp_task_off(const ray_symgrp_t* g, int64_t t, bool again) { (void)g; (void)t; (void)again; return -1; }
int64_t ray_symgrp_window(const ray_symgrp_t* g, int64_t ta, int64_t budget) { (void)g; (void)budget; return ta + 1; }
bool ray_symgrp_load(ray_symgrp_t* g, int64_t ta, int64_t tb) { (void)g; (void)ta; (void)tb; return false; }
bool ray_symgrp_same(ray_symgrp_t* g, uint32_t pos, const char* s, uint32_t len) { (void)g; (void)pos; (void)s; (void)len; return false; }
bool ray_symgrp_defer(ray_symgrp_t* g, uint32_t worker, int64_t t, uint32_t local, uint64_t h, uint32_t pos, const char* s, uint32_t len) {
    (void)g; (void)worker; (void)t; (void)local; (void)h; (void)pos; (void)s; (void)len; return false;
}
bool ray_symgrp_settle(ray_symgrp_t* g, int64_t** redo, int64_t* nredo) { (void)g; *redo = NULL; *nredo = 0; return false; }
int64_t ray_symgrp_override(const ray_symgrp_t* g, int64_t t, uint32_t local) { (void)g; (void)t; (void)local; return -1; }
int64_t ray_symgrp_collide(ray_symgrp_t* g, uint64_t h, const char* s, uint32_t len) { (void)g; (void)h; (void)s; (void)len; return -1; }
void ray_symgrp_end(ray_symgrp_t* g) { (void)g; }
void ray_symgrp_note(ray_symgrp_t* g, int64_t compares, int64_t bytes) { (void)g; (void)compares; (void)bytes; }
void ray_symgrp_stats(ray_symgrp_t* g, ray_symgrp_stats_t* out) { (void)g; *out = (ray_symgrp_stats_t){0}; }
int64_t ray_symgrp_entries(const ray_symgrp_t* g) { (void)g; return 0; }
ray_symgrp_order_t ray_symgrp_order(const ray_symgrp_t* g) { (void)g; return RAY_SYMGRP_ROWS; }

#endif
