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
#include "store/encidx.h"        /* the index region: parse */
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
#include "ops/hash.h"            /* ray_hash_bytes: the STR dictionary lookup table */
#include "core/pool.h"
#include "mem/heap.h"            /* ray_heap_anon_watermark: the trim budget */
#include "core/block.h"          /* ray_block_size: what a kept decode occupies */
#include "lang/eval.h"           /* ray_eval_depth: the trim waits for the statement */
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

/* Columns whose whole-column decode is published, for the statement-
 * boundary trim.  A decode may be published from a worker, so the list has
 * its own lock; only the boundary drops decodes. */
static atomic_flag    g_dec_lock = ATOMIC_FLAG_INIT;
static ray_enccol_t*  g_dec_head;
static _Atomic(uint64_t) g_dec_stmt = 1;

static void dec_lock(void)   { while (atomic_flag_test_and_set_explicit(&g_dec_lock, memory_order_acquire)) ray_enc_yield(); }
static void dec_unlock(void) { atomic_flag_clear_explicit(&g_dec_lock, memory_order_release); }

static void dec_unlink(ray_enccol_t* e) {   /* under the lock */
    if (!atomic_load_explicit(&e->dec_listed, memory_order_relaxed)) return;
    if (e->dec_prev) e->dec_prev->dec_next = e->dec_next; else g_dec_head = e->dec_next;
    if (e->dec_next) e->dec_next->dec_prev = e->dec_prev;
    e->dec_prev = e->dec_next = NULL;
    atomic_store_explicit(&e->dec_listed, false, memory_order_release);
}

/* Bytes a kept decode frees when dropped: none while something else holds it. */
static int64_t dec_bytes(ray_t* d) {
    if (!d || ray_atomic_load(&d->rc) > 1) return 0;
    return (int64_t)ray_block_size(d);
}

static void dec_note(ray_enccol_t* e) {
    atomic_store_explicit(&e->dec_used, atomic_load_explicit(&g_dec_stmt, memory_order_relaxed),
                          memory_order_relaxed);
    if (atomic_load_explicit(&e->dec_listed, memory_order_acquire)) return;
    dec_lock();
    if (!atomic_load_explicit(&e->dec_listed, memory_order_relaxed) && e->decoded) {
        e->dec_prev = NULL;
        e->dec_next = g_dec_head;
        if (g_dec_head) g_dec_head->dec_prev = e;
        g_dec_head = e;
        atomic_store_explicit(&e->dec_listed, true, memory_order_release);
    }
    dec_unlock();
}

int64_t ray_enccol_cache_bytes(void) {
    int64_t total = 0;
    dec_lock();
    for (ray_enccol_t* e = g_dec_head; e; e = e->dec_next) total += dec_bytes(e->decoded);
    dec_unlock();
    return total;
}

void ray_enccol_cache_trim(void) {
    uint64_t stmt = atomic_fetch_add_explicit(&g_dec_stmt, 1, memory_order_relaxed);
    int64_t wm = ray_heap_anon_watermark();
    if (wm <= 0) return;
    if (ray_eval_depth() > 0) return;     /* a statement is still on the stack */
    if (atomic_load_explicit(&ray_parallel_flag, memory_order_acquire) != 0) return;
    int64_t cap = wm / 4;
    dec_lock();
    int64_t total = 0;
    for (ray_enccol_t* e = g_dec_head; e; e = e->dec_next) total += dec_bytes(e->decoded);
    while (total > cap) {
        ray_enccol_t* v = NULL;
        uint64_t vu = 0; int64_t vb = 0;
        for (ray_enccol_t* e = g_dec_head; e; e = e->dec_next) {
            int64_t  b = dec_bytes(e->decoded);
            if (b == 0) continue;
            uint64_t u = atomic_load_explicit(&e->dec_used, memory_order_relaxed);
            if (!v || u < vu || (u == vu && b > vb)) { v = e; vu = u; vb = b; }
        }
        if (!v) break;
        ray_t* d = v->decoded;
        v->decoded = NULL;
        ray_enc_once_init(&v->once);          /* the next read decodes again */
        dec_unlink(v);
        ray_idx_lazy_forget(d);               /* a lazy hash goes with its object */
        ray_release(d);
        total -= vb;
    }
    dec_unlock();
    (void)stmt;
}

static void enccol_free(ray_enccol_t* e) {
    atomic_fetch_sub_explicit(&g_enccol_live, 1, memory_order_relaxed);
    if (atomic_load_explicit(&e->dec_listed, memory_order_acquire)) { dec_lock(); dec_unlink(e); dec_unlock(); }
    ray_idx_lazy_forget_enc(e);              /* a pending probe count goes with the payload */
    if (e->decoded) {
        ray_idx_lazy_forget(e->decoded);     /* a lazy hash goes with its object */
        ray_release(e->decoded);
    }
    if (e->dict)    ray_release(e->dict);
    if (e->sdict)   ray_release(e->sdict);
    if (e->fsst)    ray_release(e->fsst);
    if (e->idict)   ray_release(e->idict);
    if (e->icodes)  ray_release(e->icodes);
    if (e->wrapper) ray_release(e->wrapper);
    if (e->dom && e->dom_owned) ray_sym_domain_release(e->dom);
    if (e->map)     ray_file_map_release(e->map);
    ray_free_raw((void*)e->touch_ok);
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

static bool enc_type_float(int8_t t)     { return t == RAY_F32 || t == RAY_F64; }

static uint64_t rd_u64(const uint8_t* p) { uint64_t x; memcpy(&x, p, 8); return x; }

#define ENC_CORRUPT(...) return ray_error("corrupt", __VA_ARGS__)

/* The side table (spec "Side data"): where each entry starts and how long
 * it is.  Its contents are taken as written; only its extents are checked,
 * once, so a short file fails whole instead of reading past its mapping. */
static ray_t* enc_parse_side_table(ray_enccol_t* e) {
    const uint8_t* s = e->base + e->hdr.side_off;
    uint64_t sl = e->hdr.side_len;
    uint32_t ne; memcpy(&ne, s, 4);
    if (ne == 0 || ne > 3) ENC_CORRUPT("column %s: side table entry count %u", e->path, ne);
    if (8 + 24ull * ne > sl) ENC_CORRUPT("column %s: side table longer than the side data", e->path);
    for (uint32_t i = 0; i < ne; i++) {
        const uint8_t* t = s + 8 + 24 * i;
        uint16_t kind; memcpy(&kind, t, 2);
        uint64_t off = rd_u64(t + 8), len = rd_u64(t + 16);
        if (len < 8 || len > sl || off > sl - len)
            ENC_CORRUPT("column %s: side entry %u overruns the side data", e->path, i);
        if (kind == RAY_ENC_SIDE_VALUE_DICT)    { e->vdict_off = e->hdr.side_off + off; e->vdict_len = len; }
        else if (kind == RAY_ENC_SIDE_STR_DICT) { e->sdict_off = e->hdr.side_off + off; e->sdict_len = len; }
        else                                    { e->fsst_off = e->hdr.side_off + off; e->fsst_len = len; }
    }
    return NULL;
}

/* The headers, the chunk count, the directory's null total and the side
 * table.  Only the header version selects a reader; the values are taken
 * as written (files rayforce wrote are trusted).  The extents of every
 * section are checked against the file's size, once at open: a truncated
 * or short file fails whole, with its name. */
static ray_t* enc_parse_load(ray_enccol_t* e, uint32_t* saved_sym) {
    const uint8_t* b = e->base;
    uint64_t size = e->size;
    if (size < RAY_ENC_DIR_OFF) ENC_CORRUPT("column %s: generation-2 file shorter than its headers", e->path);
    ray_t h; memcpy(&h, b, 32);
    uint32_t rc; memcpy(&rc, b + offsetof(ray_t, rc), 4);
    *saved_sym = rc;
    e->type = h.type; e->attrs = h.attrs; e->len = h.len;
    ray_enc_hdr_t* eh = &e->hdr;
    memcpy(eh, b + RAY_ENC_HDR_OFF, sizeof *eh);
    if (eh->header_version != RAY_ENC_HDR_VERSION)
        return ray_error("version", "column %s: generation-2 header version %u", e->path, eh->header_version);
    if (eh->end_off != size)
        ENC_CORRUPT("column %s: end offset %llu, file is %llu bytes (truncated?)", e->path,
                    (unsigned long long)eh->end_off, (unsigned long long)size);
    /* the directory must fit: bound len before deriving the chunk count */
    if (h.len < 0 ||
        (uint64_t)h.len > ((size - RAY_ENC_DIR_OFF) / sizeof(ray_enc_dirent_t)) * (uint64_t)RAY_ENC_CHUNK)
        ENC_CORRUPT("column %s: length %lld exceeds what the file holds", e->path, (long long)h.len);
    e->n_chunks = ray_enc_nchunks(h.len);
    if ((uint64_t)e->n_chunks > (size - RAY_ENC_DIR_OFF) / sizeof(ray_enc_dirent_t))
        ENC_CORRUPT("column %s: directory past the end of the file", e->path);
    e->null_count = 0;
    for (int64_t c = 0; c < e->n_chunks; c++) {
        ray_enc_dirent_t de;
        memcpy(&de, b + RAY_ENC_DIR_OFF + c * sizeof de, sizeof de);
        if ((de.offset & 7) || de.bytes < RAY_ENC_DESC_AREA || de.offset > size || de.bytes > size - de.offset)
            ENC_CORRUPT("column %s: chunk %lld at %llu, %llu bytes, overruns the file", e->path, (long long)c,
                        (unsigned long long)de.offset, (unsigned long long)de.bytes);
        e->null_count += de.null_count;
    }
    if (eh->side_off) {
        if (eh->side_len < 8 || eh->side_off > size || eh->side_len > size - eh->side_off)
            ENC_CORRUPT("column %s: side data overruns the file", e->path);
        ray_t* serr = enc_parse_side_table(e);
        if (serr) return serr;
    }
    /* the region opens with the 80-byte wire root (spec "Index region") */
    if (eh->index_off && (eh->index_off >= size || size - eh->index_off < 80))
        ENC_CORRUPT("column %s: index region overruns the file", e->path);
    return NULL;
}

ray_t* ray_enccol_open(void* mapped, size_t size, const char* path,
                       struct ray_sym_domain_s* dom, bool require_dom) {
    /* Only this gate keeps a big-endian host from reading the
     * little-endian layout. */
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
    ray_enc_once_init(&e->sdict_once);
    ray_enc_once_init(&e->fsst_once);
    ray_enc_once_init(&e->idict_once);
    ray_enc_once_init(&e->icodes_once);
    uint32_t saved = 0;
    /* Descriptors and packed bodies are read in place as 8-byte words: the
     * format keeps every offset 8-aligned, and a mapping starts on a page. */
    ray_t* err = ((uintptr_t)mapped & 7) ? ray_error("nyi", "column %s: mapping not 8-aligned", e->path)
                                         : enc_parse_load(e, &saved);
    if (!err && e->n_chunks > 0 && e->type != RAY_GUID && e->type != RAY_STR) {
        e->touch_ok = ray_calloc_raw((size_t)e->n_chunks);
        if (!e->touch_ok) err = ray_error("oom", "column %s: chunk flags", e->path);
    }
    if (!err && e->type == RAY_SYM) {
        if (dom) {
            /* the symfile the caller named must hold every id the column
             * was saved over (a usage check: another table's symfile) */
            int64_t dcount = ray_sym_domain_count(dom);
            if (saved > 0 && (int64_t)saved > dcount)
                err = ray_error("corrupt", "column %s: saved sym count %u exceeds symfile count %lld",
                                e->path, saved, (long long)dcount);
            e->dom = dom; ray_sym_domain_retain(dom); e->dom_owned = true;
        } else if (require_dom) {
            err = ray_error("sym", "column %s: SYM data but no symfile resolved "
                                   "(missing <dir>/sym or explicit sym argument)", e->path);
        } else {
            /* runtime ids of another process: the live table must hold them */
            uint32_t cur = ray_sym_count();
            if (saved > 0 && cur > 0 && cur < saved) err = ray_error("corrupt", "column %s: saved sym count %u exceeds %u", e->path, saved, cur);
            e->dom = ray_sym_runtime_domain(); e->dom_owned = false;
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


/* Chunk c's 64 descriptors, in place in the mapping. */
const ray_enc_desc_t* ray_enccol_descs(const ray_enccol_t* e, int64_t c) {
    ray_enc_dirent_t de; dirent_at(e, c, &de);
    return (const ray_enc_desc_t*)(e->base + de.offset);   /* 8-aligned: ray_enccol_open */
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

/* ---- VALUE_DICT side entry: read once (spec "Side data") ---- */
static ray_t* enc_dict_load(void* ctx, ray_enc_snap_t* fail) {
    ray_enccol_t* e = ctx;
    ray_enc_kind_t k;
    (void)ray_enc_value_kind(e->type, e->attrs, &k);
    if (enc_alloc_fails(e)) { ray_enc_snap_set(fail, "oom", "column %s: VALUE_DICT", e->path); return NULL; }
    const uint8_t* d = e->base + e->vdict_off;
    uint32_t n; memcpy(&n, d, 4);
    const size_t esz = (size_t)k.T / 8;
    ray_t* vals = ray_vec_new(RAY_I64, n);
    if (!vals || RAY_IS_ERR(vals)) {
        ray_error_free(vals);  /* vec_new can return an owned error, not just NULL */
        ray_enc_snap_set(fail, "oom", "column %s: VALUE_DICT", e->path);
        return NULL;
    }
    vals->len = n;
    uint64_t* out = (uint64_t*)ray_data(vals);
    for (uint32_t i = 0; i < n; i++) { uint64_t x = 0; memcpy(&x, d + 8 + i * esz, esz); out[i] = x; }
    return vals;
}

/* The dictionary once published, else false (never loads): read without
 * the once-state's lock, as the DONE store releases the slot it publishes
 * (ray_enccol_decoded_peek relies on the same). */
static bool enc_dict_ready(const ray_enccol_t* e, ray_enc_dict_t* out) {
    if (atomic_load_explicit(&e->dict_once.state, memory_order_acquire) != RAY_ENC_ONCE_DONE) return false;
    out->v = (const uint64_t*)ray_data(e->dict);
    out->n = (uint32_t)e->dict->len;
    return true;
}

static ray_t* enc_dict(ray_enccol_t* e, ray_enc_dict_t* out) {
    if (enc_dict_ready(e, out)) return NULL;      /* every vector decode of a DICT column asks */
    ray_t* d = ray_enc_once_get(&e->dict_once, &e->dict, enc_dict_load, e);
    if (RAY_IS_ERR(d)) return d;
    out->v = (const uint64_t*)ray_data(d);
    out->n = (uint32_t)d->len;
    return NULL;
}

/* ---- STR side entries (P6): read once, before first use ---- */

/* The entry table of a STR dictionary (the STR_DICT side entry, or the
 * persisted DICT's entries, P6b): over n entries at offs / bytes, a zeroed
 * RAY_U8 blob of head + sizeof(ray_enc_sdict_t) + cap * 4 bytes whose
 * ray_enc_sdict_t sits at offset head (8-aligned), filled: the slot table,
 * null_code, pooled.  NULL with *blob set, or an owned oom error. */
static ray_t* enc_sdict_build(const ray_enccol_t* e, const char* what, const uint64_t* offs,
                              const uint8_t* bytes, uint64_t n, size_t head, ray_t** blob) {
    *blob = NULL;
    uint64_t cap = 16; while (cap < 2ull * n) cap <<= 1;
    const int64_t blen = (int64_t)(head + sizeof(ray_enc_sdict_t) + cap * sizeof(uint32_t));
    ray_t* b = ray_vec_new(RAY_U8, blen);
    if (!b || RAY_IS_ERR(b)) {
        if (b) ray_error_free(b);
        return ray_error("oom", "column %s: %s", e->path, what);
    }
    b->len = blen;
    memset(ray_data(b), 0, (size_t)blen);
    ray_enc_sdict_t* d = (ray_enc_sdict_t*)((uint8_t*)ray_data(b) + head);
    d->n = (uint32_t)n; d->null_code = UINT32_MAX; d->offs = offs; d->bytes = bytes; d->mask = cap - 1;
    for (uint64_t k = 0; k < n; k++) {
        const uint64_t len = offs[k + 1] - offs[k];
        if (len == 0) d->null_code = (uint32_t)k;    /* at most one: entries are distinct */
        if (len > RAY_STR_INLINE_MAX) d->pooled += len;
        uint64_t h = ray_hash_bytes(bytes + offs[k], (size_t)len) & d->mask;
        while (d->slot[h]) h = (h + 1) & d->mask;
        d->slot[h] = (uint32_t)k + 1;
    }
    *blob = b;
    return NULL;
}

/* The code of s in an entry table: >= 0, or -1 absent. */
static int64_t enc_sdict_find(const ray_enc_sdict_t* d, const char* s, size_t len) {
    uint64_t h = ray_hash_bytes(s, len) & d->mask;
    while (d->slot[h]) {
        const uint32_t q = d->slot[h] - 1;
        if (d->offs[q + 1] - d->offs[q] == len && memcmp(d->bytes + d->offs[q], s, len) == 0) return q;
        h = (h + 1) & d->mask;
    }
    return -1;
}

static ray_t* enc_sdict_load(void* ctx, ray_enc_snap_t* fail) {
    ray_enccol_t* e = ctx;
    if (enc_alloc_fails(e)) { ray_enc_snap_set(fail, "oom", "column %s: STR_DICT", e->path); return NULL; }
    const uint8_t* p = e->base + e->sdict_off;
    uint32_t n; memcpy(&n, p, 4);
    const uint64_t* offs = (const uint64_t*)(p + 8);  /* 8-aligned: 32-aligned entry in a mapping */
    const uint8_t* bytes = p + 8 + 8ull * (n + 1);
    ray_t* blob = NULL;
    ray_t* err = enc_sdict_build(e, "STR_DICT", offs, bytes, n, 0, &blob);
    if (err) { ray_enc_snap_take(fail, err); return NULL; }
    return blob;
}

/* The dictionary once published, else NULL (never loads): read without
 * the once-state's lock, as enc_dict_ready. */
static const ray_enc_sdict_t* enc_sdict_ready(const ray_enccol_t* e) {
    if (atomic_load_explicit(&e->sdict_once.state, memory_order_acquire) != RAY_ENC_ONCE_DONE) return NULL;
    return (const ray_enc_sdict_t*)ray_data(e->sdict);
}

ray_t* ray_enccol_strdict(ray_enccol_t* e, const ray_enc_sdict_t** out) {
    *out = NULL;
    if (e->type != RAY_STR || !e->sdict_len) return NULL;
    if ((*out = enc_sdict_ready(e)) != NULL) return NULL;      /* every STR_DICT vector decode asks */
    ray_t* b = ray_enc_once_get(&e->sdict_once, &e->sdict, enc_sdict_load, e);
    if (RAY_IS_ERR(b)) return b;
    *out = (const ray_enc_sdict_t*)ray_data(b);
    return NULL;
}

ray_t* ray_enccol_strdict_code(ray_enccol_t* e, const char* s, size_t len, int64_t* code) {
    const ray_enc_sdict_t* d;
    ray_t* err = ray_enccol_strdict(e, &d);
    if (err) return err;
    if (!d) { *code = -2; return NULL; }
    *code = enc_sdict_find(d, s, len);
    return NULL;
}

/* ---- the persisted DICT (P6b, spec "STR dictionary index") ----
 * Two once-states: the entry table (parsed from the region) precedes every
 * use; the heap copy of the codes precedes every whole-codes use. */
typedef struct {
    ray_enc_idict_t     pub;
} enc_idict_blob_t;
/* The blob: this head, then the entry table */
_Static_assert(sizeof(enc_idict_blob_t) % _Alignof(ray_enc_sdict_t) == 0, "the entry table follows aligned");

static ray_t* enc_idict_load(void* ctx, ray_enc_snap_t* fail) {
    ray_enccol_t* e = ctx;
    if (enc_alloc_fails(e)) { ray_enc_snap_set(fail, "oom", "column %s: DICT", e->path); return NULL; }
    ray_enc_dict_view_t v;
    ray_enc_dict_parse(e->base + e->hdr.index_off, &v);
    ray_t* blob = NULL;
    ray_t* err = enc_sdict_build(e, "DICT", v.offs, v.bytes, (uint64_t)v.n_distinct, sizeof(enc_idict_blob_t), &blob);
    if (err) { ray_enc_snap_take(fail, err); return NULL; }
    enc_idict_blob_t* d = (enc_idict_blob_t*)ray_data(blob);
    d->pub.len = v.len; d->pub.n_distinct = v.n_distinct; d->pub.codes = v.codes; d->pub.first_occ = v.first_occ;
    d->pub.ent = (const ray_enc_sdict_t*)((const uint8_t*)ray_data(blob) + sizeof(enc_idict_blob_t));
    return blob;
}

/* The entry table once published, else NULL (never loads): read without
 * the once-state's lock, as enc_sdict_ready. */
static const enc_idict_blob_t* enc_idict_ready(const ray_enccol_t* e) {
    if (atomic_load_explicit(&e->idict_once.state, memory_order_acquire) != RAY_ENC_ONCE_DONE) return NULL;
    return (const enc_idict_blob_t*)ray_data(e->idict);
}

bool ray_enccol_has_idict(const ray_enccol_t* e) {
    return e && e->kind == RAY_ENCCOL_SINGLE && !e->synthetic && e->hdr.index_kind == RAY_IDX_DICT;
}

static ray_t* enc_idict_get(ray_enccol_t* e, const enc_idict_blob_t** out) {
    *out = NULL;
    if (!ray_enccol_has_idict(e)) return NULL;
    if ((*out = enc_idict_ready(e)) != NULL) return NULL;
    ray_t* b = ray_enc_once_get(&e->idict_once, &e->idict, enc_idict_load, e);
    if (RAY_IS_ERR(b)) return b;
    *out = (const enc_idict_blob_t*)ray_data(b);
    return NULL;
}

ray_t* ray_enccol_idict(ray_enccol_t* e, const ray_enc_idict_t** out) {
    const enc_idict_blob_t* b;
    ray_t* err = enc_idict_get(e, &b);
    *out = b ? &b->pub : NULL;
    return err;
}

ray_t* ray_enccol_idict_code(ray_enccol_t* e, const char* s, size_t len, int64_t* code) {
    const ray_enc_idict_t* d;
    ray_t* err = ray_enccol_idict(e, &d);
    if (err) return err;
    if (!d) { *code = -2; return NULL; }
    *code = enc_sdict_find(d->ent, s, len);
    return NULL;
}

ray_t* ray_enccol_idict_codes(ray_enccol_t* e, int64_t start, int64_t n, const int32_t** out) {
    *out = NULL;
    const ray_enc_idict_t* d;
    ray_t* err = ray_enccol_idict(e, &d);
    if (err) return err;
    if (!d) return ray_error("type", "column %s: no DICT index", e ? e->path : "?");
    if (start < 0 || n < 0 || start > d->len || n > d->len - start)
        return ray_error("range", "column %s: DICT codes [%lld, +%lld) outside %lld rows", e->path,
                         (long long)start, (long long)n, (long long)d->len);
    *out = d->codes + start;
    return NULL;
}

/* The heap copy of the codes: not a materialization (spec
 * "Instrumentation", P6b), so neither e->once nor the counters move. */
static ray_t* enc_icodes_build(void* ctx, ray_enc_snap_t* fail) {
    ray_enccol_t* e = ctx;
    const enc_idict_blob_t* d;
    ray_t* err = enc_idict_get(e, &d);
    if (err) { ray_enc_snap_take(fail, err); return NULL; }
    if (!d) { ray_enc_snap_set(fail, "type", "column %s: no DICT index", e->path); return NULL; }
    ray_t* v = enc_alloc_fails(e) ? NULL : ray_vec_new(RAY_I32, d->pub.len);
    if (!v || RAY_IS_ERR(v)) {
        if (v) ray_error_free(v);
        ray_enc_snap_set(fail, "oom", "column %s: DICT codes", e->path);
        return NULL;
    }
    v->len = d->pub.len;
    memcpy(ray_data(v), d->pub.codes, (size_t)d->pub.len * sizeof(int32_t));
    return v;
}

ray_t* ray_enccol_idict_codes_vec(ray_enccol_t* e, ray_t** out) {
    *out = NULL;
    if (!e) return ray_error("type", "column ?: no DICT index");
    if (atomic_load_explicit(&e->icodes_once.state, memory_order_acquire) == RAY_ENC_ONCE_DONE) {
        *out = e->icodes; return NULL;
    }
    ray_t* v = ray_enc_once_get(&e->icodes_once, &e->icodes, enc_icodes_build, e);
    if (RAY_IS_ERR(v)) return v;
    *out = v;
    return NULL;
}

/* FSST_TABLE (spec "Side data"): u8 n, 7 zero bytes, n symbol lengths
 * padded to 8, n symbols of 8 bytes (zero past their length). */
static ray_t* enc_fsst_load(void* ctx, ray_enc_snap_t* fail) {
    ray_enccol_t* e = ctx;
    if (enc_alloc_fails(e)) { ray_enc_snap_set(fail, "oom", "column %s: FSST_TABLE", e->path); return NULL; }
    const uint8_t* p = e->base + e->fsst_off;
    const uint8_t n = p[0];
    ray_t* blob = ray_vec_new(RAY_U8, (int64_t)sizeof(ray_fsst_table_t));
    if (!blob || RAY_IS_ERR(blob)) {
        if (blob) ray_error_free(blob);
        ray_enc_snap_set(fail, "oom", "column %s: FSST_TABLE", e->path);
        return NULL;
    }
    blob->len = (int64_t)sizeof(ray_fsst_table_t);
    ray_fsst_table_t* t = (ray_fsst_table_t*)ray_data(blob);
    memset(t, 0, sizeof *t);
    t->n = n;
    const uint8_t* syms = p + 8 + ray_enc_pad8(n);
    for (int k = 0; k < n; k++) { t->len[k] = p[8 + k]; memcpy(t->sym[k], syms + 8 * k, 8); }
    return blob;
}

static ray_t* enc_fsst(ray_enccol_t* e, const ray_fsst_table_t** out) {
    *out = NULL;
    if (atomic_load_explicit(&e->fsst_once.state, memory_order_acquire) == RAY_ENC_ONCE_DONE) {
        *out = (const ray_fsst_table_t*)ray_data(e->fsst);     /* published: no lock (enc_dict_ready) */
        return NULL;
    }
    ray_t* b = ray_enc_once_get(&e->fsst_once, &e->fsst, enc_fsst_load, e);
    if (RAY_IS_ERR(b)) return b;
    *out = (const ray_fsst_table_t*)ray_data(b);
    return NULL;
}

static ray_t* dec_ints(ray_enccol_t* e, const ray_enc_desc_t* d, int vlen,
                       const uint8_t* p, uint64_t avail, const uint8_t* bm, void* out) {
    ray_enc_kind_t k;
    (void)ray_enc_kind_of(e->type, e->attrs, &k);
    ray_enc_dict_t dict = {0};
    if (d->encoding == RAY_ENC_DICT) { ray_t* err = enc_dict(e, &dict); if (err) return err; }
    uint64_t bits[RAY_ENC_VEC];
    (void)ray_enc_read(p, (size_t)avail, d->encoding, d->width, vlen, k,
                       RAY_ENC_SLOT_TOP, 0, d->encoding == RAY_ENC_DICT ? &dict : NULL, bits);
    const uint8_t esz = ray_enccol_esz(e);
    const bool sym = e->type == RAY_SYM;
    for (int i = 0; i < vlen; i++) {
        const bool null = bm && ((bm[i >> 3] >> (i & 7)) & 1);
        const uint64_t x = bits[i];
        switch (esz) {
        case 1: ((uint8_t*)out)[i] = (uint8_t)x; break;
        case 2: if (sym) ((uint16_t*)out)[i] = (uint16_t)x; else ((int16_t*)out)[i] = null ? NULL_I16 : (int16_t)x; break;
        case 4: if (sym) ((uint32_t*)out)[i] = (uint32_t)x; else ((int32_t*)out)[i] = null ? NULL_I32 : (int32_t)x; break;
        default: if (sym) ((uint64_t*)out)[i] = x; else ((int64_t*)out)[i] = null ? NULL_I64 : (int64_t)x; break;
        }
    }
    return NULL;
}

/* The float trailer after a codec body that ends at t (spec "Floats"):
 * its raw NaN bits written at their rows (after the canonical nulls). */
static void enc_float_trailer(ray_enccol_t* e, const ray_enc_desc_t* d, const uint8_t* t, void* out) {
    if (!(d->flags & RAY_ENC_DESC_TRAILER)) return;
    const size_t esz = e->type == RAY_F32 ? 4 : 8;
    uint16_t cnt; memcpy(&cnt, t, 2);
    const uint8_t* raw = t + 8 + ray_enc_pad8(2ull * cnt);
    for (int i = 0; i < cnt; i++) {
        uint16_t pos; memcpy(&pos, t + 8 + 2 * i, 2);
        memcpy((uint8_t*)out + pos * esz, raw + i * esz, esz);
    }
}

/* Every float encoding (RAW, CONST, RLE, DICT, ALP, ALP_RD): the codec
 * body, then the canonical nulls, then the trailer's raw bits (spec
 * "Floats"). */
static ray_t* dec_floats(ray_enccol_t* e, const ray_enc_desc_t* d, int vlen,
                         const uint8_t* p, uint64_t avail, const uint8_t* bm, void* out) {
    const bool f32 = e->type == RAY_F32;
    const size_t esz = f32 ? 4 : 8;
    ray_enc_dict_t dict = {0};
    if (d->encoding == RAY_ENC_DICT) { ray_t* err = enc_dict(e, &dict); if (err) return err; }
    uint64_t bits[RAY_ENC_VEC];
    int64_t used = ray_enc_float_read(p, (size_t)avail, d->encoding, vlen, f32 ? 32 : 64,
                                      d->encoding == RAY_ENC_DICT ? &dict : NULL, bits);
    for (int i = 0; i < vlen; i++) {
        uint8_t* q = (uint8_t*)out + (size_t)i * esz;
        if (bm && ((bm[i >> 3] >> (i & 7)) & 1)) {
            if (f32) { float nf = NULL_F32; memcpy(q, &nf, 4); } else { double nd = NULL_F64; memcpy(q, &nd, 8); }
            continue;
        }
        memcpy(q, &bits[i], esz);
    }
    enc_float_trailer(e, d, p + used, out);
    return NULL;
}

static void dec_guid(const ray_enc_desc_t* d, int vlen, const uint8_t* p, void* out) {
    if (d->encoding == RAY_ENC_CONST) {
        for (int i = 0; i < vlen; i++) memcpy((uint8_t*)out + i * 16, p, 16);
    } else {
        memcpy(out, p, (size_t)vlen * 16);
    }
}

/* STR vectors: the RAW / CONST grammar (spec "STR RAW"), STR_DICT codes
 * into the column's dictionary and FSST slices against its table.  With
 * out == NULL it only sizes the decoded pool bytes; with out set pool_out
 * has room for pool_base + pooled bytes.  dict_pool_off: NULL in vector
 * scope, where a STR_DICT vector copies each distinct pooled code's entry
 * once, at its first row, and every row of that code shares the offset
 * (Ruling 3-1: the layout of the dictionary-first pool; both passes meet
 * the codes in the same order, so they count the same bytes); at
 * materialization the dictionary region's offset of each code, whose
 * entries add no bytes to the vector's (Task 4). */
static ray_t* dec_str(ray_enccol_t* e, const ray_enc_desc_t* d, int vlen,
                      const uint8_t* p, uint64_t avail, void* out, char* pool_out,
                      uint64_t pool_base, uint64_t* pooled_out, const uint32_t* dict_pool_off) {
    ray_str_t* o = (ray_str_t*)out;
    uint64_t pooled = 0;
    if (d->encoding == RAY_ENC_STR_DICT) {
        const ray_enc_sdict_t* sd;
        ray_t* err = ray_enccol_strdict(e, &sd);
        if (err) return err;
        uint64_t codes[RAY_ENC_VEC];
        ray_enc_strdict_read(p, (size_t)avail, vlen, codes);
        /* vector scope: the pooled codes met so far (code + 1) and their
         * offsets in the vector's pool; at most vlen <= 1024 of them */
        uint32_t vk[2 * RAY_ENC_VEC], vo[2 * RAY_ENC_VEC];
        _Static_assert(2 * RAY_ENC_VEC == 1 << 11, "the slot hash below keeps the product's top 11 bits");
        if (!dict_pool_off) memset(vk, 0, sizeof vk);
        for (int i = 0; i < vlen; i++) {
            const char* sp; uint32_t len;
            ray_enc_sdict_entry(sd, (uint32_t)codes[i], &sp, &len);
            const bool pooled_row = len > RAY_STR_INLINE_MAX;
            uint32_t slot = 0; bool first = false;
            if (pooled_row && !dict_pool_off) {
                slot = (uint32_t)(codes[i] * 0x9E3779B1u) >> (32 - 11);          /* 2048 slots: the high bits (multiples of 2048 spread) */
                while (vk[slot] && vk[slot] != (uint32_t)codes[i] + 1) slot = (slot + 1) & (2 * RAY_ENC_VEC - 1);
                if (!vk[slot]) {
                    first = true;
                    vk[slot] = (uint32_t)codes[i] + 1;
                    vo[slot] = (uint32_t)pooled;
                    pooled += len;
                }
            }
            if (!out) continue;
            memset(&o[i], 0, sizeof o[i]);
            o[i].len = len;
            if (!pooled_row) { memcpy(o[i].data, sp, len); continue; }
            memcpy(o[i].prefix, sp, 4);
            if (dict_pool_off) { o[i].pool_off = dict_pool_off[codes[i]]; continue; }
            const uint64_t off = pool_base + vo[slot];
            if (first) memcpy(pool_out + off, sp, len);
            o[i].pool_off = (uint32_t)off;
        }
    } else if (d->encoding == RAY_ENC_FSST) {
        const ray_fsst_table_t* t;
        ray_t* err = enc_fsst(e, &t);
        if (err) return err;
        uint64_t clen[RAY_ENC_VEC], xlen[RAY_ENC_VEC]; const uint8_t* cb;
        ray_enc_fsst_parse(p, (size_t)avail, vlen, t, clen, xlen, &cb);
        for (int i = 0; i < vlen; i++) if (xlen[i] > RAY_STR_INLINE_MAX) pooled += xlen[i];
        uint64_t at = 0, run = 0;
        for (int i = 0; out && i < vlen; i++) {
            memset(&o[i], 0, sizeof o[i]);
            o[i].len = (uint32_t)xlen[i];
            if (xlen[i] <= RAY_STR_INLINE_MAX) {
                (void)ray_fsst_decode(t, cb + at, (uint32_t)clen[i], (uint8_t*)o[i].data);
            } else {
                uint8_t* dst = (uint8_t*)pool_out + pool_base + run;
                (void)ray_fsst_decode(t, cb + at, (uint32_t)clen[i], dst);
                memcpy(o[i].prefix, dst, 4);
                o[i].pool_off = (uint32_t)(pool_base + run);
                run += xlen[i];
            }
            at += clen[i];
        }
    } else if (d->encoding == RAY_ENC_CONST) {
        uint32_t len; memcpy(&len, p, 4);
        if (len > RAY_STR_INLINE_MAX) pooled = (uint64_t)len * (uint64_t)vlen;
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
        const uint64_t dbytes = (uint64_t)vlen * 16;
        memcpy(&pooled, p + dbytes, 8);
        if (out) {
            const uint8_t* vp = p + dbytes + 8;
            uint64_t run = 0;
            for (int i = 0; i < vlen; i++) {
                const uint8_t* w = p + (size_t)i * 16;
                uint32_t len; memcpy(&len, w, 4);
                if (len <= RAY_STR_INLINE_MAX) { memcpy(&o[i], w, 16); continue; }
                memcpy(pool_out + pool_base + run, vp + run, len);
                memset(&o[i], 0, sizeof o[i]);
                o[i].len = len; memcpy(o[i].prefix, w + 4, 4);
                o[i].pool_off = (uint32_t)(pool_base + run);
                run += len;
            }
        }
    }
    if (pooled_out) *pooled_out = pooled;
    return NULL;
}

/* Split a payload into bitmap and body; shared by decode and pool sizing. */
static void payload_parts(const ray_enccol_t* e, int64_t c, const ray_enc_desc_t* d,
                          const uint8_t** body, uint64_t* avail, const uint8_t** bm) {
    ray_enc_dirent_t de; dirent_at(e, c, &de);
    const uint8_t* p = e->base + de.offset + d->payload_off;
    uint64_t n = d->payload_bytes;
    *bm = NULL;
    if (d->n_nulls > 0 && ray_enc_has_bitmap(e->type)) { *bm = p; p += 128; n -= 128; }
    *body = p; *avail = n;
}

/* ---- Integer fast path: FOR_BP / DELTA_BP straight into the destination ----
 * One pass unpacks the interleaved lanes at the lane width T, adds the base
 * (DELTA_BP: the lane's previous row and the delta base) and stores the
 * element, at the generation-0 width or widened for an expression register;
 * with a bitmap, one more pass writes the null into its rows.  No u64
 * intermediate, no aside copy.  It declines only on shapes it has no kernel
 * for (an alignment, a code child, a widening it does not do), and the
 * reference path decodes those. */

/* Lane l's field of the current row: w bits at bit offset off of word a[l]
 * (off + w > T: continued in the next word row, a[l + L]).  Arithmetic is
 * in PT (unsigned, or uint64_t for 64-bit lanes) with shift counts masked
 * below T, so the compiler keeps T-bit lanes (no widening to int). */
#define FK_FIELD1(UT, PT, a, l, sr, m)        ((UT)((PT)((PT)(a)[l] >> (sr)) & (PT)(m)))
#define FK_FIELD2(UT, PT, L, a, l, sr, sl, m) \
    ((UT)((PT)(((PT)(a)[l] >> (sr)) | ((PT)(a)[(l) + (L)] << (sl))) & (PT)(m)))

/* One row of lanes [0, nl): the T-bit value V stored widened from ST to OT. */
#define FK_ROW(UT, ST, OT, V) do { for (int l = 0; l < nl; l++) { const UT v_ = (V); o[l] = (OT)(ST)v_; } } while (0)

/* FOR_BP: out[i] = base + field, rows below n.  ST: the signed (or
 * unsigned) view the element widens from; OT: the stored element. */
#define FK_FOR(NAME, UT, PT, TB, ST, OT)                                                \
static void NAME(const UT* restrict in, int w, UT base, OT* restrict out, int n) {      \
    enum { L = RAY_ENC_VEC / TB };                                                      \
    const UT m = w >= TB ? (UT)~(UT)0 : (UT)(((PT)1 << w) - 1);                         \
    for (int r = 0, bit = 0; r * L < n; r++, bit += w) {                                \
        const int k = bit / TB, off = bit % TB;                                         \
        const unsigned sr = (unsigned)off & (TB - 1), sl = (unsigned)(TB - off) & (TB - 1); \
        const int nl = n - r * L < L ? n - r * L : L;                                   \
        const UT* a = in + (size_t)k * L;                                               \
        OT* o = out + (size_t)r * L;                                                    \
        if (w == 0) FK_ROW(UT, ST, OT, base);                                           \
        else if (off + w > TB) FK_ROW(UT, ST, OT, (UT)((PT)base + FK_FIELD2(UT, PT, L, a, l, sr, sl, m))); \
        else FK_ROW(UT, ST, OT, (UT)((PT)base + FK_FIELD1(UT, PT, a, l, sr, m)));       \
    }                                                                                   \
}
/* DELTA_BP: row 0 is the heads, row r the row above plus dbase plus field r. */
#define FK_DELTA(NAME, UT, PT, TB, ST, OT)                                              \
static void NAME(const UT* restrict in, int w, UT dbase, const UT* restrict head,      \
                 OT* restrict out, int n) {                                             \
    enum { L = RAY_ENC_VEC / TB };                                                      \
    const UT m = w >= TB ? (UT)~(UT)0 : (UT)(((PT)1 << w) - 1);                         \
    UT prev[L];                                                                         \
    for (int l = 0; l < L; l++) prev[l] = head[l];                                      \
    { const int nl = n < L ? n : L; OT* o = out; FK_ROW(UT, ST, OT, prev[l]); }         \
    for (int r = 1, bit = w; r * L < n; r++, bit += w) {                                \
        const int k = bit / TB, off = bit % TB;                                         \
        const unsigned sr = (unsigned)off & (TB - 1), sl = (unsigned)(TB - off) & (TB - 1); \
        const int nl = n - r * L < L ? n - r * L : L;                                   \
        const UT* a = in + (size_t)k * L;                                               \
        OT* o = out + (size_t)r * L;                                                    \
        if (w == 0) FK_ROW(UT, ST, OT, prev[l] = (UT)((PT)prev[l] + dbase));            \
        else if (off + w > TB)                                                          \
            FK_ROW(UT, ST, OT, prev[l] = (UT)((PT)prev[l] + dbase + FK_FIELD2(UT, PT, L, a, l, sr, sl, m))); \
        else FK_ROW(UT, ST, OT, prev[l] = (UT)((PT)prev[l] + dbase + FK_FIELD1(UT, PT, a, l, sr, m))); \
    }                                                                                   \
}
FK_FOR(fk_for8,     uint8_t,  unsigned,  8, uint8_t,  uint8_t)
FK_FOR(fk_for16,    uint16_t, unsigned, 16, uint16_t, uint16_t)
FK_FOR(fk_for32,    uint32_t, unsigned, 32, uint32_t, uint32_t)
FK_FOR(fk_for64,    uint64_t, uint64_t, 64, uint64_t, uint64_t)
FK_FOR(fk_for8_i,   uint8_t,  unsigned,  8, uint8_t,  int64_t)
FK_FOR(fk_for16_i,  uint16_t, unsigned, 16, int16_t,  int64_t)
FK_FOR(fk_for32_i,  uint32_t, unsigned, 32, int32_t,  int64_t)
FK_FOR(fk_for8_f,   uint8_t,  unsigned,  8, uint8_t,  double)
FK_FOR(fk_for16_f,  uint16_t, unsigned, 16, int16_t,  double)
FK_FOR(fk_for32_f,  uint32_t, unsigned, 32, int32_t,  double)
FK_DELTA(fk_dlt16,   uint16_t, unsigned, 16, uint16_t, uint16_t)
FK_DELTA(fk_dlt32,   uint32_t, unsigned, 32, uint32_t, uint32_t)
FK_DELTA(fk_dlt64,   uint64_t, uint64_t, 64, uint64_t, uint64_t)
FK_DELTA(fk_dlt16_i, uint16_t, unsigned, 16, int16_t,  int64_t)
FK_DELTA(fk_dlt32_i, uint32_t, unsigned, 32, int32_t,  int64_t)
FK_DELTA(fk_dlt16_f, uint16_t, unsigned, 16, int16_t,  double)
FK_DELTA(fk_dlt32_f, uint32_t, unsigned, 32, int32_t,  double)

/* The bitmap's rows become the null z.  Out of line, so the loop
 * vectorises. */
#define FK_MERGE(NAME, OT, MT)                                                          \
static __attribute__((noinline)) void NAME(OT* restrict o, int n, const uint8_t* restrict bm, OT z) { \
    for (int i = 0; i < n; i++) {                                                       \
        const MT nul = (MT)-(MT)((bm[i >> 3] >> (i & 7)) & 1u);                         \
        o[i] = nul ? z : o[i];                                                          \
    }                                                                                   \
}
FK_MERGE(fk_merge16, int16_t, uint16_t)
FK_MERGE(fk_merge32, int32_t, uint32_t)
FK_MERGE(fk_merge64, int64_t, uint64_t)
FK_MERGE(fk_mergef,  double,  uint64_t)

static bool fk_int_type(int8_t t) {
    return t != RAY_F32 && t != RAY_F64 && t != RAY_GUID && t != RAY_STR;
}

/* The nulls of a signed kind at the generation-0 width: the bitmap's rows
 * become the type's null. */
static void fk_nulls_native(const ray_enc_desc_t* d, int T, int vlen, const uint8_t* bm, void* out) {
    if (!d->n_nulls || !bm) return;
    switch (T) {
    case 16: fk_merge16((int16_t*)out, vlen, bm, NULL_I16); break;
    case 32: fk_merge32((int32_t*)out, vlen, bm, NULL_I32); break;
    case 64: fk_merge64((int64_t*)out, vlen, bm, NULL_I64); break;
    default: break;                                              /* BOOL, U8: no nulls */
    }
}

/* The codes of a DICT body over a U32 FOR_BP code child, unpacked by the
 * lane kernel.  false (declined) on another code child or when the
 * dictionary is not yet published (the reference path loads it). */
static bool fk_dict_codes(ray_enccol_t* e, int vlen, const uint8_t* body, uint64_t avail,
                          ray_enc_dict_t* dict, uint32_t* codes) {
    if (avail < 16 || body[0] != RAY_ENC_FOR_BP) return false;
    if (!enc_dict_ready(e, dict)) return false;
    uint64_t base; memcpy(&base, body + 8, 8);
    fk_for32((const uint32_t*)(body + 16), body[1], (uint32_t)base, codes, vlen);
    return true;
}

/* DICT over a U32 FOR_BP code child, at the generation-0 width: the codes
 * unpacked by the lane kernel, gathered from the dictionary, then the
 * nulls as the FOR_BP path writes them. */
static int dec_fast_dict(ray_enccol_t* e, const ray_enc_desc_t* d, int vlen, const uint8_t* body,
                         uint64_t avail, const uint8_t* bm, ray_enc_kind_t k, void* out) {
    if (!k.is_signed) return 0;
    ray_enc_dict_t dict;
    _Alignas(64) uint32_t codes[RAY_ENC_VEC];
    if (!fk_dict_codes(e, vlen, body, avail, &dict, codes)) return 0;
    const uint64_t* dv = dict.v;
#define FK_GATHER(UT) do { UT* o = out; for (int i = 0; i < vlen; i++) o[i] = (UT)dv[codes[i]]; } while (0)
    switch (k.T) {
    case 16: FK_GATHER(uint16_t); break;
    case 32: FK_GATHER(uint32_t); break;
    case 64: FK_GATHER(uint64_t); break;
    default: return 0;
    }
#undef FK_GATHER
    fk_nulls_native(d, k.T, vlen, bm, out);
    return 1;
}

/* ---- CONST / RLE: the runs read, then filled at the element width ----
 * A CONST vector is one run.  An RLE vector is nr runs: its values and its
 * run ends are children at depth 1 (RAW or FOR_BP values of the element
 * kind; RAW, FOR_BP or DELTA_BP U16 ends), each unpacked for its nr values
 * only, by the lane kernels above.  Then each run is filled, at the
 * generation-0 width or widened as the FOR_BP kernels widen, and the
 * bitmap's rows set to the null. */

/* nr T-bit values, by lane width (the view's lane buffer, enccol.h). */
typedef ray_enc_lanes_t fk_vals_t;

static uint64_t fk_val(const fk_vals_t* v, int T, uint32_t i) {
    switch (T) { case 8: return v->b[i]; case 16: return v->h[i]; case 32: return v->s[i]; default: return v->q[i]; }
}

/* An RLE child at depth 1: an 8-byte header at p, then nr values of T
 * bits into out.  The bytes used, or -1 for an encoding it has no kernel
 * for. */
static int64_t fk_child(const uint8_t* p, uint32_t nr, int T, fk_vals_t* out) {
    const uint8_t enc = p[0], w = p[1];
    uint32_t bb;  memcpy(&bb, p + 4, 4);
    const uint8_t* b = p + 8;
    if (enc == RAY_ENC_RAW) {
        memcpy(out, b, (size_t)nr * (size_t)T / 8);
    } else if (enc == RAY_ENC_FOR_BP) {
        uint64_t base; memcpy(&base, b, 8);
        switch (T) {
        case 8:  fk_for8((const uint8_t*)(b + 8), w, (uint8_t)base, out->b, (int)nr); break;
        case 16: fk_for16((const uint16_t*)(b + 8), w, (uint16_t)base, out->h, (int)nr); break;
        case 32: fk_for32((const uint32_t*)(b + 8), w, (uint32_t)base, out->s, (int)nr); break;
        default: fk_for64((const uint64_t*)(b + 8), w, base, out->q, (int)nr); break;
        }
    } else if (enc == RAY_ENC_DELTA_BP && T == 16) {
        enum { L = RAY_ENC_VEC / 16 };
        uint64_t dbase; memcpy(&dbase, b, 8);
        uint16_t head[L];
        for (int l = 0; l < L; l++) { uint64_t x; memcpy(&x, b + 8 + 8 * (size_t)l, 8); head[l] = (uint16_t)x; }
        fk_dlt16((const uint16_t*)(b + 8 + 8 * (size_t)L), w, (uint16_t)dbase, head, out->h, (int)nr);
    } else {
        return -1;
    }
    return 8 + (int64_t)bb;
}

/* The runs of a CONST or RLE body of kind T: *nr values, run i ending at
 * ends[i].  1, or 0 declined. */
static int fk_runs(uint8_t enc, int vlen, const uint8_t* body, int T,
                   fk_vals_t* vals, fk_vals_t* ends, uint32_t* nr_out) {
    uint32_t nr;
    if (enc == RAY_ENC_CONST) {
        memcpy(vals, body, (size_t)T / 8);
        ends->h[0] = (uint16_t)vlen;
        nr = 1;
    } else {
        memcpy(&nr, body, 4);
        int64_t a = fk_child(body + 8, nr, T, vals);
        if (a < 0 || fk_child(body + 8 + a, nr, 16, ends) < 0) return 0;
    }
    *nr_out = nr;
    return 1;
}

/* A CONST / RLE vector of e's type into out (dec_fast's contract). */
static int dec_fast_runs(ray_enccol_t* e, const ray_enc_desc_t* d, int vlen, const uint8_t* body,
                         const uint8_t* bm, int8_t wide, ray_enc_kind_t k, void* out) {
    const int T = k.T;
    const bool sym = e->type == RAY_SYM;
    _Alignas(64) fk_vals_t vals, ends;
    uint32_t nr;
    if (!fk_runs(d->encoding, vlen, body, T, &vals, &ends, &nr)) return 0;
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
    if (sym || !k.is_signed || !d->n_nulls || !bm) return 1;
    if (!wide) fk_nulls_native(d, T, vlen, bm, out);
    else if (wide == RAY_F64) fk_mergef((double*)out, vlen, bm, NULL_F64);
    else fk_merge64((int64_t*)out, vlen, bm, NULL_I64);
    return 1;
}

/* A RAW vector of e's type at the generation-0 width into out: the values
 * copied, the bitmap merged. */
static int dec_fast_raw(ray_enccol_t* e, const ray_enc_desc_t* d, int vlen, const uint8_t* body,
                        const uint8_t* bm, ray_enc_kind_t k, void* out) {
    memcpy(out, body, (size_t)vlen * k.T / 8);
    if (e->type != RAY_SYM && k.is_signed) fk_nulls_native(d, k.T, vlen, bm, out);
    return 1;
}

/* ---- Float fast path: every float encoding straight into the destination ----
 * The values are written at the element width, then (with a bitmap) one
 * branch-free pass writes the canonical null into the bitmap's rows.  It
 * declines (0) on a trailer and on shapes it has no kernel for; the
 * reference path decodes those. */
#define FL_MERGE(NAME, FT, UT, NULLV)                                                   \
static __attribute__((noinline)) void NAME(UT* restrict o, int n, const uint8_t* restrict bm) { \
    FT nf = NULLV; UT nb; memcpy(&nb, &nf, sizeof nb);                                  \
    for (int i = 0; i < n; i++) {                                                       \
        const UT nul = (UT)-(UT)((bm[i >> 3] >> (i & 7)) & 1u);                         \
        o[i] = (UT)((o[i] & ~nul) | (nb & nul));                                        \
    }                                                                                   \
}
FL_MERGE(fl_merge64, double, uint64_t, NULL_F64)
FL_MERGE(fl_merge32, float, uint32_t, NULL_F32)
#undef FL_MERGE

static int dec_fast_float(ray_enccol_t* e, const ray_enc_desc_t* d, int vlen,
                          const uint8_t* body, uint64_t avail, const uint8_t* bm, void* out) {
    const bool f32 = e->type == RAY_F32;
    const int T = f32 ? 32 : 64;
    const uint8_t enc = d->encoding;
    if (d->flags & RAY_ENC_DESC_TRAILER) return 0;
    if (((uintptr_t)body & 7) || ((uintptr_t)out & ((size_t)T / 8 - 1))) return 0;
    switch (enc) {
    case RAY_ENC_CONST: case RAY_ENC_RLE: case RAY_ENC_DICT: {
        _Alignas(64) fk_vals_t vals, ends;
        uint32_t nr;
        if (enc == RAY_ENC_DICT) {
            if (avail >= 8 && body[0] == RAY_ENC_FOR_BP) {
                ray_enc_dict_t dict;
                _Alignas(64) uint32_t codes[RAY_ENC_VEC];
                if (!fk_dict_codes(e, vlen, body, avail, &dict, codes)) return 0;
                const uint64_t* dv = dict.v;
                if (f32) { uint32_t* o = out; for (int i = 0; i < vlen; i++) o[i] = (uint32_t)dv[codes[i]]; }
                else     { uint64_t* o = out; for (int i = 0; i < vlen; i++) o[i] = dv[codes[i]]; }
                break;
            }
            /* an RLE code child (depth 1, its children at depth 2): the
             * runs at the U32 code kind, each code an entry */
            if (avail < 16 || body[0] != RAY_ENC_RLE) return 0;
            ray_enc_dict_t dict;
            if (!enc_dict_ready(e, &dict)) return 0;
            if (!fk_runs(RAY_ENC_RLE, vlen, body + 8, 32, &vals, &ends, &nr)) return 0;
            /* the runs' values: the entries their codes name (nr <= vlen,
             * so in place, at a lane index never above the code's) */
            if (f32) for (uint32_t i = 0; i < nr; i++) vals.s[i] = (uint32_t)dict.v[vals.s[i]];
            else     for (uint32_t i = nr; i-- > 0;) vals.q[i] = dict.v[vals.s[i]];
        } else {
            if (!fk_runs(enc, vlen, body, T, &vals, &ends, &nr)) return 0;
        }
        /* Each run in fixed FL_W-element stores while they stay inside
         * the vector (one store for most runs, no per-row loop); the rows a
         * store writes past the run's end belong to the runs after it,
         * which are filled later and wholly, so every row ends up holding
         * its own run's value; nothing is written past vlen. */
#define FL_FILL(OT, V) do { OT* o_ = (OT*)out; uint32_t p_ = 0;                          \
        for (uint32_t i = 0; i < nr; i++) {                                             \
            const OT v_ = (V); const uint32_t e_ = ends.h[i];                           \
            uint32_t r_ = p_;                                                           \
            while (r_ < e_ && r_ + FL_W <= (uint32_t)vlen) {                            \
                for (uint32_t z = 0; z < FL_W; z++) o_[r_ + z] = v_;                    \
                r_ += FL_W;                                                             \
            }                                                                           \
            for (; r_ < e_; r_++) o_[r_] = v_;                                          \
            p_ = e_; } } while (0)
#define FL_W 16
        if (f32) FL_FILL(uint32_t, vals.s[i]); else FL_FILL(uint64_t, vals.q[i]);
#undef FL_W
#undef FL_FILL
        break;
    }
    case RAY_ENC_RAW:
        memcpy(out, body, (size_t)vlen * (size_t)T / 8);
        break;
    case RAY_ENC_ALP: case RAY_ENC_ALP_RD: {
        /* ALP writes vlen patterns, ALP_RD a whole vector: F64 straight
         * into out when that fits, else aside */
        if (!f32 && (enc == RAY_ENC_ALP || vlen == RAY_ENC_VEC)) {
            (void)ray_enc_float_read(body, (size_t)avail, enc, vlen, 64, NULL, (uint64_t*)out);
            break;
        }
        _Alignas(64) uint64_t bits[RAY_ENC_VEC];
        (void)ray_enc_float_read(body, (size_t)avail, enc, vlen, T, NULL, bits);
        if (f32) { uint32_t* o = out; for (int i = 0; i < vlen; i++) o[i] = (uint32_t)bits[i]; }
        else memcpy(out, bits, (size_t)vlen * 8);
        break;
    }
    default: return 0;
    }
    if (bm && d->n_nulls) { if (f32) fl_merge32((uint32_t*)out, vlen, bm); else fl_merge64((uint64_t*)out, vlen, bm); }
    return 1;
}

/* Decode a FOR_BP / DELTA_BP / CONST / RLE vector body of e's type into
 * out: `wide` 0 at the generation-0 width, RAY_I64 / RAY_F64 widened with
 * nulls as NULL_I64 / NULL_F64 (narrow non-SYM types only).  DICT over a
 * FOR_BP code child (dec_fast_dict) and RAW (dec_fast_raw) at the
 * generation-0 width; every float encoding at its width (dec_fast_float,
 * wide 0 only).  1 decoded, 0 declined (out may be partly written). */
static int dec_fast(ray_enccol_t* e, const ray_enc_desc_t* d, int vlen,
                    const uint8_t* body, uint64_t avail, const uint8_t* bm, int8_t wide, void* out) {
    const uint8_t enc = d->encoding;
    ray_enc_kind_t k;
    if (e->type == RAY_F32 || e->type == RAY_F64) return wide ? 0 : dec_fast_float(e, d, vlen, body, avail, bm, out);
    if (!fk_int_type(e->type) || !ray_enc_kind_of(e->type, e->attrs, &k)) return 0;
    if (enc == RAY_ENC_CONST || enc == RAY_ENC_RLE || enc == RAY_ENC_RAW) {
        if ((uintptr_t)body & 7) return 0;
        if (wide && (e->type == RAY_SYM || k.T == 64 || enc == RAY_ENC_RAW)) return 0;
        if ((uintptr_t)out & ((wide ? 8 : (size_t)k.T / 8) - 1)) return 0;
        return enc == RAY_ENC_RAW ? dec_fast_raw(e, d, vlen, body, bm, k, out)
                                  : dec_fast_runs(e, d, vlen, body, bm, wide, k, out);
    }
    if (enc != RAY_ENC_FOR_BP && enc != RAY_ENC_DELTA_BP && enc != RAY_ENC_DICT) return 0;
    if (enc == RAY_ENC_DICT) {
        if (wide || e->type == RAY_SYM) return 0;
        if (((uintptr_t)body & 7) || ((uintptr_t)out & ((size_t)k.T / 8 - 1))) return 0;
        return dec_fast_dict(e, d, vlen, body, avail, bm, k, out);
    }
    const int T = k.T, w = d->width, L = RAY_ENC_VEC / T;
    const bool sym = e->type == RAY_SYM, delta = enc == RAY_ENC_DELTA_BP;
    if (wide && (sym || T == 64)) return 0;
    if (delta && !k.is_signed) return 0;
    const size_t osz = wide ? 8 : (size_t)T / 8;
    if (((uintptr_t)body & 7) || ((uintptr_t)out & (osz - 1))) return 0;
    uint64_t base; memcpy(&base, body, 8);
    const uint8_t* pk = body + 8 + (delta ? 8 * (size_t)L : 0);
    union { uint8_t b[128]; uint16_t h[64]; uint32_t s[32]; uint64_t q[16]; } head;
    if (delta) {
        for (int l = 0; l < L; l++) {
            uint64_t x; memcpy(&x, body + 8 + 8 * (size_t)l, 8);
            switch (T) { case 16: head.h[l] = (uint16_t)x; break; case 32: head.s[l] = (uint32_t)x; break; default: head.q[l] = x; break; }
        }
    }
    const int8_t wt = wide;
#define FK_RUN(FOR, DLT, UT, OT) do {                                                    \
        if (delta) DLT((const UT*)pk, w, (UT)base, (const UT*)head.b, (OT*)out, vlen);   \
        else       FOR((const UT*)pk, w, (UT)base, (OT*)out, vlen); } while (0)
#define FK_RUN_FOR(FOR, UT, OT) FOR((const UT*)pk, w, (UT)base, (OT*)out, vlen)
    switch (T * 4 + (wt == RAY_I64 ? 1 : wt == RAY_F64 ? 2 : 0)) {
    case 8 * 4:      FK_RUN_FOR(fk_for8, uint8_t, uint8_t); break;
    case 8 * 4 + 1:  FK_RUN_FOR(fk_for8_i, uint8_t, int64_t); break;
    case 8 * 4 + 2:  FK_RUN_FOR(fk_for8_f, uint8_t, double); break;
    case 16 * 4:     FK_RUN(fk_for16, fk_dlt16, uint16_t, uint16_t); break;
    case 16 * 4 + 1: FK_RUN(fk_for16_i, fk_dlt16_i, uint16_t, int64_t); break;
    case 16 * 4 + 2: FK_RUN(fk_for16_f, fk_dlt16_f, uint16_t, double); break;
    case 32 * 4:     FK_RUN(fk_for32, fk_dlt32, uint32_t, uint32_t); break;
    case 32 * 4 + 1: FK_RUN(fk_for32_i, fk_dlt32_i, uint32_t, int64_t); break;
    case 32 * 4 + 2: FK_RUN(fk_for32_f, fk_dlt32_f, uint32_t, double); break;
    case 64 * 4:     FK_RUN(fk_for64, fk_dlt64, uint64_t, uint64_t); break;
    default: return 0;
    }
#undef FK_RUN
#undef FK_RUN_FOR
    if (sym || !k.is_signed || !d->n_nulls || !bm) return 1;    /* SYM nulls are in band; BOOL, U8 have none */
    if (!wt) fk_nulls_native(d, T, vlen, bm, out);
    else if (wt == RAY_F64) fk_mergef((double*)out, vlen, bm, NULL_F64);
    else fk_merge64((int64_t*)out, vlen, bm, NULL_I64);
    return 1;
}

/* The reference decoders by type, into out (vlen elements). */
static ray_t* dec_ref(ray_enccol_t* e, const ray_enc_desc_t* d, int vlen,
                      const uint8_t* body, uint64_t avail, const uint8_t* bm, void* out) {
    switch (e->type) {
    case RAY_F32: case RAY_F64: return dec_floats(e, d, vlen, body, avail, bm, out);
    case RAY_GUID:              dec_guid(d, vlen, body, out); return NULL;
    default:                    return dec_ints(e, d, vlen, body, avail, bm, out);
    }
}

/* Vector j of chunk c read in place: the mapped payload of a RAW vector
 * with no nulls and no trailer, whose bits equal its decode. */
const void* ray_enccol_raw_view(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d) {
    (void)j;
    if (e->kind != RAY_ENCCOL_SINGLE || e->type == RAY_GUID || e->type == RAY_STR ||
        d->encoding != RAY_ENC_RAW || d->n_nulls != 0 || (d->flags & RAY_ENC_DESC_TRAILER))
        return NULL;
    ray_enc_dirent_t de; dirent_at(e, c, &de);
    if (e->touch_ok && !atomic_load_explicit(&e->touch_ok[c], memory_order_acquire)) {
        /* A chunk's RAW payloads are read through once, in order, when its
         * first vector is viewed.  A grouping that re-scans its input in
         * waves then finds those pages still cached where memory is short:
         * without this pass a 2 GB box read the near-unique two-key count
         * (WatchID, ClientIP) half as much again from disk on every run. */
        const ray_enc_desc_t* descs = ray_enccol_descs(e, c);
        int nv = (int)((chunk_rows(e, c) + RAY_ENC_VEC - 1) / RAY_ENC_VEC);
        uint64_t acc = 0;
        for (int v = 0; v < nv; v++) {
            if (descs[v].encoding != RAY_ENC_RAW) continue;
            const uint64_t* w = (const uint64_t*)(e->base + de.offset + descs[v].payload_off);
            for (uint64_t i = 0; i < descs[v].payload_bytes / 8; i++) acc ^= w[i];
        }
        static _Atomic(uint64_t) sink;   /* keeps the reads; workers fold in at once */
        atomic_fetch_xor_explicit(&sink, acc, memory_order_relaxed);
        atomic_store_explicit(&e->touch_ok[c], 1, memory_order_release);
    }
    return e->base + de.offset + d->payload_off;      /* no bitmap: n_nulls == 0 */
}

/* The shared vector decode: straight into `out`, through the fast kernels
 * or the reference decoders.  An error (oom of a side entry, the FP
 * environment) leaves `out` unwritten. */
static ray_t* decode_vec(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d,
                         void* out, char* pool_out, uint64_t pool_base,
                         uint64_t* produced, const uint32_t* dict_pool_off) {
    *produced = 0;
    if ((e->type == RAY_F32 || e->type == RAY_F64) &&
        d->encoding == RAY_ENC_ALP && !ray_enc_fp_env_ok())
        return ray_error("fpenv", "column %s: " RAY_ENC_FPENV_MSG, e->path);
    int64_t rows = chunk_rows(e, c);
    int vlen = (int)(rows - (int64_t)j * RAY_ENC_VEC < RAY_ENC_VEC ? rows - (int64_t)j * RAY_ENC_VEC : RAY_ENC_VEC);
    const uint8_t* body = NULL; uint64_t avail = 0; const uint8_t* bm = NULL;
    payload_parts(e, c, d, &body, &avail, &bm);
    uint64_t pooled = 0;
    const size_t nbytes = (size_t)vlen * ray_enccol_esz(e);
    ray_t* err = NULL;
    if (e->type == RAY_STR)
        err = dec_str(e, d, vlen, body, avail, out, pool_out, pool_base, &pooled, dict_pool_off);
    else if (!dec_fast(e, d, vlen, body, avail, bm, 0, out))
        err = dec_ref(e, d, vlen, body, avail, bm, out);
    if (!err) *produced = (uint64_t)nbytes + pooled;
    return err;
}

void ray_enccol_note_decoded(ray_enccol_t* e, uint64_t nb) {
    if (!nb) return;
    atomic_fetch_add_explicit(&g_enc_decoded_bytes, nb, memory_order_relaxed);
    atomic_fetch_add_explicit(&e->decoded_bytes, nb, memory_order_relaxed);   /* per column */
}

ray_t* ray_enccol_decode_into(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d,
                              void* out, char* pool_out, uint64_t pool_base, uint64_t* produced) {
    return decode_vec(e, c, j, d, out, pool_out, pool_base, produced, NULL);
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
    payload_parts(e, c, d, &body, &avail, &bm);
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

/* Row i of a CONST / RLE body: its run's T-bit value into *x, the run
 * found by binary search over the ends.  Out of line: its 16 KB of run
 * buffers stay off the FOR_BP / DICT point read's frame. */
static __attribute__((noinline)) bool point_run(uint8_t enc, const uint8_t* p, int vlen, int i,
                                                int T, uint64_t* x) {
    if ((uintptr_t)p & 7) return false;
    _Alignas(64) fk_vals_t vals, ends;
    uint32_t nr;
    if (!fk_runs(enc, vlen, p, T, &vals, &ends, &nr)) return false;
    uint32_t lo = 0, hi = nr - 1;                          /* the first run whose end is above i */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (ends.h[mid] > (uint32_t)i) hi = mid; else lo = mid + 1;
    }
    *x = fk_val(&vals, T, lo);
    return true;
}

/* Row i of vector body p (descriptor d, vlen rows) as its T-bit pattern:
 * 1 served; 0 declined when the vector has nulls (the bitmap decides) or
 * the codec is not FOR_BP, DICT over a FOR_BP code child, CONST or RLE, so
 * the vector decode decides; -1 the dictionary's error (oom). */
static int point_body(ray_enccol_t* e, const ray_enc_desc_t* d, const uint8_t* p, uint64_t avail,
                      int vlen, int i, ray_enc_kind_t k, uint64_t* x, ray_t** err) {
    if (d->n_nulls) return 0;
    const uint64_t m = k.T == 64 ? ~0ull : (1ull << k.T) - 1;
    uint64_t v;
    if (d->encoding == RAY_ENC_FOR_BP) {
        uint64_t base; memcpy(&base, p, 8);
        v = (base + point_field(p + 8, k.T, d->width, i)) & m;
    } else if (d->encoding == RAY_ENC_DICT && d->width == 0) {
        /* the codes child: an 8-byte header (encoding, width, reserved,
         * body bytes), then a U32 FOR_BP body filling the payload */
        if (avail < 16 || p[0] != RAY_ENC_FOR_BP) return 0;
        uint64_t base; memcpy(&base, p + 8, 8);
        uint64_t code = (base + point_field(p + 16, 32, p[1], i)) & 0xffffffffull;
        ray_enc_dict_t dict;
        if ((*err = enc_dict(e, &dict)) != NULL) return -1;
        v = dict.v[code] & m;
    } else if ((d->encoding == RAY_ENC_CONST || d->encoding == RAY_ENC_RLE) && d->width == 0) {
        if (!point_run(d->encoding, p, vlen, i, k.T, &v)) return 0;
        v &= m;
    } else {
        return 0;
    }
    *x = v;
    return 1;
}

static bool point_type(const ray_enccol_t* e, ray_enc_kind_t* k) {
    return fk_int_type(e->type) && e->type != RAY_SYM && ray_enc_kind_of(e->type, e->attrs, k);
}

/* Row i of vector j of chunk c (descriptor d, body p of avail bytes) as
 * its logical value: 1 served, 0 declined, -1 with *err. */
static int point_read(ray_enccol_t* e, const ray_enc_desc_t* d, const uint8_t* p, uint64_t avail,
                      int vlen, int i, ray_enc_kind_t k, int64_t* out, ray_t** err) {
    uint64_t x;
    int r = point_body(e, d, p, avail, vlen, i, k, &x, err);
    if (r != 1) return r;
    *out = ray_enc_logical(x, k);
    return 1;
}

int ray_enccol_point_i64(ray_enccol_t* e, int64_t row, int64_t* out, ray_t** err) {
    *err = NULL;
    ray_enc_kind_t k;
    if (e->kind != RAY_ENCCOL_SINGLE || row < 0 || row >= e->len || !point_type(e, &k)) return 0;
    const int64_t c = row / RAY_ENC_CHUNK;
    const int j = (int)(row % RAY_ENC_CHUNK / RAY_ENC_VEC), i = (int)(row % RAY_ENC_VEC);
    const ray_enc_desc_t* d = &ray_enccol_descs(e, c)[j];
    ray_enc_dirent_t de; dirent_at(e, c, &de);
    const int64_t rows = chunk_rows(e, c);
    const int vlen = (int)(rows - (int64_t)j * RAY_ENC_VEC < RAY_ENC_VEC ? rows - (int64_t)j * RAY_ENC_VEC : RAY_ENC_VEC);
    return point_read(e, d, e->base + de.offset + d->payload_off, d->payload_bytes, vlen, i, k, out, err);
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
 *   with a bitmap   decode 1.8-2.4 us  N=64 0.17-0.25 (every codec)     -> RAY_ENC_VEC / 16
 * SYM vectors took the same limits (FOR_BP N=32 0.52-0.55, RLE N=64
 * 1.01-1.87, CONST N=8 0.79, N=64 3.2).  RAY_ENC_VEC / 16 is RAY_COL_POINT_ROWS,
 * the most rows a caller hands one call.  Structural ratios, not knobs. */
#define PT_ROWS_FOR   (RAY_ENC_VEC / 32)
#define PT_ROWS_RUNS  (RAY_ENC_VEC / 16)
#define PT_ROWS_CONST (RAY_ENC_VEC / 16)
#define PT_ROWS_DICT  (RAY_ENC_VEC / 16)
#define PT_ROWS_NULLS (RAY_ENC_VEC / 16)
_Static_assert(PT_ROWS_RUNS <= RAY_COL_POINT_ROWS && PT_ROWS_DICT <= RAY_COL_POINT_ROWS &&
               PT_ROWS_NULLS <= RAY_COL_POINT_ROWS && PT_ROWS_FOR <= RAY_COL_POINT_ROWS &&
               PT_ROWS_CONST <= RAY_COL_POINT_ROWS,
               "a codec's point-read limit is at most the caller's batch");

/* One vector's point-read context: its kind, the bitmap (NULL: the vector
 * has none) and whether it is SYM. */
typedef struct {
    const ray_enccol_t*   e;
    const uint8_t*        bm;      /* the null bitmap, or NULL */
    uint8_t*              nul;     /* the caller's null flags (NULL: none taken) */
    ray_enc_kind_t        k;
    uint64_t              m;       /* the lane mask */
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

/* The T-bit pattern x of a row outside the bitmap as its logical value
 * (SYM ids as themselves, id 0 the in-band null). */
static inline __attribute__((always_inline)) void pt_val(const pt_ctx_t* t, uint64_t x, int64_t q, int64_t* out) {
    const int sh = 64 - t->k.T;
    out[q] = !t->sym && t->k.is_signed ? (int64_t)(x << sh) >> sh : (int64_t)x;
}

/* Rows of one CONST / RLE vector (local rows rows[q] - base, ascending)
 * read off its runs: the runs read once (fk_runs), each row's run found
 * by advancing over the ends.  Out of line, as point_run.  1 / 0. */
static __attribute__((noinline)) int points_runs(const pt_ctx_t* t, uint8_t enc, const uint8_t* p, int vlen,
                                                 const int64_t* rows, int64_t n, int64_t base, int64_t* out) {
    if ((uintptr_t)p & 7) return 0;
    _Alignas(64) fk_vals_t vals, ends;
    uint32_t nr;
    if (!fk_runs(enc, vlen, p, t->k.T, &vals, &ends, &nr)) return 0;
    uint32_t r = 0;
    for (int64_t q = 0; q < n; q++) {
        const int64_t i = rows[q] - base;
        if (pt_null(t, (int)i, q, out)) continue;
        while (r + 1 < nr && ends.h[r] <= (uint32_t)i) r++;
        pt_val(t, fk_val(&vals, t->k.T, r) & t->m, q, out);
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

/* Rows of one FOR_BP vector: the lane arithmetic per row. */
static void points_for(const pt_ctx_t* t, int w, const uint8_t* p, const int64_t* rows, int64_t n,
                       int64_t base, int64_t* out) {
    const int T = t->k.T;
    uint64_t fb; memcpy(&fb, p, 8);
    const uint8_t* pk = p + 8;
    const uint64_t wm = w >= 64 ? ~0ull : (1ull << w) - 1;
#define FOR_POINTS(UT, FIELD) do {                                                               \
        const UT* lanes = (const UT*)(const void*)pk;                                            \
        for (int64_t q = 0; q < n; q++) {                                                        \
            const int i = (int)(rows[q] - base);                                                 \
            if (pt_null(t, i, q, out)) continue;                                                 \
            const uint64_t f = w ? FIELD(lanes, w, wm, i) : 0;                                   \
            pt_val(t, (fb + f) & t->m, q, out);                                                  \
        }                                                                                        \
    } while (0)
    switch (T) {
    case 8:  FOR_POINTS(uint8_t,  pt_field8);  break;
    case 16: FOR_POINTS(uint16_t, pt_field16); break;
    case 32: FOR_POINTS(uint32_t, pt_field32); break;
    default: FOR_POINTS(uint64_t, pt_field64); break;
    }
#undef FOR_POINTS
}

/* Rows of one DICT vector over a U32 FOR_BP code child: the dictionary
 * loaded (once per column, its oom -1), then per row the code unpacked at
 * its lane and its entry read.  0: another code child (the decode reads
 * it). */
static int points_dict(const pt_ctx_t* t, ray_enccol_t* e, const uint8_t* p, uint64_t avail, const int64_t* rows,
                       int64_t n, int64_t base, int64_t* out, ray_t** err) {
    if (avail < 16 || p[0] != RAY_ENC_FOR_BP) return 0;
    const int w = p[1];
    uint64_t cb; memcpy(&cb, p + 8, 8);
    ray_enc_dict_t dict;
    if ((*err = enc_dict(e, &dict)) != NULL) return -1;
    const uint32_t* lanes = (const uint32_t*)(const void*)(p + 16);
    const uint64_t wm = w >= 64 ? ~0ull : (1ull << w) - 1;
    for (int64_t q = 0; q < n; q++) {
        const int i = (int)(rows[q] - base);
        if (pt_null(t, i, q, out)) continue;
        const uint64_t code = (cb + (w ? pt_field32(lanes, w, wm, i) : 0)) & 0xffffffffull;
        pt_val(t, dict.v[code] & t->m, q, out);
    }
    return 1;
}

/* Rows of one RAW vector with a bitmap: each row's T-bit element. */
static void points_raw(const pt_ctx_t* t, const uint8_t* p, const int64_t* rows, int64_t n,
                       int64_t base, int64_t* out) {
    const size_t tb = (size_t)t->k.T / 8;
    for (int64_t q = 0; q < n; q++) {
        const int i = (int)(rows[q] - base);
        if (pt_null(t, i, q, out)) continue;
        uint64_t x = 0;
        memcpy(&x, p + (size_t)i * tb, tb);                     /* little-endian: the low tb bytes */
        pt_val(t, x, q, out);
    }
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
    const ray_enc_desc_t* d = &ray_enccol_descs(e, c)[j];
    ray_enc_dirent_t de; dirent_at(e, c, &de);
    const uint8_t* p = e->base + de.offset + d->payload_off;
    uint64_t avail = d->payload_bytes;
    pt_ctx_t t = { .e = e, .bm = NULL, .nul = nul, .k = k,
                   .m = k.T == 64 ? ~0ull : (1ull << k.T) - 1, .sym = sym };
    if (d->n_nulls && !sym) {
        if (!nul || !ray_enc_has_bitmap(e->type)) return 0;
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
        /* without a bitmap the in-place view is the cheaper read */
        if (!bm || n > PT_ROWS_NULLS) return 0;
        break;
    default:
        return 0;                                          /* DELTA_BP and the rest: the decode */
    }
    if (nul) memset(nul, 0, (size_t)n);
    switch (d->encoding) {
    case RAY_ENC_CONST: case RAY_ENC_RLE: return points_runs(&t, d->encoding, p, vlen, rows, n, vb, out);
    case RAY_ENC_FOR_BP:                  points_for(&t, d->width, p, rows, n, vb, out); return 1;
    case RAY_ENC_DICT:                    return points_dict(&t, e, p, avail, rows, n, vb, out, err);
    default:                              points_raw(&t, p, rows, n, vb, out); return 1;
    }
}

int ray_enccol_points_i64(ray_enccol_t* e, const int64_t* rows, int64_t n, int64_t base, int64_t* out, ray_t** err) {
    return points_core(e, rows, n, base, out, NULL, err);
}

int ray_enccol_gather_points(ray_enccol_t* e, const int64_t* rows, int64_t n, int64_t base, int64_t* out,
                             uint8_t* nul, ray_t** err) {
    return points_core(e, rows, n, base, out, nul, err);
}

/* ---- Encoded vector views: a body parsed, never expanded ----
 * A view is built for CONST, RLE, FOR_BP and DICT (over a U32 FOR_BP code
 * child) integer bodies, CONST, RLE and DICT float bodies and STR_DICT
 * codes; on any other shape it declines and the caller's range read
 * decodes the vector.  The writer's base of a FOR_BP body is the vector
 * minimum, so its residuals compare in logical order. */

void ray_enccol_unpack_resid(const ray_enc_vview_t* v, void* out) {
    switch (v->k.T) {
    case 8:  fk_for8 ((const uint8_t*) v->packed, v->width, 0, (uint8_t*) out, v->n); break;
    case 16: fk_for16((const uint16_t*)v->packed, v->width, 0, (uint16_t*)out, v->n); break;
    case 32: fk_for32((const uint32_t*)v->packed, v->width, 0, (uint32_t*)out, v->n); break;
    default: fk_for64((const uint64_t*)v->packed, v->width, 0, (uint64_t*)out, v->n); break;
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

/* The U32 FOR_BP code child spanning the DICT body (its encoding byte
 * checked by the caller): the codes unpacked into v->codes.  Shared by the
 * integer and float DICT views. */
static void vview_codes_for(const uint8_t* body, ray_enc_vview_t* v) {
    uint64_t base; memcpy(&base, body + 8, 8);
    fk_for32((const uint32_t*)(body + 16), body[1], (uint32_t)base, v->codes, v->n);
}

/* A STR_DICT vector's view: the codes (a U32 FOR_BP child through
 * vview_codes_for, any other code child through ray_enc_strdict_read).  No
 * entry is read, no byte decoded or counted. */
static int vview_str_dict(ray_enccol_t* e, const uint8_t* body, uint64_t avail, ray_enc_vview_t* v, ray_t** err) {
    const ray_enc_sdict_t* sd;
    if ((*err = ray_enccol_strdict(e, &sd)) != NULL) return -1;   /* read once per column */
    if (avail >= 16 && body[0] == RAY_ENC_FOR_BP) {
        vview_codes_for(body, v);
    } else {
        uint64_t codes[RAY_ENC_VEC];
        ray_enc_strdict_read(body, (size_t)avail, v->n, codes);
        for (int i = 0; i < v->n; i++) v->codes[i] = (uint32_t)codes[i];
    }
    v->dict_n = sd->n;
    return 1;
}

/* Vector vec of a float column as a view: CONST, RLE (RAW values) and DICT
 * over a FOR_BP code child; anything else declines (the caller decodes). */
static int vview_float(ray_enccol_t* e, const ray_enc_desc_t* d, const uint8_t* body, uint64_t avail,
                       ray_enc_vview_t* v, ray_t** err) {
    if (d->width) return 0;
    const int T = v->k.T;
    switch (d->encoding) {
    case RAY_ENC_CONST:
        if (!fk_runs(RAY_ENC_CONST, v->n, body, T, &v->run_vals, &v->run_ends, &v->n_runs)) return 0;
        v->cbits = T == 32 ? v->run_vals.s[0] : v->run_vals.q[0];
        return 1;
    case RAY_ENC_RLE:
        if (avail < 16 || body[8] != RAY_ENC_RAW) return 0;
        return fk_runs(RAY_ENC_RLE, v->n, body, T, &v->run_vals, &v->run_ends, &v->n_runs) ? 1 : 0;
    case RAY_ENC_DICT: {
        if (avail < 16 || body[0] != RAY_ENC_FOR_BP) return 0;
        ray_enc_dict_t dict;
        if ((*err = enc_dict(e, &dict)) != NULL) return -1;  /* read once per column */
        vview_codes_for(body, v);
        v->dict = dict.v; v->dict_n = dict.n;
        return 1;
    }
    default: return 0;
    }
}

int ray_enccol_vview(ray_enccol_t* e, int64_t vec, ray_enc_vview_t* v, ray_t** err) {
    *err = NULL;
    if (vec < 0 || vec >= ray_enc_nvecs(e->len) || e->kind != RAY_ENCCOL_SINGLE || e->synthetic) return 0;
    const int64_t c = vec / RAY_ENC_VECS_PER_CHUNK;
    const int j = (int)(vec % RAY_ENC_VECS_PER_CHUNK);
    const ray_enc_desc_t* d = &ray_enccol_descs(e, c)[j];
    const int n = ray_enc_vec_len(e->len, vec);
    const bool fl = enc_type_float(e->type);
    if (d->flags & RAY_ENC_DESC_TRAILER) return 0;           /* floats only: decline (D6) */
    const bool str = e->type == RAY_STR;
    if (str) {
        if (d->encoding != RAY_ENC_STR_DICT) return 0;       /* RAW, CONST, FSST: decode */
        v->k = (ray_enc_kind_t){ 32, false, false };         /* the codes */
    } else if (fl) {
        if (d->encoding != RAY_ENC_CONST && d->encoding != RAY_ENC_RLE && d->encoding != RAY_ENC_DICT) return 0;
        if (!ray_enc_value_kind(e->type, e->attrs, &v->k)) return 0;   /* { 32 | 64, unsigned, float } */
    } else {
        if (!fk_int_type(e->type) || !ray_enc_kind_of(e->type, e->attrs, &v->k)) return 0;
        if (d->encoding != RAY_ENC_CONST && d->encoding != RAY_ENC_FOR_BP &&
            d->encoding != RAY_ENC_RLE && d->encoding != RAY_ENC_DICT) return 0;
    }
    const uint8_t* body; uint64_t avail; const uint8_t* bm;
    payload_parts(e, c, d, &body, &avail, &bm);
    if ((uintptr_t)body & 7) return 0;
    v->enc = d->encoding; v->n = n; v->n_nulls = d->n_nulls; v->nullmap = bm; v->type = e->type;
    v->cbits = 0; v->base = 0; v->width = 0; v->packed = NULL;
    v->n_runs = 0; v->dict = NULL; v->dict_n = 0;
    if (str) return vview_str_dict(e, body, avail, v, err);   /* nulls in band: no bitmap */
    if (fl) return vview_float(e, d, body, avail, v, err);
    const int T = v->k.T;
    switch (d->encoding) {
    case RAY_ENC_CONST: case RAY_ENC_RLE:
        if (d->width) return 0;
        if (!fk_runs(d->encoding, n, body, T, &v->run_vals, &v->run_ends, &v->n_runs)) return 0;
        if (d->encoding == RAY_ENC_CONST) v->cbits = fk_val(&v->run_vals, T, 0);
        return 1;
    case RAY_ENC_FOR_BP: {
        uint64_t base; memcpy(&base, body, 8);
        v->base = base; v->width = d->width; v->packed = body + 8;
        return 1;
    }
    case RAY_ENC_DICT: {
        if (d->width || !v->k.is_signed || e->type == RAY_SYM || avail < 16 || body[0] != RAY_ENC_FOR_BP) return 0;
        ray_enc_dict_t dict;
        if ((*err = enc_dict(e, &dict)) != NULL) return -1;  /* read once per column */
        vview_codes_for(body, v);
        v->dict = dict.v; v->dict_n = dict.n;
        return 1;
    }
    default:
        return 0;
    }
}

/* A stand-in column of `type` for the body hooks; with a VALUE_DICT of
 * dict[0, n_dict) (T-bit patterns). */
static ray_enccol_t* test_col(int8_t type, uint8_t attrs, const uint64_t* dict, uint32_t n_dict) {
    ray_enccol_t* e = ray_calloc_raw(sizeof *e);
    if (!e) return NULL;
    e->type = type; e->attrs = attrs;
    snprintf(e->path, sizeof e->path, "decode_body");
    ray_enc_once_init(&e->dict_once);
    ray_enc_kind_t k;
    if (dict && ray_enc_value_kind(type, attrs, &k)) {      /* floats: their F32 / F64 patterns */
        size_t esz = (size_t)k.T / 8, len = 8 + (size_t)ray_enc_pad8((uint64_t)n_dict * esz);
        uint8_t* b = ray_calloc_raw(len);
        if (!b) { ray_free_raw(e); return NULL; }
        memcpy(b, &n_dict, 4);
        for (uint32_t i = 0; i < n_dict; i++) memcpy(b + 8 + i * esz, &dict[i], esz);
        e->base = b;
        e->vdict_off = 0;
        e->vdict_len = len;
    }
    return e;
}
static void test_col_free(ray_enccol_t* e) {
    if (e->dict) ray_release(e->dict);
    ray_free_raw((void*)e->base);
    ray_free_raw(e);
}

/* The kernels read a published dictionary only: publish it first. */
static void test_col_dict(ray_enccol_t* e) {
    ray_enc_dict_t dd;
    ray_t* err = enc_dict(e, &dd);
    if (err) ray_error_free(err);
}

int ray_enccol_test_point_body(int8_t type, const uint64_t* dict, uint32_t n_dict, uint8_t enc,
                               uint8_t width, int n_nulls, const uint8_t* body, uint64_t avail,
                               int vlen, int64_t* out, int8_t* res) {
    ray_enccol_t* e = test_col(type, 0, dict, n_dict);
    if (!e) return -1;
    ray_enc_desc_t d; memset(&d, 0, sizeof d);
    d.encoding = enc; d.width = width; d.n_nulls = (uint16_t)n_nulls;
    d.flags = RAY_ENC_STAT_ABSENT;
    ray_enc_kind_t k;
    const bool ok = point_type(e, &k);
    for (int i = 0; i < vlen; i++) {
        ray_t* err = NULL;
        int64_t v = 0;
        res[i] = ok ? (int8_t)point_read(e, &d, body, avail, vlen, i, k, &v, &err) : 0;
        if (res[i] == 1) out[i] = v;
        if (err) ray_error_free(err);
    }
    test_col_free(e);
    return 0;
}

static int test_decode(ray_enccol_t* e, uint8_t enc, uint8_t width, int n_nulls, const uint8_t* bm,
                       const uint8_t* body, uint64_t avail, int vlen, bool fast, int8_t wide, void* out) {
    ray_enc_desc_t d; memset(&d, 0, sizeof d);
    d.encoding = enc; d.width = width; d.n_nulls = (uint16_t)n_nulls;
    d.flags = RAY_ENC_STAT_ABSENT;
    if (fast) return dec_fast(e, &d, vlen, body, avail, bm, wide, out);
    if (wide) return -1;
    ray_t* err = dec_ints(e, &d, vlen, body, avail, bm, out);
    if (err) ray_error_free(err);
    return err ? -1 : 1;
}

int ray_enccol_test_decode_body(int8_t type, uint8_t attrs, uint8_t enc,
                                uint8_t width, int n_nulls, const uint8_t* bm, const uint8_t* body,
                                uint64_t avail, int vlen, bool fast, int8_t wide, void* out) {
    ray_enccol_t* e = test_col(type, attrs, NULL, 0);
    if (!e) return -1;
    int r = test_decode(e, enc, width, n_nulls, bm, body, avail, vlen, fast, wide, out);
    test_col_free(e);
    return r;
}

int ray_enccol_test_decode_float(int8_t type, const uint64_t* dict, uint32_t n_dict, uint8_t enc, int n_nulls,
                                 const uint8_t* bm, const uint8_t* body, uint64_t avail, int vlen,
                                 bool fast, void* out) {
    if (!enc_type_float(type)) return -1;
    ray_enccol_t* e = test_col(type, 0, dict, n_dict);
    if (!e) return -1;
    if (dict && fast) test_col_dict(e);
    ray_enc_desc_t d; memset(&d, 0, sizeof d);
    d.encoding = enc; d.n_nulls = (uint16_t)n_nulls;
    d.flags = RAY_ENC_STAT_ABSENT;
    int r;
    if (fast) r = dec_fast(e, &d, vlen, body, avail, bm, 0, out);
    else {
        ray_t* err = dec_floats(e, &d, vlen, body, avail, bm, out);
        if (err) ray_error_free(err);
        r = err ? -1 : 1;
    }
    test_col_free(e);
    return r;
}

int ray_enccol_test_decode_dict(int8_t type, const uint64_t* dict, uint32_t n_dict,
                                int n_nulls, const uint8_t* bm, const uint8_t* body, uint64_t avail,
                                int vlen, bool fast, void* out) {
    ray_enccol_t* e = test_col(type, 0, dict, n_dict);
    if (!e) return -1;
    if (fast) test_col_dict(e);
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
    payload_parts(e, c, d, &body, &avail, &bm);
    return dec_str(e, d, vlen, body, avail, NULL, NULL, 0, out, NULL);   /* vector scope */
}

ray_t* ray_enccol_decode_vec(ray_enccol_t* e, int64_t vec, void* out, char* pool_out, uint64_t pool_base) {
    if (vec < 0 || vec >= ray_enc_nvecs(e->len)) return ray_error("range", "column %s: vector %lld", e->path, (long long)vec);
    int64_t c = vec / RAY_ENC_VECS_PER_CHUNK;
    int j = (int)(vec % RAY_ENC_VECS_PER_CHUNK);
    uint64_t nb;
    ray_t* err = decode_vec(e, c, j, &ray_enccol_descs(e, c)[j], out, pool_out, pool_base, &nb, NULL);
    if (!err) ray_enccol_note_decoded(e, nb);
    return err;
}

ray_t* ray_enccol_vec_pool_bytes(ray_enccol_t* e, int64_t vec, uint64_t* out) {
    *out = 0;
    if (vec < 0 || vec >= ray_enc_nvecs(e->len)) return ray_error("range", "column %s: vector %lld", e->path, (long long)vec);
    int64_t c = vec / RAY_ENC_VECS_PER_CHUNK;
    int j = (int)(vec % RAY_ENC_VECS_PER_CHUNK);
    return ray_enccol_desc_pool_bytes(e, c, j, &ray_enccol_descs(e, c)[j], out);
}

/* ---- Single-column materialization (spec "Materialized equivalence") ---- */

typedef struct { ray_enc_snap_t snap; bool failed; bool done; } enc_task_res_t;

typedef struct {
    ray_enccol_t*   e;
    uint8_t*        out;
    char*           pool;
    const uint64_t* chunk_pool;     /* STR: decoded-pool base of each chunk */
    const uint32_t* dict_pool_off;  /* STR with a STR_DICT entry: each code's offset in the
                                     * dictionary region (UINT32_MAX: inline entry) */
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
        const ray_enc_desc_t* descs = ray_enccol_descs(e, c);
        ray_t* err = NULL;
        int64_t rows = chunk_rows(e, c);
        int nv = (int)((rows + RAY_ENC_VEC - 1) / RAY_ENC_VEC);
        uint64_t pbase = x->chunk_pool ? x->chunk_pool[c] : 0;
        uint64_t produced = 0;                  /* counted once per chunk */
        for (int j = 0; !err && j < nv; j++) {
            int64_t row = c * RAY_ENC_CHUNK + (int64_t)j * RAY_ENC_VEC;
            uint8_t* dst = x->out + (size_t)row * esz;   /* private until published */
            uint64_t nb = 0;
            /* ray_enccol_decode_into with the dictionary region's offsets */
            err = decode_vec(e, c, j, &descs[j], dst, x->pool, pbase, &nb, x->dict_pool_off);
            produced += nb;
            if (!err && e->type == RAY_STR) {
                int vlen = ray_enc_vec_len(e->len, row / RAY_ENC_VEC);
                ray_str_t* s = (ray_str_t*)dst;
                /* the vector's own pool bytes: a STR_DICT vector's rows point
                 * into the dictionary region and add none (its nb is its
                 * descriptors' bytes only), so the pool advances only over
                 * RAW, CONST and FSST vectors, as the prepass counted them */
                pbase += nb - (uint64_t)vlen * esz;
                for (int i = 0; i < vlen; i++) {
                    if (ray_str_is_inline(&s[i])) continue;
                    /* the generation-0 rule (col_str_region), computed
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

/* STR column-scope pass (spec "STR pool"): the decoded pool is the
 * dictionary's entries over 12 bytes once, in code order (sd, the STR_DICT
 * entry, or NULL), then every RAW / CONST / FSST vector's pooled bytes in
 * row order; STR_DICT vectors share the dictionary region and add none.
 * chunk_pool[c] is where chunk c's pooled bytes start. */
static ray_t* enc_str_prepass(ray_enccol_t* e, uint64_t* chunk_pool, const ray_enc_sdict_t* sd) {
    uint64_t total = sd ? sd->pooled : 0;                 /* rule 1: the dictionary region first */
    for (int64_t c = 0; c < e->n_chunks; c++) {
        const ray_enc_desc_t* descs = ray_enccol_descs(e, c);
        chunk_pool[c] = total;
        int nv = (int)((chunk_rows(e, c) + RAY_ENC_VEC - 1) / RAY_ENC_VEC);
        for (int j = 0; j < nv; j++) {
            if (descs[j].encoding == RAY_ENC_STR_DICT) continue;   /* rows in the dictionary region */
            uint64_t pb;
            ray_t* err = ray_enccol_desc_pool_bytes(e, c, j, &descs[j], &pb);
            if (err) return err;
            total += pb;
        }
    }
    return NULL;
}

static _Atomic(bool) g_fail_index_parse;
void ray_enccol_test_fail_index_parse(bool on) { atomic_store(&g_fail_index_parse, on); }

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
        if ((de.flags & RAY_ENC_STAT_ABSENT) || e->type == RAY_STR) continue;   /* STR: code bounds, no order */
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
        bool absent = (de.flags & RAY_ENC_STAT_ABSENT) || e->type == RAY_STR;   /* STR: code bounds, no order */
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

/* Recorded kind or UNIQUE (single file). */
static bool enc_may_have_index(const ray_enccol_t* e) {
    return e->kind == RAY_ENCCOL_SINGLE && (e->hdr.index_kind || (e->hdr.col_flags & RAY_ENC_FLAG_UNIQUE));
}

/* ZONE / CHUNK_ZONE roots are synthesized from the directory; HASH /
 * SORT / BLOOM / PART roots are parsed from the index region on first
 * materialization.  The UNIQUE marker comes from
 * the column flag (the parse leaves it to this caller).  A recorded DICT
 * (P6b) attaches a heap root from the persisted region: codes the heap copy
 * (shared with the aware paths), first_occ a copy; nothing in the root
 * refers to the mapping.  UNIQUE marks whichever root results. */
static ray_t* enc_attach_index(ray_enccol_t* e, ray_t** vp) {
    uint8_t kind = e->hdr.index_kind;
    bool unique = e->hdr.col_flags & RAY_ENC_FLAG_UNIQUE;
    if (kind == 0 && !unique) return NULL;
    ray_t* root = NULL;
    if (kind == RAY_IDX_ZONE || kind == RAY_IDX_CHUNK_ZONE) {
        root = kind == RAY_IDX_ZONE ? enc_synth_zone(e) : enc_synth_chunk_zone(e);
        if (RAY_IS_ERR(root)) return root;
    } else if (kind == RAY_IDX_HASH || kind == RAY_IDX_SORT || kind == RAY_IDX_BLOOM || kind == RAY_IDX_PART) {
        /* the nth counter's step (fail_path already failed at "materialize") */
        if (enc_alloc_fails(e) || atomic_load(&g_fail_index_parse))
            return ray_error("oom", "column %s: index parse", e->path);
        ray_enc_idx_view_t iv;
        ray_enc_index_view(e->base + e->hdr.index_off, e->hdr.end_off - e->hdr.index_off,
                           kind, e->type, e->attrs, e->len, &iv);
        ray_t* perr = ray_enc_index_parse(&iv, *vp, &root);
        if (perr) {
            /* the parse names no path: keep its code, add the column's */
            char code[8], msg[200];
            snprintf(code, sizeof code, "%s", ray_err_code(perr) ? ray_err_code(perr) : "error");
            const char* m = ray_error_msg();
            snprintf(msg, sizeof msg, "%s", m ? m : "index parse");
            ray_error_free(perr);
            return ray_error(code, "column %s: %s", e->path, msg);
        }
    } else if (kind == RAY_IDX_DICT) {
        /* spec "Materialization" step 2 (P6b): both tiers, then a heap root */
        if (enc_alloc_fails(e) || atomic_load(&g_fail_index_parse))
            return ray_error("oom", "column %s: index parse", e->path);
        const ray_enc_idict_t* d;
        ray_t* cv = NULL;
        ray_t* err = ray_enccol_idict(e, &d);
        if (!err) err = ray_enccol_idict_codes_vec(e, &cv);
        if (err) return err;
        ray_t* fo = enc_alloc_fails(e) ? NULL : ray_vec_new(RAY_I32, d->n_distinct);
        root = fo && !RAY_IS_ERR(fo) && !enc_alloc_fails(e) ? ray_index_new_root(RAY_IDX_DICT, RAY_STR, e->len) : NULL;
        if (!root || RAY_IS_ERR(root)) {
            if (fo && !RAY_IS_ERR(fo)) ray_release(fo); else if (fo) ray_error_free(fo);
            if (root) ray_error_free(root);
            return ray_error("oom", "column %s: DICT index", e->path);
        }
        fo->len = d->n_distinct;
        memcpy(ray_data(fo), d->first_occ, (size_t)d->n_distinct * 4);
        ray_retain(cv);                           /* shared with the aware paths */
        ray_index_t* ix = ray_index_payload(root);
        ix->u.dict.codes = cv; ix->u.dict.first_occ = fo; ix->u.dict.n_distinct = d->n_distinct;
    }
    if (unique) {
        if (!root) {
            root = enc_alloc_fails(e) ? NULL : ray_index_new_root(RAY_IDX_NONE, e->type, e->len);
            if (!root || RAY_IS_ERR(root)) return root ? root : ray_error("oom", "column %s: unique marker", e->path);
        }
        ray_index_payload(root)->markers |= RAY_MARK_UNIQUE;
    }
    ray_t* r = ray_index_attach_persisted(vp, root);    /* consumes root */
    if (RAY_IS_ERR(r)) return r;
    *vp = r;
    return NULL;
}

/* Test hook: STR pools allocated by materialization (after the prepass, so a
 * column it rejects allocates none). */
static _Atomic(uint64_t) g_str_pool_allocs;
uint64_t ray_enccol_test_str_pool_allocs(void) { return atomic_load(&g_str_pool_allocs); }

static ray_t* enc_materialize_single(void* ctx, ray_enc_snap_t* fail) {
    ray_enccol_t* e = ctx;
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
    uint32_t* dict_pool_off = NULL;
    enc_task_res_t* res = NULL;
    if (e->type == RAY_STR) {
        const ray_enc_sdict_t* sd = NULL;                /* the dictionary first: the pool starts with it */
        if ((err = ray_enccol_strdict(e, &sd)) != NULL) goto fail;
        chunk_pool = enc_alloc_fails(e) ? NULL : ray_alloc_raw((size_t)(e->n_chunks + 1) * sizeof(uint64_t));
        if (!chunk_pool) { err = ray_error("oom", "column %s: materialize", e->path); goto fail; }
        if ((err = enc_str_prepass(e, chunk_pool, sd)) != NULL) goto fail;
        if (e->hdr.str_pool_bytes > 0) {
            ray_t* pool = enc_alloc_fails(e) ? NULL : ray_alloc((size_t)e->hdr.str_pool_bytes);
            if (!pool || RAY_IS_ERR(pool)) { err = pool ? pool : ray_error("oom", "column %s: pool", e->path); goto fail; }
            pool->type = RAY_U8;
            pool->len  = (int64_t)e->hdr.str_pool_bytes;
            vec->str_pool = pool;
            atomic_fetch_add_explicit(&g_str_pool_allocs, 1, memory_order_relaxed);
        }
        if (sd) {
            /* the dictionary region: entries over 12 bytes once, in code
             * order, at the pool's start (the prepass counted sd->pooled
             * of it, so a pool exists when any entry is pooled) */
            dict_pool_off = enc_alloc_fails(e) ? NULL : ray_alloc_raw((size_t)sd->n * sizeof(uint32_t));
            if (!dict_pool_off) { err = ray_error("oom", "column %s: materialize", e->path); goto fail; }
            char* pb = vec->str_pool ? (char*)ray_data(vec->str_pool) : NULL;
            uint64_t at = 0;
            for (uint32_t k = 0; k < sd->n; k++) {
                const char* sp; uint32_t len;
                ray_enc_sdict_entry(sd, k, &sp, &len);
                if (len <= RAY_STR_INLINE_MAX) { dict_pool_off[k] = UINT32_MAX; continue; }
                memcpy(pb + at, sp, len);
                dict_pool_off[k] = (uint32_t)at;         /* at + len <= sd->pooled <= str_pool_bytes <= UINT32_MAX */
                at += len;
            }
        }
    }
    res = enc_alloc_fails(e) ? NULL : ray_calloc_raw((size_t)(e->n_chunks + 1) * sizeof *res);
    if (!res) { err = ray_error("oom", "column %s: materialize", e->path); goto fail; }
    enc_mat_ctx_t x = {
        .e = e, .out = (uint8_t*)ray_data(vec),
        .pool = vec->str_pool ? (char*)ray_data(vec->str_pool) : NULL,
        .chunk_pool = chunk_pool, .dict_pool_off = dict_pool_off,
        .res = res,
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
            ray_free_raw(res); ray_free_raw(chunk_pool); ray_free_raw(dict_pool_off); ray_release(vec);
            return NULL;
        }
    ray_free_raw(res); res = NULL;
    ray_free_raw(chunk_pool); chunk_pool = NULL;
    ray_free_raw(dict_pool_off); dict_pool_off = NULL;
    vec->attrs = e->attrs;                       /* persisted attrs (SYM width included) */
    if (e->type == RAY_SYM) {
        vec->sym_domain = e->dom;                /* sym_vec_new's runtime domain is immortal */
        if (e->dom_owned) ray_sym_domain_retain(e->dom);
    }
    if ((err = enc_attach_index(e, &vec)) != NULL) goto fail;
    atomic_fetch_add_explicit(&g_enc_materialized, 1, memory_order_relaxed);
    return vec;
fail:
    ray_free_raw(res); ray_free_raw(chunk_pool); ray_free_raw(dict_pool_off);
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
    /* Reject before the once/cache lookup: caller control state is not a
     * permanent property of the file, and workers can have distinct state. */
    if ((e->type == RAY_F32 || e->type == RAY_F64) && !ray_enc_fp_env_ok())
        return ray_error("fpenv", "column %s: " RAY_ENC_FPENV_MSG, e->path);
    ray_t* r = ray_enc_once_get(&e->once, &e->decoded,
                                e->kind == RAY_ENCCOL_PARTED ? enc_materialize_parted : enc_materialize_single, e);
    if (r && !RAY_IS_ERR(r)) dec_note(e);
    return r;
}

static _Atomic(int64_t) g_kept_limit_test;   /* test hook: the budget, 0 = a quarter of the watermark */
void ray_enccol_test_kept_limit(int64_t bytes) {
    atomic_store_explicit(&g_kept_limit_test, bytes > 0 ? bytes : 0, memory_order_relaxed);
}

bool ray_enccol_decode_kept(ray_t* encv) {
    ray_enccol_t* e = ray_enccol_of(encv);
    if (!e) return true;
    int64_t cap = atomic_load_explicit(&g_kept_limit_test, memory_order_relaxed);
    if (cap <= 0) {
        int64_t wm = ray_heap_anon_watermark();
        if (wm <= 0) return true;             /* no watermark keeps everything */
        cap = wm / 4;                         /* ray_enccol_cache_trim's budget */
    }
    return (double)e->len * (double)ray_enccol_esz(e) <= (double)cap;
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
        const ray_enc_desc_t* d = ray_enccol_descs(e, c);
        int64_t rows = e->len - c * RAY_ENC_CHUNK;
        int nv = (int)(((rows > RAY_ENC_CHUNK ? RAY_ENC_CHUNK : rows) + RAY_ENC_VEC - 1) / RAY_ENC_VEC);
        for (int j = 0; j < nv; j++) if (d[j].encoding < 10) out->vectors[d[j].encoding]++;
    }
    out->encoded_bytes = out->synthetic ? 0 : e->hdr.end_off;
    /* the materialized column's bytes: for STR, str_pool_bytes is the
     * decoded pool's exact size, which holds each STR_DICT entry once (spec
     * "STR pool"), so a STR_DICT column reports less than its generation-0
     * file's per-occurrence pool (P6 D15; the header keeps no
     * per-occurrence count, and .idx.info decodes nothing) */
    out->raw_bytes = (uint64_t)e->len * ray_enccol_esz(e) + (e->type == RAY_STR ? e->hdr.str_pool_bytes : 0);
    out->materialized = ray_enccol_decoded_peek(encv) != NULL;
    return NULL;
}

bool ray_enccol_may_have_index(ray_t* encv) {
    ray_enccol_t* e = ray_enccol_of(encv);
    return e && enc_may_have_index(e);
}

ray_t* ray_enccol_decoded_peek(ray_t* encv) {
    ray_enccol_t* e = ray_enccol_of(encv);
    if (!e) return NULL;
    if (atomic_load_explicit(&e->once.state, memory_order_acquire) != RAY_ENC_ONCE_DONE) return NULL;
    return e->decoded;
}

void ray_enccol_chunk_stat(const ray_enccol_t* e, int64_t c, ray_enc_chunk_stat_t* out) {
    ray_enc_dirent_t de;
    dirent_at(e, c, &de);
    out->rows = chunk_rows(e, c);
    /* STR bounds are code bounds (first-occurrence codes, not ordered
     * values): only ray_enccol_code_bounds reads them */
    out->has_minmax = (de.flags & RAY_ENC_STAT_ABSENT) == 0 && e->type != RAY_STR;
    out->min = e->type == RAY_STR ? 0 : de.min;          /* a reader ignoring has_minmax sees no codes */
    out->max = e->type == RAY_STR ? 0 : de.max;
    out->null_count = de.null_count;
    out->sum_lo = de.sum_lo; out->sum_hi = de.sum_hi;
}

bool ray_enccol_code_bounds(const ray_enccol_t* e, int64_t c, int j, int64_t* lo, int64_t* hi) {
    if (e->type != RAY_STR || e->kind != RAY_ENCCOL_SINGLE || e->synthetic || c < 0 || c >= e->n_chunks) return false;
    if (j < 0) {
        ray_enc_dirent_t de; dirent_at(e, c, &de);
        if (de.flags & RAY_ENC_STAT_ABSENT) return false;
        *lo = de.min; *hi = de.max;
        return true;
    }
    if (j >= RAY_ENC_VECS_PER_CHUNK || (c * RAY_ENC_VECS_PER_CHUNK + j) * RAY_ENC_VEC >= e->len) return false;
    const ray_enc_desc_t* d = &ray_enccol_descs(e, c)[j];
    if (d->flags & RAY_ENC_STAT_ABSENT) return false;
    *lo = d->min; *hi = d->max;
    return true;
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
        if ((de.flags & RAY_ENC_STAT_ABSENT) || e->type == RAY_STR) {   /* STR: code bounds are no ordered statistic */
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
