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
#include "ops/idxop.h"            /* ray_index_kind_admits */
#include "lang/format.h"          /* ray_type_name */
#include "core/platform.h"
#include "mem/sys.h"             /* ray_sys_alloc: the map descriptor, as col.c */
#include "table/sym.h"
#include "table/domain.h"
#include "store/enc.h"
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

static void enccol_free(ray_enccol_t* e) {
    if (e->decoded) ray_release(e->decoded);
    if (e->dict)    ray_release(e->dict);
    if (e->wrapper) ray_release(e->wrapper);
    if (e->dom && e->dom_owned) ray_sym_domain_release(e->dom);
    if (e->map)     ray_file_map_release(e->map);
    ray_free_raw(e);
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
    atomic_store(&e->rc, 1);
    e->kind = RAY_ENCCOL_SINGLE;
    e->map = m; e->base = (const uint8_t*)mapped; e->size = size;
    snprintf(e->path, sizeof e->path, "%s", path ? path : "?");
    ray_enc_once_init(&e->once);
    ray_enc_once_init(&e->dict_once);
    uint32_t saved = 0;
    ray_t* err = enc_validate_load(e, &saved);
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

ray_t* ray_enccol_chunk_descs(ray_enccol_t* e, int64_t c, ray_enc_desc_t out[64]) {
    static const uint8_t zero[48];
    ray_enc_dirent_t de; dirent_at(e, c, &de);
    const uint8_t* p = e->base + de.offset;
    memcpy(out, p, RAY_ENC_DESC_AREA);
    int64_t rows = chunk_rows(e, c);
    int nv = (int)((rows + RAY_ENC_VEC - 1) / RAY_ENC_VEC);
    uint64_t at = RAY_ENC_DESC_AREA;
    uint64_t nsum = 0;
    int T = lane_width(e);
    for (int j = 0; j < RAY_ENC_VECS_PER_CHUNK; j++) {
        const ray_enc_desc_t* d = &out[j];
        if (j >= nv) {
            if (memcmp(p + (size_t)j * 48, zero, 48) != 0)
                ENC_CORRUPT("col %s: chunk %lld trailing descriptor %d not zero", e->path, (long long)c, j);
            continue;
        }
        int vlen = (int)(rows - (int64_t)j * RAY_ENC_VEC < RAY_ENC_VEC ? rows - (int64_t)j * RAY_ENC_VEC : RAY_ENC_VEC);
        int cls = top_encoding_class(e->type, d->encoding);
        if (cls == 0) ENC_CORRUPT("col %s: chunk %lld vector %d illegal encoding %u", e->path, (long long)c, j, d->encoding);
        if (cls == 2) return ray_error("nyi", "col %s: encoding %u needs a newer build", e->path, d->encoding);
        if (d->rsv0 || d->rsv1 || d->rsv2) ENC_CORRUPT("col %s: chunk %lld vector %d descriptor reserved fields", e->path, (long long)c, j);
        uint16_t okflags = RAY_ENC_STAT_ABSENT | (enc_type_float(e->type) ? RAY_ENC_DESC_TRAILER : 0);
        if (d->flags & ~okflags) ENC_CORRUPT("col %s: chunk %lld vector %d flags 0x%x", e->path, (long long)c, j, d->flags);
        bool packed = d->encoding == RAY_ENC_FOR_BP || d->encoding == RAY_ENC_DELTA_BP;
        if (packed ? d->width > T : d->width != 0) ENC_CORRUPT("col %s: chunk %lld vector %d width %u", e->path, (long long)c, j, d->width);
        if (d->n_nulls > vlen) ENC_CORRUPT("col %s: chunk %lld vector %d null count %u above the vector length", e->path, (long long)c, j, d->n_nulls);
        if (!enc_type_nullable(e->type) && d->n_nulls)
            ENC_CORRUPT("col %s: chunk %lld vector %d nulls counted in a non-nullable type", e->path, (long long)c, j);
        if (d->payload_off != at) ENC_CORRUPT("col %s: chunk %lld vector %d payload offset %llu, expected %llu", e->path, (long long)c, j,
                                             (unsigned long long)d->payload_off, (unsigned long long)at);
        if (d->payload_bytes & 7) ENC_CORRUPT("col %s: chunk %lld vector %d payload size not a multiple of 8", e->path, (long long)c, j);
        if (d->payload_bytes > de.bytes - at) ENC_CORRUPT("col %s: chunk %lld vector %d payload overruns the chunk", e->path, (long long)c, j);
        at += d->payload_bytes;
        nsum += d->n_nulls;
        bool absent = d->flags & RAY_ENC_STAT_ABSENT;
        bool all_null = enc_type_nullable(e->type) && d->n_nulls == vlen;
        if (all_null && d->encoding != RAY_ENC_CONST)
            ENC_CORRUPT("col %s: chunk %lld vector %d all-null vector not CONST", e->path, (long long)c, j);
        if (all_null && !absent)
            ENC_CORRUPT("col %s: chunk %lld vector %d all-null vector carries statistics", e->path, (long long)c, j);
        if (absent && (d->min || d->max)) ENC_CORRUPT("col %s: chunk %lld vector %d absent statistics not zero", e->path, (long long)c, j);
        if (!absent && e->type == RAY_GUID) ENC_CORRUPT("col %s: chunk %lld vector %d statistics on GUID", e->path, (long long)c, j);
        /* Only STR_DICT carries code bounds, and a STR_DICT vector already
         * returned nyi above: on RAW / CONST this is damage in every build. */
        if (!absent && e->type == RAY_STR) ENC_CORRUPT("col %s: chunk %lld vector %d statistics on a RAW / CONST string vector", e->path, (long long)c, j);
        if (!absent) {
            if (enc_type_float(e->type)) {
                double mn, mx; memcpy(&mn, &d->min, 8); memcpy(&mx, &d->max, 8);
                if (isnan(mn) || isnan(mx) || mn > mx) ENC_CORRUPT("col %s: chunk %lld vector %d float bounds", e->path, (long long)c, j);
            } else if (d->min > d->max) {
                ENC_CORRUPT("col %s: chunk %lld vector %d min above max", e->path, (long long)c, j);
            } else if (e->type == RAY_SYM && (d->min < 1 || (uint64_t)d->max >= e->sym_bound ||
                                              (T < 64 && ((uint64_t)d->max >> T)))) {
                ENC_CORRUPT("col %s: chunk %lld vector %d SYM statistics outside the domain or the id width", e->path, (long long)c, j);
            }
        }
    }
    if (nsum != de.null_count) ENC_CORRUPT("col %s: chunk %lld descriptor null counts sum to %llu, directory says %u", e->path,
                                           (long long)c, (unsigned long long)nsum, de.null_count);
    if (at != de.bytes) ENC_CORRUPT("col %s: chunk %lld has %llu bytes after its payloads", e->path,
                                    (long long)c, (unsigned long long)(de.bytes - at));
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
static bool enc_alloc_fails(const ray_enccol_t* e) {
    if (atomic_load(&g_fail_path_on) && strstr(e->path, g_fail_path)) return true;
    int64_t v = atomic_load(&g_fail_alloc_at);
    while (v > 0)
        if (atomic_compare_exchange_weak(&g_fail_alloc_at, &v, v - 1)) return v == 1;
    return false;
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
        ray_enc_snap_set(fail, "corrupt", "col %s: DICT vector without a VALUE_DICT entry", e->path);
        return NULL;
    }
    if (enc_alloc_fails(e)) { ray_enc_snap_set(fail, "oom", "col %s: VALUE_DICT", e->path); return NULL; }
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
        if (!vals || RAY_IS_ERR(vals)) { ray_enc_snap_set(fail, "oom", "col %s: VALUE_DICT", e->path); return NULL; }
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
    ray_enc_snap_set(fail, "corrupt", "col %s: %s", e->path, why);
    return NULL;
}

static ray_t* enc_dict(ray_enccol_t* e, ray_enc_dict_t* out) {
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

#define VEC_CORRUPT(what) ENC_CORRUPT("col %s: chunk %lld vector %d %s", e->path, (long long)c, j, what)

static ray_t* dec_ints(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d, int vlen,
                       const uint8_t* p, uint64_t avail, const uint8_t* bm, void* out) {
    ray_enc_kind_t k;
    if (!ray_enc_kind_of(e->type, e->attrs, &k)) VEC_CORRUPT("kind");
    ray_enc_dict_t dict = {0};
    if (d->encoding == RAY_ENC_DICT) { ray_t* err = enc_dict(e, &dict); if (err) return err; }
    uint64_t bits[RAY_ENC_VEC];
    int64_t used = ray_enc_read(p, (size_t)avail, d->encoding, d->width, vlen, k,
                                RAY_ENC_SLOT_TOP, 0, d->encoding == RAY_ENC_DICT ? &dict : NULL, bits);
    if (used < 0) ENC_CORRUPT("col %s: chunk %lld vector %d codec body: %s", e->path, (long long)c, j, ray_enc_read_why());
    if ((uint64_t)used != avail) VEC_CORRUPT("bytes after the codec body");
    uint8_t esz = ray_enccol_esz(e);
    const bool sym = e->type == RAY_SYM;
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
    for (int i = 0; i < vlen; i++) {
        bool null = bm && ((bm[i >> 3] >> (i & 7)) & 1);
        uint8_t* q = (uint8_t*)out + i * esz;
        if (null) {
            if (f32) { float n = NULL_F32; memcpy(q, &n, 4); } else { double n = NULL_F64; memcpy(q, &n, 8); }
        } else {
            double x; if (f32) { float f; memcpy(&f, q, 4); x = f; } else memcpy(&x, q, 8);
            if (x != x) VEC_CORRUPT("NaN outside the bitmap");
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

ray_t* ray_enccol_decode_desc(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d,
                              void* out, char* pool_out, uint64_t pool_base, uint64_t pool_cap) {
    int64_t rows = chunk_rows(e, c);
    int vlen = (int)(rows - (int64_t)j * RAY_ENC_VEC < RAY_ENC_VEC ? rows - (int64_t)j * RAY_ENC_VEC : RAY_ENC_VEC);
    const uint8_t* body = NULL; uint64_t avail = 0; const uint8_t* bm = NULL;
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
    } else {
        /* decode aside and publish only a fully validated vector */
        uint8_t tmp[RAY_ENC_VEC * 16];
        switch (e->type) {
        case RAY_F32: case RAY_F64: err = dec_floats(e, c, j, d, vlen, body, avail, bm, tmp); break;
        case RAY_GUID:              err = dec_guid(e, c, j, d, vlen, body, avail, tmp); break;
        default:                    err = dec_ints(e, c, j, d, vlen, body, avail, bm, tmp); break;
        }
        if (!err) memcpy(out, tmp, nbytes);
    }
    if (!err) {
        uint64_t nb = (uint64_t)nbytes + pooled;
        atomic_fetch_add_explicit(&g_enc_decoded_bytes, nb, memory_order_relaxed);
        atomic_fetch_add_explicit(&e->decoded_bytes, nb, memory_order_relaxed);   /* per column */
    }
    return err;
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
    if (vec < 0 || vec >= ray_enc_nvecs(e->len)) return ray_error("range", "col %s: vector %lld", e->path, (long long)vec);
    ray_enc_desc_t descs[64];
    int64_t c = vec / RAY_ENC_VECS_PER_CHUNK;
    ray_t* err = ray_enccol_chunk_descs(e, c, descs);
    if (err) return err;
    int j = (int)(vec % RAY_ENC_VECS_PER_CHUNK);
    return ray_enccol_decode_desc(e, c, j, &descs[j], out, pool_out, pool_base, pool_cap);
}

ray_t* ray_enccol_vec_pool_bytes(ray_enccol_t* e, int64_t vec, uint64_t* out) {
    *out = 0;
    if (vec < 0 || vec >= ray_enc_nvecs(e->len)) return ray_error("range", "col %s: vector %lld", e->path, (long long)vec);
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
            ray_enc_snap_set(&r->snap, "cancel", "col %s: interrupted", e->path);
            r->failed = true;
            continue;
        }
        ray_enc_desc_t descs[64];
        ray_t* err = ray_enccol_chunk_descs(e, c, descs);
        int64_t rows = chunk_rows(e, c);
        int nv = (int)((rows + RAY_ENC_VEC - 1) / RAY_ENC_VEC);
        uint64_t pbase = x->chunk_pool ? x->chunk_pool[c] : 0;
        for (int j = 0; !err && j < nv; j++) {
            int64_t row = c * RAY_ENC_CHUNK + (int64_t)j * RAY_ENC_VEC;
            uint8_t* dst = x->out + (size_t)row * esz;
            err = ray_enccol_decode_desc(e, c, j, &descs[j], dst, x->pool, pbase, x->pool_cap);
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
                return ray_error("corrupt", "col %s: decoded pool exceeds str_pool_bytes at chunk %lld", e->path, (long long)c);
            total += pb;
        }
    }
    if (total != e->hdr.str_pool_bytes)
        return ray_error("corrupt", "col %s: decoded pool is %llu bytes, header says %llu", e->path,
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
    if (!root || RAY_IS_ERR(root)) return root ? root : ray_error("oom", "col %s: zone", e->path);
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
    if (!root || RAY_IS_ERR(root)) return root ? root : ray_error("oom", "col %s: chunk zone", e->path);
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
        return ray_error("oom", "col %s: chunk zone", e->path);
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
            return ray_error("corrupt", "col %s: zone index on type %s", e->path, ray_type_name(e->type));
        root = kind == RAY_IDX_ZONE ? enc_synth_zone(e) : enc_synth_chunk_zone(e);
        if (RAY_IS_ERR(root)) return root;
    } else if (kind != 0) {
        return ray_error("corrupt", "col %s: index kind %u without a region", e->path, kind);
    }
    if (unique) {
        if (!root) {
            root = enc_alloc_fails(e) ? NULL : ray_index_new_root(RAY_IDX_NONE, e->type, e->len);
            if (!root || RAY_IS_ERR(root)) return root ? root : ray_error("oom", "col %s: unique marker", e->path);
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
        return ray_error("corrupt", "col %s: index or UNIQUE flag not admitted on type %s",
                         e->path, ray_type_name(e->type));
    }
    *vp = r;
    return NULL;
}

static ray_t* enc_materialize_single(void* ctx, ray_enc_snap_t* fail) {
    ray_enccol_t* e = ctx;
    uint8_t ik = e->hdr.index_kind;
    if (ik == 1 || ik == 2 || ik == 4 || ik == 6) {
        ray_enc_snap_set(fail, "nyi", "col %s: index region needs a newer build", e->path);
        return NULL;
    }
    if (ray_interrupted()) { ray_enc_snap_set(fail, "cancel", "col %s: interrupted", e->path); return NULL; }
    if (enc_alloc_fails(e)) { ray_enc_snap_set(fail, "oom", "col %s: materialize", e->path); return NULL; }
    ray_t* vec = e->type == RAY_SYM ? ray_sym_vec_new(e->attrs & RAY_SYM_W_MASK, e->len)
                                    : ray_vec_new(e->type, e->len);
    if (!vec || RAY_IS_ERR(vec)) {
        if (vec) ray_enc_snap_take(fail, vec); else ray_enc_snap_set(fail, "oom", "col %s: materialize", e->path);
        return NULL;
    }
    vec->len = e->len;
    ray_t* err = NULL;
    uint64_t* chunk_pool = NULL;
    enc_task_res_t* res = NULL;
    if (e->type == RAY_STR) {
        chunk_pool = enc_alloc_fails(e) ? NULL : ray_alloc_raw((size_t)(e->n_chunks + 1) * sizeof(uint64_t));
        if (!chunk_pool) { err = ray_error("oom", "col %s: materialize", e->path); goto fail; }
        if ((err = enc_str_prepass(e, chunk_pool)) != NULL) goto fail;
        if (e->hdr.str_pool_bytes > 0) {
            ray_t* pool = enc_alloc_fails(e) ? NULL : ray_alloc((size_t)e->hdr.str_pool_bytes);
            if (!pool || RAY_IS_ERR(pool)) { err = pool ? pool : ray_error("oom", "col %s: pool", e->path); goto fail; }
            pool->type = RAY_U8;
            pool->len  = (int64_t)e->hdr.str_pool_bytes;
            vec->str_pool = pool;
        }
    }
    res = enc_alloc_fails(e) ? NULL : ray_calloc_raw((size_t)(e->n_chunks + 1) * sizeof *res);
    if (!res) { err = ray_error("oom", "col %s: materialize", e->path); goto fail; }
    enc_mat_ctx_t x = {
        .e = e, .out = (uint8_t*)ray_data(vec),
        .pool = vec->str_pool ? (char*)ray_data(vec->str_pool) : NULL,
        .pool_cap = e->hdr.str_pool_bytes, .chunk_pool = chunk_pool, .res = res,
    };
    atomic_init(&x.lowest_failed, INT64_MAX);
    if (e->n_chunks > (int64_t)UINT32_MAX) { err = ray_error("range", "col %s: too many chunks", e->path); goto fail; }
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
            else ray_enc_snap_set(fail, "cancel", "col %s: interrupted", e->path);
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

ray_t* ray_enccol_parted_new(ray_t* wrapper) {
    if (!wrapper || RAY_IS_ERR(wrapper) || !RAY_IS_PARTED(wrapper->type))
        return ray_error("type", "parted encoded column needs a parted wrapper");
    ray_enccol_t* e = ray_calloc_raw(sizeof *e);
    if (!e) return ray_error("oom", NULL);
    atomic_store(&e->rc, 1);
    e->kind = RAY_ENCCOL_PARTED;
    e->type = (int8_t)RAY_PARTED_BASETYPE(wrapper->type);
    e->len = ray_parted_nrows(wrapper);
    ray_t** segs = (ray_t**)ray_data(wrapper);
    for (int64_t i = 0; i < wrapper->len; i++) {
        if (!segs[i]) continue;
        ray_col_meta_t m;
        if (!ray_col_meta_of(segs[i], &m)) continue;
        bool nulls = m.null_count >= 0 ? m.null_count > 0 : (m.attrs & RAY_ATTR_HAS_NULLS) != 0;
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
        ray_enc_snap_set(fail, "oom", "col %s: parted materialize", e->path);
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
            else ray_enc_snap_set(fail, "cancel", "col %s: interrupted", e->path);
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
