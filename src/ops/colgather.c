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

#include "ops/colgather.h"
#include "ops/internal.h"     /* parted_sym_width */
#include "ops/rowsel.h"
#include "core/pool.h"         /* the parallel selection gather */
#include "table/domain.h"    /* ray_sym_domain_retain */
#include "table/sym.h"
#include "vec/str.h"
#include "vec/vec.h"           /* ray_str_pool_reserve, ray_raw_is_null */
#include <assert.h>
#include <stdatomic.h>
#include <string.h>

static _Atomic(uint64_t) g_par_gathers;   /* test hook: parallel selection gathers */
uint64_t ray_col_test_gather_par(void) { return atomic_load_explicit(&g_par_gathers, memory_order_relaxed); }

/* The result under construction: the output vector (len set, cells filled
 * by position) and, for STR, the pool built for it (a RAY_U8 block grown
 * as ray_str_vec_append grows one, attached at the finish).  A parallel
 * gather gives each task its own accumulator over the one shared output:
 * the task writes only its own output positions and its own pool, and
 * notes a null in `nulls` instead of the output's attrs, which only the
 * finish sets (after the join). */
typedef struct {
    ray_t*  out;
    ray_t*  pool;
    uint8_t ow;          /* SYM: the output's width class */
    bool    nulls;       /* a null was written: HAS_NULLS at the finish */
} gather_acc_t;

static ray_t* acc_open(gather_acc_t* a, const ray_col_src_t* src, int64_t n) {
    memset(a, 0, sizeof *a);
    if (src->type == RAY_LIST) return ray_error("nyi", "gather: LIST source");
    if (src->type == RAY_SYM)
        /* a parted source's own width is not authoritative (segments may
         * differ): the widest segment's, as agg_src_standin sizes it */
        a->ow = src->parted ? parted_sym_width(src->obj) : (uint8_t)(src->attrs & RAY_SYM_W_MASK);
    ray_t* v = src->type == RAY_SYM ? ray_sym_vec_new(a->ow, n > 0 ? n : 1) : ray_vec_new(src->type, n > 0 ? n : 1);
    if (!v || RAY_IS_ERR(v)) return v ? v : ray_error("oom", NULL);
    v->len = n;
    a->out = v;
    return NULL;
}

/* The partial result, its pool and nothing else: the domain is retained
 * only at the finish. */
static void acc_abort(gather_acc_t* a) {
    if (a->out) ray_release(a->out);
    if (a->pool) ray_release(a->pool);
    a->out = a->pool = NULL;
}

/* Copy len bytes into the result's pool: their offset in *off.  The pool
 * is created and grown as ray_str_vec_append's (ray_str_pool_reserve);
 * `range` when it would cross UINT32_MAX, as for any STR pool. */
static ray_t* acc_pool_put(gather_acc_t* a, const char* p, uint32_t len, uint32_t* off) {
    int64_t at;
    ray_err_t pe = ray_str_pool_reserve(&a->pool, len, &at);
    if (pe == RAY_ERR_RANGE)
        return ray_error("range", "gather: STR pool exceeds %lld bytes", (long long)UINT32_MAX);
    if (pe != RAY_OK) return ray_error("oom", NULL);
    memcpy((char*)ray_data(a->pool) + at, p, len);
    a->pool->len = at + (int64_t)len;
    *off = (uint32_t)at;
    return NULL;
}

/* The type's null sentinel at output position pos, as ray_vec_set_null
 * writes it for the fixed-width types a view can carry nulls in; the
 * HAS_NULLS bit waits for the finish (a.nulls). */
static void acc_null(gather_acc_t* a, int64_t pos) {
    void* p = ray_data(a->out);
    switch (a->out->type) {
    case RAY_F64:                               ((double*)p)[pos]  = NULL_F64; break;
    case RAY_F32:                               ((float*)p)[pos]   = NULL_F32; break;
    case RAY_I64: case RAY_TIMESTAMP:           ((int64_t*)p)[pos] = NULL_I64; break;
    case RAY_I32: case RAY_DATE: case RAY_TIME: ((int32_t*)p)[pos] = NULL_I32; break;
    case RAY_I16:                               ((int16_t*)p)[pos] = NULL_I16; break;
    case RAY_GUID:                              memset((uint8_t*)p + pos * 16, 0, 16); break;
    default: return;                            /* not nullable (BOOL, U8): as ray_vec_set_null */
    }
    a->nulls = true;
}

/* Elements [i, i + cnt) of view `v` to output positions [pos, pos + cnt),
 * as generation 0's gather leaves them: STR descriptors rebuilt over the
 * result's pool (as ray_str_vec_append builds them), SYM ids written as
 * logical values at the output width, nulls under the view's may_null as
 * ray_vec_set_null writes them (SYM and STR nulls are in-band and always
 * tested, as ray_vec_may_have_nulls has it). */
static ray_t* acc_put(gather_acc_t* a, const ray_col_range_t* v, int64_t i, int64_t pos, int64_t cnt) {
    ray_t* o = a->out;
    if (v->type == RAY_STR) {
        const ray_str_t* sv = (const ray_str_t*)v->data;
        ray_str_t* d = (ray_str_t*)ray_data(o);
        for (int64_t k = 0; k < cnt; k++) {
            const ray_str_t* s = &sv[i + k];
            ray_str_t e;
            memset(&e, 0, sizeof e);
            e.len = s->len;
            if (s->len == 0) {
                a->nulls = true;
            } else if (ray_str_is_inline(s)) {
                memcpy(e.data, s->data, s->len);
            } else {
                /* the view's pool lives until the next read: copy now */
                const char* p = v->str_pool + s->pool_off;
                uint32_t off;
                ray_t* err = acc_pool_put(a, p, s->len, &off);
                if (err) return err;
                memcpy(e.prefix, p, 4);
                e.pool_off = off;
                /* a nonzero cached hash is the content hash (every builder
                 * and loader keeps it so, col.c / enccol.c): reused, else
                 * computed as ray_str_vec_append computes it */
                e.hash32 = s->hash32;
                e.hash32 = ray_str_t_hash32(&e, (const char*)ray_data(a->pool));
            }
            d[pos + k] = e;
        }
        return NULL;
    }
    if (v->type == RAY_SYM) {
        uint8_t vw = v->attrs & RAY_SYM_W_MASK;
        void* d = ray_data(o);
        if (vw == a->ow) {
            uint8_t esz = ray_sym_elem_size(RAY_SYM, v->attrs);
            memcpy((char*)d + (size_t)pos * esz, (const char*)v->data + (size_t)i * esz, (size_t)cnt * esz);
        }
        uint64_t lim = a->ow == RAY_SYM_W8 ? UINT8_MAX : a->ow == RAY_SYM_W16 ? UINT16_MAX
                     : a->ow == RAY_SYM_W32 ? UINT32_MAX : UINT64_MAX;
        for (int64_t k = 0; k < cnt; k++) {
            uint64_t id = (uint64_t)ray_read_sym(v->data, i + k, RAY_SYM, v->attrs);
            if (id == 0) a->nulls = true;
            if (vw == a->ow) continue;
            if (id > lim)    /* the width scan said every segment fits */
                return ray_error("corrupt", "gather: symbol id %llu above its width class", (unsigned long long)id);
            ray_write_sym(d, pos + k, id, RAY_SYM, o->attrs);
        }
        return NULL;
    }
    uint8_t esz = ray_sym_elem_size(v->type, v->attrs);
    memcpy((char*)ray_data(o) + (size_t)pos * esz, (const char*)v->data + (size_t)i * esz, (size_t)cnt * esz);
    if (v->may_null)
        for (int64_t k = 0; k < cnt; k++)
            if (ray_raw_is_null(v->type, v->attrs, v->data, i + k)) acc_null(a, pos + k);
    return NULL;
}

/* A partial vector: view elements base + li[q] (li16 or li64, whichever is
 * not NULL), q in [0, cnt), to output positions [pos, pos + cnt).  A
 * fixed-width view (SYM at the output's width included) is copied by one
 * width-specialised loop, then its nulls are rewritten as acc_put rewrites
 * them; STR and widened SYM go element by element through acc_put. */
#define ACC_LIST_COPY(IX) do {                                                                       \
        switch (esz) {                                                                               \
        case 1: for (int64_t q = 0; q < cnt; q++) d[q] = sd[(IX)]; break;                            \
        case 2: for (int64_t q = 0; q < cnt; q++) memcpy(d + q * 2, sd + (IX) * 2, 2); break;        \
        case 4: for (int64_t q = 0; q < cnt; q++) memcpy(d + q * 4, sd + (IX) * 4, 4); break;        \
        case 8: for (int64_t q = 0; q < cnt; q++) memcpy(d + q * 8, sd + (IX) * 8, 8); break;        \
        default: for (int64_t q = 0; q < cnt; q++) memcpy(d + q * esz, sd + (IX) * esz, esz); break; \
        }                                                                                            \
    } while (0)
static ray_t* acc_put_list(gather_acc_t* a, const ray_col_range_t* v, int64_t base, const uint16_t* li16,
                           const int64_t* li64, int64_t cnt, int64_t pos) {
    if (v->type == RAY_STR || (v->type == RAY_SYM && (v->attrs & RAY_SYM_W_MASK) != a->ow)) {
        for (int64_t q = 0; q < cnt; q++) {
            ray_t* err = acc_put(a, v, base + (li16 ? (int64_t)li16[q] : li64[q]), pos + q, 1);
            if (err) return err;
        }
        return NULL;
    }
    uint8_t esz = ray_sym_elem_size(v->type, v->attrs);
    const char* sd = (const char*)v->data;
    char* d = (char*)ray_data(a->out) + (size_t)pos * esz;
    if (li16) ACC_LIST_COPY(base + (int64_t)li16[q]);
    else      ACC_LIST_COPY(base + li64[q]);
    if (v->type == RAY_SYM) {
        for (int64_t q = 0; q < cnt && !a->nulls; q++)
            if (ray_read_sym(ray_data(a->out), pos + q, RAY_SYM, a->out->attrs) == 0) a->nulls = true;
    } else if (v->may_null) {
        for (int64_t q = 0; q < cnt; q++) {
            int64_t r = base + (li16 ? (int64_t)li16[q] : li64[q]);
            if (ray_raw_is_null(v->type, v->attrs, v->data, r)) acc_null(a, pos + q);
        }
    }
    return NULL;
}
#undef ACC_LIST_COPY

/* Point reads (open decision D2): a vector holding at most
 * RAY_COL_POINT_ROWS of the requested rows serves them through
 * ray_col_src_points_i64 (FOR_BP, DICT over FOR_BP, CONST or RLE without
 * nulls: the body read once, then one lane unpacked or one run found per
 * row) instead of being decoded whole; the crossover is measured in the
 * Task 6b report.  A
 * declined read (nulls, RAW, DELTA_BP, a plain or materialized source)
 * leaves the vector to the cursor, which decodes or reports it.  A
 * point-served value lies inside its vector's statistics
 * (ray_enccol_point_i64, `corrupt` otherwise), and those nest inside the
 * chunk's directory statistics (descriptor validation), so a guarded
 * source (stats_guard) needs no further check: ray_enccol_check_dir's rule
 * holds for every value served. */
#define GATHER_POINT_ROWS RAY_COL_POINT_ROWS

/* The source's values can be point-read: an encoded integer or temporal
 * column (SYM ids are read by decode, which checks them against the
 * domain bound). */
static bool gather_point_src(const ray_col_src_t* src) {
    if (!ray_col_src_encoded(src)) return false;
    switch (src->type) {
    case RAY_I64: case RAY_I32: case RAY_I16: case RAY_U8: case RAY_BOOL:
    case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP: return true;
    default: return false;
    }
}

/* A point-served value (never null) at output position pos. */
static inline void acc_point(gather_acc_t* a, int64_t pos, int64_t x) {
    void* d = ray_data(a->out);
    switch (ray_sym_elem_size(a->out->type, a->out->attrs)) {
    case 1:  ((uint8_t*)d)[pos]  = (uint8_t)x;  break;
    case 2:  ((int16_t*)d)[pos]  = (int16_t)x;  break;
    case 4:  ((int32_t*)d)[pos]  = (int32_t)x;  break;
    default: ((int64_t*)d)[pos]  = x;           break;
    }
}

static ray_t* acc_finish(gather_acc_t* a, const ray_col_src_t* src, ray_t** out) {
    ray_t* o = a->out;
    if (a->pool) { o->str_pool = a->pool; a->pool = NULL; }   /* the result's own pool */
    if (a->nulls) o->attrs |= RAY_ATTR_HAS_NULLS;
    if (src->type == RAY_SYM && src->dom && src->dom != ray_sym_runtime_domain()) {
        ray_sym_domain_retain(src->dom);                     /* the agg_src_standin pattern */
        o->sym_domain = src->dom;
    }
    a->out = NULL;
    *out = o;
    return NULL;
}

/* Positions 0..n) ordered by ids[pos], stable (repeated ids keep their
 * input order): an LSD radix sort, 11-bit digits over the bits of
 * max_id.  Ids are checked to lie in [0, max_id] by the caller. */
#define GATHER_RADIX_BITS 11
static ray_t* gather_order(const int64_t* ids, int64_t n, int64_t max_id, int64_t** out) {
    int64_t* ord = (int64_t*)ray_alloc_raw((size_t)n * sizeof(int64_t));
    int64_t* tmp = (int64_t*)ray_alloc_raw((size_t)n * sizeof(int64_t));
    if (!ord || !tmp) { ray_free_raw(ord); ray_free_raw(tmp); return ray_error("oom", "gather order"); }
    for (int64_t k = 0; k < n; k++) ord[k] = k;
    int bits = 0;
    while (bits < 63 && ((uint64_t)max_id >> bits) != 0) bits++;
    int64_t cnt[1 << GATHER_RADIX_BITS];
    for (int sh = 0; sh < bits; sh += GATHER_RADIX_BITS) {
        memset(cnt, 0, sizeof cnt);
        for (int64_t k = 0; k < n; k++) cnt[((uint64_t)ids[k] >> sh) & ((1u << GATHER_RADIX_BITS) - 1)]++;
        int64_t sum = 0;
        for (int d = 0; d < (1 << GATHER_RADIX_BITS); d++) { int64_t c = cnt[d]; cnt[d] = sum; sum += c; }
        for (int64_t k = 0; k < n; k++) {
            int64_t p = ord[k];
            tmp[cnt[((uint64_t)ids[p] >> sh) & ((1u << GATHER_RADIX_BITS) - 1)]++] = p;
        }
        int64_t* sw = ord; ord = tmp; tmp = sw;
    }
    ray_free_raw(tmp);
    *out = ord;
    return NULL;
}

ray_t* ray_col_gather_rows(ray_col_src_t* src, const int64_t* ids, int64_t n, ray_t** out) {
    *out = NULL;
    if (n < 0) return ray_error("range", "gather: %lld rows", (long long)n);
    bool sorted = true;
    for (int64_t k = 0; k < n; k++) {
        if (ids[k] < 0 || ids[k] >= src->len)
            return ray_error("range", "gather: row %lld of %lld", (long long)ids[k], (long long)src->len);
        if (k && ids[k] < ids[k - 1]) sorted = false;
    }
    gather_acc_t a;
    ray_t* err = acc_open(&a, src, n);
    if (err) return err;
    /* the decode schedule: positions in id order, so the cursor keeps its
     * view while the ids stay inside it and each touched vector (segment
     * local for a parted source) is range-read once; each value still goes
     * to its own output position */
    int64_t* ord = NULL;
    if (!sorted && (err = gather_order(ids, n, src->len - 1, &ord)) != NULL) { acc_abort(&a); return err; }
    ray_col_fixed_t fixed;                       /* the decode vector on the stack (P2 17i) */
    ray_col_cursor_t cur;
    ray_col_cursor_init_lent(&cur, src, &fixed);
    const bool point = gather_point_src(src);
    for (int64_t k = 0; k < n && !err; ) {
        int64_t pos = ord ? ord[k] : k, r = ids[pos];
        int64_t vlo, vhi;
        if (point && ray_col_src_vec_span(src, r, &vlo, &vhi)) {
            /* the ids inside r's vector, in decode order: few are point-read */
            int64_t j = k + 1;
            while (j < n && j - k <= GATHER_POINT_ROWS && ids[ord ? ord[j] : j] < vhi) j++;
            if (j - k <= GATHER_POINT_ROWS) {
                int64_t rows[GATHER_POINT_ROWS], x[GATHER_POINT_ROWS];
                const int64_t* rp = ids + k;     /* in order: the ids themselves */
                if (ord) { for (int64_t q = k; q < j; q++) rows[q - k] = ids[ord[q]]; rp = rows; }
                int pr = ray_col_src_points_i64(src, rp, j - k, x, &err);
                if (pr < 0) break;
                if (pr > 0) {                    /* every row of the vector served */
                    for (int64_t q = k; q < j; q++) acc_point(&a, ord ? ord[q] : q, x[q - k]);
                    k = j;
                    continue;
                }                                /* declined: the cursor reads the vector */
            }
        }
        const ray_col_range_t* v;
        if ((err = ray_col_cursor_at(&cur, r, &v)) != NULL) break;
        int64_t end = v->start + v->n;
        if (!ord) {                              /* in order: the view's ids are one copy */
            int64_t j = k + 1;
            bool run = true;                     /* every step +1: one run, no repeats */
            for (; j < n && ids[j] < end; j++) run = run && ids[j] == ids[j - 1] + 1;
            err = run ? acc_put(&a, v, ids[k] - v->start, k, j - k)
                      : acc_put_list(&a, v, -v->start, NULL, ids + k, j - k, k);
            k = j;
            continue;
        }
        for (; k < n && !err; k++) {
            pos = ord[k];
            r = ids[pos];
            if (r >= end) break;
            err = acc_put(&a, v, r - v->start, pos, 1);
        }
    }
    ray_col_cursor_free(&cur);
    ray_free_raw(ord);
    if (err) { acc_abort(&a); return err; }
    return acc_finish(&a, src, out);
}

/* ray_col_gather_sel's work: segments [s0, s1) of the selection, the
 * first written at output position pos, through one task's cursor. */
typedef struct {
    ray_col_src_t*  src;
    const uint8_t*  flags;    /* NULL: every segment ALL */
    const uint32_t* offs;
    const uint16_t* idx;
    bool            point;    /* gather_point_src: few-row segments are point-read */
} gsel_in_t;

static ray_t* gsel_range(const gsel_in_t* in, gather_acc_t* a, ray_col_cursor_t* cur,
                         int64_t s0, int64_t s1, int64_t pos) {
    ray_col_src_t* src = in->src;
    ray_t* err = NULL;
    for (int64_t s = s0; s < s1 && !err; s++) {
        uint8_t f = in->flags ? in->flags[s] : RAY_SEL_ALL;
        if (f == RAY_SEL_NONE) continue;                 /* the vector is never read */
        int64_t lo = s * RAY_MORSEL_ELEMS;
        int64_t hi = lo + RAY_MORSEL_ELEMS < src->len ? lo + RAY_MORSEL_ELEMS : src->len;
        if (f == RAY_SEL_ALL) {
            /* a segment may span two views (a parted segment boundary) */
            for (int64_t r = lo; r < hi && !err; ) {
                const ray_col_range_t* v;
                if ((err = ray_col_cursor_at(cur, r, &v)) != NULL) break;
                int64_t end = v->start + v->n < hi ? v->start + v->n : hi;
                err = acc_put(a, v, r - v->start, pos, end - r);
                pos += end - r;
                r = end;
            }
            continue;
        }
        uint32_t k0 = in->offs[s], k1 = in->offs[s + 1];
        for (uint32_t k = k0; k < k1; k++)
            if (in->idx[k] >= hi - lo)
                return ray_error("corrupt", "gather: selection row %lld past its segment", (long long)(lo + in->idx[k]));
        uint32_t k = k0;
        if (in->point && k1 - k0 <= GATHER_POINT_ROWS) {
            /* few rows: point reads of each vector they fall in (a parted
             * segment boundary may split them; the segment's rows bound
             * each vector's share), a declined vector left to the cursor */
            int64_t rows[GATHER_POINT_ROWS], x[GATHER_POINT_ROWS];
            while (k < k1) {
                int64_t vlo, vhi;
                if (!ray_col_src_vec_span(src, lo + in->idx[k], &vlo, &vhi)) break;
                uint32_t j = k;
                for (; j < k1 && lo + in->idx[j] < vhi; j++) rows[j - k] = lo + in->idx[j];
                int pr = ray_col_src_points_i64(src, rows, j - k, x, &err);
                if (pr <= 0) break;
                for (uint32_t q = k; q < j; q++) acc_point(a, pos++, x[q - k]);
                k = j;
            }
            if (err) break;
        }
        while (k < k1 && !err) {
            const ray_col_range_t* v;
            if ((err = ray_col_cursor_at(cur, lo + in->idx[k], &v)) != NULL) break;
            int64_t end = v->start + v->n;
            uint32_t j = k + 1;
            /* a rowsel's indices ascend inside a segment (engine-built),
             * so a view's run never starts before the view */
            while (j < k1 && lo + in->idx[j] < end) { assert(in->idx[j] > in->idx[j - 1]); j++; }
            err = acc_put_list(a, v, lo - v->start, in->idx + k, NULL, j - k, pos);
            pos += j - k;
            k = j;
        }
    }
    return err;
}

/* The parallel form: task t gathers segments [t * spt, (t + 1) * spt) at
 * output position pos0[t] (the selection's own counts, summed before the
 * dispatch) into its own accumulator over the shared output; failures go
 * to the envelope keyed by the task's first row. */
typedef struct {
    gsel_in_t        in;
    int64_t          nseg, spt;
    const int64_t*   pos0;
    gather_acc_t*    acc;
    ray_scan_fail_t  fail;
} gsel_par_t;

static void gsel_task(void* ctxp, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    gsel_par_t* c = (gsel_par_t*)ctxp;
    for (int64_t t = start; t < end; t++) {
        int64_t s0 = t * c->spt, s1 = s0 + c->spt < c->nseg ? s0 + c->spt : c->nseg;
        int64_t key = s0 * RAY_MORSEL_ELEMS;
        if (ray_scan_fail_skip(&c->fail, key)) continue;
        ray_col_fixed_t fixed;                   /* the task's decode vector on its stack */
        ray_col_cursor_t cur;
        ray_col_cursor_init_lent(&cur, c->in.src, &fixed);
        ray_t* err = gsel_range(&c->in, &c->acc[t], &cur, s0, s1, c->pos0[t]);
        ray_col_cursor_free(&cur);
        if (err) ray_scan_fail_note(&c->fail, key, err);
    }
}

/* Task pools appended in task order (row order) into one pool: each task's
 * pooled descriptors (output positions [pos0[t], pos0[t + 1])) move by the
 * bytes before them.  `range` when the whole would pass UINT32_MAX. */
static ray_t* gsel_merge_pools(gather_acc_t* acc, int64_t ntask, const int64_t* pos0, gather_acc_t* a) {
    int64_t total = 0, first = -1, users = 0;
    for (int64_t t = 0; t < ntask; t++)
        if (acc[t].pool) { total += acc[t].pool->len; users++; if (first < 0) first = t; }
    if (!users) return NULL;
    /* one pool: adopted as is (nothing before it, nothing to move) */
    if (users == 1) { a->pool = acc[first].pool; acc[first].pool = NULL; return NULL; }
    int64_t at;
    ray_err_t pe = ray_str_pool_reserve(&a->pool, (size_t)total, &at);
    if (pe == RAY_ERR_RANGE)
        return ray_error("range", "gather: STR pool exceeds %lld bytes", (long long)UINT32_MAX);
    if (pe != RAY_OK) return ray_error("oom", NULL);
    ray_str_t* d = (ray_str_t*)ray_data(a->out);
    int64_t b = at;
    for (int64_t t = 0; t < ntask; t++) {
        if (!acc[t].pool) continue;
        memcpy((char*)ray_data(a->pool) + b, ray_data(acc[t].pool), (size_t)acc[t].pool->len);
        if (b)
            for (int64_t p = pos0[t]; p < pos0[t + 1]; p++)
                if (d[p].len && !ray_str_is_inline(&d[p])) d[p].pool_off += (uint32_t)b;
        b += acc[t].pool->len;
        ray_release(acc[t].pool);
        acc[t].pool = NULL;
    }
    a->pool->len = b;
    return NULL;
}

ray_t* ray_col_gather_sel(ray_col_src_t* src, ray_t* sel, ray_t** out) {
    *out = NULL;
    ray_rowsel_t* m = sel ? ray_rowsel_meta(sel) : NULL;
    if (m && m->nrows != src->len)
        return ray_error("length", "gather: selection of %lld rows over %lld", (long long)m->nrows, (long long)src->len);
    int64_t n = m ? m->total_pass : src->len;
    gather_acc_t a;
    ray_t* err = acc_open(&a, src, n);
    if (err) return err;
    int64_t nseg = m ? (int64_t)m->n_segs : (src->len + RAY_MORSEL_ELEMS - 1) / RAY_MORSEL_ELEMS;
    gsel_in_t in = { .src = src, .flags = sel ? ray_rowsel_flags(sel) : NULL,
                     .offs = sel ? ray_rowsel_offsets(sel) : NULL, .idx = sel ? ray_rowsel_idx(sel) : NULL,
                     .point = gather_point_src(src) };
    /* the tasks: the pool's grain of segments each, at most the ring's
     * initial capacity of them; one runs here when the pool cannot help */
    ray_pool_t* pool = ray_pool_get();
    int64_t spt = RAY_DISPATCH_MORSELS;
    while ((nseg + spt - 1) / spt > (int64_t)RAY_POOL_INIT_TASKS) spt *= 2;
    int64_t ntask = ray_pool_par_dispatch_ok(pool, n, RAY_PARALLEL_THRESHOLD) ? (nseg + spt - 1) / spt : 1;
    if (ntask <= 1) { ntask = 1; spt = nseg; }
    /* each task's first output position, from the selection's counts:
     * checked against total_pass before anything is written */
    int64_t pos0_one[2];
    int64_t* pos0 = ntask > 1 ? (int64_t*)ray_alloc_raw((size_t)(ntask + 1) * sizeof(int64_t)) : pos0_one;
    gather_acc_t* acc = ntask > 1 ? (gather_acc_t*)ray_calloc_raw((size_t)ntask * sizeof(gather_acc_t)) : NULL;
    if (!pos0 || (ntask > 1 && !acc)) {
        if (pos0 != pos0_one) ray_free_raw(pos0);
        ray_free_raw(acc);
        acc_abort(&a);
        return ray_error("oom", "gather tasks");
    }
    int64_t cum = 0;
    for (int64_t s = 0; s < nseg && !err; s++) {
        if (s % spt == 0) pos0[s / spt] = cum;
        uint8_t f = in.flags ? in.flags[s] : RAY_SEL_ALL;
        if (f == RAY_SEL_NONE) continue;
        int64_t lo = s * RAY_MORSEL_ELEMS;
        int64_t hi = lo + RAY_MORSEL_ELEMS < src->len ? lo + RAY_MORSEL_ELEMS : src->len;
        /* the rows this segment adds, checked before any is written */
        int64_t add = f == RAY_SEL_ALL ? hi - lo : (int64_t)in.offs[s + 1] - (int64_t)in.offs[s];
        if (add < 0 || add > n - cum)
            err = ray_error("corrupt", "gather: selection holds more rows than its %lld", (long long)n);
        else
            cum += add;
    }
    if (nseg == 0) pos0[0] = 0;
    pos0[ntask] = cum;
    if (!err && cum != n)
        err = ray_error("corrupt", "gather: selection holds %lld rows, says %lld", (long long)cum, (long long)n);
    if (!err && ntask == 1) {
        ray_col_fixed_t fixed;                   /* the decode vector on the stack (P2 17i) */
        ray_col_cursor_t cur;
        ray_col_cursor_init_lent(&cur, src, &fixed);
        err = gsel_range(&in, &a, &cur, 0, nseg, 0);
        ray_col_cursor_free(&cur);
    } else if (!err) {
        gsel_par_t c = { .in = in, .nseg = nseg, .spt = spt, .pos0 = pos0, .acc = acc };
        for (int64_t t = 0; t < ntask; t++) { acc[t].out = a.out; acc[t].ow = a.ow; }
        ray_scan_fail_init(&c.fail);
        atomic_fetch_add_explicit(&g_par_gathers, 1, memory_order_relaxed);
        ray_pool_dispatch_n(pool, gsel_task, &c, (uint32_t)ntask);
        err = ray_scan_fail_finish(&c.fail, pool);
        for (int64_t t = 0; t < ntask; t++) a.nulls |= acc[t].nulls;
        if (!err) err = gsel_merge_pools(acc, ntask, pos0, &a);
        for (int64_t t = 0; t < ntask; t++) if (acc[t].pool) ray_release(acc[t].pool);
    }
    if (pos0 != pos0_one) ray_free_raw(pos0);
    ray_free_raw(acc);
    if (err) { acc_abort(&a); return err; }
    return acc_finish(&a, src, out);
}

ray_t* ray_col_gather_slot(ray_t* enc) {
    return (enc && enc->type == RAY_ENCCOL && !(enc->attrs & RAY_ATTR_SLICE)) ? enc : NULL;
}

ray_t* ray_col_gather_enc(ray_t* enc, ray_t* sel, bool by_rows, const int64_t* ids, int64_t n, ray_t** out) {
    *out = NULL;
    ray_col_src_t s;
    ray_t* err = ray_col_src_bind(enc, &s);
    if (err) return err;
    err = by_rows ? ray_col_gather_rows(&s, ids, n, out) : ray_col_gather_sel(&s, sel, out);
    ray_col_src_release(&s);
    return err;
}
