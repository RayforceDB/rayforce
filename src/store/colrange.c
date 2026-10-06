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
#include "store/col.h"
#include "store/enccol.h"
#include "table/sym.h"
#include "table/domain.h"
#include "table/table.h"
#include "ops/ops.h"
#include "vec/vec.h"
#include <string.h>

/* Fixed scratch: one vector of the widest element (GUID, 16 bytes). */
#define SCRATCH_FIXED_BYTES ((size_t)RAY_ENC_VEC * 16)

void ray_col_scratch_init(ray_col_scratch_t* s) { memset(s, 0, sizeof *s); }
void ray_col_scratch_free(ray_col_scratch_t* s) {
    ray_free_raw(s->fixed);
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

static ray_t* range_enc(ray_enccol_t* e, int64_t start, int64_t n,
                        ray_col_scratch_t* sc, ray_col_range_t* out) {
    int64_t vec = start / RAY_ENC_VEC;
    int64_t off = start % RAY_ENC_VEC;
    int64_t vlen = ray_enc_vec_len(e->len, vec);
    if (n > vlen - off) n = vlen - off;
    if (!sc->fixed) {
        sc->fixed = ray_alloc_raw(SCRATCH_FIXED_BYTES);
        if (!sc->fixed) return ray_error("oom", "range read scratch");
    }
    int64_t c = vec / RAY_ENC_VECS_PER_CHUNK;
    int j = (int)(vec % RAY_ENC_VECS_PER_CHUNK);
    ray_enc_desc_t descs[RAY_ENC_VECS_PER_CHUNK];
    ray_t* err = ray_enccol_chunk_descs(e, c, descs);       /* validated once per vector */
    if (err) return err;
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
    err = ray_enccol_decode_desc(e, c, j, &descs[j], sc->fixed, sc->pool, 0, sc->pool_cap);
    if (err) return err;
    out->data = sc->fixed + (size_t)off * ray_enccol_esz(e);
    out->n = n;
    out->type = e->type;
    out->attrs = e->attrs;
    out->may_null = descs[j].n_nulls > 0;
    out->str_pool = e->type == RAY_STR ? sc->pool : NULL;
    out->dom = e->type == RAY_SYM ? e->dom : NULL;
    return NULL;
}

static ray_t* range_obj(ray_t* obj, int64_t start, int64_t n,
                        ray_col_scratch_t* sc, ray_col_range_t* out) {
    if (obj->type == RAY_ENCCOL) {
        ray_enccol_t* e = ray_enccol_of(obj);
        if (e->kind == RAY_ENCCOL_SINGLE) return range_enc(e, start, n, sc, out);
        obj = e->wrapper;
    }
    if (RAY_IS_PARTED(obj->type)) {
        ray_t** segs = (ray_t**)ray_data(obj);
        int64_t acc = 0;
        for (int64_t i = 0; i < obj->len; i++) {
            int64_t sl = segs[i] ? ray_table_col_nrows(segs[i]) : 0;   /* missing: no rows */
            if (start < acc + sl) {
                int64_t local = start - acc;
                if (n > sl - local) n = sl - local;          /* never cross a segment */
                return range_obj(segs[i], local, n, sc, out);
            }
            acc += sl;
        }
        return ray_error("range", "range read past the last segment");
    }
    range_plain(obj, start, n, out);
    return NULL;
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
    ray_t* err = range_obj(src->obj, start, n, scratch, out);
    out->start = start;
    return err;
}
