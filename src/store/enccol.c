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
    if (rsv || ne == 0 || ne > 3 || 8 + 24ull * ne > sl)
        ENC_CORRUPT("col %s: side table header", e->path);
    uint64_t at = 8 + 24ull * ne;              /* end of the table */
    bool seen[4] = { false, false, false, false };
    for (uint32_t i = 0; i < ne; i++) {
        const uint8_t* t = s + 8 + 24 * i;
        uint16_t kind, r16; uint32_t r32;
        memcpy(&kind, t, 2); memcpy(&r16, t + 2, 2); memcpy(&r32, t + 4, 4);
        uint64_t off = rd_u64(t + 8), len = rd_u64(t + 16);
        if (r16 || r32 || kind < RAY_ENC_SIDE_VALUE_DICT || kind > RAY_ENC_SIDE_FSST_TABLE || seen[kind])
            ENC_CORRUPT("col %s: side entry %u kind %u", e->path, i, kind);
        seen[kind] = true;
        if ((off & 31) || off != ray_enc_pad32(at) || len < 8 || len > sl || off > sl - len)
            ENC_CORRUPT("col %s: side entry %u placement", e->path, i);
        if (!all_zero(s + at, (size_t)(off - at))) ENC_CORRUPT("col %s: side padding", e->path);
        if (kind == RAY_ENC_SIDE_VALUE_DICT) {
            if (!ray_encw_type_has_dict(e->type)) ENC_CORRUPT("col %s: VALUE_DICT on type %s", e->path, ray_type_name(e->type));
            e->vdict_off = e->hdr.side_off + off; e->vdict_len = len;
        } else if (e->type != RAY_STR) {
            ENC_CORRUPT("col %s: string side entry on type %s", e->path, ray_type_name(e->type));
        }
        at = off + len;
    }
    if (at != sl && !all_zero(s + at, (size_t)(sl - at))) ENC_CORRUPT("col %s: side tail", e->path);
    if (ray_enc_pad32(at) < sl) ENC_CORRUPT("col %s: side tail longer than padding", e->path);
    return NULL;
}

static ray_t* enc_validate_load(ray_enccol_t* e, uint32_t* saved_sym) {
    const uint8_t* b = e->base;
    uint64_t size = e->size;
    if (size < RAY_ENC_DIR_OFF) ENC_CORRUPT("col %s: generation-2 file shorter than its headers", e->path);
    ray_t h; memcpy(&h, b, 32);
    if (!all_zero(b + 4, 12) || h.mmod != 0)
        ENC_CORRUPT("col %s: reserved header bytes set", e->path);
    if (!ray_encw_type_supported(h.type)) ENC_CORRUPT("col %s: type %d not storable in generation 2", e->path, (int)h.type);
    if (h.attrs & ~COL_DISK_ATTRS_MASK) ENC_CORRUPT("col %s: attrs 0x%02x outside the persisted set", e->path, h.attrs);
    if (h.type != RAY_SYM && (h.attrs & RAY_SYM_W_MASK)) ENC_CORRUPT("col %s: width bits on a non-SYM column", e->path);
    if (h.len < 0) ENC_CORRUPT("col %s: negative length", e->path);
    uint32_t rc; memcpy(&rc, b + offsetof(ray_t, rc), 4);
    if (h.type != RAY_SYM && rc != 0) ENC_CORRUPT("col %s: SYM count on a non-SYM column", e->path);
    *saved_sym = rc;
    e->type = h.type; e->attrs = h.attrs; e->len = h.len;

    ray_enc_hdr_t* eh = &e->hdr;
    memcpy(eh, b + RAY_ENC_HDR_OFF, sizeof *eh);
    if (memcmp(eh->magic, "RENC", 4) != 0) ENC_CORRUPT("col %s: missing RENC header", e->path);
    if (eh->header_version != RAY_ENC_HDR_VERSION)
        return ray_error("version", "col %s: generation-2 header version %u", e->path, eh->header_version);
    if (eh->index_kind > 6) ENC_CORRUPT("col %s: index kind %u", e->path, eh->index_kind);
    /* the admission matrix, shared with the writer and the attach (spec
     * "Materialization" step 3): a pair it refuses is a wire violation */
    if (eh->index_kind && !ray_index_kind_admits((ray_idx_kind_t)eh->index_kind, h.type))
        ENC_CORRUPT("col %s: index kind %u not admitted on type %s", e->path, eh->index_kind, ray_type_name(h.type));
    if ((eh->col_flags & RAY_ENC_FLAG_UNIQUE) && !ray_index_kind_admits(RAY_IDX_NONE, h.type))
        ENC_CORRUPT("col %s: UNIQUE not admitted on type %s", e->path, ray_type_name(h.type));
    if (eh->col_flags & ~RAY_ENC_FLAG_UNIQUE) ENC_CORRUPT("col %s: reserved column flags", e->path);
    if (eh->rsv0 || eh->rsv1 || eh->rsv2 || eh->rsv3) ENC_CORRUPT("col %s: reserved enc header fields set", e->path);
    if (eh->str_pool_bytes > UINT32_MAX || (h.type != RAY_STR && eh->str_pool_bytes))
        ENC_CORRUPT("col %s: str_pool_bytes %llu", e->path, (unsigned long long)eh->str_pool_bytes);
    if (eh->end_off != size)
        ENC_CORRUPT("col %s: end offset %llu, file is %llu bytes (truncated?)", e->path,
                    (unsigned long long)eh->end_off, (unsigned long long)size);

    e->n_chunks = ray_enc_nchunks(h.len);
    if ((uint64_t)e->n_chunks > (size - RAY_ENC_DIR_OFF) / sizeof(ray_enc_dirent_t))
        ENC_CORRUPT("col %s: directory past the end of the file", e->path);
    uint64_t prev = RAY_ENC_DIR_OFF + (uint64_t)e->n_chunks * sizeof(ray_enc_dirent_t);
    bool summable = ray_encw_type_summable(h.type);
    e->null_count = 0;
    for (int64_t c = 0; c < e->n_chunks; c++) {
        ray_enc_dirent_t de;
        memcpy(&de, b + RAY_ENC_DIR_OFF + c * sizeof de, sizeof de);
        int64_t rows = h.len - c * RAY_ENC_CHUNK;
        if (rows > RAY_ENC_CHUNK) rows = RAY_ENC_CHUNK;
        if (de.offset != prev) ENC_CORRUPT("col %s: chunk %lld at %llu, expected %llu", e->path,
                                           (long long)c, (unsigned long long)de.offset, (unsigned long long)prev);
        if (de.bytes < RAY_ENC_DESC_AREA || (de.bytes & 7) || de.bytes > size - de.offset)
            ENC_CORRUPT("col %s: chunk %lld size %llu", e->path, (long long)c, (unsigned long long)de.bytes);
        if (de.rsv0 || de.rsv1 || (de.flags & ~RAY_ENC_STAT_ABSENT))
            ENC_CORRUPT("col %s: chunk %lld reserved directory bits", e->path, (long long)c);
        if ((int64_t)de.null_count > rows) ENC_CORRUPT("col %s: chunk %lld null count %u over %lld rows",
                                                      e->path, (long long)c, de.null_count, (long long)rows);
        if (!enc_type_nullable(h.type) && de.null_count) ENC_CORRUPT("col %s: nulls in a non-nullable column", e->path);
        bool absent = de.flags & RAY_ENC_STAT_ABSENT;
        if ((int64_t)de.null_count == rows && !absent)
            ENC_CORRUPT("col %s: chunk %lld statistics on an all-null chunk", e->path, (long long)c);
        if (h.type == RAY_GUID && !absent)
            ENC_CORRUPT("col %s: chunk %lld statistics on GUID", e->path, (long long)c);
        /* STR code bounds exist exactly when every vector is STR_DICT (P6):
         * a valid newer file, not a damaged one. */
        if (h.type == RAY_STR && !absent)
            return ray_error("nyi", "col %s: STR dictionary statistics need a newer build", e->path);
        if (absent && (de.min || de.max)) ENC_CORRUPT("col %s: chunk %lld absent statistics not zero", e->path, (long long)c);
        if (!absent) {
            if (enc_type_float(h.type)) {
                double mn, mx; memcpy(&mn, &de.min, 8); memcpy(&mx, &de.max, 8);
                if (isnan(mn) || isnan(mx) || mn > mx) ENC_CORRUPT("col %s: chunk %lld float bounds", e->path, (long long)c);
            } else if (de.min > de.max) {
                ENC_CORRUPT("col %s: chunk %lld min above max", e->path, (long long)c);
            }
        }
        if (!summable && (de.sum_lo || de.sum_hi)) ENC_CORRUPT("col %s: chunk %lld sum on an unsummed type", e->path, (long long)c);
        if ((int64_t)de.null_count == rows && (de.sum_lo || de.sum_hi)) ENC_CORRUPT("col %s: chunk %lld sum of no values", e->path, (long long)c);
        e->null_count += de.null_count;
        prev = de.offset + de.bytes;
    }
    if (eh->side_off) {
        if (eh->side_off != ray_enc_pad32(prev) || eh->side_len < 8 || eh->side_off > size ||
            eh->side_len > size - eh->side_off || !all_zero(b + prev, (size_t)(eh->side_off - prev)))
            ENC_CORRUPT("col %s: side data placement", e->path);
        ray_t* serr = enc_validate_side_table(e);
        if (serr) return serr;
        prev = eh->side_off + eh->side_len;
    } else if (eh->side_len) {
        ENC_CORRUPT("col %s: side length without side data", e->path);
    }
    bool want_index = eh->index_kind == 1 || eh->index_kind == 2 ||
                      eh->index_kind == 4 || eh->index_kind == 6;
    if (want_index != (eh->index_off != 0)) ENC_CORRUPT("col %s: index kind %u disagrees with index offset", e->path, eh->index_kind);
    if (eh->index_off) {
        if (eh->index_off != ray_enc_pad32(prev) || eh->index_off >= size ||
            !all_zero(b + prev, (size_t)(eh->index_off - prev)))
            ENC_CORRUPT("col %s: index region placement", e->path);
        prev = size;
    }
    if (prev != size) ENC_CORRUPT("col %s: %llu bytes after the last section", e->path, (unsigned long long)(size - prev));
    return NULL;
}

ray_t* ray_enccol_open(void* mapped, size_t size, const char* path,
                       struct ray_sym_domain_s* dom, bool require_dom) {
#if !RAY_ENC_LE
    ray_vm_unmap_file(mapped, size);
    return ray_error("nyi", "col %s: generation 2 needs a little-endian host", path);
#else
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
                err = ray_error("corrupt", "col %s: chunk %lld SYM ids outside the symbol domain", e->path, (long long)c);
        }
    }
    if (err) { enccol_free(e); return err; }
    ray_t* v = ray_enccol_wrap(e);
    if (!v || RAY_IS_ERR(v)) enccol_free(e);
    return v;
#endif
}
