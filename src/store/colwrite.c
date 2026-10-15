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
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
#  define _GNU_SOURCE
#endif
#include "store/col.h"
#include "store/encw.h"
#include "store/colrange.h"
#include "store/enccol.h"
#include "vec/str.h"
#include "store/fileio.h"
#include "mem/heap.h"
#include "ops/idxop.h"
#include "table/sym.h"
#include "table/domain.h"
#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* tmp -> fsync (durable) -> rename -> dir sync; removes a stale .link
 * sidecar (generation-2 columns are never linked). */
static ray_err_t colw_publish(const char* tmp, const char* path, bool durable) {
    ray_err_t e;
    if (durable) {
        ray_fd_t fd = ray_file_open(tmp, RAY_OPEN_READ | RAY_OPEN_WRITE);
        if (fd == RAY_FD_INVALID) { remove(tmp); return RAY_ERR_IO; }
        e = ray_file_sync(fd);
        ray_file_close(fd);
        if (e != RAY_OK) { remove(tmp); return e; }
    }
    if ((e = ray_file_rename(tmp, path)) != RAY_OK) { remove(tmp); return e; }
    if (durable && (e = ray_file_sync_dir(path)) != RAY_OK) return e;
    char link[1100];
    if (snprintf(link, sizeof link, "%s.link", path) < (int)sizeof link) remove(link);
    return RAY_OK;
}

/* Generation 2 holds plain vectors of BOOL..STR without a link (spec
 * "Always generation 0"), with every index kind: a SYM column's HASH or
 * PART is rebuilt over the target ids by the range writer (spec "SYM
 * columns"). */
bool ray_col_gen2_eligible(ray_t* col) {
    if (!col || RAY_IS_ERR(col)) return false;
    if (!ray_encw_type_supported(col->type)) return false;
    if (col->attrs & RAY_ATTR_HAS_LINK) return false;
    return true;
}

/* Index kind and UNIQUE flag a plain column records: every fresh kind with
 * a region or a synthesized form; DICT and a stale root record nothing. */
void ray_col_gen2_index_meta(ray_t* col, uint8_t* index_kind, uint8_t* col_flags) {
    *index_kind = 0; *col_flags = 0;
    if (!ray_index_has(col)) return;
    ray_index_t* ix = ray_index_payload(col->index);
    if (ix->built_for_len != col->len) return;                /* stale: dropped */
    if (ix->markers & RAY_MARK_UNIQUE) *col_flags |= RAY_ENC_FLAG_UNIQUE;
    switch (ix->kind) {
    case RAY_IDX_ZONE: case RAY_IDX_CHUNK_ZONE: case RAY_IDX_HASH:
    case RAY_IDX_SORT: case RAY_IDX_BLOOM: case RAY_IDX_PART:
        *index_kind = (uint8_t)ix->kind; break;
    default: break;
    }
}

/* The order name of a trustworthy root, or NULL (spec "order_sym"). */
static const char* colw_order_name(const ray_t* root, uint32_t* len) {
    *len = 0;
    int64_t s = ray_enc_index_trusted_order(root);
    if (s < 0) return NULL;
    ray_t* na = ray_sym_str(s);
    if (!na || RAY_IS_ERR(na) || ray_str_len(na) < 1 || ray_str_len(na) > RAY_ENC_IDX_NAME_MAX) return NULL;
    *len = (uint32_t)ray_str_len(na);
    return ray_str_ptr(na);
}

/* The region writer's "index region: ..." message gains the column's
 * "column %s: " prefix (the spec's message form); the code is unchanged. */
static void colw_region_msg(const char* path) {
    const char* m = ray_error_msg();
    if (!m || strncmp(m, "index region:", 13) != 0) return;
    char copy[256];
    snprintf(copy, sizeof copy, "%s", m);
    ray_error_free(ray_error("io", "column %s: %s", path, copy));   /* sets the message; code unused */
}

static ray_err_t colw_gen2_vec(ray_t* vec, const char* path, bool durable, uint8_t ik_override,
                               uint8_t ix_decision) {
    if (!ray_enc_gen2_supported()) return RAY_ERR_NYI;      /* before <path>.tmp exists */
    char tmp[1100];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return RAY_ERR_IO;
    FILE* f = fopen(tmp, "wb+");
    if (!f) return RAY_ERR_IO;
    uint8_t ik, cf;
    ray_col_gen2_index_meta(vec, &ik, &cf);
    /* a region from the root's own kind; an override never re-labels a
     * region root, and DROP writes none */
    bool region = ray_enc_idx_kind_has_region(ik) && (!ik_override || ik_override == ik) &&
                  ix_decision != RAY_COL_IDX_DROP;
    if (ik_override) ik = ik_override;
    if (ray_enc_idx_kind_has_region(ik) && !region) ik = 0;           /* no region: none recorded */
    /* the writer applies the same matrix as the validator (spec step 3) */
    if ((ik && !ray_index_kind_admits((ray_idx_kind_t)ik, vec->type)) ||
        ((cf & RAY_ENC_FLAG_UNIQUE) && !ray_index_kind_admits(RAY_IDX_NONE, vec->type))) {
        fclose(f); remove(tmp);
        return RAY_ERR_NYI;
    }
    uint32_t sc = 0;                    /* SYM never reaches here: colw_gen2_range */
    uint8_t esz = ray_sym_elem_size(vec->type, vec->attrs);
    const char* data = (const char*)ray_data(vec);          /* slice-safe */
    ray_t* owner = (vec->attrs & RAY_ATTR_SLICE) ? vec->slice_parent : vec;
    const char* pool = (vec->type == RAY_STR && owner->str_pool && !RAY_IS_ERR(owner->str_pool))
                       ? (const char*)ray_data(owner->str_pool) : NULL;
    ray_encw_t w;
    ray_err_t e = ray_encw_begin(&w, f, vec->type, vec->attrs, vec->len, sc, ik, cf);
    if (e == RAY_OK && region) {
        ray_enc_idx_src_t is = { vec->index, vec->type, vec->attrs, vec->len, NULL, 0 };
        is.order_name = colw_order_name(vec->index, &is.order_len);
        ray_encw_set_index(&w, &is);
    }
    for (int64_t c = 0; e == RAY_OK && c < w.n_chunks; c++) {
        int64_t n = vec->len - c * RAY_ENC_CHUNK;
        if (n > RAY_ENC_CHUNK) n = RAY_ENC_CHUNK;
        e = ray_encw_chunk(&w, data + (size_t)c * RAY_ENC_CHUNK * esz, pool, n);
    }
    /* colw_region_msg reads the thread-local message: clear it first, so a
     * plain io error from finish never picks up a stale "index region:" */
    if (e == RAY_OK && region) ray_error_clear();
    if (e == RAY_OK && (e = ray_encw_finish(&w)) != RAY_OK && region) colw_region_msg(path);
    ray_encw_free(&w);
    if (fclose(f) != 0 && e == RAY_OK) e = RAY_ERR_IO;
    if (e != RAY_OK) { remove(tmp); return e; }             /* nothing of this write remains */
    return colw_publish(tmp, path, durable);
}

/* Index kind / flags recorded for a source: a plain column's attached
 * index, or an encoded column's own header (every recorded kind). */
static void colw_src_index_meta(ray_t* obj, uint8_t* ik, uint8_t* cf) {
    if (obj->type == RAY_ENCCOL) {
        ray_enccol_t* e = ray_enccol_of(obj);
        *ik = e->hdr.index_kind;
        *cf = e->hdr.col_flags & RAY_ENC_FLAG_UNIQUE;
        return;
    }
    ray_col_gen2_index_meta(obj, ik, cf);
}

/* The target id of `id` (P1 Task 14's rule): checked against target_dom
 * when the source already resolves over it, re-expressed in target_dom
 * from another domain, or through the runtime LUT for a bare save of a
 * FILE-domain source.  False: the id has no target. */
static bool colw_sym_map(ray_col_write_t* w, struct ray_sym_domain_s* vdom, const int64_t* lut,
                         int64_t id, int64_t* pos) {
    *pos = id;
    if (w->target_dom && vdom == w->target_dom) {
        if (id < 0 || id >= ray_sym_domain_count(w->target_dom)) *pos = -1;
    } else if (w->target_dom) {
        ray_t* str = ray_sym_domain_str(vdom, id);
        *pos = str ? ray_sym_domain_find(w->target_dom, ray_str_ptr(str), ray_str_len(str)) : -1;
    } else if (lut) {
        *pos = (id >= 0 && id < ray_sym_domain_count(vdom)) ? lut[id] : -1;
    }
    return *pos >= 0;
}

/* A fresh root of `kind` built by the generation-0 builder over `v`
 * (owned: consumed on every path; a shared vector is copied by the
 * builder, never modified). */
static ray_t* colw_rebuild_root(ray_t* v, uint8_t kind, ray_t** root_out) {
    *root_out = NULL;
    ray_t* r = kind == RAY_IDX_HASH ? ray_index_attach_hash(&v)
             : kind == RAY_IDX_SORT ? ray_index_attach_sort(&v)
             : kind == RAY_IDX_BLOOM ? ray_index_attach_bloom(&v)
             : ray_index_attach_part(&v);
    if (!r || RAY_IS_ERR(r)) { ray_release(v); return r ? r : ray_error("oom", "index rebuild"); }
    *root_out = r->index;
    ray_retain(*root_out);
    ray_release(r);
    return NULL;
}

/* SYM: `plain` (borrowed) remapped into the target ids at the target width
 * (`attrs`), then its HASH / PART rebuilt (spec "SYM columns", "Index
 * rebuild"). */
static ray_t* colw_sym_rebuild(ray_col_write_t* w, ray_t* plain, uint8_t attrs, const int64_t* lut,
                               uint8_t kind, ray_t** root_out) {
    *root_out = NULL;
    ray_t* rm = ray_sym_vec_new(attrs & RAY_SYM_W_MASK, plain->len);
    if (!rm || RAY_IS_ERR(rm)) return rm ? rm : ray_error("oom", "index rebuild");
    rm->len = plain->len;
    rm->attrs |= (uint8_t)(attrs & RAY_ATTR_HAS_NULLS);
    struct ray_sym_domain_s* vdom = ray_sym_vec_domain(plain);
    const void* sd = ray_data(plain);
    for (int64_t i = 0; i < plain->len; i++) {
        int64_t pos;
        if (!colw_sym_map(w, vdom, lut, ray_read_sym(sd, i, RAY_SYM, plain->attrs), &pos)) {
            ray_release(rm);
            return ray_error("corrupt", "col %s: SYM id outside the target domain", w->name ? w->name : "?");
        }
        ray_write_sym(ray_data(rm), i, (uint64_t)pos, RAY_SYM, attrs);
    }
    return colw_rebuild_root(rm, kind, root_out);
}

/* The region of a range write (spec "Index decisions"), decided before the
 * file exists: a root to serialize (*root, owned: a SYM HASH / PART
 * rebuilt over the target ids, or any kind on REBUILD, built from the
 * materialized column), or an encoded non-SYM source's region to copy
 * after validation (*copy, into the source's mapping).  The order name of
 * a rebuilt root is the source root's trustworthy one, which a
 * materialization of generation 2 carries (its parsed root). */
static ray_t* colw_region_prepare(ray_col_write_t* w, uint8_t ik, uint8_t attrs, const int64_t* lut,
                                  ray_t** root, const uint8_t** copy, uint64_t* copy_len,
                                  const char** order, uint32_t* order_len) {
    ray_t* src = w->src.obj;
    bool enc = src->type == RAY_ENCCOL;
    if (enc && w->src.type != RAY_SYM && w->index_decision == RAY_COL_IDX_KEEP) {
        ray_enccol_t* se = ray_enccol_of(src);
        ray_enc_idx_view_t sv;
        ray_t* verr = ray_enc_index_validate(se->base + se->hdr.index_off, se->hdr.end_off - se->hdr.index_off,
                                             ik, se->type, se->attrs, se->len, 0, NULL, se->path, &sv);
        if (verr) return verr;
        *copy = se->base + se->hdr.index_off;
        *copy_len = se->hdr.end_off - se->hdr.index_off;
        return NULL;
    }
    /* a materialization is counted once and owned by the source */
    ray_t* plain = enc ? ray_enccol_materialize(src) : src;
    if (RAY_IS_ERR(plain)) return plain;
    if (ray_index_has(plain)) *order = colw_order_name(plain->index, order_len);
    if (w->src.type == RAY_SYM) return colw_sym_rebuild(w, plain, attrs, lut, ik, root);
    ray_retain(plain);
    return colw_rebuild_root(plain, ik, root);
}

/* Test hook: the nth allocation of the range writer's own buffers fails. */
static _Atomic int64_t g_colw_fail_at;
static void* colw_alloc(void* old, size_t n) {
    int64_t k = atomic_load(&g_colw_fail_at);
    if (k > 0 && atomic_fetch_sub(&g_colw_fail_at, 1) == 1) return NULL;
    return old ? ray_realloc_raw(old, n) : ray_alloc_raw(n);
}
void ray_col_test_fail_alloc(int64_t nth) { atomic_store(&g_colw_fail_at, nth); }

/* Generation 2 from range reads: encoded sources and every SYM source.
 * One chunk of generation-0 values is staged at a time (spec "Writers":
 * no whole-column materialization). */
static ray_err_t colw_gen2_range(ray_col_write_t* w, const char* path) {
    if (!ray_enc_gen2_supported()) return RAY_ERR_NYI;      /* before <path>.tmp exists */
    ray_col_src_t* s = &w->src;
    int8_t type = s->type;
    uint8_t attrs = s->attrs;
    uint32_t sc = 0;
    const int64_t* lut = NULL;
    if (type == RAY_SYM) {
        if (w->target_dom) {
            int64_t dc = ray_sym_domain_count(w->target_dom);
            if (dc > UINT32_MAX) return RAY_ERR_RANGE;
            uint8_t keep_sorted = (s->dom == w->target_dom) ? (attrs & RAY_ATTR_SORTED) : 0;
            attrs = (uint8_t)(ray_sym_dict_width(dc) | keep_sorted | (attrs & RAY_ATTR_HAS_NULLS));
            sc = (uint32_t)dc;
        } else {
            /* bare file: process-local runtime ids, as col_save_impl writes them */
            if (s->dom != ray_sym_runtime_domain()) {
                lut = ray_sym_domain_runtime_lut(s->dom);
                if (!lut) return RAY_ERR_OOM;
                attrs = (uint8_t)(ray_sym_dict_width((int64_t)ray_sym_count()) | (attrs & RAY_ATTR_HAS_NULLS));
            }
            sc = ray_sym_count();
        }
    }
    uint8_t src_ik, cf;
    colw_src_index_meta(s->obj, &src_ik, &cf);
    /* a region from the source's own kind (as colw_gen2_vec): an override
     * never re-labels a region kind, DROP writes none, and a region kind is
     * never recorded without its region */
    bool region = ray_enc_idx_kind_has_region(src_ik) && (!w->index_kind || w->index_kind == src_ik) &&
                  w->index_decision != RAY_COL_IDX_DROP;
    uint8_t ik = w->index_kind ? w->index_kind : src_ik;
    if (ray_enc_idx_kind_has_region(ik) && !region) ik = 0;
    if ((ik && !ray_index_kind_admits((ray_idx_kind_t)ik, type)) ||
        ((cf & RAY_ENC_FLAG_UNIQUE) && !ray_index_kind_admits(RAY_IDX_NONE, type)))
        return RAY_ERR_NYI;
    uint8_t esz = ray_sym_elem_size(type, attrs);
    char tmp[1100];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return RAY_ERR_IO;
    /* the region is decided before the file exists: a rebuilt root must be
     * whole before the chunk loop, and ray_encw_set_index borrows it */
    ray_t* idx_root = NULL;                 /* owned; released on every exit */
    const uint8_t* idx_copy = NULL; uint64_t idx_copy_len = 0;
    const char* order = NULL; uint32_t order_len = 0;
    if (region) {
        ray_t* ierr = colw_region_prepare(w, ik, attrs, lut, &idx_root, &idx_copy, &idx_copy_len, &order, &order_len);
        if (ierr) { ray_err_t ie = ray_err_from_obj(ierr); ray_error_free(ierr); return ie; }
    }
    uint8_t* buf = colw_alloc(NULL, (size_t)RAY_ENC_CHUNK * (esz ? esz : 1));
    char* pool = NULL; size_t pool_cap = 0;
    ray_col_scratch_t scr; ray_col_scratch_init(&scr);
    FILE* f = buf ? fopen(tmp, "wb+") : NULL;
    ray_err_t e = !buf ? RAY_ERR_OOM : (!f ? RAY_ERR_IO : RAY_OK);
    ray_encw_t ew; memset(&ew, 0, sizeof ew);
    if (e == RAY_OK) e = ray_encw_begin(&ew, f, type, attrs, s->len, sc, ik, cf);
    if (e == RAY_OK && idx_root) {
        /* the source attrs, as colw_gen2_vec passes them: finish writes the
         * region at the column's final persisted attrs (SYM PART keys at
         * the saved width) */
        ray_enc_idx_src_t is = { idx_root, type, s->attrs, s->len, order, order_len };
        ray_encw_set_index(&ew, &is);
    } else if (e == RAY_OK && idx_copy) {
        ray_encw_set_index_copy(&ew, ik, idx_copy, idx_copy_len);
    }
    for (int64_t c = 0; e == RAY_OK && c < ew.n_chunks; c++) {
        int64_t base = c * RAY_ENC_CHUNK;
        int64_t rows = s->len - base < RAY_ENC_CHUNK ? s->len - base : RAY_ENC_CHUNK;
        size_t pool_used = 0;
        for (int64_t r = 0; e == RAY_OK && r < rows; ) {
            ray_col_range_t v;
            ray_t* err = ray_col_range(s, base + r, rows - r, &scr, &v);
            if (err) { e = ray_err_from_obj(err); ray_error_free(err); break; }
            if (v.n <= 0) { e = RAY_ERR_CORRUPT; break; }
            if (type == RAY_SYM) {
                for (int64_t i = 0; i < v.n; i++) {
                    int64_t pos;
                    if (!colw_sym_map(w, v.dom, lut, ray_read_sym(v.data, i, RAY_SYM, v.attrs), &pos)) {
                        e = RAY_ERR_CORRUPT; break;
                    }
                    ray_write_sym(buf, r + i, (uint64_t)pos, RAY_SYM, attrs);
                }
            } else if (type == RAY_STR) {
                const ray_str_t* d = v.data;
                ray_str_t* o = (ray_str_t*)buf + r;
                for (int64_t i = 0; i < v.n; i++) {
                    o[i] = d[i];
                    if (ray_str_is_inline(&d[i])) continue;
                    if (pool_used + d[i].len > pool_cap) {
                        size_t nc = pool_cap ? pool_cap * 2 : 1 << 20;
                        while (nc < pool_used + d[i].len) nc *= 2;
                        char* np = colw_alloc(pool, nc);
                        if (!np) { e = RAY_ERR_OOM; break; }
                        pool = np; pool_cap = nc;
                    }
                    memcpy(pool + pool_used, v.str_pool + d[i].pool_off, d[i].len);
                    o[i].pool_off = (uint32_t)pool_used;     /* chunk-local pool */
                    pool_used += d[i].len;
                }
            } else {
                memcpy(buf + (size_t)r * esz, v.data, (size_t)v.n * esz);
            }
            r += v.n;
        }
        if (e == RAY_OK) e = ray_encw_chunk(&ew, buf, pool, rows);
    }
    bool has_region = idx_root || idx_copy;
    /* colw_region_msg reads the thread-local message: clear it first */
    if (e == RAY_OK && has_region) ray_error_clear();
    if (e == RAY_OK && (e = ray_encw_finish(&ew)) != RAY_OK && has_region) colw_region_msg(path);
    ray_encw_free(&ew);
    if (idx_root) ray_release(idx_root);
    ray_col_scratch_free(&scr);
    ray_free_raw(buf); ray_free_raw(pool);
    if (f && fclose(f) != 0 && e == RAY_OK) e = RAY_ERR_IO;
    if (e != RAY_OK) { if (f) remove(tmp); return e; }
    return colw_publish(tmp, path, w->durable);
}

static bool colw_src_gen2_ok(ray_t* obj) {
    if (!obj) return false;
    if (obj->type == RAY_ENCCOL) return !ray_enccol_is_parted(obj);
    return ray_col_gen2_eligible(obj);
}

ray_err_t ray_col_save_ctx(ray_col_write_t* w, const char* path) {
    ray_t* obj = w->src.obj;
    if (!obj || !path) return RAY_ERR_TYPE;
    if (w->index_decision > RAY_COL_IDX_DROP) return RAY_ERR_TYPE;
    /* parted sources have no single-file form; a later preflight admitting
     * RAY_ENCCOL must keep excluding parted kinds */
    if (RAY_IS_PARTED(obj->type) || (obj->type == RAY_ENCCOL && ray_enccol_is_parted(obj)))
        return RAY_ERR_NYI;
    if (w->gen == 2 && colw_src_gen2_ok(obj)) {
        /* direct pointers; a REBUILD goes by range, the path that holds it */
        if (obj->type != RAY_ENCCOL && obj->type != RAY_SYM && w->index_decision != RAY_COL_IDX_REBUILD)
            return colw_gen2_vec(obj, path, w->durable, w->index_kind, w->index_decision);
        return colw_gen2_range(w, path);
    }
    if (w->gen != 0 && w->gen != 2) return RAY_ERR_VERSION;
    /* generation 0 writes its index as today: it takes no other decision */
    if (w->index_decision != RAY_COL_IDX_KEEP) return RAY_ERR_NYI;
    ray_t* col = obj;
    if (obj->type == RAY_ENCCOL) {
        col = ray_enccol_materialize(obj);          /* the object's own once-state */
        if (RAY_IS_ERR(col)) { ray_err_t e = ray_err_from_obj(col); ray_error_free(col); return e; }
    }
    if (col->type == RAY_SYM && w->target_dom)
        return ray_col_save_sym_encoded(col, path, w->target_dom, w->durable);
    return w->durable ? ray_col_save(col, path) : ray_col_save_bulk(col, path);
}

static ray_err_t colw_save_gen(ray_t* vec, const char* path, uint8_t gen, bool durable) {
    if (!vec || RAY_IS_ERR(vec)) return RAY_ERR_TYPE;
    if (!path) return RAY_ERR_IO;
    ray_col_write_t w; memset(&w, 0, sizeof w);
    ray_t* err = ray_col_src_bind(vec, &w.src);
    if (err) { ray_err_t be = ray_err_from_obj(err); ray_error_free(err); return be; }
    w.gen = gen; w.durable = durable;
    ray_err_t e = ray_col_save_ctx(&w, path);
    ray_col_src_release(&w.src);
    return e;
}

ray_err_t ray_col_save_gen(ray_t* vec, const char* path, uint8_t gen) {
    return colw_save_gen(vec, path, gen, true);
}
ray_err_t ray_col_save_bulk_gen(ray_t* vec, const char* path, uint8_t gen) {
    return colw_save_gen(vec, path, gen, false);
}

/* Equivalence-run hook: an absent option behaves as true. */
static _Atomic(bool) g_force_compress;
void ray_store_test_force_compress(bool on) { atomic_store(&g_force_compress, on); }

/* A missing file is generation 0; any other failure to read the header
 * (permissions, a short file, a bad discriminator) is an error, so an
 * absent option never silently drops a destination's generation. */
ray_err_t ray_col_file_gen_err(const char* path, uint8_t* gen) {
    *gen = 0;
    if (!path) return RAY_ERR_IO;
    errno = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? RAY_OK : RAY_ERR_IO;
    uint8_t h[32];
    size_t n = fread(h, 1, 32, f);
    bool rd_err = ferror(f) != 0;
    fclose(f);
    if (rd_err) return RAY_ERR_IO;
    if (ray_col_header_is_ext(h, n)) return RAY_OK;       /* list / string formats */
    if (n != 32) return RAY_ERR_CORRUPT;
    int g = ray_col_gen_of(h);
    if (g == RAY_ENC_GEN_VERSION) return RAY_ERR_VERSION;
    if (g < 0) return RAY_ERR_CORRUPT;
    *gen = g == 2 ? 2 : 0;
    return RAY_OK;
}

uint8_t ray_col_file_gen(const char* path) {
    uint8_t g;
    return ray_col_file_gen_err(path, &g) == RAY_OK ? g : 0;
}

/* The generation the destination's current generation stores `name` in. */
static ray_err_t colw_dest_gen(const char* cur, const char* name, size_t nlen, uint8_t* gen) {
    *gen = 0;
    if (!cur || !cur[0]) return RAY_OK;                    /* no published table */
    if (!name) return RAY_ERR_TYPE;
    char p[1200];
    int n = snprintf(p, sizeof p, "%s/%.*s", cur, (int)nlen, name);
    if (n < 0 || (size_t)n >= sizeof p) return RAY_ERR_RANGE;
    return ray_col_file_gen_err(p, gen);
}

ray_err_t ray_col_resolve_gen_err(ray_compress_t mode, ray_t* src, const char* cur,
                                  const char* name, size_t nlen, uint8_t* gen) {
    *gen = 0;
    if (!colw_src_gen2_ok(src) || mode == RAY_COMPRESS_OFF) return RAY_OK;
    *gen = 2;
    if (mode == RAY_COMPRESS_ON || atomic_load(&g_force_compress)) return RAY_OK;
    /* the slot still holds a column saved encoded (a synthetic stand-in,
     * e.g. a ragged partition's absent column, inherits like a plain one) */
    if (src->type == RAY_ENCCOL && !ray_enccol_is_synthetic(src)) return RAY_OK;
    return colw_dest_gen(cur, name, nlen, gen);            /* the destination already stores it as 2 */
}

bool ray_col_resolve_needs_dest(ray_compress_t mode, ray_t* src) {
    return mode == RAY_COMPRESS_ABSENT && colw_src_gen2_ok(src) &&
           !atomic_load(&g_force_compress) &&
           (src->type != RAY_ENCCOL || ray_enccol_is_synthetic(src));
}

bool ray_col_resolve_type_needs_dest(ray_compress_t mode, int8_t type) {
    return mode == RAY_COMPRESS_ABSENT && ray_encw_type_supported(type) &&
           !atomic_load(&g_force_compress);
}

ray_err_t ray_col_resolve_gen_type_err(ray_compress_t mode, int8_t type, const char* cur,
                                       const char* name, size_t nlen, uint8_t* gen) {
    *gen = 0;
    if (!ray_encw_type_supported(type) || mode == RAY_COMPRESS_OFF) return RAY_OK;
    *gen = 2;
    if (mode == RAY_COMPRESS_ON || atomic_load(&g_force_compress)) return RAY_OK;
    return colw_dest_gen(cur, name, nlen, gen);
}

uint8_t ray_col_resolve_gen(ray_compress_t mode, ray_t* src,
                            const char* cur, const char* name, size_t nlen) {
    uint8_t g;
    return ray_col_resolve_gen_err(mode, src, cur, name, nlen, &g) == RAY_OK ? g : 0;
}

uint8_t ray_col_resolve_gen_type(ray_compress_t mode, int8_t type,
                                 const char* cur, const char* name, size_t nlen) {
    uint8_t g;
    return ray_col_resolve_gen_type_err(mode, type, cur, name, nlen, &g) == RAY_OK ? g : 0;
}

ray_t* ray_compress_opt_parse(ray_t* opts, bool strict, ray_compress_t* mode) {
    *mode = RAY_COMPRESS_ABSENT;
    if (!opts) return NULL;
    if (opts->type != RAY_DICT) return ray_error("type", "options must be a dict");
    ray_t* keys = ray_dict_keys(opts);
    if (keys && keys->len > 0 && keys->type != RAY_SYM)
        return ray_error("domain", "option keys must be symbols");
    for (int64_t i = 0; strict && keys && i < keys->len; i++) {
        ray_t* k = ray_sym_vec_cell(keys, i);
        if (!k || ray_str_len(k) != 8 || memcmp(ray_str_ptr(k), "compress", 8) != 0)
            return ray_error("domain", "unknown option '%.*s' (only compress is accepted)",
                             k ? (int)ray_str_len(k) : 1, k ? ray_str_ptr(k) : "?");
    }
    ray_t* key = ray_sym(ray_sym_intern("compress", 8));
    if (!key || RAY_IS_ERR(key)) return key ? key : ray_error("oom", NULL);
    ray_t* v = ray_dict_get(opts, key);
    ray_release(key);
    if (!v) return NULL;
    if (RAY_IS_ERR(v)) return v;
    bool ok = v->type == -RAY_BOOL;
    if (ok) *mode = v->b8 ? RAY_COMPRESS_ON : RAY_COMPRESS_OFF;
    ray_release(v);
    return ok ? NULL : ray_error("domain", "compress must be true or false");
}
