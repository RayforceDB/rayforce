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

#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include "table/sym.h"
#include "table/table.h"
#include "core/runtime.h"
#include "core/platform.h"
#include "core/pool.h"
#include "lang/eval.h"
#include "lang/format.h"
#include "store/col.h"
#include "store/colrange.h"
#include "store/enccol.h"
#include "store/encfmt.h"
#include "store/splay.h"
#include "store/part.h"
#include "vec/str.h"
#include "vec/vec.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Every path below goes through ray_test_tmp (a per-process root). */
#define SR_TMP "/tmp/rayforce_scanrange"
static const char* sr_root(void) { return ray_test_tmp(SR_TMP); }

/* Heap-only fixtures (Tasks 1-2: vectors saved and mapped directly). */
static void sr_setup(void)    { ray_heap_init(); (void)ray_sym_init(); (void)ray_test_rm_rf(sr_root()); (void)ray_test_mkdir_p(sr_root()); }
static void sr_teardown(void) { ray_test_hooks_off(); ray_sym_destroy(); ray_heap_destroy(); (void)ray_test_rm_rf(sr_root()); }

/* Runtime fixtures without the shared table (parted loads, Rayfall saves). */
static ray_runtime_t* g_srt;
static void sr_rt_setup(void)    { g_srt = ray_runtime_create(0, NULL); (void)ray_test_rm_rf(sr_root()); (void)ray_test_mkdir_p(sr_root()); }
static void sr_rt_teardown(void) { ray_test_hooks_off(); (void)ray_test_rm_rf(sr_root()); ray_runtime_destroy(g_srt); }

/* Evaluate Rayfall that must succeed; "/tmp" paths inside are moved. */
static void sr_eval_ok(const char* src) {
    ray_t* r = ray_eval_str(ray_test_tmp(src));
    if (r && RAY_IS_ERR(r)) { fprintf(stderr, "sr_eval_ok: %s: %s\n", src, ray_error_msg()); ray_error_free(r); abort(); }
    if (r) ray_release(r);
}
static void sr_evalf_ok(const char* fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    sr_eval_ok(buf);
}
/* Fixtures for the later P2 tests in this suite. */
__attribute__((unused)) static ray_t* sr_eval(const char* src) { return ray_eval_str(ray_test_tmp(src)); }
__attribute__((unused)) static void sr_drop(ray_t* o) { if (!o) return; if (RAY_IS_ERR(o)) ray_error_free(o); else ray_release(o); }

static ray_t* sr_i32(int64_t n, int64_t null_every) {
    ray_t* v = ray_vec_new(RAY_I32, n); v->len = n;
    for (int64_t i = 0; i < n; i++)
        ((int32_t*)ray_data(v))[i] = (null_every && i % null_every == 0) ? NULL_I32 : (int32_t)(i * 3 - 7);
    if (null_every) v->attrs |= RAY_ATTR_HAS_NULLS;
    return v;
}
static ray_t* sr_f64(int64_t n, int64_t null_every) {
    ray_t* v = ray_vec_new(RAY_F64, n); v->len = n;
    for (int64_t i = 0; i < n; i++)
        ((double*)ray_data(v))[i] = (null_every && i % null_every == 0) ? NULL_F64 : (double)i * 0.25 - 3.0;
    if (null_every) v->attrs |= RAY_ATTR_HAS_NULLS;
    return v;
}

/* Save `v` as generation 2 under the root as <name> and map it back. */
static ray_t* sr_gen2(ray_t* v, const char* name) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", sr_root(), name);
    if (ray_col_save_gen(v, p, 2) != RAY_OK) return NULL;
    return ray_col_mmap(p);
}

/* A piece function that copies every piece of source 0 (4-byte or 8-byte
 * elements) into out[b - lo .. e - lo) and records the piece count and the
 * attrs / pool of the last piece. */
typedef struct { char* out; int64_t lo; uint8_t esz; int pieces; int64_t max_piece;
                 uint8_t attrs; const char* pool; const void* first_data; } sr_walk_t;
static void sr_piece(void* ctx, uint32_t wid, int64_t b, int64_t e, const ray_col_piece_t* p) {
    (void)wid;
    sr_walk_t* w = ctx;
    memcpy(w->out + (size_t)(b - w->lo) * w->esz, p[0].data, (size_t)(e - b) * w->esz);
    if (!w->pieces) w->first_data = p[0].data;
    w->pieces++;
    if (e - b > w->max_piece) w->max_piece = e - b;
    w->attrs = p[0].attrs; w->pool = p[0].str_pool;
}

static test_result_t test_walk_gen0_zero_copy(void) {
    ray_t* v = sr_i32(20000, 5);
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(v, &s));
    TEST_ASSERT_EQ_PTR(ray_col_src_ready(&s), v);                     /* plain: ready as is */
    TEST_ASSERT_EQ_PTR(ray_col_src_base(&s), ray_data(v));
    ray_col_src_t* srcs[1] = { &s };
    const void* plain[1] = { ray_col_src_base(&s) };
    static char out[20000 * 4];
    sr_walk_t w = { out, 1000, 4, 0, 0, 0, NULL, NULL };
    TEST_ASSERT_NULL(ray_col_walk(srcs, plain, 1, 1000, 9192, false, &w, 0, sr_piece));
    TEST_ASSERT_EQ_I(w.pieces, 1);                                  /* one piece, zero copy */
    TEST_ASSERT_EQ_PTR(w.first_data, (int32_t*)ray_data(v) + 1000);
    ray_col_cursor_t c; ray_col_cursor_init(&c, &s);
    const ray_col_range_t* rv;
    TEST_ASSERT_NULL(ray_col_cursor_at(&c, 1500, &rv));
    TEST_ASSERT_EQ_PTR(rv->data, (int32_t*)ray_data(v) + 1500);   /* plain: pointer identity */
    ray_col_cursor_free(&c); ray_col_src_release(&s); ray_release(v);
    PASS();
}

static test_result_t test_walk_gen2_vectors(void) {
    ray_t* v = sr_i32(20000, 5);
    ray_t* c = sr_gen2(v, "a");
    TEST_ASSERT_TRUE(c && c->type == RAY_ENCCOL);
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(c, &s));
    TEST_ASSERT_TRUE(ray_col_src_encoded(&s));
    TEST_ASSERT_NULL(ray_col_src_ready(&s));                        /* not materialized */
    ray_col_src_t* srcs[1] = { &s };
    const void* plain[1] = { NULL };
    static char out[20000 * 4];
    sr_walk_t w = { out, 2048, 4, 0, 0, 0, NULL, NULL };
    ray_enc_counters_reset();
    TEST_ASSERT_NULL(ray_col_walk(srcs, plain, 1, 2048, 10240, false, &w, 0, sr_piece));
    TEST_ASSERT_EQ_I(w.pieces, 8);                                  /* one per vector */
    TEST_ASSERT_EQ_I(w.max_piece, 1024);
    TEST_ASSERT_TRUE(memcmp(out, (int32_t*)ray_data(v) + 2048, 8192 * 4) == 0);   /* sentinels in data */
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 8192 * 4);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    ray_col_src_release(&s); ray_release(c); ray_release(v);
    PASS();
}

/* Carry 6 / finding I7: once the column is materialized, cursors and the
 * walk read the published vector; nothing is decoded again. */
static test_result_t test_walk_decoded_cache(void) {
    ray_t* v = sr_i32(20000, 5);
    ray_t* c = sr_gen2(v, "dc");
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(c, &s));
    ray_t* pl = NULL;
    TEST_ASSERT_NULL(ray_col_src_plain(&s, &pl));                  /* one materialization */
    TEST_ASSERT_EQ_PTR(ray_col_src_ready(&s), pl);
    TEST_ASSERT_EQ_PTR(ray_col_src_base(&s), ray_data(pl));         /* published: read in place */
    ray_enc_counters_reset();
    ray_col_cursor_t cur; ray_col_cursor_init(&cur, &s);
    const ray_col_range_t* rv;
    TEST_ASSERT_NULL(ray_col_cursor_at(&cur, 3000, &rv));
    TEST_ASSERT_EQ_PTR(rv->data, (int32_t*)ray_data(pl) + 3000);  /* zero copy from the published vector */
    ray_col_cursor_free(&cur);
    ray_col_src_t* srcs[1] = { &s };
    const void* plain[1] = { NULL };                               /* a caller that did not check: still no decode */
    static char out[20000 * 4];
    sr_walk_t w = { out, 0, 4, 0, 0, 0, NULL, NULL };
    TEST_ASSERT_NULL(ray_col_walk(srcs, plain, 1, 0, 20000, false, &w, 0, sr_piece));
    TEST_ASSERT_TRUE(memcmp(out, ray_data(v), 20000 * 4) == 0);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 0);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    ray_col_src_release(&s); ray_release(c); ray_release(v);
    PASS();
}

/* Review focus 1: unaligned bounds over a plain and a generation-2 source
 * at once: pieces end at the generation-2 vector ends, and both sources'
 * pointers address the same rows. */
typedef struct { const int32_t* ref0; const int32_t* ref2; int bad; int pieces; } sr_two_t;
static void sr_two(void* ctx, uint32_t wid, int64_t b, int64_t e, const ray_col_piece_t* p) {
    (void)wid;
    sr_two_t* t = ctx;
    t->pieces++;
    for (int64_t r = b; r < e; r++) {
        if (((const int32_t*)p[0].data)[r - b] != t->ref0[r]) t->bad++;
        if (((const int32_t*)p[1].data)[r - b] != t->ref2[r]) t->bad++;
    }
}
static test_result_t test_walk_unaligned_mixed(void) {
    ray_t* a = sr_i32(20000, 0);
    ray_t* bv = sr_i32(20000, 7);
    ray_t* c = sr_gen2(bv, "u");
    ray_col_src_t s0, s2;
    TEST_ASSERT_NULL(ray_col_src_bind(a, &s0));
    TEST_ASSERT_NULL(ray_col_src_bind(c, &s2));
    ray_col_src_t* srcs[2] = { &s0, &s2 };
    const void* plain[2] = { ray_data(a), NULL };
    for (int64_t lo = 0; lo < 20000; lo += 1777) {
        int64_t hi = lo + 5000 < 20000 ? lo + 5000 : 20000;
        sr_two_t t = { (const int32_t*)ray_data(a), (const int32_t*)ray_data(bv), 0, 0 };
        TEST_ASSERT_NULL(ray_col_walk(srcs, plain, 2, lo, hi, false, &t, 0, sr_two));
        TEST_ASSERT_FMT(t.bad == 0, "lo=%lld", (long long)lo);
        int64_t want = (hi - 1) / 1024 - lo / 1024 + 1;              /* vectors touched */
        TEST_ASSERT_EQ_I(t.pieces, want);
    }
    ray_col_src_release(&s0); ray_col_src_release(&s2);
    ray_release(c); ray_release(a); ray_release(bv);
    PASS();
}

static test_result_t test_walk_str_pool_from_view(void) {
    ray_t* v = ray_vec_new(RAY_STR, 5000);
    char buf[64];
    for (int i = 0; i < 5000; i++) {
        int n = (i % 3 == 0) ? snprintf(buf, sizeof buf, "s%d", i)
                             : snprintf(buf, sizeof buf, "a pooled string of row %d", i);
        v = ray_str_vec_append(v, buf, (size_t)n);
    }
    ray_t* c = sr_gen2(v, "s");
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(c, &s));
    ray_col_cursor_t cur; ray_col_cursor_init(&cur, &s);
    const ray_str_t* x = (const ray_str_t*)ray_data(v);
    const char* px = v->str_pool ? (const char*)ray_data(v->str_pool) : NULL;
    for (int64_t r = 900; r < 4900; r++) {
        const ray_col_range_t* rv;
        TEST_ASSERT_NULL(ray_col_cursor_at(&cur, r, &rv));
        const ray_str_t* y = (const ray_str_t*)rv->data + (r - rv->start);
        TEST_ASSERT_EQ_I(y->len, x[r].len);
        TEST_ASSERT_TRUE(memcmp(ray_str_t_ptr(y, rv->str_pool), ray_str_t_ptr(&x[r], px), y->len) == 0);
    }
    ray_col_cursor_free(&cur); ray_col_src_release(&s);
    /* A plain STR source read in place carries its column's pool. */
    ray_col_src_t sp; TEST_ASSERT_NULL(ray_col_src_bind(v, &sp));
    ray_col_src_t* srcs[1] = { &sp };
    const void* plain[1] = { ray_data(v) };
    static char out[5000 * sizeof(ray_str_t)];
    sr_walk_t w = { out, 100, (uint8_t)sizeof(ray_str_t), 0, 0, 0, NULL, NULL };
    TEST_ASSERT_NULL(ray_col_walk(srcs, plain, 1, 100, 4100, false, &w, 0, sr_piece));
    TEST_ASSERT_EQ_I(w.pieces, 1);
    TEST_ASSERT_EQ_PTR(w.pool, px);
    ray_col_src_release(&sp); ray_release(c); ray_release(v);
    PASS();
}

/* Review focus 2: a parted SYM source whose segments have different saved
 * width classes (W8 and W16): each piece carries its own segment's attrs,
 * and the values equal the materialized wrapper's. */
typedef struct { ray_t* plain; int bad; uint8_t widths[2]; } sr_sym_t;
static void sr_sym(void* ctx, uint32_t wid, int64_t b, int64_t e, const ray_col_piece_t* p) {
    (void)wid;
    sr_sym_t* t = ctx;
    ray_t** segs = (ray_t**)ray_data(t->plain);                     /* not-a-table: a parted wrapper */
    for (int64_t r = b; r < e; r++) {
        ray_t* seg = r < 1500 ? segs[0] : segs[1];
        int64_t want = ray_read_sym(ray_data(seg), r < 1500 ? r : r - 1500, RAY_SYM, seg->attrs);
        if (ray_read_sym(p[0].data, r - b, RAY_SYM, p[0].attrs) != want) t->bad++;
    }
    t->widths[b < 1500 ? 0 : 1] = p[0].attrs & RAY_SYM_W_MASK;
}
static test_result_t test_walk_sym_widths(void) {
    char dir[512], sym[512];
    snprintf(sym, sizeof sym, "%s/pw/.sym", sr_root());
    for (int p = 0; p < 2; p++) {
        int64_t n = 1500, distinct = p ? 300 : 20;            /* W8 (< 256 ids) then W16 */
        ray_t* s = ray_sym_vec_new(RAY_SYM_W64, n); s->len = n;
        for (int64_t i = 0; i < n; i++) {
            char b2[16]; int nb = snprintf(b2, sizeof b2, "w%lld", (long long)(i % distinct));
            ((int64_t*)ray_data(s))[i] = ray_sym_intern(b2, (size_t)nb);
        }
        ray_t* t = ray_table_new(1);
        t = ray_table_add_col(t, ray_sym_intern("s", 1), s); ray_release(s);
        snprintf(dir, sizeof dir, "%s/pw/2024.01.0%d/t", sr_root(), p + 1);
        TEST_ASSERT_EQ_I(ray_splay_save_mode(t, dir, sym, RAY_COMPRESS_ON), RAY_OK);
        ray_release(t);
    }
    snprintf(dir, sizeof dir, "%s/pw", sr_root());
    ray_t* t = ray_read_parted(dir, "t");
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_col_src_t s; TEST_ASSERT_NULL(ray_table_col_src(t, ray_sym_intern("s", 1), &s));
    ray_col_src_t* srcs[1] = { &s };
    const void* pl[1] = { NULL };
    ray_t* plain = ray_table_col(t, ray_sym_intern("s", 1));        /* reference: the materialized wrapper */
    TEST_ASSERT_TRUE(plain && !RAY_IS_ERR(plain));
    sr_sym_t st = { plain, 0, { 0, 0 } };
    TEST_ASSERT_NULL(ray_col_walk(srcs, pl, 1, 1000, 2500, false, &st, 0, sr_sym));   /* crosses the segment boundary */
    TEST_ASSERT_EQ_I(st.bad, 0);
    TEST_ASSERT_TRUE(st.widths[0] != st.widths[1]);
    ray_col_src_release(&s); ray_release(t);
    PASS();
}

/* Finding I5: a ragged generation-0 parted table (partition 3 lacks v)
 * reads its synthetic all-null segment through the walk, never
 * materialized, with the eager fill's values (NULL_I32 rows). */
static test_result_t test_walk_ragged_parted(void) {
    for (int p = 0; p < 3; p++)
        sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/rg/2024.01.0%d/t/\" %s {compress: false})", p + 1,
                    p == 2 ? "(table [a] (list (til 3000)))"
                           : "(table [a v] (list (til 3000) (as 'I32 (map (fn [j] (if (== 0 (% j 9)) 0N j)) (til 3000)))))");
    char root[512]; snprintf(root, sizeof root, "%s/rg", sr_root());
    ray_t* t = ray_read_parted(root, "t");
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_col_src_t s; TEST_ASSERT_NULL(ray_table_col_src(t, ray_sym_intern("v", 1), &s));
    TEST_ASSERT_TRUE(ray_col_src_encoded(&s) && s.parted);           /* RAY_ENCCOL_PARTED: synthetic segment */
    TEST_ASSERT_NULL(ray_col_src_ready(&s));
    static char out[9000 * 4];
    ray_col_src_t* srcs[1] = { &s };
    const void* pl[1] = { NULL };
    sr_walk_t w = { out, 0, 4, 0, 0, 0, NULL, NULL };
    ray_enc_counters_reset();
    TEST_ASSERT_NULL(ray_col_walk(srcs, pl, 1, 0, 9000, false, &w, 0, sr_piece));
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    ray_t* ref = ray_table_col(t, ray_sym_intern("v", 1));          /* the eager reference (materializes now) */
    TEST_ASSERT_TRUE(ref && !RAY_IS_ERR(ref));
    ray_t** segs = (ray_t**)ray_data(ref);                          /* not-a-table: a parted wrapper */
    for (int64_t r = 0; r < 9000; r++) {
        int32_t want = ((const int32_t*)ray_data(segs[r / 3000]))[r % 3000];
        TEST_ASSERT_FMT(((const int32_t*)out)[r] == want, "row %lld", (long long)r);
    }
    for (int64_t r = 6000; r < 9000; r++) TEST_ASSERT_EQ_I(((const int32_t*)out)[r], NULL_I32);
    ray_col_src_release(&s); ray_release(t);
    PASS();
}

/* Sparse ascending rows: each touched vector is decoded once.  The last
 * vector of a 10000-row column holds 784 rows. */
static test_result_t test_cursor_reuses_vector(void) {
    ray_t* v = sr_i32(10000, 0);
    ray_t* c = sr_gen2(v, "c");
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(c, &s));
    ray_col_cursor_t cur; ray_col_cursor_init(&cur, &s);
    ray_enc_counters_reset();
    static const int64_t rows[] = { 0, 17, 1023, 1024, 1500, 7168, 7169, 9216, 9999 };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        const ray_col_range_t* rv;
        TEST_ASSERT_NULL(ray_col_cursor_at(&cur, rows[i], &rv));
        TEST_ASSERT_EQ_I(((const int32_t*)rv->data)[rows[i] - rv->start], ((int32_t*)ray_data(v))[rows[i]]);
    }
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.decoded_bytes, (3 * 1024 + 784) * 4);   /* vectors 0, 1, 7 and 9 (784 rows) */
    ray_col_cursor_free(&cur); ray_col_src_release(&s); ray_release(c); ray_release(v);
    PASS();
}

/* Contiguous mode: one piece, floats copied, nulls (NaN) preserved. */
static test_result_t test_walk_contiguous_floats(void) {
    ray_t* v = sr_f64(20000, 13);
    ray_t* c = sr_gen2(v, "f");
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(c, &s));
    ray_col_src_t* srcs[1] = { &s };
    const void* plain[1] = { NULL };
    static char out[20000 * 8];
    sr_walk_t w = { out, 1000, 8, 0, 0, 0, NULL, NULL };
    TEST_ASSERT_NULL(ray_col_walk(srcs, plain, 1, 1000, 9192, true, &w, 0, sr_piece));
    TEST_ASSERT_EQ_I(w.pieces, 1);
    TEST_ASSERT_TRUE(memcmp(out, (double*)ray_data(v) + 1000, 8192 * 8) == 0);   /* bit-identical, NaN payloads too */
    ray_col_src_release(&s); ray_release(c); ray_release(v);
    PASS();
}

static test_result_t test_scan_fail_lowest_key(void) {
    ray_scan_fail_t f; ray_scan_fail_init(&f);
    TEST_ASSERT_FALSE(ray_scan_failed(&f));
    TEST_ASSERT_NULL(ray_scan_fail_error(&f));
    ray_scan_fail_note(&f, 16384, ray_error("corrupt", "col /x: chunk 2 late"));
    ray_scan_fail_note(&f, 8192, ray_error("corrupt", "col /x: chunk 1 early"));
    ray_scan_fail_note(&f, 24576, ray_error("oom", "later"));
    TEST_ASSERT_TRUE(ray_scan_failed(&f));
    ray_error_clear();
    ray_t* e = ray_scan_fail_error(&f);
    TEST_ASSERT_TRUE(e && RAY_IS_ERR(e));
    TEST_ASSERT_STR_EQ(ray_err_code(e), "corrupt");
    TEST_ASSERT_TRUE(strstr(ray_error_msg(), "chunk 1 early") != NULL);
    ray_error_free(e);
    PASS();
}

static test_result_t test_raw_is_null(void) {
    int32_t i32[3] = { 1, NULL_I32, 0 };
    double  f64[2] = { 0.0, NULL_F64 };
    uint16_t sym[2] = { 0, 7 };
    TEST_ASSERT_FALSE(ray_raw_is_null(RAY_I32, 0, i32, 0));
    TEST_ASSERT_TRUE(ray_raw_is_null(RAY_I32, 0, i32, 1));
    TEST_ASSERT_TRUE(ray_raw_is_null(RAY_F64, 0, f64, 1));
    TEST_ASSERT_TRUE(ray_raw_is_null(RAY_SYM, RAY_SYM_W16, sym, 0));
    TEST_ASSERT_FALSE(ray_raw_is_null(RAY_SYM, RAY_SYM_W16, sym, 1));
    uint8_t u8[1] = { 0 };
    TEST_ASSERT_FALSE(ray_raw_is_null(RAY_U8, 0, u8, 0));
    PASS();
}

/* ---- Fix round 1: envelope determinism, cancel, parted lookup, gaps ---- */

/* Two threads note keys concurrently: the lowest always wins, whichever
 * thread holds it. */
typedef struct { ray_scan_fail_t* f; int64_t base; _Atomic(int)* go; } sr_noter_t;
static void sr_noter(void* raw) {
    sr_noter_t* t = raw;
    while (!atomic_load(t->go)) RAY_CPU_RELAX();
    for (int64_t k = 40; k >= 0; k--) {
        int64_t key = (t->base + 2 * k) * 8192;
        ray_scan_fail_note(t->f, key, ray_error("corrupt", "key %lld", (long long)key));
    }
    ray_heap_abandon();
}
static test_result_t test_scan_fail_concurrent(void) {
    for (int it = 0; it < 200; it++) {
        ray_scan_fail_t f; ray_scan_fail_init(&f);
        _Atomic(int) go = 0;
        sr_noter_t a = { &f, it & 1, &go }, b = { &f, !(it & 1), &go };   /* key 0 alternates threads */
        ray_thread_t ta, tb;
        TEST_ASSERT_EQ_I(ray_thread_create(&ta, sr_noter, &a), RAY_OK);
        TEST_ASSERT_EQ_I(ray_thread_create(&tb, sr_noter, &b), RAY_OK);
        atomic_store(&go, 1);
        ray_thread_join(ta); ray_thread_join(tb);
        ray_error_clear();
        ray_t* e = ray_scan_fail_error(&f);
        TEST_ASSERT_TRUE(e && RAY_IS_ERR(e));
        TEST_ASSERT_FMT(strcmp(ray_error_msg(), "key 0") == 0, "it=%d msg=%s", it, ray_error_msg());
        ray_error_free(e);
    }
    PASS();
}

/* The early-return rule: a task skips only when a lower key failed, so a
 * skipped task never hides a lower failure. */
static test_result_t test_scan_fail_skip(void) {
    ray_scan_fail_t f; ray_scan_fail_init(&f);
    TEST_ASSERT_FALSE(ray_scan_fail_skip(&f, 0));
    ray_scan_fail_note(&f, 8192, ray_error("corrupt", "mid"));
    TEST_ASSERT_TRUE(ray_scan_fail_skip(&f, 16384));
    TEST_ASSERT_FALSE(ray_scan_fail_skip(&f, 8192));
    TEST_ASSERT_FALSE(ray_scan_fail_skip(&f, 0));                 /* below the lowest: still runs */
    ray_scan_fail_note(&f, 0, ray_error("oom", "low"));
    ray_error_clear();
    ray_t* e = ray_scan_fail_error(&f);
    TEST_ASSERT_STR_EQ(ray_err_code(e), "oom");
    ray_error_free(e);
    PASS();
}

/* Cancel through the pool: the tasks the pool skips note nothing, and the
 * finish reports `cancel`; a noted failure stands; no cancel, no error. */
typedef struct { ray_scan_fail_t f; _Atomic(int64_t) ran; int64_t fail_at; } sr_disp_t;
static void sr_disp_task(void* ctx, uint32_t wid, int64_t lo, int64_t hi) {
    (void)wid; (void)hi;
    sr_disp_t* d = ctx;
    if (ray_scan_fail_skip(&d->f, lo)) return;
    atomic_fetch_add(&d->ran, 1);
    if (lo == d->fail_at) ray_scan_fail_note(&d->f, lo, ray_error("corrupt", "task %lld", (long long)lo));
}
static test_result_t test_scan_fail_cancel(void) {
    ray_pool_t* pool = ray_pool_get();
    TEST_ASSERT_NOT_NULL(pool);
    sr_disp_t d; ray_scan_fail_init(&d.f); atomic_store(&d.ran, 0); d.fail_at = -1;
    ray_pool_dispatch_n(pool, sr_disp_task, &d, 64);
    TEST_ASSERT_EQ_I(atomic_load(&d.ran), 64);
    TEST_ASSERT_NULL(ray_scan_fail_finish(&d.f, pool));
    ray_cancel();                                                   /* the pool skips every task */
    ray_scan_fail_init(&d.f); atomic_store(&d.ran, 0);
    ray_pool_dispatch_n(pool, sr_disp_task, &d, 64);
    ray_t* e = ray_scan_fail_finish(&d.f, pool);
    ray_cancel_reset();
    TEST_ASSERT_EQ_I(atomic_load(&d.ran), 0);
    TEST_ASSERT_TRUE(e && RAY_IS_ERR(e));
    TEST_ASSERT_STR_EQ(ray_err_code(e), "cancel");
    ray_error_free(e);
    ray_request_interrupt();                                        /* the interrupt alone also cancels */
    ray_scan_fail_init(&d.f);
    e = ray_scan_fail_finish(&d.f, NULL);
    TEST_ASSERT_TRUE(e && RAY_IS_ERR(e));
    TEST_ASSERT_STR_EQ(ray_err_code(e), "cancel");
    ray_error_free(e);
    ray_scan_fail_note(&d.f, 3, ray_error("corrupt", "noted"));     /* a noted failure stands */
    e = ray_scan_fail_finish(&d.f, pool);
    ray_clear_interrupt();
    TEST_ASSERT_STR_EQ(ray_err_code(e), "corrupt");
    ray_error_free(e);
    PASS();
}

/* A hand-built plain parted wrapper of nseg I64 segments of seglen rows
 * (value = global row). */
static ray_t* sr_parted_i64(int64_t nseg, int64_t seglen) {
    ray_t* w = ray_alloc((size_t)nseg * sizeof(ray_t*));
    w->type = RAY_PARTED_BASE + RAY_I64; w->len = nseg;
    for (int64_t i = 0; i < nseg; i++) {
        ray_t* v = ray_vec_new(RAY_I64, seglen); v->len = seglen;
        for (int64_t j = 0; j < seglen; j++) ((int64_t*)ray_data(v))[j] = i * seglen + j;
        ((ray_t**)ray_data(w))[i] = v;
    }
    return w;
}
typedef struct { int64_t pieces, bad; } sr_seq_t;
static void sr_seq(void* ctx, uint32_t wid, int64_t b, int64_t e, const ray_col_piece_t* p) {
    (void)wid;
    sr_seq_t* t = ctx;
    t->pieces++;
    for (int64_t r = b; r < e; r++) if (((const int64_t*)p[0].data)[r - b] != r) t->bad++;
}
static double sr_now(void) { struct timespec ts; timespec_get(&ts, TIME_UTC); return (double)ts.tv_sec + ts.tv_nsec * 1e-9; }
/* Best of 3 full walks; checks values and the piece count. */
static double sr_walk_parted(ray_t* obj, int64_t nseg, int* ok) {
    ray_col_src_t s; *ok = 0;
    if (ray_col_src_bind(obj, &s)) return 0;
    ray_col_src_t* srcs[1] = { &s };
    const void* pl[1] = { ray_col_src_base(&s) };                   /* parted: NULL */
    double best = 1e9; int good = pl[0] == NULL;
    for (int k = 0; k < 3; k++) {
        sr_seq_t t = { 0, 0 };
        double t0 = sr_now();
        ray_t* e = ray_col_walk(srcs, pl, 1, 0, s.len, false, &t, 0, sr_seq);
        double dt = sr_now() - t0;
        if (e) { ray_error_free(e); good = 0; }
        if (t.bad || t.pieces != nseg) good = 0;
        if (dt < best) best = dt;
    }
    ray_col_src_release(&s);
    *ok = good;
    return best;
}
/* 10,000 small segments: a full walk is linear in rows (segment found by
 * binary search over prefix offsets, not by a scan from the first). */
static test_result_t test_walk_parted_10k(void) {
    int ok;
    ray_t* w1 = sr_parted_i64(1000, 10);
    ray_t* w10 = sr_parted_i64(10000, 10);
    double t1 = sr_walk_parted(w1, 1000, &ok);   TEST_ASSERT_TRUE(ok);
    double t10 = sr_walk_parted(w10, 10000, &ok); TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FMT(t10 < 40 * t1 + 0.005, "1k %.3f ms, 10k %.3f ms", t1 * 1e3, t10 * 1e3);   /* linear ~10x; quadratic ~100x */
    ray_t* ep = ray_enccol_parted_new(w10);                         /* the RAY_ENCCOL_PARTED form */
    TEST_ASSERT_TRUE(ep && !RAY_IS_ERR(ep));
    double te = sr_walk_parted(ep, 10000, &ok); TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FMT(te < 40 * t1 + 0.005, "1k %.3f ms, enc 10k %.3f ms", t1 * 1e3, te * 1e3);
    /* Cursor jumps (backwards too) find the right segment. */
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(ep, &s));
    ray_col_cursor_t c; ray_col_cursor_init(&c, &s);
    static const int64_t rows[] = { 99999, 0, 50005, 9, 10, 77777 };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        const ray_col_range_t* v;
        TEST_ASSERT_NULL(ray_col_cursor_at(&c, rows[i], &v));
        TEST_ASSERT_EQ_I(((const int64_t*)v->data)[rows[i] - v->start], rows[i]);
        TEST_ASSERT_EQ_I(v->start + v->n, (rows[i] / 10 + 1) * 10);    /* clipped at the segment */
    }
    ray_col_cursor_free(&c); ray_col_src_release(&s);
    ray_release(ep); ray_release(w10); ray_release(w1);
    PASS();
}

/* A NULL source is skipped (zero piece); more sources than the stack
 * holds take the heap arrays. */
typedef struct { int64_t pieces; int bad; uint32_t n; const int32_t* ref; } sr_many_t;
static void sr_many(void* ctx, uint32_t wid, int64_t b, int64_t e, const ray_col_piece_t* p) {
    (void)wid;
    sr_many_t* t = ctx;
    t->pieces++;
    if (p[0].data != NULL) t->bad++;
    for (uint32_t i = 1; i < t->n; i++)
        for (int64_t r = b; r < e; r++)
            if (((const int32_t*)p[i].data)[r - b] != t->ref[r]) t->bad++;
}
static test_result_t test_walk_null_and_many_sources(void) {
    ray_t* v = sr_i32(5000, 4);
    ray_t* c = sr_gen2(v, "m");
    ray_col_src_t sp, se;
    TEST_ASSERT_NULL(ray_col_src_bind(v, &sp));
    TEST_ASSERT_NULL(ray_col_src_bind(c, &se));
    enum { NS = 20 };                                               /* > the walk's stack arrays */
    ray_col_src_t* srcs[NS]; const void* pl[NS];
    srcs[0] = NULL; pl[0] = NULL;
    for (int i = 1; i < NS; i++) {
        srcs[i] = (i & 1) ? &se : &sp;
        pl[i] = (i & 1) ? NULL : ray_col_src_base(&sp);
    }
    sr_many_t t = { 0, 0, NS, (const int32_t*)ray_data(v) };
    TEST_ASSERT_NULL(ray_col_walk(srcs, pl, NS, 100, 4100, false, &t, 0, sr_many));
    TEST_ASSERT_EQ_I(t.bad, 0);
    TEST_ASSERT_EQ_I(t.pieces, 4100 / 1024 - 100 / 1024 + 1);
    ray_col_src_release(&sp); ray_col_src_release(&se); ray_release(c); ray_release(v);
    PASS();
}

/* Errors: a non-float cursor source in contiguous mode is nyi; a cursor
 * outside the source is a range error. */
static test_result_t test_walk_contig_nyi_and_cursor_range(void) {
    ray_t* v = sr_i32(5000, 0);
    ray_t* c = sr_gen2(v, "n");
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(c, &s));
    ray_col_src_t* srcs[1] = { &s };
    const void* pl[1] = { NULL };
    static char out[5000 * 4];
    sr_walk_t w = { out, 0, 4, 0, 0, 0, NULL, NULL };
    ray_t* e = ray_col_walk(srcs, pl, 1, 0, 4000, true, &w, 0, sr_piece);
    TEST_ASSERT_TRUE(e && RAY_IS_ERR(e));
    TEST_ASSERT_STR_EQ(ray_err_code(e), "nyi");
    ray_error_free(e);
    TEST_ASSERT_EQ_I(w.pieces, 0);
    e = ray_col_walk(srcs, pl, 1, 0, RAY_COL_BLOCK_ROWS + 1, true, &w, 0, sr_piece);
    TEST_ASSERT_STR_EQ(ray_err_code(e), "range");                   /* contiguous block bound */
    ray_error_free(e);
    ray_col_cursor_t cur; ray_col_cursor_init(&cur, &s);
    const ray_col_range_t* rv = (const ray_col_range_t*)1;
    e = ray_col_cursor_at(&cur, 5000, &rv);
    TEST_ASSERT_STR_EQ(ray_err_code(e), "range");
    TEST_ASSERT_NULL(rv);
    ray_error_free(e);
    e = ray_col_cursor_at(&cur, -1, &rv);
    TEST_ASSERT_STR_EQ(ray_err_code(e), "range");
    ray_error_free(e);
    ray_col_cursor_free(&cur); ray_col_src_release(&s); ray_release(c); ray_release(v);
    PASS();
}

/* Corrupt chunk 1 of a three-chunk column: the walk hands out the pieces
 * before it, then returns `corrupt` and calls fn no more. */
static test_result_t test_walk_error_mid_walk(void) {
    ray_t* v = sr_i32(3 * 65536, 0);
    char p[512]; snprintf(p, sizeof p, "%s/bad", sr_root());
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    FILE* f = fopen(p, "r+b"); TEST_ASSERT_TRUE(f != NULL);
    ray_enc_dirent_t d;
    fseek(f, RAY_ENC_DIR_OFF + (long)sizeof d, SEEK_SET);           /* chunk 1's directory entry */
    TEST_ASSERT_EQ_I((int)fread(&d, sizeof d, 1, f), 1);
    fseek(f, (long)d.offset + 6, SEEK_SET); fputc(1, f);            /* vector 0's descriptor rsv0 */
    fclose(f);
    ray_t* c = ray_col_mmap(p);
    TEST_ASSERT_TRUE(c && c->type == RAY_ENCCOL);
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(c, &s));
    ray_col_src_t* srcs[1] = { &s };
    const void* pl[1] = { NULL };
    static char out[3 * 65536 * 4];
    sr_walk_t w = { out, 60000, 4, 0, 0, 0, NULL, NULL };
    ray_t* e = ray_col_walk(srcs, pl, 1, 60000, 70000, false, &w, 0, sr_piece);
    TEST_ASSERT_TRUE(e && RAY_IS_ERR(e));
    TEST_ASSERT_STR_EQ(ray_err_code(e), "corrupt");
    ray_error_free(e);
    TEST_ASSERT_EQ_I(w.pieces, 6);                                  /* rows 60000..65535: vectors 58..63 */
    TEST_ASSERT_TRUE(memcmp(out, (int32_t*)ray_data(v) + 60000, (65536 - 60000) * 4) == 0);
    ray_col_src_release(&s); ray_release(c); ray_release(v);
    PASS();
}

/* A plain slice reads in place, at the slice's rows. */
static test_result_t test_walk_plain_slice(void) {
    ray_t* v = sr_i32(5000, 3);
    ray_t* sl = ray_vec_slice(v, 700, 3000);
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(sl, &s));
    TEST_ASSERT_EQ_PTR(ray_col_src_base(&s), (int32_t*)ray_data(v) + 700);
    ray_col_src_t* srcs[2] = { &s, &s };
    const void* pl[2] = { ray_col_src_base(&s), NULL };             /* in place and by cursor */
    sr_two_t t = { (const int32_t*)ray_data(v) + 700, (const int32_t*)ray_data(v) + 700, 0, 0 };
    TEST_ASSERT_NULL(ray_col_walk(srcs, pl, 2, 10, 2990, false, &t, 0, sr_two));
    TEST_ASSERT_EQ_I(t.bad, 0);
    TEST_ASSERT_EQ_I(t.pieces, 1);                                  /* the cursor view runs to the slice end */
    ray_col_src_release(&s); ray_release(sl); ray_release(v);
    PASS();
}

/* A materialized RAY_ENCCOL_PARTED (ragged table) walks the published
 * wrapper: nothing is decoded, values as the eager fill. */
static test_result_t test_walk_materialized_parted(void) {
    for (int p = 0; p < 3; p++)
        sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/mp/2024.01.0%d/t/\" %s {compress: false})", p + 1,
                    p == 1 ? "(table [a] (list (til 3000)))"
                           : "(table [a v] (list (til 3000) (as 'I32 (map (fn [j] (if (== 0 (% j 7)) 0N j)) (til 3000)))))");
    char root[512]; snprintf(root, sizeof root, "%s/mp", sr_root());
    ray_t* t = ray_read_parted(root, "t");
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_col_src_t s; TEST_ASSERT_NULL(ray_table_col_src(t, ray_sym_intern("v", 1), &s));
    TEST_ASSERT_TRUE(ray_col_src_encoded(&s) && s.parted);
    ray_t* ref = ray_table_col(t, ray_sym_intern("v", 1));          /* materializes the wrapper */
    TEST_ASSERT_TRUE(ref && !RAY_IS_ERR(ref));
    TEST_ASSERT_EQ_PTR(ray_col_src_ready(&s), ref);
    TEST_ASSERT_NULL(ray_col_src_base(&s));                          /* parted: no single base */
    ray_enc_counters_reset();
    static char out[9000 * 4];
    ray_col_src_t* srcs[1] = { &s };
    const void* pl[1] = { NULL };
    sr_walk_t w = { out, 0, 4, 0, 0, 0, NULL, NULL };
    TEST_ASSERT_NULL(ray_col_walk(srcs, pl, 1, 0, 9000, false, &w, 0, sr_piece));
    TEST_ASSERT_EQ_I(w.pieces, 3);                                  /* one per segment */
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 0);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    ray_t** segs = (ray_t**)ray_data(ref);
    for (int64_t r = 0; r < 9000; r++)
        TEST_ASSERT_EQ_I(((const int32_t*)out)[r], ((const int32_t*)ray_data(segs[r / 3000]))[r % 3000]);
    ray_col_src_release(&s); ray_release(t);
    PASS();
}

/* Contiguous mode over a plain parted F64 source (segment 0 null-free,
 * segment 1 with nulls): a block inside one segment is that segment's view
 * (no copy, its own may_null); a block across both is copied and its
 * may_null is the OR of the views. */
typedef struct { const void* data; bool may_null; int pieces; double first; } sr_contig_t;
static void sr_contig(void* ctx, uint32_t wid, int64_t b, int64_t e, const ray_col_piece_t* p) {
    (void)wid; (void)b; (void)e;
    sr_contig_t* t = ctx;
    t->pieces++; t->data = p[0].data; t->may_null = p[0].may_null; t->first = ((const double*)p[0].data)[0];
}
static test_result_t test_walk_contig_views(void) {
    ray_t* s0 = sr_f64(3000, 0);
    ray_t* s1 = sr_f64(3000, 5);
    ray_t* w = ray_alloc(2 * sizeof(ray_t*));
    w->type = RAY_PARTED_BASE + RAY_F64; w->len = 2;
    ((ray_t**)ray_data(w))[0] = s0; ((ray_t**)ray_data(w))[1] = s1;
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(w, &s));
    TEST_ASSERT_TRUE(s.may_null);
    ray_col_src_t* srcs[1] = { &s };
    const void* pl[1] = { NULL };
    sr_contig_t t = { 0 };
    TEST_ASSERT_NULL(ray_col_walk(srcs, pl, 1, 500, 2500, true, &t, 0, sr_contig));
    TEST_ASSERT_EQ_I(t.pieces, 1);
    TEST_ASSERT_EQ_PTR(t.data, (double*)ray_data(s0) + 500);       /* one view covers it: no copy */
    TEST_ASSERT_FALSE(t.may_null);
    t = (sr_contig_t){ 0 };
    TEST_ASSERT_NULL(ray_col_walk(srcs, pl, 1, 2500, 3500, true, &t, 0, sr_contig));
    TEST_ASSERT_EQ_I(t.pieces, 1);
    TEST_ASSERT_TRUE(t.data != (double*)ray_data(s0) + 2500);       /* copied */
    TEST_ASSERT_TRUE(t.first == ((double*)ray_data(s0))[2500]);
    TEST_ASSERT_TRUE(t.may_null);                                   /* OR of the two views */
    ray_col_src_release(&s); ray_release(w);
    PASS();
}

const test_entry_t scanrange_entries[] = {
    { "scanrange/walk_gen0_zero_copy",     test_walk_gen0_zero_copy,     sr_setup,    sr_teardown },
    { "scanrange/walk_gen2_vectors",       test_walk_gen2_vectors,       sr_setup,    sr_teardown },
    { "scanrange/walk_decoded_cache",      test_walk_decoded_cache,      sr_setup,    sr_teardown },
    { "scanrange/walk_unaligned_mixed",    test_walk_unaligned_mixed,    sr_setup,    sr_teardown },
    { "scanrange/walk_str_pool_from_view", test_walk_str_pool_from_view, sr_setup,    sr_teardown },
    { "scanrange/walk_sym_widths",         test_walk_sym_widths,         sr_rt_setup, sr_rt_teardown },
    { "scanrange/walk_ragged_parted",      test_walk_ragged_parted,      sr_rt_setup, sr_rt_teardown },
    { "scanrange/cursor_reuses_vector",    test_cursor_reuses_vector,    sr_setup,    sr_teardown },
    { "scanrange/walk_contiguous_floats",  test_walk_contiguous_floats,  sr_setup,    sr_teardown },
    { "scanrange/scan_fail_lowest_key",    test_scan_fail_lowest_key,    sr_setup,    sr_teardown },
    { "scanrange/raw_is_null",             test_raw_is_null,             sr_setup,    sr_teardown },
    { "scanrange/scan_fail_concurrent",    test_scan_fail_concurrent,    sr_setup,    sr_teardown },
    { "scanrange/scan_fail_skip",          test_scan_fail_skip,          sr_setup,    sr_teardown },
    { "scanrange/scan_fail_cancel",        test_scan_fail_cancel,        sr_rt_setup, sr_rt_teardown },
    { "scanrange/walk_parted_10k",         test_walk_parted_10k,         sr_setup,    sr_teardown },
    { "scanrange/walk_null_and_many",      test_walk_null_and_many_sources, sr_setup, sr_teardown },
    { "scanrange/walk_contig_nyi_range",   test_walk_contig_nyi_and_cursor_range, sr_setup, sr_teardown },
    { "scanrange/walk_error_mid_walk",     test_walk_error_mid_walk,     sr_setup,    sr_teardown },
    { "scanrange/walk_plain_slice",        test_walk_plain_slice,        sr_setup,    sr_teardown },
    { "scanrange/walk_materialized_parted", test_walk_materialized_parted, sr_rt_setup, sr_rt_teardown },
    { "scanrange/walk_contig_views",       test_walk_contig_views,       sr_setup,    sr_teardown },
    { NULL, NULL, NULL, NULL },
};
