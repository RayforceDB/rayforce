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
#include "ops/idxop.h"
#include "table/sym.h"
#include "lang/format.h"
#include "mem/heap.h"
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
 * starting at 96, the name block last, the region ending at pad32. */
typedef struct { int n; uint64_t off[4], bytes[4]; uint64_t name_off, end; } ei_layout_t;

static bool ei_layout(int n, const uint64_t* bytes, uint32_t name_len, ei_layout_t* L) {
    uint64_t cur = RAY_ENC_IDX_FIRST_CHILD;
    L->n = n;
    for (int i = 0; i < n; i++) {
        L->off[i] = ray_enc_pad32(cur);
        L->bytes[i] = bytes[i];
        if (bytes[i] > UINT64_MAX / 2 - L->off[i]) return false;
        cur = L->off[i] + bytes[i];
    }
    if (name_len) { L->name_off = ray_enc_pad32(cur); cur = L->name_off + 8 + name_len; }
    else L->name_off = 0;
    L->end = ray_enc_pad32(cur);
    return true;
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
    uint32_t nlen = (s->order_name && s->order_len >= 1 && s->order_len <= RAY_ENC_IDX_NAME_MAX)
                    ? s->order_len : 0;
    int kesz = 0;
    switch (kind) {
    case RAY_IDX_HASH: {
        uint64_t cap = ix->u.hash.mask + 1;
        int64_t ng = ix->u.hash.n_groups, nk = ix->u.hash.n_keys;
        if (ng < 0 || nk < 0 || ng > nk || nk > s->len ||
            cap == 0 || (cap & (cap - 1)) || !ei_child_ok(ix->u.hash.table, RAY_I64, (int64_t)cap) ||
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
        if (m < 8 || (m & (m - 1)) || ix->u.bloom.k == 0 || ix->u.bloom.k > RAY_ENC_IDX_BLOOM_K_MAX ||
            !ei_child_ok(ix->u.bloom.bits, RAY_U8, (int64_t)(m / 8)))
            EI_NYI("index region: BLOOM child of the wrong type or length");
        ch[0] = ix->u.bloom.bits; cb[0] = m / 8; nch = 1;
        break;
    }
    default: {   /* PART */
        int64_t np = ix->u.part.n_parts;
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
