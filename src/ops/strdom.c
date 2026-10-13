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
#include "vec/str.h"

#include <stdint.h>
#include <string.h>

/* Open addressing over the interned strings: slots hold id + 1 (0 empty),
 * at most half full; entries {hash, len, offset into the arena}. */
struct ray_strdom_s {
    char*     arena;
    size_t    used, cap;     /* arena bytes; cap <= UINT32_MAX */
    uint64_t* hash;
    uint32_t* len;
    uint32_t* off;
    int64_t   n, ecap;       /* entries */
    int64_t*  slots;
    uint64_t  mask;          /* slots - 1 */
};

int64_t ray_strdom_count(const ray_strdom_t* d) { return d ? d->n : 0; }

void ray_strdom_free(ray_strdom_t* d) {
    if (!d) return;
    ray_free_raw(d->arena); ray_free_raw(d->hash); ray_free_raw(d->len);
    ray_free_raw(d->off); ray_free_raw(d->slots);
    ray_free_raw(d);
}

static ray_t* sd_rehash(ray_strdom_t* d, uint64_t nslots) {
    int64_t* s = (int64_t*)ray_calloc_raw((size_t)nslots * sizeof(int64_t));
    if (!s) return ray_error("oom", NULL);
    uint64_t m = nslots - 1;
    for (int64_t i = 0; i < d->n; i++) {
        uint64_t p = d->hash[i] & m;
        while (s[p]) p = (p + 1) & m;
        s[p] = i + 1;
    }
    ray_free_raw(d->slots);
    d->slots = s; d->mask = m;
    return NULL;
}

/* The id of the bytes [p, p + n) with hash h, interned when new. */
static ray_t* sd_intern(ray_strdom_t* d, const char* p, uint32_t n, uint64_t h, int64_t* id) {
    uint64_t q = h & d->mask;
    for (int64_t e; (e = d->slots[q]) != 0; q = (q + 1) & d->mask) {
        int64_t i = e - 1;
        if (d->hash[i] == h && d->len[i] == n && memcmp(d->arena + d->off[i], p, n) == 0) {
            *id = i; return NULL;
        }
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
        uint64_t* hh = (uint64_t*)ray_realloc_raw(d->hash, (size_t)c * sizeof(uint64_t));
        if (!hh) return ray_error("oom", NULL);
        d->hash = hh;
        uint32_t* ll = (uint32_t*)ray_realloc_raw(d->len, (size_t)c * sizeof(uint32_t));
        if (!ll) return ray_error("oom", NULL);
        d->len = ll;
        uint32_t* oo = (uint32_t*)ray_realloc_raw(d->off, (size_t)c * sizeof(uint32_t));
        if (!oo) return ray_error("oom", NULL);
        d->off = oo;
        d->ecap = c;
    }
    int64_t i = d->n++;
    if (n) memcpy(d->arena + d->used, p, n);
    d->hash[i] = h; d->len[i] = n; d->off[i] = (uint32_t)d->used;
    d->used += n;
    d->slots[q] = i + 1;
    *id = i;
    if ((uint64_t)d->n * 2 > d->mask + 1) return sd_rehash(d, (d->mask + 1) * 2);
    return NULL;
}

ray_t* ray_strdom_build(ray_col_src_t* src, ray_strdom_t** dom_out, ray_t** ids_out) {
    *dom_out = NULL; *ids_out = NULL;
    if (!src || !src->obj || src->type != RAY_STR)
        return ray_error("type", "string domain: not a STR source");
    int64_t len = src->len;
    ray_strdom_t* d = (ray_strdom_t*)ray_calloc_raw(sizeof *d);
    if (!d) return ray_error("oom", NULL);
    ray_t* err = sd_rehash(d, 1024);
    if (err) { ray_strdom_free(d); return err; }
    ray_t* ids = ray_vec_new(RAY_I64, len > 0 ? len : 1);
    if (!ids || RAY_IS_ERR(ids)) { ray_strdom_free(d); return ids ? ids : ray_error("oom", NULL); }
    ids->len = len;
    int64_t* out = (int64_t*)ray_data(ids);
    /* Serial, each vector decoded once into a stack vector: the interning is
     * the walk's shared state. */
    ray_col_fixed_t fixed;
    ray_col_cursor_t cur;
    ray_col_cursor_init_lent(&cur, src, &fixed);
    /* Consecutive rows repeat a string often (a CONST vector is one):
     * remember the last one interned. */
    const char* lp = NULL; uint32_t ln = 0; int64_t lid = -1;
    for (int64_t r = 0; r < len && !err; ) {
        const ray_col_range_t* v;
        err = ray_col_cursor_at(&cur, r, &v);
        if (err) break;
        const ray_str_t* s = (const ray_str_t*)v->data;
        int64_t k = r - v->start, e = v->n;
        for (; k < e; k++) {
            uint32_t n = s[k].len;
            const char* p = n ? ray_str_t_ptr(&s[k], v->str_pool) : "";
            int64_t id = 0;
            if (lid >= 0 && n == ln && memcmp(p, lp, n) == 0) id = lid;
            else {
                err = sd_intern(d, p, n, ray_str_t_hash(&s[k], v->str_pool), &id);
                if (err) break;
                lid = id; ln = n; lp = d->arena + d->off[id];
            }
            out[v->start + k] = id;
        }
        r = v->start + e;
    }
    ray_col_cursor_free(&cur);
    if (err) { ray_release(ids); ray_strdom_free(d); return err; }
    *dom_out = d; *ids_out = ids;
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
        else { ptrs[i] = d->arena + d->off[id]; lens[i] = d->len[id]; }
    }
    ray_t* v = ray_str_vec_from_parts(ptrs, lens, NULL, n);
    ray_free_raw(ptrs); ray_free_raw(lens);
    if (!v || RAY_IS_ERR(v)) return v ? v : ray_error("oom", NULL);
    *out = v;
    return NULL;
}
