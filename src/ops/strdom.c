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
#include "ops/idxop.h"      /* RAY_IDX_DICT: a segment's persisted DICT */
#include "store/enccol.h"
#include "store/encfmt.h"
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
        if (d->ent[i].len == n && (n == 0 || memcmp(d->arena + d->ent[i].off, p, n) == 0)) { *id = i; return NULL; }
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

/* One build: the domain, the source's cursor, the destination parts, the
 * last string interned (consecutive rows repeat one often: a CONST vector
 * is one) and the source's segments (a parted source's, else the source
 * itself as one).  A segment that is a single-file encoded column (not
 * synthetic, not materialized) with a STR_DICT entry reads its STR_DICT
 * vectors as views: each code maps to its id through the segment's remap,
 * filled on the code's first occurrence among the rows read.  A segment
 * recording a persisted DICT (P6b) takes precedence: its codes are read by
 * range (one call per vector piece), materialized or not, and mapped the
 * same way.  Codes are segment-local: a remap lives while its segment is
 * read. */
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
    int64_t           n_segs;
    ray_t* const*     segv;     /* borrowed */
    const int64_t*    seg_off;  /* n_segs + 1 */
    int64_t           sg;       /* the current segment, -1 before the first */
    ray_enccol_t*     se;       /* its codes column (with a STR_DICT entry), else NULL */
    const ray_enc_sdict_t* sdict;
    const ray_enc_idict_t* dd;  /* the current segment's persisted DICT, else NULL */
    int64_t*          remap;    /* sdict->n (dd->n_distinct) ids, -1 unseen; allocated on first use */
    ray_enc_vview_t*  vv;       /* the build's view (on the builder's stack) */
    int64_t           vv_at;    /* the segment-local vector vv describes, -1 none */
    bool              vv_codes; /* vv_at is a STR_DICT view (else read by range) */
} sd_build_t;

/* Row r's id slot; r moves forward, or back inside a morsel (its
 * unselected rows are zeroed before its selected ones are read). */
static inline int64_t* sd_slot(sd_build_t* b, int64_t r) {
    while (r < b->part_off[b->p]) b->p--;
    while (r >= b->part_off[b->p + 1]) b->p++;
    return &b->part_dst[b->p][r - b->part_off[b->p]];
}

/* A segment's codes column: single-file encoded, not synthetic, and
 * either recording a persisted DICT (its codes read the mapping, not the
 * decoded column) or without a published materialization (read by range
 * otherwise). */
static ray_enccol_t* sd_codes_col(ray_t* o) {
    if (!o || o->type != RAY_ENCCOL) return NULL;
    ray_enccol_t* e = ray_enccol_of(o);
    if (!e || e->kind != RAY_ENCCOL_SINGLE || e->synthetic) return NULL;
    if (e->hdr.index_kind != RAY_IDX_DICT && ray_enccol_decoded_peek(o)) return NULL;
    return e;
}

/* Make the segment holding row r current: a new segment drops the last
 * one's remap and view and looks up its own dictionary. */
static ray_t* sd_seg(sd_build_t* b, int64_t r) {
    int64_t s = b->sg < 0 ? 0 : b->sg;
    while (s > 0 && r < b->seg_off[s]) s--;
    while (s + 1 < b->n_segs && r >= b->seg_off[s + 1]) s++;
    if (s == b->sg) return NULL;
    ray_free_raw(b->remap);
    b->remap = NULL; b->se = NULL; b->sdict = NULL; b->dd = NULL; b->vv_at = -1; b->sg = s;
    ray_enccol_t* e = sd_codes_col(b->segv[s]);
    if (!e) return NULL;
    const ray_enc_idict_t* dd = NULL;
    ray_t* err = ray_enccol_idict(e, &dd);
    if (err) return err;
    if (dd) {
        b->se = e; b->dd = dd;
        b->remap = ray_enccol_test_nth_alloc_fails() ? NULL       /* the materialization fail hook */
                 : (int64_t*)ray_alloc_raw((size_t)dd->n_distinct * sizeof(int64_t));
        if (!b->remap) return ray_error("oom", NULL);
        for (int64_t k = 0; k < dd->n_distinct; k++) b->remap[k] = -1;
        return NULL;
    }
    if (ray_enccol_decoded_peek(b->segv[s])) return NULL;   /* materialized: read by range */
    const ray_enc_sdict_t* sd = NULL;
    err = ray_enccol_strdict(e, &sd);
    if (err) return err;
    if (sd) { b->se = e; b->sdict = sd; }
    return NULL;
}

/* Is segment-local vector v of the current segment a STR_DICT view (vv)?
 * 1 yes, 0 no (read it by range: another encoding, or a declined view,
 * which the range read decodes), -1 an owned *err. */
static int sd_view(sd_build_t* b, int64_t v, int64_t vrows, ray_t** err) {
    if (b->vv_at == v) return b->vv_codes;
    b->vv_at = -1;
    const ray_enc_desc_t* d = ray_enccol_descs(b->se, v / RAY_ENC_VECS_PER_CHUNK);
    int got = 0;
    if (d[v % RAY_ENC_VECS_PER_CHUNK].encoding == RAY_ENC_STR_DICT) {
        got = ray_enccol_vview(b->se, v, b->vv, err);
        if (got < 0) return -1;
        if (got > 0 && b->vv->enc != RAY_ENC_STR_DICT) got = 0;
        /* an invariant check, not a validation: the view's row count and
         * entry count come from the same descriptors and dictionary as
         * vrows and b->sdict, so this cannot fire on any file */
        if (got > 0 && (b->vv->n != vrows || b->vv->dict_n != b->sdict->n)) {
            *err = ray_error("corrupt", "column %s: string domain: vector %lld holds %d rows of %u entries",
                             b->se->path, (long long)v, b->vv->n, b->vv->dict_n);
            return -1;
        }
        if (got > 0 && !b->remap) {
            b->remap = ray_enccol_test_nth_alloc_fails() ? NULL       /* the materialization fail hook */
                     : (int64_t*)ray_alloc_raw((size_t)(b->sdict->n ? b->sdict->n : 1) * sizeof(int64_t));
            if (!b->remap) { *err = ray_error("oom", NULL); return -1; }
            for (uint32_t k = 0; k < b->sdict->n; k++) b->remap[k] = -1;
        }
    }
    b->vv_at = v; b->vv_codes = got > 0;
    return b->vv_codes;
}

/* Intern rows [lo, hi) (idx NULL) or rows lo + idx[0..n_idx) (ascending). */
static ray_t* sd_rows(sd_build_t* b, int64_t lo, int64_t hi, const uint16_t* idx, int64_t n_idx) {
    int64_t j = 0;
    for (int64_t r = idx ? lo + idx[0] : lo; idx ? j < n_idx : r < hi; ) {
        ray_t* err = b->segv ? sd_seg(b, r) : NULL;
        if (err) return err;
        if (b->dd) {
            /* a DICT segment: the codes of the vector piece holding r, by
             * range (each checked below n_distinct), through the remap */
            const int64_t base = b->seg_off[b->sg], v = (r - base) / RAY_ENC_VEC;
            const int64_t v0 = base + v * RAY_ENC_VEC;
            int64_t vend = v0 + RAY_ENC_VEC;
            if (vend > b->seg_off[b->sg + 1]) vend = b->seg_off[b->sg + 1];
            const int32_t* cd = NULL;
            err = ray_enccol_idict_codes(b->se, v0 - base, vend - v0, &cd);
            if (err) return err;
            for (; r < vend; ) {
                const int32_t c = cd[r - v0];
                int64_t id = b->remap[c];
                if (id < 0) {
                    const char* p; uint32_t n;
                    ray_enc_sdict_entry(b->dd->ent, (uint32_t)c, &p, &n);
                    err = sd_intern(b->d, p, n, ray_str_bytes_hash(p, n), &id);
                    if (err) return err;
                    b->remap[c] = id;
                    /* the arena may have moved: the last string is this one */
                    b->lid = id; b->ln = n; b->lp = n ? b->d->arena + b->d->ent[id].off : p;   /* "": maybe no arena yet */
                }
                *sd_slot(b, r) = id;
                if (idx) { if (++j >= n_idx) return NULL; r = lo + idx[j]; }
                else if (++r >= hi) return NULL;
            }
            continue;
        }
        if (b->se) {
            const int64_t base = b->seg_off[b->sg], v = (r - base) / RAY_ENC_VEC;
            int64_t vend = base + (v + 1) * RAY_ENC_VEC;
            if (vend > b->seg_off[b->sg + 1]) vend = b->seg_off[b->sg + 1];
            const int got = sd_view(b, v, vend - base - v * RAY_ENC_VEC, &err);
            if (got < 0) return err;
            if (got) {
                /* codes through the segment's remap: every row of the
                 * vector read (without idx) or the selected ones */
                const int64_t v0 = base + v * RAY_ENC_VEC;
                const uint32_t* cd = b->vv->codes;
                const ray_enc_sdict_t* sd = b->sdict;
                for (; r < vend; ) {
                    const uint32_t k = cd[r - v0];
                    int64_t id = b->remap[k];
                    if (id < 0) {
                        const char* p; uint32_t n;
                        ray_enc_sdict_entry(sd, k, &p, &n);
                        err = sd_intern(b->d, p, n, ray_str_bytes_hash(p, n), &id);
                        if (err) return err;
                        b->remap[k] = id;
                        /* the arena may have moved: the last string is this one */
                        b->lid = id; b->ln = n; b->lp = n ? b->d->arena + b->d->ent[id].off : p;   /* "": maybe no arena yet */
                    }
                    *sd_slot(b, r) = id;
                    if (idx) { if (++j >= n_idx) return NULL; r = lo + idx[j]; }
                    else if (++r >= hi) return NULL;
                }
                continue;
            }
        }
        const ray_col_range_t* v;
        err = ray_col_cursor_at(&b->cur, r, &v);
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
                b->lid = id; b->ln = n; b->lp = n ? b->d->arena + b->d->ent[id].off : p;   /* "": maybe no arena yet */
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
    /* the segments: a parted source's, else the source as one */
    ray_enc_vview_t vv;                 /* about 21 KB: on the stack (P3 rule) */
    int64_t one_off[2] = { 0, len };
    b.vv = &vv; b.sg = -1; b.vv_at = -1;
    if (src->parted) {
        ray_t** segs = NULL; const int64_t* off = NULL; int64_t ns = 0;
        if (!ray_col_src_parted_segs(src, &ns, &segs, &off, &err)) {
            ray_strdom_free(b.d);
            return err ? err : ray_error("type", "string domain: parted source without segments");
        }
        if (ns > 0 && off[ns] == len) { b.n_segs = ns; b.segv = segs; b.seg_off = off; }
    } else if (len > 0) {
        b.n_segs = 1; b.segv = &src->obj; b.seg_off = one_off;
    }
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
    ray_free_raw(b.remap);
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
        else { lens[i] = d->ent[id].len; ptrs[i] = lens[i] ? d->arena + d->ent[id].off : ""; }
    }
    ray_t* v = ray_str_vec_from_parts(ptrs, lens, NULL, n);
    ray_free_raw(ptrs); ray_free_raw(lens);
    if (!v || RAY_IS_ERR(v)) return v ? v : ray_error("oom", NULL);
    *out = v;
    return NULL;
}
