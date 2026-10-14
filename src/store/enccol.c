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
#include "store/enccol.h"
#include "store/col.h"
#include "store/encw.h"          /* ray_encw_type_supported / _summable / _has_dict */
#include "ops/idxop.h"            /* ray_index_kind_admits, ray_idx_lazy_forget */
#include "lang/format.h"          /* ray_type_name */
#include "core/platform.h"
#include "mem/sys.h"             /* ray_sys_alloc: the map descriptor, as col.c */
#include "table/sym.h"
#include "table/domain.h"
#include "vec/vec.h"              /* ray_vec_has_nulls */
#include "store/enc.h"
#include "store/colrange.h"     /* RAY_COL_POINT_ROWS: the most rows a caller hands one point read */
#include "vec/str.h"
#include "core/pool.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

ray_enccol_t* ray_enccol_of(ray_t* v) {
    if (!v || RAY_IS_ERR(v) || v->type != RAY_ENCCOL) return NULL;
    ray_enccol_t* e;
    memcpy(&e, v->data, sizeof e);
    return e;
}

static _Atomic(int64_t) g_enccol_live;          /* test hook, see enccol.h */
int64_t ray_enccol_test_live(void) { return atomic_load(&g_enccol_live); }

static void enccol_free(ray_enccol_t* e) {
    atomic_fetch_sub_explicit(&g_enccol_live, 1, memory_order_relaxed);
    ray_idx_lazy_forget_enc(e);              /* a pending probe count goes with the payload */
    if (e->decoded) {
        ray_idx_lazy_forget(e->decoded);     /* a lazy hash goes with its object */
        ray_release(e->decoded);
    }
    if (e->dict)    ray_release(e->dict);
    if (e->wrapper) ray_release(e->wrapper);
    if (e->dom && e->dom_owned) ray_sym_domain_release(e->dom);
    if (e->map)     ray_file_map_release(e->map);
    ray_free_raw((void*)e->desc_ok);
    ray_free_raw((void*)e->raw_ok);
    ray_free_raw((void*)e->dir_vec_ok);
    ray_free_raw(e);
}

void ray_enccol_ref(ray_enccol_t* e) { atomic_fetch_add_explicit(&e->rc, 1, memory_order_relaxed); }
void ray_enccol_unref(ray_enccol_t* e) {
    if (atomic_fetch_sub_explicit(&e->rc, 1, memory_order_acq_rel) == 1) enccol_free(e);
}

void ray_enccol_payload_retain(ray_t* v) {
    ray_enccol_t* e = ray_enccol_of(v);
    if (e) atomic_fetch_add_explicit(&e->rc, 1, memory_order_relaxed);
}

void ray_enccol_payload_release(ray_t* v) {
    ray_enccol_t* e = ray_enccol_of(v);
    if (!e) return;
    memset(v->data, 0, sizeof e);
    if (atomic_fetch_sub_explicit(&e->rc, 1, memory_order_acq_rel) == 1) enccol_free(e);
}

void ray_enccol_payload_detach(ray_t* v) {
    if (v && v->type == RAY_ENCCOL) memset(v->data, 0, sizeof(ray_enccol_t*));
}

size_t ray_enccol_own_bytes(ray_t* v) {
    ray_enccol_t* e = ray_enccol_of(v);
    return (e && e->map) ? e->map->len : 0;
}

int ray_enccol_children(ray_t* v, ray_t* out[2]) {
    ray_enccol_t* e = ray_enccol_of(v);
    int n = 0;
    if (!e) return 0;
    if (e->decoded) out[n++] = e->decoded;
    if (e->wrapper) out[n++] = e->wrapper;
    return n;
}

ray_t* ray_enccol_wrap(ray_enccol_t* e) {
    ray_t* v = ray_alloc(sizeof(ray_enccol_t*));
    if (!v || RAY_IS_ERR(v)) return v ? v : ray_error("oom", NULL);
    v->type  = RAY_ENCCOL;
    v->attrs = 0;
    v->len   = e->len;
    memset(v->aux, 0, 16);
    memcpy(v->data, &e, sizeof e);
    return v;
}

static bool enc_type_nullable(int8_t t)  { return t != RAY_BOOL && t != RAY_U8; }
static bool enc_type_float(int8_t t)     { return t == RAY_F32 || t == RAY_F64; }
/* An integer statistic v inside the logical range of kind k (the lane
 * width and signedness: BOOL / U8 8 unsigned, I16 / I32 / I64 / DATE /
 * TIME / TIMESTAMP signed, SYM ids unsigned at the column's id width). */
static bool stat_in_kind(ray_enc_kind_t k, int64_t v) {
    if (k.T == 64) return k.is_signed || v >= 0;
    if (k.is_signed) { const int64_t h = (int64_t)1 << (k.T - 1); return v >= -h && v < h; }
    return v >= 0 && v < ((int64_t)1 << k.T);
}

static bool all_zero(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) if (p[i]) return false;
    return true;
}

#define ENC_CORRUPT(...) return ray_error("corrupt", __VA_ARGS__)

static uint64_t rd_u64(const uint8_t* p) { uint64_t x; memcpy(&x, p, 8); return x; }

/* Side table structure (spec "Side data"), checked at load: entry count,
 * reserved fields, kinds (at most one of each), 32-aligned non-overlapping
 * entries in order, and zero bytes in every gap and in the tail.  Entry
 * contents (VALUE_DICT ordering) are checked once, before first use
 * (Task 7). */
static ray_t* enc_validate_side_table(ray_enccol_t* e) {
    const uint8_t* s = e->base + e->hdr.side_off;
    uint64_t sl = e->hdr.side_len;
    uint32_t ne, rsv; memcpy(&ne, s, 4); memcpy(&rsv, s + 4, 4);
    if (rsv) ENC_CORRUPT("column %s: side table reserved word set", e->path);
    if (ne == 0 || ne > 3) ENC_CORRUPT("column %s: side table entry count %u", e->path, ne);
    if (8 + 24ull * ne > sl) ENC_CORRUPT("column %s: side table longer than the side data", e->path);
    uint64_t at = 8 + 24ull * ne;              /* end of the table */
    bool seen[4] = { false, false, false, false };
    for (uint32_t i = 0; i < ne; i++) {
        const uint8_t* t = s + 8 + 24 * i;
        uint16_t kind, r16; uint32_t r32;
        memcpy(&kind, t, 2); memcpy(&r16, t + 2, 2); memcpy(&r32, t + 4, 4);
        uint64_t off = rd_u64(t + 8), len = rd_u64(t + 16);
        if (r16 || r32) ENC_CORRUPT("column %s: side entry %u reserved fields set", e->path, i);
        if (kind < RAY_ENC_SIDE_VALUE_DICT || kind > RAY_ENC_SIDE_FSST_TABLE)
            ENC_CORRUPT("column %s: side entry %u unknown kind %u", e->path, i, kind);
        if (seen[kind]) ENC_CORRUPT("column %s: side entry %u duplicate kind %u", e->path, i, kind);
        seen[kind] = true;
        if ((off & 31) || off != ray_enc_pad32(at))
            ENC_CORRUPT("column %s: side entry %u offset not the next 32-aligned position", e->path, i);
        if (len < 8) ENC_CORRUPT("column %s: side entry %u length %llu", e->path, i, (unsigned long long)len);
        if (len > sl || off > sl - len) ENC_CORRUPT("column %s: side entry %u overruns the side data", e->path, i);
        if (!all_zero(s + at, (size_t)(off - at))) ENC_CORRUPT("column %s: side padding", e->path);
        if (kind == RAY_ENC_SIDE_VALUE_DICT) {
            /* float dictionaries are legal from P5 (spec "Type coverage"):
             * a newer build's file, not damage */
            if (e->type == RAY_F32 || e->type == RAY_F64)
                return ray_error("nyi", "column %s: float dictionary needs a newer build", e->path);
            if (!ray_encw_type_has_dict(e->type)) ENC_CORRUPT("column %s: VALUE_DICT on type %s", e->path, ray_type_name(e->type));
            e->vdict_off = e->hdr.side_off + off; e->vdict_len = len;
        } else if (e->type != RAY_STR) {
            ENC_CORRUPT("column %s: string side entry on type %s", e->path, ray_type_name(e->type));
        }
        at = off + len;
    }
    if (at != sl && !all_zero(s + at, (size_t)(sl - at))) ENC_CORRUPT("column %s: side tail", e->path);
    if (ray_enc_pad32(at) < sl) ENC_CORRUPT("column %s: side tail longer than padding", e->path);
    return NULL;
}

static ray_t* enc_validate_load(ray_enccol_t* e, uint32_t* saved_sym) {
    const uint8_t* b = e->base;
    uint64_t size = e->size;
    if (size < RAY_ENC_DIR_OFF) ENC_CORRUPT("column %s: generation-2 file shorter than its headers", e->path);
    ray_t h; memcpy(&h, b, 32);
    if (!all_zero(b + 4, 12)) ENC_CORRUPT("column %s: reserved header bytes set", e->path);
    if (h.mmod != 0) ENC_CORRUPT("column %s: header mmod byte set", e->path);
    if (!ray_encw_type_supported(h.type)) ENC_CORRUPT("column %s: type %d not storable in generation 2", e->path, (int)h.type);
    if (h.attrs & ~COL_DISK_ATTRS_MASK) ENC_CORRUPT("column %s: attrs 0x%02x outside the persisted set", e->path, h.attrs);
    if (h.type != RAY_SYM && (h.attrs & RAY_SYM_W_MASK)) ENC_CORRUPT("column %s: width bits on a non-SYM column", e->path);
    if (h.len < 0) ENC_CORRUPT("column %s: negative length", e->path);
    uint32_t rc; memcpy(&rc, b + offsetof(ray_t, rc), 4);
    if (h.type != RAY_SYM && rc != 0) ENC_CORRUPT("column %s: SYM count on a non-SYM column", e->path);
    *saved_sym = rc;
    e->type = h.type; e->attrs = h.attrs; e->len = h.len;

    ray_enc_hdr_t* eh = &e->hdr;
    memcpy(eh, b + RAY_ENC_HDR_OFF, sizeof *eh);
    if (memcmp(eh->magic, "RENC", 4) != 0) ENC_CORRUPT("column %s: missing RENC header", e->path);
    if (eh->header_version != RAY_ENC_HDR_VERSION)
        return ray_error("version", "column %s: generation-2 header version %u", e->path, eh->header_version);
    if (eh->index_kind > 6) ENC_CORRUPT("column %s: index kind %u", e->path, eh->index_kind);
    /* the admission matrix, shared with the writer and the attach (spec
     * "Materialization" step 3): a pair it refuses is a wire violation */
    if (eh->index_kind && !ray_index_kind_admits((ray_idx_kind_t)eh->index_kind, h.type))
        ENC_CORRUPT("column %s: index kind %u not admitted on type %s", e->path, eh->index_kind, ray_type_name(h.type));
    if ((eh->col_flags & RAY_ENC_FLAG_UNIQUE) && !ray_index_kind_admits(RAY_IDX_NONE, h.type))
        ENC_CORRUPT("column %s: UNIQUE not admitted on type %s", e->path, ray_type_name(h.type));
    if (eh->col_flags & ~RAY_ENC_FLAG_UNIQUE) ENC_CORRUPT("column %s: reserved column flags", e->path);
    if (eh->rsv0 || eh->rsv1 || eh->rsv2 || eh->rsv3) ENC_CORRUPT("column %s: reserved enc header fields set", e->path);
    if (eh->str_pool_bytes > UINT32_MAX)
        ENC_CORRUPT("column %s: str_pool_bytes %llu above 4 GiB", e->path, (unsigned long long)eh->str_pool_bytes);
    if (h.type != RAY_STR && eh->str_pool_bytes)
        ENC_CORRUPT("column %s: str_pool_bytes %llu on a non-STR column", e->path, (unsigned long long)eh->str_pool_bytes);
    if (eh->end_off != size)
        ENC_CORRUPT("column %s: end offset %llu, file is %llu bytes (truncated?)", e->path,
                    (unsigned long long)eh->end_off, (unsigned long long)size);

    /* the directory must fit: bound len before deriving the chunk count */
    if ((uint64_t)h.len > ((size - RAY_ENC_DIR_OFF) / sizeof(ray_enc_dirent_t)) * (uint64_t)RAY_ENC_CHUNK)
        ENC_CORRUPT("column %s: length %lld exceeds what the file holds", e->path, (long long)h.len);
    e->n_chunks = ray_enc_nchunks(h.len);
    if ((uint64_t)e->n_chunks > (size - RAY_ENC_DIR_OFF) / sizeof(ray_enc_dirent_t))
        ENC_CORRUPT("column %s: directory past the end of the file", e->path);
    uint64_t prev = RAY_ENC_DIR_OFF + (uint64_t)e->n_chunks * sizeof(ray_enc_dirent_t);
    bool summable = ray_encw_type_summable(h.type);
    ray_enc_kind_t kind;
    const bool have_kind = ray_enc_kind_of(h.type, h.attrs, &kind);
    e->null_count = 0;
    for (int64_t c = 0; c < e->n_chunks; c++) {
        ray_enc_dirent_t de;
        memcpy(&de, b + RAY_ENC_DIR_OFF + c * sizeof de, sizeof de);
        int64_t rows = h.len - c * RAY_ENC_CHUNK;
        if (rows > RAY_ENC_CHUNK) rows = RAY_ENC_CHUNK;
        if (de.offset != prev) ENC_CORRUPT("column %s: chunk %lld at %llu, expected %llu", e->path,
                                           (long long)c, (unsigned long long)de.offset, (unsigned long long)prev);
        if (de.bytes < RAY_ENC_DESC_AREA)
            ENC_CORRUPT("column %s: chunk %lld size %llu below the descriptor area", e->path, (long long)c, (unsigned long long)de.bytes);
        if (de.bytes & 7) ENC_CORRUPT("column %s: chunk %lld size %llu not a multiple of 8", e->path, (long long)c, (unsigned long long)de.bytes);
        if (de.bytes > size - de.offset) ENC_CORRUPT("column %s: chunk %lld size %llu overruns the file", e->path, (long long)c, (unsigned long long)de.bytes);
        if (de.rsv0 || de.rsv1) ENC_CORRUPT("column %s: chunk %lld reserved directory words set", e->path, (long long)c);
        if (de.flags & ~RAY_ENC_STAT_ABSENT) ENC_CORRUPT("column %s: chunk %lld reserved directory flag bits", e->path, (long long)c);
        if ((int64_t)de.null_count > rows) ENC_CORRUPT("column %s: chunk %lld null count %u over %lld rows",
                                                      e->path, (long long)c, de.null_count, (long long)rows);
        if (!enc_type_nullable(h.type) && de.null_count) ENC_CORRUPT("column %s: nulls in a non-nullable column", e->path);
        bool absent = de.flags & RAY_ENC_STAT_ABSENT;
        if ((int64_t)de.null_count == rows && !absent)
            ENC_CORRUPT("column %s: chunk %lld statistics on an all-null chunk", e->path, (long long)c);
        if (h.type == RAY_GUID && !absent)
            ENC_CORRUPT("column %s: chunk %lld statistics on GUID", e->path, (long long)c);
        /* STR code bounds exist exactly when every vector is STR_DICT (P6):
         * a valid newer file, not a damaged one. */
        if (h.type == RAY_STR && !absent)
            return ray_error("nyi", "column %s: STR dictionary statistics need a newer build", e->path);
        if (absent && (de.min || de.max)) ENC_CORRUPT("column %s: chunk %lld absent statistics not zero", e->path, (long long)c);
        if (!absent) {
            if (enc_type_float(h.type)) {
                double mn, mx; memcpy(&mn, &de.min, 8); memcpy(&mx, &de.max, 8);
                if (isnan(mn) || isnan(mx) || mn > mx) ENC_CORRUPT("column %s: chunk %lld float bounds", e->path, (long long)c);
            } else if (de.min > de.max) {
                ENC_CORRUPT("column %s: chunk %lld min above max", e->path, (long long)c);
            } else if (have_kind && (!stat_in_kind(kind, de.min) || !stat_in_kind(kind, de.max))) {
                /* consumers take spans and slot counts from these: a value
                 * outside the type is damage, never a wider range */
                ENC_CORRUPT("column %s: chunk %lld statistics outside the type", e->path, (long long)c);
            }
        }
        if (!summable && (de.sum_lo || de.sum_hi)) ENC_CORRUPT("column %s: chunk %lld sum on an unsummed type", e->path, (long long)c);
        if ((int64_t)de.null_count == rows && (de.sum_lo || de.sum_hi)) ENC_CORRUPT("column %s: chunk %lld sum of no values", e->path, (long long)c);
        e->null_count += de.null_count;
        prev = de.offset + de.bytes;
    }
    if (eh->side_off) {
        if (eh->side_off != ray_enc_pad32(prev)) ENC_CORRUPT("column %s: side data not at the next 32-aligned offset", e->path);
        if (eh->side_len < 8) ENC_CORRUPT("column %s: side data length %llu", e->path, (unsigned long long)eh->side_len);
        if (eh->side_off > size || eh->side_len > size - eh->side_off) ENC_CORRUPT("column %s: side data overruns the file", e->path);
        if (!all_zero(b + prev, (size_t)(eh->side_off - prev))) ENC_CORRUPT("column %s: padding before the side data not zero", e->path);
        ray_t* serr = enc_validate_side_table(e);
        if (serr) return serr;
        prev = eh->side_off + eh->side_len;
    } else if (eh->side_len) {
        ENC_CORRUPT("column %s: side length without side data", e->path);
    }
    bool want_index = eh->index_kind == 1 || eh->index_kind == 2 ||
                      eh->index_kind == 4 || eh->index_kind == 6;
    if (want_index != (eh->index_off != 0)) ENC_CORRUPT("column %s: index kind %u disagrees with index offset", e->path, eh->index_kind);
    if (eh->index_off) {
        if (eh->index_off != ray_enc_pad32(prev)) ENC_CORRUPT("column %s: index region not at the next 32-aligned offset", e->path);
        if (eh->index_off >= size) ENC_CORRUPT("column %s: index region past the end of the file", e->path);
        if (!all_zero(b + prev, (size_t)(eh->index_off - prev))) ENC_CORRUPT("column %s: padding before the index region not zero", e->path);
        /* the region opens with the 80-byte wire root (spec "Index region") */
        if (size - eh->index_off < 80) ENC_CORRUPT("column %s: index region shorter than its 80-byte root", e->path);
        prev = size;
    }
    if (prev != size) ENC_CORRUPT("column %s: %llu bytes after the last section", e->path, (unsigned long long)(size - prev));
    return NULL;
}

ray_t* ray_enccol_open(void* mapped, size_t size, const char* path,
                       struct ray_sym_domain_s* dom, bool require_dom) {
    /* Every validator below is portable C; only this gate keeps a
     * big-endian host from reading the little-endian layout. */
    if (!ray_enc_gen2_supported()) {
        ray_vm_unmap_file(mapped, size);
        return ray_error("nyi", "column %s: generation 2 needs a little-endian host", path);
    }
    ray_file_map_t* m = (ray_file_map_t*)ray_sys_alloc(sizeof(*m));
    if (!m) { ray_vm_unmap_file(mapped, size); return ray_error("oom", NULL); }
    m->base = mapped; m->len = size; m->rc = 1; m->next = NULL;
    ray_enccol_t* e = ray_calloc_raw(sizeof *e);
    if (!e) { ray_file_map_release(m); return ray_error("oom", NULL); }
    atomic_fetch_add_explicit(&g_enccol_live, 1, memory_order_relaxed);
    atomic_store(&e->rc, 1);
    e->kind = RAY_ENCCOL_SINGLE;
    e->map = m; e->base = (const uint8_t*)mapped; e->size = size;
    snprintf(e->path, sizeof e->path, "%s", path ? path : "?");
    ray_enc_once_init(&e->once);
    ray_enc_once_init(&e->dict_once);
    uint32_t saved = 0;
    /* Descriptors and packed bodies are read in place as 8-byte words: the
     * format keeps every offset 8-aligned, and a mapping starts on a page. */
    ray_t* err = ((uintptr_t)mapped & 7) ? ray_error("nyi", "column %s: mapping not 8-aligned", e->path)
                                         : enc_validate_load(e, &saved);
    if (!err && e->n_chunks > 0) {
        e->desc_ok = ray_calloc_raw((size_t)e->n_chunks);
        if (!e->desc_ok) err = ray_error("oom", "column %s: chunk flags", e->path);
        if (!err && e->type != RAY_GUID && e->type != RAY_STR) {
            e->raw_ok = ray_calloc_raw((size_t)e->n_chunks);
            if (!e->raw_ok) err = ray_error("oom", "column %s: chunk flags", e->path);
        }
        if (!err) {
            e->dir_vec_ok = ray_calloc_raw((size_t)e->n_chunks * sizeof(uint64_t));
            if (!e->dir_vec_ok) err = ray_error("oom", "column %s: chunk flags", e->path);
        }
    }
    if (!err && e->type == RAY_SYM) {
        if (dom) {
            int64_t dcount = ray_sym_domain_count(dom);
            if (saved > 0 && (int64_t)saved > dcount)
                err = ray_error("corrupt", "column %s: saved sym count %u exceeds symfile count %lld",
                                e->path, saved, (long long)dcount);
            e->sym_bound = saved ? saved : (dcount > UINT32_MAX ? UINT32_MAX : (uint32_t)dcount);
            e->dom = dom; ray_sym_domain_retain(dom); e->dom_owned = true;
        } else if (require_dom) {
            err = ray_error("sym", "column %s: SYM data but no symfile resolved "
                                   "(missing <dir>/sym or explicit sym argument)", e->path);
        } else {
            uint32_t cur = ray_sym_count();
            if (saved > 0 && cur > 0 && cur < saved) err = ray_error("corrupt", "column %s: saved sym count %u exceeds %u", e->path, saved, cur);
            e->sym_bound = saved ? saved : cur;
            e->dom = ray_sym_runtime_domain(); e->dom_owned = false;
        }
        /* Directory id bounds are below the bound (stats exclude id 0). */
        for (int64_t c = 0; !err && c < e->n_chunks; c++) {
            ray_enc_dirent_t de;
            memcpy(&de, e->base + RAY_ENC_DIR_OFF + c * sizeof de, sizeof de);
            if (!(de.flags & RAY_ENC_STAT_ABSENT) && (de.min < 1 || (uint64_t)de.max >= e->sym_bound))
                err = ray_error("corrupt", "column %s: chunk %lld SYM ids outside the symbol domain", e->path, (long long)c);
        }
    }
    if (err) { enccol_free(e); return err; }
    ray_t* v = ray_enccol_wrap(e);
    if (!v || RAY_IS_ERR(v)) enccol_free(e);
    return v;
}

static _Atomic(uint64_t) g_enc_materialized;
static _Atomic(uint64_t) g_enc_decoded_bytes;
static _Atomic(uint64_t) g_enc_at_first_exec = UINT64_MAX;
void ray_enc_note_exec_start(void) {
    uint64_t want = UINT64_MAX;
    atomic_compare_exchange_strong(&g_enc_at_first_exec, &want, atomic_load(&g_enc_materialized));
}
void ray_enc_counters_get(ray_enc_counters_t* o) {
    o->at_first_exec = atomic_load(&g_enc_at_first_exec);
    o->materialized  = atomic_load(&g_enc_materialized);
    o->decoded_bytes = atomic_load(&g_enc_decoded_bytes);
}
void ray_enc_counters_reset(void) {
    atomic_store(&g_enc_materialized, 0);
    atomic_store(&g_enc_decoded_bytes, 0);
    atomic_store(&g_enc_at_first_exec, UINT64_MAX);
}

uint8_t ray_enccol_esz(const ray_enccol_t* e) { return ray_sym_elem_size(e->type, e->attrs); }

static int64_t chunk_rows(const ray_enccol_t* e, int64_t c) {
    int64_t r = e->len - c * RAY_ENC_CHUNK;
    return r > RAY_ENC_CHUNK ? RAY_ENC_CHUNK : r;
}

static void dirent_at(const ray_enccol_t* e, int64_t c, ray_enc_dirent_t* de) {
    memcpy(de, e->base + RAY_ENC_DIR_OFF + c * sizeof *de, sizeof *de);
}

static int lane_width(const ray_enccol_t* e) { return 8 * (int)ray_enccol_esz(e); }

/* 1 legal and implemented, 2 legal by the spec but not in this build
 * (ALP, ALP_RD, float RLE/DICT: P5; STR_DICT, FSST: P6), 0 illegal. */
static int top_encoding_class(int8_t t, uint8_t enc) {
    switch (t) {
    case RAY_BOOL: case RAY_U8:
        return (enc == RAY_ENC_RAW || enc == RAY_ENC_CONST || enc == RAY_ENC_FOR_BP || enc == RAY_ENC_RLE) ? 1 : 0;
    case RAY_I16: case RAY_I32: case RAY_I64: case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP:
        return enc <= RAY_ENC_DICT ? 1 : 0;
    case RAY_SYM:
        return (enc == RAY_ENC_RAW || enc == RAY_ENC_CONST || enc == RAY_ENC_FOR_BP || enc == RAY_ENC_RLE) ? 1 : 0;
    case RAY_F32: case RAY_F64:
        if (enc == RAY_ENC_RAW || enc == RAY_ENC_CONST) return 1;
        return (enc == RAY_ENC_RLE || enc == RAY_ENC_DICT || enc == RAY_ENC_ALP || enc == RAY_ENC_ALP_RD) ? 2 : 0;
    case RAY_STR:
        if (enc == RAY_ENC_RAW || enc == RAY_ENC_CONST) return 1;
        return (enc == RAY_ENC_STR_DICT || enc == RAY_ENC_FSST) ? 2 : 0;
    case RAY_GUID:
        return (enc == RAY_ENC_RAW || enc == RAY_ENC_CONST) ? 1 : 0;
    default: return 0;
    }
}

/* An integer statistic is a logical value of its type: inside the T-bit
 * kind's range.  The decode kernels compare values against statistics in
 * T-bit modular arithmetic, which is exact only for statistics inside it. */
static _Atomic(uint64_t) g_desc_validations;     /* test hook, see enccol.h */
uint64_t ray_enccol_test_desc_validations(void) { return atomic_load(&g_desc_validations); }

/* Chunk c's descriptors `out` (64, the bytes at p: in place or a copy)
 * against the spec's descriptor rules. */
static ray_t* chunk_descs_validate(ray_enccol_t* e, int64_t c, const ray_enc_desc_t* out, const uint8_t* p,
                                   const ray_enc_dirent_t* dep) {
    static const uint8_t zero[48];
    const ray_enc_dirent_t de = *dep;
    int64_t rows = chunk_rows(e, c);
    int nv = (int)((rows + RAY_ENC_VEC - 1) / RAY_ENC_VEC);
    uint64_t at = RAY_ENC_DESC_AREA;
    uint64_t nsum = 0;
    int T = lane_width(e);
    ray_enc_kind_t kind;
    const bool have_kind = ray_enc_kind_of(e->type, e->attrs, &kind);
    const bool de_absent = de.flags & RAY_ENC_STAT_ABSENT;
    for (int j = 0; j < RAY_ENC_VECS_PER_CHUNK; j++) {
        const ray_enc_desc_t* d = &out[j];
        if (j >= nv) {
            if (memcmp(p + (size_t)j * 48, zero, 48) != 0)
                ENC_CORRUPT("column %s: chunk %lld trailing descriptor %d not zero", e->path, (long long)c, j);
            continue;
        }
        int vlen = (int)(rows - (int64_t)j * RAY_ENC_VEC < RAY_ENC_VEC ? rows - (int64_t)j * RAY_ENC_VEC : RAY_ENC_VEC);
        int cls = top_encoding_class(e->type, d->encoding);
        if (cls == 0) ENC_CORRUPT("column %s: chunk %lld vector %d illegal encoding %u", e->path, (long long)c, j, d->encoding);
        if (cls == 2) return ray_error("nyi", "column %s: encoding %u needs a newer build", e->path, d->encoding);
        if (d->rsv0 || d->rsv1 || d->rsv2) ENC_CORRUPT("column %s: chunk %lld vector %d descriptor reserved fields", e->path, (long long)c, j);
        uint16_t okflags = RAY_ENC_STAT_ABSENT | (enc_type_float(e->type) ? RAY_ENC_DESC_TRAILER : 0);
        if (d->flags & ~okflags) ENC_CORRUPT("column %s: chunk %lld vector %d flags 0x%x", e->path, (long long)c, j, d->flags);
        bool packed = d->encoding == RAY_ENC_FOR_BP || d->encoding == RAY_ENC_DELTA_BP;
        if (packed ? d->width > T : d->width != 0) ENC_CORRUPT("column %s: chunk %lld vector %d width %u", e->path, (long long)c, j, d->width);
        if (d->n_nulls > vlen) ENC_CORRUPT("column %s: chunk %lld vector %d null count %u above the vector length", e->path, (long long)c, j, d->n_nulls);
        if (!enc_type_nullable(e->type) && d->n_nulls)
            ENC_CORRUPT("column %s: chunk %lld vector %d nulls counted in a non-nullable type", e->path, (long long)c, j);
        if (d->payload_off != at) ENC_CORRUPT("column %s: chunk %lld vector %d payload offset %llu, expected %llu", e->path, (long long)c, j,
                                             (unsigned long long)d->payload_off, (unsigned long long)at);
        if (d->payload_bytes & 7) ENC_CORRUPT("column %s: chunk %lld vector %d payload size not a multiple of 8", e->path, (long long)c, j);
        if (d->payload_bytes > de.bytes - at) ENC_CORRUPT("column %s: chunk %lld vector %d payload overruns the chunk", e->path, (long long)c, j);
        at += d->payload_bytes;
        nsum += d->n_nulls;
        bool absent = d->flags & RAY_ENC_STAT_ABSENT;
        bool all_null = enc_type_nullable(e->type) && d->n_nulls == vlen;
        if (all_null && d->encoding != RAY_ENC_CONST)
            ENC_CORRUPT("column %s: chunk %lld vector %d all-null vector not CONST", e->path, (long long)c, j);
        if (all_null && !absent)
            ENC_CORRUPT("column %s: chunk %lld vector %d all-null vector carries statistics", e->path, (long long)c, j);
        if (absent && (d->min || d->max)) ENC_CORRUPT("column %s: chunk %lld vector %d absent statistics not zero", e->path, (long long)c, j);
        if (!absent && e->type == RAY_GUID) ENC_CORRUPT("column %s: chunk %lld vector %d statistics on GUID", e->path, (long long)c, j);
        /* Only STR_DICT carries code bounds, and a STR_DICT vector already
         * returned nyi above: on RAW / CONST this is damage in every build. */
        if (!absent && e->type == RAY_STR) ENC_CORRUPT("column %s: chunk %lld vector %d statistics on a RAW / CONST string vector", e->path, (long long)c, j);
        if (!absent) {
            if (enc_type_float(e->type)) {
                double mn, mx; memcpy(&mn, &d->min, 8); memcpy(&mx, &d->max, 8);
                if (isnan(mn) || isnan(mx) || mn > mx) ENC_CORRUPT("column %s: chunk %lld vector %d float bounds", e->path, (long long)c, j);
            } else if (d->min > d->max) {
                ENC_CORRUPT("column %s: chunk %lld vector %d min above max", e->path, (long long)c, j);
            } else if (e->type == RAY_SYM && (d->min < 1 || (uint64_t)d->max >= e->sym_bound ||
                                              (T < 64 && ((uint64_t)d->max >> T)))) {
                ENC_CORRUPT("column %s: chunk %lld vector %d SYM statistics outside the domain or the id width", e->path, (long long)c, j);
            } else if (have_kind && (!stat_in_kind(kind, d->min) || !stat_in_kind(kind, d->max))) {
                ENC_CORRUPT("column %s: chunk %lld vector %d statistics outside the type", e->path, (long long)c, j);
            }
        }
        /* Statistics nest: the chunk's directory range sizes engine
         * structures (dense slabs, direct arrays, key ranges), and decode
         * keeps every value inside its vector's range, so each vector's
         * range must lie inside the chunk's.  A vector with values carries
         * statistics exactly when its chunk does (the writer's rule): one
         * without them under a chunk with them would leave its values
         * unchecked against the range consumers use. */
        if (!all_null && absent != de_absent) {
            if (absent) ENC_CORRUPT("column %s: chunk %lld vector %d statistics missing under the chunk's", e->path, (long long)c, j);
            ENC_CORRUPT("column %s: chunk %lld vector %d statistics under a chunk without them", e->path, (long long)c, j);
        }
        if (!absent && !de_absent) {
            bool out;
            if (enc_type_float(e->type)) {
                double mn, mx, dmn, dmx;
                memcpy(&mn, &d->min, 8); memcpy(&mx, &d->max, 8); memcpy(&dmn, &de.min, 8); memcpy(&dmx, &de.max, 8);
                out = mn < dmn || mx > dmx;
            } else {
                out = d->min < de.min || d->max > de.max;
            }
            if (out) ENC_CORRUPT("column %s: chunk %lld vector %d statistics outside the chunk's", e->path, (long long)c, j);
        }
    }
    if (nsum != de.null_count) ENC_CORRUPT("column %s: chunk %lld descriptor null counts sum to %llu, directory says %u", e->path,
                                           (long long)c, (unsigned long long)nsum, de.null_count);
    if (at != de.bytes) ENC_CORRUPT("column %s: chunk %lld has %llu bytes after its payloads", e->path,
                                    (long long)c, (unsigned long long)(de.bytes - at));
    return NULL;
}

/* Validated once per column (spec "Validation": before a chunk's
 * descriptors are first used).  The flag is set with release order only
 * after the chunk passed, and read with acquire: a reader that sees it set
 * also sees a fully validated chunk, and racing first readers each
 * validate (the same bytes, the same verdict) before either sets it.  A
 * failed chunk is never marked, so it fails every read.  Later reads trust
 * the mapping, as generation 0 trusts its mmap. */
static bool chunk_validated(const ray_enccol_t* e, int64_t c) {
    return e->desc_ok && atomic_load_explicit(&e->desc_ok[c], memory_order_acquire);
}

ray_t* ray_enccol_chunk_descs_ref(ray_enccol_t* e, int64_t c, const ray_enc_desc_t** out) {
    *out = NULL;
    ray_enc_dirent_t de; dirent_at(e, c, &de);
    const uint8_t* p = e->base + de.offset;            /* 8-aligned: ray_enccol_open */
    const ray_enc_desc_t* d = (const ray_enc_desc_t*)p;
    if (!chunk_validated(e, c)) {
        ray_t* err = chunk_descs_validate(e, c, d, p, &de);
        /* The test hook counts failed validations and the one that marks
         * the chunk, not the racing first readers that lose the mark. */
        if (err || !e->desc_ok || !atomic_exchange_explicit(&e->desc_ok[c], 1, memory_order_acq_rel))
            atomic_fetch_add_explicit(&g_desc_validations, 1, memory_order_relaxed);
        if (err) return err;
    }
    *out = d;
    return NULL;
}

ray_t* ray_enccol_chunk_descs(ray_enccol_t* e, int64_t c, ray_enc_desc_t out[64]) {
    const ray_enc_desc_t* d;
    ray_t* err = ray_enccol_chunk_descs_ref(e, c, &d);
    if (err) return err;
    memcpy(out, d, RAY_ENC_DESC_AREA);
    return NULL;
}

static _Atomic(int64_t) g_fail_alloc_at;
static char g_fail_path[256];
static _Atomic(bool) g_fail_path_on;
void ray_enccol_test_fail_alloc(int64_t nth) { atomic_store(&g_fail_alloc_at, nth); }
void ray_enccol_test_fail_path(const char* sub) {
    atomic_store(&g_fail_path_on, false);
    if (sub) { snprintf(g_fail_path, sizeof g_fail_path, "%s", sub); atomic_store(&g_fail_path_on, true); }
}
bool ray_enccol_test_nth_alloc_fails(void) {
    int64_t v = atomic_load(&g_fail_alloc_at);
    while (v > 0)
        if (atomic_compare_exchange_weak(&g_fail_alloc_at, &v, v - 1)) return v == 1;
    return false;
}
static bool enc_alloc_fails(const ray_enccol_t* e) {
    if (atomic_load(&g_fail_path_on) && strstr(e->path, g_fail_path)) return true;
    return ray_enccol_test_nth_alloc_fails();
}

/* ---- VALUE_DICT side entry: validated once (spec "Side data") ---- */
static ray_t* enc_dict_validate(void* ctx, ray_enc_snap_t* fail) {
    ray_enccol_t* e = ctx;
    ray_enc_kind_t k;
    uint32_t rsv;
    const char* why;
    /* Structure was checked at load (Task 6); the entry's contents are
     * checked here, once, before the first lookup. */
    if (!ray_enc_kind_of(e->type, e->attrs, &k) || !e->vdict_len) {
        ray_enc_snap_set(fail, "corrupt", "column %s: DICT vector without a VALUE_DICT entry", e->path);
        return NULL;
    }
    if (enc_alloc_fails(e)) { ray_enc_snap_set(fail, "oom", "column %s: VALUE_DICT", e->path); return NULL; }
    uint64_t dict_len = e->vdict_len;
    {
        const uint8_t* d = e->base + e->vdict_off;
        uint32_t n; memcpy(&n, d, 4); memcpy(&rsv, d + 4, 4);
        size_t esz = (size_t)k.T / 8;
        if (rsv) { why = "VALUE_DICT entry reserved word"; goto bad; }
        if (n == 0 || n > (1u << 16)) { why = "VALUE_DICT entry count"; goto bad; }
        if (dict_len != 8 + ray_enc_pad8((uint64_t)n * esz)) { why = "VALUE_DICT entry size"; goto bad; }
        if (!all_zero(d + 8 + (size_t)n * esz, (size_t)(ray_enc_pad8((uint64_t)n * esz) - (uint64_t)n * esz))) { why = "VALUE_DICT entry padding"; goto bad; }
        ray_t* vals = ray_vec_new(RAY_I64, n);
        if (!vals || RAY_IS_ERR(vals)) { ray_enc_snap_set(fail, "oom", "column %s: VALUE_DICT", e->path); return NULL; }
        vals->len = n;
        uint64_t* out = (uint64_t*)ray_data(vals);
        for (uint32_t i = 0; i < n; i++) {
            uint64_t x = 0; memcpy(&x, d + 8 + i * esz, esz);
            out[i] = x;
            if (i && !ray_enc_lt(out[i - 1], x, k)) { ray_release(vals); why = "VALUE_DICT values not ascending"; goto bad; }  /* strictly ascending */
        }
        return vals;
    }
bad:
    ray_enc_snap_set(fail, "corrupt", "column %s: %s", e->path, why);
    return NULL;
}

/* The validated dictionary once published, else false (never validates):
 * read without the once-state's lock, as the DONE store releases the slot
 * it publishes (ray_enccol_decoded_peek relies on the same). */
static bool enc_dict_ready(const ray_enccol_t* e, ray_enc_dict_t* out) {
    if (atomic_load_explicit(&e->dict_once.state, memory_order_acquire) != RAY_ENC_ONCE_DONE) return false;
    out->v = (const uint64_t*)ray_data(e->dict);
    out->n = (uint32_t)e->dict->len;
    return true;
}

static ray_t* enc_dict(ray_enccol_t* e, ray_enc_dict_t* out) {
    if (enc_dict_ready(e, out)) return NULL;      /* every vector decode of a DICT column asks */
    ray_t* d = ray_enc_once_get(&e->dict_once, &e->dict, enc_dict_validate, e);
    if (RAY_IS_ERR(d)) return d;
    out->v = (const uint64_t*)ray_data(d);
    out->n = (uint32_t)d->len;
    return NULL;
}

static uint32_t popcount_prefix(const uint8_t* bm, int n, bool* tail_clear) {
    uint32_t c = 0;
    for (int i = 0; i < n; i++) c += (bm[i >> 3] >> (i & 7)) & 1;
    *tail_clear = true;
    for (int i = n; i < RAY_ENC_VEC; i++) if ((bm[i >> 3] >> (i & 7)) & 1) { *tail_clear = false; break; }
    return c;
}

#define VEC_CORRUPT(what) ENC_CORRUPT("column %s: chunk %lld vector %d %s", e->path, (long long)c, j, what)

static ray_t* dec_ints(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d, int vlen,
                       const uint8_t* p, uint64_t avail, const uint8_t* bm, void* out) {
    ray_enc_kind_t k;
    if (!ray_enc_kind_of(e->type, e->attrs, &k)) VEC_CORRUPT("kind");
    ray_enc_dict_t dict = {0};
    if (d->encoding == RAY_ENC_DICT) { ray_t* err = enc_dict(e, &dict); if (err) return err; }
    uint64_t bits[RAY_ENC_VEC];
    int64_t used = ray_enc_read(p, (size_t)avail, d->encoding, d->width, vlen, k,
                                RAY_ENC_SLOT_TOP, 0, d->encoding == RAY_ENC_DICT ? &dict : NULL, bits);
    if (used < 0) ENC_CORRUPT("column %s: chunk %lld vector %d codec body: %s", e->path, (long long)c, j, ray_enc_read_why());
    if ((uint64_t)used != avail) VEC_CORRUPT("bytes after the codec body");
    uint8_t esz = ray_enccol_esz(e);
    const bool sym = e->type == RAY_SYM;
    /* SYM ids and DICT values inside the vector's statistics (absent only
     * on an all-null vector, chunk_descs_validate): checked below, as the
     * fast kernels check them.  Other encodings are not: a consumer that
     * sizes memory from statistics guards it (ray_col_src_t.stats_guard). */
    const bool has_st = !(d->flags & RAY_ENC_STAT_ABSENT) && (sym || d->encoding == RAY_ENC_DICT);
    int zeros = 0;
    for (int i = 0; i < vlen; i++) {
        bool null = bm && ((bm[i >> 3] >> (i & 7)) & 1);
        uint64_t x = bits[i];
        if (sym) {
            /* Defence in depth: ray_enc_read masks to the lane width, so the
             * width test cannot fire here; the real width guard is the
             * descriptor-statistics check in ray_enccol_chunk_descs.  Id 0
             * is the null and is always legal. */
            if ((esz < 8 && (x >> (8 * esz))) || (x && x >= e->sym_bound)) VEC_CORRUPT("SYM id outside the symbol domain or the id width");
            zeros += x == 0;
        }
        switch (esz) {
        case 1: ((uint8_t*)out)[i] = (uint8_t)x; break;
        case 2: if (sym) { ((uint16_t*)out)[i] = (uint16_t)x; break; }
                if (!null && (int16_t)x == NULL_I16) VEC_CORRUPT("sentinel outside the bitmap");
                ((int16_t*)out)[i] = null ? NULL_I16 : (int16_t)x; break;
        case 4: if (sym) { ((uint32_t*)out)[i] = (uint32_t)x; break; }
                if (!null && (int32_t)x == NULL_I32) VEC_CORRUPT("sentinel outside the bitmap");
                ((int32_t*)out)[i] = null ? NULL_I32 : (int32_t)x; break;
        default: if (sym) { ((uint64_t*)out)[i] = x; break; }
                if (!null && (int64_t)x == NULL_I64) VEC_CORRUPT("sentinel outside the bitmap");
                ((int64_t*)out)[i] = null ? NULL_I64 : (int64_t)x; break;
        }
    }
    if (e->type == RAY_SYM && zeros != d->n_nulls) VEC_CORRUPT("SYM null count");
    /* last, so a vector that also breaks a rule above reports that one */
    for (int i = 0; has_st && i < vlen; i++) {
        if ((bm && ((bm[i >> 3] >> (i & 7)) & 1)) || (sym && bits[i] == 0)) continue;
        const int64_t lv = ray_enc_logical(bits[i], k);
        if (lv < d->min || lv > d->max) VEC_CORRUPT("value outside its statistics");
    }
    return NULL;
}

static ray_t* dec_floats(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d, int vlen,
                         const uint8_t* p, uint64_t avail, const uint8_t* bm, void* out) {
    const bool f32 = e->type == RAY_F32;
    const size_t esz = f32 ? 4 : 8;
    uint64_t body = d->encoding == RAY_ENC_CONST ? 8 : ray_enc_pad8((uint64_t)vlen * esz);
    if (body > avail) VEC_CORRUPT("float body short");
    if (d->encoding == RAY_ENC_CONST) {
        if (!all_zero(p + esz, 8 - esz)) VEC_CORRUPT("float CONST padding");
        for (int i = 0; i < vlen; i++) memcpy((uint8_t*)out + i * esz, p, esz);
    } else {
        if (!all_zero(p + (size_t)vlen * esz, (size_t)(body - (uint64_t)vlen * esz))) VEC_CORRUPT("float body padding");
        memcpy(out, p, (size_t)vlen * esz);
    }
    const bool has_st = !(d->flags & RAY_ENC_STAT_ABSENT);
    double smin = 0, smax = 0;
    if (has_st) { memcpy(&smin, &d->min, 8); memcpy(&smax, &d->max, 8); }
    for (int i = 0; i < vlen; i++) {
        bool null = bm && ((bm[i >> 3] >> (i & 7)) & 1);
        uint8_t* q = (uint8_t*)out + i * esz;
        if (null) {
            if (f32) { float n = NULL_F32; memcpy(q, &n, 4); } else { double n = NULL_F64; memcpy(q, &n, 8); }
        } else {
            double x; if (f32) { float f; memcpy(&f, q, 4); x = f; } else memcpy(&x, q, 8);
            if (x != x) VEC_CORRUPT("NaN outside the bitmap");
            if (has_st && (x < smin || x > smax)) VEC_CORRUPT("value outside its statistics");
        }
    }
    uint64_t rest = avail - body;
    const uint8_t* t = p + body;
    if (d->flags & RAY_ENC_DESC_TRAILER) {
        if (rest < 8) VEC_CORRUPT("trailer missing");
        uint16_t cnt; memcpy(&cnt, t, 2);
        if (!all_zero(t + 2, 6)) VEC_CORRUPT("trailer reserved bytes");
        if (cnt == 0 || cnt > d->n_nulls) VEC_CORRUPT("trailer count");
        uint64_t need = 8 + ray_enc_pad8(2ull * cnt) + ray_enc_pad8((uint64_t)cnt * esz);
        if (need != rest) VEC_CORRUPT("trailer size");
        if (!all_zero(t + 8 + 2 * cnt, (size_t)(ray_enc_pad8(2ull * cnt) - 2ull * cnt))) VEC_CORRUPT("trailer position padding");
        const uint8_t* raw = t + 8 + ray_enc_pad8(2ull * cnt);
        if (!all_zero(raw + cnt * esz, (size_t)(ray_enc_pad8((uint64_t)cnt * esz) - (uint64_t)cnt * esz))) VEC_CORRUPT("trailer value padding");
        int prev = -1;
        for (int i = 0; i < cnt; i++) {
            uint16_t pos; memcpy(&pos, t + 8 + 2 * i, 2);
            if ((int)pos <= prev) VEC_CORRUPT("trailer positions not ascending");
            if (pos >= vlen) VEC_CORRUPT("trailer position past the vector");
            if (!bm || !((bm[pos >> 3] >> (pos & 7)) & 1)) VEC_CORRUPT("trailer position not in the bitmap");
            prev = pos;
            double x; if (f32) { float f; memcpy(&f, raw + i * esz, 4); x = f; } else memcpy(&x, raw + i * esz, 8);
            if (x == x) VEC_CORRUPT("trailer value is not a NaN");
            memcpy((uint8_t*)out + pos * esz, raw + i * esz, esz);
        }
    } else if (rest != 0) {
        VEC_CORRUPT("bytes after the body");
    }
    return NULL;
}

static ray_t* dec_guid(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d, int vlen,
                       const uint8_t* p, uint64_t avail, void* out) {
    static const uint8_t zero[16];
    if (d->encoding == RAY_ENC_CONST) {
        if (avail != 16) VEC_CORRUPT("GUID CONST body size");
        for (int i = 0; i < vlen; i++) memcpy((uint8_t*)out + i * 16, p, 16);
    } else {
        if (avail != (uint64_t)vlen * 16) VEC_CORRUPT("GUID RAW body size");
        memcpy(out, p, (size_t)avail);
    }
    int nulls = 0;
    for (int i = 0; i < vlen; i++) nulls += memcmp((uint8_t*)out + i * 16, zero, 16) == 0;
    if (nulls != d->n_nulls) VEC_CORRUPT("GUID null count");
    return NULL;
}

/* STR RAW / CONST grammar (spec "STR RAW").  With out == NULL it only
 * validates and sizes the decoded pool bytes (pooled strings once per row);
 * with out set the caller has already validated, and pool_out has room for
 * pool_base + pooled bytes. */
static ray_t* dec_str(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d, int vlen,
                      const uint8_t* p, uint64_t avail, void* out, char* pool_out,
                      uint64_t pool_base, uint64_t pool_cap, uint64_t* pooled_out) {
    ray_str_t* o = (ray_str_t*)out;
    int nulls = 0;
    uint64_t pooled = 0;
    if (d->encoding == RAY_ENC_CONST) {
        if (avail < 8) VEC_CORRUPT("STR CONST body short");
        uint32_t len, rsv; memcpy(&len, p, 4); memcpy(&rsv, p + 4, 4);
        if (rsv) VEC_CORRUPT("STR CONST reserved word");
        if (avail != 8 + ray_enc_pad8(len)) VEC_CORRUPT("STR CONST body size");
        if (!all_zero(p + 8 + len, (size_t)(ray_enc_pad8(len) - len))) VEC_CORRUPT("STR CONST padding");
        if (len > RAY_STR_INLINE_MAX) pooled = (uint64_t)len * (uint64_t)vlen;
        nulls = len == 0 ? vlen : 0;
        if (out) {
            for (int i = 0; i < vlen; i++) {
                memset(&o[i], 0, sizeof o[i]);
                o[i].len = len;
                if (len <= RAY_STR_INLINE_MAX) { memcpy(o[i].data, p + 8, len); continue; }
                uint64_t off = pool_base + (uint64_t)i * len;
                memcpy(pool_out + off, p + 8, len);
                memcpy(o[i].prefix, p + 8, 4);
                o[i].pool_off = (uint32_t)off;
            }
        }
    } else {
        uint64_t dbytes = (uint64_t)vlen * 16;
        if (avail < dbytes + 8) VEC_CORRUPT("STR RAW body short");
        uint64_t pb; memcpy(&pb, p + dbytes, 8);
        if (pb > UINT32_MAX) VEC_CORRUPT("pool size above 4 GiB");
        if (avail != dbytes + 8 + ray_enc_pad8(pb)) VEC_CORRUPT("pool size");
        const uint8_t* vp = p + dbytes + 8;
        if (!all_zero(vp + pb, (size_t)(ray_enc_pad8(pb) - pb))) VEC_CORRUPT("pool padding");
        uint64_t run = 0;
        for (int i = 0; i < vlen; i++) {
            const uint8_t* w = p + (size_t)i * 16;
            uint32_t len; memcpy(&len, w, 4);
            if (len == 0) nulls++;
            if (len <= RAY_STR_INLINE_MAX) {
                if (!all_zero(w + 4 + len, 12 - len)) VEC_CORRUPT("inline padding");
                if (out) memcpy(&o[i], w, 16);
                continue;
            }
            uint32_t off, h; memcpy(&off, w + 8, 4); memcpy(&h, w + 12, 4);
            if (h != 0) VEC_CORRUPT("pooled descriptor hash not zero");
            if (off != run) VEC_CORRUPT("pooled descriptor offset");
            if (len > pb - run) VEC_CORRUPT("pooled descriptor past the pool");
            if (memcmp(w + 4, vp + off, 4) != 0) VEC_CORRUPT("pooled descriptor prefix");
            if (out) {
                memcpy(pool_out + pool_base + run, vp + off, len);
                memset(&o[i], 0, sizeof o[i]);
                o[i].len = len; memcpy(o[i].prefix, w + 4, 4);
                o[i].pool_off = (uint32_t)(pool_base + run);
            }
            run += len;
        }
        if (run != pb) VEC_CORRUPT("pool not fully used");
        pooled = pb;
    }
    if (nulls != d->n_nulls) VEC_CORRUPT("STR null count");
    if (pooled_out) *pooled_out = pooled;
    return NULL;
}

/* Split a payload into bitmap and body; shared by decode and pool sizing. */
static ray_t* payload_parts(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d, int vlen,
                            const uint8_t** body, uint64_t* avail, const uint8_t** bm) {
    ray_enc_dirent_t de; dirent_at(e, c, &de);
    const uint8_t* p = e->base + de.offset + d->payload_off;
    uint64_t n = d->payload_bytes;
    *bm = NULL;
    if (d->n_nulls > 0 && ray_enc_has_bitmap(e->type)) {
        if (n < 128) VEC_CORRUPT("null bitmap short");
        bool tail_clear;
        if (popcount_prefix(p, vlen, &tail_clear) != d->n_nulls) VEC_CORRUPT("null bitmap count");
        if (!tail_clear) VEC_CORRUPT("null bitmap tail bits set");
        *bm = p; p += 128; n -= 128;
    }
    *body = p; *avail = n;
    return NULL;
}

/* ---- Integer fast path: FOR_BP / DELTA_BP straight into the destination ----
 * One pass unpacks the interleaved lanes at the lane width T, adds the base
 * (DELTA_BP: the lane's previous row and the delta base) and stores the
 * element, at the generation-0 width or widened for an expression register.
 * The nulls are checked after it: a vectorised sentinel scan (fused into the
 * widening kernels; skipped when the FOR_BP base and width prove that no
 * value is the sentinel), or with a bitmap one pass that also merges it.
 * No u64 intermediate, no aside copy.  It accepts exactly what dec_ints
 * accepts: when anything is off (a size, a base, a head, a placeholder, a
 * sentinel, a SYM id or a DICT value outside the statistics) it declines,
 * and the reference path decodes or reports the vector, so a corrupt file
 * keeps its exact error.  FOR_BP / DELTA_BP values are not checked against
 * the statistics: a consumer that sizes memory from them guards it
 * (ray_col_src_t.stats_guard). */

/* Lane l's field of the current row: w bits at bit offset off of word a[l]
 * (off + w > T: continued in the next word row, a[l + L]).  Arithmetic is
 * in PT (unsigned, or uint64_t for 64-bit lanes) with shift counts masked
 * below T, so the compiler keeps T-bit lanes (no widening to int). */
#define FK_FIELD1(UT, PT, a, l, sr, m)        ((UT)((PT)((PT)(a)[l] >> (sr)) & (PT)(m)))
#define FK_FIELD2(UT, PT, L, a, l, sr, sl, m) \
    ((UT)((PT)(((PT)(a)[l] >> (sr)) | ((PT)(a)[(l) + (L)] << (sl))) & (PT)(m)))

/* One row of lanes [0, nl): the T-bit value V stored widened from ST to OT;
 * SENT: also flag a value on the T-bit pattern s (the widening kernels,
 * where this beats a second pass over the 8-byte output). */
#define FK_ROW(UT, ST, OT, SENT, V) do { for (int l = 0; l < nl; l++) {                 \
        const UT v_ = (V); if (SENT) bad |= (UT)-(UT)(v_ == s); o[l] = (OT)(ST)v_; } } while (0)

/* FOR_BP: out[i] = base + field, rows below n.  ST: the signed (or
 * unsigned) view the element widens from; OT: the stored element.
 * Returns the SENT flag (0 without SENT). */
#define FK_FOR(NAME, UT, PT, TB, ST, OT, SENT)                                          \
static UT NAME(const UT* restrict in, int w, UT base, OT* restrict out, int n, UT s) { \
    enum { L = RAY_ENC_VEC / TB };                                                      \
    const UT m = w >= TB ? (UT)~(UT)0 : (UT)(((PT)1 << w) - 1);                         \
    UT bad = 0; (void)s;                                                                \
    for (int r = 0, bit = 0; r * L < n; r++, bit += w) {                                \
        const int k = bit / TB, off = bit % TB;                                         \
        const unsigned sr = (unsigned)off & (TB - 1), sl = (unsigned)(TB - off) & (TB - 1); \
        const int nl = n - r * L < L ? n - r * L : L;                                   \
        const UT* a = in + (size_t)k * L;                                               \
        OT* o = out + (size_t)r * L;                                                    \
        if (w == 0) FK_ROW(UT, ST, OT, SENT, base);                                     \
        else if (off + w > TB) FK_ROW(UT, ST, OT, SENT, (UT)((PT)base + FK_FIELD2(UT, PT, L, a, l, sr, sl, m))); \
        else FK_ROW(UT, ST, OT, SENT, (UT)((PT)base + FK_FIELD1(UT, PT, a, l, sr, m))); \
    }                                                                                   \
    return bad;                                                                         \
}
/* DELTA_BP: row 0 is the heads, row r the row above plus dbase plus field r. */
#define FK_DELTA(NAME, UT, PT, TB, ST, OT, SENT)                                        \
static UT NAME(const UT* restrict in, int w, UT dbase, const UT* restrict head,        \
               OT* restrict out, int n, UT s) {                                         \
    enum { L = RAY_ENC_VEC / TB };                                                      \
    const UT m = w >= TB ? (UT)~(UT)0 : (UT)(((PT)1 << w) - 1);                         \
    UT bad = 0; (void)s;                                                                \
    UT prev[L];                                                                         \
    for (int l = 0; l < L; l++) prev[l] = head[l];                                      \
    { const int nl = n < L ? n : L; OT* o = out; FK_ROW(UT, ST, OT, SENT, prev[l]); }   \
    for (int r = 1, bit = w; r * L < n; r++, bit += w) {                                \
        const int k = bit / TB, off = bit % TB;                                         \
        const unsigned sr = (unsigned)off & (TB - 1), sl = (unsigned)(TB - off) & (TB - 1); \
        const int nl = n - r * L < L ? n - r * L : L;                                   \
        const UT* a = in + (size_t)k * L;                                               \
        OT* o = out + (size_t)r * L;                                                    \
        if (w == 0) FK_ROW(UT, ST, OT, SENT, prev[l] = (UT)((PT)prev[l] + dbase));      \
        else if (off + w > TB)                                                          \
            FK_ROW(UT, ST, OT, SENT, prev[l] = (UT)((PT)prev[l] + dbase + FK_FIELD2(UT, PT, L, a, l, sr, sl, m))); \
        else FK_ROW(UT, ST, OT, SENT, prev[l] = (UT)((PT)prev[l] + dbase + FK_FIELD1(UT, PT, a, l, sr, m))); \
    }                                                                                   \
    return bad;                                                                         \
}
FK_FOR(fk_for8,     uint8_t,  unsigned,  8, uint8_t,  uint8_t,  0)
FK_FOR(fk_for16,    uint16_t, unsigned, 16, uint16_t, uint16_t, 0)
FK_FOR(fk_for32,    uint32_t, unsigned, 32, uint32_t, uint32_t, 0)
FK_FOR(fk_for64,    uint64_t, uint64_t, 64, uint64_t, uint64_t, 0)
FK_FOR(fk_for8_i,   uint8_t,  unsigned,  8, uint8_t,  int64_t,  0)
FK_FOR(fk_for16_i,  uint16_t, unsigned, 16, int16_t,  int64_t,  1)
FK_FOR(fk_for32_i,  uint32_t, unsigned, 32, int32_t,  int64_t,  1)
FK_FOR(fk_for8_f,   uint8_t,  unsigned,  8, uint8_t,  double,   0)
FK_FOR(fk_for16_f,  uint16_t, unsigned, 16, int16_t,  double,   1)
FK_FOR(fk_for32_f,  uint32_t, unsigned, 32, int32_t,  double,   1)
FK_FOR(fk_for16_in, uint16_t, unsigned, 16, int16_t,  int64_t,  0)   /* no value can be the sentinel */
FK_FOR(fk_for32_in, uint32_t, unsigned, 32, int32_t,  int64_t,  0)
FK_FOR(fk_for16_fn, uint16_t, unsigned, 16, int16_t,  double,   0)
FK_FOR(fk_for32_fn, uint32_t, unsigned, 32, int32_t,  double,   0)
FK_DELTA(fk_dlt16,   uint16_t, unsigned, 16, uint16_t, uint16_t, 0)
FK_DELTA(fk_dlt32,   uint32_t, unsigned, 32, uint32_t, uint32_t, 0)
FK_DELTA(fk_dlt64,   uint64_t, uint64_t, 64, uint64_t, uint64_t, 0)
FK_DELTA(fk_dlt16_i, uint16_t, unsigned, 16, int16_t,  int64_t,  1)
FK_DELTA(fk_dlt32_i, uint32_t, unsigned, 32, int32_t,  int64_t,  1)
FK_DELTA(fk_dlt16_f, uint16_t, unsigned, 16, int16_t,  double,   1)
FK_DELTA(fk_dlt32_f, uint32_t, unsigned, 32, int32_t,  double,   1)

/* Nulls of a signed kind over the stored elements.  scan (no bitmap, the
 * generation-0 width; the widening kernels flag it themselves): an element
 * on the sentinel s is damage (true).  merge: rows in the bitmap become the
 * output null z, and a row outside it on s is damage.  The flags are kept
 * at the element width (MT) and the loops out of line, so the scan
 * vectorises. */
#define FK_SCAN(NAME, OT, MT)                                                           \
static __attribute__((noinline)) bool NAME(const OT* restrict o, int n, OT s) {        \
    MT bad = 0;                                                                         \
    for (int i = 0; i < n; i++) bad |= (MT)-(MT)(o[i] == s);                            \
    return bad != 0;                                                                    \
}
#define FK_MERGE(NAME, OT, MT)                                                          \
static __attribute__((noinline)) bool NAME(OT* restrict o, int n, const uint8_t* restrict bm, OT s, OT z) { \
    MT bad = 0;                                                                         \
    for (int i = 0; i < n; i++) {                                                       \
        const MT nul = (MT)-(MT)((bm[i >> 3] >> (i & 7)) & 1u);                         \
        bad |= (MT)~nul & (MT)-(MT)(o[i] == s);                                         \
        o[i] = nul ? z : o[i];                                                          \
    }                                                                                   \
    return bad != 0;                                                                    \
}
FK_SCAN(fk_scan16, int16_t, uint16_t)
FK_SCAN(fk_scan32, int32_t, uint32_t)
FK_SCAN(fk_scan64, int64_t, uint64_t)
FK_MERGE(fk_merge16, int16_t, uint16_t)
FK_MERGE(fk_merge32, int32_t, uint32_t)
FK_MERGE(fk_merge64, int64_t, uint64_t)
FK_MERGE(fk_mergef,  double,  uint64_t)

/* SYM ids: below the bound or 0 (the null), the non-null ones inside the
 * vector's statistics [lo, lo + span], and exactly n_nulls zeros.  Out of
 * line, as the scans above: inlined into dec_fast the check loop stayed
 * scalar (a third of q34's URL decode). */
#define FK_SYM(NAME, UT)                                                                \
static __attribute__((noinline)) bool NAME(const UT* restrict o, int n, uint64_t bound, int n_nulls, UT lo, UT span) { \
    UT bad = 0; uint32_t zeros = 0;                                                     \
    /* x is bad when x != 0 and x >= bound, i.e. x > lim */                             \
    const UT lim = bound == 0 ? 0 : bound - 1 > (uint64_t)(UT)~(UT)0 ? (UT)~(UT)0 : (UT)(bound - 1); \
    for (int i = 0; i < n; i++) {                                                       \
        zeros += o[i] == 0;                                                             \
        bad |= (UT)-(UT)((o[i] != 0) & ((o[i] > lim) | ((UT)(o[i] - lo) > span)));     \
    }                                                                                   \
    return !bad && zeros == (uint32_t)n_nulls;                                          \
}
FK_SYM(fk_sym8,  uint8_t)
FK_SYM(fk_sym16, uint16_t)
FK_SYM(fk_sym32, uint32_t)
FK_SYM(fk_sym64, uint64_t)

static bool fk_int_type(int8_t t) {
    return t != RAY_F32 && t != RAY_F64 && t != RAY_GUID && t != RAY_STR;
}

/* Nulls of a signed kind after a fast decode at the generation-0 width,
 * without a widening kernel's flag: no bitmap, a sentinel scan (can_hit:
 * one is possible); with one, the merge.  1 sound, 0 declined. */
static int fk_nulls_native(const ray_enc_desc_t* d, int T, int vlen, const uint8_t* bm, bool can_hit, void* out) {
    if (d->n_nulls && !bm) return 0;
    bool bad;
    if (!d->n_nulls) {
        if (!can_hit) return 1;
        switch (T) {
        case 16: bad = fk_scan16((const int16_t*)out, vlen, NULL_I16); break;
        case 32: bad = fk_scan32((const int32_t*)out, vlen, NULL_I32); break;
        default: bad = fk_scan64((const int64_t*)out, vlen, NULL_I64); break;
        }
        return bad ? 0 : 1;
    }
    switch (T) {
    case 16: bad = fk_merge16((int16_t*)out, vlen, bm, NULL_I16, NULL_I16); break;
    case 32: bad = fk_merge32((int32_t*)out, vlen, bm, NULL_I32, NULL_I32); break;
    default: bad = fk_merge64((int64_t*)out, vlen, bm, NULL_I64, NULL_I64); break;
    }
    return bad ? 0 : 1;
}

/* The rows outside the bitmap inside the vector's statistics, read from
 * the DICT kernel's output (nulls merged): the exact check behind its
 * range flag over a vector with nulls (a null slot's code may name any
 * entry; a writer fills it with the minimum, so this is rare). */
static bool fk_range_exact(const ray_enc_desc_t* d, const void* out, int T, int vlen, const uint8_t* bm) {
    for (int i = 0; i < vlen; i++) {
        if ((bm[i >> 3] >> (i & 7)) & 1) continue;
        int64_t v;
        if (T == 64)      v = ((const int64_t*)out)[i];
        else if (T == 32) v = ((const int32_t*)out)[i];
        else              v = ((const int16_t*)out)[i];
        if (v < d->min || v > d->max) return false;
    }
    return true;
}

/* The vector's statistics as the kernels check them: lo and span (max -
 * min) as T-bit patterns, exact for statistics inside the type
 * (chunk_descs_validate).  Without statistics (an all-null vector) every
 * value passes: span is all ones. */
static void fk_stats(const ray_enc_desc_t* d, uint64_t m, uint64_t* lo, uint64_t* span) {
    const bool has = !(d->flags & RAY_ENC_STAT_ABSENT);
    *lo = has ? (uint64_t)d->min & m : 0;
    *span = has ? ((uint64_t)d->max - (uint64_t)d->min) & m : m;
}

/* DICT over a U32 FOR_BP code child, at the generation-0 width: the codes
 * unpacked by the lane kernel, checked below the dictionary size in one
 * or-reduce, gathered from the validated dictionary with the values'
 * distance above the statistics' minimum folded into a max-reduce, then
 * the nulls as the FOR_BP path checks them.  It accepts exactly what dec_ints accepts and
 * declines on anything else (a RAW or RLE code child, a child header or
 * size off, a dictionary not yet validated, a code outside it, a
 * sentinel, a value outside the statistics), so the reference path decodes
 * or reports the vector (and runs the dictionary's one validation, with
 * its errors). */
static int dec_fast_dict(ray_enccol_t* e, const ray_enc_desc_t* d, int vlen, const uint8_t* body,
                         uint64_t avail, const uint8_t* bm, ray_enc_kind_t k, void* out) {
    if (d->width || !k.is_signed || avail < 16 || body[0] != RAY_ENC_FOR_BP) return 0;
    const int w = body[1];
    uint16_t rsv; memcpy(&rsv, body + 2, 2);
    uint32_t bb; memcpy(&bb, body + 4, 4);
    if (rsv || w > 32 || (uint64_t)bb + 8 != avail || bb != (uint64_t)ray_enc_size_for(w)) return 0;
    uint64_t base; memcpy(&base, body + 8, 8);
    if (base >> 32) return 0;
    ray_enc_dict_t dict;
    if (!enc_dict_ready(e, &dict)) return 0;                 /* validation and its errors: the reference path */
    _Alignas(64) uint32_t codes[RAY_ENC_VEC];
    (void)fk_for32((const uint32_t*)(body + 16), w, (uint32_t)base, codes, vlen, 0);
    const uint32_t lim = dict.n - 1;                         /* a validated dictionary is not empty */
    uint32_t bad = 0;
    for (int i = 0; i < vlen; i++) bad |= (uint32_t)-(uint32_t)(codes[i] > lim);
    if (bad) return 0;
    const uint64_t* dv = dict.v;
    uint64_t lo, span;
    fk_stats(d, k.T == 64 ? ~0ull : (1ull << k.T) - 1, &lo, &span);
    bool range_hit = false;
#define FK_GATHER(UT) do { UT* o = out; UT dm = 0;                                       \
        for (int i = 0; i < vlen; i++) {                                                \
            const UT v = (UT)dv[codes[i]], r = (UT)(v - (UT)lo);                        \
            dm = r > dm ? r : dm; o[i] = v;                                             \
        }                                                                               \
        range_hit = dm > (UT)span; } while (0)
    switch (k.T) {
    case 16: FK_GATHER(uint16_t); break;
    case 32: FK_GATHER(uint32_t); break;
    case 64: FK_GATHER(uint64_t); break;
    default: return 0;
    }
#undef FK_GATHER
    if (range_hit && !(bm && d->n_nulls)) return 0;           /* a value outside the statistics */
    if (!fk_nulls_native(d, k.T, vlen, bm, true, out)) return 0;
    return !range_hit || fk_range_exact(d, out, k.T, vlen, bm) ? 1 : 0;
}

/* ---- CONST / RLE: the runs validated, then filled at the element width ----
 * A CONST vector is one run.  An RLE vector is nr runs: its values and its
 * run ends are children at depth 1 (RAW or FOR_BP values of the element
 * kind; RAW, FOR_BP or DELTA_BP U16 ends), each unpacked for its nr values
 * only, by the lane kernels above.  Every rule ray_enc_read and dec_ints
 * apply is checked on the runs before a row is written: the header, the
 * run count, each child's header, legality, width, base, heads,
 * placeholders, padding and size, the bytes used, the ends (strictly
 * ascending, the last at vlen), a run on the sentinel (every row of it in
 * the bitmap), the SYM ids (domain, statistics, the null count).  Then each
 * run is filled, at the generation-0 width or widened as the FOR_BP
 * kernels widen, and the bitmap's rows set to the null.  As there, anything
 * off declines and the reference path decodes or reports the vector. */

/* nr T-bit values, by lane width (the view's lane buffer, enccol.h). */
typedef ray_enc_lanes_t fk_vals_t;

static uint64_t fk_val(const fk_vals_t* v, int T, uint32_t i) {
    switch (T) { case 8: return v->b[i]; case 16: return v->h[i]; case 32: return v->s[i]; default: return v->q[i]; }
}

/* An RLE child at depth 1: an 8-byte header at p (avail bytes left in the
 * parent, p 8-aligned), then nr values of T bits into out.  ends: the run
 * ends slot (U16), where DELTA_BP is legal too.  The bytes used, or -1. */
static int64_t fk_child(const uint8_t* p, uint64_t avail, uint32_t nr, int T, bool ends, fk_vals_t* out) {
    if (avail < 8) return -1;
    const uint8_t enc = p[0], w = p[1];
    uint16_t rsv; memcpy(&rsv, p + 2, 2);
    uint32_t bb;  memcpy(&bb, p + 4, 4);
    if (rsv || (bb & 7) || bb > avail - 8) return -1;
    const uint8_t* b = p + 8;
    const uint64_t m = T == 64 ? ~0ull : (1ull << T) - 1;
    if (enc == RAY_ENC_RAW) {
        const size_t nb = (size_t)nr * (size_t)T / 8;
        if (w || bb != (uint64_t)ray_enc_size_raw((int)nr, T) || !all_zero(b + nb, bb - nb)) return -1;
        memcpy(out, b, nb);
    } else if (enc == RAY_ENC_FOR_BP) {
        if (w > T || bb != (uint64_t)ray_enc_size_for(w)) return -1;
        uint64_t base; memcpy(&base, b, 8);
        if (base & ~m) return -1;
        switch (T) {
        case 8:  (void)fk_for8((const uint8_t*)(b + 8), w, (uint8_t)base, out->b, (int)nr, 0); break;
        case 16: (void)fk_for16((const uint16_t*)(b + 8), w, (uint16_t)base, out->h, (int)nr, 0); break;
        case 32: (void)fk_for32((const uint32_t*)(b + 8), w, (uint32_t)base, out->s, (int)nr, 0); break;
        default: (void)fk_for64((const uint64_t*)(b + 8), w, base, out->q, (int)nr, 0); break;
        }
    } else if (enc == RAY_ENC_DELTA_BP && ends) {
        enum { L = RAY_ENC_VEC / 16 };
        if (w > 16 || bb != (uint64_t)ray_enc_size_delta(16, w)) return -1;
        uint64_t dbase; memcpy(&dbase, b, 8);
        if (dbase & ~m) return -1;
        uint16_t head[L];
        for (int l = 0; l < L; l++) {
            uint64_t x; memcpy(&x, b + 8 + 8 * (size_t)l, 8);
            if (x & ~m) return -1;                                   /* a head above the type width */
            head[l] = (uint16_t)x;
        }
        const uint16_t* pk = (const uint16_t*)(b + 8 + 8 * (size_t)L);
        const uint16_t wm = (uint16_t)(w >= 16 ? 0xFFFF : (1u << w) - 1);
        uint16_t any = 0;
        for (int l = 0; l < L; l++) any |= pk[l] & wm;                /* row 0 holds zero placeholders */
        if (any) return -1;
        (void)fk_dlt16(pk, w, (uint16_t)dbase, head, out->h, (int)nr, 0);
    } else {
        return -1;                                                   /* illegal in the slot, or unknown */
    }
    return 8 + (int64_t)bb;
}

/* The runs of a CONST or RLE body of kind T: *nr values, run i ending at
 * ends[i] (strictly ascending, the last at vlen).  1, or 0 declined. */
static int fk_runs(uint8_t enc, int vlen, const uint8_t* body, uint64_t avail, int T,
                   fk_vals_t* vals, fk_vals_t* ends, uint32_t* nr_out) {
    uint32_t nr;
    if (enc == RAY_ENC_CONST) {
        if (avail != (uint64_t)ray_enc_size_raw(1, T) || !all_zero(body + T / 8, (size_t)avail - (size_t)T / 8)) return 0;
        memcpy(vals, body, (size_t)T / 8);
        ends->h[0] = (uint16_t)vlen;
        nr = 1;
    } else {
        if (avail < 8) return 0;
        uint32_t rsv; memcpy(&nr, body, 4); memcpy(&rsv, body + 4, 4);
        if (rsv || nr == 0 || nr > (uint32_t)vlen) return 0;
        int64_t a = fk_child(body + 8, avail - 8, nr, T, false, vals);
        if (a < 0) return 0;
        int64_t b = fk_child(body + 8 + a, avail - 8 - (uint64_t)a, nr, 16, true, ends);
        if (b < 0 || (uint64_t)(8 + a + b) != avail) return 0;
        uint32_t prev = 0, bad = 0;
        for (uint32_t i = 0; i < nr; i++) { bad |= (uint32_t)(ends->h[i] <= prev); prev = ends->h[i]; }
        if (bad || prev != (uint32_t)vlen) return 0;                 /* ascending, so every end <= vlen */
    }
    *nr_out = nr;
    return 1;
}

/* Every row of [lo, hi) in the bitmap. */
static bool fk_bits_all(const uint8_t* bm, uint32_t lo, uint32_t hi) {
    for (uint32_t r = lo; r < hi; r++) if (!((bm[r >> 3] >> (r & 7)) & 1)) return false;
    return true;
}

/* The value rules of nr runs (fk_runs' output) of an integer kind, shared
 * by the run decode and the view so they cannot drift: SYM ids below the
 * bound or 0 (the null), the others inside the vector's statistics, and
 * exactly n_nulls rows of id 0; a signed kind's run on the sentinel wholly
 * inside the bitmap.  Inlined; the decode is behaviour-identical (its
 * register allocation differs slightly). */
static inline __attribute__((always_inline)) bool fk_runs_ok(const ray_enccol_t* e, const ray_enc_desc_t* d,
                                                             const uint8_t* bm, ray_enc_kind_t k,
                                                             const fk_vals_t* vals, const fk_vals_t* ends, uint32_t nr) {
    const int T = k.T;
    const uint64_t m = T == 64 ? ~0ull : (1ull << T) - 1;
    if (e->type == RAY_SYM) {
        uint64_t lo, span;
        fk_stats(d, m, &lo, &span);
        const uint64_t lim = e->sym_bound == 0 ? 0 : (uint64_t)e->sym_bound - 1 < m ? (uint64_t)e->sym_bound - 1 : m;
        uint32_t zeros = 0, prev = 0;
        for (uint32_t i = 0; i < nr; i++) {
            const uint64_t x = fk_val(vals, T, i);
            if (x == 0) zeros += ends->h[i] - prev;
            else if (x > lim || ((x - lo) & m) > span) return false;
            prev = ends->h[i];
        }
        return zeros == (uint32_t)d->n_nulls;
    }
    if (k.is_signed) {
        const uint64_t sent = (uint64_t)1 << (T - 1);
        if (d->n_nulls && !bm) return false;
        /* one branch-free pass at the lane width finds whether any run is on
         * the sentinel; only then are the runs walked with the bitmap */
        bool any = false;
        switch (T) {
        case 8:  for (uint32_t i = 0; i < nr; i++) any |= vals->b[i] == (uint8_t)sent; break;
        case 16: for (uint32_t i = 0; i < nr; i++) any |= vals->h[i] == (uint16_t)sent; break;
        case 32: for (uint32_t i = 0; i < nr; i++) any |= vals->s[i] == (uint32_t)sent; break;
        default: for (uint32_t i = 0; i < nr; i++) any |= vals->q[i] == sent; break;
        }
        if (!any) return true;
        uint32_t prev = 0;
        for (uint32_t i = 0; i < nr; i++) {                          /* a run on the sentinel: all null */
            if (fk_val(vals, T, i) == sent && (!d->n_nulls || !fk_bits_all(bm, prev, ends->h[i]))) return false;
            prev = ends->h[i];
        }
    }
    return true;
}

/* A CONST / RLE vector of e's type into out (dec_fast's contract).  The
 * runs and their values are checked first, so a declined vector writes
 * nothing. */
static int dec_fast_runs(ray_enccol_t* e, const ray_enc_desc_t* d, int vlen, const uint8_t* body, uint64_t avail,
                         const uint8_t* bm, int8_t wide, ray_enc_kind_t k, void* out) {
    const int T = k.T;
    const bool sym = e->type == RAY_SYM;
    _Alignas(64) fk_vals_t vals, ends;
    uint32_t nr;
    if (!fk_runs(d->encoding, vlen, body, avail, T, &vals, &ends, &nr)) return 0;
    if (!fk_runs_ok(e, d, bm, k, &vals, &ends, nr)) return 0;
    /* A run of at most 8 rows away from the vector's end is filled with
     * one fixed 8-element store: the rows it writes past the run's end
     * belong to the runs after it, which are filled later and wholly, so
     * every row ends up holding its own run's value; nothing is written
     * past vlen. */
#define FK_FILL(OT, V) do { OT* o_ = (OT*)out; uint32_t p_ = 0;                          \
        for (uint32_t i = 0; i < nr; i++) {                                             \
            const OT v_ = (OT)(V); const uint32_t e_ = ends.h[i];                       \
            if (e_ - p_ <= 8 && p_ + 8 <= (uint32_t)vlen)                               \
                for (uint32_t z = 0; z < 8; z++) o_[p_ + z] = v_;                       \
            else                                                                        \
                for (uint32_t r = p_; r < e_; r++) o_[r] = v_;                          \
            p_ = e_; } } while (0)
    switch (T * 4 + (wide == RAY_I64 ? 1 : wide == RAY_F64 ? 2 : 0)) {
    case 8 * 4:      FK_FILL(uint8_t, vals.b[i]); break;
    case 8 * 4 + 1:  FK_FILL(int64_t, vals.b[i]); break;
    case 8 * 4 + 2:  FK_FILL(double, vals.b[i]); break;
    case 16 * 4:     FK_FILL(uint16_t, vals.h[i]); break;
    case 16 * 4 + 1: FK_FILL(int64_t, (int16_t)vals.h[i]); break;
    case 16 * 4 + 2: FK_FILL(double, (int16_t)vals.h[i]); break;
    case 32 * 4:     FK_FILL(uint32_t, vals.s[i]); break;
    case 32 * 4 + 1: FK_FILL(int64_t, (int32_t)vals.s[i]); break;
    case 32 * 4 + 2: FK_FILL(double, (int32_t)vals.s[i]); break;
    case 64 * 4:     FK_FILL(uint64_t, vals.q[i]); break;
    default: return 0;
    }
#undef FK_FILL
    if (sym || !k.is_signed || !d->n_nulls) return 1;
    /* the bitmap's rows become the null (a row outside it on the sentinel
     * was ruled out above; the merge flags one all the same) */
    if (!wide) return fk_nulls_native(d, T, vlen, bm, true, out);
    bool bad;
    if (wide == RAY_F64)
        bad = fk_mergef((double*)out, vlen, bm, T == 16 ? (double)NULL_I16 : (double)NULL_I32, NULL_F64);
    else
        bad = fk_merge64((int64_t*)out, vlen, bm, T == 16 ? (int64_t)NULL_I16 : (int64_t)NULL_I32, NULL_I64);
    return bad ? 0 : 1;
}

/* The values of a RAW body without a bitmap (or of an unsigned kind) as
 * dec_ints holds them: no sentinel (signed), SYM ids in the domain and the
 * statistics with n_nulls zeros. */
static bool fk_raw_ok(ray_enccol_t* e, const ray_enc_desc_t* d, int vlen, const uint8_t* body, ray_enc_kind_t k) {
    const int T = k.T;
    if (e->type == RAY_SYM) {
        uint64_t lo, span;
        fk_stats(d, T == 64 ? ~0ull : (1ull << T) - 1, &lo, &span);
        switch (T) {
        case 8:  return fk_sym8(body, vlen, e->sym_bound, d->n_nulls, (uint8_t)lo, (uint8_t)span);
        case 16: return fk_sym16((const uint16_t*)body, vlen, e->sym_bound, d->n_nulls, (uint16_t)lo, (uint16_t)span);
        case 32: return fk_sym32((const uint32_t*)body, vlen, e->sym_bound, d->n_nulls, (uint32_t)lo, (uint32_t)span);
        default: return fk_sym64((const uint64_t*)body, vlen, e->sym_bound, d->n_nulls, lo, span);
        }
    }
    if (!k.is_signed) return true;                                   /* BOOL, U8: no nulls */
    if (d->n_nulls) return false;                                    /* a bitmap: the merge decides */
    switch (T) {
    case 16: return !fk_scan16((const int16_t*)body, vlen, NULL_I16);
    case 32: return !fk_scan32((const int32_t*)body, vlen, NULL_I32);
    default: return !fk_scan64((const int64_t*)body, vlen, NULL_I64);
    }
}

/* A RAW vector of e's type at the generation-0 width into out: the size,
 * padding, sentinel (or SYM ids) checked as dec_ints checks them, the
 * values copied, the bitmap merged. */
static int dec_fast_raw(ray_enccol_t* e, const ray_enc_desc_t* d, int vlen, const uint8_t* body, uint64_t avail,
                        const uint8_t* bm, ray_enc_kind_t k, void* out) {
    const size_t nb = (size_t)vlen * k.T / 8;
    if (avail != (uint64_t)ray_enc_size_raw(vlen, k.T) || !all_zero(body + nb, (size_t)avail - nb)) return 0;
    if (!d->n_nulls || e->type == RAY_SYM || !k.is_signed) {
        if (!fk_raw_ok(e, d, vlen, body, k)) return 0;               /* checked in place, then copied */
        memcpy(out, body, nb);
        return 1;
    }
    memcpy(out, body, nb);
    return fk_nulls_native(d, k.T, vlen, bm, true, out);
}

/* Decode a FOR_BP / DELTA_BP / CONST / RLE vector body of e's type into
 * out: `wide` 0 at the generation-0 width, RAY_I64 / RAY_F64 widened with
 * nulls as NULL_I64 / NULL_F64 (narrow non-SYM types only).  DICT over a
 * FOR_BP code child (dec_fast_dict) and RAW (dec_fast_raw) at the
 * generation-0 width.  1 decoded, 0 declined (out may be partly written). */
static int dec_fast(ray_enccol_t* e, const ray_enc_desc_t* d, int vlen,
                    const uint8_t* body, uint64_t avail, const uint8_t* bm, int8_t wide, void* out) {
    const uint8_t enc = d->encoding;
    ray_enc_kind_t k;
    if (!fk_int_type(e->type) || !ray_enc_kind_of(e->type, e->attrs, &k)) return 0;
    if (enc == RAY_ENC_CONST || enc == RAY_ENC_RLE || enc == RAY_ENC_RAW) {
        if (d->width || vlen < 1 || vlen > RAY_ENC_VEC || ((uintptr_t)body & 7)) return 0;   /* a width only on BP */
        if (wide && (e->type == RAY_SYM || k.T == 64 || enc == RAY_ENC_RAW)) return 0;
        if ((uintptr_t)out & ((wide ? 8 : (size_t)k.T / 8) - 1)) return 0;
        return enc == RAY_ENC_RAW ? dec_fast_raw(e, d, vlen, body, avail, bm, k, out)
                                  : dec_fast_runs(e, d, vlen, body, avail, bm, wide, k, out);
    }
    if (enc != RAY_ENC_FOR_BP && enc != RAY_ENC_DELTA_BP && enc != RAY_ENC_DICT) return 0;
    if (enc == RAY_ENC_DICT) {
        if (wide || e->type == RAY_SYM || vlen < 1 || vlen > RAY_ENC_VEC) return 0;
        if (((uintptr_t)body & 7) || ((uintptr_t)out & ((size_t)k.T / 8 - 1))) return 0;
        return dec_fast_dict(e, d, vlen, body, avail, bm, k, out);
    }
    const int T = k.T, w = d->width, L = RAY_ENC_VEC / T;
    const bool sym = e->type == RAY_SYM, delta = enc == RAY_ENC_DELTA_BP;
    if (wide && (sym || T == 64)) return 0;
    if (w > T || vlen < 1 || vlen > RAY_ENC_VEC || (delta && !k.is_signed)) return 0;
    const size_t osz = wide ? 8 : (size_t)T / 8;
    if (((uintptr_t)body & 7) || ((uintptr_t)out & (osz - 1))) return 0;
    if (avail != (uint64_t)(delta ? ray_enc_size_delta(T, w) : ray_enc_size_for(w))) return 0;
    const uint64_t m = T == 64 ? ~0ull : (1ull << T) - 1;
    uint64_t base; memcpy(&base, body, 8);
    if (base & ~m) return 0;
    const uint8_t* pk = body + 8 + (delta ? 8 * (size_t)L : 0);
    union { uint8_t b[128]; uint16_t h[64]; uint32_t s[32]; uint64_t q[16]; } head;
    if (delta) {
        for (int l = 0; l < L; l++) {
            uint64_t x; memcpy(&x, body + 8 + 8 * (size_t)l, 8);
            if (x & ~m) return 0;                                    /* a head above the type width */
            switch (T) { case 16: head.h[l] = (uint16_t)x; break; case 32: head.s[l] = (uint32_t)x; break; default: head.q[l] = x; break; }
        }
        if (w > 0) {                                                 /* row 0 holds zero placeholders */
            const uint64_t wm = w >= 64 ? ~0ull : (1ull << w) - 1;
            uint64_t any = 0;
            switch (T) {
            case 16: { const uint16_t* a = (const uint16_t*)pk; for (int l = 0; l < L; l++) any |= a[l] & wm; break; }
            case 32: { const uint32_t* a = (const uint32_t*)pk; for (int l = 0; l < L; l++) any |= a[l] & wm; break; }
            default: { const uint64_t* a = (const uint64_t*)pk; for (int l = 0; l < L; l++) any |= a[l] & wm; break; }
            }
            if (any) return 0;
        }
    }
    const int8_t wt = wide;
    const uint64_t sent = k.is_signed ? (uint64_t)1 << (T - 1) : 0;   /* the sentinel's T-bit pattern */
    uint64_t hit = 0;                                                    /* widening kernels: a value on it */
    /* FOR_BP values are base + u (mod 2^T), u < 2^w: none is the sentinel
     * unless sentinel - base (mod 2^T) is such a u.  Then no check is
     * needed, from the body alone (never from the untrusted statistics). */
    const uint64_t umax = w >= 64 ? ~0ull : (1ull << w) - 1;
    const bool can_hit = k.is_signed && (delta || ((sent - base) & m) <= umax);
#define FK_RUN(FOR, DLT, UT, OT) do {                                                    \
        if (delta) hit = DLT((const UT*)pk, w, (UT)base, (const UT*)head.b, (OT*)out, vlen, (UT)sent); \
        else       hit = FOR((const UT*)pk, w, (UT)base, (OT*)out, vlen, (UT)sent); } while (0)
#define FK_RUN_FOR(FOR, UT, OT) hit = FOR((const UT*)pk, w, (UT)base, (OT*)out, vlen, (UT)sent)
    switch (T * 4 + (wt == RAY_I64 ? 1 : wt == RAY_F64 ? 2 : 0)) {
    case 8 * 4:      FK_RUN_FOR(fk_for8, uint8_t, uint8_t); break;
    case 8 * 4 + 1:  FK_RUN_FOR(fk_for8_i, uint8_t, int64_t); break;
    case 8 * 4 + 2:  FK_RUN_FOR(fk_for8_f, uint8_t, double); break;
    case 16 * 4:     FK_RUN(fk_for16, fk_dlt16, uint16_t, uint16_t); break;
    case 16 * 4 + 1: if (can_hit) FK_RUN(fk_for16_i, fk_dlt16_i, uint16_t, int64_t); else FK_RUN_FOR(fk_for16_in, uint16_t, int64_t); break;
    case 16 * 4 + 2: if (can_hit) FK_RUN(fk_for16_f, fk_dlt16_f, uint16_t, double); else FK_RUN_FOR(fk_for16_fn, uint16_t, double); break;
    case 32 * 4:     FK_RUN(fk_for32, fk_dlt32, uint32_t, uint32_t); break;
    case 32 * 4 + 1: if (can_hit) FK_RUN(fk_for32_i, fk_dlt32_i, uint32_t, int64_t); else FK_RUN_FOR(fk_for32_in, uint32_t, int64_t); break;
    case 32 * 4 + 2: if (can_hit) FK_RUN(fk_for32_f, fk_dlt32_f, uint32_t, double); else FK_RUN_FOR(fk_for32_fn, uint32_t, double); break;
    case 64 * 4:     FK_RUN(fk_for64, fk_dlt64, uint64_t, uint64_t); break;
    default: return 0;
    }
#undef FK_RUN
#undef FK_RUN_FOR
    if (sym) {
        /* the ids inside the domain and the vector's statistics */
        uint64_t lo, span;
        fk_stats(d, m, &lo, &span);
        bool ok;
        switch (T) {
        case 8:  ok = fk_sym8((const uint8_t*)out, vlen, e->sym_bound, d->n_nulls, (uint8_t)lo, (uint8_t)span); break;
        case 16: ok = fk_sym16((const uint16_t*)out, vlen, e->sym_bound, d->n_nulls, (uint16_t)lo, (uint16_t)span); break;
        case 32: ok = fk_sym32((const uint32_t*)out, vlen, e->sym_bound, d->n_nulls, (uint32_t)lo, (uint32_t)span); break;
        default: ok = fk_sym64((const uint64_t*)out, vlen, e->sym_bound, d->n_nulls, lo, span); break;
        }
        return ok ? 1 : 0;
    }
    if (!k.is_signed) return 1;                                      /* BOOL, U8: no nulls */
    if (!wt) return fk_nulls_native(d, T, vlen, bm, can_hit, out);
    if (d->n_nulls && !bm) return 0;
    if (!d->n_nulls) return hit ? 0 : 1;                             /* flagged by the widening kernel, or impossible */
    bool bad;
    if (wt == RAY_F64)
        bad = fk_mergef((double*)out, vlen, bm, T == 16 ? (double)NULL_I16 : (double)NULL_I32, NULL_F64);
    else
        bad = fk_merge64((int64_t*)out, vlen, bm, T == 16 ? (int64_t)NULL_I16 : (int64_t)NULL_I32, NULL_I64);
    return bad ? 0 : 1;
}

/* The shared vector decode.  priv: `out` is the caller's private
 * destination, so the integer fast path writes it directly (a failed decode
 * may leave it partly written); otherwise the vector is decoded aside and
 * `out` written only once it is fully validated. */
/* The reference decoders by type, into out (vlen elements). */
static ray_t* dec_ref(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d, int vlen,
                      const uint8_t* body, uint64_t avail, const uint8_t* bm, void* out) {
    switch (e->type) {
    case RAY_F32: case RAY_F64: return dec_floats(e, c, j, d, vlen, body, avail, bm, out);
    case RAY_GUID:              return dec_guid(e, c, j, d, vlen, body, avail, out);
    default:                    return dec_ints(e, c, j, d, vlen, body, avail, bm, out);
    }
}

/* RAW float vector j of chunk c without nulls or trailer: the checks
 * dec_floats makes on it, without the copy. */
static bool raw_float_vec_ok(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d) {
    int64_t rows = chunk_rows(e, c);
    int vlen = (int)(rows - (int64_t)j * RAY_ENC_VEC < RAY_ENC_VEC ? rows - (int64_t)j * RAY_ENC_VEC : RAY_ENC_VEC);
    const uint8_t* p = NULL; uint64_t avail = 0; const uint8_t* bm = NULL;
    ray_t* err = payload_parts(e, c, j, d, vlen, &p, &avail, &bm);
    if (err) { ray_error_free(err); return false; }
    const size_t esz = e->type == RAY_F32 ? 4 : 8;
    uint64_t body = ray_enc_pad8((uint64_t)vlen * esz);
    if (bm || body != avail || ((uintptr_t)p & 7)) return false;
    if (!all_zero(p + (size_t)vlen * esz, (size_t)(body - (uint64_t)vlen * esz))) return false;
    /* and every value inside the vector's statistics, as dec_floats checks */
    const bool has_st = !(d->flags & RAY_ENC_STAT_ABSENT);
    double smin = -INFINITY, smax = INFINITY;
    if (has_st) { memcpy(&smin, &d->min, 8); memcpy(&smax, &d->max, 8); }
    if (esz == 4) {
        for (int i = 0; i < vlen; i++) { float f; memcpy(&f, p + 4 * i, 4); if (f != f || f < smin || f > smax) return false; }
    } else {
        for (int i = 0; i < vlen; i++) { double x; memcpy(&x, p + 8 * i, 8); if (x != x || x < smin || x > smax) return false; }
    }
    return true;
}

/* RAW integer vector j of chunk c without nulls: the checks dec_ints
 * makes on it (size, padding, sentinel, SYM ids), without the copy. */
static bool raw_int_vec_ok(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d) {
    ray_enc_kind_t k;
    if (!fk_int_type(e->type) || !ray_enc_kind_of(e->type, e->attrs, &k)) return false;
    int64_t rows = chunk_rows(e, c);
    int vlen = (int)(rows - (int64_t)j * RAY_ENC_VEC < RAY_ENC_VEC ? rows - (int64_t)j * RAY_ENC_VEC : RAY_ENC_VEC);
    const uint8_t* p = NULL; uint64_t avail = 0; const uint8_t* bm = NULL;
    ray_t* err = payload_parts(e, c, j, d, vlen, &p, &avail, &bm);
    if (err) { ray_error_free(err); return false; }
    const size_t nb = (size_t)vlen * k.T / 8;
    if (bm || ((uintptr_t)p & 7) || avail != (uint64_t)ray_enc_size_raw(vlen, k.T) || !all_zero(p + nb, (size_t)avail - nb))
        return false;
    return fk_raw_ok(e, d, vlen, p, k);
}

static bool raw_view_eligible(const ray_enc_desc_t* d) {
    return d->encoding == RAY_ENC_RAW && d->n_nulls == 0 && !(d->flags & RAY_ENC_DESC_TRAILER);
}

const void* ray_enccol_raw_view(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d) {
    if (!e->raw_ok || e->kind != RAY_ENCCOL_SINGLE || !raw_view_eligible(d)) return NULL;
    if (!atomic_load_explicit(&e->raw_ok[c], memory_order_acquire)) {
        const ray_enc_desc_t* descs;
        ray_t* err = ray_enccol_chunk_descs_ref(e, c, &descs);
        if (err) { ray_error_free(err); return NULL; }
        const bool flt = enc_type_float(e->type);
        int nv = (int)((chunk_rows(e, c) + RAY_ENC_VEC - 1) / RAY_ENC_VEC);
        for (int v = 0; v < nv; v++)
            if (raw_view_eligible(&descs[v]) && !(flt ? raw_float_vec_ok(e, c, v, &descs[v]) : raw_int_vec_ok(e, c, v, &descs[v])))
                return NULL;
        atomic_store_explicit(&e->raw_ok[c], 1, memory_order_release);
    }
    ray_enc_dirent_t de; dirent_at(e, c, &de);
    return e->base + de.offset + d->payload_off;      /* no bitmap: n_nulls == 0 */
}

static ray_t* dec_aside(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d, int vlen,
                        const uint8_t* body, uint64_t avail, const uint8_t* bm, void* out,
                        size_t nbytes, bool try_fast) __attribute__((noinline));
static ray_t* dec_aside(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d, int vlen,
                        const uint8_t* body, uint64_t avail, const uint8_t* bm, void* out,
                        size_t nbytes, bool try_fast) {
    _Alignas(8) uint8_t tmp[RAY_ENC_VEC * 16];      /* the fast kernel needs 8-aligned output */
    ray_t* err;
    if (try_fast && dec_fast(e, d, vlen, body, avail, bm, 0, tmp)) {
        memcpy(out, tmp, nbytes);
        return NULL;
    }
    err = dec_ref(e, c, j, d, vlen, body, avail, bm, tmp);
    if (!err) memcpy(out, tmp, nbytes);
    return err;
}

static ray_t* decode_vec(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d,
                         void* out, char* pool_out, uint64_t pool_base, uint64_t pool_cap,
                         bool priv, uint64_t* produced) {
    int64_t rows = chunk_rows(e, c);
    int vlen = (int)(rows - (int64_t)j * RAY_ENC_VEC < RAY_ENC_VEC ? rows - (int64_t)j * RAY_ENC_VEC : RAY_ENC_VEC);
    const uint8_t* body = NULL; uint64_t avail = 0; const uint8_t* bm = NULL;
    *produced = 0;
    ray_t* err = payload_parts(e, c, j, d, vlen, &body, &avail, &bm);
    if (err) return err;
    uint64_t pooled = 0;
    const size_t nbytes = (size_t)vlen * ray_enccol_esz(e);
    if (e->type == RAY_STR) {
        /* validate and size first, so a corrupt vector writes nothing */
        err = dec_str(e, c, j, d, vlen, body, avail, NULL, NULL, 0, 0, &pooled);
        if (err) return err;
        if (pooled > pool_cap || pool_base > pool_cap - pooled || pool_base + pooled > UINT32_MAX)
            VEC_CORRUPT("pool overflow");
        err = dec_str(e, c, j, d, vlen, body, avail, out, pool_out, pool_base, pool_cap, NULL);
    } else if (priv) {
        /* a private destination may hold a partial vector on failure (it is
         * never returned): decode straight into it, no aside copy */
        if (!dec_fast(e, d, vlen, body, avail, bm, 0, out)) err = dec_ref(e, c, j, d, vlen, body, avail, bm, out);
    } else {
        err = dec_aside(e, c, j, d, vlen, body, avail, bm, out, nbytes, true);
    }
    if (!err) *produced = (uint64_t)nbytes + pooled;
    return err;
}

void ray_enccol_note_decoded(ray_enccol_t* e, uint64_t nb) {
    if (!nb) return;
    atomic_fetch_add_explicit(&g_enc_decoded_bytes, nb, memory_order_relaxed);
    atomic_fetch_add_explicit(&e->decoded_bytes, nb, memory_order_relaxed);   /* per column */
}

ray_t* ray_enccol_decode_desc(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d,
                              void* out, char* pool_out, uint64_t pool_base, uint64_t pool_cap) {
    uint64_t nb;
    ray_t* err = decode_vec(e, c, j, d, out, pool_out, pool_base, pool_cap, false, &nb);
    if (!err) ray_enccol_note_decoded(e, nb);
    return err;
}

ray_t* ray_enccol_decode_into(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d,
                              void* out, char* pool_out, uint64_t pool_base, uint64_t pool_cap,
                              uint64_t* produced) {
    return decode_vec(e, c, j, d, out, pool_out, pool_base, pool_cap, true, produced);
}

int ray_enccol_decode_wide(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d,
                           int8_t wide, void* out, uint64_t* produced) {
    *produced = 0;
    if ((wide != RAY_I64 && wide != RAY_F64) || !fk_int_type(e->type) || e->type == RAY_SYM ||
        ray_enccol_esz(e) == 8)
        return 0;
    int64_t rows = chunk_rows(e, c);
    int vlen = (int)(rows - (int64_t)j * RAY_ENC_VEC < RAY_ENC_VEC ? rows - (int64_t)j * RAY_ENC_VEC : RAY_ENC_VEC);
    const uint8_t* body = NULL; uint64_t avail = 0; const uint8_t* bm = NULL;
    ray_t* err = payload_parts(e, c, j, d, vlen, &body, &avail, &bm);
    if (err) { ray_error_free(err); return 0; }          /* the range read reports it */
    if (!dec_fast(e, d, vlen, body, avail, bm, wide, out)) return 0;
    *produced = (uint64_t)vlen * ray_enccol_esz(e);        /* counted as the generation-0 bytes */
    return 1;
}

/* ---- Point reads: one row of a packed vector without decoding it ----
 * Value i of a packed body is lane i % L, row i / L (L = 1024 / T): its w
 * bits start at bit (i / L) * w of the lane, in word (bit / T) * L + lane,
 * continued in the lane's next word when they cross a T-bit boundary.
 * The body is ray_enc_size_for(w) bytes, so every word read is inside it. */
static uint64_t point_field(const uint8_t* pk, int T, int w, int i) {
    if (w == 0) return 0;
    const int L = RAY_ENC_VEC / T, bit = (i / L) * w, off = bit % T;
    const size_t at = (size_t)(bit / T) * (size_t)L + (size_t)(i % L), tb = (size_t)T / 8;
    uint64_t lo = 0, hi = 0;
    memcpy(&lo, pk + at * tb, tb);                         /* little-endian: the low tb bytes */
    if (off + w > T) memcpy(&hi, pk + (at + (size_t)L) * tb, tb);
    uint64_t v = lo >> off;
    if (off + w > T) v |= hi << (T - off);
    return w >= 64 ? v : v & ((1ull << w) - 1);
}

/* Row i of a CONST / RLE body: its run's T-bit value into *x, the runs
 * checked as the run kernel checks them (fk_runs) and the run found by
 * binary search over the ends.  Out of line: its 16 KB of run buffers stay
 * off the FOR_BP / DICT point read's frame. */
static __attribute__((noinline)) bool point_run(uint8_t enc, const uint8_t* p, uint64_t avail, int vlen, int i,
                                                int T, uint64_t* x) {
    if (vlen < 1 || vlen > RAY_ENC_VEC || i < 0 || i >= vlen || ((uintptr_t)p & 7)) return false;
    _Alignas(64) fk_vals_t vals, ends;
    uint32_t nr;
    if (!fk_runs(enc, vlen, p, avail, T, &vals, &ends, &nr)) return false;
    uint32_t lo = 0, hi = nr - 1;                          /* the first run whose end is above i */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (ends.h[mid] > (uint32_t)i) hi = mid; else lo = mid + 1;
    }
    *x = fk_val(&vals, T, lo);
    return true;
}

/* Row i of vector body p (avail bytes, descriptor d, vlen rows) as its
 * T-bit pattern: 1 served; 0 declined when the vector has nulls (the
 * bitmap decides) or the body is not one this read proves (any codec but
 * FOR_BP, DICT over a FOR_BP code child, CONST and RLE; a size, width,
 * base or reserved field off, a code outside the dictionary, runs the run
 * kernel's checks reject, the sentinel), so the vector decode decides; -1
 * the dictionary's error.  CONST / RLE: the runs are read and checked as
 * the run kernel reads them (fk_runs: headers, children, sizes, padding,
 * ends strictly ascending to vlen), then row i's run is found by binary
 * search over the ends; no row is expanded. */
static int point_body(ray_enccol_t* e, const ray_enc_desc_t* d, const uint8_t* p, uint64_t avail,
                      int vlen, int i, ray_enc_kind_t k, uint64_t* x, ray_t** err) {
    if (d->n_nulls) return 0;
    const uint64_t m = k.T == 64 ? ~0ull : (1ull << k.T) - 1;
    uint64_t v;
    if (d->encoding == RAY_ENC_FOR_BP) {
        const int w = d->width;
        if (w > k.T || avail != (uint64_t)ray_enc_size_for(w)) return 0;
        uint64_t base; memcpy(&base, p, 8);
        if (base & ~m) return 0;
        v = (base + point_field(p + 8, k.T, w, i)) & m;
    } else if (d->encoding == RAY_ENC_DICT && d->width == 0) {
        /* the codes child: an 8-byte header (encoding, width, reserved,
         * body bytes), then a U32 FOR_BP body filling the payload */
        if (avail < 16 || p[0] != RAY_ENC_FOR_BP) return 0;
        const int w = p[1];
        uint16_t rsv; memcpy(&rsv, p + 2, 2);
        uint32_t bb; memcpy(&bb, p + 4, 4);
        if (rsv || w > 32 || (uint64_t)bb + 8 != avail || bb != (uint64_t)ray_enc_size_for(w)) return 0;
        uint64_t base; memcpy(&base, p + 8, 8);
        if (base >> 32) return 0;
        uint64_t code = (base + point_field(p + 16, 32, w, i)) & 0xffffffffull;
        ray_enc_dict_t dict;
        if ((*err = enc_dict(e, &dict)) != NULL) return -1;
        if (code >= dict.n) return 0;
        v = dict.v[code] & m;
    } else if ((d->encoding == RAY_ENC_CONST || d->encoding == RAY_ENC_RLE) && d->width == 0) {
        if (!point_run(d->encoding, p, avail, vlen, i, k.T, &v)) return 0;
        v &= m;
    } else {
        return 0;
    }
    if (k.is_signed && v == (uint64_t)1 << (k.T - 1)) return 0;   /* the sentinel outside a bitmap */
    *x = v;
    return 1;
}

static bool point_type(const ray_enccol_t* e, ray_enc_kind_t* k) {
    return fk_int_type(e->type) && e->type != RAY_SYM && ray_enc_kind_of(e->type, e->attrs, k);
}

/* Row i of vector j of chunk c (descriptor d, body p of avail bytes) as
 * its logical value, checked against the vector's statistics (and so its
 * chunk's): 1 served, 0 declined, -1 with *err. */
static int point_read(ray_enccol_t* e, const ray_enc_desc_t* d, const uint8_t* p, uint64_t avail,
                      int64_t c, int j, int vlen, int i, ray_enc_kind_t k, int64_t* out, ray_t** err) {
    uint64_t x;
    int r = point_body(e, d, p, avail, vlen, i, k, &x, err);
    if (r != 1) return r;
    const int64_t v = ray_enc_logical(x, k);
    if (!(d->flags & RAY_ENC_STAT_ABSENT) && (v < d->min || v > d->max)) {
        *err = ray_error("corrupt", "column %s: chunk %lld vector %d value outside its statistics", e->path, (long long)c, j);
        return -1;
    }
    *out = v;
    return 1;
}

int ray_enccol_point_i64(ray_enccol_t* e, int64_t row, int64_t* out, ray_t** err) {
    *err = NULL;
    ray_enc_kind_t k;
    if (e->kind != RAY_ENCCOL_SINGLE || row < 0 || row >= e->len || !point_type(e, &k)) return 0;
    const int64_t c = row / RAY_ENC_CHUNK;
    const int j = (int)(row % RAY_ENC_CHUNK / RAY_ENC_VEC), i = (int)(row % RAY_ENC_VEC);
    const ray_enc_desc_t* descs;
    if ((*err = ray_enccol_chunk_descs_ref(e, c, &descs)) != NULL) return -1;   /* validated once per chunk */
    const ray_enc_desc_t* d = &descs[j];
    ray_enc_dirent_t de; dirent_at(e, c, &de);
    const int64_t rows = chunk_rows(e, c);
    const int vlen = (int)(rows - (int64_t)j * RAY_ENC_VEC < RAY_ENC_VEC ? rows - (int64_t)j * RAY_ENC_VEC : RAY_ENC_VEC);
    return point_read(e, d, e->base + de.offset + d->payload_off, d->payload_bytes, c, j, vlen, i, k, out, err);
}

/* ---- Batched point reads: the rows of one vector, its body read once ----
 * The most rows of one vector a batched point read serves, per codec:
 * past it one decode is the cheaper read (open decision D2, re-measured
 * and extended per codec in Task 10b).  Release microbenchmark over the
 * 10M ClickBench store's vectors, warm: one cursor range read of a vector
 * against one batched point read of N rows spread over it, as a fraction
 * of the read (crossover: where it reaches 1):
 *   FOR_BP          decode 140-420 ns  N=16 0.30-0.77, N=32 0.47-1.33   -> RAY_ENC_VEC / 32
 *   RLE             decode 200-1100 ns N=32 0.68-1.20, N=64 0.99-1.91   -> RAY_ENC_VEC / 16
 *   CONST           decode ~100 ns     N=8  0.79-0.86, N=16 1.18-1.33   -> RAY_ENC_VEC / 16, as
 *                   ruled for runs (the warm crossover is ~10 rows; the group
 *                   key reads pinned at 10-26 rows per vector rely on it)
 *   DICT            decode 660-1350 ns N=64 0.30-0.61                   -> RAY_ENC_VEC / 16
 *   RAW, viewed     a view, 34-38 ns   N=1  1.05-1.26                   -> declined
 *   RAW, chunk not yet validated: the view's first read checks every RAW
 *                   vector of the chunk (0.4-0.9 us each, ~60 us a chunk),
 *                   so a few rows are point-read; N=8 costs 2.8 views   -> RAY_ENC_VEC / 128
 *                   This one is a heuristic around that one-time cost, not
 *                   a D2 crossover: a workload that only ever reads a few
 *                   rows of a chunk never validates it and pays the point
 *                   read (1.05-2.8 views) on every warm read.  It also
 *                   depends on timing: in a parallel read another task may
 *                   validate the chunk through a view first, so a vector's
 *                   decoded bytes (0 or the vector's) vary between runs.
 *                   Answers do not: both reads deliver the same values.
 *   with a bitmap   decode 1.8-2.4 us  N=64 0.17-0.25 (every codec)     -> RAY_ENC_VEC / 16
 * SYM vectors took the same limits (FOR_BP N=32 0.52-0.55, RLE N=64
 * 1.01-1.87, CONST N=8 0.79, N=64 3.2).  RAY_ENC_VEC / 16 is RAY_COL_POINT_ROWS,
 * the most rows a caller hands one call.  Structural ratios, not knobs. */
#define PT_ROWS_FOR   (RAY_ENC_VEC / 32)
#define PT_ROWS_RUNS  (RAY_ENC_VEC / 16)
#define PT_ROWS_CONST (RAY_ENC_VEC / 16)
#define PT_ROWS_DICT  (RAY_ENC_VEC / 16)
#define PT_ROWS_NULLS (RAY_ENC_VEC / 16)
#define PT_ROWS_RAW   (RAY_ENC_VEC / 128)
_Static_assert(PT_ROWS_RUNS <= RAY_COL_POINT_ROWS && PT_ROWS_DICT <= RAY_COL_POINT_ROWS &&
               PT_ROWS_NULLS <= RAY_COL_POINT_ROWS && PT_ROWS_FOR <= RAY_COL_POINT_ROWS &&
               PT_ROWS_CONST <= RAY_COL_POINT_ROWS && PT_ROWS_RAW <= RAY_COL_POINT_ROWS,
               "a codec's point-read limit is at most the caller's batch");

/* What every served row of one vector is checked against: its kind, the
 * descriptor's statistics, the bitmap (NULL: the vector has none) and, for
 * SYM, the domain bound. */
typedef struct {
    ray_enccol_t*         e;
    const ray_enc_desc_t* d;
    const uint8_t*        bm;      /* the validated null bitmap, or NULL */
    uint8_t*              nul;     /* the caller's null flags (NULL: none taken) */
    ray_enc_kind_t        k;
    uint64_t              m;       /* the lane mask */
    uint64_t              sent;    /* a signed kind's sentinel (none for unsigned kinds) */
    int64_t               c;
    int                   j;
    bool                  sym;
} pt_ctx_t;

/* Row i (output q) in the bitmap: a null, flagged; nothing else of the row
 * is read (a null slot's value or code may be anything the writer chose). */
static inline __attribute__((always_inline)) bool pt_null(const pt_ctx_t* t, int i, int64_t q, int64_t* out) {
    if (!t->bm || !((t->bm[i >> 3] >> (i & 7)) & 1)) return false;
    t->nul[q] = 1;
    out[q] = 0;
    return true;
}

/* The T-bit pattern x of a row outside the bitmap as its logical value:
 * 1 served, 0 declined (the decode reports it: a sentinel outside the
 * bitmap, a SYM id at or above the domain bound), -1 a value outside the
 * vector's statistics (`corrupt`, which the decode of a FOR_BP, RAW or
 * run vector does not check).  SYM id 0 is the in-band null and is served
 * as itself.  So every value served lies inside its vector's statistics,
 * and those inside the chunk's (descriptor validation): a stats_guard
 * source needs no further check.  The bound matters where statistics are
 * absent: present SYM statistics lie below it (descriptor validation). */
static inline __attribute__((always_inline)) int pt_val(const pt_ctx_t* t, uint64_t x, int64_t q, int64_t* out, ray_t** err) {
    const ray_enc_desc_t* d = t->d;
    if (t->sym) {
        if (x == 0) { out[q] = 0; return 1; }
        if (x >= t->e->sym_bound) return 0;
    } else if (x == t->sent) {
        return 0;
    }
    const int sh = 64 - t->k.T;
    const int64_t v = t->k.is_signed ? (int64_t)(x << sh) >> sh : (int64_t)x;
    if (!(d->flags & RAY_ENC_STAT_ABSENT) && (v < d->min || v > d->max)) {
        *err = ray_error("corrupt", "column %s: chunk %lld vector %d value outside its statistics",
                         t->e->path, (long long)t->c, t->j);
        return -1;
    }
    out[q] = v;
    return 1;
}

/* Rows of one CONST / RLE vector (local rows rows[q] - base, ascending)
 * read off its runs, checked as point_body and point_read check them:
 * the runs read once (fk_runs), each row's run found by advancing over
 * the ends.  Out of line, as point_run.  1 / 0 / -1 as point_read. */
static __attribute__((noinline)) int points_runs(const pt_ctx_t* t, const uint8_t* p, uint64_t avail, int vlen,
                                                 const int64_t* rows, int64_t n, int64_t base, int64_t* out,
                                                 ray_t** err) {
    if (vlen < 1 || vlen > RAY_ENC_VEC || ((uintptr_t)p & 7)) return 0;
    _Alignas(64) fk_vals_t vals, ends;
    uint32_t nr;
    if (!fk_runs(t->d->encoding, vlen, p, avail, t->k.T, &vals, &ends, &nr)) return 0;
    uint32_t r = 0;
    for (int64_t q = 0; q < n; q++) {
        const int64_t i = rows[q] - base;
        if (pt_null(t, (int)i, q, out)) continue;
        while (r + 1 < nr && ends.h[r] <= (uint32_t)i) r++;
        const int pr = pt_val(t, fk_val(&vals, t->k.T, r) & t->m, q, out, err);
        if (pr != 1) return pr;
    }
    return 1;
}

/* point_field at a fixed lane width UT: L = 1024 / T lanes (LS = log2 L,
 * TS = log2 T), the divisions as shifts and masks, fixed-size loads. */
#define PT_FIELD(NAME, UT, LS, TS)                                                               \
static inline __attribute__((always_inline)) uint64_t NAME(const UT* lanes, int w, uint64_t wm, int i) { \
    const int T = 8 * (int)sizeof(UT);                                                           \
    const int bit = (i >> (LS)) * w, off = bit & (T - 1);                                        \
    const size_t at = (size_t)(bit >> (TS)) * ((size_t)1 << (LS)) + (size_t)(i & ((1 << (LS)) - 1)); \
    UT lo, hi;                                                                                   \
    memcpy(&lo, lanes + at, sizeof lo);                                                          \
    uint64_t f = (uint64_t)lo >> off;                                                            \
    if (off + w > T) { memcpy(&hi, lanes + at + ((size_t)1 << (LS)), sizeof hi); f |= (uint64_t)hi << (T - off); } \
    return f & wm;                                                                               \
}
PT_FIELD(pt_field8,  uint8_t,  7, 3)
PT_FIELD(pt_field16, uint16_t, 6, 4)
PT_FIELD(pt_field32, uint32_t, 5, 5)
PT_FIELD(pt_field64, uint64_t, 4, 6)
#undef PT_FIELD

/* Rows of one FOR_BP vector: point_body's FOR_BP checks once, then the
 * lane arithmetic per row. */
static int points_for(const pt_ctx_t* t, const uint8_t* p, uint64_t avail, const int64_t* rows, int64_t n,
                      int64_t base, int64_t* out, ray_t** err) {
    const int w = t->d->width, T = t->k.T;
    if (w > T || avail != (uint64_t)ray_enc_size_for(w)) return 0;
    uint64_t fb; memcpy(&fb, p, 8);
    if (fb & ~t->m) return 0;
    const uint8_t* pk = p + 8;
    const uint64_t wm = w >= 64 ? ~0ull : (1ull << w) - 1;
#define FOR_POINTS(UT, FIELD) do {                                                               \
        const UT* lanes = (const UT*)(const void*)pk;                                            \
        for (int64_t q = 0; q < n; q++) {                                                        \
            const int i = (int)(rows[q] - base);                                                 \
            if (pt_null(t, i, q, out)) continue;                                                 \
            const uint64_t f = w ? FIELD(lanes, w, wm, i) : 0;                                   \
            const int pr = pt_val(t, (fb + f) & t->m, q, out, err);                              \
            if (pr != 1) return pr;                                                              \
        }                                                                                        \
    } while (0)
    switch (T) {
    case 8:  FOR_POINTS(uint8_t,  pt_field8);  break;
    case 16: FOR_POINTS(uint16_t, pt_field16); break;
    case 32: FOR_POINTS(uint32_t, pt_field32); break;
    default: FOR_POINTS(uint64_t, pt_field64); break;
    }
#undef FOR_POINTS
    return 1;
}

/* Rows of one DICT vector over a U32 FOR_BP code child: point_body's
 * child checks once and the dictionary validated (once per column, its
 * errors -1), then per row the code unpacked at its lane, checked below
 * the dictionary size (else declined: the decode reports the code) and
 * its entry read. */
static int points_dict(const pt_ctx_t* t, const uint8_t* p, uint64_t avail, const int64_t* rows, int64_t n,
                       int64_t base, int64_t* out, ray_t** err) {
    if (avail < 16 || p[0] != RAY_ENC_FOR_BP) return 0;
    const int w = p[1];
    uint16_t rsv; memcpy(&rsv, p + 2, 2);
    uint32_t bb; memcpy(&bb, p + 4, 4);
    if (rsv || w > 32 || (uint64_t)bb + 8 != avail || bb != (uint64_t)ray_enc_size_for(w)) return 0;
    uint64_t cb; memcpy(&cb, p + 8, 8);
    if (cb >> 32) return 0;
    ray_enc_dict_t dict;
    if ((*err = enc_dict(t->e, &dict)) != NULL) return -1;
    const uint32_t* lanes = (const uint32_t*)(const void*)(p + 16);
    const uint64_t wm = w >= 64 ? ~0ull : (1ull << w) - 1;
    for (int64_t q = 0; q < n; q++) {
        const int i = (int)(rows[q] - base);
        if (pt_null(t, i, q, out)) continue;
        const uint64_t code = (cb + (w ? pt_field32(lanes, w, wm, i) : 0)) & 0xffffffffull;
        if (code >= dict.n) return 0;
        const int pr = pt_val(t, dict.v[code] & t->m, q, out, err);
        if (pr != 1) return pr;
    }
    return 1;
}

/* Rows of one RAW vector: the body's size checked once (dec_fast_raw's
 * rule; the padding and the other rows are not read), then each row's
 * T-bit element. */
static int points_raw(const pt_ctx_t* t, const uint8_t* p, uint64_t avail, int vlen, const int64_t* rows,
                      int64_t n, int64_t base, int64_t* out, ray_t** err) {
    if (avail != (uint64_t)ray_enc_size_raw(vlen, t->k.T)) return 0;
    const size_t tb = (size_t)t->k.T / 8;
    for (int64_t q = 0; q < n; q++) {
        const int i = (int)(rows[q] - base);
        if (pt_null(t, i, q, out)) continue;
        uint64_t x = 0;
        memcpy(&x, p + (size_t)i * tb, tb);                     /* little-endian: the low tb bytes */
        const int pr = pt_val(t, x, q, out, err);
        if (pr != 1) return pr;
    }
    return 1;
}

/* ray_enccol_points_i64 and ray_enccol_gather_points: nul NULL declines
 * SYM and vectors with a null bitmap, else both are served (nul[q] set for
 * a bitmap row). */
static int points_core(ray_enccol_t* e, const int64_t* rows, int64_t n, int64_t base, int64_t* out, uint8_t* nul,
                       ray_t** err) {
    *err = NULL;
    ray_enc_kind_t k;
    if (n <= 0 || e->kind != RAY_ENCCOL_SINGLE || !fk_int_type(e->type) || !ray_enc_kind_of(e->type, e->attrs, &k))
        return 0;
    const bool sym = e->type == RAY_SYM;
    if (sym && !nul) return 0;                                   /* ids only to a caller that takes them */
    const int64_t r0 = rows[0] - base;
    if (r0 < 0 || r0 >= e->len) return 0;
    const int64_t c = r0 / RAY_ENC_CHUNK;
    const int j = (int)(r0 % RAY_ENC_CHUNK / RAY_ENC_VEC);
    const int64_t vs = c * RAY_ENC_CHUNK + (int64_t)j * RAY_ENC_VEC;
    const int64_t rows_c = chunk_rows(e, c);
    const int vlen = (int)(rows_c - (int64_t)j * RAY_ENC_VEC < RAY_ENC_VEC ? rows_c - (int64_t)j * RAY_ENC_VEC : RAY_ENC_VEC);
    for (int64_t q = 0; q < n; q++)                        /* one vector, ascending: the caller's contract */
        if (rows[q] - base < vs || rows[q] - base >= vs + vlen || (q && rows[q] < rows[q - 1])) return 0;
    const ray_enc_desc_t* descs;
    if ((*err = ray_enccol_chunk_descs_ref(e, c, &descs)) != NULL) return -1;   /* validated once per chunk */
    const ray_enc_desc_t* d = &descs[j];
    ray_enc_dirent_t de; dirent_at(e, c, &de);
    const uint8_t* p = e->base + de.offset + d->payload_off;
    uint64_t avail = d->payload_bytes;
    pt_ctx_t t = { .e = e, .d = d, .bm = NULL, .nul = nul, .k = k,
                   .m = k.T == 64 ? ~0ull : (1ull << k.T) - 1,
                   .sent = k.is_signed ? (uint64_t)1 << (k.T - 1) : ~0ull, .c = c, .j = j, .sym = sym };
    if (d->n_nulls && !sym) {
        /* the bitmap as payload_parts checks it: present, its count and
         * a clear tail (a word at a time: popcount_prefix's bit loop cost
         * more than the rows); anything off is the decode's to report */
        if (!nul || !ray_enc_has_bitmap(e->type) || avail < 128) return 0;
        uint32_t cnt = 0;
        uint64_t tail = 0;
        for (int w = 0; w < RAY_ENC_VEC / 64; w++) {
            uint64_t x; memcpy(&x, p + 8 * w, 8);
            const int lo = 64 * w;
            const uint64_t keep = vlen >= lo + 64 ? ~0ull : vlen <= lo ? 0 : (1ull << (vlen - lo)) - 1;
            cnt += (uint32_t)__builtin_popcountll(x & keep);
            tail |= x & ~keep;
        }
        if (cnt != (uint32_t)d->n_nulls || tail) return 0;
        t.bm = p; p += 128; avail -= 128;
    }
    /* the codec's limit (above): a vector with a bitmap decodes through
     * the merge, so its point reads win up to the caller's batch */
    const bool bm = t.bm != NULL;
    const int64_t vb = base + vs;
    switch (d->encoding) {
    case RAY_ENC_CONST:
        if (d->width || n > (bm ? PT_ROWS_NULLS : PT_ROWS_CONST)) return 0;
        break;
    case RAY_ENC_RLE:
        if (d->width || n > (bm ? PT_ROWS_NULLS : PT_ROWS_RUNS)) return 0;
        break;
    case RAY_ENC_FOR_BP:
        if (n > (bm ? PT_ROWS_NULLS : PT_ROWS_FOR)) return 0;
        break;
    case RAY_ENC_DICT:
        if (d->width || n > (bm ? PT_ROWS_NULLS : PT_ROWS_DICT)) return 0;
        break;
    case RAY_ENC_RAW:
        /* without a bitmap the view is the cheaper read once its chunk is
         * validated (raw_ok); before that, a few rows are point-read */
        if (bm ? n > PT_ROWS_NULLS
               : n > PT_ROWS_RAW || (e->raw_ok && atomic_load_explicit(&e->raw_ok[c], memory_order_acquire)))
            return 0;
        break;
    default:
        return 0;                                          /* DELTA_BP and the rest: the decode */
    }
    if (nul) memset(nul, 0, (size_t)n);
    switch (d->encoding) {
    case RAY_ENC_CONST: case RAY_ENC_RLE: return points_runs(&t, p, avail, vlen, rows, n, vb, out, err);
    case RAY_ENC_FOR_BP:                  return points_for(&t, p, avail, rows, n, vb, out, err);
    case RAY_ENC_DICT:                    return points_dict(&t, p, avail, rows, n, vb, out, err);
    default:                              return points_raw(&t, p, avail, vlen, rows, n, vb, out, err);
    }
}

int ray_enccol_points_i64(ray_enccol_t* e, const int64_t* rows, int64_t n, int64_t base, int64_t* out, ray_t** err) {
    return points_core(e, rows, n, base, out, NULL, err);
}

int ray_enccol_gather_points(ray_enccol_t* e, const int64_t* rows, int64_t n, int64_t base, int64_t* out,
                             uint8_t* nul, ray_t** err) {
    return points_core(e, rows, n, base, out, nul, err);
}

/* ---- Encoded vector views: a body validated, never expanded ----
 * A view is built only after every check the vector's decode makes has
 * passed, through the fast kernels' own validators (fk_runs, fk_runs_ok,
 * the FOR_BP and DICT rules of dec_fast / dec_fast_dict, fk_sym*), so an
 * aware path never answers from a vector that decode calls `corrupt`.  On
 * anything off it declines and the caller's range read decodes or reports
 * the vector, keeping its exact error. */

void ray_enccol_unpack_resid(const ray_enc_vview_t* v, void* out) {
    switch (v->k.T) {
    case 8:  (void)fk_for8 ((const uint8_t*) v->packed, v->width, 0, (uint8_t*) out, v->n, 0); break;
    case 16: (void)fk_for16((const uint16_t*)v->packed, v->width, 0, (uint16_t*)out, v->n, 0); break;
    case 32: (void)fk_for32((const uint32_t*)v->packed, v->width, 0, (uint32_t*)out, v->n, 0); break;
    default: (void)fk_for64((const uint64_t*)v->packed, v->width, 0, (uint64_t*)out, v->n, 0); break;
    }
}

/* Fused unpack-and-compare (perf fix 6): rows of n packed fields that
 * compare true (unsigned, at the lane width) against t, never storing
 * the fields.  FK_FOR's row walk with the store replaced by a count. */
#define FK_CNT_ROW(UT, CMP, V) do { for (int l = 0; l < nl; l++) { const UT v_ = (V); acc += (uint32_t)(v_ CMP t); } } while (0)
#define FK_CNT(NAME, UT, PT, TB, CMP)                                                  \
static uint32_t NAME(const UT* restrict in, int w, UT t, int n) {                       \
    enum { L = RAY_ENC_VEC / TB };                                                      \
    const UT m = w >= TB ? (UT)~(UT)0 : (UT)(((PT)1 << w) - 1);                         \
    uint32_t acc = 0;                                                                   \
    for (int r = 0, bit = 0; r * L < n; r++, bit += w) {                                \
        const int k = bit / TB, off = bit % TB;                                         \
        const unsigned sr = (unsigned)off & (TB - 1), sl = (unsigned)(TB - off) & (TB - 1); \
        const int nl = n - r * L < L ? n - r * L : L;                                   \
        const UT* a = in + (size_t)k * L;                                               \
        if (w == 0) FK_CNT_ROW(UT, CMP, (UT)0);                                         \
        else if (off + w > TB) FK_CNT_ROW(UT, CMP, FK_FIELD2(UT, PT, L, a, l, sr, sl, m)); \
        else FK_CNT_ROW(UT, CMP, FK_FIELD1(UT, PT, a, l, sr, m));                       \
    }                                                                                   \
    return acc;                                                                         \
}
#define FK_CNT_OPS(T, UT, PT) FK_CNT(fk_cnt##T##_eq, UT, PT, T, ==) FK_CNT(fk_cnt##T##_ne, UT, PT, T, !=) \
    FK_CNT(fk_cnt##T##_lt, UT, PT, T, <) FK_CNT(fk_cnt##T##_le, UT, PT, T, <=)                           \
    FK_CNT(fk_cnt##T##_gt, UT, PT, T, >) FK_CNT(fk_cnt##T##_ge, UT, PT, T, >=)
FK_CNT_OPS(8, uint8_t, unsigned)
FK_CNT_OPS(16, uint16_t, unsigned)
FK_CNT_OPS(32, uint32_t, unsigned)
FK_CNT_OPS(64, uint64_t, uint64_t)
#define FK_CNT_CALL(T, UT) switch (op) {                                                 \
    case OP_EQ: return fk_cnt##T##_eq((const UT*)v->packed, v->width, (UT)t, v->n);     \
    case OP_NE: return fk_cnt##T##_ne((const UT*)v->packed, v->width, (UT)t, v->n);     \
    case OP_LT: return fk_cnt##T##_lt((const UT*)v->packed, v->width, (UT)t, v->n);     \
    case OP_LE: return fk_cnt##T##_le((const UT*)v->packed, v->width, (UT)t, v->n);     \
    case OP_GT: return fk_cnt##T##_gt((const UT*)v->packed, v->width, (UT)t, v->n);     \
    default:    return fk_cnt##T##_ge((const UT*)v->packed, v->width, (UT)t, v->n); }
uint32_t ray_enccol_count_resid(const ray_enc_vview_t* v, uint16_t op, uint64_t t) {
    switch (v->k.T) {
    case 8:  FK_CNT_CALL(8, uint8_t);
    case 16: FK_CNT_CALL(16, uint16_t);
    case 32: FK_CNT_CALL(32, uint32_t);
    default: FK_CNT_CALL(64, uint64_t);
    }
}
#undef FK_CNT_CALL
#undef FK_CNT_OPS
#undef FK_CNT
#undef FK_CNT_ROW

/* True when no non-null row of the residuals res (lanes at width T) is
 * hitv, the sentinel's residual. */
static bool vview_no_hit(const ray_enc_vview_t* v, const ray_enc_lanes_t* res, uint64_t hitv) {
    for (int i = 0; i < v->n; i++)
        if (fk_val(res, v->k.T, (uint32_t)i) == hitv && !(v->nullmap && ((v->nullmap[i >> 3] >> (i & 7)) & 1)))
            return false;
    return true;
}

/* The largest / smallest of n lanes at width T (out of line loops, so
 * they vectorise). */
#define VV_RED(NAME, UT, OP) static __attribute__((noinline)) UT NAME(const UT* restrict a, int n, UT x) { \
        for (int i = 0; i < n; i++) { x = a[i] OP x ? a[i] : x; }                     \
        return x; }
VV_RED(vv_max8,  uint8_t,  >) VV_RED(vv_max16, uint16_t, >) VV_RED(vv_max32, uint32_t, >) VV_RED(vv_max64, uint64_t, >)
VV_RED(vv_min8,  uint8_t,  <) VV_RED(vv_min16, uint16_t, <) VV_RED(vv_min32, uint32_t, <) VV_RED(vv_min64, uint64_t, <)
#undef VV_RED
static uint64_t vview_lanes_max(const ray_enc_lanes_t* l, int T, int n) {
    switch (T) {
    case 8:  return vv_max8(l->b, n, 0);
    case 16: return vv_max16(l->h, n, 0);
    case 32: return vv_max32(l->s, n, 0);
    default: return vv_max64(l->q, n, 0);
    }
}
static uint64_t vview_lanes_min(const ray_enc_lanes_t* l, int T, int n) {
    switch (T) {
    case 8:  return vv_min8(l->b, n, UINT8_MAX);
    case 16: return vv_min16(l->h, n, UINT16_MAX);
    case 32: return vv_min32(l->s, n, UINT32_MAX);
    default: return vv_min64(l->q, n, UINT64_MAX);
    }
}

/* A SYM FOR_BP view's ids (base added, mod 2^T) into ids, checked as
 * dec_fast's FOR_BP arm checks them: below the bound or 0, the non-null
 * ones inside the vector's statistics, exactly n_nulls zeros. */
static bool vview_sym_ids_ok(const ray_enccol_t* e, const ray_enc_desc_t* d, const ray_enc_vview_t* v,
                             ray_enc_lanes_t* ids) {
    const int T = v->k.T, n = v->n, w = v->width;
    uint64_t lo, span;
    fk_stats(d, T == 64 ? ~0ull : (1ull << T) - 1, &lo, &span);
    switch (T) {
    case 8:
        (void)fk_for8((const uint8_t*)v->packed, w, (uint8_t)v->base, ids->b, n, 0);
        return fk_sym8(ids->b, n, e->sym_bound, d->n_nulls, (uint8_t)lo, (uint8_t)span);
    case 16:
        (void)fk_for16((const uint16_t*)v->packed, w, (uint16_t)v->base, ids->h, n, 0);
        return fk_sym16(ids->h, n, e->sym_bound, d->n_nulls, (uint16_t)lo, (uint16_t)span);
    case 32:
        (void)fk_for32((const uint32_t*)v->packed, w, (uint32_t)v->base, ids->s, n, 0);
        return fk_sym32(ids->s, n, e->sym_bound, d->n_nulls, (uint32_t)lo, (uint32_t)span);
    default:
        (void)fk_for64((const uint64_t*)v->packed, w, v->base, ids->q, n, 0);
        return fk_sym64(ids->q, n, e->sym_bound, d->n_nulls, lo, span);
    }
}

/* dec_fast_dict's checks without the gather: the U32 FOR_BP code child's
 * header, size and base; the codes (unpacked into v->codes) below the
 * dictionary size; every non-null row's value inside the vector's
 * statistics, which over the strictly ascending dictionary is a code
 * interval [clo, chi] (two binary searches); no non-null row on the
 * sentinel, which only entry 0 can hold (the type's least value). */
static bool vview_dict_codes(const ray_enc_desc_t* d, const uint8_t* body, uint64_t avail, const uint8_t* bm,
                             const ray_enc_dict_t* dict, ray_enc_vview_t* v) {
    const int w = body[1];
    uint16_t rsv; memcpy(&rsv, body + 2, 2);
    uint32_t bb; memcpy(&bb, body + 4, 4);
    if (rsv || w > 32 || (uint64_t)bb + 8 != avail || bb != (uint64_t)ray_enc_size_for(w)) return false;
    uint64_t base; memcpy(&base, body + 8, 8);
    if (base >> 32) return false;
    if (d->n_nulls && !bm) return false;
    (void)fk_for32((const uint32_t*)(body + 16), w, (uint32_t)base, v->codes, v->n, 0);
    const ray_enc_kind_t k = v->k;
    const uint32_t n = dict->n;                              /* a validated dictionary is not empty */
    uint32_t clo = 0, chi_x = n;                             /* non-null codes in [clo, chi_x) */
    if (!(d->flags & RAY_ENC_STAT_ABSENT)) {
        uint32_t a = 0, b = n;                               /* the first entry >= min */
        while (a < b) { uint32_t mid = a + (b - a) / 2; if (ray_enc_logical(dict->v[mid], k) < d->min) a = mid + 1; else b = mid; }
        clo = a;
        a = 0; b = n;                                        /* the first entry > max */
        while (a < b) { uint32_t mid = a + (b - a) / 2; if (ray_enc_logical(dict->v[mid], k) <= d->max) a = mid + 1; else b = mid; }
        chi_x = a;
    }
    const uint64_t sent = (uint64_t)1 << (k.T - 1);
    if (clo == 0 && dict->v[0] == sent) clo = 1;             /* the sentinel's code */
    const bool none = clo >= chi_x;                          /* no non-null row may exist */
    const uint32_t lim = n - 1, span = none ? 0 : chi_x - 1 - clo;
    uint32_t bad = 0;
    for (int i = 0; i < v->n; i++) {
        const uint32_t c = v->codes[i];
        const uint32_t nul = bm ? (uint32_t)((bm[i >> 3] >> (i & 7)) & 1) : 0;
        bad |= (uint32_t)(c > lim) | ((nul ^ 1u) & (uint32_t)(none | (c - clo > span)));
    }
    return !bad;
}

/* A float CONST body (no trailer): dec_floats' checks.  The 8-byte body
 * with zero padding (fk_runs' CONST arm at T bits), then, when a row is
 * not null, the value not a NaN and inside the vector's statistics. */
static bool vview_float_const(const ray_enc_desc_t* d, const uint8_t* body, uint64_t avail, ray_enc_vview_t* v) {
    const int T = v->k.T;
    if (!fk_runs(RAY_ENC_CONST, v->n, body, avail, T, &v->run_vals, &v->run_ends, &v->n_runs)) return false;
    v->cbits = T == 32 ? v->run_vals.s[0] : v->run_vals.q[0];
    if (d->n_nulls == v->n) return true;                     /* every row null: the value is never read */
    double x;
    if (T == 32) { float f; memcpy(&f, &v->run_vals.s[0], 4); x = f; } else memcpy(&x, &v->run_vals.q[0], 8);
    if (x != x) return false;                                /* a NaN outside the bitmap */
    if (!(d->flags & RAY_ENC_STAT_ABSENT)) {
        double smin, smax; memcpy(&smin, &d->min, 8); memcpy(&smax, &d->max, 8);
        if (x < smin || x > smax) return false;
    }
    return true;
}

int ray_enccol_vview(ray_enccol_t* e, int64_t vec, ray_enc_vview_t* v, ray_t** err) {
    *err = NULL;
    if (vec < 0 || vec >= ray_enc_nvecs(e->len) || e->kind != RAY_ENCCOL_SINGLE || e->synthetic) return 0;
    const int64_t c = vec / RAY_ENC_VECS_PER_CHUNK;
    const int j = (int)(vec % RAY_ENC_VECS_PER_CHUNK);
    const ray_enc_desc_t* descs;
    if ((*err = ray_enccol_chunk_descs_ref(e, c, &descs)) != NULL) return -1;   /* validated once per chunk */
    const ray_enc_desc_t* d = &descs[j];
    const int n = ray_enc_vec_len(e->len, vec);
    const bool fl = enc_type_float(e->type);
    if (d->flags & RAY_ENC_DESC_TRAILER) return 0;
    if (fl) {
        if (d->encoding != RAY_ENC_CONST) return 0;
        v->k = (ray_enc_kind_t){ (uint8_t)(e->type == RAY_F32 ? 32 : 64), false };
    } else {
        if (!fk_int_type(e->type) || !ray_enc_kind_of(e->type, e->attrs, &v->k)) return 0;
        if (d->encoding != RAY_ENC_CONST && d->encoding != RAY_ENC_FOR_BP &&
            d->encoding != RAY_ENC_RLE && d->encoding != RAY_ENC_DICT) return 0;
    }
    const uint8_t* body; uint64_t avail; const uint8_t* bm;
    if ((*err = payload_parts(e, c, j, d, n, &body, &avail, &bm)) != NULL) return -1;
    if (((uintptr_t)body & 7) || n < 1 || n > RAY_ENC_VEC) return 0;
    v->enc = d->encoding; v->n = n; v->n_nulls = d->n_nulls; v->nullmap = bm; v->type = e->type;
    v->cbits = 0; v->base = 0; v->width = 0; v->packed = NULL;
    v->n_runs = 0; v->dict = NULL; v->dict_n = 0;
    if (fl) return d->width == 0 && vview_float_const(d, body, avail, v) ? 1 : 0;
    const int T = v->k.T;
    const uint64_t m = T == 64 ? ~0ull : (1ull << T) - 1;
    const bool sym = e->type == RAY_SYM;
    switch (d->encoding) {
    case RAY_ENC_CONST: case RAY_ENC_RLE:
        if (d->width) return 0;
        if (!fk_runs(d->encoding, n, body, avail, T, &v->run_vals, &v->run_ends, &v->n_runs)) return 0;
        if (!fk_runs_ok(e, d, bm, v->k, &v->run_vals, &v->run_ends, v->n_runs)) return 0;
        if (d->encoding == RAY_ENC_CONST) v->cbits = fk_val(&v->run_vals, T, 0);
        return 1;
    case RAY_ENC_FOR_BP: {
        const int w = d->width;
        if (w > T || avail != (uint64_t)ray_enc_size_for(w)) return 0;
        uint64_t base; memcpy(&base, body, 8);
        if (base & ~m) return 0;
        v->base = base; v->width = w; v->packed = body + 8;
        /* no wrap: logical(base) + every residual stays at or below the
         * kind's largest value, so the residuals compare in logical order.
         * Fast accept: base + (2^w - 1) cannot wrap.  Otherwise the actual
         * residuals decide (exact, from the body, never the statistics):
         * the writer's base is the vector minimum and its largest residual
         * max - min, so a sound file always passes. */
        const uint64_t umax = w >= 64 ? ~0ull : (1ull << w) - 1;
        const uint64_t room = v->k.is_signed ? (uint64_t)(int64_t)(m >> 1) - (uint64_t)ray_enc_logical(base, v->k)
                                             : m - base;
        const bool may_wrap = umax > room;
        _Alignas(64) ray_enc_lanes_t lanes;                  /* one unpack, on the stack */
        if (sym) {
            /* ids are base + residual mod 2^T: a residual wrapped exactly
             * when its id fell below the base */
            if (!vview_sym_ids_ok(e, d, v, &lanes)) return 0;
            return !may_wrap || vview_lanes_min(&lanes, T, v->n) >= base ? 1 : 0;
        }
        const uint64_t sent = (uint64_t)1 << (T - 1), hitv = (sent - base) & m;
        const bool can_hit = v->k.is_signed && hitv <= umax;
        if (v->k.is_signed && d->n_nulls && !bm) return 0;
        if (!may_wrap && !can_hit) return 1;                 /* BOOL, U8 have no nulls; no residual is the sentinel */
        ray_enccol_unpack_resid(v, &lanes);
        if (may_wrap && vview_lanes_max(&lanes, T, v->n) > room) return 0;
        return !can_hit || vview_no_hit(v, &lanes, hitv) ? 1 : 0;
    }
    case RAY_ENC_DICT: {
        if (d->width || !v->k.is_signed || sym || avail < 16 || body[0] != RAY_ENC_FOR_BP) return 0;
        ray_enc_dict_t dict;
        if ((*err = enc_dict(e, &dict)) != NULL) return -1;  /* validated once per column */
        if (!vview_dict_codes(d, body, avail, bm, &dict, v)) return 0;
        v->dict = dict.v; v->dict_n = dict.n;
        return 1;
    }
    default:
        return 0;
    }
}

/* A stand-in column of `type` for the body hooks; with a VALUE_DICT of
 * dict[0, n_dict) (T-bit patterns), its entry `trim` bytes short. */
static ray_enccol_t* test_col(int8_t type, uint8_t attrs, uint32_t sym_bound,
                              const uint64_t* dict, uint32_t n_dict, int trim) {
    ray_enccol_t* e = ray_calloc_raw(sizeof *e);
    if (!e) return NULL;
    e->type = type; e->attrs = attrs; e->sym_bound = sym_bound;
    snprintf(e->path, sizeof e->path, "decode_body");
    ray_enc_once_init(&e->dict_once);
    ray_enc_kind_t k;
    if (dict && ray_enc_kind_of(type, attrs, &k)) {
        size_t esz = (size_t)k.T / 8, len = 8 + (size_t)ray_enc_pad8((uint64_t)n_dict * esz);
        uint8_t* b = ray_calloc_raw(len);
        if (!b) { ray_free_raw(e); return NULL; }
        memcpy(b, &n_dict, 4);
        for (uint32_t i = 0; i < n_dict; i++) memcpy(b + 8 + i * esz, &dict[i], esz);
        e->base = b;
        e->vdict_off = 0;
        e->vdict_len = len - (size_t)trim;
    }
    return e;
}
static void test_col_free(ray_enccol_t* e) {
    if (e->dict) ray_release(e->dict);
    ray_free_raw((void*)e->base);
    ray_free_raw(e);
}

int ray_enccol_test_point_body_stats(int8_t type, const uint64_t* dict, uint32_t n_dict, uint8_t enc,
                                     uint8_t width, int n_nulls, const uint8_t* body, uint64_t avail,
                                     int vlen, bool stats, int64_t mn, int64_t mx, int64_t* out, int8_t* res) {
    ray_enccol_t* e = test_col(type, 0, 0, dict, n_dict, 0);
    if (!e) return -1;
    ray_enc_desc_t d; memset(&d, 0, sizeof d);
    d.encoding = enc; d.width = width; d.n_nulls = (uint16_t)n_nulls;
    d.flags = stats ? 0 : RAY_ENC_STAT_ABSENT;
    d.min = stats ? mn : 0; d.max = stats ? mx : 0;
    ray_enc_kind_t k;
    const bool ok = point_type(e, &k);
    for (int i = 0; i < vlen; i++) {
        ray_t* err = NULL;
        int64_t v = 0;
        res[i] = ok ? (int8_t)point_read(e, &d, body, avail, 0, 0, vlen, i, k, &v, &err) : 0;
        if (res[i] == 1) out[i] = v;
        if (err) ray_error_free(err);
    }
    test_col_free(e);
    return 0;
}

int ray_enccol_test_point_body(int8_t type, const uint64_t* dict, uint32_t n_dict, uint8_t enc,
                               uint8_t width, int n_nulls, const uint8_t* body, uint64_t avail,
                               int vlen, int64_t* out, int8_t* res) {
    return ray_enccol_test_point_body_stats(type, dict, n_dict, enc, width, n_nulls, body, avail, vlen,
                                            false, 0, 0, out, res);
}

/* The descriptor of a hook body carries the statistics a writer would
 * give it: the non-null range of its reference decode (absent when that
 * fails or every row is null), so the decoders check against real ones. */
static void test_desc_stats(ray_enccol_t* e, ray_enc_desc_t* d, int vlen, const uint8_t* body, uint64_t avail,
                            const uint8_t* bm) {
    d->flags = RAY_ENC_STAT_ABSENT;
    ray_enc_kind_t k;
    if (!ray_enc_kind_of(e->type, e->attrs, &k) || vlen < 1 || vlen > RAY_ENC_VEC) return;
    _Alignas(8) uint8_t tmp[RAY_ENC_VEC * 8];
    ray_t* err = dec_ints(e, 0, 0, d, vlen, body, avail, bm, tmp);
    if (err) { ray_error_free(err); return; }
    const uint8_t esz = ray_enccol_esz(e);
    bool have = false;
    int64_t mn = 0, mx = 0;
    for (int i = 0; i < vlen; i++) {
        uint64_t x = 0;
        memcpy(&x, tmp + (size_t)i * esz, esz);
        if ((bm && ((bm[i >> 3] >> (i & 7)) & 1)) || (e->type == RAY_SYM && x == 0)) continue;
        const int64_t v = ray_enc_logical(x, k);
        if (!have || v < mn) mn = v;
        if (!have || v > mx) mx = v;
        have = true;
    }
    if (have) { d->flags = 0; d->min = mn; d->max = mx; }
}

static int test_decode(ray_enccol_t* e, uint8_t enc, uint8_t width, int n_nulls, const uint8_t* bm,
                       const uint8_t* body, uint64_t avail, int vlen, bool fast, int8_t wide, void* out) {
    ray_enc_desc_t d; memset(&d, 0, sizeof d);
    d.encoding = enc; d.width = width; d.n_nulls = (uint16_t)n_nulls;
    test_desc_stats(e, &d, vlen, body, avail, bm);
    if (fast) return dec_fast(e, &d, vlen, body, avail, bm, wide, out);
    if (wide) return -1;
    ray_t* err = dec_ints(e, 0, 0, &d, vlen, body, avail, bm, out);
    if (err) ray_error_free(err);
    return err ? -1 : 1;
}

int ray_enccol_test_decode_body(int8_t type, uint8_t attrs, uint32_t sym_bound, uint8_t enc,
                                uint8_t width, int n_nulls, const uint8_t* bm, const uint8_t* body,
                                uint64_t avail, int vlen, bool fast, int8_t wide, void* out) {
    ray_enccol_t* e = test_col(type, attrs, sym_bound, NULL, 0, 0);
    if (!e) return -1;
    int r = test_decode(e, enc, width, n_nulls, bm, body, avail, vlen, fast, wide, out);
    test_col_free(e);
    return r;
}

int ray_enccol_test_decode_dict(int8_t type, const uint64_t* dict, uint32_t n_dict, int trim,
                                int n_nulls, const uint8_t* bm, const uint8_t* body, uint64_t avail,
                                int vlen, bool fast, void* out) {
    ray_enccol_t* e = test_col(type, 0, 0, dict, n_dict, trim);
    if (!e) return -1;
    if (fast) {                                   /* the kernel reads a published dictionary only */
        ray_enc_dict_t dd;
        ray_t* err = enc_dict(e, &dd);
        if (err) ray_error_free(err);
    }
    int r = test_decode(e, RAY_ENC_DICT, 0, n_nulls, bm, body, avail, vlen, fast, 0, out);
    test_col_free(e);
    return r;
}

ray_t* ray_enccol_desc_pool_bytes(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d, uint64_t* out) {
    *out = 0;
    if (e->type != RAY_STR) return NULL;
    int64_t rows = chunk_rows(e, c);
    int vlen = (int)(rows - (int64_t)j * RAY_ENC_VEC < RAY_ENC_VEC ? rows - (int64_t)j * RAY_ENC_VEC : RAY_ENC_VEC);
    const uint8_t* body = NULL; uint64_t avail = 0; const uint8_t* bm = NULL;
    ray_t* err = payload_parts(e, c, j, d, vlen, &body, &avail, &bm);
    if (err) return err;
    return dec_str(e, c, j, d, vlen, body, avail, NULL, NULL, 0, 0, out);
}

ray_t* ray_enccol_decode_vec(ray_enccol_t* e, int64_t vec, void* out, char* pool_out,
                             uint64_t pool_base, uint64_t pool_cap) {
    if (vec < 0 || vec >= ray_enc_nvecs(e->len)) return ray_error("range", "column %s: vector %lld", e->path, (long long)vec);
    ray_enc_desc_t descs[64];
    int64_t c = vec / RAY_ENC_VECS_PER_CHUNK;
    ray_t* err = ray_enccol_chunk_descs(e, c, descs);
    if (err) return err;
    int j = (int)(vec % RAY_ENC_VECS_PER_CHUNK);
    return ray_enccol_decode_desc(e, c, j, &descs[j], out, pool_out, pool_base, pool_cap);
}

ray_t* ray_enccol_vec_pool_bytes(ray_enccol_t* e, int64_t vec, uint64_t* out) {
    *out = 0;
    if (vec < 0 || vec >= ray_enc_nvecs(e->len)) return ray_error("range", "column %s: vector %lld", e->path, (long long)vec);
    ray_enc_desc_t descs[64];
    int64_t c = vec / RAY_ENC_VECS_PER_CHUNK;
    ray_t* err = ray_enccol_chunk_descs(e, c, descs);
    if (err) return err;
    int j = (int)(vec % RAY_ENC_VECS_PER_CHUNK);
    return ray_enccol_desc_pool_bytes(e, c, j, &descs[j], out);
}

int ray_enccol_vec_nulls(ray_enccol_t* e, int64_t vec, int* n_nulls) {
    if (vec < 0 || vec >= ray_enc_nvecs(e->len)) return -1;
    ray_enc_desc_t descs[64];
    ray_t* err = ray_enccol_chunk_descs(e, vec / RAY_ENC_VECS_PER_CHUNK, descs);
    if (err) { ray_error_free(err); return -1; }
    *n_nulls = descs[vec % RAY_ENC_VECS_PER_CHUNK].n_nulls;
    return 0;
}

/* ---- Single-column materialization (spec "Materialized equivalence") ---- */

typedef struct { ray_enc_snap_t snap; bool failed; bool done; } enc_task_res_t;

typedef struct {
    ray_enccol_t*   e;
    uint8_t*        out;
    char*           pool;
    uint64_t        pool_cap;
    const uint64_t* chunk_pool;     /* STR: decoded-pool base of each chunk */
    enc_task_res_t* res;            /* one attempt result per chunk (task) */
    _Atomic(int64_t) lowest_failed; /* lowest failed chunk so far (INT64_MAX: none) */
} enc_mat_ctx_t;

static _Atomic(int64_t) g_interrupt_after;     /* test hook, see enccol.h */
void ray_enccol_test_interrupt_after(int64_t nth) { atomic_store(&g_interrupt_after, nth); }

/* One task per chunk.  Every failure, cancellation included, goes into the
 * task's own fixed-size result from this thread's state; the coordinator
 * picks the lowest failing chunk. */
static void enc_mat_chunk(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    enc_mat_ctx_t* x = raw;
    ray_enccol_t* e = x->e;
    uint8_t esz = ray_enccol_esz(e);
    for (int64_t c = start; c < end; c++) {
        enc_task_res_t* r = &x->res[c];
        /* Once a chunk has failed, chunks above it cannot change the
         * reported (lowest) failure: stop decoding them.  Lower chunks
         * still run, so the result stays deterministic. */
        if (c > atomic_load_explicit(&x->lowest_failed, memory_order_relaxed)) continue;
        if (ray_interrupted()) {
            ray_enc_snap_set(&r->snap, "cancel", "column %s: interrupted", e->path);
            r->failed = true;
            continue;
        }
        const ray_enc_desc_t* descs;
        ray_t* err = ray_enccol_chunk_descs_ref(e, c, &descs);
        int64_t rows = chunk_rows(e, c);
        int nv = (int)((rows + RAY_ENC_VEC - 1) / RAY_ENC_VEC);
        uint64_t pbase = x->chunk_pool ? x->chunk_pool[c] : 0;
        uint64_t produced = 0;                  /* counted once per chunk */
        for (int j = 0; !err && j < nv; j++) {
            int64_t row = c * RAY_ENC_CHUNK + (int64_t)j * RAY_ENC_VEC;
            uint8_t* dst = x->out + (size_t)row * esz;   /* private until published */
            uint64_t nb = 0;
            err = ray_enccol_decode_into(e, c, j, &descs[j], dst, x->pool, pbase, x->pool_cap, &nb);
            produced += nb;
            if (!err && e->type == RAY_STR) {
                int vlen = ray_enc_vec_len(e->len, row / RAY_ENC_VEC);
                ray_str_t* s = (ray_str_t*)dst;
                for (int i = 0; i < vlen; i++) {
                    if (ray_str_is_inline(&s[i])) continue;
                    pbase += s[i].len;
                    /* the generation-0 rule (col_validate_str_region), computed
                     * whatever the decoder left in the field */
                    uint32_t h = (uint32_t)ray_str_t_hash(&s[i], x->pool);
                    s[i].hash32 = h != 0 ? h : 1u;
                }
            }
        }
        ray_enccol_note_decoded(e, produced);
        if (err) { ray_enc_snap_take(&r->snap, err); r->failed = true; }
        if (r->failed) {
            int64_t cur = atomic_load_explicit(&x->lowest_failed, memory_order_relaxed);
            while (c < cur && !atomic_compare_exchange_weak_explicit(&x->lowest_failed, &cur, c,
                                   memory_order_relaxed, memory_order_relaxed)) {}
        } else {
            int64_t k = atomic_load_explicit(&g_interrupt_after, memory_order_relaxed);
            while (k > 0 && !atomic_compare_exchange_weak(&g_interrupt_after, &k, k - 1)) {}
            r->done = true;
            if (k == 1) ray_request_interrupt();
        }
    }
}

/* STR column-scope pass: every vector's decoded-pool bytes, with checked
 * arithmetic, before anything is written; the total must equal
 * str_pool_bytes.  The pool buffer is sized from these (str_pool_bytes and
 * the *_pool_bytes functions) only: decode reports a too-small pool_cap as
 * corrupt, so no other size may be used. */
static ray_t* enc_str_prepass(ray_enccol_t* e, uint64_t* chunk_pool) {
    uint64_t total = 0;
    for (int64_t c = 0; c < e->n_chunks; c++) {
        ray_enc_desc_t descs[64];
        ray_t* err = ray_enccol_chunk_descs(e, c, descs);
        if (err) return err;
        chunk_pool[c] = total;
        int nv = (int)((chunk_rows(e, c) + RAY_ENC_VEC - 1) / RAY_ENC_VEC);
        for (int j = 0; j < nv; j++) {
            uint64_t pb;
            if ((err = ray_enccol_desc_pool_bytes(e, c, j, &descs[j], &pb)) != NULL) return err;
            if (pb > e->hdr.str_pool_bytes - total)
                return ray_error("corrupt", "column %s: decoded pool exceeds str_pool_bytes at chunk %lld", e->path, (long long)c);
            total += pb;
        }
    }
    if (total != e->hdr.str_pool_bytes)
        return ray_error("corrupt", "column %s: decoded pool is %llu bytes, header says %llu", e->path,
                         (unsigned long long)total, (unsigned long long)e->hdr.str_pool_bytes);
    return NULL;
}

static _Atomic int g_refuse_attach;
void ray_enccol_test_refuse_attach(int on) { atomic_store(&g_refuse_attach, on); }

static double dbl_of(int64_t b) { double d; memcpy(&d, &b, 8); return d; }

/* ZONE: combined min/max (first of ties wins, row order) and null count;
 * 0/0 when no value (zone_scan_int / zone_scan_float, src/ops/idxop.c). */
static ray_t* enc_synth_zone(ray_enccol_t* e) {
    ray_t* root = enc_alloc_fails(e) ? NULL : ray_index_new_root(RAY_IDX_ZONE, e->type, e->len);
    if (!root || RAY_IS_ERR(root)) return root ? root : ray_error("oom", "column %s: zone", e->path);
    ray_index_t* ix = ray_index_payload(root);
    bool fl = enc_type_float(e->type), have = false;
    int64_t mn = 0, mx = 0;
    for (int64_t c = 0; c < e->n_chunks; c++) {
        ray_enc_dirent_t de; dirent_at(e, c, &de);
        if (de.flags & RAY_ENC_STAT_ABSENT) continue;
        bool lt = fl ? dbl_of(de.min) < dbl_of(mn) : de.min < mn;
        bool gt = fl ? dbl_of(de.max) > dbl_of(mx) : de.max > mx;
        if (!have || lt) mn = de.min;
        if (!have || gt) mx = de.max;
        have = true;
    }
    if (fl) { ix->u.zone.min_f = have ? dbl_of(mn) : 0.0; ix->u.zone.max_f = have ? dbl_of(mx) : 0.0; }
    else    { ix->u.zone.min_i = have ? mn : 0;          ix->u.zone.max_i = have ? mx : 0; }
    ix->u.zone.n_nulls = e->null_count;
    return root;
}

/* CHUNK_ZONE at chunk_log2 16 (the builder default) from the directory;
 * all-null chunks keep the builder's empty extrema (INT64_MAX / INT64_MIN,
 * +inf / -inf); aggs [sum_lo | non-null rows | sum_hi] only for the types
 * whose directory carries sums (ray_encw_type_summable), NULL otherwise
 * (readers accept a NULL aggs). */
static ray_t* enc_synth_chunk_zone(ray_enccol_t* e) {
    ray_t* root = enc_alloc_fails(e) ? NULL : ray_index_new_root(RAY_IDX_CHUNK_ZONE, e->type, e->len);
    if (!root || RAY_IS_ERR(root)) return root ? root : ray_error("oom", "column %s: chunk zone", e->path);
    ray_index_t* ix = ray_index_payload(root);
    uint32_t nc = (uint32_t)e->n_chunks;
    bool fl = enc_type_float(e->type);
    bool sums = ray_encw_type_summable(e->type);
    ix->u.chunk_zone.n_chunks = nc;
    ix->u.chunk_zone.chunk_log2 = 16;
    ix->u.chunk_zone.is_f64 = fl ? 1 : 0;
    ray_t* mins = ray_vec_new(fl ? RAY_F64 : RAY_I64, nc);
    ray_t* maxs = ray_vec_new(fl ? RAY_F64 : RAY_I64, nc);
    ray_t* nb   = ray_vec_new(RAY_U8, (nc + 7) / 8);
    ray_t* aggs = sums ? ray_vec_new(RAY_I64, 3 * (int64_t)nc) : NULL;
    /* Stored before any check so ray_release(root) frees what exists. */
    ix->u.chunk_zone.mins = (mins && !RAY_IS_ERR(mins)) ? mins : NULL;
    ix->u.chunk_zone.maxs = (maxs && !RAY_IS_ERR(maxs)) ? maxs : NULL;
    ix->u.chunk_zone.null_bits = (nb && !RAY_IS_ERR(nb)) ? nb : NULL;
    ix->u.chunk_zone.aggs = (aggs && !RAY_IS_ERR(aggs)) ? aggs : NULL;
    if (!ix->u.chunk_zone.mins || !ix->u.chunk_zone.maxs || !ix->u.chunk_zone.null_bits ||
        (sums && !ix->u.chunk_zone.aggs) || enc_alloc_fails(e)) {
        ray_release(root);
        return ray_error("oom", "column %s: chunk zone", e->path);
    }
    mins->len = maxs->len = nc;
    nb->len = (nc + 7) / 8;
    memset(ray_data(nb), 0, (size_t)nb->len);
    if (aggs) aggs->len = 3 * (int64_t)nc;
    for (uint32_t c = 0; c < nc; c++) {
        ray_enc_dirent_t de; dirent_at(e, c, &de);
        bool absent = de.flags & RAY_ENC_STAT_ABSENT;
        if (fl) {
            ((double*)ray_data(mins))[c] = absent ? INFINITY  : dbl_of(de.min);
            ((double*)ray_data(maxs))[c] = absent ? -INFINITY : dbl_of(de.max);
        } else {
            ((int64_t*)ray_data(mins))[c] = absent ? INT64_MAX : de.min;
            ((int64_t*)ray_data(maxs))[c] = absent ? INT64_MIN : de.max;
        }
        if (de.null_count) ((uint8_t*)ray_data(nb))[c >> 3] |= (uint8_t)(1u << (c & 7));
        if (aggs) {
            int64_t* ag = (int64_t*)ray_data(aggs);
            ag[c] = (int64_t)de.sum_lo;
            ag[nc + c] = chunk_rows(e, c) - (int64_t)de.null_count;
            ag[2 * (int64_t)nc + c] = (int64_t)de.sum_hi;
        }
    }
    return root;
}

/* ZONE / CHUNK_ZONE roots are synthesized from the verified directory and
 * the UNIQUE marker from the column flag; every attach goes through the
 * admission predicate, a refusal is corrupt. */
static ray_t* enc_attach_index(ray_enccol_t* e, ray_t** vp) {
    uint8_t kind = e->hdr.index_kind;
    bool unique = e->hdr.col_flags & RAY_ENC_FLAG_UNIQUE;
    if (kind == 0 && !unique) return NULL;
    ray_t* root = NULL;
    if (kind == RAY_IDX_ZONE || kind == RAY_IDX_CHUNK_ZONE) {
        if (!ray_zone_acc_supported(e->type))
            return ray_error("corrupt", "column %s: zone index on type %s", e->path, ray_type_name(e->type));
        root = kind == RAY_IDX_ZONE ? enc_synth_zone(e) : enc_synth_chunk_zone(e);
        if (RAY_IS_ERR(root)) return root;
    } else if (kind != 0) {
        return ray_error("corrupt", "column %s: index kind %u without a region", e->path, kind);
    }
    if (unique) {
        if (!root) {
            root = enc_alloc_fails(e) ? NULL : ray_index_new_root(RAY_IDX_NONE, e->type, e->len);
            if (!root || RAY_IS_ERR(root)) return root ? root : ray_error("oom", "column %s: unique marker", e->path);
        }
        ray_index_payload(root)->markers |= RAY_MARK_UNIQUE;
    }
    if (atomic_load_explicit(&g_refuse_attach, memory_order_relaxed))
        ray_index_payload(root)->built_for_len++;       /* test: the predicate must refuse */
    ray_t* r = ray_index_attach_persisted(vp, root);    /* consumes root */
    if (RAY_IS_ERR(r)) {
        /* only the predicate's refusal is a wire violation; oom and
         * programming errors pass through unchanged */
        if (strcmp(ray_err_code(r), "corrupt") != 0) return r;
        ray_error_free(r);
        return ray_error("corrupt", "column %s: index or UNIQUE flag not admitted on type %s",
                         e->path, ray_type_name(e->type));
    }
    *vp = r;
    return NULL;
}

static ray_t* enc_materialize_single(void* ctx, ray_enc_snap_t* fail) {
    ray_enccol_t* e = ctx;
    uint8_t ik = e->hdr.index_kind;
    if (ik == 1 || ik == 2 || ik == 4 || ik == 6) {
        ray_enc_snap_set(fail, "nyi", "column %s: index region needs a newer build", e->path);
        return NULL;
    }
    if (ray_interrupted()) { ray_enc_snap_set(fail, "cancel", "column %s: interrupted", e->path); return NULL; }
    if (enc_alloc_fails(e)) { ray_enc_snap_set(fail, "oom", "column %s: materialize", e->path); return NULL; }
    ray_t* vec = e->type == RAY_SYM ? ray_sym_vec_new(e->attrs & RAY_SYM_W_MASK, e->len)
                                    : ray_vec_new(e->type, e->len);
    if (!vec || RAY_IS_ERR(vec)) {
        if (vec) ray_enc_snap_take(fail, vec); else ray_enc_snap_set(fail, "oom", "column %s: materialize", e->path);
        return NULL;
    }
    vec->len = e->len;
    ray_t* err = NULL;
    uint64_t* chunk_pool = NULL;
    enc_task_res_t* res = NULL;
    if (e->type == RAY_STR) {
        chunk_pool = enc_alloc_fails(e) ? NULL : ray_alloc_raw((size_t)(e->n_chunks + 1) * sizeof(uint64_t));
        if (!chunk_pool) { err = ray_error("oom", "column %s: materialize", e->path); goto fail; }
        if ((err = enc_str_prepass(e, chunk_pool)) != NULL) goto fail;
        if (e->hdr.str_pool_bytes > 0) {
            ray_t* pool = enc_alloc_fails(e) ? NULL : ray_alloc((size_t)e->hdr.str_pool_bytes);
            if (!pool || RAY_IS_ERR(pool)) { err = pool ? pool : ray_error("oom", "column %s: pool", e->path); goto fail; }
            pool->type = RAY_U8;
            pool->len  = (int64_t)e->hdr.str_pool_bytes;
            vec->str_pool = pool;
        }
    }
    res = enc_alloc_fails(e) ? NULL : ray_calloc_raw((size_t)(e->n_chunks + 1) * sizeof *res);
    if (!res) { err = ray_error("oom", "column %s: materialize", e->path); goto fail; }
    enc_mat_ctx_t x = {
        .e = e, .out = (uint8_t*)ray_data(vec),
        .pool = vec->str_pool ? (char*)ray_data(vec->str_pool) : NULL,
        .pool_cap = e->hdr.str_pool_bytes, .chunk_pool = chunk_pool, .res = res,
    };
    atomic_init(&x.lowest_failed, INT64_MAX);
    if (e->n_chunks > (int64_t)UINT32_MAX) { err = ray_error("range", "column %s: too many chunks", e->path); goto fail; }
    ray_pool_t* tp = ray_pool_get();
    if (e->n_chunks >= 2 && ray_pool_par_dispatch_ok(tp, e->n_chunks, 2))
        ray_pool_dispatch_n(tp, enc_mat_chunk, &x, (uint32_t)e->n_chunks);
    else
        enc_mat_chunk(&x, 0, 0, e->n_chunks);
    /* A cancel also stops the pool from running its remaining tasks: a chunk
     * that neither finished nor failed was never run and counts as cancelled. */
    for (int64_t c = 0; c < e->n_chunks; c++)
        if (res[c].failed || !res[c].done) {     /* the lowest-numbered failing task */
            if (res[c].failed) *fail = res[c].snap;
            else ray_enc_snap_set(fail, "cancel", "column %s: interrupted", e->path);
            ray_free_raw(res); ray_free_raw(chunk_pool); ray_release(vec);
            return NULL;
        }
    ray_free_raw(res); res = NULL;
    ray_free_raw(chunk_pool); chunk_pool = NULL;
    vec->attrs = e->attrs;                       /* persisted attrs (SYM width included) */
    if (e->type == RAY_SYM) {
        vec->sym_domain = e->dom;                /* sym_vec_new's runtime domain is immortal */
        if (e->dom_owned) ray_sym_domain_retain(e->dom);
    }
    if ((err = enc_attach_index(e, &vec)) != NULL) goto fail;
    atomic_fetch_add_explicit(&g_enc_materialized, 1, memory_order_relaxed);
    return vec;
fail:
    ray_free_raw(res); ray_free_raw(chunk_pool);
    ray_release(vec);
    ray_enc_snap_take(fail, err);
    return NULL;
}

/* ---- Parted object (Task 12) ---- */

bool ray_enccol_is_parted(ray_t* encv) {
    ray_enccol_t* e = ray_enccol_of(encv);
    return e && e->kind == RAY_ENCCOL_PARTED;
}

void ray_enccol_set_synthetic(ray_t* encv) {
    ray_enccol_t* e = ray_enccol_of(encv);
    if (e && e->kind == RAY_ENCCOL_SINGLE) e->synthetic = true;
}

bool ray_enccol_is_synthetic(ray_t* encv) {
    ray_enccol_t* e = ray_enccol_of(encv);
    if (!e) return false;
    if (e->kind == RAY_ENCCOL_SINGLE) return e->synthetic;
    if (!e->wrapper) return false;
    ray_t** segs = (ray_t**)ray_data(e->wrapper);
    for (int64_t i = 0; i < e->wrapper->len; i++)
        if (segs[i] && segs[i]->type == RAY_ENCCOL && !ray_enccol_is_synthetic(segs[i])) return false;
    return true;
}

ray_t* ray_enccol_parted_new(ray_t* wrapper) {
    if (!wrapper || RAY_IS_ERR(wrapper) || !RAY_IS_PARTED(wrapper->type))
        return ray_error("type", "parted encoded column needs a parted wrapper");
    ray_enccol_t* e = ray_calloc_raw(sizeof *e);
    if (!e) return ray_error("oom", NULL);
    atomic_fetch_add_explicit(&g_enccol_live, 1, memory_order_relaxed);
    atomic_store(&e->rc, 1);
    e->kind = RAY_ENCCOL_PARTED;
    e->type = (int8_t)RAY_PARTED_BASETYPE(wrapper->type);
    e->len = ray_parted_nrows(wrapper);
    ray_t** segs = (ray_t**)ray_data(wrapper);
    for (int64_t i = 0; i < wrapper->len; i++) {
        if (!segs[i]) continue;
        ray_col_meta_t m;
        if (!ray_col_meta_of(segs[i], &m)) continue;
        /* an encoded segment: its directory's null count (in-band text nulls
         * included); a plain one: the exact check, which scans a SYM/STR
         * payload (text nulls set no flag) */
        bool nulls = m.null_count >= 0 ? m.null_count > 0
                   : !m.enc && (m.type == RAY_SYM || m.type == RAY_STR) ? ray_vec_has_nulls(segs[i])
                   : (m.attrs & RAY_ATTR_HAS_NULLS) != 0;
        if (nulls) e->attrs |= RAY_ATTR_HAS_NULLS;      /* OR across every segment; width and
                                                         * SORTED are per-segment properties */
    }
    /* Distinct from every segment's path: a segment-scoped failure (or test
     * hook) never matches the wrapper itself. */
    snprintf(e->path, sizeof e->path, "(parted)");
    ray_enc_once_init(&e->once);
    ray_enc_once_init(&e->dict_once);
    ray_retain(wrapper);
    e->wrapper = wrapper;
    ray_t* v = ray_enccol_wrap(e);
    if (!v || RAY_IS_ERR(v)) { enccol_free(e); return v ? v : ray_error("oom", NULL); }
    return v;
}

ray_t* ray_enccol_seg(ray_t* encv, int64_t i) {
    ray_enccol_t* e = ray_enccol_of(encv);
    if (!e || e->kind != RAY_ENCCOL_PARTED || !e->wrapper || i < 0 || i >= e->wrapper->len) return NULL;
    return ((ray_t**)ray_data(e->wrapper))[i];
}

ray_t* ray_enccol_seg_col(ray_t* encv, int64_t i) {
    ray_t* s = ray_enccol_seg(encv, i);
    if (!s) return NULL;
    return s->type == RAY_ENCCOL ? ray_enccol_materialize(s) : s;
}

typedef struct {
    ray_t**         in;        /* the wrapper's segments */
    ray_t**         plain;     /* borrowed results */
    enc_task_res_t* res;       /* one attempt result per segment (task) */
} enc_seg_ctx_t;

/* One task per segment; each materializes through its own once-state, so a
 * segment keeps its cached vector or failure whatever the others do.  The
 * snapshot is taken from the error on this thread. */
static void enc_seg_task(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    enc_seg_ctx_t* x = raw;
    for (int64_t i = start; i < end; i++) {
        ray_t* s = x->in[i];
        if (!s || s->type != RAY_ENCCOL) { x->plain[i] = s; x->res[i].done = true; continue; }
        ray_t* m = ray_enccol_materialize(s);
        if (RAY_IS_ERR(m)) {
            ray_enc_snap_take(&x->res[i].snap, m);
            x->res[i].failed = true;
            continue;
        }
        x->plain[i] = m;
        x->res[i].done = true;
    }
}

static _Atomic(int) g_parted_mode;       /* test hook: 0 auto, 1 coordinator, 2 per-segment tasks */
void ray_enccol_test_parted_mode(int mode) { atomic_store(&g_parted_mode, mode); }

static ray_t* enc_materialize_parted(void* ctx, ray_enc_snap_t* fail) {
    ray_enccol_t* e = ctx;
    int64_t n = e->wrapper->len;
    bool inj = enc_alloc_fails(e);
    ray_t* out = inj ? NULL : ray_alloc((size_t)n * sizeof(ray_t*));
    ray_t** plain = inj ? NULL : ray_calloc_raw((size_t)(n + 1) * sizeof(ray_t*));
    enc_task_res_t* res = inj ? NULL : ray_calloc_raw((size_t)(n + 1) * sizeof *res);
    if (!out || RAY_IS_ERR(out) || !plain || !res) {
        if (out && !RAY_IS_ERR(out)) ray_free(out);
        ray_free_raw(plain); ray_free_raw(res);
        ray_enc_snap_set(fail, "oom", "column %s: parted materialize", e->path);
        return NULL;
    }
    out->type = e->wrapper->type;
    out->len = n;
    out->attrs = 0;
    memset(out->aux, 0, 16);
    memset(ray_data(out), 0, (size_t)n * sizeof(ray_t*));
    enc_seg_ctx_t x = { (ray_t**)ray_data(e->wrapper), plain, res };
    ray_pool_t* pool = ray_pool_get();
    /* Per-segment tasks run their chunks serially (nested dispatch), so with
     * fewer encoded segments left than workers the coordinator takes them one
     * after another and each keeps full chunk parallelism.  Either way every
     * segment is attempted and reported through its own result slot. */
    int64_t n_enc = 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* sg = x.in[i];
        if (sg && sg->type == RAY_ENCCOL && !ray_enccol_of(sg)->decoded) n_enc++;
    }
    int mode = atomic_load(&g_parted_mode);
    bool fan = n >= 2 && n <= (int64_t)UINT32_MAX && ray_pool_par_dispatch_ok(pool, n, 2) &&
               n_enc >= (int64_t)pool->n_workers;
    if (mode == 1) fan = false;
    else if (mode == 2) fan = n >= 2 && n <= (int64_t)UINT32_MAX && ray_pool_par_dispatch_ok(pool, n, 2);
    if (fan) ray_pool_dispatch_n(pool, enc_seg_task, &x, (uint32_t)n);
    else enc_seg_task(&x, 0, 0, n);
    /* A task the pool skipped on cancel neither finished nor failed. */
    for (int64_t i = 0; i < n; i++)
        if (res[i].failed || !res[i].done) {        /* lowest-numbered failing segment */
            if (res[i].failed) *fail = res[i].snap;
            else ray_enc_snap_set(fail, "cancel", "column %s: interrupted", e->path);
            ray_free(out);                          /* no segment reference was taken yet */
            ray_free_raw(plain); ray_free_raw(res);
            return NULL;
        }
    ray_t** segs = (ray_t**)ray_data(out);
    for (int64_t i = 0; i < n; i++) if (plain[i]) { ray_retain(plain[i]); segs[i] = plain[i]; }
    ray_free_raw(plain); ray_free_raw(res);
    return out;
}

/* A caller waiting on another thread's attempt (ray_enc_once_get) does not
 * observe its own cancellation: it waits for the attempt, which polls
 * ray_interrupted() once per chunk and ends with a transient "cancel"
 * (the state then resets), so the wait is bounded by one chunk's decode. */
ray_t* ray_enccol_materialize(ray_t* encv) {
    ray_enccol_t* e = ray_enccol_of(encv);
    if (!e) return ray_error("type", "materialize: not an encoded column");
    return ray_enc_once_get(&e->once, &e->decoded,
                            e->kind == RAY_ENCCOL_PARTED ? enc_materialize_parted : enc_materialize_single, e);
}

void ray_enccol_adopt_domain(ray_t* out, ray_t* encv) {
    ray_enccol_t* e = ray_enccol_of(encv);
    if (!e || !out || out->type != RAY_SYM || e->type != RAY_SYM || !e->dom) return;
    out->sym_domain = e->dom;                    /* sym_vec_new's runtime domain is immortal */
    if (e->dom_owned) ray_sym_domain_retain(e->dom);
}

void ray_enccol_meta(ray_t* encv, ray_col_meta_t* m) {
    ray_enccol_t* e = ray_enccol_of(encv);
    memset(m, 0, sizeof *m);
    m->enc = encv;
    m->len = e->len;
    if (e->kind == RAY_ENCCOL_PARTED) {
        m->type = (int8_t)(RAY_PARTED_BASE + e->type);
        m->attrs = e->attrs;
        m->n_segs = e->wrapper ? e->wrapper->len : 0;
        m->null_count = -1;
        return;
    }
    m->type = e->type;
    m->attrs = e->attrs;
    m->index_kind = e->hdr.index_kind;
    m->unique = (e->hdr.col_flags & RAY_ENC_FLAG_UNIQUE) != 0;
    m->null_count = e->null_count;
}

/* Generation-0 payload bytes of a plain vector (STR: its pool too). */
uint64_t ray_enccol_plain_bytes(ray_t* s) {
    if (!s || !ray_is_vec(s)) return 0;
    uint64_t b = (uint64_t)s->len * ray_sym_elem_size(s->type, s->attrs);
    if (s->type == RAY_STR && s->str_pool) b += (uint64_t)s->str_pool->len;
    return b;
}

ray_t* ray_enccol_info(ray_t* encv, ray_enccol_info_t* out) {
    memset(out, 0, sizeof *out);
    ray_enccol_t* e = ray_enccol_of(encv);
    if (!e) return ray_error("type", "info: not an encoded column");
    out->synthetic = ray_enccol_is_synthetic(encv);
    if (e->kind == RAY_ENCCOL_PARTED) {
        int64_t ns = e->wrapper ? e->wrapper->len : 0;
        for (int64_t i = 0; i < ns; i++) {
            ray_t* s = ray_enccol_seg(encv, i);
            if (!s) continue;
            ray_enccol_info_t si;
            if (s->type == RAY_ENCCOL) {
                ray_t* err = ray_enccol_info(s, &si);
                if (err) return err;
            } else {
                memset(&si, 0, sizeof si);
                si.encoded_bytes = si.raw_bytes = ray_enccol_plain_bytes(s);
            }
            for (int k = 0; k < 10; k++) out->vectors[k] += si.vectors[k];
            out->encoded_bytes += si.encoded_bytes;
            out->raw_bytes += si.raw_bytes;
            out->materialized |= si.materialized;
        }
        out->materialized |= ray_enccol_decoded_peek(encv) != NULL;
        return NULL;
    }
    /* a synthetic stand-in has an in-memory null image, not a file: no
     * encodings and no file bytes */
    for (int64_t c = 0; !out->synthetic && c < e->n_chunks; c++) {
        const ray_enc_desc_t* d;
        ray_t* err = ray_enccol_chunk_descs_ref(e, c, &d);
        if (err) return err;
        int64_t rows = e->len - c * RAY_ENC_CHUNK;
        int nv = (int)(((rows > RAY_ENC_CHUNK ? RAY_ENC_CHUNK : rows) + RAY_ENC_VEC - 1) / RAY_ENC_VEC);
        for (int j = 0; j < nv; j++) if (d[j].encoding < 10) out->vectors[d[j].encoding]++;
    }
    out->encoded_bytes = out->synthetic ? 0 : e->hdr.end_off;
    out->raw_bytes = (uint64_t)e->len * ray_enccol_esz(e) + (e->type == RAY_STR ? e->hdr.str_pool_bytes : 0);
    out->materialized = ray_enccol_decoded_peek(encv) != NULL;
    return NULL;
}

bool ray_enccol_may_have_index(ray_t* encv) {
    ray_enccol_t* e = ray_enccol_of(encv);
    if (!e || e->kind != RAY_ENCCOL_SINGLE) return false;
    if (e->hdr.index_kind || (e->hdr.col_flags & RAY_ENC_FLAG_UNIQUE)) return true;
    if (e->type != RAY_STR || e->n_chunks == 0) return false;
    for (int64_t c = 0; c < e->n_chunks; c++) {
        ray_enc_dirent_t de; dirent_at(e, c, &de);
        if (de.flags & RAY_ENC_STAT_ABSENT) return false;
    }
    return true;
}

ray_t* ray_enccol_decoded_peek(ray_t* encv) {
    ray_enccol_t* e = ray_enccol_of(encv);
    if (!e) return NULL;
    if (atomic_load_explicit(&e->once.state, memory_order_acquire) != RAY_ENC_ONCE_DONE) return NULL;
    return e->decoded;
}

/* The non-null values of n elements at data inside [lo, hi]: one
 * or-reduce of the unsigned distance above lo against hi - lo at the
 * element width (vectorises at every width; a null, in band, takes no
 * part).  [lo, hi] is first clipped to the type. */
static bool dir_range_ok(int8_t type, const void* data, int64_t n, bool may_null, int64_t lo, int64_t hi) {
#define DIR_REDUCE(T, UT, TMIN, TMAX, NUL) do {                                         \
        const T* p = (const T*)data;                                                    \
        const int64_t l = lo > (int64_t)(TMIN) ? lo : (int64_t)(TMIN);                  \
        const int64_t h = hi < (int64_t)(TMAX) ? hi : (int64_t)(TMAX);                  \
        if (l > h) {                                     /* no value of the type fits */ \
            for (int64_t i = 0; i < n; i++) if (!may_null || p[i] != (T)(NUL)) return false; \
            return true;                                                                \
        }                                                                               \
        const UT b = (UT)(T)l, sp = (UT)((UT)(T)h - b);                                 \
        UT bad = 0;                                                                     \
        if (may_null) for (int64_t i = 0; i < n; i++) {                                 \
            const UT d = p[i] == (T)(NUL) ? 0 : (UT)((UT)p[i] - b);                     \
            bad |= (UT)-(UT)(d > sp);                                                   \
        } else for (int64_t i = 0; i < n; i++) {                                        \
            const UT d = (UT)((UT)p[i] - b);                                            \
            bad |= (UT)-(UT)(d > sp);                                                   \
        }                                                                               \
        return bad == 0; } while (0)
    switch (type) {
    case RAY_BOOL: case RAY_U8:                 DIR_REDUCE(uint8_t, uint8_t, 0, UINT8_MAX, 0); break;   /* never null */
    case RAY_I16:                               DIR_REDUCE(int16_t, uint16_t, INT16_MIN, INT16_MAX, NULL_I16); break;
    case RAY_I32: case RAY_DATE: case RAY_TIME: DIR_REDUCE(int32_t, uint32_t, INT32_MIN, INT32_MAX, NULL_I32); break;
    case RAY_I64: case RAY_TIMESTAMP:           DIR_REDUCE(int64_t, uint64_t, INT64_MIN, INT64_MAX, NULL_I64); break;
    default: return true;
    }
#undef DIR_REDUCE
}

bool ray_enccol_dir_vec_ok(const ray_enccol_t* e, int64_t c, int j) {
    return e->dir_vec_ok && ((atomic_load_explicit(&e->dir_vec_ok[c], memory_order_acquire) >> j) & 1);
}

/* The guard's verdict on vector j of chunk c: a pass (or a chunk without
 * statistics) is recorded, a failure is `corrupt` and never recorded. */
static ray_t* check_dir_tail(ray_enccol_t* e, int64_t c, int j, bool ok) {
    if (ok) {
        if (e->dir_vec_ok) atomic_fetch_or_explicit(&e->dir_vec_ok[c], UINT64_C(1) << j, memory_order_release);
        return NULL;
    }
    return ray_error("corrupt", "column %s: chunk %lld vector %d value outside the chunk's statistics",
                     e->path, (long long)c, j);
}

ray_t* ray_enccol_check_dir(ray_enccol_t* e, int64_t c, int j, const void* data, int vlen, bool may_null) {
    /* a vector that passed once passes again (the mapping is immutable,
     * the decode deterministic); a failure is never recorded */
    if (ray_enccol_dir_vec_ok(e, c, j)) return NULL;
    ray_enc_dirent_t de; dirent_at(e, c, &de);
    return check_dir_tail(e, c, j, (de.flags & RAY_ENC_STAT_ABSENT) ||   /* no statistics: nothing sized from them */
        dir_range_ok(e->type, data, vlen, may_null && !(e->type == RAY_BOOL || e->type == RAY_U8), de.min, de.max));
}

ray_t* ray_enccol_check_dir_range(ray_enccol_t* e, int64_t c, int j, int64_t mn, int64_t mx) {
    ray_enc_dirent_t de; dirent_at(e, c, &de);
    return check_dir_tail(e, c, j, (de.flags & RAY_ENC_STAT_ABSENT) || (mn >= de.min && mx <= de.max));
}

ray_t* ray_enccol_check_dir_all(ray_t* encv) {
    ray_enccol_t* e = ray_enccol_of(encv);
    if (!e) return NULL;
    if (atomic_load_explicit(&e->mat_dir_ok, memory_order_acquire)) return NULL;
    if (e->kind == RAY_ENCCOL_PARTED) {
        /* every encoded segment against its own statistics (the parted
         * materialization holds the segments' own) */
        if (!e->wrapper || !ray_enccol_decoded_peek(encv)) return NULL;
        ray_t** segs = (ray_t**)ray_data(e->wrapper);
        for (int64_t i = 0; i < e->wrapper->len; i++) {
            if (!segs[i] || segs[i]->type != RAY_ENCCOL) continue;
            ray_t* err = ray_enccol_check_dir_all(segs[i]);
            if (err) return err;
        }
        atomic_store_explicit(&e->mat_dir_ok, 1, memory_order_release);
        return NULL;
    }
    if (e->kind != RAY_ENCCOL_SINGLE) return NULL;
    ray_t* m = ray_enccol_decoded_peek(encv);
    if (!m) return NULL;
    const uint8_t esz = ray_enccol_esz(e);
    for (int64_t c = 0; c < e->n_chunks; c++) {
        ray_enc_dirent_t de; dirent_at(e, c, &de);
        if (de.flags & RAY_ENC_STAT_ABSENT) continue;
        const void* d = (const uint8_t*)ray_data(m) + (size_t)(c * RAY_ENC_CHUNK) * esz;
        if (!dir_range_ok(e->type, d, chunk_rows(e, c), de.null_count > 0, de.min, de.max))
            return ray_error("corrupt", "column %s: chunk %lld value outside the chunk's statistics", e->path, (long long)c);
    }
    atomic_store_explicit(&e->mat_dir_ok, 1, memory_order_release);
    return NULL;
}

void ray_enccol_chunk_stat(const ray_enccol_t* e, int64_t c, ray_enc_chunk_stat_t* out) {
    ray_enc_dirent_t de;
    dirent_at(e, c, &de);
    out->rows = chunk_rows(e, c);
    out->has_minmax = (de.flags & RAY_ENC_STAT_ABSENT) == 0;
    out->min = de.min; out->max = de.max;
    out->null_count = de.null_count;
    out->sum_lo = de.sum_lo; out->sum_hi = de.sum_hi;
}

void ray_enccol_dir_totals(ray_enccol_t* e, ray_enc_col_totals_t* out) {
    if (atomic_load_explicit(&e->totals_state, memory_order_acquire) == 2) { *out = e->totals; return; }
    bool fl = enc_type_float(e->type);
    out->sum_wrapped = 0; out->sum_hi = 0; out->sum_lo = 0; out->nonnull = 0;
    out->min_i = INT64_MAX; out->max_i = INT64_MIN;
    out->min_f = INFINITY;  out->max_f = -INFINITY;
    out->mm_partial = false;
    for (int64_t c = 0; c < e->n_chunks; c++) {
        ray_enc_dirent_t de;
        dirent_at(e, c, &de);
        out->sum_wrapped += de.sum_lo;
        uint64_t l = out->sum_lo + de.sum_lo;              /* ray_i128_add128 */
        out->sum_hi += (int64_t)de.sum_hi + (l < de.sum_lo ? 1 : 0);
        out->sum_lo = l;
        out->nonnull += chunk_rows(e, c) - (int64_t)de.null_count;
        if (de.flags & RAY_ENC_STAT_ABSENT) {
            if ((int64_t)de.null_count < chunk_rows(e, c)) out->mm_partial = true;
            continue;
        }
        if (fl) {
            double mn = dbl_of(de.min), mx = dbl_of(de.max);
            if (mn < out->min_f) out->min_f = mn;
            if (mx > out->max_f) out->max_f = mx;
        } else {
            if (de.min < out->min_i) out->min_i = de.min;
            if (de.max > out->max_i) out->max_i = de.max;
        }
    }
    /* first finisher stores the (identical) totals; readers wait for 2 */
    uint8_t idle = 0;
    if (atomic_compare_exchange_strong_explicit(&e->totals_state, &idle, 1,
                                                memory_order_acq_rel, memory_order_relaxed)) {
        e->totals = *out;
        atomic_store_explicit(&e->totals_state, 2, memory_order_release);
    }
}

void ray_enccol_byte_range(const ray_enccol_t* e, int64_t lo, int64_t hi,
                           uint64_t* off, uint64_t* len) {
    int64_t c0 = lo / RAY_ENC_CHUNK, c1 = (hi - 1) / RAY_ENC_CHUNK;
    ray_enc_dirent_t a, b;
    dirent_at(e, c0, &a);
    dirent_at(e, c1, &b);
    *off = a.offset;
    *len = b.offset + b.bytes - a.offset;
}

