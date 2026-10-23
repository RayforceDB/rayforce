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
#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include "table/sym.h"
#include "core/runtime.h"
#include "store/col.h"
#include "store/encfmt.h"
#include "store/enc.h"
#include "store/enccol.h"
#include "vec/str.h"
#include "store/splay.h"
#include "store/csr.h"
#include "table/domain.h"
#include "table/table.h"
#include "ops/idxop.h"
#include "store/encw.h"
#include "store/part.h"
#include "store/colrange.h"
#include "core/block.h"
#include "lang/format.h"
#include "lang/eval.h"
#include "ops/colgather.h"     /* ray_col_gather_span */
#include "ops/cdfuse.h"        /* ray_cd_bounded, ray_cd_bounded_all */
#include <math.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENCCOL_TMP "/tmp/rayforce_enccol"

/* 256 interned symbols, so SYM test ids up to 255 lie inside the runtime domain. */
static void enccol_setup(void) {
    ray_heap_init(); (void)ray_sym_init(); (void)ray_test_mkdir_p(ray_test_tmp(ENCCOL_TMP));
    for (int i = 0; i < 256; i++) { char b[8]; int n = snprintf(b, sizeof b, "s%d", i); (void)ray_sym_intern(b, (size_t)n); }
}
static void enccol_teardown(void) { ray_test_hooks_off(); ray_sym_destroy(); ray_heap_destroy(); (void)ray_test_rm_rf(ray_test_tmp(ENCCOL_TMP)); }

static ray_err_t save_i32_rows(const char* path, int64_t n) {
    ray_t* v = ray_vec_new(RAY_I32, n); v->len = n;
    for (int64_t i = 0; i < n; i++) ((int32_t*)ray_data(v))[i] = (int32_t)(i % 977);
    ray_err_t e = ray_col_save_gen(v, path, 2);
    ray_release(v);
    return e;
}

/* Patch `n` bytes at file offset `off`. */
static void patch(const char* path, long off, const void* b, size_t n) {
    FILE* f = fopen(path, "r+b");
    if (!f) ray_test_fatal("cannot open %s", path);
    fseek(f, off, SEEK_SET); fwrite(b, 1, n, f); fclose(f);
}

static ray_t* mmap_expect_err(const char* path, const char* code) {
    ray_t* r = ray_col_mmap(path);
    if (!r || !RAY_IS_ERR(r)) { if (r) ray_release(r); return ray_error("fail", "loaded"); }
    if (strcmp(ray_err_code(r), code) != 0) { ray_error_free(r); return ray_error("fail", "code"); }
    ray_error_free(r);
    return NULL;
}

static test_result_t test_open_returns_enccol(void) {
    ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
    TEST_ASSERT_EQ_I(save_i32_rows(ray_test_tmp(ENCCOL_TMP "/a"), 70000), RAY_OK);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/a"));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    TEST_ASSERT_EQ_I(c->type, RAY_ENCCOL);
    TEST_ASSERT_EQ_I(c->len, 70000);
    TEST_ASSERT_EQ_I(c->attrs, 0);
    ray_enccol_t* e = ray_enccol_of(c);
    TEST_ASSERT_EQ_I(e->kind, RAY_ENCCOL_SINGLE);
    TEST_ASSERT_EQ_I(e->type, RAY_I32);
    TEST_ASSERT_EQ_I(e->n_chunks, 2);
    TEST_ASSERT_EQ_I(e->null_count, 0);
    ray_t* copy = ray_alloc_copy(c);                 /* header copy shares the payload */
    TEST_ASSERT_TRUE(copy && !RAY_IS_ERR(copy));
    TEST_ASSERT_EQ_PTR(ray_enccol_of(copy), e);
    TEST_ASSERT_EQ_I(atomic_load(&e->rc), 2);
    ray_release(copy);
    ray_release(c);
    ray_mem_stats(&m1);
    TEST_ASSERT_EQ_U(m1.sys_mapped, m0.sys_mapped);   /* mapping released */
    PASS();
}

/* A header version this build does not read: `version` (the version
 * selects the reader). */
static test_result_t test_load_version(void) {
    const char* p = ray_test_tmp(ENCCOL_TMP "/r");
    uint8_t two = 2;
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    patch(p, 32 + 4, &two, 1);
    TEST_ASSERT_NULL(mmap_expect_err(p, "version"));
    PASS();
}

/* Appending a generation-0 index region would stamp "XIDX" over "XENC"
 * and produce (order 2, "XIDX"), a file that no longer loads. */
static test_result_t test_append_index_refuses_gen2(void) {
    TEST_ASSERT_EQ_I(save_i32_rows(ray_test_tmp(ENCCOL_TMP "/ai"), 3000), RAY_OK);
    ray_t* v = ray_vec_new(RAY_I32, 3000); v->len = 3000;
    for (int64_t i = 0; i < 3000; i++) ((int32_t*)ray_data(v))[i] = (int32_t)(i % 977);
    ray_t* z = ray_index_chunk_zone_compute(v, 8);
    TEST_ASSERT_TRUE(z && !RAY_IS_ERR(z));
    TEST_ASSERT_EQ_I(ray_col_append_index(ray_test_tmp(ENCCOL_TMP "/ai"), ray_index_payload(z), 3000, RAY_I32), RAY_ERR_VERSION);
    ray_release(z); ray_release(v);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/ai"));             /* still loads */
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_release(c);
    PASS();
}

static test_result_t test_sym_requires_domain(void) {
    uint8_t ids[16];
    for (int i = 0; i < 16; i++) ids[i] = (uint8_t)(i % 4);
    ray_t* v = ray_sym_vec_new(RAY_SYM_W8, 16);
    v->len = 16; memcpy(ray_data(v), ids, 16);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/s"), 2), RAY_OK);
    ray_release(v);
    ray_t* r = ray_col_mmap_splayed_dom(ray_test_tmp(ENCCOL_TMP "/s"), NULL);
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "sym");
    ray_error_free(r);
    PASS();
}

/* ray_col_load (non-mmap) on generation 2 returns the plain vector and
 * leaves nothing mapped behind once it is released. */
static test_result_t test_load_gen2_plain(void) {
    ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
    TEST_ASSERT_EQ_I(save_i32_rows(ray_test_tmp(ENCCOL_TMP "/ln"), 3000), RAY_OK);
    ray_t* r = ray_col_load(ray_test_tmp(ENCCOL_TMP "/ln"));
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, RAY_I32);
    TEST_ASSERT_EQ_I(r->len, 3000);
    ray_release(r);
    ray_mem_stats(&m1);
    TEST_ASSERT_EQ_U(m1.sys_mapped, m0.sys_mapped);
    PASS();
}

/* A valid open, then a refused one (a header version this build does not
 * read), leave no mapping. */
static test_result_t test_no_mapping_leak(void) {
    const char* p = ray_test_tmp(ENCCOL_TMP "/lk");
    uint8_t two = 2;
    ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    ray_t* c = ray_col_mmap(p);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_release(c);
    {
        TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
        patch(p, 32 + 4, &two, 1);                         /* version */
        ray_t* r = ray_col_mmap(p);
        TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
        ray_error_free(r);
    }
    ray_mem_stats(&m1);
    TEST_ASSERT_EQ_U(m1.sys_mapped, m0.sys_mapped);
    PASS();
}

/* The big-endian gate (generation 2 answers nyi there, not corrupt) cannot
 * be exercised end to end on a little-endian host: ray_enccol_open's
 * `!ray_enc_gen2_supported()` branch is never taken (the build with
 * -U__BYTE_ORDER__ checks that it compiles).  What is testable here: the
 * discriminator recognises "XENC" bytewise, so on any host a generation-2 file reaches ray_enccol_open
 * instead of being rejected as corrupt before the gate. */
static test_result_t test_be_gate_logic(void) {
    uint8_t h[32] = {0};
    memcpy(h, "XENC", 4); h[17] = RAY_ENC_GEN;
    TEST_ASSERT_EQ_I(ray_col_gen_of(h), 2);
    memcpy(h, "CNEX", 4);                       /* byte-reversed marker is not XENC */
    TEST_ASSERT_EQ_I(ray_col_gen_of(h), RAY_ENC_GEN_CORRUPT);
    PASS();
}

/* ---- fix round 1 ---- */

#define LEN_OFF  ((long)offsetof(ray_t, len))
#define MMOD_OFF ((long)offsetof(ray_t, mmod))
#define RC_OFF   ((long)offsetof(ray_t, rc))

/* Real writer output with a VALUE_DICT side entry loads. */
static const int64_t g_dv[4] = { 11, 900000000001ll, 77, 123456789012345ll };
static test_result_t test_value_dict_side(void) {
    const char* p = ray_test_tmp(ENCCOL_TMP "/vd");
    int64_t d[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) d[i] = g_dv[(uint32_t)((uint32_t)i * 2654435761u) >> 30];
    ray_t* v = ray_vec_from_raw(RAY_I64, d, RAY_ENC_VEC);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    ray_release(v);
    ray_t* c = ray_col_mmap(p);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_enccol_t* e = ray_enccol_of(c);
    TEST_ASSERT_TRUE(e->vdict_off != 0);
    TEST_ASSERT_EQ_U(e->vdict_off % 32, 0);
    ray_release(c);
    PASS();
}

/* SYM with a domain: the saved-count rule (the symfile the caller names
 * must hold every id the column was saved over). */
static ray_t* sym_col_save(const char* p) {
    ray_t* v = ray_sym_vec_new(RAY_SYM_W8, 16);
    v->len = 16;
    for (int i = 0; i < 16; i++) ((uint8_t*)ray_data(v))[i] = (uint8_t)((i % 4) + 1);
    ray_err_t e = ray_col_save_gen(v, p, 2);
    ray_release(v);
    return e == RAY_OK ? NULL : ray_error("fail", "save");
}
static test_result_t test_sym_domain_rules(void) {
    const char* p = ray_test_tmp(ENCCOL_TMP "/sd");
    ray_sym_domain_t* dom = ray_sym_runtime_domain();
    TEST_ASSERT_NULL(sym_col_save(p));
    ray_t* c = ray_col_mmap_splayed_dom(p, dom);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    TEST_ASSERT_EQ_I(ray_enccol_of(c)->type, RAY_SYM);
    ray_release(c);
    /* saved count above the domain's */
    uint32_t big = 1000000; patch(p, RC_OFF, &big, 4);
    ray_t* r = ray_col_mmap_splayed_dom(p, dom);
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
    ray_error_free(r);
    PASS();
}

static bool csr_col_equal(const ray_t* a, const ray_t* b) {
    if (!a || !b) return a == b;
    return a->type == b->type && a->len == b->len && memcmp(ray_data((ray_t*)a), ray_data((ray_t*)b), (size_t)a->len * 8) == 0;
}
static bool csr_equal(const ray_csr_t* a, const ray_csr_t* b) {
    return csr_col_equal(a->offsets, b->offsets) && csr_col_equal(a->targets, b->targets) &&
           csr_col_equal(a->rowmap, b->rowmap) && a->n_nodes == b->n_nodes && a->n_edges == b->n_edges;
}

/* The mapping loader refuses an encoded column; the copying loader materializes it. */
static test_result_t test_consumers_refuse(void) {
    /* (a) a complete generation-0 relation maps; one file rewritten as
     * generation 2 (required or optional) makes the map fail */
    {
        int64_t sd[] = {0, 0, 1, 1, 2, 3}, dd[] = {1, 2, 2, 3, 3, 0};
        ray_t* sv = ray_vec_from_raw(RAY_I64, sd, 6); ray_t* dv = ray_vec_from_raw(RAY_I64, dd, 6);
        ray_t* et = ray_table_new(2);
        et = ray_table_add_col(et, ray_sym_intern("src", 3), sv);
        et = ray_table_add_col(et, ray_sym_intern("dst", 3), dv);
        ray_release(sv); ray_release(dv);
        ray_rel_t* rel = ray_rel_from_edges(et, "src", "dst", 4, 4, true);
        TEST_ASSERT_TRUE(rel != NULL);
        const char* rd = ray_test_tmp(ENCCOL_TMP "/rel");
        TEST_ASSERT_EQ_I(ray_rel_save(rel, rd), RAY_OK);
        ray_rel_free(rel); ray_release(et);
        int64_t md[5] = {0, 1, 2, 0, 1};                   /* plain meta, whatever the saver wrote */
        ray_t* mv = ray_vec_from_raw(RAY_I64, md, 5);
        TEST_ASSERT_EQ_I(ray_col_save(mv, ray_test_tmp(ENCCOL_TMP "/rel/meta")), RAY_OK);
        ray_release(mv);
        ray_rel_t* base = ray_rel_load(rd);                 /* the generation-0 load */
        TEST_ASSERT_TRUE(base != NULL);
        ray_rel_t* good = ray_rel_mmap(rd);
        TEST_ASSERT_TRUE(good != NULL);
        TEST_ASSERT_TRUE(good->fwd.rowmap != NULL);        /* the optional file is really there */
        ray_rel_free(good);
        static const char* files[] = { "fwd_offsets", "fwd_targets", "rev_offsets", "rev_targets",
                                       "fwd_rowmap", "rev_rowmap", "meta" };
        for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
            char fp[256], bak[260];
            snprintf(fp, sizeof fp, "%s/%s", rd, files[i]);
            snprintf(bak, sizeof bak, "%s.bak", fp);
            TEST_ASSERT_EQ_I(rename(fp, bak), 0);
            ray_t* orig = ray_col_load(bak);                /* the original column, rewritten as generation 2 */
            TEST_ASSERT_TRUE(orig && !RAY_IS_ERR(orig));
            TEST_ASSERT_EQ_I(ray_col_save_gen(orig, fp, 2), RAY_OK);
            ray_release(orig);
            TEST_ASSERT_NULL(ray_rel_mmap(rd));             /* mapping refuses an encoded column */
            ray_rel_t* copied = ray_rel_load(rd);           /* the copying loader materializes it */
            TEST_ASSERT_TRUE(copied != NULL);
            TEST_ASSERT_TRUE(csr_equal(&copied->fwd, &base->fwd) && csr_equal(&copied->rev, &base->rev));
            ray_rel_free(copied);
            TEST_ASSERT_EQ_I(rename(bak, fp), 0);
            ray_rel_t* again = ray_rel_mmap(rd);
            TEST_ASSERT_TRUE(again != NULL);
            ray_rel_free(again);
        }
        ray_rel_free(base);
    }
    /* (c) splayed table load: one plain column, one generation-2 file */
    ray_t* tbl = ray_table_new(2);
    ray_t* a = ray_vec_new(RAY_I64, 3000); a->len = 3000;
    ray_t* b = ray_vec_new(RAY_I32, 3000); b->len = 3000;
    for (int i = 0; i < 3000; i++) { ((int64_t*)ray_data(a))[i] = i; ((int32_t*)ray_data(b))[i] = i % 977; }
    tbl = ray_table_add_col(tbl, ray_sym_intern("ca", 2), a);
    tbl = ray_table_add_col(tbl, ray_sym_intern("cb", 2), b);
    ray_release(a); ray_release(b);
    TEST_ASSERT_EQ_I(ray_splay_save(tbl, ray_test_tmp(ENCCOL_TMP "/tb"), NULL), RAY_OK);
    ray_release(tbl);
    TEST_ASSERT_EQ_I(save_i32_rows(ray_test_tmp(ENCCOL_TMP "/tb/cb"), 3000), RAY_OK);   /* now generation 2 */
    ray_t* r = ray_read_splayed(ray_test_tmp(ENCCOL_TMP "/tb"), NULL);
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));          /* the loader keeps the encoded column */
    TEST_ASSERT_NULL(ray_table_get_col_enc(r, ray_sym_intern("ca", 2)));
    TEST_ASSERT_TRUE(ray_table_get_col_enc(r, ray_sym_intern("cb", 2)) != NULL);
    ray_t* pcb = ray_table_get_col(r, ray_sym_intern("cb", 2));   /* the legacy getter decodes */
    TEST_ASSERT_TRUE(pcb && pcb->type == RAY_I32 && pcb->len == 3000);
    TEST_ASSERT_EQ_I(((int32_t*)ray_data(pcb))[2000], 2000 % 977);
    ray_release(r);
    PASS();
}

static uint64_t g_seed = 1;
static uint8_t g_symw = RAY_SYM_W16;      /* SYM width make_col builds */
static uint64_t rng(void) { uint64_t x = g_seed; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return g_seed = x; }

/* A column of `type` and `n` rows in shape s: 0 random, 1 small range,
 * 2 runs, 3 constant, 4 sparse nulls, 5 all null. */
static ray_t* make_col(int8_t type, int64_t n, int s) {
    if (type == RAY_STR) {
        ray_t* v = ray_vec_new(RAY_STR, n);
        char buf[64];
        for (int64_t i = 0; i < n; i++) {
            int len;
            if (s == 5 || (s == 4 && i % 7 == 0)) len = 0;
            else if (s == 3) len = snprintf(buf, sizeof buf, "constant string value");
            else if (s == 2) len = snprintf(buf, sizeof buf, "run %lld long tail text", (long long)(i / 300));
            else len = snprintf(buf, sizeof buf, "%llu", (unsigned long long)(rng() % (s == 1 ? 50 : 100000000000ull)));
            v = ray_str_vec_append(v, buf, (size_t)len);
        }
        if (s >= 4) v->attrs |= RAY_ATTR_HAS_NULLS;
        return v;
    }
    ray_t* v = type == RAY_SYM ? ray_sym_vec_new(g_symw, n) : ray_vec_new(type, n);
    v->len = n;
    uint8_t esz = ray_sym_elem_size(type, v->attrs);
    uint8_t* d = ray_data(v);
    for (int64_t i = 0; i < n; i++) {
        uint64_t x = s == 0 ? rng() : s == 1 ? rng() % 64 : s == 2 ? (uint64_t)(i / 300) * 7919 : 12345;
        bool null = s == 5 || (s == 4 && i % 7 == 0);
        switch (type) {
        case RAY_BOOL: d[i] = (uint8_t)(x & 1); break;
        case RAY_U8:   d[i] = (uint8_t)x; break;
        case RAY_I16:  { int16_t y = null ? NULL_I16 : (int16_t)(x % 30000); memcpy(d + i * 2, &y, 2); break; }
        case RAY_I32: case RAY_DATE: case RAY_TIME:
                       { int32_t y = null ? NULL_I32 : (int32_t)(x % 2000000000); memcpy(d + i * 4, &y, 4); break; }
        case RAY_I64: case RAY_TIMESTAMP:
                       { int64_t y = null ? NULL_I64 : (int64_t)(x >> 1); memcpy(d + i * 8, &y, 8); break; }
        case RAY_F32:  { float y = null ? NULL_F32 : (float)(int64_t)(x % 100000) / 8.0f; memcpy(d + i * 4, &y, 4); break; }
        case RAY_F64:  { double y = null ? NULL_F64 : (double)(int64_t)(x % 1000000) / 16.0; memcpy(d + i * 8, &y, 8); break; }
        case RAY_GUID: { memset(d + i * 16, 0, 16); if (!null) { memcpy(d + i * 16, &x, 8); d[i * 16 + 15] = 1; } break; }
        case RAY_SYM:  ray_write_sym(d, i, null ? 0 : 1 + x % 200, RAY_SYM, v->attrs); break;
        }
        (void)esz;
    }
    if (s >= 4 && type != RAY_BOOL && type != RAY_U8) v->attrs |= RAY_ATTR_HAS_NULLS;
    return v;
}

/* Decode every vector of the generation-2 file and compare with `v`. */
static test_result_t check_decode(ray_t* v, const char* path) {
    ray_t* c = ray_col_mmap(path);
    TEST_ASSERT_FMT(c && !RAY_IS_ERR(c), "mmap %s: %s", path, c ? ray_err_code(c) : "null");
    ray_enccol_t* e = ray_enccol_of(c);
    uint8_t esz = ray_enccol_esz(e);
    uint8_t out[RAY_ENC_VEC * 16];
    char pool[RAY_ENC_VEC * 64];
    const char* spool = (v->type == RAY_STR && v->str_pool) ? (const char*)ray_data(v->str_pool) : NULL;
    for (int64_t vec = 0; vec < ray_enc_nvecs(v->len); vec++) {
        ray_t* err = ray_enccol_decode_vec(e, vec, out, pool, 0);
        TEST_ASSERT_FMT(err == NULL, "vec %lld: %s %s", (long long)vec, err ? ray_err_code(err) : "", ray_error_msg() ? ray_error_msg() : "");
        int vn = ray_enc_vec_len(v->len, vec);
        if (v->type == RAY_STR) {
            const ray_str_t* a = (const ray_str_t*)ray_data(v) + vec * RAY_ENC_VEC;
            const ray_str_t* b = (const ray_str_t*)out;
            for (int i = 0; i < vn; i++) {
                TEST_ASSERT_FMT(a[i].len == b[i].len, "str len row %d", i);
                TEST_ASSERT_FMT(memcmp(ray_str_t_ptr(&a[i], spool), ray_str_t_ptr(&b[i], pool), a[i].len) == 0, "str bytes row %d", i);
            }
        } else {
            const uint8_t* a = (const uint8_t*)ray_data(v) + (size_t)vec * RAY_ENC_VEC * esz;
            TEST_ASSERT_FMT(memcmp(a, out, (size_t)vn * esz) == 0, "type %d vec %lld bytes differ", (int)v->type, (long long)vec);
        }
    }
    ray_release(c);
    PASS();
}

static test_result_t test_roundtrip_types_shapes(void) {
    static const int8_t types[] = { RAY_BOOL, RAY_U8, RAY_I16, RAY_I32, RAY_I64, RAY_DATE, RAY_TIME,
                                    RAY_TIMESTAMP, RAY_F32, RAY_F64, RAY_GUID, RAY_SYM, RAY_STR };
    static const uint8_t widths[] = { RAY_SYM_W8, RAY_SYM_W16, RAY_SYM_W32, RAY_SYM_W64 };
    for (size_t t = 0; t < sizeof types; t++)
        for (size_t w = 0; w < (types[t] == RAY_SYM ? sizeof widths : 1); w++)
            for (int s = 0; s < 6; s++) {
                if ((types[t] == RAY_BOOL || types[t] == RAY_U8) && s >= 4) continue;
                g_symw = widths[w];
                ray_t* v = make_col(types[t], 3000, s);
                g_symw = RAY_SYM_W16;
                TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/rt"), 2), RAY_OK);
                test_result_t r = check_decode(v, ray_test_tmp(ENCCOL_TMP "/rt"));
                ray_release(v);
                if (r.status != TEST_PASS) return r;
            }
    PASS();
}

static test_result_t test_roundtrip_lengths(void) {
    static const int64_t lens[] = { 1, 1023, 1024, 1025, 65535, 65536, 65537 };
    static const int8_t types[] = { RAY_I32, RAY_F64, RAY_STR, RAY_SYM };
    static const uint8_t widths[] = { RAY_SYM_W8, RAY_SYM_W16, RAY_SYM_W32, RAY_SYM_W64 };
    for (size_t t = 0; t < sizeof types; t++)
        for (size_t w = 0; w < (types[t] == RAY_SYM ? sizeof widths : 1); w++)
            for (size_t l = 0; l < sizeof lens / sizeof lens[0]; l++) {
                g_symw = widths[w];
                ray_t* v = make_col(types[t], lens[l], 4);
                g_symw = RAY_SYM_W16;
                TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/len"), 2), RAY_OK);
                test_result_t r = check_decode(v, ray_test_tmp(ENCCOL_TMP "/len"));
                ray_release(v);
                if (r.status != TEST_PASS) return r;
            }
    PASS();
}

/* Signedness at the domain edges and the 127/128 boundary. */
static test_result_t test_roundtrip_edges(void) {
    int64_t d64[6] = { INT64_MIN + 1, INT64_MAX, 0, NULL_I64, -1, INT64_MAX };
    ray_t* v = ray_vec_from_raw(RAY_I64, d64, 6); v->attrs |= RAY_ATTR_HAS_NULLS;
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/e"), 2), RAY_OK);
    test_result_t r = check_decode(v, ray_test_tmp(ENCCOL_TMP "/e")); ray_release(v);
    if (r.status != TEST_PASS) return r;
    int16_t d16[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) d16[i] = (int16_t)(-32767 + (i % 3) * 32767);
    v = ray_vec_from_raw(RAY_I16, d16, RAY_ENC_VEC);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/e"), 2), RAY_OK);
    r = check_decode(v, ray_test_tmp(ENCCOL_TMP "/e")); ray_release(v);
    if (r.status != TEST_PASS) return r;
    uint8_t u8[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) u8[i] = (uint8_t)(127 + (i & 1));
    v = ray_vec_from_raw(RAY_U8, u8, RAY_ENC_VEC);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/e"), 2), RAY_OK);
    r = check_decode(v, ray_test_tmp(ENCCOL_TMP "/e")); ray_release(v);
    return r;
}

/* The checked-in fixtures decode to the vectors that produced them. */
static test_result_t test_golden_decode(void) {
    static const char* names[] = { "i32_for", "i64_const", "i64_delta", "i64_dict_codes_rle", "i64_dict",
                                   "i32_nulls", "i16_all_null", "u8_raw", "bool_rle", "f64_trailer",
                                   "f64_const", "f32_zeros", "guid_mixed", "str_mixed", "str_const",
                                   "i64_empty", "i32_two_chunks", "str_inline", "str_pooled", "str_repeated",
                                   "i16_delta_wT", "i32_delta_wT", "i64_delta_wT", "i64_dict_const_child",
                                   "i64_rle_values_for", "i64_rle_ends_delta" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char p[256]; snprintf(p, sizeof p, "test/data/enc/%s.bin", names[i]);
        ray_t* c = ray_col_mmap(p);
        TEST_ASSERT_FMT(c && !RAY_IS_ERR(c), "fixture %s does not load", names[i]);
        ray_enccol_t* e = ray_enccol_of(c);
        uint8_t out[RAY_ENC_VEC * 16]; char pool[RAY_ENC_VEC * 64];
        for (int64_t vec = 0; vec < ray_enc_nvecs(e->len); vec++) {
            ray_t* err = ray_enccol_decode_vec(e, vec, out, pool, 0);
            TEST_ASSERT_FMT(err == NULL, "fixture %s vec %lld", names[i], (long long)vec);
        }
        ray_release(c);
    }
    PASS();
}

/* The VALUE_DICT entry is validated once whoever asks first; a transient
 * failure of that validation is retried (spec "Validation": once-state). */
typedef struct { ray_enccol_t* e; ray_t* err; } dthr_t;
static void dict_decode_thread(void* raw) {
    dthr_t* t = raw;
    uint8_t out[RAY_ENC_VEC * 8];
    t->err = ray_enccol_decode_vec(t->e, 0, out, NULL, 0);
    if (t->err) { ray_error_free(t->err); t->err = (ray_t*)1; }
    ray_heap_abandon();
}

static test_result_t test_dict_once_concurrent(void) {
    static const int64_t vals[4] = { 11, 900000000001ll, 77, 123456789012345ll };
    ray_t* v = ray_vec_new(RAY_I64, 1024); v->len = 1024;
    for (int i = 0; i < 1024; i++) ((int64_t*)ray_data(v))[i] = vals[(uint32_t)((uint32_t)i * 2654435761u) >> 30];
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/dc"), 2), RAY_OK);
    ray_release(v);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/dc"));
    ray_enccol_t* e = ray_enccol_of(c);
    ray_enc_desc_t d0; memcpy(&d0, e->base + 160, 48);
    TEST_ASSERT_EQ_I(d0.encoding, RAY_ENC_DICT);
    ray_enccol_test_fail_alloc(1);                   /* the validation's allocation */
    uint8_t out[RAY_ENC_VEC * 8];
    ray_t* r = ray_enccol_decode_vec(e, 0, out, NULL, 0);
    ray_enccol_test_fail_alloc(0);
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "oom");
    ray_error_free(r);
    TEST_ASSERT_NULL(e->dict);                       /* not cached */
    dthr_t t[8]; ray_thread_t th[8];
    for (int i = 0; i < 8; i++) { t[i].e = e; t[i].err = NULL; ray_thread_create(&th[i], dict_decode_thread, &t[i]); }
    for (int i = 0; i < 8; i++) { ray_thread_join(th[i]); TEST_ASSERT_NULL(t[i].err); }
    TEST_ASSERT_NOT_NULL(e->dict);
    ray_release(c);
    PASS();
}

/* ---- Task 3b: the integer fast kernel against the reference decode ---- */

/* One case: values (T-bit patterns, vlen rows) encoded as `enc`, decoded
 * by the fast kernel at every output it serves and by the reference path;
 * nulls are rows set in `bm` (NULL: none).  want_w >= 0 pins the width the
 * writer chose.  The fast kernel must take a sound vector. */
static test_result_t fk_case(int8_t type, uint8_t attrs, uint8_t enc, const uint64_t* vals, int vlen,
                             const uint8_t* bm, int want_w, const char* what) {
    ray_enc_kind_t k; TEST_ASSERT_TRUE(ray_enc_kind_of(type, attrs, &k));
    ray_enc_buf_t b = { 0 }; ray_enc_choice_t ch;
    TEST_ASSERT_FMT(ray_enc_write_as(enc, vals, vlen, k, NULL, &b, &ch), "%s: write_as", what);
    if (want_w >= 0) TEST_ASSERT_FMT(ch.width == want_w, "%s: width %d, want %d", what, ch.width, want_w);
    int n_nulls = 0;
    if (type == RAY_SYM) { for (int i = 0; i < vlen; i++) n_nulls += vals[i] == 0; }
    else if (bm) { for (int i = 0; i < vlen; i++) n_nulls += (bm[i >> 3] >> (i & 7)) & 1; }
    const uint8_t* bmp = n_nulls && type != RAY_SYM ? bm : NULL;
    static uint8_t ref[RAY_ENC_VEC * 8 + 8], fast[RAY_ENC_VEC * 8 + 8];
    memset(ref, 0xA5, sizeof ref); memset(fast, 0x5A, sizeof fast);
    int rr = ray_enccol_test_decode_body(type, attrs, enc, ch.width, n_nulls, bmp, b.p, b.len, vlen, false, 0, ref);
    int fr = ray_enccol_test_decode_body(type, attrs, enc, ch.width, n_nulls, bmp, b.p, b.len, vlen, true, 0, fast);
    TEST_ASSERT_FMT(rr == 1, "%s: reference %d", what, rr);
    TEST_ASSERT_FMT(fr == 1, "%s: fast kernel %d", what, fr);
    size_t esz = (size_t)k.T / 8;
    TEST_ASSERT_FMT(memcmp(ref, fast, (size_t)vlen * esz) == 0, "%s: fast differs", what);
    TEST_ASSERT_FMT(fast[(size_t)vlen * esz] == 0x5A, "%s: wrote past the vector", what);
    /* the expression register widening (narrow non-SYM types) */
    if (type != RAY_SYM && k.T < 64) {
        static int64_t wi[RAY_ENC_VEC + 1]; static double wf[RAY_ENC_VEC + 1];
        wi[vlen] = 77; wf[vlen] = 77.0;
        TEST_ASSERT_FMT(ray_enccol_test_decode_body(type, attrs, enc, ch.width, n_nulls, bmp, b.p, b.len, vlen, true, RAY_I64, wi) == 1, "%s: wide i64", what);
        TEST_ASSERT_FMT(ray_enccol_test_decode_body(type, attrs, enc, ch.width, n_nulls, bmp, b.p, b.len, vlen, true, RAY_F64, wf) == 1, "%s: wide f64", what);
        for (int i = 0; i < vlen; i++) {
            int64_t x = k.T == 8 ? (int64_t)ref[i] : k.T == 16 ? (int64_t)((int16_t*)ref)[i] : (int64_t)((int32_t*)ref)[i];
            bool null = bmp && ((bmp[i >> 3] >> (i & 7)) & 1);
            TEST_ASSERT_FMT(wi[i] == (null ? NULL_I64 : x), "%s: wide i64 row %d", what, i);
            TEST_ASSERT_FMT(null ? isnan(wf[i]) : wf[i] == (double)x, "%s: wide f64 row %d", what, i);
        }
        TEST_ASSERT_FMT(wi[vlen] == 77 && wf[vlen] == 77.0, "%s: wide wrote past the vector", what);
    }
    ray_enc_buf_free(&b);
    PASS();
}

/* Sentinel of a signed kind as a T-bit pattern. */
static uint64_t fk_sentinel(int T) { return T == 64 ? (uint64_t)1 << 63 : (uint64_t)1 << (T - 1); }

static test_result_t test_fast_kernel_equal(void) {
    static const struct { int8_t type; uint8_t attrs; } kinds[] = {
        { RAY_U8, 0 }, { RAY_BOOL, 0 }, { RAY_I16, 0 }, { RAY_I32, 0 }, { RAY_I64, 0 }, { RAY_DATE, 0 }, { RAY_TIME, 0 },
        { RAY_SYM, RAY_SYM_W8 }, { RAY_SYM, RAY_SYM_W16 }, { RAY_SYM, RAY_SYM_W32 }, { RAY_SYM, RAY_SYM_W64 },
    };
    static const int lens[] = { 1024, 1000, 77, 1 };
    static uint64_t v[RAY_ENC_VEC];
    static uint8_t bm[RAY_ENC_VEC / 8];
    char what[160];
    g_seed = 0x3b3b;
    for (size_t ki = 0; ki < sizeof kinds / sizeof kinds[0]; ki++) {
        int8_t type = kinds[ki].type; uint8_t attrs = kinds[ki].attrs;
        ray_enc_kind_t k; ray_enc_kind_of(type, attrs, &k);
        const int T = k.T;
        const uint64_t m = T == 64 ? ~0ull : (1ull << T) - 1;
        const bool sym = type == RAY_SYM, sgn = k.is_signed;
        int widths[5] = { 0, 1, T / 2 + 1, T - 1, T };
        for (int wi = 0; wi < 5; wi++) {
            int w = widths[wi];
            if (sym && w > 29) continue;                     /* ids stay below the bound 1 << 30 */
            for (size_t li = 0; li < sizeof lens / sizeof lens[0]; li++) {
                int n = lens[li];
                if (w > 0 && n == 1) continue;                /* one row has range 0 */
                /* FOR_BP: offsets [0, span] over a base.  Signed kinds keep
                 * the logical order and stay off the sentinel: a full-width
                 * signed range is [MIN + 1, MAX], span 2^T - 2, still width T.
                 * SYM ids are 1..2^T - 1 (0 is the null). */
                uint64_t span = w == 0 ? 0 : (w == 64 ? ~0ull : (1ull << w) - 1);
                uint64_t base;
                if (sym) {
                    uint64_t top = m < (1ull << 30) - 1 ? m : (1ull << 30) - 1;   /* the largest id: below the bound */
                    if (w == T) span = m - 1;
                    base = 1 + rng() % (top - span);
                } else if (!sgn) {
                    base = rng() % (m - span + 1);
                } else {
                    if (w == T) span -= 1;
                    __int128 lo = -(((__int128)1) << (T - 1)) + 1, hi = (((__int128)1) << (T - 1)) - 1;
                    uint64_t room = (uint64_t)(hi - lo + 1 - (__int128)span);   /* bases b with b + span <= MAX */
                    __int128 lb = lo + (__int128)(rng() % room);
                    if (wi & 1) lb = lo + (__int128)(rng() % (room / 2 + 1));   /* the lower half: negative */
                    base = (uint64_t)lb & m;
                }
                for (int i = 0; i < n; i++) v[i] = (base + (span ? rng() % span : 0)) & m;
                if (span && n > 1) { v[0] = base; v[n - 1] = (base + span) & m; }
                snprintf(what, sizeof what, "FOR type %d attrs %d w %d n %d", type, attrs, w, n);
                test_result_t r = fk_case(type, attrs, RAY_ENC_FOR_BP, v, n, NULL, n == 1024 ? w : -1, what);
                if (r.status != TEST_PASS) return r;
                if (sgn) {
                    /* nulls in the bitmap (any bits underneath), one at the sentinel */
                    memset(bm, 0, sizeof bm);
                    for (int i = 0; i < n; i += 13) bm[i >> 3] |= (uint8_t)(1u << (i & 7));
                    if (n > 5) { v[5] = fk_sentinel(T); bm[0] |= 1u << 5; }
                    snprintf(what, sizeof what, "FOR nulls type %d w %d n %d", type, w, n);
                    if ((r = fk_case(type, attrs, RAY_ENC_FOR_BP, v, n, bm, -1, what)).status != TEST_PASS) return r;
                    /* DELTA_BP: lane walks with steps in [s0, s0 + span]; a value on the sentinel becomes a null */
                    int L = RAY_ENC_VEC / T;
                    uint64_t s0 = rng() & m;
                    memset(bm, 0, sizeof bm);
                    for (int i = 0; i < n; i++) {
                        uint64_t step = (s0 + (span ? rng() % (span + 1) : 0)) & m;
                        v[i] = i < L ? rng() & m : (v[i - L] + step) & m;
                    }
                    for (int i = 0; i < n; i++) if (v[i] == fk_sentinel(T)) bm[i >> 3] |= (uint8_t)(1u << (i & 7));
                    snprintf(what, sizeof what, "DELTA type %d w %d n %d", type, w, n);
                    if ((r = fk_case(type, attrs, RAY_ENC_DELTA_BP, v, n, bm, -1, what)).status != TEST_PASS) return r;
                }
            }
        }
    }
    PASS();
}

/* ---- Task 17a: the CONST, RLE and RAW kernels against the reference decode ---- */

/* One sound hook body: the reference takes it, the fast kernel takes it
 * with the reference's bytes and writes nothing past vlen, and `want` (the
 * T-bit pattern of each row, NULL: unchecked) is what the reference holds.
 * Narrow non-SYM kinds: the widened outputs are served exactly when `wide`
 * and hold the reference's values, nulls as NULL_I64 / NULL_F64. */
static test_result_t rk_check(int8_t type, uint8_t attrs, uint8_t enc, uint8_t width, const uint8_t* body,
                              uint64_t len, int vlen, const uint8_t* bm, int n_nulls, bool wide,
                              const uint64_t* want, const char* what) {
    ray_enc_kind_t k; TEST_ASSERT_TRUE(ray_enc_kind_of(type, attrs, &k));
    const uint8_t* bmp = n_nulls && ray_enc_has_bitmap(type) ? bm : NULL;
    static uint8_t ref[RAY_ENC_VEC * 8 + 8], fast[RAY_ENC_VEC * 8 + 8];
    memset(ref, 0xA5, sizeof ref); memset(fast, 0x5A, sizeof fast);
    int rr = ray_enccol_test_decode_body(type, attrs, enc, width, n_nulls, bmp, body, len, vlen, false, 0, ref);
    int fr = ray_enccol_test_decode_body(type, attrs, enc, width, n_nulls, bmp, body, len, vlen, true, 0, fast);
    TEST_ASSERT_FMT(rr == 1, "%s: reference %d", what, rr);
    TEST_ASSERT_FMT(fr == 1, "%s: fast kernel %d", what, fr);
    const size_t esz = (size_t)k.T / 8;
    TEST_ASSERT_FMT(memcmp(ref, fast, (size_t)vlen * esz) == 0, "%s: fast differs", what);
    TEST_ASSERT_FMT(fast[(size_t)vlen * esz] == 0x5A, "%s: wrote past the vector", what);
    for (int i = 0; want && i < vlen; i++) {
        uint64_t x = 0; memcpy(&x, ref + (size_t)i * esz, esz);
        TEST_ASSERT_FMT(x == want[i], "%s: row %d holds %llx, want %llx", what, i,
                        (unsigned long long)x, (unsigned long long)want[i]);
    }
    if (type != RAY_SYM && k.T < 64) {
        static int64_t wi[RAY_ENC_VEC + 1]; static double wf[RAY_ENC_VEC + 1];
        wi[vlen] = 77; wf[vlen] = 77.0;
        int ri = ray_enccol_test_decode_body(type, attrs, enc, width, n_nulls, bmp, body, len, vlen, true, RAY_I64, wi);
        int rf = ray_enccol_test_decode_body(type, attrs, enc, width, n_nulls, bmp, body, len, vlen, true, RAY_F64, wf);
        TEST_ASSERT_FMT(ri == (wide ? 1 : 0) && rf == ri, "%s: wide %d / %d, want %d", what, ri, rf, wide ? 1 : 0);
        for (int i = 0; wide && i < vlen; i++) {
            int64_t x = k.T == 8 ? (int64_t)ref[i] : k.T == 16 ? (int64_t)((int16_t*)ref)[i] : (int64_t)((int32_t*)ref)[i];
            bool null = bmp && ((bmp[i >> 3] >> (i & 7)) & 1);
            TEST_ASSERT_FMT(wi[i] == (null ? NULL_I64 : x), "%s: wide i64 row %d", what, i);
            TEST_ASSERT_FMT(null ? isnan(wf[i]) : wf[i] == (double)x, "%s: wide f64 row %d", what, i);
        }
        TEST_ASSERT_FMT(wi[vlen] == 77 && wf[vlen] == 77.0, "%s: wide wrote past the vector", what);
    }
    PASS();
}

/* nr run values of kind k (T-bit patterns), adjacent ones distinct: signed
 * kinds off the sentinel, SYM ids in [1, 2^30) and inside the id width;
 * narrow: inside one base + [0, 15]. */
static void rk_vals(ray_enc_kind_t k, bool sym, int nr, bool narrow, uint64_t* v) {
    const uint64_t m = k.T == 64 ? ~0ull : (1ull << k.T) - 1;
    const uint64_t top = sym ? (m < (1ull << 30) - 1 ? m : (1ull << 30) - 1) : m;
    const uint64_t base = sym ? 1 + rng() % (top - 16) : rng() & m;
    for (int i = 0; i < nr; i++) {
        uint64_t x;
        do {
            x = narrow ? (base + rng() % 16) & m : rng() & m;
            if (sym && !narrow) x = 1 + x % top;
        } while ((k.is_signed && x == fk_sentinel(k.T)) || (i && x == v[i - 1]));
        v[i] = x;
    }
}

/* nr run ends over vlen rows: nr - 1 distinct cuts in [1, vlen), then vlen. */
static void rk_ends(int vlen, int nr, uint64_t* ends) {
    static uint8_t cut[RAY_ENC_VEC + 1];
    memset(cut, 0, sizeof cut);
    for (int got = 0; got < nr - 1;) {
        int p = 1 + (int)(rng() % (uint64_t)(vlen - 1));
        if (!cut[p]) { cut[p] = 1; got++; }
    }
    int r = 0;
    for (int p = 1; p < vlen; p++) if (cut[p]) ends[r++] = (uint64_t)p;
    ends[r] = (uint64_t)vlen;
}

/* A child of an RLE body as the writer lays it out (header, then body),
 * its encoding forced. */
static bool rk_child(ray_enc_buf_t* b, uint8_t enc, const uint64_t* v, int n, ray_enc_kind_t k) {
    ray_enc_buf_t c = { 0 }; ray_enc_choice_t ch;
    bool ok = ray_enc_write_as(enc, v, n, k, NULL, &c, &ch);
    uint32_t bb = (uint32_t)c.len;
    uint8_t h[8] = { enc, ok ? ch.width : 0, 0, 0, 0, 0, 0, 0 };
    memcpy(h + 4, &bb, 4);
    ok = ok && ray_enc_buf_put(b, h, 8) && ray_enc_buf_put(b, c.p, c.len);
    ray_enc_buf_free(&c);
    return ok;
}
/* An RLE body of nr runs: the values child venc (RAW / FOR_BP) of kind k,
 * the ends child eenc (RAW / FOR_BP / DELTA_BP) of U16 run ends.  The
 * writer emits DELTA_BP for signed kinds; run ends read the same as I16. */
static bool rk_rle(ray_enc_buf_t* b, ray_enc_kind_t k, const uint64_t* vals, const uint64_t* ends, int nr,
                   uint8_t venc, uint8_t eenc) {
    uint32_t h[2] = { (uint32_t)nr, 0 };
    ray_enc_kind_t ke = { 16, eenc == RAY_ENC_DELTA_BP, false };
    return ray_enc_buf_put(b, h, 8) && rk_child(b, venc, vals, nr, k) && rk_child(b, eenc, ends, nr, ke);
}

/* The rows of nr runs, then the stored rows the reference holds (signed:
 * the sentinel at a bitmap row; SYM: id 0 is the null itself). */
static int rk_expand(ray_enc_kind_t k, bool sym, const uint64_t* vals, const uint64_t* ends, int nr,
                     const uint8_t* bm, uint64_t* rows, uint64_t* want) {
    int n_nulls = 0;
    for (int i = 0, p = 0; i < nr; p = (int)ends[i++])
        for (int r = p; r < (int)ends[i]; r++) {
            rows[r] = vals[i];
            bool null = k.is_signed && ((bm[r >> 3] >> (r & 7)) & 1);
            want[r] = null ? fk_sentinel(k.T) : vals[i];
            n_nulls += null || (sym && vals[i] == 0);
        }
    return n_nulls;
}

static test_result_t test_runs_kernel_equal(void) {
    static const struct { int8_t type; uint8_t attrs; } kinds[] = {
        { RAY_U8, 0 }, { RAY_BOOL, 0 }, { RAY_I16, 0 }, { RAY_I32, 0 }, { RAY_I64, 0 }, { RAY_DATE, 0 },
        { RAY_TIME, 0 }, { RAY_TIMESTAMP, 0 },
        { RAY_SYM, RAY_SYM_W8 }, { RAY_SYM, RAY_SYM_W16 }, { RAY_SYM, RAY_SYM_W32 }, { RAY_SYM, RAY_SYM_W64 },
    };
    static const int lens[] = { 1024, 1000, 77, 1 };
    static const int runs[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 31, 32, 33, 63, 64, 65,
                                100, 127, 128, 129, 255, 256, 257, 500, 511, 512, 513, 767, 1000, 1023, 1024 };
    /* the children: the writer's choice (0xFF), then each forced pair */
    static const uint8_t kids[][2] = {
        { 0xFF, 0xFF }, { RAY_ENC_RAW, RAY_ENC_RAW }, { RAY_ENC_RAW, RAY_ENC_FOR_BP }, { RAY_ENC_RAW, RAY_ENC_DELTA_BP },
        { RAY_ENC_FOR_BP, RAY_ENC_RAW }, { RAY_ENC_FOR_BP, RAY_ENC_FOR_BP }, { RAY_ENC_FOR_BP, RAY_ENC_DELTA_BP },
    };
    static uint64_t vals[RAY_ENC_VEC], ends[RAY_ENC_VEC], rows[RAY_ENC_VEC], want[RAY_ENC_VEC];
    static uint8_t bm[RAY_ENC_VEC / 8];
    char what[200];
    long cases = 0;
    g_seed = 0x17a;
    for (size_t ki = 0; ki < sizeof kinds / sizeof kinds[0]; ki++) {
        const int8_t type = kinds[ki].type; const uint8_t attrs = kinds[ki].attrs;
        ray_enc_kind_t k; ray_enc_kind_of(type, attrs, &k);
        const bool sym = type == RAY_SYM, sgn = k.is_signed;
        const bool wide = !sym && k.T < 64;
        for (size_t li = 0; li < sizeof lens / sizeof lens[0]; li++) {
            const int n = lens[li];
            for (size_t ri = 0; ri < sizeof runs / sizeof runs[0]; ri++) {
                const int nr = runs[ri];
                if (nr > n) continue;
                for (int nulls = 0; nulls < ((sgn || sym) ? 2 : 1); nulls++) {
                    rk_vals(k, sym, nr, (ri + (size_t)nulls) & 1, vals);
                    rk_ends(n, nr, ends);
                    memset(bm, 0, sizeof bm);
                    if (nulls) {
                        /* every fifth run null (signed: the sentinel, its rows in
                         * the bitmap; SYM: id 0), and scattered bitmap rows */
                        for (int i = 2; i < nr; i += 5) {
                            vals[i] = sym ? 0 : fk_sentinel(k.T);
                            for (int r = i ? (int)ends[i - 1] : 0; sgn && r < (int)ends[i]; r++) bm[r >> 3] |= (uint8_t)(1u << (r & 7));
                        }
                        for (int r = 3; sgn && r < n; r += 37) bm[r >> 3] |= (uint8_t)(1u << (r & 7));
                    }
                    const int n_nulls = rk_expand(k, sym, vals, ends, nr, bm, rows, want);
                    for (size_t c = 0; c < sizeof kids / sizeof kids[0]; c++) {
                        ray_enc_buf_t b = { 0 }; ray_enc_choice_t ch;
                        bool ok = kids[c][0] == 0xFF ? ray_enc_write_as(RAY_ENC_RLE, rows, n, k, NULL, &b, &ch)
                                                     : rk_rle(&b, k, vals, ends, nr, kids[c][0], kids[c][1]);
                        TEST_ASSERT_FMT(ok, "type %d: build", type);
                        snprintf(what, sizeof what, "RLE type %d attrs %d n %d nr %d nulls %d kids %u/%u", type, attrs,
                                 n, nr, n_nulls, kids[c][0], kids[c][1]);
                        test_result_t r = rk_check(type, attrs, RAY_ENC_RLE, 0, b.p, b.len, n, bm, n_nulls, wide, want, what);
                        ray_enc_buf_free(&b);
                        if (r.status != TEST_PASS) return r;
                        cases++;
                    }
                    if (nr == 1) {
                        ray_enc_buf_t b = { 0 }; ray_enc_choice_t ch;
                        TEST_ASSERT_TRUE(ray_enc_write_as(RAY_ENC_CONST, rows, n, k, NULL, &b, &ch));
                        snprintf(what, sizeof what, "CONST type %d attrs %d n %d nulls %d", type, attrs, n, n_nulls);
                        test_result_t r = rk_check(type, attrs, RAY_ENC_CONST, 0, b.p, b.len, n, bm, n_nulls, wide, want, what);
                        ray_enc_buf_free(&b);
                        if (r.status != TEST_PASS) return r;
                    }
                }
            }
            /* CONST all null: signed on the sentinel with every row in the
             * bitmap, SYM id 0 */
            if (sgn || sym) {
                for (int i = 0; i < n; i++) { rows[i] = sym ? 0 : fk_sentinel(k.T); want[i] = rows[i]; }
                memset(bm, 0, sizeof bm);
                for (int i = 0; sgn && i < n; i++) bm[i >> 3] |= (uint8_t)(1u << (i & 7));
                ray_enc_buf_t b = { 0 }; ray_enc_choice_t ch;
                TEST_ASSERT_TRUE(ray_enc_write_as(RAY_ENC_CONST, rows, n, k, NULL, &b, &ch));
                snprintf(what, sizeof what, "CONST all null type %d attrs %d n %d", type, attrs, n);
                test_result_t r = rk_check(type, attrs, RAY_ENC_CONST, 0, b.p, b.len, n, bm, n, wide, want, what);
                ray_enc_buf_free(&b);
                if (r.status != TEST_PASS) return r;
            }
            /* RAW: random rows; with nulls, every seventh row (signed: the
             * sentinel or any value under the bitmap; SYM: id 0) */
            for (int nulls = 0; nulls < ((sgn || sym) ? 2 : 1); nulls++) {
                rk_vals(k, sym, n, false, rows);
                memset(bm, 0, sizeof bm);
                int n_nulls = 0;
                for (int i = 0; i < n; i++) {
                    if (nulls && i % 7 == 1) {
                        n_nulls++;
                        if (sym) rows[i] = 0;
                        else { bm[i >> 3] |= (uint8_t)(1u << (i & 7)); if (i % 14 == 1) rows[i] = fk_sentinel(k.T); }
                    }
                    want[i] = sgn && ((bm[i >> 3] >> (i & 7)) & 1) ? fk_sentinel(k.T) : rows[i];
                }
                ray_enc_buf_t b = { 0 }; ray_enc_choice_t ch;
                TEST_ASSERT_TRUE(ray_enc_write_as(RAY_ENC_RAW, rows, n, k, NULL, &b, &ch));
                snprintf(what, sizeof what, "RAW type %d attrs %d n %d nulls %d", type, attrs, n, n_nulls);
                test_result_t r = rk_check(type, attrs, RAY_ENC_RAW, 0, b.p, b.len, n, bm, n_nulls, false, want, what);
                ray_enc_buf_free(&b);
                if (r.status != TEST_PASS) return r;
            }
        }
    }
    /* every run count 1..1024 over a full I16 vector, the writer's children */
    ray_enc_kind_t k16 = { 16, true, false };
    memset(bm, 0, sizeof bm);
    for (int nr = 1; nr <= RAY_ENC_VEC; nr++) {
        rk_vals(k16, false, nr, nr & 1, vals);
        rk_ends(RAY_ENC_VEC, nr, ends);
        (void)rk_expand(k16, false, vals, ends, nr, bm, rows, want);
        ray_enc_buf_t b = { 0 }; ray_enc_choice_t ch;
        TEST_ASSERT_TRUE(ray_enc_write_as(RAY_ENC_RLE, rows, RAY_ENC_VEC, k16, NULL, &b, &ch));
        snprintf(what, sizeof what, "RLE I16 nr %d", nr);
        test_result_t r = rk_check(RAY_I16, 0, RAY_ENC_RLE, 0, b.p, b.len, RAY_ENC_VEC, bm, 0, true, want, what);
        ray_enc_buf_free(&b);
        if (r.status != TEST_PASS) return r;
    }
    TEST_ASSERT_TRUE(cases > 10000);
    PASS();
}

/* A RAW integer vector without nulls is read in place: the range read's
 * data lies in the mapping and holds the generation-0 values, guarded or
 * not, and counts the vector's bytes as delivered (as its decode would).
 * With nulls, or any other encoding, it is decoded.  (The writer trains a VALUE_DICT on the first chunk, so signed
 * kinds hold their RAW vectors in the second.) */
static test_result_t test_raw_int_view(void) {
    static const struct { int8_t type; uint8_t attrs; int64_t n; bool nulls; } cs[] = {
        { RAY_I64, 0, 70001, false }, { RAY_I16, 0, 70000, false }, { RAY_U8, 0, 1500, false },
        { RAY_SYM, RAY_SYM_W8, 2100, false }, { RAY_TIMESTAMP, 0, 69632, false }, { RAY_I32, 0, 70000, true },
    };
    const char* path = ray_test_tmp(ENCCOL_TMP "/rawv");
    g_seed = 0x4a7;
    for (size_t ci = 0; ci < sizeof cs / sizeof cs[0]; ci++) {
        const int8_t type = cs[ci].type;
        const int64_t n = cs[ci].n;
        ray_t* v = type == RAY_SYM ? ray_sym_vec_new(cs[ci].attrs, n) : ray_vec_new(type, n);
        v->len = n;
        const uint8_t esz = ray_sym_elem_size(type, v->attrs);
        uint8_t* d = ray_data(v);
        for (int64_t i = 0; i < n; i++) {
            uint64_t x = rng();
            switch (type) {
            case RAY_U8:  d[i] = (uint8_t)x; break;
            case RAY_SYM: d[i] = (uint8_t)(1 + x % 255); break;
            case RAY_I16: { int16_t y = (int16_t)x; if (y == NULL_I16) y = 0; memcpy(d + i * 2, &y, 2); break; }
            case RAY_I32: { int32_t y = (int32_t)(uint32_t)(x >> 32); if (y == NULL_I32) y = 0;
                            if (cs[ci].nulls && i % 9 == 0) y = NULL_I32;
                            memcpy(d + i * 4, &y, 4); break; }
            default:      { int64_t y = (int64_t)x; if (y == NULL_I64) y = 0; memcpy(d + i * 8, &y, 8); break; }
            }
        }
        if (cs[ci].nulls) v->attrs |= RAY_ATTR_HAS_NULLS;
        TEST_ASSERT_EQ_I(ray_col_save_gen(v, path, 2), RAY_OK);
        ray_t* c = ray_col_mmap(path);
        TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
        ray_enccol_t* e = ray_enccol_of(c);
        TEST_ASSERT_NOT_NULL(e);
        int raw = 0;
        {
            ray_col_src_t src;
            TEST_ASSERT_NULL(ray_col_src_bind(c, &src));
            for (int64_t at = 0; at < n; at += RAY_ENC_VEC) {
                const ray_enc_desc_t* descs = ray_enccol_descs(e, at / RAY_ENC_CHUNK);
                const bool is_raw = descs[at % RAY_ENC_CHUNK / RAY_ENC_VEC].encoding == RAY_ENC_RAW;
                raw += is_raw;
                const bool view = is_raw && !cs[ci].nulls;
                int64_t len = n - at < RAY_ENC_VEC ? n - at : RAY_ENC_VEC;
                for (int64_t off = 0; off < len; off += len / 2 + 1) {    /* a whole vector and its tail */
                    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
                    ray_enc_counters_reset();
                    ray_col_range_t r;
                    TEST_ASSERT_NULL(ray_col_range(&src, at + off, len - off, &sc, &r));
                    TEST_ASSERT_EQ_I(r.n, len - off);
                    const bool in_map = (const uint8_t*)r.data >= e->base && (const uint8_t*)r.data < e->base + e->size;
                    TEST_ASSERT_FMT(in_map == view, "type %d at %lld: in the mapping %d", type, (long long)(at + off), in_map);
                    TEST_ASSERT_TRUE(!view || !r.may_null);
                    TEST_ASSERT_TRUE(memcmp(r.data, d + (size_t)(at + off) * esz, (size_t)(len - off) * esz) == 0);
                    TEST_ASSERT_TRUE(type != RAY_SYM || r.dom == e->dom);
                    ray_col_scratch_free(&sc);
                    ray_enc_counters_t k; ray_enc_counters_get(&k);
                    TEST_ASSERT_FMT(k.decoded_bytes == (uint64_t)len * esz, "type %d at %lld: counted %llu",
                                    type, (long long)(at + off), (unsigned long long)k.decoded_bytes);
                }
            }
            ray_col_src_release(&src);
        }
        TEST_ASSERT_FMT(raw >= 2, "type %d: %d RAW vectors", type, raw);   /* a full one and a tail */
        ray_release(c);
        ray_release(v);
    }
    PASS();
}

/* ---- Task 13b: point reads and the DICT kernel against the reference decode ---- */

/* Element i of a generation-0 vector of `type`, widened to int64. */
static int64_t gen0_at(int8_t type, const void* d, int i) {
    switch (type) {
    case RAY_I64: case RAY_TIMESTAMP: return ((const int64_t*)d)[i];
    case RAY_I32: case RAY_DATE: case RAY_TIME: return ((const int32_t*)d)[i];
    case RAY_I16: return ((const int16_t*)d)[i];
    default: return ((const uint8_t*)d)[i];
    }
}

/* A DICT vector body into out (8-aligned): the codes base + u[i] (1024 of
 * them) as a U32 FOR_BP child of width w.  Returns its bytes. */
static uint64_t dict_body(uint8_t* out, const uint64_t* u, int w, uint64_t base) {
    uint32_t bb = (uint32_t)ray_enc_size_for(w);
    memset(out, 0, 8 + (size_t)bb);
    out[0] = RAY_ENC_FOR_BP; out[1] = (uint8_t)w;
    memcpy(out + 4, &bb, 4);
    memcpy(out + 8, &base, 8);
    if (w) ray_enc_pack(u, 32, w, out + 16);
    return 8 + (uint64_t)bb;
}

/* Every row's point read of a body equals the reference decode's row
 * (dict: a DICT body over dict[0, n_dict)). */
static test_result_t point_case(int8_t type, uint8_t enc, const uint64_t* dict, uint32_t n_dict,
                                uint8_t width, const uint8_t* body, uint64_t avail, int vlen, const char* what) {
    static uint8_t ref[RAY_ENC_VEC * 8];
    static int64_t pv[RAY_ENC_VEC]; static int8_t res[RAY_ENC_VEC];
    int rr = dict ? ray_enccol_test_decode_dict(type, dict, n_dict, 0, NULL, body, avail, vlen, false, ref)
                  : ray_enccol_test_decode_body(type, 0, enc, width, 0, NULL, body, avail, vlen, false, 0, ref);
    TEST_ASSERT_FMT(rr == 1, "%s: reference %d", what, rr);
    TEST_ASSERT_EQ_I(ray_enccol_test_point_body(type, dict, n_dict, enc, width, 0, body, avail, vlen, pv, res), 0);
    for (int i = 0; i < vlen; i++) {
        TEST_ASSERT_FMT(res[i] == 1, "%s: row %d declined (%d)", what, i, res[i]);
        TEST_ASSERT_FMT(pv[i] == gen0_at(type, ref, i), "%s: row %d point %lld, decode %lld", what, i,
                        (long long)pv[i], (long long)gen0_at(type, ref, i));
    }
    PASS();
}

/* A strictly ascending dictionary of n values of kind k spread over the
 * whole non-null range (the extremes MIN + 1 and MAX included). */
static void dict_values(uint64_t* dv, uint32_t n, ray_enc_kind_t k) {
    const uint64_t m = k.T == 64 ? ~0ull : (1ull << k.T) - 1;
    __int128 lo = -(((__int128)1) << (k.T - 1)) + 1, hi = (((__int128)1) << (k.T - 1)) - 1;
    for (uint32_t j = 0; j < n; j++)
        dv[j] = (uint64_t)(n == 1 ? lo : lo + (hi - lo) * (__int128)j / (__int128)(n - 1)) & m;
}

static test_result_t test_point_read_equal(void) {
    static const int8_t ftypes[] = { RAY_U8, RAY_BOOL, RAY_I16, RAY_I32, RAY_I64, RAY_DATE, RAY_TIME, RAY_TIMESTAMP };
    static const int lens[] = { 1024, 1000, 77, 1 };
    static uint64_t v[RAY_ENC_VEC], u[RAY_ENC_VEC], dv[65536], bodyw[RAY_ENC_VEC / 2 + 4];
    char what[160];
    g_seed = 0x13b1;
    /* FOR_BP: widths 0, 1, T - 1 and T, negative bases, the extremes */
    for (size_t ti = 0; ti < sizeof ftypes / sizeof ftypes[0]; ti++) {
        int8_t type = ftypes[ti];
        ray_enc_kind_t k; TEST_ASSERT_TRUE(ray_enc_kind_of(type, 0, &k));
        const int T = k.T;
        const uint64_t m = T == 64 ? ~0ull : (1ull << T) - 1;
        int widths[4] = { 0, 1, T - 1, T };
        for (int wi = 0; wi < 4; wi++) {
            int w = widths[wi];
            for (size_t li = 0; li < sizeof lens / sizeof lens[0]; li++) {
                int n = lens[li];
                if (w > 0 && n == 1) continue;
                uint64_t span = w == 0 ? 0 : (w == 64 ? ~0ull : (1ull << w) - 1), base;
                if (!k.is_signed) {
                    base = rng() % (m - span + 1);
                } else {
                    if (w == T) span -= 1;                        /* [MIN + 1, MAX]: off the sentinel */
                    __int128 lo = -(((__int128)1) << (T - 1)) + 1, hi = (((__int128)1) << (T - 1)) - 1;
                    uint64_t room = (uint64_t)(hi - lo + 1 - (__int128)span);
                    __int128 lb = lo + (__int128)(rng() % (wi & 1 ? room / 2 + 1 : room));   /* odd: negative */
                    base = (uint64_t)lb & m;
                }
                for (int i = 0; i < n; i++) v[i] = (base + (span ? rng() % span : 0)) & m;
                if (span && n > 1) { v[0] = base; v[n - 1] = (base + span) & m; }
                ray_enc_buf_t b = { 0 }; ray_enc_choice_t ch;
                TEST_ASSERT_TRUE(ray_enc_write_as(RAY_ENC_FOR_BP, v, n, k, NULL, &b, &ch));
                if (n == 1024) TEST_ASSERT_EQ_I(ch.width, w);
                snprintf(what, sizeof what, "FOR type %d w %d n %d", type, ch.width, n);
                test_result_t r = point_case(type, RAY_ENC_FOR_BP, NULL, 0, ch.width, b.p, b.len, n, what);
                ray_enc_buf_free(&b);
                if (r.status != TEST_PASS) return r;
            }
        }
    }
    /* DICT over a FOR_BP code child: code widths 0..16, dictionaries of 1,
     * 2, 255, 256 and 65535 values, full and partial vectors */
    static const int8_t dtypes[] = { RAY_I16, RAY_I32, RAY_I64, RAY_DATE, RAY_TIMESTAMP };
    static const uint32_t sizes[] = { 1, 2, 255, 256, 65535 };
    for (size_t ti = 0; ti < sizeof dtypes / sizeof dtypes[0]; ti++) {
        ray_enc_kind_t k; TEST_ASSERT_TRUE(ray_enc_kind_of(dtypes[ti], 0, &k));
        for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
            uint32_t nd = sizes[si];
            dict_values(dv, nd, k);
            for (int w = 0; w <= 16; w++)
                for (int li = 0; li < 3; li++) {
                    int n = lens[li];
                    uint64_t span = 1ull << w, base = nd >= span ? rng() % (nd - span + 1) : 0;
                    uint64_t lim = nd >= span ? span : nd;              /* codes base + u below nd */
                    memset(u, 0, sizeof u);
                    for (int i = 0; i < n; i++) u[i] = rng() % lim;
                    u[0] = 0; u[n - 1] = lim - 1;
                    uint64_t len = dict_body((uint8_t*)bodyw, u, w, base);
                    snprintf(what, sizeof what, "DICT type %d dict %u w %d n %d", dtypes[ti], nd, w, n);
                    test_result_t r = point_case(dtypes[ti], RAY_ENC_DICT, dv, nd, 0, (const uint8_t*)bodyw, len, n, what);
                    if (r.status != TEST_PASS) return r;
                }
        }
    }
    PASS();
}


/* The fast DICT kernel against the reference decode of one body: the same
 * bytes, nothing written past the vector. */
static test_result_t dict_kernel_case(int8_t type, const uint64_t* dv, uint32_t nd, const uint8_t* body,
                                      uint64_t len, int n, int n_nulls, const uint8_t* bm, const char* what) {
    static _Alignas(8) uint8_t ref[RAY_ENC_VEC * 8 + 8], fast[RAY_ENC_VEC * 8 + 8];   /* the kernel's alignment */
    memset(ref, 0xA5, sizeof ref); memset(fast, 0x5A, sizeof fast);
    int rr = ray_enccol_test_decode_dict(type, dv, nd, n_nulls, bm, body, len, n, false, ref);
    int fr = ray_enccol_test_decode_dict(type, dv, nd, n_nulls, bm, body, len, n, true, fast);
    TEST_ASSERT_FMT(rr == 1, "%s: reference %d", what, rr);
    TEST_ASSERT_FMT(fr == 1, "%s: fast kernel %d", what, fr);
    size_t esz = ray_sym_elem_size(type, 0);
    TEST_ASSERT_FMT(memcmp(ref, fast, (size_t)n * esz) == 0, "%s: fast differs", what);
    TEST_ASSERT_FMT(fast[(size_t)n * esz] == 0x5A, "%s: wrote past the vector", what);
    PASS();
}

/* The DICT kernel: code widths 0..16, dictionaries of 1, 2, 255, 256 and
 * 65535 values, full and partial vectors, with and without nulls. */
static test_result_t test_dict_kernel_equal(void) {
    static const int8_t dtypes[] = { RAY_I16, RAY_I32, RAY_I64, RAY_DATE, RAY_TIME, RAY_TIMESTAMP };
    static const uint32_t sizes[] = { 1, 2, 255, 256, 65535 };
    static const int lens[] = { 1024, 1000, 77, 1 };
    static uint64_t u[RAY_ENC_VEC], dv[65536], bodyw[RAY_ENC_VEC / 2 + 4];
    static uint8_t bm[RAY_ENC_VEC / 8];
    char what[160];
    g_seed = 0x13b2;
    for (size_t ti = 0; ti < sizeof dtypes / sizeof dtypes[0]; ti++) {
        ray_enc_kind_t k; TEST_ASSERT_TRUE(ray_enc_kind_of(dtypes[ti], 0, &k));
        for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
            uint32_t nd = sizes[si];
            dict_values(dv, nd, k);
            for (int w = 0; w <= 16; w++)
                for (size_t li = 0; li < sizeof lens / sizeof lens[0]; li++) {
                    int n = lens[li];
                    uint64_t span = 1ull << w, base = nd >= span ? rng() % (nd - span + 1) : 0;
                    uint64_t lim = nd >= span ? span : nd;
                    memset(u, 0, sizeof u);
                    for (int i = 0; i < n; i++) u[i] = rng() % lim;
                    u[0] = 0; u[n - 1] = lim - 1;
                    uint64_t len = dict_body((uint8_t*)bodyw, u, w, base);
                    snprintf(what, sizeof what, "DICT type %d dict %u w %d n %d", dtypes[ti], nd, w, n);
                    test_result_t r = dict_kernel_case(dtypes[ti], dv, nd, (const uint8_t*)bodyw, len, n, 0, NULL, what);
                    if (r.status != TEST_PASS) return r;
                    /* nulls in the bitmap fold in (any code underneath) */
                    memset(bm, 0, sizeof bm);
                    int nn = 0;
                    for (int i = 0; i < n; i += 11) { bm[i >> 3] |= (uint8_t)(1u << (i & 7)); nn++; }
                    snprintf(what, sizeof what, "DICT nulls type %d dict %u w %d n %d", dtypes[ti], nd, w, n);
                    if ((r = dict_kernel_case(dtypes[ti], dv, nd, (const uint8_t*)bodyw, len, n, nn, bm, what)).status != TEST_PASS)
                        return r;
                }
        }
    }
    PASS();
}

/* Every check of the reference DICT path stands in the kernel: a code at
 * the dictionary size, a child header or size off, a RAW code child, a
 * dictionary value on the sentinel outside the bitmap, a dictionary entry
 * one byte short.  The kernel declines (the reference reports or decodes
 * it); through a file, the code outside the dictionary is `corrupt` on
 * every read. */
static test_result_t test_dict_kernel_shapes(void) {
    static uint64_t u[RAY_ENC_VEC], bodyw[RAY_ENC_VEC / 2 + 4];
    static _Alignas(8) uint8_t out[RAY_ENC_VEC * 8];
    static const uint64_t dv3[3] = { 10, 20, 30 };
    const uint8_t* body = (const uint8_t*)bodyw;
    for (int i = 0; i < RAY_ENC_VEC; i++) u[i] = (uint64_t)(i % 3);
    uint64_t len = dict_body((uint8_t*)bodyw, u, 2, 0);
    TEST_ASSERT_EQ_I(ray_enccol_test_decode_dict(RAY_I64, dv3, 3, 0, NULL, body, len, RAY_ENC_VEC, true, out), 1);
    /* a dictionary value on the sentinel under the bitmap: a null */
    static const uint64_t dvs[3] = { 0x8000000000000000ull, 20, 30 };
    {
        static uint8_t bm[RAY_ENC_VEC / 8];
        memset(bm, 0, sizeof bm);
        int nn = 0;
        for (int i = 0; i < RAY_ENC_VEC; i += 3) { bm[i >> 3] |= (uint8_t)(1u << (i & 7)); nn++; }
        TEST_ASSERT_EQ_I(ray_enccol_test_decode_dict(RAY_I64, dvs, 3, nn, bm, body, len, RAY_ENC_VEC, false, out), 1);
        TEST_ASSERT_EQ_I(ray_enccol_test_decode_dict(RAY_I64, dvs, 3, nn, bm, body, len, RAY_ENC_VEC, true, out), 1);
        TEST_ASSERT_TRUE(((int64_t*)out)[0] == NULL_I64 && ((int64_t*)out)[1] == 20);
    }
    /* a RAW code child decodes by the reference path only */
    {
        uint8_t* p = (uint8_t*)bodyw;
        memset(p, 0, 8 + RAY_ENC_VEC * 4);
        uint32_t bb = RAY_ENC_VEC * 4;
        p[0] = RAY_ENC_RAW; memcpy(p + 4, &bb, 4);
        for (int i = 0; i < RAY_ENC_VEC; i++) { uint32_t c = (uint32_t)(i % 3); memcpy(p + 8 + 4 * i, &c, 4); }
        TEST_ASSERT_EQ_I(ray_enccol_test_decode_dict(RAY_I64, dv3, 3, 0, NULL, p, 8 + bb, RAY_ENC_VEC, true, out), 0);
        TEST_ASSERT_EQ_I(ray_enccol_test_decode_dict(RAY_I64, dv3, 3, 0, NULL, p, 8 + bb, RAY_ENC_VEC, false, out), 1);
    }
    PASS();
}

/* Task 17b: the CONST / RLE point read equals the reference decode on every
 * row, for every integer kind, lengths 1024, 1000, 77 and 1, run counts
 * 1..1024 and every legal child pair; a run whose value is outside the
 * vector's statistics is `corrupt` (-1) at its rows. */
static test_result_t test_point_read_runs(void) {
    static const int8_t types[] = { RAY_U8, RAY_BOOL, RAY_I16, RAY_I32, RAY_I64, RAY_DATE, RAY_TIME, RAY_TIMESTAMP };
    static const int lens[] = { 1024, 1000, 77, 1 };
    static const int runs[] = { 1, 2, 7, 17, 64, 65, 257, 512, 1023, 1024 };
    static const uint8_t kids[][2] = {
        { 0xFF, 0xFF }, { RAY_ENC_RAW, RAY_ENC_RAW }, { RAY_ENC_RAW, RAY_ENC_FOR_BP }, { RAY_ENC_RAW, RAY_ENC_DELTA_BP },
        { RAY_ENC_FOR_BP, RAY_ENC_RAW }, { RAY_ENC_FOR_BP, RAY_ENC_FOR_BP }, { RAY_ENC_FOR_BP, RAY_ENC_DELTA_BP },
    };
    static uint64_t vals[RAY_ENC_VEC], ends[RAY_ENC_VEC], rows[RAY_ENC_VEC];
    char what[160];
    long cases = 0;
    g_seed = 0x17b;
    for (size_t ti = 0; ti < sizeof types / sizeof types[0]; ti++) {
        const int8_t type = types[ti];
        ray_enc_kind_t k; TEST_ASSERT_TRUE(ray_enc_kind_of(type, 0, &k));
        for (size_t li = 0; li < sizeof lens / sizeof lens[0]; li++) {
            const int n = lens[li];
            for (size_t ri = 0; ri < sizeof runs / sizeof runs[0]; ri++) {
                const int nr = runs[ri];
                if (nr > n) continue;
                rk_vals(k, false, nr, ri & 1, vals);
                rk_ends(n, nr, ends);
                for (int i = 0, p = 0; i < nr; p = (int)ends[i++])
                    for (int r = p; r < (int)ends[i]; r++) rows[r] = vals[i];
                for (size_t c = 0; c < sizeof kids / sizeof kids[0]; c++) {
                    ray_enc_buf_t b = { 0 }; ray_enc_choice_t ch;
                    bool ok = kids[c][0] == 0xFF ? ray_enc_write_as(RAY_ENC_RLE, rows, n, k, NULL, &b, &ch)
                                                 : rk_rle(&b, k, vals, ends, nr, kids[c][0], kids[c][1]);
                    TEST_ASSERT_FMT(ok, "type %d: build", type);
                    snprintf(what, sizeof what, "RLE type %d n %d nr %d kids %u/%u", type, n, nr, kids[c][0], kids[c][1]);
                    test_result_t r = point_case(type, RAY_ENC_RLE, NULL, 0, 0, b.p, b.len, n, what);
                    ray_enc_buf_free(&b);
                    if (r.status != TEST_PASS) return r;
                    cases++;
                }
                if (nr == 1) {
                    ray_enc_buf_t b = { 0 }; ray_enc_choice_t ch;
                    TEST_ASSERT_TRUE(ray_enc_write_as(RAY_ENC_CONST, rows, n, k, NULL, &b, &ch));
                    snprintf(what, sizeof what, "CONST type %d n %d", type, n);
                    test_result_t r = point_case(type, RAY_ENC_CONST, NULL, 0, 0, b.p, b.len, n, what);
                    ray_enc_buf_free(&b);
                    if (r.status != TEST_PASS) return r;
                }
            }
        }
    }
    TEST_ASSERT_TRUE(cases > 1000);
    /* I32 runs 10, 20, 30 (rows 0..99, 100..599, 600..1023) */
    const ray_enc_kind_t k32 = { 32, true, false };
    const uint64_t v3[3] = { 10, 20, 30 }, e3[3] = { 100, 600, 1024 };
    ray_enc_buf_t b = { 0 };
    TEST_ASSERT_TRUE(rk_rle(&b, k32, v3, e3, 3, RAY_ENC_RAW, RAY_ENC_RAW));
    static int64_t pv[RAY_ENC_VEC]; static int8_t res[RAY_ENC_VEC];
    TEST_ASSERT_EQ_I(ray_enccol_test_point_body(RAY_I32, NULL, 0, RAY_ENC_RLE, 0, 0, b.p, b.len, RAY_ENC_VEC, pv, res), 0);
    for (int i = 0; i < RAY_ENC_VEC; i++)
        TEST_ASSERT_FMT(res[i] == 1 && pv[i] == (i < 100 ? 10 : i < 600 ? 20 : 30), "row %d", i);
    /* a run vector with nulls declines every row (the bitmap decides) */
    TEST_ASSERT_EQ_I(ray_enccol_test_point_body(RAY_I32, NULL, 0, RAY_ENC_RLE, 0, 1, b.p, b.len, RAY_ENC_VEC, pv, res), 0);
    for (int i = 0; i < RAY_ENC_VEC; i++) TEST_ASSERT_FMT(res[i] == 0, "nulls: row %d %d", i, res[i]);
    /* a descriptor width on a run body declines */
    TEST_ASSERT_EQ_I(ray_enccol_test_point_body(RAY_I32, NULL, 0, RAY_ENC_RLE, 3, 0, b.p, b.len, RAY_ENC_VEC, pv, res), 0);
    for (int i = 0; i < RAY_ENC_VEC; i++) TEST_ASSERT_EQ_I(res[i], 0);
    ray_enc_buf_free(&b);
    PASS();
}

/* A point read never serves a row the decode would refuse: on nulls, the
 * sentinel, any other codec, and every corruption it reads, it declines. */
static test_result_t test_point_read_declines(void) {
    static uint64_t v[RAY_ENC_VEC], bodyw[RAY_ENC_VEC / 2 + 4];
    static int64_t pv[RAY_ENC_VEC]; static int8_t res[RAY_ENC_VEC];
    static uint8_t ref[RAY_ENC_VEC * 8];
    ray_enc_kind_t k32 = { 32, true, false };
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = (uint64_t)(uint32_t)(int32_t)(i % 300 - 100);
    ray_enc_buf_t b = { 0 }; ray_enc_choice_t ch;
    TEST_ASSERT_TRUE(ray_enc_write_as(RAY_ENC_FOR_BP, v, RAY_ENC_VEC, k32, NULL, &b, &ch));
    /* nulls in the vector: the bitmap decides */
    TEST_ASSERT_EQ_I(ray_enccol_test_point_body(RAY_I32, NULL, 0, RAY_ENC_FOR_BP, ch.width, 3, b.p, b.len, RAY_ENC_VEC, pv, res), 0);
    for (int i = 0; i < RAY_ENC_VEC; i++) TEST_ASSERT_EQ_I(res[i], 0);
    ray_enc_buf_free(&b);
    /* RAW and DELTA_BP decline; CONST and RLE are served (Task 17b) */
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = (uint64_t)(uint32_t)(int32_t)(i / 100 * 3 - 7);
    static const uint8_t others[] = { RAY_ENC_RAW, RAY_ENC_CONST, RAY_ENC_DELTA_BP, RAY_ENC_RLE };
    for (size_t o = 0; o < sizeof others; o++) {
        if (others[o] == RAY_ENC_CONST) for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = 5;
        TEST_ASSERT_TRUE(ray_enc_write_as(others[o], v, RAY_ENC_VEC, k32, NULL, &b, &ch));
        TEST_ASSERT_EQ_I(ray_enccol_test_decode_body(RAY_I32, 0, others[o], ch.width, 0, NULL, b.p, b.len, RAY_ENC_VEC, false, 0, ref), 1);
        TEST_ASSERT_EQ_I(ray_enccol_test_point_body(RAY_I32, NULL, 0, others[o], ch.width, 0, b.p, b.len, RAY_ENC_VEC, pv, res), 0);
        const bool runs = others[o] == RAY_ENC_CONST || others[o] == RAY_ENC_RLE;
        for (int i = 0; i < RAY_ENC_VEC; i++) {
            TEST_ASSERT_FMT(res[i] == (runs ? 1 : 0), "codec %u row %d: %d", others[o], i, res[i]);
            TEST_ASSERT_FMT(!runs || pv[i] == ((const int32_t*)ref)[i], "codec %u row %d", others[o], i);
        }
        ray_enc_buf_free(&b);
    }
    /* SYM and the float types are never point-read */
    TEST_ASSERT_TRUE(ray_enc_write_as(RAY_ENC_FOR_BP, v, RAY_ENC_VEC, k32, NULL, &b, &ch));
    TEST_ASSERT_EQ_I(ray_enccol_test_point_body(RAY_SYM, NULL, 0, RAY_ENC_FOR_BP, ch.width, 0, b.p, b.len, 8, pv, res), 0);
    for (int i = 0; i < 8; i++) TEST_ASSERT_EQ_I(res[i], 0);
    ray_enc_buf_free(&b);
    static const uint64_t dv3[3] = { 10, 20, 30 };
    /* a RAW code child is sound, but not a body the point read takes */
    {
        uint8_t* p = (uint8_t*)bodyw;
        memset(p, 0, 8 + RAY_ENC_VEC * 4);
        uint32_t bb = RAY_ENC_VEC * 4;
        p[0] = RAY_ENC_RAW; memcpy(p + 4, &bb, 4);
        for (int i = 0; i < RAY_ENC_VEC; i++) { uint32_t c = (uint32_t)(i % 3); memcpy(p + 8 + 4 * i, &c, 4); }
        TEST_ASSERT_EQ_I(ray_enccol_test_decode_dict(RAY_I64, dv3, 3, 0, NULL, p, 8 + bb, RAY_ENC_VEC, false, ref), 1);
        TEST_ASSERT_EQ_I(ray_enccol_test_point_body(RAY_I64, dv3, 3, RAY_ENC_DICT, 0, 0, p, 8 + bb, RAY_ENC_VEC, pv, res), 0);
        for (int i = 0; i < RAY_ENC_VEC; i++) TEST_ASSERT_EQ_I(res[i], 0);
    }
    PASS();
}

/* Through a loaded column: every row of a FOR_BP, DICT, CONST or RLE vector
 * without nulls is served and equals the materialization; vectors with
 * nulls and other codecs decline; nothing is decoded; a materialized column
 * declines (its cursor reads it in place). */
static test_result_t test_point_read_column(void) {
    static const int64_t big[4] = { -900000000001ll, 11, 77, 123456789012345ll };
    const int64_t n = 70000;
    ray_t* a = ray_vec_new(RAY_I32, n); a->len = n;
    ray_t* d = ray_vec_new(RAY_I64, n); d->len = n;
    for (int64_t i = 0; i < n; i++) {
        ((int32_t*)ray_data(a))[i] = (i / 1024) % 5 == 3 && i % 97 == 0 ? NULL_I32 : (int32_t)((i * 7919) % 100000 - 50000);
        ((int64_t*)ray_data(d))[i] = big[(uint32_t)((uint32_t)i * 2654435761u) >> 30];
    }
    a->attrs |= RAY_ATTR_HAS_NULLS;
    /* runs (Task 17b): RLE vectors of runs of 100, CONST vectors (from row
     * 20480), a null in every eleventh vector */
    ray_t* q = ray_vec_new(RAY_I16, n); q->len = n;
    for (int64_t i = 0; i < n; i++)
        ((int16_t*)ray_data(q))[i] = (i / 1024) % 11 == 4 && i % 1024 == 9 ? NULL_I16
                                   : i >= 20480 && i < 40960 ? (int16_t)-3 : (int16_t)((i / 100) % 7 - 2);
    q->attrs |= RAY_ATTR_HAS_NULLS;
    ray_t* src[3] = { a, d, q };
    const char* names[3] = { ENCCOL_TMP "/pa", ENCCOL_TMP "/pd", ENCCOL_TMP "/pq" };
    for (int c = 0; c < 3; c++) {
        TEST_ASSERT_EQ_I(ray_col_save_gen(src[c], ray_test_tmp(names[c]), 2), RAY_OK);
        ray_t* col = ray_col_mmap(ray_test_tmp(names[c]));
        TEST_ASSERT_TRUE(col && !RAY_IS_ERR(col));
        ray_enccol_t* e = ray_enccol_of(col);
        ray_col_src_t s;
        TEST_ASSERT_NULL(ray_col_src_bind(col, &s));
        ray_enc_counters_reset();
        int64_t served = 0, declined = 0;
        for (int64_t r = 0; r < n; r++) {
            int64_t x = 0; ray_t* err = NULL;
            int pr = ray_col_src_point_i64(&s, r, &x, &err);
            TEST_ASSERT_FMT(pr >= 0 && !err, "col %d row %lld: %d", c, (long long)r, pr);
            const ray_enc_desc_t dd = ray_enccol_descs(e, r / RAY_ENC_CHUNK)[r % RAY_ENC_CHUNK / RAY_ENC_VEC];
            bool want = !dd.n_nulls && (dd.encoding == RAY_ENC_FOR_BP || dd.encoding == RAY_ENC_DICT ||
                                        dd.encoding == RAY_ENC_CONST || dd.encoding == RAY_ENC_RLE);
            TEST_ASSERT_FMT(pr == (want ? 1 : 0), "col %d row %lld: %d, encoding %u nulls %u", c, (long long)r, pr,
                            dd.encoding, dd.n_nulls);
            if (pr == 1) {
                served++;
                TEST_ASSERT_FMT(x == gen0_at(src[c]->type, ray_data(src[c]), (int)r), "col %d row %lld", c, (long long)r);
            } else declined++;
        }
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        TEST_ASSERT_EQ_U(k.decoded_bytes, 0);
        TEST_ASSERT_TRUE(served > n / 2);
        if (c == 0) TEST_ASSERT_TRUE(declined >= 13 * 1024);    /* the vectors with nulls, the RAW tail */
        if (c == 2) {                                           /* every RLE and CONST vector without nulls */
            int64_t runs = 0;
            for (int64_t v = 0; v * RAY_ENC_VEC < n; v++) {
                const ray_enc_desc_t dd = ray_enccol_descs(e, v * RAY_ENC_VEC / RAY_ENC_CHUNK)[v % RAY_ENC_VECS_PER_CHUNK];
                runs += !dd.n_nulls && (dd.encoding == RAY_ENC_RLE || dd.encoding == RAY_ENC_CONST);
                TEST_ASSERT_TRUE(dd.encoding == RAY_ENC_RLE || dd.encoding == RAY_ENC_CONST);
            }
            TEST_ASSERT_TRUE(runs >= 50);
        }
        ray_t* m = ray_enccol_materialize(col);
        TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
        int64_t x; ray_t* err = NULL;
        TEST_ASSERT_EQ_I(ray_col_src_point_i64(&s, 5, &x, &err), 0);
        ray_col_src_release(&s);
        ray_release(col);
    }
    ray_release(a); ray_release(d); ray_release(q);
    PASS();
}

/* The scratch references the column it counts for: freeing it after its
 * source and column are released is safe and still counts (ASan). */
static test_result_t test_scratch_outlives_source(void) {
    const char* p = ray_test_tmp(ENCCOL_TMP "/so");
    TEST_ASSERT_EQ_I(save_i32_rows(p, 5000), RAY_OK);
    ray_t* c = ray_col_mmap(p);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(c, &s));
    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
    ray_col_range_t r;
    ray_enc_counters_reset();
    int64_t live = ray_enccol_test_live();
    TEST_ASSERT_NULL(ray_col_range(&s, 1024, 1024, &sc, &r));
    ray_col_src_release(&s);
    ray_release(c);                                   /* the scratch's reference keeps the column */
    TEST_ASSERT_EQ_I(ray_enccol_test_live(), live);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 0);             /* not counted yet */
    ray_col_scratch_free(&sc);                        /* counts, then drops the last reference */
    ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 1024 * 4);
    TEST_ASSERT_EQ_I(ray_enccol_test_live(), live - 1);
    PASS();
}

/* Concurrent first touch: eight threads range-read every vector of the same
 * unvalidated chunk at once, each in its own order; every answer is right
 * and the chunk ends validated. */
typedef struct { ray_col_src_t* s; int seed; int bad; } ftouch_t;
/* ---- hardening: crafted input never reads out of bounds, never half-writes ---- */

#define CANARY 0xA5

static test_result_t test_payload_is_tail(void) {
    /* force -1: selection.  P5: a float column selects DICT, whose
     * VALUE_DICT follows the payload; the float cases are forced to the
     * float bodies without side data, so the payload stays the tail.
     * P6: the same for STR, whose STR_DICT / FSST entries follow the
     * payload; the side data being absent is this test's subject, so the
     * STR cases are forced RAW. */
    static const struct { int8_t type; int s; int force; } cases[] = {
        { RAY_I32, 1, -1 }, { RAY_I64, 3, -1 }, { RAY_I16, 4, -1 }, { RAY_U8, 0, -1 }, { RAY_BOOL, 2, -1 },
        { RAY_F64, 0, RAY_ENC_RAW }, { RAY_F32, 4, RAY_ENC_RAW }, { RAY_F64, 0, RAY_ENC_RLE },
        { RAY_F64, 0, RAY_ENC_ALP }, { RAY_F32, 4, RAY_ENC_ALP }, { RAY_F64, 4, RAY_ENC_ALP_RD },
        { RAY_GUID, 0, -1 }, { RAY_STR, 0, RAY_ENC_RAW }, { RAY_STR, 3, RAY_ENC_RAW }, { RAY_SYM, 1, -1 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        ray_t* v = make_col(cases[i].type, 1000, cases[i].s);
        ray_encw_test_force(cases[i].force);
        ray_err_t se = ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/tb"), 2);
        ray_encw_test_force(-1);
        ray_release(v);
        TEST_ASSERT_EQ_I(se, RAY_OK);
        ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/tb"));
        TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
        TEST_ASSERT_EQ_U(ray_enccol_of(c)->hdr.side_off, 0);     /* the payload is the file's tail */
        if (cases[i].force >= 0) {
            int enc = ray_enccol_descs(ray_enccol_of(c), 0)[0].encoding;
            TEST_ASSERT_FMT(enc == cases[i].force, "type %d shape %d: encoding %d, forced %d",
                            (int)cases[i].type, cases[i].s, enc, cases[i].force);
        }
        ray_release(c);
    }
    PASS();
}

/* A DICT body is refused without a usable dictionary.  The explicit NULL
 * guards in the codec's size / write / read bodies sit behind the
 * legality check every public entry runs first, so they are not reachable
 * directly; this pins the observable behaviour of the public entries. */
static test_result_t test_dict_null_dictionary(void) {
    uint64_t v[16]; uint64_t dv[2] = { 10, 20 };
    for (int i = 0; i < 16; i++) v[i] = dv[i & 1];
    ray_enc_kind_t k; TEST_ASSERT_TRUE(ray_enc_kind_of(RAY_I64, 0, &k));
    ray_enc_dict_t d = { dv, 2 };
    ray_enc_buf_t buf = {0}, buf2 = {0}; ray_enc_choice_t ch;
    bool wrote = ray_enc_write_as(RAY_ENC_DICT, v, 16, k, &d, &buf, &ch);
    bool wrote_null = ray_enc_write_as(RAY_ENC_DICT, v, 16, k, NULL, &buf2, &ch);
    uint64_t out[16];
    int64_t good = wrote ? ray_enc_read(buf.p, buf.len, RAY_ENC_DICT, 0, 16, k, RAY_ENC_SLOT_TOP, 0, &d, out) : -2;
    size_t blen = buf.len;
    ray_enc_buf_free(&buf); ray_enc_buf_free(&buf2);
    TEST_ASSERT_TRUE(wrote);
    TEST_ASSERT_FALSE(wrote_null);
    TEST_ASSERT_EQ_I(good, (int64_t)blen);
    for (int i = 0; i < 16; i++) TEST_ASSERT_EQ_U(out[i], dv[i & 1]);
    PASS();
}

/* Out-of-range vector numbers are refused by every column-global entry. */
static test_result_t test_vec_range_checks(void) {
    ray_t* v = make_col(RAY_STR, 2500, 0);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/vr"), 2), RAY_OK);
    ray_release(v);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/vr"));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_enccol_t* e = ray_enccol_of(c);
    int64_t nv = ray_enc_nvecs(e->len);
    TEST_ASSERT_EQ_I(nv, 3);
    static const int64_t bad[] = { -1, 3, 64, 1000000 };
    int bad_ok = 1;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        uint64_t pb = 7;
        ray_t* err = ray_enccol_vec_pool_bytes(e, bad[i], &pb);
        if (!err || strcmp(ray_err_code(err), "range") != 0 || pb != 0) bad_ok = 0;
        if (err) ray_error_free(err);
    }
    uint64_t pb;
    ray_t* e0 = ray_enccol_vec_pool_bytes(e, nv - 1, &pb);
    int good_ok = e0 == NULL;
    if (e0) ray_error_free(e0);
    ray_release(c);
    TEST_ASSERT_TRUE(bad_ok);
    TEST_ASSERT_TRUE(good_ok);
    PASS();
}

/* ---- Task 8: materialization ---- */

/* Same vector saved as generation 0 and 2: the generation-0 load and the
 * materialized generation-2 column are equal. */
static test_result_t check_equivalence(ray_t* v) {
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/g0"), 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/g2"), 2), RAY_OK);
    ray_t* a = ray_col_load(ray_test_tmp(ENCCOL_TMP "/g0"));
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/g2"));
    TEST_ASSERT_TRUE(a && !RAY_IS_ERR(a) && c && !RAY_IS_ERR(c));
    ray_t* b = ray_enccol_materialize(c);
    TEST_ASSERT_FMT(b && !RAY_IS_ERR(b), "materialize: %s", b ? ray_err_code(b) : "null");
    TEST_ASSERT_EQ_I(b->type, a->type);
    TEST_ASSERT_EQ_I(b->len, a->len);
    TEST_ASSERT_EQ_I(b->attrs & COL_DISK_ATTRS_MASK, a->attrs & COL_DISK_ATTRS_MASK);
    if (a->type == RAY_STR) {
        const ray_str_t* x = ray_data(a); const ray_str_t* y = ray_data(b);
        const char* px = a->str_pool ? ray_data(a->str_pool) : NULL;
        const char* py = b->str_pool ? ray_data(b->str_pool) : NULL;
        for (int64_t i = 0; i < a->len; i++) {
            TEST_ASSERT_FMT(x[i].len == y[i].len, "len row %lld", (long long)i);
            TEST_ASSERT_FMT(memcmp(ray_str_t_ptr(&x[i], px), ray_str_t_ptr(&y[i], py), x[i].len) == 0, "row %lld", (long long)i);
            if (!ray_str_is_inline(&y[i])) TEST_ASSERT_FMT(y[i].hash32 == x[i].hash32, "hash32 row %lld", (long long)i);
        }
    } else {
        TEST_ASSERT_TRUE(memcmp(ray_data(a), ray_data(b), (size_t)a->len * ray_sym_elem_size(a->type, a->attrs)) == 0);
    }
    if (a->type == RAY_SYM) {
        TEST_ASSERT_EQ_I(b->attrs & RAY_SYM_W_MASK, a->attrs & RAY_SYM_W_MASK);   /* width class */
        TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(b), ray_sym_vec_domain(a));        /* domain */
    }
    TEST_ASSERT_EQ_PTR(ray_enccol_materialize(c), b);       /* cached */
    ray_retain(b);
    ray_release(c);                                          /* retained column outlives the object */
    TEST_ASSERT_EQ_I(b->len, a->len);
    ray_release(b);
    ray_release(a);
    PASS();
}

static test_result_t test_materialize_equivalence(void) {
    static const int8_t types[] = { RAY_BOOL, RAY_U8, RAY_I16, RAY_I32, RAY_I64, RAY_DATE, RAY_TIME,
                                    RAY_TIMESTAMP, RAY_F32, RAY_F64, RAY_GUID, RAY_SYM, RAY_STR };
    for (size_t t = 0; t < sizeof types; t++)
        for (int s = 0; s < 6; s++) {
            if ((types[t] == RAY_BOOL || types[t] == RAY_U8) && s >= 4) continue;
            ray_t* v = make_col(types[t], 140000, s);       /* three chunks, parallel decode */
            test_result_t r = check_equivalence(v);
            ray_release(v);
            if (r.status != TEST_PASS) return r;
        }
    PASS();
}

/* Every type with nulls at the boundary lengths; SYM at every width. */
static test_result_t test_materialize_lengths(void) {
    static const int8_t types[] = { RAY_BOOL, RAY_U8, RAY_I16, RAY_I32, RAY_I64, RAY_DATE, RAY_TIME,
                                    RAY_TIMESTAMP, RAY_F32, RAY_F64, RAY_GUID, RAY_SYM, RAY_STR };
    static const int64_t lens[] = { 0, 1, 1023, 1024, 1025, 65535, 65536, 65537 };
    static const uint8_t widths[] = { RAY_SYM_W8, RAY_SYM_W16, RAY_SYM_W32, RAY_SYM_W64 };
    for (size_t t = 0; t < sizeof types; t++)
        for (size_t l = 0; l < sizeof lens / sizeof *lens; l++)
            for (int s = 0; s < 5; s += 4)                   /* shapes 0 (no nulls) and 4 (nulls) */
                for (size_t w = 0; w < (types[t] == RAY_SYM ? 4u : 1u); w++) {
                    g_symw = widths[w];
                    ray_t* v = make_col(types[t], lens[l], s);
                    test_result_t r = check_equivalence(v);
                    ray_release(v);
                    g_symw = RAY_SYM_W16;
                    if (r.status != TEST_PASS) return r;
                }
    PASS();
}

/* Float payloads survive bit for bit: NaN payloads, -0.0, infinities, the null sentinel. */
static test_result_t test_materialize_float_bits(void) {
    static const uint64_t d64[] = { 0x7ff8000000000123ull, 0x7ff0000000000001ull, 0x8000000000000000ull,
                                    0x0000000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull,
                                    0xfff8000000000abcull, 0x3ff0000000000000ull };
    static const uint32_t d32[] = { 0x7fc00123u, 0x7f800001u, 0x80000000u, 0u, 0x7f800000u, 0xff800000u, 0xffc00abcu, 0x3f800000u };
    for (int k = 0; k < 2; k++) {
        int64_t n = 70000;
        ray_t* v = ray_vec_new(k ? RAY_F32 : RAY_F64, n); v->len = n;
        for (int64_t i = 0; i < n; i++) {
            if (k) memcpy((uint8_t*)ray_data(v) + i * 4, &d32[(i / 3) % 8], 4);
            else   memcpy((uint8_t*)ray_data(v) + i * 8, &d64[(i / 3) % 8], 8);
        }
        test_result_t r = check_equivalence(v);
        ray_release(v);
        if (r.status != TEST_PASS) return r;
    }
    PASS();
}

/* ---- Task 25: decoded peek and the in-place column list ---- */

static void peek_thread(void* arg) {
    ray_t* c = arg;
    for (int i = 0; i < 20000; i++) {
        ray_t* d = ray_enccol_decoded_peek(c);
        if (d) (void)d->len;
    }
}
static void peek_mat_thread(void* arg) { (void)ray_enccol_materialize((ray_t*)arg); }

static test_result_t test_decoded_peek(void) {
    ray_t* v = make_col(RAY_I64, 5000, 0);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/pk"), 2), RAY_OK);
    ray_release(v);
    ray_enc_counters_reset();
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/pk"));
    TEST_ASSERT_NULL(ray_enccol_decoded_peek(c));
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);                   /* a peek decodes nothing */
    ray_thread_t th[3];
    ray_thread_create(&th[0], peek_thread, c);
    ray_thread_create(&th[1], peek_thread, c);
    ray_thread_create(&th[2], peek_mat_thread, c);
    for (int i = 0; i < 3; i++) ray_thread_join(th[i]);
    ray_t* m = ray_enccol_materialize(c);
    TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
    TEST_ASSERT_EQ_PTR(ray_enccol_decoded_peek(c), m);
    ray_release(c);
    PASS();
}

static test_result_t test_cols_inplace_encoded(void) {
    ray_t* v = make_col(RAY_I64, 100, 0);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/ci"), 2), RAY_OK);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/ci"));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_t* t = ray_table_new(2);
    t = ray_table_add_col(t, ray_sym_intern("a", 1), v);
    TEST_ASSERT_TRUE(ray_table_cols_inplace(t) != NULL);
    t = ray_table_add_col(t, ray_sym_intern("b", 1), c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(t));
    TEST_ASSERT_NULL(ray_table_cols_inplace(t));           /* an encoded slot refuses */
    ray_release(t); ray_release(c); ray_release(v);
    PASS();
}

static test_result_t test_materialize_counts_once(void) {
    ray_t* v = make_col(RAY_I64, 5000, 0);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/m"), 2), RAY_OK);
    ray_release(v);
    ray_enc_counters_reset();
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/m"));
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);                   /* load decodes nothing */
    TEST_ASSERT_EQ_U(k.decoded_bytes, 0);
    ray_t* a = ray_enccol_materialize(c); ray_t* b = ray_enccol_materialize(c);
    TEST_ASSERT_EQ_PTR(a, b);
    ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 1);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 5000 * 8);
    TEST_ASSERT_EQ_U(atomic_load(&ray_enccol_of(c)->decoded_bytes), 5000 * 8);   /* per column */
    ray_release(c);
    PASS();
}

/* Cancellation is transient: reported, not cached, retried. */
static test_result_t test_cancel_retried(void) {
    ray_t* v = make_col(RAY_I32, 3 * 65536, 0);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/cn"), 2), RAY_OK);
    ray_release(v);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/cn"));
    ray_request_interrupt();
    ray_t* r = ray_enccol_materialize(c);
    ray_clear_interrupt();
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "cancel");
    ray_error_free(r);
    r = ray_enccol_materialize(c);
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    ray_release(c);
    PASS();
}

/* The per-chunk poll: the interrupt arrives after chunk 0 has decoded. */
static test_result_t test_cancel_mid_decode(void) {
    ray_t* v = make_col(RAY_I32, 128 * 65536, 1);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/cm"), 2), RAY_OK);
    ray_release(v);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/cm"));
    ray_enccol_test_interrupt_after(1);
    ray_t* r = ray_enccol_materialize(c);
    ray_enccol_test_interrupt_after(0);
    ray_clear_interrupt();
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "cancel");
    ray_error_free(r);
    TEST_ASSERT_TRUE(atomic_load(&ray_enccol_of(c)->decoded_bytes) < (uint64_t)128 * 65536 * 4);   /* stopped early */
    r = ray_enccol_materialize(c);                          /* retried */
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    ray_release(c);
    PASS();
}

/* A waiter on a cancelled attempt receives the attempt's "cancel". */
typedef struct { ray_t* c; char code[8]; } cwait_t;   /* code: "" on success */
static void cancel_wait_thread(void* raw) {
    cwait_t* t = raw;
    ray_t* r = ray_enccol_materialize(t->c);
    t->code[0] = 0;
    if (r && RAY_IS_ERR(r)) { snprintf(t->code, sizeof t->code, "%s", ray_err_code(r)); ray_error_free(r); }
    ray_heap_abandon();
}
static test_result_t test_cancel_waiter(void) {
    ray_t* v = make_col(RAY_I32, 128 * 65536, 1);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/cw"), 2), RAY_OK);
    ray_release(v);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/cw"));
    ray_enccol_t* e = ray_enccol_of(c);
    cwait_t p = { c, "" }, w = { c, "" };
    ray_thread_t tp, tw;
    ray_thread_create(&tp, cancel_wait_thread, &p);
    while (atomic_load(&e->once.state) != RAY_ENC_ONCE_RUNNING) ray_enc_yield();
    ray_thread_create(&tw, cancel_wait_thread, &w);
    while (ray_enc_once_waiters(&e->once) < 1) ray_enc_yield();
    ray_request_interrupt();
    ray_thread_join(tp); ray_thread_join(tw);
    ray_clear_interrupt();
    TEST_ASSERT_STR_EQ(p.code, "cancel");
    TEST_ASSERT_STR_EQ(w.code, "cancel");
    ray_t* r = ray_enccol_materialize(c);
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    ray_release(c);
    PASS();
}

/* ray_col_load_dom on a generation-2 SYM file: the loaded vector holds its own
 * reference on the symfile domain (owned branch) and outlives the object. */
static test_result_t test_load_dom_refcount(void) {
    const char* p = ray_test_tmp(ENCCOL_TMP "/ld");
    TEST_ASSERT_NULL(sym_col_save(p));
    ray_sym_domain_t* dom = ray_sym_domain_new();
    for (int i = 0; i < 400; i++) { char b[12]; int n = snprintf(b, sizeof b, "d%d", i); (void)ray_sym_domain_intern(dom, b, (size_t)n); }
    ray_t* b = ray_col_load_dom(p, dom);
    TEST_ASSERT_TRUE(b && !RAY_IS_ERR(b));
    TEST_ASSERT_EQ_I(b->type, RAY_SYM);
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(b), dom);
    ray_sym_domain_release(dom);                            /* ours; the vector keeps the domain alive */
    ray_t* s = ray_sym_domain_str(ray_sym_vec_domain(b), 1);
    TEST_ASSERT_TRUE(s != NULL);
    ray_release(b);                                         /* last reference: frees the domain (ASan checks) */
    PASS();
}

static test_result_t test_oom_then_retry(void) {
    ray_t* v = make_col(RAY_STR, 3000, 2);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/o"), 2), RAY_OK);
    ray_release(v);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/o"));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    /* P6: the column selects STR_DICT, whose validated dictionary is a
     * once-state the column keeps after the first successful validation
     * (a cache, not a leak).  Validate it before the baseline so the
     * no-leak check counts only what one materialization attempt owns. */
    const ray_enc_sdict_t* sd = NULL;
    ray_t* se = ray_enccol_strdict(ray_enccol_of(c), &sd);
    TEST_ASSERT_NULL(se);
    ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
    bool done = false;
    for (int nth = 1; nth <= 16 && !done; nth++) {          /* until an attempt passes every injection point */
        ray_enccol_test_fail_alloc(nth);
        ray_t* r = ray_enccol_materialize(c);
        ray_enccol_test_fail_alloc(0);
        if (r && RAY_IS_ERR(r)) {
            TEST_ASSERT_STR_EQ(ray_err_code(r), "oom");
            ray_error_free(r);
            ray_mem_stats(&m1);
            TEST_ASSERT_FMT(m1.bytes_allocated == m0.bytes_allocated, "nth %d: %llu vs %llu", nth, (unsigned long long)m1.bytes_allocated, (unsigned long long)m0.bytes_allocated);   /* a failed attempt freed everything */
        } else done = true;
    }
    TEST_ASSERT_TRUE(done);
    ray_t* r = ray_enccol_materialize(c);                   /* the success is cached */
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    ray_release(c);
    PASS();
}

static test_result_t test_col_load_materializes(void) {
    ray_t* v = make_col(RAY_F64, 2000, 4);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/l"), 2), RAY_OK);
    ray_t* b = ray_col_load(ray_test_tmp(ENCCOL_TMP "/l"));
    TEST_ASSERT_TRUE(b && !RAY_IS_ERR(b));
    TEST_ASSERT_EQ_I(b->type, RAY_F64);
    TEST_ASSERT_TRUE(memcmp(ray_data(b), ray_data(v), 2000 * 8) == 0);
    ray_release(b); ray_release(v);
    PASS();
}

static ray_t* save_and_materialize(ray_t* v, const char* path, ray_t** keep) {
    if (ray_col_save_gen(v, path, 2) != RAY_OK) return ray_error("io", NULL);
    *keep = ray_col_mmap(path);
    if (!*keep || RAY_IS_ERR(*keep)) return *keep;
    return ray_enccol_materialize(*keep);
}

/* Overwrite rows [from, to) of an 8- or 4-byte element column with its null. */
static void null_rows(ray_t* v, int64_t from, int64_t to) {
    uint8_t* d = ray_data(v);
    for (int64_t i = from; i < to; i++)
        switch (v->type) {
        case RAY_I32: case RAY_DATE: case RAY_TIME: { int32_t x = NULL_I32; memcpy(d + i * 4, &x, 4); break; }
        case RAY_I64: case RAY_TIMESTAMP:           { int64_t x = NULL_I64; memcpy(d + i * 8, &x, 8); break; }
        case RAY_F64:                               { double x = NULL_F64;  memcpy(d + i * 8, &x, 8); break; }
        default: break;
        }
}

static test_result_t test_synth_zone(void) {
    static const int8_t types[] = { RAY_I32, RAY_I64, RAY_F64 };
    for (size_t t = 0; t < 3; t++) {
        int64_t n = 2 * 65536 + 5000;                      /* three chunks */
        ray_t* v = make_col(types[t], n, 4);
        if (types[t] == RAY_F64) {                         /* -0.0 / +0.0 tie across a chunk boundary */
            double* f = ray_data(v);
            for (int64_t i = 0; i < n; i++) if (f[i] == 0.0 || f[i] < 0.0) f[i] = 1.0;
            f[65535] = 0.0; f[65536] = -0.0; f[10] = 5.0;
            f[2 * 65536 - 1] = -0.0; f[2 * 65536] = 0.0;
            for (int64_t i = 0; i < n; i++) if (i != 65535 && i != 65536 && i != 2 * 65536 - 1 && i != 2 * 65536 && f[i] <= 0.0) f[i] = 1.0;
            for (int64_t i = 0; i < 65535; i += 7) f[i] = 2.0;
        }
        ray_t* z = ray_index_attach_zone(&v);
        TEST_ASSERT_TRUE(z && !RAY_IS_ERR(z));
        v = z;
        ray_t* keep = NULL;
        ray_t* m = save_and_materialize(v, ray_test_tmp(ENCCOL_TMP "/z"), &keep);
        TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
        TEST_ASSERT_EQ_I(ray_index_kind(m), RAY_IDX_ZONE);
        ray_index_t* a = ray_index_payload(v->index);
        ray_index_t* b = ray_index_payload(m->index);
        if (types[t] == RAY_F64) {
            TEST_ASSERT_TRUE(memcmp(&b->u.zone.min_f, &a->u.zone.min_f, 8) == 0);   /* bit-equal: the sign of a tie */
            TEST_ASSERT_TRUE(memcmp(&b->u.zone.max_f, &a->u.zone.max_f, 8) == 0);
        } else {
            TEST_ASSERT_EQ_I(b->u.zone.min_i, a->u.zone.min_i);
            TEST_ASSERT_EQ_I(b->u.zone.max_i, a->u.zone.max_i);
        }
        TEST_ASSERT_EQ_I(b->u.zone.n_nulls, a->u.zone.n_nulls);
        TEST_ASSERT_EQ_I(b->built_for_len, n);
        ray_release(keep); ray_release(v);
    }
    PASS();
}

/* The synthesized chunk zone's arrays equal the builder's, aggs only when
 * the type is summable (NULL otherwise). */
static test_result_t chunk_zone_equal(ray_t* b_idx, ray_t* a_idx, bool summable) {
    ray_index_t* a = ray_index_payload(a_idx);
    ray_index_t* b = ray_index_payload(b_idx);
    uint32_t nc = a->u.chunk_zone.n_chunks;
    TEST_ASSERT_EQ_I(b->u.chunk_zone.n_chunks, nc);
    TEST_ASSERT_EQ_I(b->u.chunk_zone.chunk_log2, a->u.chunk_zone.chunk_log2);
    TEST_ASSERT_EQ_I(b->u.chunk_zone.is_f64, a->u.chunk_zone.is_f64);
    TEST_ASSERT_TRUE(memcmp(ray_data(b->u.chunk_zone.mins), ray_data(a->u.chunk_zone.mins), 8 * nc) == 0);
    TEST_ASSERT_TRUE(memcmp(ray_data(b->u.chunk_zone.maxs), ray_data(a->u.chunk_zone.maxs), 8 * nc) == 0);
    TEST_ASSERT_TRUE(memcmp(ray_data(b->u.chunk_zone.null_bits), ray_data(a->u.chunk_zone.null_bits), (nc + 7) / 8) == 0);
    if (summable) {
        TEST_ASSERT_TRUE(b->u.chunk_zone.aggs && a->u.chunk_zone.aggs);
        TEST_ASSERT_TRUE(memcmp(ray_data(b->u.chunk_zone.aggs), ray_data(a->u.chunk_zone.aggs), 8 * 3 * nc) == 0);
    } else {
        TEST_ASSERT_TRUE(b->u.chunk_zone.aggs == NULL);
    }
    PASS();
}

/* CHUNK_ZONE synthesized at 64K chunks equals the builder's on the
 * decoded vector, for integer, float and temporal columns with an all-null chunk. */
static test_result_t test_synth_chunk_zone(void) {
    static const int8_t types[] = { RAY_I64, RAY_F64, RAY_DATE, RAY_TIMESTAMP };
    for (size_t t = 0; t < 4; t++) {
        int64_t n = 3 * 65536 + 17;
        ray_t* v = make_col(types[t], n, 4);
        null_rows(v, 65536, 2 * 65536);                           /* chunk 1 all null */
        ray_t* z = ray_index_attach_chunk_zone(&v, 16);
        TEST_ASSERT_TRUE(z && !RAY_IS_ERR(z)); v = z;
        ray_t* keep = NULL;
        ray_t* m = save_and_materialize(v, ray_test_tmp(ENCCOL_TMP "/cz"), &keep);
        TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
        TEST_ASSERT_EQ_I(ray_index_kind(m), RAY_IDX_CHUNK_ZONE);
        ray_t* ref = ray_index_chunk_zone_compute(m, 16);         /* builder on the decoded data */
        TEST_ASSERT_TRUE(ref && !RAY_IS_ERR(ref));
        test_result_t r = chunk_zone_equal(m->index, ref, ray_encw_type_summable(types[t]));
        if (r.status != TEST_PASS) return r;
        ray_release(ref); ray_release(keep); ray_release(v);
    }
    PASS();
}

static test_result_t test_unique_flag(void) {
    int64_t d[100]; for (int i = 0; i < 100; i++) d[i] = i * 3;
    ray_t* v = ray_vec_from_raw(RAY_I64, d, 100);
    ray_t* root = ray_index_new_root(RAY_IDX_NONE, RAY_I64, 100);
    ray_index_payload(root)->markers = RAY_MARK_UNIQUE;
    ray_t* a = ray_index_attach_persisted(&v, root);
    TEST_ASSERT_TRUE(a && !RAY_IS_ERR(a)); v = a;
    ray_t* keep = NULL;
    ray_t* m = save_and_materialize(v, ray_test_tmp(ENCCOL_TMP "/u"), &keep);
    TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
    ray_enccol_t* e = ray_enccol_of(keep);
    TEST_ASSERT_EQ_I(e->hdr.col_flags, RAY_ENC_FLAG_UNIQUE);
    TEST_ASSERT_EQ_I(e->hdr.index_kind, 0);
    TEST_ASSERT_TRUE(ray_index_has(m));
    TEST_ASSERT_EQ_I(ray_index_kind(m), RAY_IDX_NONE);
    TEST_ASSERT_TRUE(ray_index_payload(m->index)->markers & RAY_MARK_UNIQUE);
    ray_release(keep); ray_release(v);
    PASS();
}

/* The matrix refuses a pair: on a file (GUID claiming UNIQUE) at load; on
 * the attach itself the root is consumed exactly once (ASan would report a
 * double release or a leak). */
static test_result_t test_attach_refused(void) {
    ray_t* v = make_col(RAY_GUID, 50, 0);
    ray_t* root = ray_index_new_root(RAY_IDX_ZONE, RAY_GUID, v->len);
    ray_t* r = ray_index_attach_persisted(&v, root);
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
    ray_error_free(r);
    TEST_ASSERT_FALSE(ray_index_has(v));
    ray_release(v);
    PASS();
}


/* A gen-2 load carries the same attachment a gen-0 load of the same vector
 * with the same index does: ZONE and CHUNK_ZONE (integer, float, temporal;
 * several chunks, nulls, an all-null chunk) plus UNIQUE on top.  The gen-0
 * side is mmap'd: ray_col_load drops the index. */
static test_result_t test_synth_matches_gen0(void) {
    static const int8_t types[] = { RAY_I64, RAY_F64, RAY_DATE, RAY_TIMESTAMP };
    for (size_t t = 0; t < 4; t++)
    for (int k = 0; k < 2; k++) {
        int64_t n = 2 * 65536 + 5;
        ray_t* v = make_col(types[t], n, 4);
        null_rows(v, 65536, 2 * 65536);
        ray_t* z = k ? ray_index_attach_chunk_zone(&v, 16) : ray_index_attach_zone(&v);
        TEST_ASSERT_TRUE(z && !RAY_IS_ERR(z)); v = z;
        ray_index_payload(v->index)->markers |= RAY_MARK_UNIQUE;
        TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/g0"), 0), RAY_OK);
        TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/g2"), 2), RAY_OK);
        ray_t* a = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/g0"));
        ray_t* b = ray_col_load(ray_test_tmp(ENCCOL_TMP "/g2"));
        TEST_ASSERT_TRUE(a && !RAY_IS_ERR(a) && b && !RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(ray_index_kind(b), ray_index_kind(a));
        TEST_ASSERT_EQ_I(ray_index_kind(b), k ? RAY_IDX_CHUNK_ZONE : RAY_IDX_ZONE);
        ray_index_t* ia = ray_index_payload(a->index);
        ray_index_t* ib = ray_index_payload(b->index);
        TEST_ASSERT_EQ_I(ib->markers & RAY_MARK_UNIQUE, ia->markers & RAY_MARK_UNIQUE);   /* MMAP is residency */
        TEST_ASSERT_EQ_I(ib->built_for_len, ia->built_for_len);
        if (k) {
            /* gen-0 stores the builder's aggs for every integer type; gen 2 only where the directory has sums */
            bool sm = ray_encw_type_summable(types[t]);
            uint32_t nc = ia->u.chunk_zone.n_chunks;
            TEST_ASSERT_EQ_I(ib->u.chunk_zone.n_chunks, nc);
            TEST_ASSERT_TRUE(memcmp(ray_data(ib->u.chunk_zone.mins), ray_data(ia->u.chunk_zone.mins), 8 * nc) == 0);
            TEST_ASSERT_TRUE(memcmp(ray_data(ib->u.chunk_zone.maxs), ray_data(ia->u.chunk_zone.maxs), 8 * nc) == 0);
            TEST_ASSERT_TRUE(memcmp(ray_data(ib->u.chunk_zone.null_bits), ray_data(ia->u.chunk_zone.null_bits), (nc + 7) / 8) == 0);
            if (sm) TEST_ASSERT_TRUE(memcmp(ray_data(ib->u.chunk_zone.aggs), ray_data(ia->u.chunk_zone.aggs), 8 * 3 * nc) == 0);
            else    TEST_ASSERT_TRUE(ib->u.chunk_zone.aggs == NULL);
        } else if (types[t] == RAY_F64) {
            TEST_ASSERT_TRUE(memcmp(&ib->u.zone.min_f, &ia->u.zone.min_f, 8) == 0);
            TEST_ASSERT_TRUE(memcmp(&ib->u.zone.max_f, &ia->u.zone.max_f, 8) == 0);
        } else {
            TEST_ASSERT_EQ_I(ib->u.zone.min_i, ia->u.zone.min_i);
            TEST_ASSERT_EQ_I(ib->u.zone.max_i, ia->u.zone.max_i);
        }
        TEST_ASSERT_EQ_I(ib->u.zone.n_nulls * !k, ia->u.zone.n_nulls * !k);
        ray_release(a); ray_release(b); ray_release(v);
    }
    PASS();
}

/* Write a table as a legacy splayed directory whose columns are generation 2. */
static ray_err_t write_splayed_gen2(const char* dir, ray_t* tbl) {
    if (ray_test_mkdir_p(dir) != 0) return RAY_ERR_IO;
    int64_t nc = ray_table_ncols(tbl);
    ray_t* names = ray_vec_new(RAY_STR, nc);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* nm = ray_sym_str(ray_table_col_name(tbl, c));
        char p[512]; snprintf(p, sizeof p, "%s/%.*s", dir, (int)ray_str_len(nm), ray_str_ptr(nm));
        ray_err_t e = ray_col_save_gen(ray_table_get_col_idx(tbl, c), p, 2);
        if (e != RAY_OK) { ray_release(names); return e; }
        names = ray_str_vec_append(names, ray_str_ptr(nm), ray_str_len(nm));
    }
    char p[512]; snprintf(p, sizeof p, "%s/.d", dir);
    ray_err_t e = ray_col_save(names, p);
    ray_release(names);
    return e;
}

static ray_t* tbl2(int64_t n) {
    ray_t* t = ray_table_new(2);
    ray_t* a = make_col(RAY_I64, n, 0);
    ray_t* b = make_col(RAY_F64, n, 4);
    t = ray_table_add_col(t, ray_sym_intern("a", 1), a);
    t = ray_table_add_col(t, ray_sym_intern("b", 1), b);
    ray_release(a); ray_release(b);
    return t;
}

/* Tables are built directly from mmap'd generation-2 columns, then loaded
 * through the splayed loader, whose columns equal the generation-0 table. */
static test_result_t test_table_core_enccol(void) {
    ray_t* t = tbl2(70000);
    TEST_ASSERT_EQ_I(write_splayed_gen2(ray_test_tmp(ENCCOL_TMP "/t"), t), RAY_OK);   /* t: the generation-0 reference */
    ray_enc_counters_reset();
    ray_t* ca = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/t/a"));
    ray_t* cb = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/t/b"));
    TEST_ASSERT_FMT(ca && !RAY_IS_ERR(ca) && cb && !RAY_IS_ERR(cb), "mmap");
    TEST_ASSERT_EQ_I(ca->type, RAY_ENCCOL);
    TEST_ASSERT_EQ_I(ray_table_col_nrows(ca), 70000);
    ray_t* s = ray_table_new(2);
    s = ray_table_add_col(s, ray_sym_intern("a", 1), ca);
    s = ray_table_add_col(s, ray_sym_intern("b", 1), cb);
    TEST_ASSERT_FMT(s && !RAY_IS_ERR(s), "add_col: %s", s ? ray_err_code(s) : "null");
    ray_release(ca); ray_release(cb);
    TEST_ASSERT_EQ_I(ray_table_nrows(s), 70000);
    TEST_ASSERT_NULL(ray_table_validate_rectangular(s, "test"));
    TEST_ASSERT_TRUE(ray_table_get_col_enc_idx(s, 1) != NULL);
    /* a shorter plain column makes the table ragged */
    ray_t* shortc = make_col(RAY_I32, 10, 0);
    ray_retain(s);
    ray_t* r = ray_table_add_col(s, ray_sym_intern("c", 1), shortc);
    ray_t* e = ray_table_validate_rectangular(r, "test");
    TEST_ASSERT_TRUE(e && RAY_IS_ERR(e));
    ray_error_free(e);
    ray_release(r); ray_release(shortc);
    /* copy-on-write shares the encoded object */
    ray_t* cp = ray_alloc_copy(s);
    TEST_ASSERT_EQ_I(ray_table_nrows(cp), 70000);
    for (int64_t c = 0; c < 2; c++) {
        ray_t* enc = ray_table_get_col_enc_idx(s, c);
        TEST_ASSERT_TRUE(enc != NULL);
        TEST_ASSERT_EQ_PTR(ray_table_get_col_enc_idx(cp, c), enc);
        TEST_ASSERT_EQ_U(atomic_load(&ray_enccol_of(enc)->rc), 1);   /* one header: shared through the cols list, not duplicated */
    }
    ray_release(cp);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    TEST_ASSERT_EQ_I(ray_table_get_col_idx(s, 1)->type, RAY_F64);   /* legacy getter: plain */
    ray_release(s);
    /* the splayed loader returns the same values as the generation-0 table */
    ray_t* ref = t;
    ray_t* ld = ray_read_splayed(ray_test_tmp(ENCCOL_TMP "/t"), NULL);
    TEST_ASSERT_FMT(ld && !RAY_IS_ERR(ld), "read_splayed: %s", ld ? ray_err_code(ld) : "null");
    TEST_ASSERT_EQ_I(ray_table_nrows(ld), 70000);
    for (int64_t c = 0; c < 2; c++) {
        TEST_ASSERT_TRUE(ray_table_get_col_enc_idx(ld, c) != NULL);
        ray_t* want = ray_table_get_col_idx(ref, c);
        ray_t* got  = ray_table_col_idx(ld, c);
        TEST_ASSERT_TRUE(got && !RAY_IS_ERR(got));
        TEST_ASSERT_EQ_I(got->type, want->type);
        TEST_ASSERT_EQ_I(got->len, want->len);
        TEST_ASSERT_TRUE(memcmp(ray_data(got), ray_data(want), (size_t)want->len * 8) == 0);
    }
    ray_release(ld); ray_release(ref);
    PASS();
}

/* Row counts of the non-vector column shapes, without decoding. */
static test_result_t test_table_col_nrows_shapes(void) {
    ray_t* a = ray_alloc(0); a->type = -RAY_I64; a->i64 = 7;
    TEST_ASSERT_EQ_I(ray_table_col_nrows(a), 0);       /* an atom has no rows: its len */
    ray_release(a);
    ray_t* keys = ray_vec_new(RAY_I64, 2); keys->len = 2;
    ray_t* counts = ray_vec_new(RAY_I64, 2); counts->len = 2;
    ((int64_t*)ray_data(counts))[0] = 30; ((int64_t*)ray_data(counts))[1] = 12;
    ray_t* mc = ray_alloc(2 * sizeof(ray_t*));
    mc->type = RAY_MAPCOMMON; mc->len = 2;
    ((ray_t**)ray_data(mc))[0] = keys; ((ray_t**)ray_data(mc))[1] = counts;
    TEST_ASSERT_EQ_I(ray_table_col_nrows(mc), 42);
    ray_release(mc);
    PASS();
}

/* Three partitions of t(a I64, b F64); partition p is generation 2 when
 * gen2_mask has bit p set, generation 0 otherwise. */
static ray_err_t write_parted(const char* root, int gen2_mask, int64_t rows) {
    for (int p = 0; p < 3; p++) {
        char dir[512]; snprintf(dir, sizeof dir, "%s/2024.01.0%d/t", root, p + 1);
        ray_t* t = tbl2(rows);
        ray_err_t e;
        if (gen2_mask & (1 << p)) e = write_splayed_gen2(dir, t);
        else e = ray_splay_save(t, dir, NULL);
        ray_release(t);
        if (e != RAY_OK) return e;
    }
    return RAY_OK;
}

static test_result_t test_parted_mixed_load(void) {
    TEST_ASSERT_EQ_I(write_parted(ray_test_tmp(ENCCOL_TMP "/pdb"), 0x5, 4000), RAY_OK);   /* parts 1 and 3 encoded */
    ray_enc_counters_reset();
    ray_t* t = ray_read_parted(ray_test_tmp(ENCCOL_TMP "/pdb"), "t");
    TEST_ASSERT_FMT(t && !RAY_IS_ERR(t), "parted load: %s", t ? ray_err_code(t) : "null");
    TEST_ASSERT_EQ_I(ray_table_nrows(t), 12000);
    ray_t* slot = ray_table_get_col_enc(t, ray_sym_intern("a", 1));
    TEST_ASSERT_TRUE(slot && ray_enccol_is_parted(slot));
    TEST_ASSERT_EQ_I(ray_table_col_nrows(slot), 12000);          /* sum of segment lens, no decode */
    ray_col_meta_t m;
    TEST_ASSERT_TRUE(ray_table_col_meta(t, ray_sym_intern("a", 1), &m));
    TEST_ASSERT_EQ_I(m.type, RAY_PARTED_BASE + RAY_I64);
    TEST_ASSERT_EQ_I(m.n_segs, 3);
    TEST_ASSERT_TRUE(m.enc != NULL);
    ray_t* enc = m.enc;
    TEST_ASSERT_EQ_I(ray_enccol_seg(enc, 0)->type, RAY_ENCCOL);
    TEST_ASSERT_EQ_I(ray_enccol_seg(enc, 1)->type, RAY_I64);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    ray_t* s2 = ray_enccol_seg_col(enc, 2);                 /* one segment only */
    TEST_ASSERT_TRUE(s2 && !RAY_IS_ERR(s2) && s2->type == RAY_I64);
    ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 1);
    ray_t* w = ray_table_col(t, ray_sym_intern("a", 1));     /* plain wrapper */
    TEST_ASSERT_TRUE(w && !RAY_IS_ERR(w));
    TEST_ASSERT_EQ_I(w->type, RAY_PARTED_BASE + RAY_I64);
    ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 2);                    /* segment 0 decoded, 2 cached */
    ray_t** segs = (ray_t**)ray_data(w);
    TEST_ASSERT_EQ_PTR(segs[2], s2);
    ray_retain(w);
    ray_release(t);                                          /* retained wrapper outlives the table */
    /* segment 0 decodes to what the same directory loads as on its own */
    ray_t* solo = ray_read_splayed(ray_test_tmp(ENCCOL_TMP "/pdb/2024.01.01/t"), NULL);
    TEST_ASSERT_TRUE(solo && !RAY_IS_ERR(solo));
    ray_t* sa = ray_table_col(solo, ray_sym_intern("a", 1));
    TEST_ASSERT_TRUE(sa && !RAY_IS_ERR(sa) && sa->len == 4000 && segs[0]->len == 4000);
    TEST_ASSERT_TRUE(memcmp(ray_data(sa), ray_data(segs[0]), 4000 * sizeof(int64_t)) == 0);
    ray_release(solo);
    ray_release(w);
    PASS();
}

/* A parted load of generation-2 segments materializes to exactly the wrapper
 * a generation-0 load of the same data produces, ragged partition included. */
static test_result_t test_parted_equals_gen0(void) {
    TEST_ASSERT_EQ_I(write_parted(ray_test_tmp(ENCCOL_TMP "/pg2"), 0x5, 3000), RAY_OK);
    TEST_ASSERT_EQ_I(write_parted(ray_test_tmp(ENCCOL_TMP "/pg0"), 0x0, 3000), RAY_OK);
    /* the same rows in both (rng is consumed per write): rewrite pg0 from pg2's columns */
    for (int p = 0; p < 3; p++) {
        char d2[512], d0[512];
        snprintf(d2, sizeof d2, ray_test_tmp(ENCCOL_TMP "/pg2/2024.01.0%d/t"), p + 1);
        snprintf(d0, sizeof d0, ray_test_tmp(ENCCOL_TMP "/pg0/2024.01.0%d/t"), p + 1);
        ray_t* src = ray_read_splayed(d2, NULL);
        TEST_ASSERT_TRUE(src && !RAY_IS_ERR(src));
        int64_t nc = ray_table_ncols(src);
        if (p == 2) nc = 1;                      /* ragged: partition 3 has no column b */
        ray_t* flat = ray_table_new(nc);
        for (int64_t c = 0; c < nc; c++)
            flat = ray_table_add_col(flat, ray_table_col_name(src, c), ray_table_col_idx(src, c));
        TEST_ASSERT_EQ_I(ray_splay_save(flat, d0, NULL), RAY_OK);
        ray_release(flat); ray_release(src);
    }
    remove(ray_test_tmp(ENCCOL_TMP "/pg2/2024.01.03/t/b"));
    {
        ray_t* names = ray_vec_new(RAY_STR, 1); names = ray_str_vec_append(names, "a", 1);
        TEST_ASSERT_EQ_I(ray_col_save(names, ray_test_tmp(ENCCOL_TMP "/pg2/2024.01.03/t/.d")), RAY_OK);
        ray_release(names);
    }
    ray_t* t2 = ray_read_parted(ray_test_tmp(ENCCOL_TMP "/pg2"), "t");
    ray_t* t0 = ray_read_parted(ray_test_tmp(ENCCOL_TMP "/pg0"), "t");
    TEST_ASSERT_TRUE(t2 && !RAY_IS_ERR(t2) && t0 && !RAY_IS_ERR(t0));
    ray_enc_counters_reset();
    TEST_ASSERT_EQ_I(ray_table_ncols(t2), ray_table_ncols(t0));
    for (const char* nm = "a"; nm; nm = nm[0] == 'a' ? "b" : NULL) {
        int64_t id = ray_sym_intern(nm, 1);
        ray_t* w2 = ray_table_col(t2, id);
        ray_t* w0 = ray_table_col(t0, id);
        TEST_ASSERT_TRUE(w2 && !RAY_IS_ERR(w2) && w0 && !RAY_IS_ERR(w0));
        TEST_ASSERT_EQ_I(w2->type, w0->type);
        TEST_ASSERT_EQ_I(w2->attrs, w0->attrs);
        TEST_ASSERT_EQ_I(w2->len, w0->len);
        ray_t** s2 = (ray_t**)ray_data(w2); ray_t** s0 = (ray_t**)ray_data(w0);
        for (int64_t i = 0; i < w2->len; i++) {
            TEST_ASSERT_FMT((s2[i] == NULL) == (s0[i] == NULL), "col %s seg %lld: %p vs %p", nm, (long long)i, (void*)s2[i], (void*)s0[i]);
            if (!s2[i]) continue;
            TEST_ASSERT_EQ_I(s2[i]->type, s0[i]->type);
            TEST_ASSERT_EQ_I(s2[i]->len, s0[i]->len);
            TEST_ASSERT_TRUE(memcmp(ray_data(s2[i]), ray_data(s0[i]), (size_t)s2[i]->len * 8) == 0);
        }
    }
    ray_release(t2); ray_release(t0);
    PASS();
}

/* The statement-boundary trim drops whole-column decodes that do not fit a
 * quarter of the watermark, least recently asked for first.  A decode a
 * caller retained is neither counted nor dropped; the column decodes again
 * when next read; a budget they fit keeps them. */
static ray_t* trim_col(const char* name, int64_t n, int64_t mul, ray_t** src) {
    ray_t* v = ray_vec_new(RAY_I64, n); v->len = n;
    for (int64_t i = 0; i < n; i++) ((int64_t*)ray_data(v))[i] = (i * mul) % 1000003 - 500000;
    if (ray_col_save_gen(v, ray_test_tmp(name), 2) != RAY_OK) { ray_release(v); return NULL; }
    *src = v;
    return ray_col_mmap(ray_test_tmp(name));
}
static bool trim_same(ray_t* m, ray_t* src) {
    if (!m || RAY_IS_ERR(m) || m->len != src->len) return false;
    return memcmp(ray_data(m), ray_data(src), (size_t)src->len * sizeof(int64_t)) == 0;
}
static void trim_with(int64_t wm) {
    int64_t prev = ray_heap_anon_watermark();
    ray_heap_set_anon_watermark(wm);
    ray_enccol_cache_trim();
    ray_heap_set_anon_watermark(prev);
}

static test_result_t test_cache_trim_at_boundary(void) {
    const int64_t n = 70000;
    ray_t *sa = NULL, *sb = NULL;
    ray_t* a = trim_col(ENCCOL_TMP "/ta", n, 7919, &sa);
    ray_t* b = trim_col(ENCCOL_TMP "/tb", n, 104729, &sb);
    TEST_ASSERT_TRUE(a && !RAY_IS_ERR(a) && a->type == RAY_ENCCOL);
    TEST_ASSERT_TRUE(b && !RAY_IS_ERR(b) && b->type == RAY_ENCCOL);
    trim_with(1);                                   /* nothing kept from before */
    TEST_ASSERT_EQ_I(ray_enccol_cache_bytes(), 0);

    ray_t* ma = ray_enccol_materialize(a);
    TEST_ASSERT_TRUE(trim_same(ma, sa));
    TEST_ASSERT_EQ_PTR(ray_enccol_decoded_peek(a), ma);
    int64_t one = (int64_t)ray_block_size(ma);
    TEST_ASSERT_EQ_I(ray_enccol_cache_bytes(), one);
    ray_retain(ma);                                 /* a caller keeps it: dropping frees nothing */
    TEST_ASSERT_EQ_I(ray_enccol_cache_bytes(), 0);
    trim_with(1);
    TEST_ASSERT_EQ_PTR(ray_enccol_decoded_peek(a), ma);
    ray_release(ma);                                /* only the cache holds it now */
    TEST_ASSERT_EQ_I(ray_enccol_cache_bytes(), one);
    trim_with(1);
    TEST_ASSERT_NULL(ray_enccol_decoded_peek(a));
    TEST_ASSERT_EQ_I(ray_enccol_cache_bytes(), 0);
    ray_t* ma2 = ray_enccol_materialize(a);         /* decodes again */
    TEST_ASSERT_TRUE(trim_same(ma2, sa));
    TEST_ASSERT_EQ_PTR(ray_enccol_decoded_peek(a), ma2);

    /* a budget both fit keeps both; one that fits one keeps the later one */
    int64_t roomy = 4 * (2 * one + 4096), tight = 4 * (one + 4096);
    trim_with(roomy);                               /* A: this statement */
    ray_t* mb = ray_enccol_materialize(b);          /* B: the next one */
    TEST_ASSERT_TRUE(trim_same(mb, sb));
    trim_with(roomy);
    TEST_ASSERT_EQ_PTR(ray_enccol_decoded_peek(a), ma2);
    TEST_ASSERT_EQ_PTR(ray_enccol_decoded_peek(b), mb);
    TEST_ASSERT_EQ_I(ray_enccol_cache_bytes(), 2 * one);
    trim_with(tight);
    TEST_ASSERT_NULL(ray_enccol_decoded_peek(a));   /* least recently asked for */
    TEST_ASSERT_EQ_PTR(ray_enccol_decoded_peek(b), mb);
    TEST_ASSERT_EQ_I(ray_enccol_cache_bytes(), one);

    /* no watermark keeps everything */
    ray_t* ma3 = ray_enccol_materialize(a);
    TEST_ASSERT_TRUE(trim_same(ma3, sa));
    trim_with(0);
    TEST_ASSERT_EQ_PTR(ray_enccol_decoded_peek(a), ma3);
    TEST_ASSERT_EQ_PTR(ray_enccol_decoded_peek(b), mb);

    ray_release(a); ray_release(b);                 /* a freed column leaves the list */
    TEST_ASSERT_EQ_I(ray_enccol_cache_bytes(), 0);
    ray_release(sa); ray_release(sb);
    PASS();
}

/* (take tbl n) and the table display read an encoded column's taken or
 * shown rows by range: neither leaves the column decoded whole.  A plain
 * read still does. */
static test_result_t test_take_show_read_window(void) {
    const int64_t n = 70000;
    ray_t* tbl = ray_table_new(1);
    ray_t* a = ray_vec_new(RAY_I32, n); a->len = n;
    for (int64_t i = 0; i < n; i++) ((int32_t*)ray_data(a))[i] = (int32_t)(i % 977);
    int64_t cb = ray_sym_intern("cb", 2);
    tbl = ray_table_add_col(tbl, cb, a);
    ray_release(a);
    TEST_ASSERT_EQ_I(ray_splay_save(tbl, ray_test_tmp(ENCCOL_TMP "/tw"), NULL), RAY_OK);
    ray_release(tbl);
    TEST_ASSERT_EQ_I(save_i32_rows(ray_test_tmp(ENCCOL_TMP "/tw/cb"), n), RAY_OK);
    ray_t* r = ray_read_splayed(ray_test_tmp(ENCCOL_TMP "/tw"), NULL);
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    ray_t* enc = ray_table_get_col_enc(r, cb);
    TEST_ASSERT_NOT_NULL(enc);

    ray_t* k = ray_i64(-3);
    ray_t* t3 = ray_take_fn(r, k);
    ray_release(k);
    TEST_ASSERT_TRUE(t3 && !RAY_IS_ERR(t3) && ray_table_nrows(t3) == 3);
    ray_t* c3 = ray_table_col(t3, cb);
    TEST_ASSERT_TRUE(c3 && c3->type == RAY_I32 && c3->len == 3);
    for (int i = 0; i < 3; i++) TEST_ASSERT_EQ_I(((int32_t*)ray_data(c3))[i], (n - 3 + i) % 977);
    ray_release(t3);
    TEST_ASSERT_NULL(ray_enccol_decoded_peek(enc));

    ray_t* shown = ray_fmt(r, 1);
    TEST_ASSERT_TRUE(shown && !RAY_IS_ERR(shown));
    TEST_ASSERT_TRUE(strstr(ray_str_ptr(shown), "70000 rows") != NULL);
    TEST_ASSERT_TRUE(strstr(ray_str_ptr(shown), "│ 632 ") != NULL);  /* the last row: 69999 % 977 */
    ray_release(shown);
    TEST_ASSERT_NULL(ray_enccol_decoded_peek(enc));

    ray_t* plain = ray_table_col(r, cb);              /* a plain read decodes */
    TEST_ASSERT_TRUE(plain && !RAY_IS_ERR(plain));
    TEST_ASSERT_EQ_PTR(ray_enccol_decoded_peek(enc), plain);
    ray_release(r);
    PASS();
}

/* Whether a whole decode would stay cached (ray_enccol_decode_kept): its
 * generation-0 bytes against a quarter of the watermark, or the test
 * budget; metadata only. */
static test_result_t test_decode_kept_budget(void) {
    const int64_t n = 70000;                          /* 560000 bytes decoded */
    ray_t* v = make_col(RAY_I64, n, 0);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/kb"), 2), RAY_OK);
    ray_release(v);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/kb"));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c) && c->type == RAY_ENCCOL);
    int64_t prev = ray_heap_anon_watermark();
    ray_heap_set_anon_watermark(4 * 560000);          /* a quarter holds it exactly */
    TEST_ASSERT_TRUE(ray_enccol_decode_kept(c));
    ray_heap_set_anon_watermark(4 * 560000 - 4);
    TEST_ASSERT_FALSE(ray_enccol_decode_kept(c));
    ray_enccol_test_kept_limit(560000);               /* the test budget replaces it */
    TEST_ASSERT_TRUE(ray_enccol_decode_kept(c));
    ray_enccol_test_kept_limit(559999);
    TEST_ASSERT_FALSE(ray_enccol_decode_kept(c));
    ray_enccol_test_kept_limit(0);
    ray_heap_set_anon_watermark(prev);
    TEST_ASSERT_NULL(ray_enccol_decoded_peek(c));     /* nothing was read */
    ray_release(c);
    PASS();
}

/* Distinct cells of v[0, n) as the count-distinct kernels key them (a null
 * one value); integer types. */
static int cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y;
}
static int64_t distinct_i64(int64_t* x, int64_t n) {
    if (n == 0) return 0;
    qsort(x, (size_t)n, sizeof *x, cmp_i64);
    int64_t d = 1;
    for (int64_t i = 1; i < n; i++) d += x[i] != x[i - 1];
    return d;
}
static int64_t cell_i64(ray_t* v, int64_t i) {
    const void* d = ray_data(v);
    switch (v->type) {
    case RAY_I16: return ((const int16_t*)d)[i];
    case RAY_I32: return ((const int32_t*)d)[i];
    default:      return ((const int64_t*)d)[i];
    }
}

/* ray_col_gather_span reads rows [lo, hi) of a generation-2 column as a
 * plain vector equal to those rows of the source — nulls (HAS_NULLS where
 * the rows hold one), STR pools, spans that cross the 64 K-row chunks, end
 * inside a vector, or hold nothing — and refuses an unaligned start; the
 * bounded count-distinct kernel reads the column (as a value and as a key)
 * through range reads and counts as over the plain rows.  Neither leaves
 * the column decoded whole. */
static test_result_t test_gather_span_bounded_read_window(void) {
    const int64_t n = 140000;
    static const struct { int8_t t; int s; } C[] = {
        { RAY_I64, 4 }, { RAY_I32, 1 }, { RAY_I16, 4 }, { RAY_STR, 4 }, { RAY_F64, 4 },
    };
    const int64_t spans[][2] = {
        { 0, 140000 }, { 0, 51200 }, { 51200, 102400 }, { 65536, 131072 },
        { 139264, 140000 }, { 102400, 102400 }, { 51200, 52000 }, { 64512, 66000 },
    };
    ray_t* src_v[5] = { 0 };
    ray_t* enc[5] = { 0 };
    for (int k = 0; k < 5; k++) {
        char p[64];
        snprintf(p, sizeof p, ENCCOL_TMP "/gs%d", k);
        src_v[k] = make_col(C[k].t, n, C[k].s);
        TEST_ASSERT_EQ_I(ray_col_save_gen(src_v[k], ray_test_tmp(p), 2), RAY_OK);
        enc[k] = ray_col_mmap(ray_test_tmp(p));
        TEST_ASSERT_TRUE(enc[k] && !RAY_IS_ERR(enc[k]) && enc[k]->type == RAY_ENCCOL);
    }
    for (int k = 0; k < 5; k++) {
        ray_t* v = src_v[k];
        ray_col_src_t s;
        TEST_ASSERT_NULL(ray_col_src_bind(enc[k], &s));
        for (size_t q = 0; q < sizeof spans / sizeof spans[0]; q++) {
            int64_t lo = spans[q][0], hi = spans[q][1];
            ray_t* out = NULL;
            ray_t* err = ray_col_gather_span(&s, lo, hi, &out);
            TEST_ASSERT_FMT(!err, "type %d span [%lld, %lld): %s", C[k].t, (long long)lo, (long long)hi, err ? ray_err_code(err) : "");
            TEST_ASSERT_TRUE(out && out->type == v->type && out->len == hi - lo);
            bool nulls = false;
            for (int64_t i = lo; i < hi; i++) nulls = nulls || ray_vec_is_null(v, i);
            TEST_ASSERT_EQ_I((out->attrs & RAY_ATTR_HAS_NULLS) != 0, nulls);
            for (int64_t i = 0; i < hi - lo; i++) {
                TEST_ASSERT_EQ_I(ray_vec_is_null(out, i), ray_vec_is_null(v, lo + i));
                if (v->type == RAY_STR) {
                    size_t la = 0, lb = 0;
                    const char* a = ray_str_vec_get(out, i, &la);
                    const char* b = ray_str_vec_get(v, lo + i, &lb);
                    TEST_ASSERT_TRUE(la == lb && (la == 0 || memcmp(a, b, la) == 0));
                } else {
                    uint8_t esz = ray_sym_elem_size(v->type, v->attrs);
                    TEST_ASSERT_FMT(memcmp((char*)ray_data(out) + i * esz, (char*)ray_data(v) + (lo + i) * esz, esz) == 0,
                                    "type %d row %lld", C[k].t, (long long)(lo + i));
                }
            }
            ray_release(out);
        }
        ray_t* out = NULL;
        ray_t* err = ray_col_gather_span(&s, 100, 2148, &out);   /* not at a vector start */
        TEST_ASSERT_TRUE(err && out == NULL);
        TEST_ASSERT_STR_EQ(ray_err_code(err), "range");
        ray_error_free(err);
        ray_col_src_release(&s);
        TEST_ASSERT_NULL(ray_enccol_decoded_peek(enc[k]));
    }

    /* the bounded kernel over one group and over the I32 key's groups */
    int64_t* x = (int64_t*)ray_alloc_raw((size_t)n * sizeof(int64_t));
    TEST_ASSERT_NOT_NULL(x);
    for (int k = 0; k < 3; k++) {
        for (int64_t i = 0; i < n; i++) x[i] = cell_i64(src_v[k], i);
        int64_t want = distinct_i64(x, n);
        ray_t* got = ray_cd_bounded_all(enc[k], n);
        TEST_ASSERT_TRUE(got && !RAY_IS_ERR(got) && got->type == -RAY_I64);
        TEST_ASSERT_EQ_I(got->i64, want);
        ray_release(got);
    }
    TEST_ASSERT_NULL(ray_cd_bounded_all(enc[3], n));          /* STR: not served */
    ray_t* keys = ray_vec_new(RAY_I32, 64);
    keys->len = 64;
    for (int g = 0; g < 64; g++) ((int32_t*)ray_data(keys))[g] = g;
    ray_t* got = ray_cd_bounded(NULL, enc[1], keys, NULL, 64, enc[0], NULL, n);
    TEST_ASSERT_TRUE(got && !RAY_IS_ERR(got) && got->type == RAY_I64 && got->len == 64);
    for (int g = 0; g < 64; g++) {
        int64_t m = 0;
        for (int64_t i = 0; i < n; i++)
            if (cell_i64(src_v[1], i) == g) x[m++] = cell_i64(src_v[0], i);
        TEST_ASSERT_FMT(((int64_t*)ray_data(got))[g] == distinct_i64(x, m), "group %d", g);
    }
    ray_release(got);
    ray_release(keys);
    ray_free_raw(x);
    for (int k = 0; k < 5; k++) {
        TEST_ASSERT_NULL(ray_enccol_decoded_peek(enc[k]));
        ray_release(enc[k]);
        ray_release(src_v[k]);
    }
    PASS();
}

const test_entry_t enccol_entries[] = {
    { "enccol/take_show_read_window", test_take_show_read_window, enccol_setup, enccol_teardown },
    { "enccol/decode_kept_budget",    test_decode_kept_budget,    enccol_setup, enccol_teardown },
    { "enccol/gather_span_bounded_read_window", test_gather_span_bounded_read_window, enccol_setup, enccol_teardown },
    { "enccol/cache_trim_at_boundary", test_cache_trim_at_boundary, enccol_setup, enccol_teardown },
    { "enccol/open_returns_enccol", test_open_returns_enccol, enccol_setup, enccol_teardown },
    { "enccol/load_version",        test_load_version,        enccol_setup, enccol_teardown },
    { "enccol/sym_requires_domain", test_sym_requires_domain, enccol_setup, enccol_teardown },
    { "enccol/append_index_gen2",   test_append_index_refuses_gen2, enccol_setup, enccol_teardown },
    { "enccol/load_gen2_plain",      test_load_gen2_plain,       enccol_setup, enccol_teardown },
    { "enccol/no_mapping_leak",     test_no_mapping_leak,     enccol_setup, enccol_teardown },
    { "enccol/value_dict_side",     test_value_dict_side,     enccol_setup, enccol_teardown },
    { "enccol/sym_domain_rules",    test_sym_domain_rules,    enccol_setup, enccol_teardown },
    { "enccol/consumers_refuse",    test_consumers_refuse,    enccol_setup, enccol_teardown },
    { "enccol/be_gate_logic",       test_be_gate_logic,       enccol_setup, enccol_teardown },
    { "enccol/roundtrip_types_shapes", test_roundtrip_types_shapes, enccol_setup, enccol_teardown },
    { "enccol/roundtrip_lengths",      test_roundtrip_lengths,      enccol_setup, enccol_teardown },
    { "enccol/roundtrip_edges",        test_roundtrip_edges,        enccol_setup, enccol_teardown },
    { "enccol/golden_decode",          test_golden_decode,          enccol_setup, enccol_teardown },
    { "enccol/dict_once_concurrent",   test_dict_once_concurrent,   enccol_setup, enccol_teardown },
    { "enccol/fast_kernel_equal",      test_fast_kernel_equal,      enccol_setup, enccol_teardown },
    { "enccol/runs_kernel_equal",      test_runs_kernel_equal,      enccol_setup, enccol_teardown },
    { "enccol/raw_int_view",           test_raw_int_view,           enccol_setup, enccol_teardown },
    { "enccol/point_read_equal",       test_point_read_equal,       enccol_setup, enccol_teardown },
    { "enccol/dict_kernel_equal",      test_dict_kernel_equal,      enccol_setup, enccol_teardown },
    { "enccol/dict_kernel_shapes",    test_dict_kernel_shapes,    enccol_setup, enccol_teardown },
    { "enccol/point_read_declines",    test_point_read_declines,    enccol_setup, enccol_teardown },
    { "enccol/point_read_runs",        test_point_read_runs,        enccol_setup, enccol_teardown },
    { "enccol/point_read_column",      test_point_read_column,      enccol_setup, enccol_teardown },
    { "enccol/scratch_outlives_source", test_scratch_outlives_source, enccol_setup, enccol_teardown },
    { "enccol/payload_is_tail",  test_payload_is_tail,  enccol_setup, enccol_teardown },
    { "enccol/dict_null_dictionary",   test_dict_null_dictionary,   enccol_setup, enccol_teardown },
    { "enccol/vec_range_checks",       test_vec_range_checks,       enccol_setup, enccol_teardown },
    { "enccol/materialize_equivalence",        test_materialize_equivalence,        enccol_setup, enccol_teardown },
    { "enccol/materialize_lengths",            test_materialize_lengths,            enccol_setup, enccol_teardown },
    { "enccol/materialize_float_bits",         test_materialize_float_bits,         enccol_setup, enccol_teardown },
    { "enccol/decoded_peek",                   test_decoded_peek,                   enccol_setup, enccol_teardown },
    { "enccol/cols_inplace_encoded",           test_cols_inplace_encoded,           enccol_setup, enccol_teardown },
    { "enccol/materialize_counts_once",        test_materialize_counts_once,        enccol_setup, enccol_teardown },
    { "enccol/oom_then_retry",                 test_oom_then_retry,                 enccol_setup, enccol_teardown },
    { "enccol/col_load_materializes",          test_col_load_materializes,          enccol_setup, enccol_teardown },
    { "enccol/cancel_retried",                 test_cancel_retried,                 enccol_setup, enccol_teardown },
    { "enccol/cancel_mid_decode",              test_cancel_mid_decode,              enccol_setup, enccol_teardown },
    { "enccol/cancel_waiter",                  test_cancel_waiter,                  enccol_setup, enccol_teardown },
    { "enccol/load_dom_refcount",              test_load_dom_refcount,              enccol_setup, enccol_teardown },
    { "enccol/synth_zone",            test_synth_zone,            enccol_setup, enccol_teardown },
    { "enccol/synth_chunk_zone",      test_synth_chunk_zone,      enccol_setup, enccol_teardown },
    { "enccol/unique_flag",           test_unique_flag,           enccol_setup, enccol_teardown },
    { "enccol/attach_refused",        test_attach_refused,        enccol_setup, enccol_teardown },
    { "enccol/synth_matches_gen0",    test_synth_matches_gen0,    enccol_setup, enccol_teardown },
    { "enccol/table_core",            test_table_core_enccol,     enccol_setup, enccol_teardown },
    { "enccol/table_col_nrows_shapes", test_table_col_nrows_shapes, enccol_setup, enccol_teardown },
    { "enccol/parted_mixed_load",             test_parted_mixed_load,             enccol_setup, enccol_teardown },
    { "enccol/parted_equals_gen0",            test_parted_equals_gen0,            enccol_setup, enccol_teardown },
    { NULL, NULL, NULL, NULL },
};
