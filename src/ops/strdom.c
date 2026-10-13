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

#include "ops/strdom.h"
#include "ops/rowsel.h"
#include "vec/str.h"

#include <stdint.h>
#include <string.h>

/* Open addressing over the interned strings, at most half full.  A slot
 * holds 0 (empty) or the id + 1 in its low 40 bits under a 24-bit tag from
 * the hash's top bits, so a probe compares the tag before it loads the
 * entry.  Entries {offset into the arena, len}; the full hashes are kept
 * apart, for the rehash. */
#define SD_ID_BITS 40
#define SD_ID_MASK (((uint64_t)1 << SD_ID_BITS) - 1)
typedef struct { uint32_t off, len; } sd_ent_t;
struct ray_strdom_s {
    char*     arena;
    size_t    used, cap;     /* arena bytes; cap <= UINT32_MAX */
    sd_ent_t* ent;
    uint64_t* hash;
    int64_t   n, ecap;       /* entries; n <= UINT32_MAX + 1 (one empty string) */
    uint64_t* slots;
    uint64_t  mask;          /* slots - 1 */
};

static inline uint64_t sd_tag(uint64_t h) { return h >> SD_ID_BITS; }

int64_t ray_strdom_count(const ray_strdom_t* d) { return d ? d->n : 0; }

void ray_strdom_free(ray_strdom_t* d) {
    if (!d) return;
    ray_free_raw(d->arena); ray_free_raw(d->ent); ray_free_raw(d->hash);
    ray_free_raw(d->slots);
    ray_free_raw(d);
}

static ray_t* sd_rehash(ray_strdom_t* d, uint64_t nslots) {
    uint64_t* s = (uint64_t*)ray_calloc_raw((size_t)nslots * sizeof(uint64_t));
    if (!s) return ray_error("oom", NULL);
    uint64_t m = nslots - 1;
    for (int64_t i = 0; i < d->n; i++) {
        uint64_t p = d->hash[i] & m;
        while (s[p]) p = (p + 1) & m;
        s[p] = (sd_tag(d->hash[i]) << SD_ID_BITS) | (uint64_t)(i + 1);
    }
    ray_free_raw(d->slots);
    d->slots = s; d->mask = m;
    return NULL;
}

/* The id of the bytes [p, p + n) with hash h, interned when new. */
static ray_t* sd_intern(ray_strdom_t* d, const char* p, uint32_t n, uint64_t h, int64_t* id) {
    uint64_t q = h & d->mask, tag = sd_tag(h);
    for (uint64_t s; (s = d->slots[q]) != 0; q = (q + 1) & d->mask) {
        if ((s >> SD_ID_BITS) != tag) continue;
        int64_t i = (int64_t)(s & SD_ID_MASK) - 1;
        if (d->ent[i].len == n && memcmp(d->arena + d->ent[i].off, p, n) == 0) { *id = i; return NULL; }
    }
    if (d->used + n > UINT32_MAX)
        return ray_error("range", "string domain: more than %u bytes of distinct strings", UINT32_MAX);
    if (d->used + n > d->cap) {
        size_t c = d->cap ? d->cap : 4096;
        while (c < d->used + n) c *= 2;
        if (c > UINT32_MAX) c = UINT32_MAX;
        char* a = (char*)ray_realloc_raw(d->arena, c);
        if (!a) return ray_error("oom", NULL);
        d->arena = a; d->cap = c;
    }
    if (d->n == d->ecap) {
        int64_t c = d->ecap ? d->ecap * 2 : 256;
        sd_ent_t* ee = (sd_ent_t*)ray_realloc_raw(d->ent, (size_t)c * sizeof(sd_ent_t));
        if (!ee) return ray_error("oom", NULL);
        d->ent = ee;
        uint64_t* hh = (uint64_t*)ray_realloc_raw(d->hash, (size_t)c * sizeof(uint64_t));
        if (!hh) return ray_error("oom", NULL);
        d->hash = hh;
        d->ecap = c;
    }
    int64_t i = d->n++;
    if (n) memcpy(d->arena + d->used, p, n);
    d->ent[i].off = (uint32_t)d->used; d->ent[i].len = n;
    d->hash[i] = h;
    d->used += n;
    d->slots[q] = (tag << SD_ID_BITS) | (uint64_t)(i + 1);
    *id = i;
    if ((uint64_t)d->n * 2 > d->mask + 1) return sd_rehash(d, (d->mask + 1) * 2);
    return NULL;
}

/* One build: the domain, the source's cursor, the destination parts and
 * the last string interned (consecutive rows repeat one often: a CONST
 * vector is one). */
typedef struct {
    ray_strdom_t*     d;
    ray_col_cursor_t  cur;
    int64_t           n_parts;
    const int64_t*    part_off;
    int64_t* const*   part_dst;
    int64_t           p;        /* the part holding the current row */
    const char*       lp;
    uint32_t          ln;
    int64_t           lid;
} sd_build_t;

/* Row r's id slot; r moves forward, or back inside a morsel (its
 * unselected rows are zeroed before its selected ones are read). */
static inline int64_t* sd_slot(sd_build_t* b, int64_t r) {
    while (r < b->part_off[b->p]) b->p--;
    while (r >= b->part_off[b->p + 1]) b->p++;
    return &b->part_dst[b->p][r - b->part_off[b->p]];
}

/* Intern rows [lo, hi) (idx NULL) or rows lo + idx[0..n_idx) (ascending). */
static ray_t* sd_rows(sd_build_t* b, int64_t lo, int64_t hi, const uint16_t* idx, int64_t n_idx) {
    int64_t j = 0;
    for (int64_t r = idx ? lo + idx[0] : lo; idx ? j < n_idx : r < hi; ) {
        const ray_col_range_t* v;
        ray_t* err = ray_col_cursor_at(&b->cur, r, &v);
        if (err) return err;
        const ray_str_t* s = (const ray_str_t*)v->data;
        int64_t vend = v->start + v->n;
        for (; r < vend; ) {
            const ray_str_t* e = &s[r - v->start];
            uint32_t n = e->len;
            const char* p = n ? ray_str_t_ptr(e, v->str_pool) : "";
            int64_t id = 0;
            if (b->lid >= 0 && n == b->ln && memcmp(p, b->lp, n) == 0) id = b->lid;
            else {
                err = sd_intern(b->d, p, n, ray_str_t_hash(e, v->str_pool), &id);
                if (err) return err;
                b->lid = id; b->ln = n; b->lp = b->d->arena + b->d->ent[id].off;
            }
            *sd_slot(b, r) = id;
            if (idx) { if (++j >= n_idx) return NULL; r = lo + idx[j]; }
            else if (++r >= hi) return NULL;
        }
    }
    return NULL;
}

ray_t* ray_strdom_build_parts(ray_col_src_t* src, ray_t* sel, int64_t n_parts, const int64_t* part_off,
                              int64_t* const* part_dst, ray_strdom_t** dom_out) {
    *dom_out = NULL;
    if (!src || !src->obj || src->type != RAY_STR)
        return ray_error("type", "string domain: not a STR source");
    int64_t len = src->len;
    if (n_parts < 0 || (n_parts == 0 ? len != 0 : (part_off[0] != 0 || part_off[n_parts] != len)))
        return ray_error("range", "string domain: parts do not cover the source");
    if (sel && ray_rowsel_meta(sel)->nrows != len) sel = NULL;   /* not this source's: read every row */
    sd_build_t b;
    memset(&b, 0, sizeof b);
    b.d = (ray_strdom_t*)ray_calloc_raw(sizeof *b.d);
    if (!b.d) return ray_error("oom", NULL);
    ray_t* err = sd_rehash(b.d, 1024);
    if (err) { ray_strdom_free(b.d); return err; }
    b.n_parts = n_parts; b.part_off = part_off; b.part_dst = part_dst; b.lid = -1;
    /* Serial, each vector read decoded once into a stack vector: the
     * interning is the walk's shared state. */
    ray_col_fixed_t fixed;
    ray_col_cursor_init_lent(&b.cur, src, &fixed);
    if (!sel) err = len > 0 ? sd_rows(&b, 0, len, NULL, 0) : NULL;
    else {
        /* the selected rows only, by morsel: the ids are first-occurrence
         * over the selection; an unselected row (never grouped) gets 0 and
         * a vector holding none is never read */
        const uint8_t* fl = ray_rowsel_flags(sel);
        const uint32_t* so = ray_rowsel_offsets(sel);
        const uint16_t* ix = ray_rowsel_idx(sel);
        uint32_t ns = ray_rowsel_meta(sel)->n_segs;
        for (uint32_t m = 0; m < ns && !err; m++) {
            int64_t lo = (int64_t)m * RAY_MORSEL_ELEMS;
            int64_t hi = lo + RAY_MORSEL_ELEMS < len ? lo + RAY_MORSEL_ELEMS : len;
            if (fl[m] != RAY_SEL_ALL)
                for (int64_t r = lo; r < hi; r++) *sd_slot(&b, r) = 0;
            if (fl[m] == RAY_SEL_ALL) err = sd_rows(&b, lo, hi, NULL, 0);
            else if (fl[m] == RAY_SEL_MIX && so[m + 1] > so[m])
                err = sd_rows(&b, lo, hi, ix + so[m], (int64_t)(so[m + 1] - so[m]));
        }
    }
    ray_col_cursor_free(&b.cur);
    if (err) { ray_strdom_free(b.d); return err; }
    *dom_out = b.d;
    return NULL;
}

ray_t* ray_strdom_build(ray_col_src_t* src, ray_strdom_t** dom_out, ray_t** ids_out) {
    *dom_out = NULL; *ids_out = NULL;
    if (!src || !src->obj || src->type != RAY_STR)
        return ray_error("type", "string domain: not a STR source");
    int64_t len = src->len;
    ray_t* ids = ray_vec_new(RAY_I64, len > 0 ? len : 1);
    if (!ids || RAY_IS_ERR(ids)) return ids ? ids : ray_error("oom", NULL);
    ids->len = len;
    int64_t off[2] = { 0, len };
    int64_t* dst[1] = { (int64_t*)ray_data(ids) };
    ray_t* err = ray_strdom_build_parts(src, NULL, 1, off, dst, dom_out);
    if (err) { ray_release(ids); return err; }
    *ids_out = ids;
    return NULL;
}

ray_t* ray_strdom_emit(const ray_strdom_t* d, const int64_t* ids, int64_t n, ray_t** out) {
    *out = NULL;
    if (n <= 0) {
        ray_t* v = ray_vec_new(RAY_STR, 1);
        if (!v || RAY_IS_ERR(v)) return v ? v : ray_error("oom", NULL);
        *out = v; return NULL;
    }
    const char** ptrs = (const char**)ray_alloc_raw((size_t)n * sizeof(const char*));
    uint32_t*    lens = (uint32_t*)ray_alloc_raw((size_t)n * sizeof(uint32_t));
    if (!ptrs || !lens) { ray_free_raw(ptrs); ray_free_raw(lens); return ray_error("oom", NULL); }
    for (int64_t i = 0; i < n; i++) {
        int64_t id = ids[i];
        if (id < 0 || id >= d->n) { ptrs[i] = ""; lens[i] = 0; }
        else { ptrs[i] = d->arena + d->ent[id].off; lens[i] = d->ent[id].len; }
    }
    ray_t* v = ray_str_vec_from_parts(ptrs, lens, NULL, n);
    ray_free_raw(ptrs); ray_free_raw(lens);
    if (!v || RAY_IS_ERR(v)) return v ? v : ray_error("oom", NULL);
    *out = v;
    return NULL;
}
