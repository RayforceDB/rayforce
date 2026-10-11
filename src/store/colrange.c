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

#include "store/colrange.h"
#include "core/platform.h"
#include "core/pool.h"
#include "store/col.h"
#include "store/enccol.h"
#include "table/sym.h"
#include "table/domain.h"
#include "table/table.h"
#include "ops/ops.h"
#include "ops/idxop.h"
#include "vec/vec.h"
#include <string.h>

/* Fixed scratch: one vector of the widest element (GUID, 16 bytes). */
#define SCRATCH_FIXED_BYTES ((size_t)RAY_ENC_VEC * 16)
_Static_assert(SCRATCH_FIXED_BYTES == RAY_COL_FIXED_BYTES, "a lent fixed vector holds one vector");

void ray_col_scratch_init(ray_col_scratch_t* s) { memset(s, 0, sizeof *s); }

void ray_col_scratch_lend(ray_col_scratch_t* s, ray_col_fixed_t* buf) {
    if (!s->fixed_lent) ray_free_raw(s->fixed);
    s->fixed = buf->b;
    s->fixed_lent = true;
}

static _Atomic(uint64_t) g_fixed_allocs;           /* test hook */
uint64_t ray_col_test_fixed_allocs(void) { return atomic_load(&g_fixed_allocs); }

/* Decoded bytes are counted per scratch and column, not per vector: the
 * counters are shared by every worker.  The scratch references the column
 * it counts for (once per column, not per vector), so a flush never reads a
 * freed column, whatever the order of the scratch's and source's release. */
static _Atomic(uint64_t) g_wide_hits;              /* test hook */
uint64_t ray_col_test_wide_hits(void) { return atomic_load(&g_wide_hits); }

static void scratch_flush(ray_col_scratch_t* s) {
    if (s->acc_e) {
        ray_enccol_note_decoded(s->acc_e, s->acc_bytes);
        ray_enccol_unref(s->acc_e);
    }
    if (s->wide_hits) atomic_fetch_add_explicit(&g_wide_hits, s->wide_hits, memory_order_relaxed);
    s->acc_e = NULL;
    s->acc_bytes = 0;
    s->wide_hits = 0;
}
static void scratch_count(ray_col_scratch_t* s, ray_enccol_t* e, uint64_t nb) {
    if (s->acc_e != e) { scratch_flush(s); ray_enccol_ref(e); s->acc_e = e; }
    s->acc_bytes += nb;
}

void ray_col_scratch_free(ray_col_scratch_t* s) {
    scratch_flush(s);
    if (!s->fixed_lent) ray_free_raw(s->fixed);
    ray_free_raw(s->pool);
    memset(s, 0, sizeof *s);
}

static ray_t* plain_owner(ray_t* v) { return (v->attrs & RAY_ATTR_SLICE) ? v->slice_parent : v; }

/* The domain a SYM source's ids resolve over: the first present segment's
 * for a parted source (segments share one domain). */
static struct ray_sym_domain_s* sym_domain_of(ray_t* obj) {
    if (obj->type == RAY_ENCCOL) {
        ray_enccol_t* e = ray_enccol_of(obj);
        if (e->kind == RAY_ENCCOL_SINGLE) return e->dom;
        obj = e->wrapper;
    }
    if (RAY_IS_PARTED(obj->type)) {
        ray_t** segs = (ray_t**)ray_data(obj);
        for (int64_t i = 0; i < obj->len; i++)
            if (segs[i]) return sym_domain_of(segs[i]);
        return ray_sym_runtime_domain();
    }
    return ray_sym_vec_domain(obj);
}

static bool may_null_of(ray_t* obj) {
    ray_col_meta_t m;
    ray_col_meta_of(obj, &m);
    if (m.null_count >= 0) return m.null_count > 0;
    if (RAY_IS_PARTED(m.type)) {
        ray_t* w = obj->type == RAY_ENCCOL ? ray_enccol_of(obj)->wrapper : obj;
        ray_t** segs = (ray_t**)ray_data(w);
        for (int64_t i = 0; i < w->len; i++)
            if (segs[i] && may_null_of(segs[i])) return true;
        return false;
    }
    /* generation 0: the existing nullability proof (always true for SYM
     * and STR, whose nulls are in-band) */
    return ray_is_vec(obj) ? ray_vec_may_have_nulls(obj) : (m.attrs & RAY_ATTR_HAS_NULLS) != 0;
}

ray_t* ray_col_src_bind(ray_t* col, ray_col_src_t* out) {
    memset(out, 0, sizeof *out);
    if (!col || RAY_IS_ERR(col)) return ray_error("type", "source bind: no column");
    if (col->type == RAY_MAPCOMMON) return ray_error("nyi", "source bind: partition key column");
    if ((col->attrs & RAY_ATTR_SLICE) && (col->type == RAY_ENCCOL || RAY_IS_PARTED(col->type)))
        return ray_error("nyi", "source bind: slice of an encoded or parted column");
    ray_col_meta_t m;
    ray_col_meta_of(col, &m);
    out->type = RAY_IS_PARTED(m.type) ? (int8_t)RAY_PARTED_BASETYPE(m.type) : m.type;
    out->attrs = m.attrs & COL_DISK_ATTRS_MASK;
    out->len = m.len;
    out->may_null = may_null_of(col);
    out->parted = RAY_IS_PARTED(m.type);
    if (out->type == RAY_SYM) {
        out->dom = sym_domain_of(col);
        out->dom_owned = out->dom != ray_sym_runtime_domain();
        if (out->dom_owned) ray_sym_domain_retain(out->dom);
    }
    ray_retain(col);
    out->obj = col;
    return NULL;
}

void ray_col_src_release(ray_col_src_t* s) {
    ray_free_raw(atomic_load_explicit(&s->seg_off, memory_order_acquire));
    if (s->obj) ray_release(s->obj);
    if (s->dom && s->dom_owned) ray_sym_domain_release(s->dom);
    memset(s, 0, sizeof *s);
}

ray_t* ray_table_col_src(ray_t* tbl, int64_t name_id, ray_col_src_t* out) {
    memset(out, 0, sizeof *out);
    ray_col_meta_t m;
    if (!ray_table_col_meta(tbl, name_id, &m)) return NULL;
    ray_t* obj = m.enc ? m.enc : ray_table_col(tbl, name_id);   /* plain: no decode */
    if (!obj || RAY_IS_ERR(obj)) return obj;
    return ray_col_src_bind(obj, out);
}

static void range_plain(ray_t* v, int64_t start, int64_t n, ray_col_range_t* out) {
    uint8_t esz = ray_sym_elem_size(v->type, v->attrs);
    if (n > v->len - start) n = v->len - start;
    out->data = (const char*)ray_data(v) + (size_t)start * esz;
    out->n = n;
    out->type = v->type;
    out->attrs = v->attrs & COL_DISK_ATTRS_MASK;
    out->may_null = ray_vec_may_have_nulls(v);
    ray_t* own = plain_owner(v);
    out->str_pool = (v->type == RAY_STR && own->str_pool && !RAY_IS_ERR(own->str_pool))
                    ? (const char*)ray_data(own->str_pool) : NULL;
    out->dom = v->type == RAY_SYM ? ray_sym_vec_domain(v) : NULL;
}

static ray_t* range_enc(ray_enccol_t* e, int64_t start, int64_t n, bool guard,
                        ray_col_scratch_t* sc, ray_col_range_t* out) {
    int64_t vec = start / RAY_ENC_VEC;
    int64_t off = start % RAY_ENC_VEC;
    int64_t vlen = ray_enc_vec_len(e->len, vec);
    if (n > vlen - off) n = vlen - off;
    if (!sc->fixed) {
        sc->fixed = ray_alloc_raw(SCRATCH_FIXED_BYTES);
        if (!sc->fixed) return ray_error("oom", "range read scratch");
        atomic_fetch_add_explicit(&g_fixed_allocs, 1, memory_order_relaxed);
    }
    int64_t c = vec / RAY_ENC_VECS_PER_CHUNK;
    int j = (int)(vec % RAY_ENC_VECS_PER_CHUNK);
    const ray_enc_desc_t* descs;
    ray_t* err = ray_enccol_chunk_descs_ref(e, c, &descs);  /* in place; validated once per column */
    if (err) return err;
    if (e->type != RAY_STR && e->type != RAY_GUID) {
        /* a RAW vector without nulls is its own decode: read it in place
         * (the consumer guard checks it as it checks a decode).  An
         * integer one counts its bytes as delivered, so the read-once
         * counts of a key hold whatever its vectors' encodings; a float
         * one counts none (Task 9). */
        const void* pv = ray_enccol_raw_view(e, c, j, &descs[j]);
        if (pv) {
            if (guard && (err = ray_enccol_check_dir(e, c, j, pv, (int)vlen, false)) != NULL) return err;
            if (e->type != RAY_F64 && e->type != RAY_F32) scratch_count(sc, e, (uint64_t)vlen * ray_enccol_esz(e));
            out->data = (const uint8_t*)pv + (size_t)off * ray_enccol_esz(e);
            out->n = n;
            out->type = e->type;
            out->attrs = e->attrs;
            out->may_null = false;
            out->str_pool = NULL;
            out->dom = e->type == RAY_SYM ? e->dom : NULL;
            return NULL;
        }
    }
    if (e->type == RAY_STR) {
        uint64_t pb = 0;
        err = ray_enccol_desc_pool_bytes(e, c, j, &descs[j], &pb);
        if (err) return err;
        if (pb > sc->pool_cap) {                            /* no copy of bytes about to be overwritten */
            ray_free_raw(sc->pool);
            sc->pool = NULL;
            sc->pool_cap = 0;
            sc->pool = ray_alloc_raw((size_t)pb);
            if (!sc->pool) return ray_error("oom", "range read pool");
            sc->pool_cap = (size_t)pb;
        }
    }
    uint64_t nb = 0;
    err = ray_enccol_decode_into(e, c, j, &descs[j], sc->fixed, sc->pool, 0, sc->pool_cap, &nb);
    if (err) return err;                                    /* the view is never returned */
    /* the consumer guard: its memory was sized from the chunk's statistics */
    if (guard && (err = ray_enccol_check_dir(e, c, j, sc->fixed, (int)vlen, descs[j].n_nulls > 0)) != NULL) return err;
    scratch_count(sc, e, nb);
    out->data = sc->fixed + (size_t)off * ray_enccol_esz(e);
    out->n = n;
    out->type = e->type;
    out->attrs = e->attrs;
    out->may_null = descs[j].n_nulls > 0;
    out->str_pool = e->type == RAY_STR ? sc->pool : NULL;
    out->dom = e->type == RAY_SYM ? e->dom : NULL;
    return NULL;
}

/* The plain or encoded parted wrapper behind obj, or NULL. */
static ray_t* parted_wrapper(ray_t* obj) {
    if (obj->type == RAY_ENCCOL) {
        ray_enccol_t* e = ray_enccol_of(obj);
        return e->kind == RAY_ENCCOL_SINGLE ? NULL : e->wrapper;
    }
    return RAY_IS_PARTED(obj->type) ? obj : NULL;
}

/* A parted source's segment row offsets (off[i] = first row of segment i,
 * off[n] = len), built on the first range read and published once (workers
 * share the source).  A missing segment (hand-built only) holds no rows.
 * The encoded wrapper and its materialization have the same segments. */
static ray_t* src_seg_off(ray_col_src_t* s, ray_t* w, const int64_t** out) {
    int64_t* a = atomic_load_explicit(&s->seg_off, memory_order_acquire);
    if (!a) {
        a = ray_alloc_raw((size_t)(w->len + 1) * sizeof *a);
        if (!a) return ray_error("oom", "parted segment offsets");
        ray_t** segs = (ray_t**)ray_data(w);
        a[0] = 0;
        for (int64_t i = 0; i < w->len; i++)
            a[i + 1] = a[i] + (segs[i] ? ray_table_col_nrows(segs[i]) : 0);
        int64_t* exp = NULL;
        if (!atomic_compare_exchange_strong_explicit(&s->seg_off, &exp, a, memory_order_acq_rel,
                                                     memory_order_acquire)) {
            ray_free_raw(a);
            a = exp;
        }
    }
    *out = a;
    return NULL;
}

/* off: the parted wrapper's segment offsets (src_seg_off), NULL otherwise. */
static ray_t* range_obj(ray_t* obj, int64_t start, int64_t n, const int64_t* off, bool guard,
                        ray_col_scratch_t* sc, ray_col_range_t* out) {
    if (obj->type == RAY_ENCCOL) {
        ray_enccol_t* e = ray_enccol_of(obj);
        if (e->kind == RAY_ENCCOL_SINGLE) return range_enc(e, start, n, guard, sc, out);
        obj = e->wrapper;
    }
    if (RAY_IS_PARTED(obj->type)) {
        ray_t** segs = (ray_t**)ray_data(obj);
        int64_t ns = obj->len;
        if (!off || ns <= 0 || start < 0 || start >= off[ns])
            return ray_error("range", "range read past the last segment");
        int64_t lo = 0, hi = ns - 1;                     /* the last i with off[i] <= start */
        while (lo < hi) {
            int64_t mid = lo + (hi - lo + 1) / 2;
            if (off[mid] <= start) lo = mid; else hi = mid - 1;
        }
        int64_t sl = off[lo + 1] - off[lo];              /* > 0: start < off[lo + 1] */
        int64_t local = start - off[lo];
        if (n > sl - local) n = sl - local;              /* never cross a segment */
        /* the guard of a parted source checks each segment against its
         * own statistics (ray_col_src_parted_chunks serves them) */
        return range_obj(segs[lo], local, n, NULL, guard, sc, out);
    }
    range_plain(obj, start, n, out);
    return NULL;
}

/* range_obj over the source's object `obj` (s->obj or its published
 * materialization), with the segment offsets for a parted one. */
static ray_t* range_src(ray_col_src_t* s, ray_t* obj, int64_t start, int64_t n,
                        ray_col_scratch_t* sc, ray_col_range_t* out) {
    const int64_t* off = NULL;
    ray_t* w = parted_wrapper(obj);
    if (w) {
        ray_t* err = src_seg_off(s, w, &off);
        if (err) return err;
    }
    return range_obj(obj, start, n, off, s->stats_guard, sc, out);
}

ray_t* ray_col_range(ray_col_src_t* src, int64_t start, int64_t n,
                     ray_col_scratch_t* scratch, ray_col_range_t* out) {
    memset(out, 0, sizeof *out);
    if (!src || !src->obj || start < 0 || n < 0 || start > src->len)
        return ray_error("range", "range read [%lld, +%lld) outside %lld rows",
                         (long long)start, (long long)n, src ? (long long)src->len : 0LL);
    out->start = start;
    out->type = src->type;
    out->attrs = src->attrs;
    out->dom = src->dom;
    if (n == 0 || start == src->len) return NULL;
    ray_t* err = range_src(src, src->obj, start, n, scratch, out);
    out->start = start;
    return err;
}

ray_t* ray_col_range_wide(ray_col_src_t* src, int64_t start, int64_t n, ray_col_scratch_t* sc,
                          int8_t wide, void* dst, int64_t* got) {
    *got = 0;
    if (!src || !src->obj || src->obj->type != RAY_ENCCOL || start < 0 || n <= 0 || start % RAY_ENC_VEC) return NULL;
    ray_enccol_t* e = ray_enccol_of(src->obj);
    if (e->kind != RAY_ENCCOL_SINGLE || start >= e->len) return NULL;
    int64_t vec = start / RAY_ENC_VEC;
    /* under the consumer guard only a vector already found inside its
     * chunk's statistics; otherwise the range read checks it */
    if (src->stats_guard && !ray_enccol_dir_vec_ok(e, vec / RAY_ENC_VECS_PER_CHUNK, (int)(vec % RAY_ENC_VECS_PER_CHUNK)))
        return NULL;
    if (n != ray_enc_vec_len(e->len, vec)) return NULL;
    int64_t c = vec / RAY_ENC_VECS_PER_CHUNK;
    const ray_enc_desc_t* descs;
    ray_t* err = ray_enccol_chunk_descs_ref(e, c, &descs);
    if (err) return err;
    uint64_t nb = 0;
    if (ray_enccol_decode_wide(e, c, (int)(vec % RAY_ENC_VECS_PER_CHUNK), &descs[vec % RAY_ENC_VECS_PER_CHUNK],
                               wide, dst, &nb)) {
        scratch_count(sc, e, nb);
        sc->wide_hits++;
        *got = n;
    }
    return NULL;
}

/* The vector starting at row `start` of a generation-2 vector source:
 * the single file (or a parted source's segment) and the vector's local
 * position; false when row `start` is not such a vector's first row. */
static bool into_vec(ray_col_src_t* s, int64_t start, ray_enccol_t** ep, int64_t* local, ray_t** err) {
    ray_t* obj = s->obj;
    *err = NULL;
    if (!obj || obj->type != RAY_ENCCOL || ray_enccol_decoded_peek(obj)) return false;   /* published: read in place */
    ray_enccol_t* e = ray_enccol_of(obj);
    if (e->kind != RAY_ENCCOL_SINGLE) {
        const int64_t* off = NULL;
        ray_t* w = e->wrapper;
        if ((*err = src_seg_off(s, w, &off)) != NULL) return false;
        int64_t ns = w->len;
        if (ns <= 0 || start >= off[ns]) return false;
        int64_t lo = 0, hi = ns - 1;                     /* the last i with off[i] <= start */
        while (lo < hi) {
            int64_t mid = lo + (hi - lo + 1) / 2;
            if (off[mid] <= start) lo = mid; else hi = mid - 1;
        }
        ray_t* sg = ((ray_t**)ray_data(w))[lo];
        if (!sg || sg->type != RAY_ENCCOL) return false;
        e = ray_enccol_of(sg);
        if (e->kind != RAY_ENCCOL_SINGLE) return false;
        start -= off[lo];
    }
    if (start % RAY_ENC_VEC || start >= e->len) return false;
    *ep = e;
    *local = start;
    return true;
}

static _Atomic(uint64_t) g_into_calls, g_into_rows;   /* test hook */
void ray_col_test_into(uint64_t* calls, uint64_t* rows) {
    *calls = atomic_load(&g_into_calls);
    *rows = atomic_load(&g_into_rows);
}

ray_t* ray_col_range_into(ray_col_src_t* src, int64_t start, int64_t n, ray_col_scratch_t* sc,
                          uint8_t attrs, void* dst, int64_t* got) {
    *got = 0;
    if (!src || start < 0 || n <= 0 || start >= src->len) return NULL;
    if (src->stats_guard) return NULL;             /* guarded: the range read checks each vector */
    ray_enccol_t* e;
    int64_t local;
    ray_t* err;
    if (!into_vec(src, start, &e, &local, &err)) return err;
    if (e->type == RAY_STR || e->type == RAY_GUID) return NULL;
    if (e->type == RAY_SYM && ((e->attrs ^ attrs) & RAY_SYM_W_MASK)) return NULL;   /* another width: widen by range */
    int64_t vec = local / RAY_ENC_VEC;
    int64_t vlen = ray_enc_vec_len(e->len, vec);
    if (vlen > n) return NULL;
    int64_t c = vec / RAY_ENC_VECS_PER_CHUNK;
    int j = (int)(vec % RAY_ENC_VECS_PER_CHUNK);
    const ray_enc_desc_t* descs;
    if ((err = ray_enccol_chunk_descs_ref(e, c, &descs)) != NULL) return err;
    const size_t esz = ray_enccol_esz(e);
    const char* pv = ray_enccol_raw_view(e, c, j, &descs[j]);
    if (!pv) {
        uint64_t nb = 0;
        if ((err = ray_enccol_decode_into(e, c, j, &descs[j], dst, NULL, 0, 0, &nb)) != NULL) return err;
        scratch_count(sc, e, nb);
        *got = vlen;
        atomic_fetch_add_explicit(&g_into_calls, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_into_rows, (uint64_t)vlen, memory_order_relaxed);
        return NULL;
    }
    /* RAW vectors are their own decode (as range_enc reads them): the run
     * of them whose payloads lie end to end in the mapping is one copy (a
     * large copy streams; a vector at a time took twice a plain segment's
     * copy, ClickBench q32's WatchID on partitions) */
    int64_t rows = vlen;
    while (rows < n && local + rows < e->len) {
        int64_t v2 = (local + rows) / RAY_ENC_VEC, l2 = ray_enc_vec_len(e->len, v2);
        if (rows + l2 > n) break;
        int64_t c2 = v2 / RAY_ENC_VECS_PER_CHUNK;
        int j2 = (int)(v2 % RAY_ENC_VECS_PER_CHUNK);
        const ray_enc_desc_t* d2;
        ray_t* e2 = ray_enccol_chunk_descs_ref(e, c2, &d2);
        if (e2) { ray_error_free(e2); break; }          /* the vector's own read reports it */
        if (ray_enccol_raw_view(e, c2, j2, &d2[j2]) != pv + (size_t)rows * esz) break;
        rows += l2;
    }
    memcpy(dst, pv, (size_t)rows * esz);
    if (e->type != RAY_F64 && e->type != RAY_F32) scratch_count(sc, e, (uint64_t)rows * esz);
    *got = rows;
    atomic_fetch_add_explicit(&g_into_calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_into_rows, (uint64_t)rows, memory_order_relaxed);
    return NULL;
}

ray_t* ray_col_src_plain(ray_col_src_t* s, ray_t** out) {
    *out = NULL;
    if (!s || !s->obj) return ray_error("type", "source not bound");
    if (s->obj->type != RAY_ENCCOL) { *out = s->obj; return NULL; }
    ray_t* p = ray_enccol_materialize(s->obj);     /* the object's once-state, cached */
    if (RAY_IS_ERR(p)) return p;
    /* the consumer guard over the whole materialization: once per column */
    ray_t* gerr = s->stats_guard ? ray_enccol_check_dir_all(s->obj) : NULL;
    if (gerr) return gerr;
    *out = p;
    return NULL;
}

int64_t ray_col_src_sym_find(const ray_col_src_t* s, const char* str, size_t len) {
    if (!s || s->type != RAY_SYM || !s->dom) return -1;
    return ray_sym_domain_find(s->dom, str, len);
}

/* Codes are assigned by first occurrence (not sorted), so there is no
 * persisted string->code map: scan the n_distinct first-occurrence rows. */
int64_t ray_col_src_dict_code(const ray_col_src_t* s, const char* str, size_t len) {
    if (!s || s->type != RAY_STR || !s->obj || s->obj->type == RAY_ENCCOL || s->parted) return -2;
    ray_t* col = s->obj;
    if (ray_index_kind(col) != RAY_IDX_DICT) return -2;
    const ray_index_t* dix = ray_index_payload(col->index);
    int64_t nd = dix->u.dict.n_distinct;
    if (nd <= 0 || !dix->u.dict.first_occ || RAY_IS_ERR(dix->u.dict.first_occ)) return -1;
    const int32_t* focc = (const int32_t*)ray_data(dix->u.dict.first_occ);
    for (int64_t c = 0; c < nd; c++) {
        size_t l = 0;
        const char* p = ray_str_vec_get(col, focc[c], &l);
        if (!p) continue;
        if (l == len && (len == 0 || memcmp(p, str, len) == 0)) return c;
    }
    return -1;
}

/* ---- Scan side: cursors, the piece walk, the failure envelope ---- */

uint8_t ray_col_src_esz(const ray_col_src_t* s) { return ray_sym_elem_size(s->type, s->attrs); }

ray_t* ray_col_src_ready(const ray_col_src_t* s) {
    if (!s || !s->obj) return NULL;
    if (s->obj->type != RAY_ENCCOL) return s->obj;
    ray_t* pk = ray_enccol_decoded_peek(s->obj);   /* published materialization, or NULL */
    if (pk && s->stats_guard) {
        /* the consumer guard: in place only once the materialization is
         * inside its statistics (cached per column); otherwise not ready,
         * and the range reads or ray_col_src_plain report the failure */
        ray_t* gerr = ray_enccol_check_dir_all(s->obj);
        if (gerr) { ray_error_free(gerr); return NULL; }
    }
    return pk;
}

const void* ray_col_src_base(const ray_col_src_t* s) {
    ray_t* rd = ray_col_src_ready(s);
    return (rd && !RAY_IS_PARTED(rd->type)) ? ray_data(rd) : NULL;
}

void ray_col_cursor_init(ray_col_cursor_t* c, ray_col_src_t* src) {
    memset(c, 0, sizeof *c);
    c->src = src;
    ray_col_scratch_init(&c->sc);
}

void ray_col_cursor_init_lent(ray_col_cursor_t* c, ray_col_src_t* src, ray_col_fixed_t* buf) {
    ray_col_cursor_init(c, src);
    ray_col_scratch_lend(&c->sc, buf);
}

void ray_col_cursor_free(ray_col_cursor_t* c) {
    ray_col_scratch_free(&c->sc);
    memset(c, 0, sizeof *c);
}

ray_t* ray_col_cursor_at(ray_col_cursor_t* c, int64_t r, const ray_col_range_t** out) {
    *out = NULL;
    if (c->have && r >= c->v.start && r < c->v.start + c->v.n) { *out = &c->v; return NULL; }
    ray_col_src_t* s = c->src;
    if (!s || !s->obj || r < 0 || r >= s->len) return ray_error("range", "scan cursor: row %lld", (long long)r);
    c->have = false;
    /* A published materialization is read in place (single: a plain vector;
     * parted: the plain parted wrapper, read segment by segment). */
    ray_t* obj = s->obj;
    if (obj->type == RAY_ENCCOL) {
        ray_t* pk = ray_enccol_decoded_peek(obj);
        if (pk) {
            /* the guard over a published materialization: once per column */
            ray_t* gerr = s->stats_guard ? ray_enccol_check_dir_all(obj) : NULL;
            if (gerr) return gerr;
            obj = pk;
        }
    }
    memset(&c->v, 0, sizeof c->v);
    c->v.start = r;
    c->v.type = s->type;
    c->v.attrs = s->attrs;
    c->v.dom = s->dom;
    ray_t* err = range_src(s, obj, r, s->len - r, &c->sc, &c->v);   /* ray_col_range's body, same clipping */
    c->v.start = r;
    if (err) return err;
    if (c->v.n <= 0) return ray_error("range", "scan cursor: empty range read at row %lld", (long long)r);
    c->have = true;
    *out = &c->v;
    return NULL;
}

/* A plain source's piece fields other than data, fixed for the call. */
static ray_col_piece_t plain_piece(const ray_col_src_t* s) {
    ray_col_piece_t p = { 0 };
    p.attrs = s->attrs;
    p.may_null = s->may_null;
    p.dom = s->dom;
    if (s->type == RAY_STR) {
        ray_t* rd = ray_col_src_ready(s);
        if (rd && !RAY_IS_PARTED(rd->type)) {
            ray_t* own = plain_owner(rd);
            if (own->str_pool && !RAY_IS_ERR(own->str_pool)) p.str_pool = (const char*)ray_data(own->str_pool);
        }
    }
    return p;
}

/* The piece loop of a non-contiguous walk over cursors cur (one per cursor
 * source; their views persist past the call) and the fixed fields pc. */
static ray_t* walk_pieces(ray_col_src_t* const* srcs, const void* const* plain, uint32_t n,
                          ray_col_cursor_t* cur, ray_col_piece_t* pc, int64_t lo, int64_t hi,
                          void* ctx, uint32_t wid, ray_col_piece_fn fn) {
    ray_t* err = NULL;
    for (int64_t b = lo; b < hi && !err; ) {
        int64_t e = hi;
        for (uint32_t i = 0; i < n; i++) {
            ray_col_src_t* s = srcs[i];
            if (!s) continue;
            if (plain[i]) { pc[i].data = (const char*)plain[i] + (size_t)b * ray_col_src_esz(s); continue; }
            const ray_col_range_t* v;
            if ((err = ray_col_cursor_at(&cur[i], b, &v)) != NULL) break;
            uint8_t vesz = ray_sym_elem_size(v->type, v->attrs);
            pc[i].data = (const char*)v->data + (size_t)(b - v->start) * vesz;
            pc[i].attrs = v->attrs; pc[i].may_null = v->may_null;
            pc[i].str_pool = v->str_pool; pc[i].dom = v->dom;
            if (v->start + v->n < e) e = v->start + v->n;
        }
        if (err) break;
        fn(ctx, wid, b, e, pc);
        b = e;
    }
    return err;
}

#define WALK_STACK 16
ray_t* ray_col_walk_cur(ray_col_src_t* const* srcs, const void* const* plain, uint32_t n,
                        ray_col_cursor_t* cur, int64_t lo, int64_t hi,
                        void* ctx, uint32_t wid, ray_col_piece_fn fn) {
    if (lo >= hi) return NULL;
    ray_col_piece_t pst[WALK_STACK];
    ray_col_piece_t* pc = n > WALK_STACK ? ray_calloc_raw((size_t)n * sizeof *pc) : pst;
    if (!pc) return ray_error("oom", "scan walk");
    bool any_cursor = false;
    for (uint32_t i = 0; i < n; i++) {
        pc[i] = srcs[i] ? plain_piece(srcs[i]) : (ray_col_piece_t){ 0 };
        any_cursor |= srcs[i] && !plain[i];
    }
    ray_t* err = NULL;
    if (any_cursor) {
        err = walk_pieces(srcs, plain, n, cur, pc, lo, hi, ctx, wid, fn);
    } else {
        for (uint32_t i = 0; i < n; i++)
            if (srcs[i]) pc[i].data = (const char*)plain[i] + (size_t)lo * ray_col_src_esz(srcs[i]);
        fn(ctx, wid, lo, hi, pc);
    }
    if (pc != pst) ray_free_raw(pc);
    return err;
}

ray_t* ray_col_walk(ray_col_src_t* const* srcs, const void* const* plain, uint32_t n,
                    int64_t lo, int64_t hi, bool contiguous,
                    void* ctx, uint32_t wid, ray_col_piece_fn fn) {
    if (lo >= hi) return NULL;
    if (contiguous && hi - lo > RAY_COL_BLOCK_ROWS)
        return ray_error("range", "scan walk: contiguous block of %lld rows", (long long)(hi - lo));
    ray_col_cursor_t cst[WALK_STACK]; ray_col_piece_t pst[WALK_STACK]; void* bst[WALK_STACK];
    ray_col_cursor_t* cur = cst; ray_col_piece_t* pc = pst; void** buf = bst;
    if (n > WALK_STACK) {
        cur = ray_calloc_raw((size_t)n * sizeof *cur);
        pc  = ray_calloc_raw((size_t)n * sizeof *pc);
        buf = ray_calloc_raw((size_t)n * sizeof *buf);
        if (!cur || !pc || !buf) { ray_free_raw(cur); ray_free_raw(pc); ray_free_raw(buf); return ray_error("oom", "scan walk"); }
    }
    /* Plain and skipped sources get their fixed fields once; only cursor
     * sources hold a cursor (and, contiguous, a buffer) and are re-read per
     * piece. */
    bool any_cursor = false;
    for (uint32_t i = 0; i < n; i++) {
        pc[i] = srcs[i] ? plain_piece(srcs[i]) : (ray_col_piece_t){ 0 };
        if (srcs[i] && !plain[i]) {
            ray_col_cursor_init(&cur[i], srcs[i]);
            buf[i] = NULL;
            any_cursor = true;
        }
    }
    ray_t* err = NULL;
    if (contiguous) {
        for (uint32_t i = 0; i < n && !err; i++) {
            ray_col_src_t* s = srcs[i];
            if (!s) continue;
            uint8_t esz = ray_col_src_esz(s);
            if (plain[i]) { pc[i].data = (const char*)plain[i] + (size_t)lo * esz; continue; }
            if (s->type != RAY_F32 && s->type != RAY_F64) { err = ray_error("nyi", "scan walk: contiguous non-float source"); break; }
            const ray_col_range_t* v;
            if ((err = ray_col_cursor_at(&cur[i], lo, &v)) != NULL) break;
            if (v->start + v->n >= hi) {                 /* one view covers the block: no copy */
                pc[i].data = (const char*)v->data + (size_t)(lo - v->start) * esz;
                pc[i].may_null = v->may_null;
                continue;
            }
            buf[i] = ray_alloc_raw((size_t)(hi - lo) * esz);
            if (!buf[i]) { err = ray_error("oom", "scan walk block"); break; }
            bool mn = false;
            for (int64_t r = lo; r < hi; ) {
                if ((err = ray_col_cursor_at(&cur[i], r, &v)) != NULL) break;
                int64_t take = v->start + v->n - r; if (take > hi - r) take = hi - r;
                memcpy((char*)buf[i] + (size_t)(r - lo) * esz, (const char*)v->data + (size_t)(r - v->start) * esz, (size_t)take * esz);
                mn |= v->may_null;
                r += take;
            }
            pc[i].data = buf[i];
            pc[i].may_null = mn;
        }
        if (!err) fn(ctx, wid, lo, hi, pc);
    } else if (!any_cursor) {
        /* Every source in place: one piece, no cursor reads. */
        for (uint32_t i = 0; i < n; i++)
            if (srcs[i]) pc[i].data = (const char*)plain[i] + (size_t)lo * ray_col_src_esz(srcs[i]);
        fn(ctx, wid, lo, hi, pc);
    } else {
        err = walk_pieces(srcs, plain, n, cur, pc, lo, hi, ctx, wid, fn);
    }
    if (any_cursor)
        for (uint32_t i = 0; i < n; i++)
            if (srcs[i] && !plain[i]) { ray_col_cursor_free(&cur[i]); ray_free_raw(buf[i]); }
    if (n > WALK_STACK) { ray_free_raw(cur); ray_free_raw(pc); ray_free_raw(buf); }
    return err;
}

void ray_scan_fail_init(ray_scan_fail_t* f) {
    memset(f, 0, sizeof *f);
    atomic_store(&f->key, INT64_MAX);
}

void ray_scan_fail_note(ray_scan_fail_t* f, int64_t key, ray_t* err) {
    while (atomic_exchange_explicit(&f->lock, 1, memory_order_acquire)) RAY_CPU_RELAX();
    if (key < atomic_load_explicit(&f->key, memory_order_relaxed)) {
        ray_enc_snap_take(&f->snap, err);          /* snapshots on this thread, frees err */
        atomic_store_explicit(&f->key, key, memory_order_release);
    } else {
        ray_error_free(err);
    }
    atomic_store_explicit(&f->lock, 0, memory_order_release);
}

ray_t* ray_scan_fail_error(ray_scan_fail_t* f) {
    if (!ray_scan_failed(f)) return NULL;
    /* The snapshot is written under the lock: copy it there so a note
     * racing a late reader cannot tear it. */
    while (atomic_exchange_explicit(&f->lock, 1, memory_order_acquire)) RAY_CPU_RELAX();
    ray_enc_snap_t snap = f->snap;
    atomic_store_explicit(&f->lock, 0, memory_order_release);
    return ray_enc_snap_error(&snap);                /* allocates outside the lock */
}

ray_t* ray_scan_fail_finish(ray_scan_fail_t* f, ray_pool_t* pool) {
    if (ray_scan_failed(f)) return ray_scan_fail_error(f);
    if (ray_interrupted() || (pool && atomic_load_explicit(&pool->cancelled, memory_order_acquire)))
        return ray_error("cancel", "scan: interrupted");
    return NULL;
}

/* ---- Directory statistics and read-ahead ---- */

bool ray_col_src_chunk_stat(const ray_col_src_t* s, int64_t c, struct ray_enc_chunk_stat_s* out) {
    if (!ray_col_src_encoded(s)) return false;
    ray_enccol_t* e = ray_enccol_of(s->obj);
    if (e->kind != RAY_ENCCOL_SINGLE || c < 0 || c >= e->n_chunks) return false;
    ray_enccol_chunk_stat(e, c, out);
    return true;
}

bool ray_col_src_vec_stat(const ray_col_src_t* s, int64_t v, int64_t* min, int64_t* max, uint32_t* n_nulls) {
    if (!ray_col_src_encoded(s)) return false;
    ray_enccol_t* e = ray_enccol_of(s->obj);
    if (e->kind != RAY_ENCCOL_SINGLE || e->synthetic || v < 0 || v * RAY_ENC_VEC >= e->len) return false;
    const ray_enc_desc_t* d;
    ray_t* err = ray_enccol_chunk_descs_ref(e, v / RAY_ENC_VECS_PER_CHUNK, &d);
    if (err) { ray_error_free(err); return false; }
    d += v % RAY_ENC_VECS_PER_CHUNK;
    if (d->flags & RAY_ENC_STAT_ABSENT) return false;
    *min = d->min; *max = d->max; *n_nulls = d->n_nulls;
    return true;
}

int ray_col_src_vview(const ray_col_src_t* s, int64_t v, ray_enc_vview_t* out, ray_t** err) {
    *err = NULL;
    if (!ray_col_src_encoded(s) || ray_enccol_decoded_peek(s->obj)) return 0;   /* read it in place */
    return ray_enccol_vview(ray_enccol_of(s->obj), v, out, err);   /* declines parted and synthetic */
}

bool ray_col_src_parted_chunks(ray_col_src_t* s, void* ctx, ray_col_chunk_fn fn, ray_t** err) {
    *err = NULL;
    if (!s || !s->obj || !s->parted) return false;
    ray_t* w = parted_wrapper(s->obj);
    if (!w) return false;
    const int64_t* off;
    if ((*err = src_seg_off(s, w, &off)) != NULL) return false;
    ray_t** segs = (ray_t**)ray_data(w);
    for (int64_t i = 0; i < w->len; i++) {
        int64_t sl = off[i + 1] - off[i];
        ray_enccol_t* e = segs[i] && segs[i]->type == RAY_ENCCOL ? ray_enccol_of(segs[i]) : NULL;
        if (e && (e->kind != RAY_ENCCOL_SINGLE || e->len != sl)) e = NULL;
        for (int64_t c = 0; c * RAY_ENC_CHUNK < sl; c++) {
            int64_t lo = off[i] + c * RAY_ENC_CHUNK;
            int64_t hi = lo + RAY_ENC_CHUNK < off[i + 1] ? lo + RAY_ENC_CHUNK : off[i + 1];
            ray_enc_chunk_stat_t st;
            if (e) ray_enccol_chunk_stat(e, c, &st);
            fn(ctx, lo, hi, e ? &st : NULL);
        }
    }
    return true;
}

int ray_col_src_point_i64(ray_col_src_t* s, int64_t row, int64_t* out, ray_t** err) {
    *err = NULL;
    if (!ray_col_src_encoded(s) || ray_enccol_decoded_peek(s->obj)) return 0;   /* the cursor reads it in place */
    ray_enccol_t* e = ray_enccol_of(s->obj);
    if (e->kind == RAY_ENCCOL_SINGLE) return ray_enccol_point_i64(e, row, out, err);
    /* parted: the row's segment, when it is a single-file encoded one */
    ray_t* w = parted_wrapper(s->obj);
    const int64_t* off;
    if (!w || w->len <= 0 || row < 0 || row >= s->len) return 0;
    ray_t* oerr = src_seg_off(s, w, &off);                  /* seg_off: published once, atomically */
    if (oerr) { ray_error_free(oerr); return 0; }          /* the cursor reports it */
    int64_t lo = 0, hi = w->len - 1;                       /* the last i with off[i] <= row */
    while (lo < hi) {
        int64_t mid = lo + (hi - lo + 1) / 2;
        if (off[mid] <= row) lo = mid; else hi = mid - 1;
    }
    ray_t* sg = ((ray_t**)ray_data(w))[lo];
    if (!sg || sg->type != RAY_ENCCOL || ray_enccol_decoded_peek(sg)) return 0;
    ray_enccol_t* se = ray_enccol_of(sg);
    return se->kind == RAY_ENCCOL_SINGLE ? ray_enccol_point_i64(se, row - off[lo], out, err) : 0;
}

/* One read-ahead span: page-aligned start, bytes. */
typedef void (*advise_emit_fn)(void* ctx, uintptr_t start, size_t len);

/* Round p down to the host page.  Every mapping starts on a page boundary
 * (mmap), so the rounded start stays inside it. */
static uintptr_t page_down(uintptr_t p) {
    return p & ~(uintptr_t)(ray_vm_page_size() - 1);
}

/* The spans of rows [lo, hi) of obj (within its rows), one per touched
 * mapping: a generation-2 column's chunk bytes from its directory, a plain
 * mapped column's element bytes, each touched segment of a parted one; a
 * heap vector has none.  A synthetic segment lives in an anonymous mapping:
 * its span is as harmless a hint as a file's. */
static void advise_spans(ray_t* obj, int64_t lo, int64_t hi, advise_emit_fn emit, void* ctx) {
    if (!obj || lo >= hi) return;
    if (obj->type == RAY_ENCCOL) {
        ray_enccol_t* e = ray_enccol_of(obj);
        if (e->kind == RAY_ENCCOL_PARTED) { advise_spans(e->wrapper, lo, hi, emit, ctx); return; }
        uint64_t off, len;
        ray_enccol_byte_range(e, lo, hi, &off, &len);
        uintptr_t p = (uintptr_t)(e->base + off), a = page_down(p);
        emit(ctx, a, (size_t)(p + len - a));
        return;
    }
    if (RAY_IS_PARTED(obj->type)) {
        ray_t** segs = (ray_t**)ray_data(obj);
        int64_t acc = 0;
        for (int64_t i = 0; i < obj->len && acc < hi; i++) {
            int64_t sl = segs[i] ? ray_table_col_nrows(segs[i]) : 0;
            int64_t a = lo > acc ? lo - acc : 0, b = hi - acc < sl ? hi - acc : sl;
            if (a < b) advise_spans(segs[i], a, b, emit, ctx);
            acc += sl;
        }
        return;
    }
    ray_t* own = plain_owner(obj);
    if (own->mmod != 1) return;                          /* heap vector: nothing to hint */
    int64_t base_row = (obj->attrs & RAY_ATTR_SLICE) ? obj->slice_offset : 0;
    uint8_t esz = ray_sym_elem_size(own->type, own->attrs);
    uintptr_t p = (uintptr_t)ray_data(own) + (uintptr_t)(base_row + lo) * esz;
    uintptr_t a = page_down(p);
    emit(ctx, a, (size_t)(p + (uintptr_t)(hi - lo) * esz - a));
}

static void advise_emit(void* ctx, uintptr_t start, size_t len) {
    (void)ctx;
    ray_vm_advise_seq((void*)start, len);
}

/* [lo, hi) clipped to the source's rows. */
static void advise_walk(const ray_col_src_t* s, int64_t lo, int64_t hi, advise_emit_fn emit, void* ctx) {
    if (!s || !s->obj) return;
    if (lo < 0) lo = 0;
    if (hi > s->len) hi = s->len;
    advise_spans(s->obj, lo, hi, emit, ctx);
}

void ray_col_src_advise(const ray_col_src_t* s, int64_t lo, int64_t hi) {
    advise_walk(s, lo, hi, advise_emit, NULL);
}

typedef struct { uintptr_t* start; size_t* len; int cap, n; } advise_collect_t;
static void advise_collect(void* ctx, uintptr_t start, size_t len) {
    advise_collect_t* c = ctx;
    if (c->n < c->cap) { c->start[c->n] = start; c->len[c->n] = len; }
    c->n++;
}

int ray_col_src_test_advise_spans(const ray_col_src_t* s, int64_t lo, int64_t hi,
                                  uintptr_t* start, size_t* len, int cap) {
    advise_collect_t c = { start, len, cap, 0 };
    advise_walk(s, lo, hi, advise_collect, &c);
    return c.n;
}
