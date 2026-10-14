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
#include "store/encidx.h"
#include "store/encfmt.h"
#include "store/enccol.h"     /* ray_enccol_test_nth_alloc_fails: the materialization fail hook */
#include "ops/idxop.h"
#include "table/sym.h"
#include "lang/format.h"
#include <stdlib.h>
#include <string.h>

/* ray_enc_idx_kind_has_region (freestanding, numeric) names these kinds. */
_Static_assert(RAY_IDX_HASH == 1 && RAY_IDX_SORT == 2 && RAY_IDX_BLOOM == 4 && RAY_IDX_PART == 6,
               "ray_enc_idx_kind_has_region kinds are HASH, SORT, BLOOM, PART");

static const char* ei_kind_name(uint8_t k) {
    switch (k) {
    case RAY_IDX_HASH: return "HASH"; case RAY_IDX_SORT: return "SORT";
    case RAY_IDX_BLOOM: return "BLOOM"; case RAY_IDX_PART: return "PART";
    default: return "?";
    }
}

/* Bytes of one PART key of parent type t (SYM: the width class in attrs). */
static int ei_key_esz(int8_t t, uint8_t attrs) {
    switch (t) {
    case RAY_BOOL: case RAY_U8: return 1;
    case RAY_I16: return 2;
    case RAY_I32: case RAY_DATE: case RAY_TIME: case RAY_F32: return 4;
    case RAY_I64: case RAY_TIMESTAMP: case RAY_F64: return 8;
    case RAY_SYM: return ray_sym_elem_size(RAY_SYM, attrs);
    default: return 0;
    }
}

/* The normalized layout (Decisions): child i at pad32 of the previous end
 * starting at 96, the name block last, the region ending at pad32.  False
 * when the layout would pass EI_LAYOUT_MAX: every sum and pad stays below
 * 2^63, so offsets compare as int64 and nothing wraps. */
typedef struct { uint64_t off[4], bytes[4]; uint64_t name_off, end; } ei_layout_t;

#define EI_LAYOUT_MAX (UINT64_MAX / 2)

static bool ei_layout(int n, const uint64_t* bytes, uint32_t name_len, ei_layout_t* L) {
    uint64_t cur = RAY_ENC_IDX_FIRST_CHILD;       /* invariant: cur <= EI_LAYOUT_MAX */
    for (int i = 0; i < n; i++) {
        /* compared before padding, which adds at most 31 */
        if (cur > EI_LAYOUT_MAX - 31 || bytes[i] > EI_LAYOUT_MAX - 31 - cur) return false;
        L->off[i] = ray_enc_pad32(cur);
        L->bytes[i] = bytes[i];
        cur = L->off[i] + bytes[i];
    }
    if (cur > EI_LAYOUT_MAX - 31 - 8 - (uint64_t)name_len) return false;
    if (name_len) { L->name_off = ray_enc_pad32(cur); cur = L->name_off + 8 + name_len; }
    else L->name_off = 0;
    L->end = ray_enc_pad32(cur);
    return true;
}

/* The scalar rules of spec "Index validation" over a root's counts: one
 * predicate for the writer (which refuses, nyi) and the validator (which
 * rejects, corrupt), so the writer never emits a region the validator
 * refuses on these. */
typedef enum {
    EI_RULE_OK = 0,
    EI_RULE_HASH_NG, EI_RULE_HASH_NK, EI_RULE_HASH_POW2, EI_RULE_HASH_FULL, EI_RULE_HASH_BIG,
    EI_RULE_HASH_CAP,
    EI_RULE_BLOOM_POW2, EI_RULE_BLOOM_K, EI_RULE_BLOOM_NK, EI_RULE_BLOOM_M,
    EI_RULE_PART_NP, EI_RULE_LEN,
} ei_rule_t;

static const char* ei_rule_text(ei_rule_t r) {
    switch (r) {
    case EI_RULE_HASH_NG:    return "HASH n_groups negative or above n_keys";
    case EI_RULE_HASH_NK:    return "HASH n_keys above the column's rows";
    case EI_RULE_HASH_POW2:  return "HASH bucket count is not a power of two";
    case EI_RULE_HASH_FULL:  return "HASH table has no empty bucket";
    case EI_RULE_HASH_BIG:   return "HASH table too large";
    case EI_RULE_HASH_CAP:   return "HASH bucket count is not the builder's for n_groups";
    case EI_RULE_BLOOM_POW2: return "BLOOM bit count is not a power of two of at least 8";
    case EI_RULE_BLOOM_K:    return "BLOOM k outside 1 .. the builder's maximum";
    case EI_RULE_BLOOM_NK:   return "BLOOM n_keys negative or above the column's rows";
    case EI_RULE_BLOOM_M:    return "BLOOM bit count is not the builder's for n_keys";
    case EI_RULE_PART_NP:    return "PART n_parts outside 1 .. rows (0 only with no rows)";
    case EI_RULE_LEN:        return "negative row count";
    default:                 return "";
    }
}

/* At most 2^59 buckets (2^62 bytes, past any region): cap * 8 never wraps. */
#define EI_HASH_CAP_MAX ((uint64_t)1 << 59)

/* The builders' sizes (src/ops/idxop.c: attach_hash_impl, hash_build_par
 * and ray_index_hash_carry_append size the bucket table by distinct keys;
 * ray_index_attach_bloom sizes the bits by non-null rows):
 *   HASH  mask + 1   = 8 when n_groups < 4, else next_pow2(2 * n_groups);
 *   BLOOM m_mask + 1 = 64 when n_keys < 8, else next_pow2(8 * n_keys).
 * Callers bound the count first (n_groups < cap <= EI_HASH_CAP_MAX;
 * n_keys <= 2^56). */
static uint64_t ei_pow2_at_least(uint64_t x) {
    uint64_t p = 1;
    while (p < x) p <<= 1;
    return p;
}
static uint64_t ei_hash_cap_for(int64_t ng) {
    return ng < 4 ? 8 : ei_pow2_at_least(2 * (uint64_t)ng);
}
static uint64_t ei_bloom_bits_for(int64_t nk) {
    return nk < 8 ? 64 : ei_pow2_at_least(8 * (uint64_t)nk);
}

static ei_rule_t ei_hash_rule(int64_t ng, int64_t nk, uint64_t mask, int64_t len) {
    if (len < 0) return EI_RULE_LEN;
    if (ng < 0 || ng > nk) return EI_RULE_HASH_NG;
    if (nk > len) return EI_RULE_HASH_NK;
    uint64_t cap = mask + 1;
    if (cap == 0 || (cap & (cap - 1))) return EI_RULE_HASH_POW2;
    if (cap <= (uint64_t)ng) return EI_RULE_HASH_FULL;
    if (cap > EI_HASH_CAP_MAX) return EI_RULE_HASH_BIG;
    if (cap != ei_hash_cap_for(ng)) return EI_RULE_HASH_CAP;   /* ng < cap <= 2^59 */
    return EI_RULE_OK;
}
static ei_rule_t ei_bloom_rule(uint64_t m_mask, uint32_t k, int64_t nk, int64_t len) {
    if (len < 0) return EI_RULE_LEN;
    uint64_t m = m_mask + 1;
    if (m < 8 || (m & (m - 1))) return EI_RULE_BLOOM_POW2;
    if (k < 1 || k > RAY_ENC_IDX_BLOOM_K_MAX) return EI_RULE_BLOOM_K;
    if (nk < 0 || nk > len) return EI_RULE_BLOOM_NK;
    /* past 2^56 keys the bits pass 2^59 bytes: no region holds them */
    if (nk > ((int64_t)1 << 56) || m != ei_bloom_bits_for(nk)) return EI_RULE_BLOOM_M;
    return EI_RULE_OK;
}
static ei_rule_t ei_part_rule(int64_t np, int64_t len) {
    if (len < 0) return EI_RULE_LEN;
    if (np < 0 || np > len || (np == 0) != (len == 0)) return EI_RULE_PART_NP;
    return EI_RULE_OK;
}

static bool ei_put(FILE* f, const void* p, size_t n, uint64_t* pos) {
    if (n && fwrite(p, 1, n, f) != n) return false;
    *pos += n;
    return true;
}
static bool ei_zero(FILE* f, uint64_t upto, uint64_t* pos) {
    static const uint8_t z[256];
    while (*pos < upto) {
        size_t k = upto - *pos < sizeof z ? (size_t)(upto - *pos) : sizeof z;
        if (!ei_put(f, z, k, pos)) return false;
    }
    return true;
}

int64_t ray_enc_index_trusted_order(const ray_t* root) {
    if (!root || RAY_IS_ERR(root) || root->type != RAY_INDEX) return -1;
    const ray_index_t* ix = ray_index_payload((ray_t*)root);
    if (ix->markers & RAY_MARK_MMAP) return -1;           /* generation-0 passenger */
    if (ix->kind == RAY_IDX_HASH) return ix->u.hash.order_sym;
    if (ix->kind == RAY_IDX_PART) return ix->u.part.order_sym;
    return -1;
}

#define EI_NYI(...) do { return ray_error("nyi", __VA_ARGS__); } while (0)

static bool ei_child_ok(const ray_t* c, int8_t type, int64_t n) {
    return c && !RAY_IS_ERR(c) && c->type == type && c->len == n;
}

ray_t* ray_enc_index_write(FILE* f, const ray_enc_idx_src_t* s, uint64_t* bytes) {
    *bytes = 0;
    if (!ray_enc_gen2_supported()) EI_NYI("index region: generation 2 needs a little-endian host");
    if (!s->root || RAY_IS_ERR(s->root) || s->root->type != RAY_INDEX)
        return ray_error("type", "index region: not an index object");
    const ray_index_t* ix = ray_index_payload((ray_t*)s->root);
    uint8_t kind = ix->kind;
    if (!ray_enc_idx_kind_has_region(kind))
        EI_NYI("index region: kind %u has no region", kind);
    if (!ray_index_kind_admits((ray_idx_kind_t)kind, s->parent_type) ||
        ix->parent_type != s->parent_type || ix->built_for_len != s->len)
        EI_NYI("index region: %s on type %s is not admitted", ei_kind_name(kind), ray_type_name(s->parent_type));
    uint8_t root[RAY_ENC_IDX_FIRST_CHILD];
    memset(root, 0, sizeof root);
    root[0] = (uint8_t)s->parent_type;
    memcpy(root + 8, &s->len, 8);
    const ray_t* ch[4] = { NULL, NULL, NULL, NULL };
    uint64_t cb[4] = { 0, 0, 0, 0 };
    int nch = 0;
    /* Only HASH and PART carry order_name; a name the validator would
     * refuse (empty, too long, holding a NUL) is written as -1. */
    uint32_t nlen = ((kind == RAY_IDX_HASH || kind == RAY_IDX_PART) && s->order_name &&
                     s->order_len >= 1 && s->order_len <= RAY_ENC_IDX_NAME_MAX &&
                     !memchr(s->order_name, 0, s->order_len)) ? s->order_len : 0;
    int kesz = 0;
    switch (kind) {
    case RAY_IDX_HASH: {
        uint64_t cap = ix->u.hash.mask + 1;
        int64_t ng = ix->u.hash.n_groups, nk = ix->u.hash.n_keys;
        ei_rule_t r = ei_hash_rule(ng, nk, ix->u.hash.mask, s->len);
        if (r) EI_NYI("index region: %s", ei_rule_text(r));
        if (!ei_child_ok(ix->u.hash.table, RAY_I64, (int64_t)cap) ||
            !ei_child_ok(ix->u.hash.gkeys, RAY_I64, ng) || !ei_child_ok(ix->u.hash.offs, RAY_I64, ng + 1) ||
            !ei_child_ok(ix->u.hash.rows, RAY_I64, nk))
            EI_NYI("index region: HASH child of the wrong type or length");
        ch[0] = ix->u.hash.table; ch[1] = ix->u.hash.gkeys; ch[2] = ix->u.hash.offs; ch[3] = ix->u.hash.rows;
        cb[0] = cap * 8; cb[1] = (uint64_t)ng * 8; cb[2] = (uint64_t)(ng + 1) * 8; cb[3] = (uint64_t)nk * 8;
        nch = 4;
        break;
    }
    case RAY_IDX_SORT:
        if (!ei_child_ok(ix->u.sort.perm, RAY_I64, s->len)) EI_NYI("index region: SORT child of the wrong type or length");
        ch[0] = ix->u.sort.perm; cb[0] = (uint64_t)s->len * 8; nch = 1;
        break;
    case RAY_IDX_BLOOM: {
        uint64_t m = ix->u.bloom.m_mask + 1;
        ei_rule_t r = ei_bloom_rule(ix->u.bloom.m_mask, ix->u.bloom.k, ix->u.bloom.n_keys, s->len);
        if (r) EI_NYI("index region: %s", ei_rule_text(r));
        if (!ei_child_ok(ix->u.bloom.bits, RAY_U8, (int64_t)(m / 8)))
            EI_NYI("index region: BLOOM child of the wrong type or length");
        ch[0] = ix->u.bloom.bits; cb[0] = m / 8; nch = 1;
        break;
    }
    default: {   /* PART */
        int64_t np = ix->u.part.n_parts;
        ei_rule_t r = ei_part_rule(np, s->len);
        if (r) EI_NYI("index region: %s", ei_rule_text(r));
        kesz = ei_key_esz(s->parent_type, s->attrs);
        if (!kesz || !ei_child_ok(ix->u.part.keys, s->parent_type, np) ||
            !ei_child_ok(ix->u.part.starts, RAY_I64, np) || !ei_child_ok(ix->u.part.lens, RAY_I64, np))
            EI_NYI("index region: PART child of the wrong type or length");
        if (s->parent_type == RAY_SYM && kesz < 8) {
            /* A key id past the column's width class would be truncated;
             * checked before the first byte is written. */
            const ray_t* kv = ix->u.part.keys;
            for (int64_t j = 0; j < np; j++)
                if ((uint64_t)ray_read_sym(ray_data((ray_t*)kv), j, RAY_SYM, kv->attrs) >> (8 * kesz))
                    EI_NYI("index region: PART key wider than the column's symbol width");
        }
        ch[0] = ix->u.part.keys; ch[1] = ix->u.part.starts; ch[2] = ix->u.part.lens;
        cb[0] = (uint64_t)np * (uint64_t)kesz; cb[1] = cb[2] = (uint64_t)np * 8;
        nch = 3;
        break;
    }
    }
    ei_layout_t L;
    if (!ei_layout(nch, cb, nlen, &L)) EI_NYI("index region: %s too large", ei_kind_name(kind));
    int64_t name_field = nlen ? (int64_t)L.name_off : -1;
    uint8_t* ka = root + 16;
    switch (kind) {
    case RAY_IDX_HASH: {
        ray_enc_idx_hash_t h = { (int64_t)L.off[0], (int64_t)L.off[1], (int64_t)L.off[2], (int64_t)L.off[3],
                                 ix->u.hash.mask, ix->u.hash.n_keys, ix->u.hash.n_groups, name_field };
        memcpy(ka, &h, sizeof h);
        break;
    }
    case RAY_IDX_SORT: { ray_enc_idx_sort_t so = { (int64_t)L.off[0] }; memcpy(ka, &so, sizeof so); break; }
    case RAY_IDX_BLOOM: {
        ray_enc_idx_bloom_t b = { (int64_t)L.off[0], ix->u.bloom.m_mask, ix->u.bloom.k, 0, ix->u.bloom.n_keys };
        memcpy(ka, &b, sizeof b);
        break;
    }
    default: {
        ray_enc_idx_part_t p = { (int64_t)L.off[0], (int64_t)L.off[1], (int64_t)L.off[2], ix->u.part.n_parts, name_field };
        memcpy(ka, &p, sizeof p);
        break;
    }
    }
    uint64_t pos = 0;
    if (!ei_put(f, root, sizeof root, &pos)) return ray_error("io", "index region: short write");
    for (int i = 0; i < nch; i++) {
        if (!ei_zero(f, L.off[i], &pos)) return ray_error("io", "index region: short write");
        const ray_t* c = ch[i];
        if (kind == RAY_IDX_PART && i == 0 && s->parent_type == RAY_SYM) {
            /* SYM keys are written at the column's width class (Decisions). */
            uint8_t buf[4096];
            int64_t per = (int64_t)sizeof buf / kesz;
            for (int64_t at = 0; at < c->len; at += per) {
                int64_t k = c->len - at < per ? c->len - at : per;
                for (int64_t j = 0; j < k; j++)
                    ray_write_sym(buf, j, (uint64_t)ray_read_sym(ray_data((ray_t*)c), at + j, RAY_SYM, c->attrs),
                                  RAY_SYM, s->attrs);
                if (!ei_put(f, buf, (size_t)(k * kesz), &pos)) return ray_error("io", "index region: short write");
            }
        } else if (!ei_put(f, ray_data((ray_t*)c), (size_t)L.bytes[i], &pos)) {
            return ray_error("io", "index region: short write");
        }
    }
    if (nlen) {
        if (!ei_zero(f, L.name_off, &pos)) return ray_error("io", "index region: short write");
        uint32_t hdr[2] = { nlen, 0 };
        if (!ei_put(f, hdr, 8, &pos) || !ei_put(f, s->order_name, nlen, &pos))
            return ray_error("io", "index region: short write");
    }
    if (!ei_zero(f, L.end, &pos)) return ray_error("io", "index region: short write");
    *bytes = pos;
    return NULL;
}

/* ===== Validation (spec "Index validation") =====
 *
 * Every read of the region goes through memcpy at an offset already
 * proven inside it, so the caller's base alignment never matters.  Order:
 * the root is in the region before it is read; the counts pass the shared
 * scalar rules and are bounded by the region before any byte count is
 * derived from them; the normalized layout must match the stored offsets
 * and end exactly at `size` before any child is read; scratch is sized
 * from counts so bounded (never from `len` past the region's own size). */

#define EV(...) do { err = ray_error("corrupt", __VA_ARGS__); goto out; } while (0)
#define EV_OOM() do { err = ray_error("oom", "column %s: index validation", path); goto out; } while (0)

static bool ev_zero(const uint8_t* p, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) if (p[i]) return false;
    return true;
}
static int64_t ev_i64(const uint8_t* p) { int64_t x; memcpy(&x, p, 8); return x; }
static int64_t ev_at(const uint8_t* p, int64_t i) { return ev_i64(p + (uint64_t)i * 8); }

/* PART key i, integer types in native order (signed, BOOL / U8 zero-extended). */
static int64_t ev_key_int(const uint8_t* p, int8_t t, int64_t i) {
    switch (t) {
    case RAY_BOOL: case RAY_U8: return p[i];
    case RAY_I16: { int16_t x; memcpy(&x, p + i * 2, 2); return x; }
    case RAY_I32: case RAY_DATE: case RAY_TIME: { int32_t x; memcpy(&x, p + i * 4, 4); return x; }
    default: return ev_at(p, i);
    }
}
/* F32 / F64 key i decoded from its bits.  Floating values are never
 * compared here: release builds use -fno-signed-zeros, under which a
 * test of a zero's sign, or a compare against zero, may be folded away.
 * ord orders every non-NaN value numerically with both zeros equal. */
typedef struct { bool nan, zero, neg; uint64_t ord; } ev_fkey_t;
static ev_fkey_t ev_key_f(const uint8_t* p, int8_t t, int64_t i) {
    uint64_t mag, expo, frac;
    bool neg;
    if (t == RAY_F32) {
        uint32_t x; memcpy(&x, p + i * 4, 4);
        neg = x >> 31; mag = x & 0x7FFFFFFFu; expo = mag >> 23; frac = mag & 0x7FFFFFu;
        if (expo != 0xFF) frac = 0;
        expo = expo == 0xFF;
    } else {
        uint64_t x; memcpy(&x, p + i * 8, 8);
        neg = x >> 63; mag = x & 0x7FFFFFFFFFFFFFFFull; expo = mag >> 52; frac = mag & 0xFFFFFFFFFFFFFull;
        if (expo != 0x7FF) frac = 0;
        expo = expo == 0x7FF;
    }
    ev_fkey_t k = { expo && frac, mag == 0, neg, 0 };
    uint64_t mid = (uint64_t)1 << 63;
    k.ord = neg ? mid - mag : mid + mag;              /* both zeros: mid */
    return k;
}
/* SYM PART key i at the column's width class (esz bytes, zero-extended). */
static int64_t ev_key_sym(const uint8_t* p, int esz, int64_t i) {
    switch (esz) {
    case 1: return p[i];
    case 2: { uint16_t x; memcpy(&x, p + i * 2, 2); return x; }
    case 4: { uint32_t x; memcpy(&x, p + i * 4, 4); return x; }
    default: return ev_at(p, i);
    }
}
static int ev_cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return (x > y) - (x < y);
}

ray_t* ray_enc_index_validate(const uint8_t* b, uint64_t size, uint8_t kind,
                              int8_t pt, uint8_t attrs, int64_t len,
                              uint32_t sym_bound, const void* col_data,
                              const char* path, ray_enc_idx_view_t* v) {
    ray_t* err = NULL;
    uint8_t* bm = NULL;     /* bitmaps and scratch, one at a time; freed at out */
    int64_t* hs = NULL;     /* SYM key hash set / sorted rows */
    memset(v, 0, sizeof *v);
    v->kind = kind; v->parent_type = pt; v->attrs = attrs; v->len = len; v->base = b; v->size = size;
    if (!path) path = "?";
    if (size < RAY_ENC_IDX_FIRST_CHILD || !b) EV("column %s: index region shorter than its root", path);
    if (len < 0) EV("column %s: index over a negative row count", path);
    if (!ray_enc_idx_kind_has_region(kind)) EV("column %s: index kind %u has no region", path, kind);
    if (!ray_index_kind_admits((ray_idx_kind_t)kind, pt))
        EV("column %s: index %s not admitted on type %s", path, ei_kind_name(kind), ray_type_name(pt));
    if ((int8_t)b[0] != pt) EV("column %s: index parent type %d, column is %s", path, (int8_t)b[0], ray_type_name(pt));
    if (!ev_zero(b + 1, 7)) EV("column %s: index root reserved bytes set", path);
    if (ev_i64(b + 8) != len)
        EV("column %s: index built_for_len %lld, column has %lld rows", path, (long long)ev_i64(b + 8), (long long)len);
    if (!ev_zero(b + RAY_ENC_IDX_ROOT_BYTES, RAY_ENC_IDX_FIRST_CHILD - RAY_ENC_IDX_ROOT_BYTES))
        EV("column %s: index root padding set", path);
    const uint8_t* ka = b + 16;
    uint64_t cb[4] = { 0, 0, 0, 0 };
    int64_t off[4] = { 0, 0, 0, 0 };
    int nch = 0, kesz = 0;
    int64_t name_field = -1;
    uint64_t used_ka = 0;
    ei_rule_t rule = EI_RULE_OK;
    switch (kind) {
    case RAY_IDX_HASH: {
        memcpy(&v->hash, ka, sizeof v->hash); used_ka = sizeof v->hash;
        const ray_enc_idx_hash_t* h = &v->hash;
        if ((rule = ei_hash_rule(h->n_groups, h->n_keys, h->mask, len))) break;
        uint64_t cap = h->mask + 1;           /* <= EI_HASH_CAP_MAX */
        if (cap > size / 8) EV("column %s: index HASH table outside the region", path);
        if ((uint64_t)h->n_keys > size / 8) EV("column %s: index HASH rows outside the region", path);
        cb[0] = cap * 8; cb[1] = (uint64_t)h->n_groups * 8;
        cb[2] = ((uint64_t)h->n_groups + 1) * 8; cb[3] = (uint64_t)h->n_keys * 8;
        off[0] = h->table; off[1] = h->gkeys; off[2] = h->offs; off[3] = h->rows;
        nch = 4; name_field = h->order_name;
        break;
    }
    case RAY_IDX_SORT:
        memcpy(&v->sort, ka, sizeof v->sort); used_ka = sizeof v->sort;
        if ((uint64_t)len > size / 8) EV("column %s: index SORT perm outside the region", path);
        cb[0] = (uint64_t)len * 8; off[0] = v->sort.perm; nch = 1;
        break;
    case RAY_IDX_BLOOM: {
        memcpy(&v->bloom, ka, sizeof v->bloom); used_ka = sizeof v->bloom;
        if ((rule = ei_bloom_rule(v->bloom.m_mask, v->bloom.k, v->bloom.n_keys, len))) break;
        if (v->bloom.rsv) EV("column %s: index BLOOM reserved field set", path);
        uint64_t m = v->bloom.m_mask + 1;     /* a power of two >= 8, so m / 8 exact */
        if (m / 8 > size) EV("column %s: index BLOOM bits outside the region", path);
        cb[0] = m / 8; off[0] = v->bloom.bits; nch = 1;
        break;
    }
    default: {                                  /* PART */
        memcpy(&v->part, ka, sizeof v->part); used_ka = sizeof v->part;
        int64_t np = v->part.n_parts;
        if ((rule = ei_part_rule(np, len))) break;
        kesz = ei_key_esz(pt, attrs);
        if (kesz <= 0) EV("column %s: index PART on type %s has no key width", path, ray_type_name(pt));
        if ((uint64_t)np > size / 8) EV("column %s: index PART children outside the region", path);
        cb[0] = (uint64_t)np * (uint64_t)kesz; cb[1] = cb[2] = (uint64_t)np * 8;
        off[0] = v->part.keys; off[1] = v->part.starts; off[2] = v->part.lens;
        nch = 3; name_field = v->part.order_name;
        break;
    }
    }
    if (rule) EV("column %s: index %s", path, ei_rule_text(rule));
    if (!ev_zero(ka + used_ka, 64 - used_ka)) EV("column %s: index kind area reserved bytes set", path);

    /* Children are bounded by size / 8 each, so this layout cannot fail;
     * it places the name block, whose length is read only once its 8-byte
     * header is proven inside the region. */
    ei_layout_t L;
    if (!ei_layout(nch, cb, 0, &L)) EV("column %s: index children outside the region", path);
    uint32_t nlen = 0;
    if (name_field != -1) {
        uint64_t no = ray_enc_pad32(nch ? L.off[nch - 1] + L.bytes[nch - 1] : RAY_ENC_IDX_FIRST_CHILD);
        if (name_field != (int64_t)no || no > size || size - no < 8)
            EV("column %s: index name block placement %lld", path, (long long)name_field);
        uint32_t rsv;
        memcpy(&nlen, b + no, 4); memcpy(&rsv, b + no + 4, 4);
        if (nlen < 1 || nlen > RAY_ENC_IDX_NAME_MAX) EV("column %s: index name block length %u", path, nlen);
        if (rsv) EV("column %s: index name block reserved field set", path);
        if (size - no - 8 < nlen) EV("column %s: index name block outside the region", path);
        if (memchr(b + no + 8, 0, nlen)) EV("column %s: index name holds a NUL byte", path);
    }
    if (!ei_layout(nch, cb, nlen, &L)) EV("column %s: index children outside the region", path);
    for (int i = 0; i < nch; i++)
        if (off[i] != (int64_t)L.off[i])
            EV("column %s: index child %d placement %lld, expected %llu", path, i, (long long)off[i],
               (unsigned long long)L.off[i]);
    if (L.end != size)
        EV("column %s: index region is %llu bytes, its layout %llu", path, (unsigned long long)size,
           (unsigned long long)L.end);
    /* Every byte between the children, after the last one and after the
     * name is zero: the children tile [96, size) in order, so this covers
     * the whole region outside the root. */
    uint64_t at = RAY_ENC_IDX_FIRST_CHILD;
    for (int i = 0; i < nch; i++) {
        if (!ev_zero(b + at, L.off[i] - at)) EV("column %s: index padding set before child %d", path, i);
        at = L.off[i] + L.bytes[i];
    }
    if (nlen) {
        if (!ev_zero(b + at, L.name_off - at)) EV("column %s: index padding set before the name block", path);
        at = L.name_off + 8 + nlen;
        v->order_name = (const char*)(b + L.name_off + 8); v->order_len = nlen;
    }
    if (!ev_zero(b + at, size - at)) EV("column %s: index region tail padding set", path);

    switch (kind) {
    case RAY_IDX_HASH: {
        const ray_enc_idx_hash_t* h = &v->hash;
        uint64_t cap = h->mask + 1;
        int64_t ng = h->n_groups, nk = h->n_keys;
        const uint8_t* tbl = b + h->table;
        const uint8_t* gk = b + h->gkeys;
        const uint8_t* of = b + h->offs;
        const uint8_t* rw = b + h->rows;
        bm = ray_calloc_raw((size_t)(ng > 0 ? ng : 1));        /* ng <= size / 8 */
        if (!bm) EV_OOM();
        int64_t seen = 0;
        for (uint64_t s = 0; s < cap; s++) {
            int64_t t = ev_at(tbl, (int64_t)s);
            if (t < 0 || t > ng) EV("column %s: index HASH bucket %llu holds %lld", path, (unsigned long long)s, (long long)t);
            if (t) {
                if (bm[t - 1]) EV("column %s: index HASH group %lld in more than one bucket", path, (long long)(t - 1));
                bm[t - 1] = 1; seen++;
            }
        }
        if (seen != ng) EV("column %s: index HASH %lld of %lld groups have a bucket", path, (long long)seen, (long long)ng);
        for (int64_t g = 0; g < ng; g++) {
            int64_t kw = ev_at(gk, g);
            if (pt == RAY_SYM && (kw < 1 || (uint64_t)kw >= sym_bound))
                EV("column %s: index HASH SYM key %lld outside the symbol domain", path, (long long)kw);
        }
        /* Reachability in one O(cap) walk, on any input.  cap > ng leaves an
         * empty bucket e0; walking from it, run is the position of bucket p
         * in its cluster (0 for the first bucket after an empty one).  The
         * group in bucket p, home h = mix64(gkeys[g]) & mask, is met by a
         * probe from h without crossing an empty bucket iff
         * (p - h) & mask <= run. */
        {
            uint64_t e0 = 0;
            while (ev_at(tbl, (int64_t)e0) != 0) e0++;           /* seen == ng < cap */
            uint64_t run = 0;
            bool in_run = false;
            for (uint64_t j = 1; j <= cap; j++) {
                uint64_t p = (e0 + j) & h->mask;
                int64_t t = ev_at(tbl, (int64_t)p);
                if (!t) { in_run = false; continue; }
                run = in_run ? run + 1 : 0;
                in_run = true;
                int64_t g = t - 1;
                uint64_t home = ray_index_mix64((uint64_t)ev_at(gk, g)) & h->mask;
                if (((p - home) & h->mask) > run) {
                    if (pt == RAY_STR) EV("column %s: index HASH STR group %lld unreachable from its word", path, (long long)g);
                    EV("column %s: index HASH lookup of group %lld finds nothing", path, (long long)g);
                }
            }
        }
        /* Numeric, temporal and SYM: distinct gkeys (a sorted copy), so the
         * probe that reaches group g meets no other group of its key first
         * and the lookup returns g.  STR words may repeat. */
        if (pt != RAY_STR && ng > 1) {
            hs = ray_alloc_raw((size_t)ng * sizeof(int64_t));    /* ng <= size / 8 */
            if (!hs) EV_OOM();
            memcpy(hs, gk, (size_t)ng * sizeof(int64_t));
            qsort(hs, (size_t)ng, sizeof(int64_t), ev_cmp_i64);
            for (int64_t i = 1; i < ng; i++)
                if (hs[i] == hs[i - 1]) EV("column %s: index HASH lookup of key %lld finds two groups", path, (long long)hs[i]);
            ray_free_raw(hs); hs = NULL;
        }
        if (ev_at(of, 0) != 0 || ev_at(of, ng) != nk) EV("column %s: index HASH offs bounds", path);
        for (int64_t g = 0; g < ng; g++)
            if (ev_at(of, g + 1) <= ev_at(of, g)) EV("column %s: index HASH offs not strictly increasing", path);
        /* offs runs 0 .. nk strictly, so every slice lies inside rows. */
        for (int64_t g = 0; g < ng; g++)
            for (int64_t i = ev_at(of, g), e = ev_at(of, g + 1); i < e; i++) {
                int64_t r = ev_at(rw, i);
                if (r < 0 || r >= len) EV("column %s: index HASH rows entry %lld outside the column", path, (long long)r);
                if (i > ev_at(of, g) && r <= ev_at(rw, i - 1)) EV("column %s: index HASH rows not ascending within a group", path);
            }
        ray_free_raw(bm); bm = NULL;
        /* Uniqueness: a bitmap over len rows when it is no larger than the
         * region, else a sorted copy of the nk <= size / 8 rows. */
        if ((uint64_t)len / 8 < size) {
            bm = ray_calloc_raw((size_t)(len / 8 + 1));
            if (!bm) EV_OOM();
            for (int64_t i = 0; i < nk; i++) {
                int64_t r = ev_at(rw, i);
                if ((bm[r >> 3] >> (r & 7)) & 1) EV("column %s: index HASH rows not unique", path);
                bm[r >> 3] |= (uint8_t)(1u << (r & 7));
            }
        } else if (nk > 1) {
            hs = ray_alloc_raw((size_t)nk * sizeof(int64_t));
            if (!hs) EV_OOM();
            memcpy(hs, rw, (size_t)nk * sizeof(int64_t));
            qsort(hs, (size_t)nk, sizeof(int64_t), ev_cmp_i64);
            for (int64_t i = 1; i < nk; i++)
                if (hs[i] == hs[i - 1]) EV("column %s: index HASH rows not unique", path);
        }
        break;
    }
    case RAY_IDX_SORT: {
        const uint8_t* perm = b + v->sort.perm;
        bm = ray_calloc_raw((size_t)(len / 8 + 1));               /* len <= size / 8 */
        if (!bm) EV_OOM();
        for (int64_t i = 0; i < len; i++) {
            int64_t r = ev_at(perm, i);
            if (r < 0 || r >= len || ((bm[r >> 3] >> (r & 7)) & 1)) EV("column %s: index SORT perm is not a permutation", path);
            bm[r >> 3] |= (uint8_t)(1u << (r & 7));
        }
        break;
    }
    case RAY_IDX_BLOOM:
        break;                  /* the bit array has no further invariant */
    default: {
        int64_t np = v->part.n_parts;
        const uint8_t* kp = b + v->part.keys;
        const uint8_t* st = b + v->part.starts;
        const uint8_t* ln = b + v->part.lens;
        int64_t sum = 0;
        for (int64_t i = 0; i < np; i++) {
            int64_t li = ev_at(ln, i), si = ev_at(st, i);
            if (li <= 0) EV("column %s: index PART lens[%lld] not positive", path, (long long)i);
            if (si != sum) EV("column %s: index PART starts[%lld] is %lld, expected %lld", path, (long long)i, (long long)si, (long long)sum);
            if (li > len - sum) EV("column %s: index PART lens sum past %lld rows", path, (long long)len);
            sum += li;
        }
        if (sum != len) EV("column %s: index PART lens sum %lld, column has %lld rows", path, (long long)sum, (long long)len);
        if (pt == RAY_F32 || pt == RAY_F64) {
            for (int64_t i = 0; i < np; i++) {
                ev_fkey_t k = ev_key_f(kp, pt, i);
                if (k.nan) EV("column %s: index PART key %lld is NaN", path, (long long)i);
                if (i && !(ev_key_f(kp, pt, i - 1).ord < k.ord))
                    EV("column %s: index PART float keys do not strictly increase", path);
                /* starts[i] < len: lens are positive and sum to len */
                if (k.zero && col_data && k.neg != ev_key_f(col_data, pt, ev_at(st, i)).neg)
                    EV("column %s: index PART zero key sign differs from its block's first row", path);
            }
        } else if (pt == RAY_SYM) {
            /* distinct: a sorted copy, O(np log np) on any input */
            hs = ray_alloc_raw((size_t)(np > 0 ? np : 1) * sizeof(int64_t));   /* np <= size / 8 */
            if (!hs) EV_OOM();
            for (int64_t i = 0; i < np; i++) {
                int64_t k = ev_key_sym(kp, kesz, i);
                if (k < 1 || (uint64_t)k >= sym_bound)
                    EV("column %s: index PART SYM key %lld outside the symbol domain", path, (long long)k);
                hs[i] = k;
            }
            qsort(hs, (size_t)np, sizeof(int64_t), ev_cmp_i64);
            for (int64_t i = 1; i < np; i++)
                if (hs[i] == hs[i - 1]) EV("column %s: index PART SYM keys not distinct", path);
        } else {
            for (int64_t i = 1; i < np; i++)
                if (!(ev_key_int(kp, pt, i - 1) < ev_key_int(kp, pt, i)))
                    EV("column %s: index PART keys do not strictly ascend", path);
        }
        break;
    }
    }
out:
    ray_free_raw(bm);
    ray_free_raw(hs);
    return err;
}

/* ===== Parse (spec "Materialization" step 2) =====
 *
 * A validated view becomes a fresh heap root: the ray_index_t is rebuilt
 * field by field and every child is copied out of the region, so nothing
 * in the root refers to the mapping and the root outlives it.  No count
 * sizes an allocation before it is tied to the region again here: the
 * validator proved it, but the view is a plain struct a caller fills. */

/* `count` elements of `esz` bytes at region offset `off` lie inside the
 * region, past the root. */
static bool ep_span(const ray_enc_idx_view_t* v, int64_t off, int64_t count, uint64_t esz) {
    return count >= 0 && off >= RAY_ENC_IDX_FIRST_CHILD && (uint64_t)off <= v->size &&
           (uint64_t)count <= (v->size - (uint64_t)off) / esz;
}

/* A heap vector of `type` holding n elements copied from p; NULL on oom. */
static ray_t* ep_copy(int8_t type, const uint8_t* p, int64_t n, size_t esz) {
    if (ray_enccol_test_nth_alloc_fails()) return NULL;
    ray_t* c = ray_vec_new(type, n > 0 ? n : 1);
    if (!c) return NULL;
    if (RAY_IS_ERR(c)) { ray_error_free(c); return NULL; }
    c->len = n;
    if (n) memcpy(ray_data(c), p, (size_t)n * esz);
    return c;
}

ray_t* ray_enc_index_parse(const ray_enc_idx_view_t* v, ray_t* vec, ray_t** root_out) {
    *root_out = NULL;
    const uint8_t* b = v->base;
    int kesz = v->kind == RAY_IDX_PART ? ei_key_esz(v->parent_type, v->attrs) : 0;
    bool in = b && v->size >= RAY_ENC_IDX_FIRST_CHILD && v->len >= 0 && ray_enc_idx_kind_has_region(v->kind);
    if (in) {
        switch (v->kind) {
        case RAY_IDX_HASH:
            in = v->hash.mask < EI_HASH_CAP_MAX && v->hash.n_groups < INT64_MAX &&
                 ep_span(v, v->hash.table, (int64_t)(v->hash.mask + 1), 8) &&
                 ep_span(v, v->hash.gkeys, v->hash.n_groups, 8) &&
                 ep_span(v, v->hash.offs, v->hash.n_groups + 1, 8) &&
                 ep_span(v, v->hash.rows, v->hash.n_keys, 8);
            break;
        case RAY_IDX_SORT:
            in = ep_span(v, v->sort.perm, v->len, 8);
            break;
        case RAY_IDX_BLOOM:
            in = v->bloom.m_mask < UINT64_MAX && ep_span(v, v->bloom.bits, (int64_t)((v->bloom.m_mask + 1) / 8), 1);
            break;
        default:
            in = kesz > 0 && ep_span(v, v->part.keys, v->part.n_parts, (uint64_t)kesz) &&
                 ep_span(v, v->part.starts, v->part.n_parts, 8) && ep_span(v, v->part.lens, v->part.n_parts, 8);
            break;
        }
    }
    /* the name, when there is one, is inside the region too */
    if (in && v->order_name) {
        const uint8_t* np = (const uint8_t*)v->order_name;
        in = v->order_len >= 1 && v->order_len <= RAY_ENC_IDX_NAME_MAX && np >= b + RAY_ENC_IDX_FIRST_CHILD &&
             (uint64_t)(np - b) <= v->size && v->order_len <= v->size - (uint64_t)(np - b);
    }
    if (!in) return ray_error("corrupt", "index parse: the view is outside its region");
    /* SYM PART keys resolve over the decoded column's domain. */
    if (v->kind == RAY_IDX_PART && v->parent_type == RAY_SYM && (!vec || RAY_IS_ERR(vec) || vec->type != RAY_SYM))
        return ray_error("type", "index parse: SYM PART keys need the decoded SYM column");

    ray_t* root = ray_enccol_test_nth_alloc_fails() ? NULL
                : ray_index_new_root((ray_idx_kind_t)v->kind, v->parent_type, v->len);
    if (!root || RAY_IS_ERR(root)) {
        if (root) ray_error_free(root);
        return ray_error("oom", "index parse");
    }
    ray_index_t* ix = ray_index_payload(root);
    bool ok;
    switch (v->kind) {
    case RAY_IDX_HASH:
        ix->u.hash.mask = v->hash.mask; ix->u.hash.n_keys = v->hash.n_keys;
        ix->u.hash.n_groups = v->hash.n_groups; ix->u.hash.order_sym = -1;
        ok = (ix->u.hash.table = ep_copy(RAY_I64, b + v->hash.table, (int64_t)(v->hash.mask + 1), 8)) &&
             (ix->u.hash.gkeys = ep_copy(RAY_I64, b + v->hash.gkeys, v->hash.n_groups, 8)) &&
             (ix->u.hash.offs  = ep_copy(RAY_I64, b + v->hash.offs, v->hash.n_groups + 1, 8)) &&
             (ix->u.hash.rows  = ep_copy(RAY_I64, b + v->hash.rows, v->hash.n_keys, 8));
        break;
    case RAY_IDX_SORT:
        ok = (ix->u.sort.perm = ep_copy(RAY_I64, b + v->sort.perm, v->len, 8)) != NULL;
        break;
    case RAY_IDX_BLOOM:
        ix->u.bloom.m_mask = v->bloom.m_mask; ix->u.bloom.k = v->bloom.k; ix->u.bloom.n_keys = v->bloom.n_keys;
        ok = (ix->u.bloom.bits = ep_copy(RAY_U8, b + v->bloom.bits, (int64_t)((v->bloom.m_mask + 1) / 8), 1)) != NULL;
        break;
    default: {
        int64_t np = v->part.n_parts;
        ix->u.part.n_parts = np; ix->u.part.order_sym = -1;
        if (v->parent_type == RAY_SYM) {
            /* the column's width class, as written; ids are positions in
             * the column's domain (the validator bounded them by it) */
            ray_t* k = ray_enccol_test_nth_alloc_fails() ? NULL
                     : ray_sym_vec_new(v->attrs & RAY_SYM_W_MASK, np > 0 ? np : 1);
            if (k && RAY_IS_ERR(k)) { ray_error_free(k); k = NULL; }
            if (k) {
                k->len = np;
                if (np) memcpy(ray_data(k), b + v->part.keys, (size_t)np * (size_t)kesz);
                ray_sym_vec_adopt_domain(k, vec);
            }
            ix->u.part.keys = k;
        } else {
            ix->u.part.keys = ep_copy(v->parent_type, b + v->part.keys, np, (size_t)kesz);
        }
        ok = ix->u.part.keys &&
             (ix->u.part.starts = ep_copy(RAY_I64, b + v->part.starts, np, 8)) &&
             (ix->u.part.lens   = ep_copy(RAY_I64, b + v->part.lens, np, 8));
        break;
    }
    }
    /* order_name: interned in this process (HASH and PART carry one) */
    if (ok && v->order_name && (v->kind == RAY_IDX_HASH || v->kind == RAY_IDX_PART)) {
        int64_t id = ray_sym_intern(v->order_name, v->order_len);
        if (id < 0) ok = false;
        else if (v->kind == RAY_IDX_HASH) ix->u.hash.order_sym = id;
        else ix->u.part.order_sym = id;
    }
    if (!ok) { ray_release(root); return ray_error("oom", "index parse"); }   /* releases the set children */
    *root_out = root;
    return NULL;
}
