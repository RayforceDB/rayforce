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
#include "core/pool.h"
#include "ops/ops.h"          /* RAY_PARALLEL_THRESHOLD */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* ray_enc_idx_kind_has_region (freestanding, numeric) names these kinds. */
_Static_assert(RAY_IDX_HASH == 1 && RAY_IDX_SORT == 2 && RAY_IDX_BLOOM == 4 && RAY_IDX_PART == 6,
               "ray_enc_idx_kind_has_region kinds are HASH, SORT, BLOOM, PART");
_Static_assert(RAY_IDX_DICT == 7, "ray_enc_idx_kind_in_file names DICT as 7");

static const char* ei_kind_name(uint8_t k) {
    switch (k) {
    case RAY_IDX_HASH: return "HASH"; case RAY_IDX_SORT: return "SORT";
    case RAY_IDX_BLOOM: return "BLOOM"; case RAY_IDX_PART: return "PART";
    case RAY_IDX_DICT: return "DICT";
    default: return "?";
    }
}

/* Bytes of one PART key of parent type t (SYM: the width class in attrs). */
static int ei_key_esz(int8_t t, uint8_t attrs) {
    switch (t) {
    case RAY_BOOL: case RAY_U8: return 1;
    case RAY_I16: return 2;
    case RAY_I32: case RAY_DATE: case RAY_TIME: case RAY_F32: return 4;
    case RAY_I64: case RAY_TIMESPAN: case RAY_TIMESTAMP: case RAY_F64: return 8;
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

/* The scalar rules of spec "Index region" over a root's counts: the
 * writer refuses (nyi) a source breaking one. */
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

static ray_t* ei_index_write(FILE* f, const ray_enc_idx_src_t* s, const ray_index_t* ix, uint64_t* bytes) {
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
    /* Only HASH and PART carry order_name; a name that is empty, too long
     * or holding a NUL is written as -1. */
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

ray_t* ray_enc_index_write(FILE* f, const ray_enc_idx_src_t* s, uint64_t* bytes) {
    *bytes = 0;
    if (!ray_enc_gen2_supported()) EI_NYI("index region: generation 2 needs a little-endian host");
    if (!s->root || RAY_IS_ERR(s->root) || s->root->type != RAY_INDEX)
        return ray_error("type", "index region: not an index object");
    const ray_index_t* ix = ray_index_payload((ray_t*)s->root);
    if (ix->kind != RAY_IDX_HASH) return ei_index_write(f, s, ix, bytes);
    /* A HASH region stores the generation-1 layout (I64 arrays, the low-bits
     * home rule, groups by first occurrence): an index built narrow or under
     * the high-bits rule (RAY_MARK_HASH_HIGH) is written as that layout, so
     * the region's bytes do not depend on the builder. */
    ray_index_t lx = *ix;
    if (!ray_index_hash_legacy(ix, &lx.u.hash.table, &lx.u.hash.gkeys, &lx.u.hash.offs, &lx.u.hash.rows))
        return ei_index_write(f, s, ix, bytes);   /* no arrays to lay out: the writer's own refusal */
    lx.markers &= (uint8_t)~RAY_MARK_HASH_HIGH;
    ray_t* err = ei_index_write(f, s, &lx, bytes);
    ray_release(lx.u.hash.table);
    ray_release(lx.u.hash.gkeys);
    ray_release(lx.u.hash.offs);
    ray_release(lx.u.hash.rows);
    return err;
}

/* ===== DICT (P6b, spec "Index region": DICT) =====
 *
 * The region: the 80-byte root (parent_type STR, built_for_len, the DICT
 * kind fields), then four children at pad32 offsets from 96: codes (I32,
 * len), first_occ (I32, n_distinct), offs (u64, n_distinct + 1, the last
 * the terminal bytes_len) and the entry bytes (bytes_len), the region
 * ending at pad32.  Every pad byte is zero. */

/* The count and offset rules: the writer refuses a source breaking one
 * (nyi).  See the writer's header comment. */
static const char* ei_dict_rule(int64_t len, int64_t nd, const uint64_t* offs, uint64_t bytes_len) {
    if (len < 1 || len > INT32_MAX) return "rows outside 1 .. INT32_MAX";
    if (nd < 1 || nd > len) return "n_distinct outside 1 .. rows";
    if (offs[0] != 0) return "offsets not starting at 0";
    for (int64_t c = 0; c < nd; c++) {
        uint64_t end = c + 1 < nd ? offs[c + 1] : bytes_len;
        if (end < offs[c] || end - offs[c] > UINT32_MAX) return "offsets decreasing or an entry over UINT32_MAX bytes";
    }
    return NULL;
}

ray_t* ray_enc_dict_write(FILE* f, const ray_enc_dict_src_t* s, uint64_t* bytes) {
    *bytes = 0;
    if (!ray_enc_gen2_supported()) EI_NYI("index region: generation 2 needs a little-endian host");
    const char* why = ei_dict_rule(s->len, s->n_distinct, s->offs, s->bytes_len);
    if (why) EI_NYI("index region: DICT %s", why);
    const uint64_t nd = (uint64_t)s->n_distinct;
    uint64_t cb[4] = { (uint64_t)s->len * 4, nd * 4, (nd + 1) * 8, s->bytes_len };
    ei_layout_t L;
    if (!ei_layout(4, cb, 0, &L)) EI_NYI("index region: DICT too large");
    for (int i = 0; i < 4; i++)   /* a 32-bit host: every child's size fits a size_t */
        if ((uint64_t)(size_t)cb[i] != cb[i]) EI_NYI("index region: DICT too large");
    uint8_t root[RAY_ENC_IDX_FIRST_CHILD];
    memset(root, 0, sizeof root);
    root[0] = (uint8_t)RAY_STR;
    memcpy(root + 8, &s->len, 8);
    ray_enc_idx_dict_t d = { (int64_t)L.off[0], (int64_t)L.off[1], (int64_t)L.off[2], (int64_t)L.off[3],
                             s->n_distinct, s->bytes_len };
    memcpy(root + 16, &d, sizeof d);
    uint64_t pos = 0;
    #define EI_PUT(p, n) do { if (!ei_put(f, (p), (n), &pos)) return ray_error("io", "index region: short write"); } while (0)
    #define EI_PAD(to)   do { if (!ei_zero(f, (to), &pos)) return ray_error("io", "index region: short write"); } while (0)
    EI_PUT(root, sizeof root);
    EI_PAD(L.off[0]); EI_PUT(s->codes, (size_t)cb[0]);
    EI_PAD(L.off[1]); EI_PUT(s->first_occ, (size_t)cb[1]);
    EI_PAD(L.off[2]); EI_PUT(s->offs, (size_t)cb[2] - 8); EI_PUT(&s->bytes_len, 8);   /* the terminal offset */
    EI_PAD(L.off[3]); if (s->bytes_len) EI_PUT(s->bytes, (size_t)s->bytes_len);
    EI_PAD(L.end);
    #undef EI_PUT
    #undef EI_PAD
    *bytes = pos;
    return NULL;
}

/* ===== Reading a region we wrote =====
 *
 * The region is taken as written: the root names every child's offset and
 * count.  Every read goes through memcpy, so the caller's base alignment
 * never matters. */

static int64_t ev_i64(const uint8_t* p) { int64_t x; memcpy(&x, p, 8); return x; }

/* ---- DICT: the root fields and the children's places ---- */
void ray_enc_dict_parse(const uint8_t* b, ray_enc_dict_view_t* v) {
    ray_enc_idx_dict_t d; memcpy(&d, b + 16, sizeof d);
    v->len = ev_i64(b + 8); v->n_distinct = d.n_distinct;
    v->codes = (const int32_t*)(b + d.codes); v->first_occ = (const int32_t*)(b + d.first_occ);
    v->offs = (const uint64_t*)(b + d.offs); v->bytes = b + d.bytes;
}

/* ---- HASH / SORT / BLOOM / PART: the kind's fields and the name ---- */
void ray_enc_index_view(const uint8_t* b, uint64_t size, uint8_t kind, int8_t pt, uint8_t attrs, int64_t len,
                        ray_enc_idx_view_t* v) {
    memset(v, 0, sizeof *v);
    v->kind = kind; v->parent_type = pt; v->attrs = attrs; v->len = len; v->base = b; v->size = size;
    const uint8_t* ka = b + 16;
    int64_t name_field = -1;
    switch (kind) {
    case RAY_IDX_HASH:  memcpy(&v->hash, ka, sizeof v->hash); name_field = v->hash.order_name; break;
    case RAY_IDX_SORT:  memcpy(&v->sort, ka, sizeof v->sort); break;
    case RAY_IDX_BLOOM: memcpy(&v->bloom, ka, sizeof v->bloom); break;
    default:            memcpy(&v->part, ka, sizeof v->part); name_field = v->part.order_name; break;
    }
    if (name_field != -1) {
        uint32_t nlen; memcpy(&nlen, b + name_field, 4);
        v->order_name = (const char*)(b + name_field + 8); v->order_len = nlen;
    }
}

/* ===== Parse (spec "Materialization" step 2) =====
 *
 * A view becomes a fresh heap root: the ray_index_t is rebuilt field by
 * field and every child is copied out of the region, so nothing in the
 * root refers to the mapping and the root outlives it. */

/* A large child is copied over the pool in pieces of EP_PIECE bytes (the
 * copy's page faults are most of its cost); a piece the pool skipped on
 * cancel is copied here, so the copy is always whole. */
#define EP_PIECE ((size_t)1 << 20)
typedef struct { uint8_t* dst; const uint8_t* src; size_t n; _Atomic(bool)* done; } ep_par_copy_t;
static void ep_copy_piece(void* ctx, uint32_t wid, int64_t i0, int64_t i1) {
    (void)wid;
    ep_par_copy_t* x = ctx;
    for (int64_t i = i0; i < i1; i++) {
        size_t at = (size_t)i * EP_PIECE, k = x->n - at < EP_PIECE ? x->n - at : EP_PIECE;
        memcpy(x->dst + at, x->src + at, k);
        atomic_store_explicit(&x->done[i], true, memory_order_release);
    }
}
static void ep_memcpy(uint8_t* dst, const uint8_t* src, size_t n) {
    size_t np = (n + EP_PIECE - 1) / EP_PIECE;
    ray_pool_t* tp = ray_pool_get();
    _Atomic(bool)* done = NULL;
    if (np >= 4 && np <= RAY_POOL_INIT_TASKS && ray_pool_par_dispatch_ok(tp, (int64_t)np, 4))
        done = ray_calloc_raw(np * sizeof *done);
    if (!done) { memcpy(dst, src, n); return; }
    ep_par_copy_t x = { dst, src, n, done };
    ray_pool_dispatch_n(tp, ep_copy_piece, &x, (uint32_t)np);
    for (size_t i = 0; i < np; i++)
        if (!atomic_load_explicit(&done[i], memory_order_acquire)) ep_copy_piece(&x, 0, (int64_t)i, (int64_t)i + 1);
    ray_free_raw(done);
}

/* A heap vector of `type` holding n elements copied from p; NULL on oom. */
static ray_t* ep_copy(int8_t type, const uint8_t* p, int64_t n, size_t esz) {
    if (ray_enccol_test_nth_alloc_fails()) return NULL;
    ray_t* c = ray_vec_new(type, n > 0 ? n : 1);
    if (!c) return NULL;
    if (RAY_IS_ERR(c)) { ray_error_free(c); return NULL; }
    c->len = n;
    if (n) ep_memcpy(ray_data(c), p, (size_t)n * esz);
    return c;
}

ray_t* ray_enc_index_parse(const ray_enc_idx_view_t* v, ray_t* vec, ray_t** root_out) {
    *root_out = NULL;
    const uint8_t* b = v->base;
    int kesz = v->kind == RAY_IDX_PART ? ei_key_esz(v->parent_type, v->attrs) : 0;
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
             * the column's domain */
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
