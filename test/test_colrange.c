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
#include "table/sym.h"
#include "table/domain.h"
#include "table/table.h"
#include "store/col.h"
#include "store/colrange.h"
#include "store/enccol.h"
#include "store/splay.h"
#include "store/part.h"
#include "vec/str.h"
#include "vec/vec.h"
#include <stdio.h>
#include <string.h>

#define CR_TMP "/tmp/rayforce_colrange"

/* 256 interned symbols, so SYM test ids up to 255 lie inside the runtime domain. */
static void cr_setup(void) {
    ray_heap_init(); (void)ray_sym_init(); (void)ray_test_mkdir_p(CR_TMP);
    for (int i = 0; i < 256; i++) { char b[8]; int n = snprintf(b, sizeof b, "s%d", i); (void)ray_sym_intern(b, (size_t)n); }
}
static void cr_teardown(void) { ray_sym_destroy(); ray_heap_destroy(); (void)ray_test_rm_rf(CR_TMP); }

static ray_t* i32_nulls(int64_t n) {
    ray_t* v = ray_vec_new(RAY_I32, n); v->len = n;
    for (int64_t i = 0; i < n; i++) ((int32_t*)ray_data(v))[i] = (i % 5 == 0) ? NULL_I32 : (int32_t)i;
    v->attrs |= RAY_ATTR_HAS_NULLS;
    return v;
}

/* Deterministic column of `type`; `nulls` puts the type's null in every 7th
 * row.  STR mixes inline, pooled and empty (null) strings. */
static ray_t* gen_col(int8_t type, int64_t n, bool nulls, uint8_t symw) {
    if (type == RAY_STR) {
        ray_t* v = ray_vec_new(RAY_STR, n);
        char buf[64];
        for (int64_t i = 0; i < n; i++) {
            int len;
            if (nulls && i % 7 == 0) len = 0;
            else if (i % 3 == 0) len = snprintf(buf, sizeof buf, "ab%lld", (long long)(i % 50));
            else len = snprintf(buf, sizeof buf, "pooled string number %lld", (long long)(i * 31 % 100000));
            v = ray_str_vec_append(v, buf, (size_t)len);
        }
        return v;
    }
    ray_t* v = type == RAY_SYM ? ray_sym_vec_new(symw, n) : ray_vec_new(type, n);
    v->len = n;
    uint8_t* d = ray_data(v);
    for (int64_t i = 0; i < n; i++) {
        bool null = nulls && i % 7 == 0;
        uint64_t x = (uint64_t)(i / 3) * 2654435761u % 1000003u;
        switch (type) {
        case RAY_BOOL: d[i] = (uint8_t)(x & 1); break;
        case RAY_U8:   d[i] = (uint8_t)x; break;
        case RAY_I16:  { int16_t y = null ? NULL_I16 : (int16_t)(x % 30000); memcpy(d + i * 2, &y, 2); break; }
        case RAY_I32: case RAY_DATE: case RAY_TIME:
                       { int32_t y = null ? NULL_I32 : (int32_t)(x * 977 % 2000000000); memcpy(d + i * 4, &y, 4); break; }
        case RAY_I64: case RAY_TIMESTAMP:
                       { int64_t y = null ? NULL_I64 : (int64_t)x * 1000003; memcpy(d + i * 8, &y, 8); break; }
        case RAY_F32:  { float y = null ? NULL_F32 : (float)(int64_t)(x % 100000) / 8.0f; memcpy(d + i * 4, &y, 4); break; }
        case RAY_F64:  { double y = null ? NULL_F64 : (double)(int64_t)(x % 1000000) / 16.0; memcpy(d + i * 8, &y, 8); break; }
        case RAY_GUID: { memset(d + i * 16, 0, 16); if (!null) { memcpy(d + i * 16, &x, 8); d[i * 16 + 15] = 1; } break; }
        case RAY_SYM:  ray_write_sym(d, i, null ? 0 : 1 + x % 200, RAY_SYM, v->attrs); break;
        }
    }
    if (nulls && type != RAY_BOOL && type != RAY_U8 && type != RAY_SYM) v->attrs |= RAY_ATTR_HAS_NULLS;
    return v;
}

/* Row `row` of the plain vector `ref` against row i of the view. */
static const char* cmp_row(const ray_col_range_t* r, int64_t i, ray_t* ref, int64_t row) {
    if (r->type != ref->type) return "type";
    if (ref->type == RAY_SYM && (r->attrs & RAY_SYM_W_MASK) != (ref->attrs & RAY_SYM_W_MASK)) return "sym width";
    if (ref->type == RAY_SYM && r->dom != ray_sym_vec_domain(ref)) return "sym domain";
    bool is_null = ray_vec_is_null(ref, row);
    if (is_null && !r->may_null) return "null in a range that says may_null false";
    if (ref->type == RAY_STR) {
        const ray_str_t* a = (const ray_str_t*)r->data + i;
        const ray_str_t* b = (const ray_str_t*)ray_data(ref) + row;
        if (a->len != b->len) return "str len";
        if (a->len == 0) return NULL;
        const char* rp = r->str_pool;
        const char* fp = ref->str_pool ? (const char*)ray_data(ref->str_pool) : NULL;
        if ((a->len > 12 && !rp) || (b->len > 12 && !fp)) return "str pool missing";
        return memcmp(ray_str_t_ptr(a, rp), ray_str_t_ptr(b, fp), a->len) == 0 ? NULL : "str bytes";
    }
    uint8_t esz = ray_sym_elem_size(ref->type, ref->attrs);
    return memcmp((const char*)r->data + (size_t)i * esz, (const char*)ray_data(ref) + (size_t)row * esz, esz) == 0
           ? NULL : "element";
}

/* Walk the whole source with requests of `req` rows; every returned range is
 * compared with rows [ref_off + start, ...) of the plain vector `ref`.
 * `gen2`: every range must stay inside one 1024-row vector. */
static test_result_t walk_src(ray_col_src_t* s, ray_t* ref, int64_t ref_off, int64_t req, bool gen2) {
    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
    ray_col_range_t r;
    int64_t start = 0;
    while (start < s->len) {
        ray_t* err = ray_col_range(s, start, req, &sc, &r);
        TEST_ASSERT_FMT(!err, "range @%lld: %s", (long long)start, err ? ray_err_code(err) : "");
        TEST_ASSERT_FMT(r.n > 0 && r.n <= req && start + r.n <= s->len, "n=%lld @%lld", (long long)r.n, (long long)start);
        TEST_ASSERT_EQ_I(r.start, start);
        if (gen2) TEST_ASSERT_FMT(start / 1024 == (start + r.n - 1) / 1024, "range @%lld n=%lld crosses a vector", (long long)start, (long long)r.n);
        else {                                               /* zero copy: clipped only at the source end */
            int64_t want = s->len - start < req ? s->len - start : req;
            TEST_ASSERT_FMT(r.n == want, "gen0 n=%lld want %lld @%lld", (long long)r.n, (long long)want, (long long)start);
            uint8_t esz = ray_sym_elem_size(ref->type, ref->attrs);
            TEST_ASSERT_TRUE(r.data == (const char*)ray_data(ref) + (size_t)(ref_off + start) * esz);
        }
        for (int64_t i = 0; i < r.n; i++) {
            const char* bad = cmp_row(&r, i, ref, ref_off + start + i);
            TEST_ASSERT_FMT(!bad, "row %lld req %lld: %s", (long long)(start + i), (long long)req, bad ? bad : "");
        }
        start += r.n;
    }
    TEST_ASSERT_EQ_I(start, s->len);
    /* an empty request and the source end */
    TEST_ASSERT_NULL(ray_col_range(s, s->len, 10, &sc, &r));
    TEST_ASSERT_EQ_I(r.n, 0);
    TEST_ASSERT_NULL(ray_col_range(s, 0, 0, &sc, &r));
    TEST_ASSERT_EQ_I(r.n, 0);
    ray_col_scratch_free(&sc);
    PASS();
}

/* Starts that land mid-vector and mid-chunk. */
static test_result_t probe_src(ray_col_src_t* s, ray_t* ref, int64_t ref_off, bool gen2) {
    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
    ray_col_range_t r;
    for (int64_t start = 1; start < s->len; start += 4093) {
        int64_t reqs[] = { 1, 37, 1100 };
        for (int k = 0; k < 3; k++) {
            ray_t* err = ray_col_range(s, start, reqs[k], &sc, &r);
            TEST_ASSERT_FMT(!err, "probe @%lld", (long long)start);
            int64_t want = reqs[k];
            if (want > s->len - start) want = s->len - start;
            if (gen2) {
                int64_t vend = (start / 1024 + 1) * 1024;
                if (want > vend - start) want = vend - start;
            }
            TEST_ASSERT_EQ_I(r.n, want);
            for (int64_t i = 0; i < r.n; i++) {
                const char* bad = cmp_row(&r, i, ref, ref_off + start + i);
                TEST_ASSERT_FMT(!bad, "probe row %lld: %s", (long long)(start + i), bad ? bad : "");
            }
        }
    }
    ray_col_scratch_free(&sc);
    PASS();
}

#define RUN(call) do { test_result_t _r = (call); if (_r.status != TEST_PASS) return _r; } while (0)

/* gen-0, slice of gen-0 and gen-2 sources of every P1 type, SYM widths,
 * with and without nulls, against the materialized column. */
static test_result_t test_range_matches_materialized(void) {
    static const int8_t types[] = { RAY_BOOL, RAY_U8, RAY_I16, RAY_I32, RAY_I64, RAY_DATE, RAY_TIME,
                                    RAY_TIMESTAMP, RAY_F32, RAY_F64, RAY_GUID, RAY_SYM, RAY_STR };
    static const uint8_t widths[] = { RAY_SYM_W8, RAY_SYM_W16, RAY_SYM_W32, RAY_SYM_W64 };
    static const int64_t lens[] = { 1, 1023, 1025, 66000 };
    static const int64_t reqs[] = { 1000, 1024, 70000 };
    for (size_t t = 0; t < sizeof types; t++)
        for (size_t w = 0; w < (types[t] == RAY_SYM ? 4u : 1u); w++)
            for (size_t l = 0; l < 4; l++)
                for (int nl = 0; nl < 2; nl++) {
                    int64_t n = lens[l];
                    ray_t* v = gen_col(types[t], n, nl, widths[w]);
                    /* generation 0 */
                    ray_col_src_t s;
                    TEST_ASSERT_NULL(ray_col_src_bind(v, &s));
                    TEST_ASSERT_EQ_I(s.len, n);
                    TEST_ASSERT_EQ_I(s.type, types[t]);
                    for (size_t q = 0; q < 3; q++) RUN(walk_src(&s, v, 0, reqs[q], false));
                    if (n > 5000) RUN(probe_src(&s, v, 0, false));
                    ray_col_src_release(&s);
                    /* a slice of it */
                    if (n >= 1023) {
                        ray_t* sl = ray_vec_slice(v, 700, n - 1000);
                        TEST_ASSERT_NULL(ray_col_src_bind(sl, &s));
                        TEST_ASSERT_EQ_I(s.len, n - 1000);
                        RUN(walk_src(&s, v, 700, 1000, false));
                        ray_col_src_release(&s);
                        ray_release(sl);
                    }
                    /* generation 2 */
                    TEST_ASSERT_EQ_I(ray_col_save_gen(v, CR_TMP "/c", 2), RAY_OK);
                    ray_t* c = ray_col_mmap(CR_TMP "/c");
                    TEST_ASSERT_FMT(c && !RAY_IS_ERR(c), "mmap");
                    TEST_ASSERT_EQ_I(c->type, RAY_ENCCOL);
                    TEST_ASSERT_NULL(ray_col_src_bind(c, &s));
                    TEST_ASSERT_EQ_I(s.len, n);
                    TEST_ASSERT_EQ_I(s.type, types[t]);
                    ray_t* mat = ray_enccol_materialize(c);
                    TEST_ASSERT_TRUE(mat && !RAY_IS_ERR(mat));
                    if (!nl) TEST_ASSERT_TRUE(!s.may_null);             /* directory null counts prove it */
                    for (size_t q = 0; q < 3; q++) RUN(walk_src(&s, mat, 0, reqs[q], true));
                    RUN(walk_src(&s, v, 0, 1000, true));    /* and against the source rows */
                    if (n > 5000) RUN(probe_src(&s, mat, 0, true));
                    ray_col_src_release(&s);
                    ray_release(c); ray_release(v);
                }
    PASS();
}

static test_result_t test_range_gen0_zero_copy(void) {
    ray_t* v = i32_nulls(5000);
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(v, &s));
    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
    ray_col_range_t r;
    TEST_ASSERT_NULL(ray_col_range(&s, 1000, 3000, &sc, &r));
    TEST_ASSERT_EQ_PTR(r.data, (int32_t*)ray_data(v) + 1000);   /* pointer identity */
    TEST_ASSERT_EQ_I(r.n, 3000);
    TEST_ASSERT_TRUE(r.may_null);
    TEST_ASSERT_NULL(ray_col_range(&s, 4990, 100, &sc, &r));
    TEST_ASSERT_EQ_I(r.n, 10);                                   /* clipped at the end */
    TEST_ASSERT_TRUE(sc.fixed == NULL);                          /* generation 0 never touches scratch */
    ray_col_scratch_free(&sc); ray_col_src_release(&s); ray_release(v);
    PASS();
}

static test_result_t test_range_gen2_vector_clip_and_nulls(void) {
    ray_t* v = i32_nulls(5000);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, CR_TMP "/a", 2), RAY_OK);
    ray_t* c = ray_col_mmap(CR_TMP "/a");
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(c, &s));
    TEST_ASSERT_TRUE(s.may_null);
    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
    ray_col_range_t r;
    for (int64_t start = 0; start < 5000; ) {                /* task boundary 1000, not a chunk boundary */
        TEST_ASSERT_NULL(ray_col_range(&s, start, 1000, &sc, &r));
        int64_t vend = (start / 1024 + 1) * 1024;
        int64_t want = 1000;
        if (want > vend - start) want = vend - start;
        if (want > 5000 - start) want = 5000 - start;
        TEST_ASSERT_EQ_I(r.n, want);
        TEST_ASSERT_EQ_I(r.start, start);
        TEST_ASSERT_TRUE(r.may_null);
        TEST_ASSERT_TRUE(memcmp(r.data, (int32_t*)ray_data(v) + start, (size_t)r.n * 4) == 0);  /* sentinels in data */
        start += r.n;
    }
    TEST_ASSERT_EQ_U(sc.pool_cap, 0);
    ray_col_scratch_free(&sc); ray_col_src_release(&s);
    ray_release(c); ray_release(v);
    PASS();
}

static test_result_t test_range_two_str_views(void) {
    ray_t* a = ray_vec_new(RAY_STR, 3000); ray_t* b = ray_vec_new(RAY_STR, 3000);
    char buf[64];
    for (int i = 0; i < 3000; i++) {
        int n = snprintf(buf, sizeof buf, "pooled string number %d", i);
        a = ray_str_vec_append(a, buf, (size_t)n);
        n = snprintf(buf, sizeof buf, "another long text %d", i * 7);
        b = ray_str_vec_append(b, buf, (size_t)n);
    }
    TEST_ASSERT_EQ_I(ray_col_save_gen(b, CR_TMP "/b", 2), RAY_OK);
    ray_t* bc = ray_col_mmap(CR_TMP "/b");
    ray_col_src_t sa, sb;
    TEST_ASSERT_NULL(ray_col_src_bind(a, &sa));              /* generation 0 */
    TEST_ASSERT_NULL(ray_col_src_bind(bc, &sb));             /* generation 2 */
    ray_col_scratch_t xa, xb; ray_col_scratch_init(&xa); ray_col_scratch_init(&xb);
    ray_col_range_t ra, rb;
    TEST_ASSERT_NULL(ray_col_range(&sa, 1500, 1024, &xa, &ra));
    TEST_ASSERT_NULL(ray_col_range(&sb, 1500, 1024, &xb, &rb));   /* both views live */
    const ray_str_t* db = rb.data;
    const ray_str_t* ob = (const ray_str_t*)ray_data(b) + 1500;
    for (int64_t i = 0; i < rb.n; i++) {
        TEST_ASSERT_EQ_I(db[i].len, ob[i].len);
        TEST_ASSERT_TRUE(memcmp(ray_str_t_ptr(&db[i], rb.str_pool), ray_str_t_ptr(&ob[i], ray_data(b->str_pool)), ob[i].len) == 0);
        TEST_ASSERT_EQ_I(db[i].hash32, 0);
    }
    for (int64_t i = 0; i < ra.n; i++) {                    /* the generation-0 view is still intact */
        const ray_str_t* x = (const ray_str_t*)ra.data + i;
        const ray_str_t* y = (const ray_str_t*)ray_data(a) + 1500 + i;
        TEST_ASSERT_TRUE(x->len == y->len && memcmp(ray_str_t_ptr(x, ra.str_pool), ray_str_t_ptr(y, ray_data(a->str_pool)), x->len) == 0);
    }
    ray_col_scratch_free(&xa); ray_col_scratch_free(&xb);
    ray_col_src_release(&sa); ray_col_src_release(&sb);
    ray_release(bc); ray_release(a); ray_release(b);
    PASS();
}

static test_result_t test_range_slice_and_long_strings(void) {
    ray_t* v = i32_nulls(5000);
    ray_t* sl = ray_vec_slice(v, 100, 2000);
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(sl, &s));
    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
    ray_col_range_t r;
    TEST_ASSERT_NULL(ray_col_range(&s, 1990, 100, &sc, &r));
    TEST_ASSERT_EQ_I(r.n, 10);                               /* slice boundary */
    TEST_ASSERT_EQ_PTR(r.data, (int32_t*)ray_data(v) + 2090);
    ray_col_src_release(&s); ray_release(sl); ray_release(v);
    /* scratch grows for a vector of 1000-byte strings */
    static char big[1000]; memset(big, 'x', sizeof big);
    ray_t* st = ray_vec_new(RAY_STR, 1024);
    for (int i = 0; i < 1024; i++) { big[0] = (char)('a' + i % 26); st = ray_str_vec_append(st, big, sizeof big); }
    TEST_ASSERT_EQ_I(ray_col_save_gen(st, CR_TMP "/big", 2), RAY_OK);
    ray_t* c = ray_col_mmap(CR_TMP "/big");
    TEST_ASSERT_NULL(ray_col_src_bind(c, &s));
    TEST_ASSERT_NULL(ray_col_range(&s, 0, 1024, &sc, &r));
    TEST_ASSERT_TRUE(sc.pool_cap >= 1024 * 1000);
    TEST_ASSERT_EQ_I(((const ray_str_t*)r.data)[1023].len, 1000);
    ray_col_scratch_free(&sc); ray_col_src_release(&s); ray_release(c); ray_release(st);
    PASS();
}

static test_result_t test_range_error_propagates(void) {
    ray_t* v = i32_nulls(3000);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, CR_TMP "/bad", 2), RAY_OK);
    ray_release(v);
    FILE* f = fopen(CR_TMP "/bad", "r+b"); fseek(f, 160 + 6, SEEK_SET); fputc(1, f); fclose(f);
    ray_t* c = ray_col_mmap(CR_TMP "/bad");
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(c, &s));
    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
    ray_col_range_t r;
    ray_t* err = ray_col_range(&s, 0, 10, &sc, &r);
    TEST_ASSERT_TRUE(err && RAY_IS_ERR(err));
    TEST_ASSERT_STR_EQ(ray_err_code(err), "corrupt");
    ray_error_free(err);
    TEST_ASSERT_TRUE(r.data == NULL && r.n == 0);            /* nothing is handed out on an error */
    ray_col_scratch_free(&sc); ray_col_src_release(&s); ray_release(c);
    PASS();
}

/* Bounds, scratch size and the memory of a released source. */
static test_result_t test_range_bounds_and_scratch(void) {
    ray_t* v = gen_col(RAY_GUID, 3000, true, 0);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, CR_TMP "/g", 2), RAY_OK);
    ray_t* c = ray_col_mmap(CR_TMP "/g");
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(c, &s));
    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
    ray_col_range_t r;
    TEST_ASSERT_NULL(ray_col_range(&s, 0, 5000, &sc, &r));
    TEST_ASSERT_EQ_I(r.n, 1024);                              /* the widest element fills the 16 KB part */
    TEST_ASSERT_TRUE(sc.fixed != NULL && sc.pool == NULL && sc.pool_cap == 0);
    ray_t* e1 = ray_col_range(&s, 3001, 1, &sc, &r);          /* past the end */
    TEST_ASSERT_TRUE(e1 && RAY_IS_ERR(e1)); TEST_ASSERT_STR_EQ(ray_err_code(e1), "range"); ray_error_free(e1);
    e1 = ray_col_range(&s, -1, 1, &sc, &r);
    TEST_ASSERT_TRUE(e1 && RAY_IS_ERR(e1)); ray_error_free(e1);
    e1 = ray_col_range(&s, 0, -1, &sc, &r);
    TEST_ASSERT_TRUE(e1 && RAY_IS_ERR(e1)); ray_error_free(e1);
    TEST_ASSERT_NULL(ray_col_range(&s, 3000, 5, &sc, &r));
    TEST_ASSERT_EQ_I(r.n, 0);
    ray_col_scratch_free(&sc);
    TEST_ASSERT_TRUE(sc.fixed == NULL && sc.pool == NULL);
    ray_col_scratch_free(&sc);                                /* idempotent */
    ray_col_src_release(&s);
    TEST_ASSERT_TRUE(s.obj == NULL);
    ray_col_src_release(&s);
    ray_release(c); ray_release(v);
    /* a bind of nothing is an error, not a crash */
    ray_t* eb = ray_col_src_bind(NULL, &s);
    TEST_ASSERT_TRUE(eb && RAY_IS_ERR(eb)); ray_error_free(eb);
    PASS();
}

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

/* Three partitions of t(a I64, s STR, b F64) of different lengths; the
 * partitions in gen2_mask are generation 2.  With `ragged` the last
 * partition has no column b. */
static ray_t* load_parted(const char* root, int gen2_mask, bool ragged) {
    static const int64_t rows[3] = { 3000, 1024, 70000 };
    {   /* the shared root symfile the SYM segments' ids resolve over */
        char sp[512]; snprintf(sp, sizeof sp, "%s/.sym", root);
        if (ray_test_mkdir_p(root) != 0) return NULL;
        ray_sym_domain_t* d = ray_sym_domain_open_or_create(sp);
        if (!d) return NULL;
        for (int i = 0; i < 512; i++) { char b[8]; int n = snprintf(b, sizeof b, "d%d", i); (void)ray_sym_domain_intern(d, b, (size_t)n); }
        ray_err_t fe = ray_sym_domain_flush(d, false);
        ray_sym_domain_release(d);
        if (fe != RAY_OK) return NULL;
    }
    for (int p = 0; p < 3; p++) {
        char dir[512]; snprintf(dir, sizeof dir, "%s/2024.01.0%d/t", root, p + 1);
        char symp[512]; snprintf(symp, sizeof symp, "%s/.sym", root);
        ray_t* a = gen_col(RAY_I64, rows[p], true, 0);
        ray_t* s = gen_col(RAY_STR, rows[p], true, 0);
        ray_t* b = gen_col(RAY_F64, rows[p], true, 0);
        static const uint8_t pw[3] = { RAY_SYM_W8, RAY_SYM_W16, RAY_SYM_W32 };
        ray_t* y = gen_col(RAY_SYM, rows[p], true, pw[p]);       /* a different width per partition */
        ray_t* t = ray_table_new(4);
        t = ray_table_add_col(t, ray_sym_intern("a", 1), a);
        t = ray_table_add_col(t, ray_sym_intern("s", 1), s);
        t = ray_table_add_col(t, ray_sym_intern("y", 1), y);
        t = ray_table_add_col(t, ray_sym_intern("b", 1), b);     /* last: the ragged column */
        ray_release(a); ray_release(s); ray_release(b); ray_release(y);
        ray_err_t e = (gen2_mask & (1 << p)) ? write_splayed_gen2(dir, t) : ray_splay_save(t, dir, symp);
        ray_release(t);
        if (e != RAY_OK) return NULL;
        if (ragged && p == 2) {
            char f[600]; snprintf(f, sizeof f, "%s/b", dir); remove(f);
            ray_t* names = ray_vec_new(RAY_STR, 3);
            names = ray_str_vec_append(names, "a", 1); names = ray_str_vec_append(names, "s", 1);
            names = ray_str_vec_append(names, "y", 1);
            snprintf(f, sizeof f, "%s/.d", dir);
            e = ray_col_save(names, f);
            ray_release(names);
            if (e != RAY_OK) return NULL;
        }
    }
    ray_t* t = ray_read_parted(root, "t");
    return (t && !RAY_IS_ERR(t)) ? t : NULL;
}

/* Walk a parted source against its plain wrapper (segments; NULL = missing). */
static test_result_t walk_parted(ray_col_src_t* s, ray_t* wrapper, int64_t req) {
    ray_t** segs = (ray_t**)ray_data(wrapper);
    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
    ray_col_range_t r;
    int64_t start = 0;
    while (start < s->len) {
        ray_t* err = ray_col_range(s, start, req, &sc, &r);
        TEST_ASSERT_FMT(!err, "parted range @%lld: %s", (long long)start, err ? ray_err_code(err) : "");
        TEST_ASSERT_TRUE(r.n > 0);
        int64_t acc = 0, k = 0;
        while (k < wrapper->len && (!segs[k] || start >= acc + segs[k]->len)) { if (segs[k]) acc += segs[k]->len; k++; }
        TEST_ASSERT_TRUE(k < wrapper->len);
        TEST_ASSERT_FMT(start + r.n <= acc + segs[k]->len, "range @%lld n=%lld crosses segment %lld", (long long)start, (long long)r.n, (long long)k);
        for (int64_t i = 0; i < r.n; i++) {
            const char* bad = cmp_row(&r, i, segs[k], start - acc + i);
            TEST_ASSERT_FMT(!bad, "parted row %lld: %s", (long long)(start + i), bad ? bad : "");
        }
        start += r.n;
    }
    ray_col_scratch_free(&sc);
    PASS();
}

static test_result_t parted_case(const char* root, int mask, bool ragged) {
    ray_t* t = load_parted(root, mask, ragged);
    TEST_ASSERT_TRUE(t != NULL);
    static const char* cols[] = { "a", "s", "b", "y" };
    for (int c = 0; c < 4; c++) {
        ray_enc_counters_reset();
        int64_t id = ray_sym_intern(cols[c], 1);
        ray_col_src_t s;
        TEST_ASSERT_NULL(ray_table_col_src(t, id, &s));
        TEST_ASSERT_TRUE(s.obj != NULL);
        TEST_ASSERT_EQ_I(s.len, 3000 + 1024 + 70000);         /* ragged b too: its missing rows are nulls */
        TEST_ASSERT_EQ_I(s.type, c == 0 ? RAY_I64 : c == 1 ? RAY_STR : c == 2 ? RAY_F64 : RAY_SYM);
        TEST_ASSERT_TRUE(s.may_null);
        /* reading ranges decodes vectors only: nothing is materialized */
        ray_col_scratch_t sc; ray_col_scratch_init(&sc); ray_col_range_t r;
        TEST_ASSERT_NULL(ray_col_range(&s, 0, 100, &sc, &r));
        TEST_ASSERT_NULL(ray_col_range(&s, s.len - 50, 100, &sc, &r));
        ray_col_scratch_free(&sc);
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        TEST_ASSERT_EQ_U(k.materialized, 0);
        ray_t* w = ray_table_col(t, id);                       /* the materialized reference */
        TEST_ASSERT_TRUE(w && !RAY_IS_ERR(w) && RAY_IS_PARTED(w->type));
        if (ragged && c == 2) {                                /* the load filled the missing segment */
            ray_t* g = ((ray_t**)ray_data(w))[2];
            TEST_ASSERT_TRUE(g && g->type == RAY_F64 && g->len == 70000 && (g->attrs & RAY_ATTR_HAS_NULLS));
            for (int64_t i = 0; i < g->len; i++) TEST_ASSERT_TRUE(ray_vec_is_null(g, i));
        }
        if (c == 3 && (mask & 5) == 5) {                       /* generation 2 keeps each segment's width */
            ray_t** sg = (ray_t**)ray_data(w);
            TEST_ASSERT_TRUE((sg[0]->attrs & RAY_SYM_W_MASK) != (sg[2]->attrs & RAY_SYM_W_MASK));
        }
        static const int64_t reqs[] = { 1000, 1024, 5000, 1 << 20 };
        for (int q = 0; q < 4; q++) RUN(walk_parted(&s, w, reqs[q]));
        /* the boundaries themselves: a range starting at the last row of a segment */
        int64_t bounds[] = { 2999, 3000, 3000 + 1023, 3000 + 1024 };
        for (int q = 0; q < 4 && bounds[q] < s.len; q++) {
            ray_col_scratch_t sc2; ray_col_scratch_init(&sc2);
            TEST_ASSERT_NULL(ray_col_range(&s, bounds[q], 5000, &sc2, &r));
            int64_t seg_end = bounds[q] < 3000 ? 3000 : bounds[q] < 4024 ? 4024 : s.len;
            TEST_ASSERT_TRUE(bounds[q] + r.n <= seg_end);
            if (bounds[q] == 2999 || bounds[q] == 4023) TEST_ASSERT_EQ_I(r.n, 1);
            ray_col_scratch_free(&sc2);
        }
        ray_col_src_release(&s);
    }
    ray_release(t);
    PASS();
}

static test_result_t test_range_parted_mixed(void) {
    RUN(parted_case(CR_TMP "/p5", 0x5, false));       /* gen2, gen0, gen2 */
    RUN(parted_case(CR_TMP "/p2", 0x2, false));       /* gen0, gen2, gen0 */
    RUN(parted_case(CR_TMP "/p0", 0x0, false));       /* all gen0: plain parted wrapper */
    RUN(parted_case(CR_TMP "/p7", 0x7, false));       /* all gen2 */
    PASS();
}

/* A ragged partition's missing column reads as nulls over its rows (#724).
 * A missing segment, which only a hand-built wrapper has, holds no rows,
 * matching ray_parted_nrows (the executor refuses a table whose parted
 * columns' row counts differ, so a scan never meets one). */
static test_result_t test_range_parted_ragged(void) {
    RUN(parted_case(CR_TMP "/r5", 0x5, true));
    RUN(parted_case(CR_TMP "/r0", 0x0, true));
    ray_t* t = load_parted(CR_TMP "/rh", 0x0, false);
    TEST_ASSERT_TRUE(t != NULL);
    ray_t* b = ray_table_col(t, ray_sym_intern("b", 1));
    TEST_ASSERT_TRUE(b && !RAY_IS_ERR(b) && RAY_IS_PARTED(b->type) && b->len == 3);
    ray_t* w = ray_alloc(3 * sizeof(ray_t*));
    TEST_ASSERT_TRUE(w && !RAY_IS_ERR(w));
    w->type = b->type; w->len = 3; w->attrs = 0; memset(w->aux, 0, 16);
    ray_t** ws = (ray_t**)ray_data(w);
    for (int i = 0; i < 3; i++) { ws[i] = i == 1 ? NULL : ((ray_t**)ray_data(b))[i]; if (ws[i]) ray_retain(ws[i]); }
    TEST_ASSERT_EQ_I(ray_parted_nrows(w), 3000 + 70000);
    ray_col_src_t s;
    TEST_ASSERT_NULL(ray_col_src_bind(w, &s));
    TEST_ASSERT_EQ_I(s.len, 3000 + 70000);
    RUN(walk_parted(&s, w, 5000));
    ray_col_src_release(&s);
    ray_release(w);
    ray_release(t);
    PASS();
}

/* Bind refuses shapes range reads cannot serve. */
static test_result_t test_bind_refuses_shapes(void) {
    ray_col_src_t s;
    ray_t fake;                                              /* header only: bind rejects before any use */
    memset(&fake, 0, sizeof fake);
    fake.type = RAY_MAPCOMMON;
    ray_t* e = ray_col_src_bind(&fake, &s);
    TEST_ASSERT_TRUE(e && RAY_IS_ERR(e)); TEST_ASSERT_STR_EQ(ray_err_code(e), "nyi"); ray_error_free(e);
    TEST_ASSERT_TRUE(s.obj == NULL);
    fake.type = RAY_ENCCOL; fake.attrs = RAY_ATTR_SLICE;
    e = ray_col_src_bind(&fake, &s);
    TEST_ASSERT_TRUE(e && RAY_IS_ERR(e)); TEST_ASSERT_STR_EQ(ray_err_code(e), "nyi"); ray_error_free(e);
    fake.type = RAY_PARTED_BASE + RAY_I64;
    e = ray_col_src_bind(&fake, &s);
    TEST_ASSERT_TRUE(e && RAY_IS_ERR(e)); TEST_ASSERT_STR_EQ(ray_err_code(e), "nyi"); ray_error_free(e);
    PASS();
}

static test_result_t test_table_col_src(void) {
    ray_t* v = i32_nulls(2000);
    ray_t* v2 = i32_nulls(2000);
    ray_t* t = ray_table_new(2);
    t = ray_table_add_col(t, ray_sym_intern("x", 1), v);
    ray_release(v);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v2, CR_TMP "/e", 2), RAY_OK);
    ray_release(v2);
    ray_t* c = ray_col_mmap(CR_TMP "/e");
    t = ray_table_add_col(t, ray_sym_intern("y", 1), c);
    ray_release(c);
    ray_enc_counters_reset();
    ray_col_src_t s;
    TEST_ASSERT_NULL(ray_table_col_src(t, ray_sym_intern("x", 1), &s));
    TEST_ASSERT_TRUE(s.obj != NULL && s.len == 2000);
    ray_col_src_release(&s);
    TEST_ASSERT_NULL(ray_table_col_src(t, ray_sym_intern("y", 1), &s));
    TEST_ASSERT_TRUE(s.obj != NULL && s.obj->type == RAY_ENCCOL && s.len == 2000);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);                      /* binding never decodes */
    ray_col_src_release(&s);
    TEST_ASSERT_NULL(ray_table_col_src(t, ray_sym_intern("absent", 6), &s));
    TEST_ASSERT_TRUE(s.obj == NULL);
    ray_release(t);
    PASS();
}

static test_result_t test_range_parted_boundary(void) {
    for (int p = 0; p < 2; p++) {
        char dir[256]; snprintf(dir, sizeof dir, CR_TMP "/pdb/2024.01.0%d/t", p + 1);
        ray_t* t = ray_table_new(1);
        ray_t* a = ray_vec_new(RAY_I64, 3000); a->len = 3000;
        for (int i = 0; i < 3000; i++) ((int64_t*)ray_data(a))[i] = p * 3000 + i;
        t = ray_table_add_col(t, ray_sym_intern("a", 1), a);
        ray_release(a);
        TEST_ASSERT_EQ_I(ray_splay_save_mode(t, dir, NULL, p == 0 ? RAY_COMPRESS_ON : RAY_COMPRESS_OFF), RAY_OK);
        ray_release(t);
    }
    ray_t* t = ray_read_parted(CR_TMP "/pdb", "t");
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_col_src_t s;
    TEST_ASSERT_NULL(ray_table_col_src(t, ray_sym_intern("a", 1), &s));
    TEST_ASSERT_EQ_I(s.len, 6000);
    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
    ray_col_range_t r;
    TEST_ASSERT_NULL(ray_col_range(&s, 2990, 100, &sc, &r));
    TEST_ASSERT_EQ_I(r.n, 10);                                   /* segment boundary */
    TEST_ASSERT_EQ_I(((const int64_t*)r.data)[9], 2999);
    TEST_ASSERT_NULL(ray_col_range(&s, 3000, 100, &sc, &r));
    ray_col_meta_t m; ray_table_col_meta(t, ray_sym_intern("a", 1), &m);
    ray_t* seg1 = ray_enccol_seg(m.enc, 1);                      /* the generation-0 segment */
    TEST_ASSERT_EQ_PTR(r.data, ray_data(seg1));                  /* zero copy */
    TEST_ASSERT_EQ_I(r.n, 100);
    ray_col_scratch_free(&sc); ray_col_src_release(&s); ray_release(t);
    PASS();
}

const test_entry_t colrange_entries[] = {
    { "colrange/gen0_zero_copy",          test_range_gen0_zero_copy,            cr_setup, cr_teardown },
    { "colrange/gen2_vector_clip_nulls",  test_range_gen2_vector_clip_and_nulls, cr_setup, cr_teardown },
    { "colrange/two_str_views",           test_range_two_str_views,             cr_setup, cr_teardown },
    { "colrange/slice_and_long_strings",  test_range_slice_and_long_strings,    cr_setup, cr_teardown },
    { "colrange/error_propagates",        test_range_error_propagates,          cr_setup, cr_teardown },
    { "colrange/bounds_and_scratch",      test_range_bounds_and_scratch,        cr_setup, cr_teardown },
    { "colrange/matches_materialized",    test_range_matches_materialized,      cr_setup, cr_teardown },
    { "colrange/parted_mixed",            test_range_parted_mixed,              cr_setup, cr_teardown },
    { "colrange/parted_ragged",           test_range_parted_ragged,             cr_setup, cr_teardown },
    { "colrange/bind_refuses_shapes",       test_bind_refuses_shapes,             cr_setup, cr_teardown },
    { "colrange/table_col_src",           test_table_col_src,                   cr_setup, cr_teardown },
    { "colrange/parted_boundary",         test_range_parted_boundary,           cr_setup, cr_teardown },
    { NULL, NULL, NULL, NULL },
};
