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
#include "ops/internal.h"
#include "ops/rowsel.h"
#include "ops/idxop.h"
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
    ray_col_cursor_free(&cur);                                 /* counted when the scratch goes */
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.decoded_bytes, (3 * 1024 + 784) * 4);   /* vectors 0, 1, 7 and 9 (784 rows) */
    ray_col_src_release(&s); ray_release(c); ray_release(v);
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
    ray_scan_fail_note(&d.f, 3, ray_error("corrupt", "noted"));     /* a noted failure stands */
    ray_t* e2 = ray_scan_fail_finish(&d.f, pool);
    ray_clear_interrupt();                                          /* before any assert: none leaks */
    TEST_ASSERT_TRUE(e && RAY_IS_ERR(e));
    TEST_ASSERT_STR_EQ(ray_err_code(e), "cancel");
    ray_error_free(e);
    TEST_ASSERT_TRUE(e2 && RAY_IS_ERR(e2));
    TEST_ASSERT_STR_EQ(ray_err_code(e2), "corrupt");
    ray_error_free(e2);
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

/* ---- Task 2: directory statistics and the read-ahead byte range ---- */

static test_result_t test_chunk_stat_matches_data(void) {
    int64_t n = 3 * 65536 + 100;
    ray_t* v = sr_i32(n, 11);
    ray_t* c = sr_gen2(v, "st");
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(c, &s));
    for (int64_t ch = 0; ch < 4; ch++) {
        ray_enc_chunk_stat_t st;
        TEST_ASSERT_TRUE(ray_col_src_chunk_stat(&s, ch, &st));
        int64_t lo = ch * 65536, hi = lo + 65536 < n ? lo + 65536 : n;
        int64_t mn = INT64_MAX, mx = INT64_MIN, sum = 0; uint32_t nn = 0;
        for (int64_t i = lo; i < hi; i++) {
            int32_t x = ((int32_t*)ray_data(v))[i];
            if (x == NULL_I32) { nn++; continue; }
            if (x < mn) mn = x;
            if (x > mx) mx = x;
            sum += x;
        }
        TEST_ASSERT_EQ_I(st.rows, hi - lo);
        TEST_ASSERT_TRUE(st.has_minmax);
        TEST_ASSERT_EQ_I(st.min, mn); TEST_ASSERT_EQ_I(st.max, mx);
        TEST_ASSERT_EQ_U(st.null_count, nn);
        TEST_ASSERT_EQ_U(st.sum_lo, (uint64_t)sum);                 /* the exact sum, sign-extended */
        TEST_ASSERT_EQ_I(st.sum_hi, sum < 0 ? -1 : 0);
    }
    ray_enc_chunk_stat_t st;
    TEST_ASSERT_FALSE(ray_col_src_chunk_stat(&s, 4, &st));          /* past the last chunk */
    TEST_ASSERT_FALSE(ray_col_src_chunk_stat(&s, -1, &st));
    ray_col_src_t p; TEST_ASSERT_NULL(ray_col_src_bind(v, &p));      /* plain: no statistics */
    TEST_ASSERT_FALSE(ray_col_src_chunk_stat(&p, 0, &st));
    ray_col_src_release(&p); ray_col_src_release(&s); ray_release(c); ray_release(v);
    PASS();
}

static test_result_t test_byte_range_from_directory(void) {
    int64_t n = 4 * 65536;
    ray_t* v = sr_i32(n, 0);
    ray_t* c = sr_gen2(v, "br");
    ray_enccol_t* e = ray_enccol_of(c);
    ray_enc_dirent_t d1, d2;                            /* directory entries 1 and 2 */
    memcpy(&d1, e->base + RAY_ENC_DIR_OFF + 1 * sizeof(ray_enc_dirent_t), sizeof d1);
    memcpy(&d2, e->base + RAY_ENC_DIR_OFF + 2 * sizeof(ray_enc_dirent_t), sizeof d2);
    uint64_t off, len;
    ray_enccol_byte_range(e, 65536 + 5, 2 * 65536 + 10, &off, &len);   /* chunks 1 and 2 */
    TEST_ASSERT_EQ_U(off, d1.offset);
    TEST_ASSERT_EQ_U(off + len, d2.offset + d2.bytes);
    ray_enccol_byte_range(e, 65536, 2 * 65536, &off, &len);            /* exactly chunk 1 */
    TEST_ASSERT_EQ_U(off, d1.offset);
    TEST_ASSERT_EQ_U(len, d1.bytes);
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(c, &s));
    ray_col_src_advise(&s, 0, n);                       /* a hint only: must not fault or fail */
    ray_col_src_advise(&s, 7, 8);
    ray_col_src_advise(&s, 5, 5);                       /* empty: nothing */
    ray_col_src_release(&s);
    /* Plain mmapped, its slice, a heap vector: hints only, never a fault. */
    char p0[512]; snprintf(p0, sizeof p0, "%s/br0", sr_root());
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p0, 0), RAY_OK);
    ray_t* m = ray_col_mmap(p0);
    TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
    ray_t* sl = ray_vec_slice(m, 1000, 70000);
    TEST_ASSERT_TRUE(sl && !RAY_IS_ERR(sl));
    ray_col_src_t sm, ss, sh;
    TEST_ASSERT_NULL(ray_col_src_bind(m, &sm));
    TEST_ASSERT_NULL(ray_col_src_bind(sl, &ss));
    TEST_ASSERT_NULL(ray_col_src_bind(v, &sh));
    ray_col_src_advise(&sm, 3, n);
    ray_col_src_advise(&ss, 0, 70000);
    ray_col_src_advise(&sh, 0, n);
    ray_col_src_release(&sm); ray_col_src_release(&ss); ray_col_src_release(&sh);
    ray_release(sl); ray_release(m);
    /* A parted wrapper: each touched segment, across segment boundaries. */
    ray_t* w = sr_parted_i64(4, 1000);
    ray_col_src_t sp; TEST_ASSERT_NULL(ray_col_src_bind(w, &sp));
    ray_col_src_advise(&sp, 500, 2500);
    ray_col_src_advise(&sp, 0, 4000);
    ray_col_src_release(&sp); ray_release(w);
    ray_release(c); ray_release(v);
    PASS();
}

/* ---- Task 2 fix round 1: statistics on nulls, wide sums, floats; exact
 * byte ranges; the advise spans themselves ---- */

/* 128-bit accumulate of a sign-extended int64 (manual carry). */
static void sr_add128(uint64_t* lo, int64_t* hi, int64_t x) {
    uint64_t n = *lo + (uint64_t)x;
    *hi += (x < 0 ? -1 : 0) + (n < *lo ? 1 : 0);
    *lo = n;
}

/* Chunk 1 of an I32 column is entirely null: no min/max, every row null. */
static test_result_t test_chunk_stat_all_null(void) {
    int64_t n = 2 * 65536 + 10;
    ray_t* v = sr_i32(n, 0);
    for (int64_t i = 65536; i < 2 * 65536; i++) ((int32_t*)ray_data(v))[i] = NULL_I32;
    v->attrs |= RAY_ATTR_HAS_NULLS;
    ray_t* c = sr_gen2(v, "an");
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(c, &s));
    ray_enc_chunk_stat_t st;
    TEST_ASSERT_TRUE(ray_col_src_chunk_stat(&s, 1, &st));
    TEST_ASSERT_EQ_I(st.rows, 65536);
    TEST_ASSERT_FALSE(st.has_minmax);
    TEST_ASSERT_EQ_U(st.null_count, 65536);
    TEST_ASSERT_EQ_I(st.min, 0); TEST_ASSERT_EQ_I(st.max, 0);
    TEST_ASSERT_TRUE(ray_col_src_chunk_stat(&s, 0, &st));            /* its neighbours keep theirs */
    TEST_ASSERT_TRUE(st.has_minmax);
    TEST_ASSERT_EQ_U(st.null_count, 0);
    TEST_ASSERT_TRUE(ray_col_src_chunk_stat(&s, 2, &st));
    TEST_ASSERT_TRUE(st.has_minmax);
    TEST_ASSERT_EQ_I(st.rows, 10);
    ray_col_src_release(&s); ray_release(c); ray_release(v);
    PASS();
}

/* I64 sums wider than 64 bits: chunk 0 near INT64_MAX (sum_hi > 0), chunk
 * 1 near -INT64_MAX (sum_hi far below -1), chunk 2 small negatives
 * (sum_hi == -1), chunk 3 a partial mix with nulls; against a 128-bit
 * reference. */
static test_result_t test_chunk_stat_wide_sums(void) {
    int64_t n = 3 * 65536 + 300;
    ray_t* v = ray_vec_new(RAY_I64, n); v->len = n;
    int64_t* d = (int64_t*)ray_data(v);
    for (int64_t i = 0; i < n; i++) {
        int64_t ch = i / 65536, j = i % 65536;
        d[i] = ch == 0 ? INT64_MAX - j : ch == 1 ? -INT64_MAX + j : ch == 2 ? -1 - j
             : (j % 3 == 0 ? NULL_I64 : INT64_MAX - 7 * j);
    }
    v->attrs |= RAY_ATTR_HAS_NULLS;
    ray_t* c = sr_gen2(v, "ws");
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(c, &s));
    int64_t want_hi_sign[4] = { 1, -2, -1, 1 };     /* > 0, < -1, == -1, > 0 */
    for (int64_t ch = 0; ch < 4; ch++) {
        int64_t lo = ch * 65536, hi = lo + 65536 < n ? lo + 65536 : n;
        uint64_t slo = 0; int64_t shi = 0; uint32_t nn = 0;
        int64_t mn = INT64_MAX, mx = INT64_MIN;
        for (int64_t i = lo; i < hi; i++) {
            if (d[i] == NULL_I64) { nn++; continue; }
            sr_add128(&slo, &shi, d[i]);
            if (d[i] < mn) mn = d[i];
            if (d[i] > mx) mx = d[i];
        }
        ray_enc_chunk_stat_t st;
        TEST_ASSERT_TRUE(ray_col_src_chunk_stat(&s, ch, &st));
        TEST_ASSERT_EQ_I(st.rows, hi - lo);
        TEST_ASSERT_TRUE(st.has_minmax);
        TEST_ASSERT_EQ_I(st.min, mn); TEST_ASSERT_EQ_I(st.max, mx);
        TEST_ASSERT_EQ_U(st.null_count, nn);
        TEST_ASSERT_EQ_U(st.sum_lo, slo);
        TEST_ASSERT_EQ_I(st.sum_hi, shi);
        if (want_hi_sign[ch] == 1)  TEST_ASSERT_TRUE(st.sum_hi > 0);
        if (want_hi_sign[ch] == -2) TEST_ASSERT_TRUE(st.sum_hi < -1);
        if (want_hi_sign[ch] == -1) TEST_ASSERT_EQ_I(st.sum_hi, -1);
    }
    ray_col_src_release(&s); ray_release(c); ray_release(v);
    PASS();
}

/* F64: min and max carry the doubles' bits. */
static test_result_t test_chunk_stat_f64(void) {
    int64_t n = 2 * 65536 + 77;
    ray_t* v = sr_f64(n, 9);
    double* d = (double*)ray_data(v);
    for (int64_t i = 65536; i < n; i++) if (i % 9) d[i] = -d[i] * 1.5;   /* negatives in chunks 1, 2 */
    ray_t* c = sr_gen2(v, "fs");
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(c, &s));
    for (int64_t ch = 0; ch < 3; ch++) {
        int64_t lo = ch * 65536, hi = lo + 65536 < n ? lo + 65536 : n;
        double mn = 0, mx = 0; bool have = false; uint32_t nn = 0;
        for (int64_t i = lo; i < hi; i++) {
            if (d[i] != d[i]) { nn++; continue; }
            if (!have || d[i] < mn) mn = d[i];
            if (!have || d[i] > mx) mx = d[i];
            have = true;
        }
        ray_enc_chunk_stat_t st;
        TEST_ASSERT_TRUE(ray_col_src_chunk_stat(&s, ch, &st));
        TEST_ASSERT_EQ_I(st.rows, hi - lo);
        TEST_ASSERT_TRUE(st.has_minmax);
        TEST_ASSERT_EQ_U(st.null_count, nn);
        double smn, smx;
        memcpy(&smn, &st.min, sizeof smn); memcpy(&smx, &st.max, sizeof smx);
        TEST_ASSERT_FMT(smn == mn && smx == mx, "chunk %lld: [%g, %g] want [%g, %g]",
                        (long long)ch, smn, smx, mn, mx);
    }
    ray_col_src_release(&s); ray_release(c); ray_release(v);
    PASS();
}

static void sr_dirent(const ray_enccol_t* e, int64_t c, ray_enc_dirent_t* d) {
    memcpy(d, e->base + RAY_ENC_DIR_OFF + (size_t)c * sizeof *d, sizeof *d);
}

/* Exact byte ranges over a column with a partial last chunk. */
static test_result_t test_byte_range_edges(void) {
    int64_t n = 3 * 65536 + 100;
    ray_t* v = sr_i32(n, 13);
    ray_t* c = sr_gen2(v, "be");
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_enccol_t* e = ray_enccol_of(c);
    TEST_ASSERT_EQ_I(e->n_chunks, 4);
    ray_enc_dirent_t d[4];
    for (int k = 0; k < 4; k++) sr_dirent(e, k, &d[k]);
    static const struct { int64_t lo, hi; int c0, c1; } cases[] = {
        { 3 * 65536 + 99, 3 * 65536 + 100, 3, 3 },      /* [n-1, n) */
        { 0,              3 * 65536 + 100, 0, 3 },      /* [0, n) */
        { 0,              10,              0, 0 },      /* starts at chunk 0 */
        { 0,              65537,           0, 1 },      /* chunk 0 into chunk 1 */
        { 3 * 65536 + 50, 3 * 65536 + 51,  3, 3 },      /* one row of the partial last chunk */
        { 65535,          65536,           0, 0 },      /* the last row of chunk 0 */
    };
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        uint64_t off, len;
        ray_enccol_byte_range(e, cases[k].lo, cases[k].hi, &off, &len);
        TEST_ASSERT_EQ_U(off, d[cases[k].c0].offset);
        TEST_ASSERT_EQ_U(len, d[cases[k].c1].offset + d[cases[k].c1].bytes - d[cases[k].c0].offset);
    }
    ray_release(c); ray_release(v);
    PASS();
}

/* The advise spans: page-aligned start (host page size, at most a page
 * before the requested bytes), inside the mapping, ending exactly where
 * the requested bytes end. */
#define SR_SPANS 8
typedef struct { uintptr_t a[SR_SPANS]; size_t l[SR_SPANS]; } sr_spans_t;
static int sr_spans(const ray_col_src_t* s, int64_t lo, int64_t hi, sr_spans_t* o) {
    return ray_col_src_test_advise_spans(s, lo, hi, o->a, o->l, SR_SPANS);
}
static bool sr_span_ok(uintptr_t a, size_t l, uintptr_t map_lo, uintptr_t map_hi,
                       uintptr_t want_lo, uintptr_t want_hi) {
    size_t pg = ray_vm_page_size();
    return pg >= 4096 && (pg & (pg - 1)) == 0 && a % pg == 0 && a >= map_lo && a + l <= map_hi
           && a <= want_lo && a + l == want_hi && want_lo - a < pg;      /* ends exactly at the bytes */
}
static test_result_t test_advise_spans(void) {
    int64_t n = 3 * 65536 + 100;
    ray_t* v = sr_i32(n, 0);
    /* Generation 2: a partial range inside chunks 1-2, the partial last chunk. */
    ray_t* c = sr_gen2(v, "as");
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_enccol_t* e = ray_enccol_of(c);
    ray_enc_dirent_t d1, d2, d3;
    sr_dirent(e, 1, &d1); sr_dirent(e, 2, &d2); sr_dirent(e, 3, &d3);
    uintptr_t b = (uintptr_t)e->base, bend = b + e->size;
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(c, &s));
    sr_spans_t sp;
    TEST_ASSERT_EQ_I(sr_spans(&s, 65536 + 5, 2 * 65536 + 10, &sp), 1);
    TEST_ASSERT_TRUE(sr_span_ok(sp.a[0], sp.l[0], b, bend, b + d1.offset, b + d2.offset + d2.bytes));
    TEST_ASSERT_EQ_I(sr_spans(&s, n - 1, n, &sp), 1);
    TEST_ASSERT_TRUE(sr_span_ok(sp.a[0], sp.l[0], b, bend, b + d3.offset, b + d3.offset + d3.bytes));
    TEST_ASSERT_EQ_I(sr_spans(&s, n - 1, n + 1000, &sp), 1);           /* clamped at the source end */
    TEST_ASSERT_EQ_I(sr_spans(&s, 9, 9, &sp), 0);                      /* empty */
    ray_col_src_release(&s);
    /* Generation 0: a slice of a mapped column. */
    char p0[512]; snprintf(p0, sizeof p0, "%s/as0", sr_root());
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p0, 0), RAY_OK);
    ray_t* m = ray_col_mmap(p0);
    TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
    if (m->mmod == 1) {                                                 /* mapped (not an equivalence pass) */
        ray_t* sl = ray_vec_slice(m, 1000, 70000);
        TEST_ASSERT_TRUE(sl && !RAY_IS_ERR(sl));
        ray_col_src_t ss; TEST_ASSERT_NULL(ray_col_src_bind(sl, &ss));
        uintptr_t mb = (uintptr_t)m, md = (uintptr_t)ray_data(m), mend = md + (uintptr_t)n * 4;
        TEST_ASSERT_EQ_I(sr_spans(&ss, 5, 60000, &sp), 1);
        TEST_ASSERT_TRUE(sr_span_ok(sp.a[0], sp.l[0], mb, mend, md + 1005 * 4, md + 61000 * 4));
        ray_col_src_release(&ss); ray_release(sl);
    }
    ray_col_src_t sh; TEST_ASSERT_NULL(ray_col_src_bind(v, &sh));     /* heap: no span */
    TEST_ASSERT_EQ_I(sr_spans(&sh, 0, n, &sp), 0);
    ray_col_src_release(&sh);
    /* Parted: a mapped generation-0 segment, a generation-2 segment, a heap
     * segment, a second mapping of segment 0's file; [500, 2500) touches the
     * tail of 0, all of 1, the head of 2. */
    ray_t* s0v = sr_i32(1000, 0); ray_t* s1v = sr_i32(1000, 0);
    char q0[512], q1[512];
    snprintf(q0, sizeof q0, "%s/asp0", sr_root()); snprintf(q1, sizeof q1, "%s/asp1", sr_root());
    TEST_ASSERT_EQ_I(ray_col_save_gen(s0v, q0, 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_save_gen(s1v, q1, 2), RAY_OK);
    ray_t* g0 = ray_col_mmap(q0); ray_t* g1 = ray_col_mmap(q1); ray_t* g3 = ray_col_mmap(q0);
    TEST_ASSERT_TRUE(g0 && !RAY_IS_ERR(g0) && g1 && !RAY_IS_ERR(g1) && g1->type == RAY_ENCCOL);
    TEST_ASSERT_TRUE(g3 && !RAY_IS_ERR(g3));
    ray_t* w = ray_alloc(4 * sizeof(ray_t*));
    w->type = RAY_PARTED_BASE + RAY_I32; w->len = 4;
    ((ray_t**)ray_data(w))[0] = g0; ((ray_t**)ray_data(w))[1] = g1; ((ray_t**)ray_data(w))[2] = s0v;
    ((ray_t**)ray_data(w))[3] = g3;
    ray_retain(s0v);
    ray_t* ep = ray_enccol_parted_new(w);
    TEST_ASSERT_TRUE(ep && !RAY_IS_ERR(ep));
    ray_col_src_t sp3; TEST_ASSERT_NULL(ray_col_src_bind(ep, &sp3));
    ray_enccol_t* e1 = ray_enccol_of(g1);
    ray_enc_dirent_t f0; sr_dirent(e1, 0, &f0);
    int ns = sr_spans(&sp3, 500, 2500, &sp);
    int want = g0->mmod == 1 ? 2 : 1;
    TEST_ASSERT_EQ_I(ns, want);
    int k = 0;
    if (g0->mmod == 1) {
        uintptr_t gd = (uintptr_t)ray_data(g0);
        TEST_ASSERT_TRUE(sr_span_ok(sp.a[0], sp.l[0], (uintptr_t)g0, gd + 4000, gd + 2000, gd + 4000));
        k = 1;
    }
    uintptr_t b1 = (uintptr_t)e1->base;
    TEST_ASSERT_TRUE(sr_span_ok(sp.a[k], sp.l[k], b1, b1 + e1->size, b1 + f0.offset, b1 + f0.offset + f0.bytes));
    if (g0->mmod == 1) {                                                /* ends inside segment 0 */
        uintptr_t gd = (uintptr_t)ray_data(g0);
        TEST_ASSERT_EQ_I(sr_spans(&sp3, 100, 600, &sp), 1);
        TEST_ASSERT_TRUE(sr_span_ok(sp.a[0], sp.l[0], (uintptr_t)g0, gd + 4000, gd + 400, gd + 2400));
        uintptr_t g3d = (uintptr_t)ray_data(g3);                       /* rows local to segment 3 */
        TEST_ASSERT_EQ_I(sr_spans(&sp3, 3200, 3600, &sp), 1);
        TEST_ASSERT_TRUE(sr_span_ok(sp.a[0], sp.l[0], (uintptr_t)g3, g3d + 4000, g3d + 800, g3d + 2400));
    }
    ray_col_src_advise(&sp3, 0, 4000);                                  /* the real hint: no fault */
    ray_col_src_release(&sp3);
    ray_release(ep); ray_release(w); ray_release(s1v); ray_release(s0v);
    ray_release(m); ray_release(c); ray_release(v);
    PASS();
}

/* ---- runtime-level helpers shared by Tasks 3-16 ---------------------- */
#define SR_G0 SR_TMP "/g0/"
#define SR_G2 SR_TMP "/g2/"
/* 200000 rows (four chunks, the last partial), saved as generation 0 and 2
 * from the source table S (T is rebound by every load):
 *   k I64 = row; g I16 = row % 7; v I32 (null every 10th) = 3*row - 7;
 *   f F64 (null every 13th) = row; d DATE = row % 900; s SYM five values;
 *   b BOOL = row % 3 == 0; u U8 = row % 200;
 *   h I64 = (row * 7919) % 1000003 (unclustered, no nulls: the lazy hash shape). */
static void srr_setup(void) {
    sr_rt_setup();
    sr_eval_ok("(set i (til 200000))");
    sr_eval_ok("(set S (table [k g v f d s b u h] (list i (as 'I16 (% i 7)) "
               "(as 'I32 (map (fn [j] (if (== 0 (% j 10)) 0N (- (* 3 j) 7))) i)) "
               "(as 'F64 (map (fn [j] (if (== 0 (% j 13)) 0N j)) i)) (as 'DATE (% i 900)) "
               "(take ['a 'b 'c 'd 'e] 200000) (== 0 (% i 3)) (as 'U8 (% i 200)) (% (* i 7919) 1000003))))");
    sr_eval_ok("(.db.splayed.set \"" SR_G2 "\" S {compress: true})");
    sr_eval_ok("(.db.splayed.set \"" SR_G0 "\" S {compress: false})");
}
static void srr_teardown(void) { ray_idx_lazy_clear(); sr_rt_teardown(); }

static ray_t* sr_load(int gen) {
    ray_t* t = sr_eval(gen ? "(.db.splayed.get \"" SR_G2 "\")" : "(.db.splayed.get \"" SR_G0 "\")");
    return (t && !RAY_IS_ERR(t)) ? t : NULL;
}

/* Run `q` (the table written as T) against a fresh load of each generation:
 * equal formatted results; the generation-2 run materializes exactly
 * `want_mat` columns. */
static test_result_t sr_same(const char* q, uint64_t want_mat) {
    ray_t* out[2] = { NULL, NULL };
    for (int gen = 0; gen < 2; gen++) {
        sr_evalf_ok("(set T (.db.splayed.get \"%s\"))", gen ? SR_G2 : SR_G0);
        ray_enc_counters_reset();
        out[gen] = sr_eval(q);
        TEST_ASSERT_FMT(out[gen] && !RAY_IS_ERR(out[gen]), "gen %d: %s: %s", gen, q,
                        out[gen] ? ray_error_msg() : "null");
        if (gen) {
            ray_enc_counters_t k; ray_enc_counters_get(&k);
            TEST_ASSERT_FMT(k.materialized == want_mat, "%s: %llu materializations, want %llu",
                            q, (unsigned long long)k.materialized, (unsigned long long)want_mat);
        }
    }
    ray_t* fa = ray_fmt(out[0], 0); ray_t* fb = ray_fmt(out[1], 0);
    TEST_ASSERT_FMT(ray_str_len(fa) == ray_str_len(fb) &&
                    memcmp(ray_str_ptr(fa), ray_str_ptr(fb), ray_str_len(fa)) == 0,
                    "%s differs:\n%.*s\nvs\n%.*s", q, (int)ray_str_len(fa), ray_str_ptr(fa),
                    (int)ray_str_len(fb), ray_str_ptr(fb));
    ray_release(fa); ray_release(fb); ray_release(out[0]); ray_release(out[1]);
    PASS();
}
#define SR_SAME(q, m) do { test_result_t r_ = sr_same((q), (m)); if (r_.status != TEST_PASS) return r_; } while (0)

/* Selection from a fused predicate, compared segment by segment. */
static test_result_t sr_sel_same(ray_op_t* (*build)(ray_graph_t*)) {
    ray_t* blk[2] = { NULL, NULL }; bool all[2] = { false, false };
    for (int gen = 0; gen < 2; gen++) {
        ray_t* t = sr_load(gen);
        TEST_ASSERT_NOT_NULL(t);
        ray_enc_counters_reset();
        ray_graph_t* g = ray_graph_new(t);
        ray_op_t* pred = build(g);
        blk[gen] = exec_pred_to_selection(g, pred, ray_table_nrows(t), &all[gen]);
        TEST_ASSERT_FMT(!(blk[gen] && RAY_IS_ERR(blk[gen])), "gen %d: %s", gen, ray_error_msg());
        TEST_ASSERT_TRUE(blk[gen] || all[gen]);                     /* the fused path took it */
        if (gen) { ray_enc_counters_t k; ray_enc_counters_get(&k); TEST_ASSERT_EQ_U(k.materialized, 0); }
        ray_graph_free(g); ray_release(t);
    }
    TEST_ASSERT_EQ_I(all[0], all[1]);
    if (blk[0]) {
        ray_rowsel_t* a = ray_rowsel_meta(blk[0]); ray_rowsel_t* b = ray_rowsel_meta(blk[1]);
        TEST_ASSERT_EQ_I(a->total_pass, b->total_pass);
        TEST_ASSERT_EQ_I(a->n_segs, b->n_segs);
        TEST_ASSERT_TRUE(memcmp(ray_rowsel_flags(blk[0]), ray_rowsel_flags(blk[1]), a->n_segs) == 0);
        TEST_ASSERT_TRUE(memcmp(ray_rowsel_offsets(blk[0]), ray_rowsel_offsets(blk[1]), (a->n_segs + 1) * 4) == 0);
        uint32_t nidx = ray_rowsel_offsets(blk[0])[a->n_segs];
        TEST_ASSERT_TRUE(memcmp(ray_rowsel_idx(blk[0]), ray_rowsel_idx(blk[1]), (size_t)nidx * 2) == 0);
        ray_rowsel_release(blk[0]); ray_rowsel_release(blk[1]);
    }
    PASS();
}

static ray_op_t* pred_v_gt(ray_graph_t* g)   { return ray_gt(g, ray_scan(g, "v"), ray_const_i64(g, 100000)); }
static ray_op_t* pred_k_band(ray_graph_t* g) { return ray_and(g, ray_ge(g, ray_scan(g, "k"), ray_const_i64(g, 1000)),
                                                                  ray_lt(g, ray_scan(g, "k"), ray_const_i64(g, 150000))); }
static ray_op_t* pred_f_lt(ray_graph_t* g)   { return ray_lt(g, ray_scan(g, "f"), ray_const_f64(g, 77777.5)); }
static ray_op_t* pred_mixed(ray_graph_t* g)  { return ray_and(g, ray_eq(g, ray_scan(g, "g"), ray_const_i64(g, 3)),
                                                                  ray_gt(g, ray_add(g, ray_scan(g, "v"), ray_scan(g, "u")), ray_const_i64(g, 500))); }

static test_result_t test_expr_select_equal(void) {
    test_result_t r;
    if ((r = sr_sel_same(pred_v_gt)).status != TEST_PASS) return r;
    if ((r = sr_sel_same(pred_k_band)).status != TEST_PASS) return r;
    if ((r = sr_sel_same(pred_f_lt)).status != TEST_PASS) return r;    /* nullable F64 */
    if ((r = sr_sel_same(pred_mixed)).status != TEST_PASS) return r;   /* two encoded registers */
    PASS();
}

/* Element-wise fused output: (+ k v) as a whole vector, generation 0 vs 2. */
static test_result_t test_expr_elementwise_equal(void) {
    ray_t* res[2];
    for (int gen = 0; gen < 2; gen++) {
        ray_t* t = sr_load(gen);
        TEST_ASSERT_NOT_NULL(t);
        ray_enc_counters_reset();
        ray_graph_t* g = ray_graph_new(t);
        ray_op_t* e = ray_add(g, ray_scan(g, "k"), ray_scan(g, "v"));
        res[gen] = ray_execute(g, ray_optimize(g, e));
        TEST_ASSERT_TRUE(res[gen] && !RAY_IS_ERR(res[gen]));
        if (gen) { ray_enc_counters_t k; ray_enc_counters_get(&k); TEST_ASSERT_EQ_U(k.materialized, 0); }
        ray_graph_free(g); ray_release(t);
    }
    TEST_ASSERT_EQ_I(res[0]->type, res[1]->type);
    TEST_ASSERT_EQ_I(res[0]->len, res[1]->len);
    TEST_ASSERT_EQ_I(res[0]->attrs & RAY_ATTR_HAS_NULLS, res[1]->attrs & RAY_ATTR_HAS_NULLS);
    TEST_ASSERT_TRUE(memcmp(ray_data(res[0]), ray_data(res[1]), (size_t)res[0]->len * 8) == 0);
    ray_release(res[0]); ray_release(res[1]);
    PASS();
}

/* Review focus 3: a fused morsel that starts mid-vector.  The hook
 * ray_expr_test_eval_range runs the morsel evaluator over [1000, 1100)
 * directly (the pool only cuts unaligned tasks above its ring cap), for a
 * nullable I32 and a nullable F64 column, generation 0 against 2. */
static test_result_t sr_mid_one(ray_op_t* (*build)(ray_graph_t*)) {
    uint8_t out[2][100];
    memset(out[0], 0xA5, sizeof out[0]);       /* unwritten bytes cannot compare equal */
    memset(out[1], 0x5A, sizeof out[1]);
    for (int gen = 0; gen < 2; gen++) {
        ray_t* t = sr_load(gen);
        ray_graph_t* g = ray_graph_new(t);
        ray_expr_t ex;
        ray_t* cerr = NULL;
        TEST_ASSERT_TRUE(expr_compile(g, t, build(g), &ex, &cerr));
        TEST_ASSERT_NULL(cerr);
        TEST_ASSERT_EQ_I(ex.out_type, RAY_BOOL);
        ray_t* err = NULL;
        ray_expr_test_eval_range(&ex, 1000, 1100, out[gen], &err);
        TEST_ASSERT_NULL(err);
        ray_graph_free(g); ray_release(t);
    }
    TEST_ASSERT_TRUE(memcmp(out[0], out[1], 100) == 0);
    PASS();
}
static ray_op_t* mid_v(ray_graph_t* g) { return ray_lt(g, ray_scan(g, "v"), ray_const_i64(g, 2500)); }
static ray_op_t* mid_f(ray_graph_t* g) { return ray_gt(g, ray_scan(g, "f"), ray_const_f64(g, 1040.0)); }
static ray_op_t* mid_vf(ray_graph_t* g) { return ray_and(g, mid_v(g), ray_ne(g, ray_scan(g, "f"), ray_const_f64(g, 1050.0))); }
static test_result_t test_expr_nulls_midvector(void) {
    test_result_t r;
    if ((r = sr_mid_one(mid_v)).status != TEST_PASS) return r;    /* v: null every 10th row */
    if ((r = sr_mid_one(mid_f)).status != TEST_PASS) return r;    /* f: null every 13th row */
    if ((r = sr_mid_one(mid_vf)).status != TEST_PASS) return r;   /* two encoded registers */
    PASS();
}

/* Review focus 4: a corrupt chunk-2 descriptor surfaces as `corrupt` with
 * the column path, twice, and nothing leaks. */
static uint64_t sr_dirent_offset(const char* path, int64_t c) {
    ray_enc_dirent_t de; memset(&de, 0, sizeof de);
    FILE* f = fopen(path, "rb");
    if (f) {
        if (fseek(f, (long)(RAY_ENC_DIR_OFF + (uint64_t)c * sizeof de), SEEK_SET) != 0 ||
            fread(&de, sizeof de, 1, f) != 1) de.offset = 0;
        fclose(f);
    }
    return de.offset;
}
static void sr_patch(const char* path, long off, uint8_t val) {
    FILE* f = fopen(path, "r+b"); if (!f) return;
    if (fseek(f, off, SEEK_SET) == 0) fputc(val, f);
    fclose(f);
}
/* Copy of the generation-2 table with column `col`'s chunk 2 descriptor 0
 * reserved bytes (rsv0, offset 6) set; returns the resolved column path,
 * resolved from the directory string the loads below use (trailing slash
 * included: the load names its columns "<dir>/<col>"). */
static void sr_make_bad(const char* col, char* path, size_t cap) {
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/bad/\" S {compress: true})");
    char dir[512];
    if (ray_splay_resolve_dir(ray_test_tmp(SR_TMP "/bad/"), dir, sizeof dir) != RAY_OK) abort();
    snprintf(path, cap, "%s/%s", dir, col);
    uint64_t off = sr_dirent_offset(path, 2);
    if (!off) abort();
    sr_patch(path, (long)off + 6, 1);
}
static test_result_t test_expr_error(void) {
    char path[600];
    sr_make_bad("v", path, sizeof path);
    for (int i = 0; i < 2; i++) {
        ray_t* t = sr_eval("(.db.splayed.get \"" SR_TMP "/bad/\")");
        TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
        ray_graph_t* g = ray_graph_new(t);
        bool all = false;
        ray_error_clear();
        ray_t* s = exec_pred_to_selection(g, pred_v_gt(g), ray_table_nrows(t), &all);
        TEST_ASSERT_TRUE(s && RAY_IS_ERR(s));
        TEST_ASSERT_STR_EQ(ray_err_code(s), "corrupt");
        TEST_ASSERT_TRUE(strstr(ray_error_msg(), path) != NULL);
        ray_error_free(s);
        ray_graph_free(g); ray_release(t);
    }
    PASS();
}

/* Finding C2: (count (select {from: T where: <compare>})) is answered by
 * try_count_simple_compare on range reads (ClickBench q2's shape), never
 * materializing; a corrupt chunk is the query's error. */
static test_result_t test_count_fast_path(void) {
    uint64_t f0 = ray_query_test_count_fast();
    SR_SAME("(count (select {from: T where: (== g 3)}))", 0);       /* I16 */
    SR_SAME("(count (select {from: T where: (> k 150000)}))", 0);   /* I64 */
    SR_SAME("(count (select {from: T where: (!= u 7)}))", 0);        /* U8 */
    SR_SAME("(count (select {from: T where: (< d 100)}))", 0);       /* DATE, integer literal */
    SR_SAME("(count (select {from: T where: (== s 'c)}))", 0);       /* file-domain SYM */
    SR_SAME("(count (select {from: T where: (== s 'zz)}))", 0);      /* absent symbol */
    TEST_ASSERT_EQ_U(ray_query_test_count_fast() - f0, 12);          /* 6 shapes x 2 generations */
    char path[600];
    sr_make_bad("k", path, sizeof path);
    for (int i = 0; i < 2; i++) {
        ray_error_clear();
        ray_t* r = sr_eval("(count (select {from: (.db.splayed.get \"" SR_TMP "/bad/\") where: (> k 5)}))");
        TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
        TEST_ASSERT_TRUE(strstr(ray_error_msg(), path) != NULL);
        ray_error_free(r);
    }
    PASS();
}

/* The fast path's SYM compare, on an in-memory (runtime-domain) table and
 * over a file domain at both generations, with nulls and an absent
 * literal: EQ / NE take the fast path and equal the general path (`and
 * ... true` declines the fast path); the ordered compares decline (#738:
 * symbols order by string, never by raw id). */
static test_result_t test_count_fast_sym_nulls(void) {
    sr_eval_ok("(set N (table [s] (list (take (as 'SYM (list \"zq\" \"aq\" \"\" \"mq\")) 30001))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/n0/\" N {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/n2/\" N {compress: true})");
    static const char* eqs[] = { "(== s 'mq)", "(!= s 'mq)", "(== s 'qqq)", "(!= s 'qqq)" };
    static const char* ords[] = {
        "(< s 'mq)", "(<= s 'mq)", "(> s 'aq)", "(>= s 'aq)",
        "(< s 'qqq)", "(<= s 'qqq)", "(> s 'qqq)", "(>= s 'qqq)",
    };
    static const char* const loads[] = {
        "(set T N)",                                         /* runtime domain */
        "(set T (.db.splayed.get \"" SR_TMP "/n0/\"))",
        "(set T (.db.splayed.get \"" SR_TMP "/n2/\"))",
    };
    for (int l = 0; l < 3; l++) {
        sr_eval_ok(loads[l]);
        for (size_t i = 0; i < sizeof eqs / sizeof eqs[0]; i++) {
            char q[256];
            uint64_t f0 = ray_query_test_count_fast();
            snprintf(q, sizeof q, "(count (select {from: T where: %s}))", eqs[i]);
            ray_t* a = sr_eval(q);
            TEST_ASSERT_FMT(ray_query_test_count_fast() - f0 == 1, "load %d %s: not the fast path", l, eqs[i]);
            snprintf(q, sizeof q, "(count (select {from: T where: (and %s true)}))", eqs[i]);
            ray_t* b = sr_eval(q);
            TEST_ASSERT_TRUE(a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b));
            TEST_ASSERT_FMT(a->i64 == b->i64, "load %d %s: fast %lld, general %lld", l, eqs[i],
                            (long long)a->i64, (long long)b->i64);
            ray_release(a); ray_release(b);
        }
        for (size_t i = 0; i < sizeof ords / sizeof ords[0]; i++) {
            char q[256];
            uint64_t f0 = ray_query_test_count_fast();
            snprintf(q, sizeof q, "(count (select {from: T where: %s}))", ords[i]);
            ray_t* a = sr_eval(q);
            TEST_ASSERT_TRUE(a && !RAY_IS_ERR(a));
            TEST_ASSERT_FMT(ray_query_test_count_fast() == f0, "load %d %s: took the fast path", l, ords[i]);
            ray_release(a);
        }
    }
    PASS();
}

/* The reuse rule: a generation-2 column whose materialization is already
 * published compiles to a plain register over that vector (no range
 * reads) and is not decoded again. */
static test_result_t test_expr_reuses_materialized(void) {
    sr_evalf_ok("(set T (.db.splayed.get \"%s\"))", SR_G2);
    ray_t* s = sr_eval("(sum (at T 'v))");                          /* materializes v */
    TEST_ASSERT_TRUE(s && !RAY_IS_ERR(s));
    ray_release(s);
    ray_t* t = sr_eval("T");
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_t* ev = ray_table_get_col_enc(t, ray_sym_intern("v", 1));
    TEST_ASSERT_NOT_NULL(ev);
    ray_t* dec = ray_enccol_decoded_peek(ev);
    TEST_ASSERT_NOT_NULL(dec);
    ray_enc_counters_reset();
    ray_graph_t* g = ray_graph_new(t);
    ray_expr_t ex;
    ray_t* err = NULL;
    TEST_ASSERT_TRUE(expr_compile(g, t, ray_add(g, ray_scan(g, "k"), ray_scan(g, "v")), &ex, &err));
    TEST_ASSERT_NULL(err);
    int seen = 0;
    for (uint8_t r = 0; r < ex.n_regs; r++) {
        if (ex.regs[r].kind != REG_SCAN) continue;
        if (ex.regs[r].col_type == RAY_I32) {                       /* v */
            TEST_ASSERT_FALSE(ex.regs[r].enc);
            TEST_ASSERT_TRUE(ex.regs[r].data == ray_data(dec));
            seen++;
        } else {                                                    /* k: not materialized */
            TEST_ASSERT_TRUE(ex.regs[r].enc && !ex.regs[r].data);
            seen++;
        }
    }
    TEST_ASSERT_EQ_I(seen, 2);
    ray_t* out = expr_eval_full(&ex, ray_table_nrows(t));
    TEST_ASSERT_TRUE(out && !RAY_IS_ERR(out));
    ray_release(out);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    ray_graph_free(g); ray_release(t);
    PASS();
}

/* A range-read failure surfaces as `corrupt` with the column path through
 * the element-wise and the group call sites of expr_eval_full (on a sound
 * generation-2 table neither run decodes k or v, so the fused path is the
 * one taken; the group key g is read whole), and an interrupt is
 * `cancel`. */
static test_result_t test_expr_error_sites(void) {
    static const char* const qs[] = {
        "(select {z: (+ k v) from: T})",
        "(select {z: (max (* k v)) by: g from: T})",
    };
    for (int i = 0; i < 2; i++) {
        sr_evalf_ok("(set T (.db.splayed.get \"%s\"))", SR_G2);
        ray_t* r = sr_eval(qs[i]);
        TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
        ray_release(r);
        ray_t* t = sr_eval("T");
        TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
        for (int c = 0; c < 2; c++) {
            ray_t* ev = ray_table_get_col_enc(t, ray_sym_intern(c ? "v" : "k", 1));
            TEST_ASSERT_FMT(ev && !ray_enccol_decoded_peek(ev), "%s: column %s decoded", qs[i], c ? "v" : "k");
        }
        ray_release(t);
    }
    char path[600];
    sr_make_bad("v", path, sizeof path);
    for (int i = 0; i < 2; i++) {
        sr_eval_ok("(set T (.db.splayed.get \"" SR_TMP "/bad/\"))");
        ray_error_clear();
        ray_t* r = sr_eval(qs[i]);
        TEST_ASSERT_FMT(r && RAY_IS_ERR(r), "%s: not an error", qs[i]);
        TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
        TEST_ASSERT_TRUE(strstr(ray_error_msg(), path) != NULL);
        ray_error_free(r);
    }
    /* cancel: an interrupted dispatch is `cancel`, not a partial selection */
    ray_t* t = sr_load(1);
    TEST_ASSERT_NOT_NULL(t);
    ray_graph_t* g = ray_graph_new(t);
    bool all = false;
    ray_request_interrupt();
    ray_t* sel = exec_pred_to_selection(g, pred_v_gt(g), ray_table_nrows(t), &all);
    ray_clear_interrupt();
    TEST_ASSERT_TRUE(sel && RAY_IS_ERR(sel));
    TEST_ASSERT_STR_EQ(ray_err_code(sel), "cancel");
    ray_error_free(sel);
    ray_graph_free(g); ray_release(t);
    PASS();
}

/* ---- Task 3b: decode speed (counters, native compares, register widening) ---- */

/* Decoded-byte totals are flushed per scratch, not per vector: after a
 * query they are what one decode of every row read gives (pinned). */
static test_result_t test_decoded_bytes_pinned(void) {
    sr_evalf_ok("(set T (.db.splayed.get \"%s\"))", SR_G2);
    ray_enc_counters_reset();
    ray_t* r = sr_eval("(count (select {from: T where: (== g 3)}))");      /* the count fast path, I16 */
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    ray_release(r);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 200000 * 2);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    ray_t* t = sr_eval("T");
    ray_t* eg = ray_table_get_col_enc(t, ray_sym_intern("g", 1));
    TEST_ASSERT_NOT_NULL(eg);
    uint64_t col0 = atomic_load(&ray_enccol_of(eg)->decoded_bytes);
    TEST_ASSERT_EQ_U(col0, 200000 * 2);                                      /* per column too */
    ray_enc_counters_reset();
    r = sr_eval("(select {z: (+ k v) from: T})");                           /* fused: I64 and nullable I32 registers */
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    ray_release(r);
    ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 200000 * (8 + 4));
    TEST_ASSERT_EQ_U(k.materialized, 0);
    ray_enc_counters_reset();
    r = sr_eval("(select {z: (+ g 1) from: T})");                           /* the widened I16 register */
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    ray_release(r);
    ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 200000 * 2);
    TEST_ASSERT_EQ_U(atomic_load(&ray_enccol_of(eg)->decoded_bytes), col0 + 200000 * 2);
    ray_release(t);
    PASS();
}

/* The native-width count kernels against the widening reference, all six
 * compares, literals inside the type, at its bounds and outside it, over
 * one span longer than 65535 rows. */
static test_result_t test_count_native_kernels(void) {
    enum { n = 200003 };
    static int16_t x16[n]; static int32_t x32[n]; static uint8_t x8[n];
    uint64_t s = 0x9e3779b97f4a7c15ull;
    for (int64_t i = 0; i < n; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        x16[i] = (int16_t)(uint16_t)s; x32[i] = (int32_t)(uint32_t)(s >> 16); x8[i] = (uint8_t)(s >> 40);
    }
    x16[7] = INT16_MIN; x16[8] = INT16_MAX; x32[7] = INT32_MIN; x32[8] = INT32_MAX; x8[7] = 0; x8[8] = 255;
    static const int64_t lit16[] = { 0, 5, -77, INT16_MIN, INT16_MAX, INT16_MIN - 1, INT16_MAX + 1, 100000, -100000, INT64_MIN + 1, INT64_MAX };
    static const int64_t lit32[] = { 0, 5, -77, INT32_MIN, INT32_MAX, (int64_t)INT32_MIN - 1, (int64_t)INT32_MAX + 1, 5000000000ll, -5000000000ll };
    static const int64_t lit8[] = { 0, 1, 200, 255, 256, -1, 1000 };
    static const int64_t lens[] = { 200003, 65535, 65536, 1000, 1, 0 };
    for (int op = 1; op <= 6; op++) {
        for (size_t li = 0; li < sizeof lens / sizeof lens[0]; li++) {
            int64_t m = lens[li];
            for (size_t i = 0; i < sizeof lit16 / sizeof lit16[0]; i++) {
                int64_t a = ray_query_test_count_compare(RAY_I16, x16, m, lit16[i], op, false);
                int64_t b = ray_query_test_count_compare(RAY_I16, x16, m, lit16[i], op, true);
                TEST_ASSERT_FMT(a == b, "I16 op %d lit %lld n %lld: %lld vs %lld", op, (long long)lit16[i], (long long)m, (long long)a, (long long)b);
            }
            for (size_t i = 0; i < sizeof lit32 / sizeof lit32[0]; i++) {
                for (int t = 0; t < 3; t++) {
                    int8_t ty = t == 0 ? RAY_I32 : t == 1 ? RAY_DATE : RAY_TIME;
                    int64_t a = ray_query_test_count_compare(ty, x32, m, lit32[i], op, false);
                    int64_t b = ray_query_test_count_compare(ty, x32, m, lit32[i], op, true);
                    TEST_ASSERT_FMT(a == b, "type %d op %d lit %lld n %lld: %lld vs %lld", ty, op, (long long)lit32[i], (long long)m, (long long)a, (long long)b);
                }
            }
            for (size_t i = 0; i < sizeof lit8 / sizeof lit8[0]; i++) {
                for (int t = 0; t < 2; t++) {
                    int8_t ty = t ? RAY_BOOL : RAY_U8;
                    int64_t a = ray_query_test_count_compare(ty, x8, m, lit8[i], op, false);
                    int64_t b = ray_query_test_count_compare(ty, x8, m, lit8[i], op, true);
                    TEST_ASSERT_FMT(a == b, "type %d op %d lit %lld n %lld: %lld vs %lld", ty, op, (long long)lit8[i], (long long)m, (long long)a, (long long)b);
                }
            }
        }
    }
    /* the all-true / all-false answers are exact counts */
    TEST_ASSERT_EQ_I(ray_query_test_count_compare(RAY_I16, x16, n, 40000, 3, false), n);       /* < above the type */
    TEST_ASSERT_EQ_I(ray_query_test_count_compare(RAY_I16, x16, n, 40000, 5, false), 0);       /* > above the type */
    TEST_ASSERT_EQ_I(ray_query_test_count_compare(RAY_I32, x32, n, -5000000000ll, 6, false), n);
    PASS();
}

/* The same through queries: both generations, the fast path against the
 * general one (`and ... true` declines the fast path), nulls included (a
 * nullable column declines the fast path: the answers still agree). */
static test_result_t test_count_native_queries(void) {
    sr_eval_ok("(set i (til 70001))");
    sr_eval_ok("(set N (table [x y xn yn z] (list (as 'I16 (- (% (* i 7919) 65535) 32767)) "
               "(as 'I32 (- (% (* i 1000003) 4294967295) 2147483647)) "
               "(as 'I16 (map (fn [j] (if (== 0 (% j 11)) 0N (- (% j 3000) 1500))) i)) "
               "(as 'I32 (map (fn [j] (if (== 0 (% j 7)) 0N (* j 9))) i)) (as 'U8 (% i 251)))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/c0/\" N {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/c2/\" N {compress: true})");
    static const char* ops[] = { "==", "!=", "<", "<=", ">", ">=" };
    static const char* cols[] = { "x", "y", "xn", "z" };
    static const char* lits[] = { "0", "-1500", "32767", "-32768", "40000", "2147483647", "-2147483649", "251" };
    static const char* const loads[] = { "(set T (.db.splayed.get \"" SR_TMP "/c0/\"))",
                                         "(set T (.db.splayed.get \"" SR_TMP "/c2/\"))" };
    int64_t first[4][6][8];
    for (int l = 0; l < 2; l++) {
        sr_eval_ok(loads[l]);
        for (int c = 0; c < 4; c++)
            for (int o = 0; o < 6; o++)
                for (int k = 0; k < 8; k++) {
                    char q[256];
                    uint64_t f0 = ray_query_test_count_fast();
                    snprintf(q, sizeof q, "(count (select {from: T where: (%s %s %s)}))", ops[o], cols[c], lits[k]);
                    ray_t* a = sr_eval(q);
                    bool fast = ray_query_test_count_fast() - f0 == 1;
                    snprintf(q, sizeof q, "(count (select {from: T where: (and (%s %s %s) true)}))", ops[o], cols[c], lits[k]);
                    ray_t* b = sr_eval(q);
                    TEST_ASSERT_FMT(a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b), "gen %d %s", l * 2, q);
                    TEST_ASSERT_FMT(a->i64 == b->i64, "gen %d (%s %s %s): fast %lld, general %lld", l * 2, ops[o], cols[c], lits[k],
                                    (long long)a->i64, (long long)b->i64);
                    TEST_ASSERT_FMT(fast == (c != 2), "gen %d (%s %s %s): fast path %d", l * 2, ops[o], cols[c], lits[k], fast);
                    if (l == 0) first[c][o][k] = a->i64;
                    else TEST_ASSERT_FMT(first[c][o][k] == a->i64, "(%s %s %s): gen 0 %lld, gen 2 %lld", ops[o], cols[c], lits[k],
                                         (long long)first[c][o][k], (long long)a->i64);
                    ray_release(a); ray_release(b);
                }
    }
    PASS();
}

/* Narrow registers widened straight from the packed vector: I16 and I32
 * with nulls, element-wise and as a filter, both generations equal. */
static test_result_t test_expr_widened_registers(void) {
    uint64_t h0 = ray_col_test_wide_hits();
    SR_SAME("(select {z: (+ g 1) from: T})", 0);                 /* I16, no nulls */
    TEST_ASSERT_FMT(ray_col_test_wide_hits() - h0 == 196, "(+ g 1): %llu of 196 vectors widened in place",
                    (unsigned long long)(ray_col_test_wide_hits() - h0));   /* gen 2 only; every vector FOR_BP */
    SR_SAME("(select {z: (* v 2) from: T})", 0);                 /* I32, null every 10th */
    SR_SAME("(count (select {from: T where: (> (+ g 1) 3)}))", 0);
    SR_SAME("(select {z: (* (+ v 1) 0.5) from: T})", 0);         /* an F64 program over I32 */
    sr_eval_ok("(set i (til 70001))");
    sr_eval_ok("(set W (table [xn yn] (list (as 'I16 (map (fn [j] (if (== 0 (% j 11)) 0N (- (% j 3000) 1500))) i)) "
               "(as 'I32 (map (fn [j] (if (== 0 (% j 7)) 0N (* j -9))) i)))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/w0/\" W {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/w2/\" W {compress: true})");
    static const char* qs[] = { "(select {z: (+ xn 1) from: T})", "(select {z: (* yn 2) from: T})",
                                "(select {z: (+ xn yn) from: T})", "(select {z: (* xn 0.25) from: T})" };
    for (size_t i = 0; i < sizeof qs / sizeof qs[0]; i++) {
        ray_t* out[2];
        for (int gen = 0; gen < 2; gen++) {
            sr_evalf_ok("(set T (.db.splayed.get \"%s\"))", gen ? SR_TMP "/w2/" : SR_TMP "/w0/");
            ray_enc_counters_reset();
            uint64_t h0 = ray_col_test_wide_hits();
            out[gen] = sr_eval(qs[i]);
            TEST_ASSERT_FMT(out[gen] && !RAY_IS_ERR(out[gen]), "%s gen %d", qs[i], gen);
            uint64_t hits = ray_col_test_wide_hits() - h0;
            if (gen) {
                ray_enc_counters_t k; ray_enc_counters_get(&k); TEST_ASSERT_EQ_U(k.materialized, 0);
                TEST_ASSERT_FMT(hits > 0, "%s: no vector widened in place", qs[i]);
            } else {
                TEST_ASSERT_EQ_U(hits, 0);                          /* generation 0 reads in place */
            }
        }
        ray_t* fa = ray_fmt(out[0], 0); ray_t* fb = ray_fmt(out[1], 0);
        TEST_ASSERT_FMT(ray_str_len(fa) == ray_str_len(fb) && memcmp(ray_str_ptr(fa), ray_str_ptr(fb), ray_str_len(fa)) == 0, "%s differs", qs[i]);
        ray_release(fa); ray_release(fb); ray_release(out[0]); ray_release(out[1]);
    }
    PASS();
}

/* ---- Task 4: parted fused expressions over encoded segments ---- */

/* Three partitions of [x y] under `root`: bit p of gen2 writes partition p
 * compressed; ragged: partition 3 has only x; nullref: partition 3 carries
 * y as explicit nulls (the reference for the ragged table). */
static void sr_parted_xy(const char* root, unsigned gen2, bool ragged, bool nullref) {
    for (int p = 0; p < 3; p++) {
        const char* tbl = (ragged && p == 2) ? "(table [x] (list (til 70000)))"
                        : (nullref && p == 2) ? "(table [x y] (list (til 70000) (take [0Nl] 70000)))"
                        : "(table [x y] (list (til 70000) (% (til 70000) 97)))";
        sr_evalf_ok("(.db.splayed.set \"%s/2024.01.0%d/t/\" %s {compress: %s})", root, p + 1, tbl,
                    ((gen2 >> p) & 1) ? "true" : "false");
    }
}
/* (+ x y) over a parted table through the parted evaluator
 * (expr_eval_full_parted).  ray_execute of the bare graph streams the
 * partitions one flat segment table at a time (build_segment_table) and
 * never reaches it; a projecting select keeps the table parted, so the
 * element-wise arm sees parted registers.  Returns the z column. */
static ray_t* sr_parted_add(const char* root, uint64_t* mat) {
    sr_evalf_ok("(set T (.db.parted.get \"%s\" 't))", root);
    ray_enc_counters_reset();
    ray_t* q = sr_eval("(select {z: (+ x y) from: T})");
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    if (mat) *mat = k.materialized;
    if (!q || RAY_IS_ERR(q)) return q;
    ray_t* v = ray_table_get_col(q, ray_sym_intern("z", 1));
    if (v) ray_retain(v);
    ray_release(q);
    return v;
}
static bool sr_vec_eq(ray_t* a, ray_t* b) {
    return a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b) && a->type == b->type && a->len == b->len &&
           (a->attrs & RAY_ATTR_HAS_NULLS) == (b->attrs & RAY_ATTR_HAS_NULLS) &&
           memcmp(ray_data(a), ray_data(b), (size_t)a->len * ray_sym_elem_size(a->type, a->attrs)) == 0;
}
/* Segments generation 2, 0, 2 against an all-generation-0 copy; and a
 * ragged generation-0 table against its explicit-null reference.  Equal
 * vectors, nothing materialized. */
static test_result_t test_expr_parted_segments(void) {
    sr_parted_xy(SR_TMP "/pp2", 5, false, false);
    sr_parted_xy(SR_TMP "/pp0", 0, false, false);
    sr_parted_xy(SR_TMP "/prg", 0, true, false);
    sr_parted_xy(SR_TMP "/pref", 0, false, true);
    uint64_t m2 = 99, mr = 99;
    ray_t* a = sr_parted_add(SR_TMP "/pp2", &m2);
    ray_t* b = sr_parted_add(SR_TMP "/pp0", NULL);
    ray_t* c = sr_parted_add(SR_TMP "/prg", &mr);
    ray_t* d = sr_parted_add(SR_TMP "/pref", NULL);
    TEST_ASSERT_TRUE(sr_vec_eq(a, b));
    TEST_ASSERT_EQ_U(m2, 0);
    TEST_ASSERT_TRUE(sr_vec_eq(c, d));                 /* ragged: synthetic nulls read by range */
    TEST_ASSERT_EQ_U(mr, 0);
    /* a segment another consumer already materialized is read in place
     * (published), the others by range: same vector, no new decode */
    sr_evalf_ok("(set T (.db.parted.get \"%s\" 't))", SR_TMP "/pp2");
    ray_t* t = sr_eval("T");
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_t* ey = ray_table_get_col_enc(t, ray_sym_intern("y", 1));
    TEST_ASSERT_TRUE(ey && ey->type == RAY_ENCCOL && ray_enccol_is_parted(ey));
    ray_t* s0 = ray_enccol_seg_col(ey, 0);
    TEST_ASSERT_TRUE(s0 && !RAY_IS_ERR(s0));
    TEST_ASSERT_NOT_NULL(ray_enccol_decoded_peek(ray_enccol_seg(ey, 0)));
    TEST_ASSERT_NULL(ray_enccol_decoded_peek(ey));                /* the column is not published */
    ray_enc_counters_reset();
    ray_t* q = sr_eval("(select {z: (+ x y) from: T})");
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_TRUE(q && !RAY_IS_ERR(q));
    TEST_ASSERT_EQ_U(k.materialized, 0);
    TEST_ASSERT_TRUE(sr_vec_eq(ray_table_get_col(q, ray_sym_intern("z", 1)), b));
    ray_release(q); ray_release(t);
    sr_drop(a); sr_drop(b); sr_drop(c); sr_drop(d);
    PASS();
}

/* Column `col` of partition `part` (1-based) under `root`: its resolved
 * path, with chunk `c`'s descriptor 0 reserved bytes set (corrupt). */
static void sr_parted_bad(const char* root, int part, const char* col, int64_t c, char* path, size_t cap) {
    char rel[512], dir[512];
    snprintf(rel, sizeof rel, "%s/2024.01.0%d/t/", root, part);
    if (ray_splay_resolve_dir(ray_test_tmp(rel), dir, sizeof dir) != RAY_OK) abort();
    size_t dl = strlen(dir);
    snprintf(path, cap, "%s%s%s", dir, (dl && dir[dl - 1] == '/') ? "" : "/", col);
    uint64_t off = sr_dirent_offset(path, c);
    if (!off) abort();
    sr_patch(path, (long)off + 6, 1);
}
/* Range-read failures in two segments: the answer is the lowest one in
 * row order across the whole parted column (failure keys are global row
 * offsets), every run.  Partition 1 fails late (chunk 1, rows 65536..),
 * partition 3 early (chunk 0): partition 1 still wins.  With only
 * partition 3 corrupt, its failure is the answer. */
static test_result_t test_expr_parted_fail_lowest(void) {
    sr_parted_xy(SR_TMP "/pbad", 7, false, false);
    char p1[600], p3[600];
    sr_parted_bad(SR_TMP "/pbad", 3, "y", 0, p3, sizeof p3);
    for (int i = 0; i < 2; i++) {
        ray_error_clear();
        ray_t* v = sr_parted_add(SR_TMP "/pbad", NULL);
        TEST_ASSERT_TRUE(v && RAY_IS_ERR(v));
        TEST_ASSERT_STR_EQ(ray_err_code(v), "corrupt");
        TEST_ASSERT_FMT(strstr(ray_error_msg(), p3) != NULL, "not %s: %s", p3, ray_error_msg());
        ray_error_free(v);
    }
    sr_parted_bad(SR_TMP "/pbad", 1, "y", 1, p1, sizeof p1);
    for (int i = 0; i < 4; i++) {
        ray_error_clear();
        ray_t* v = sr_parted_add(SR_TMP "/pbad", NULL);
        TEST_ASSERT_TRUE(v && RAY_IS_ERR(v));
        TEST_ASSERT_STR_EQ(ray_err_code(v), "corrupt");
        TEST_ASSERT_FMT(strstr(ray_error_msg(), p1) != NULL, "not %s: %s", p1, ray_error_msg());
        ray_error_free(v);
    }
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
    { "scanrange/chunk_stat_matches_data",   test_chunk_stat_matches_data,   sr_setup, sr_teardown },
    { "scanrange/byte_range_from_directory", test_byte_range_from_directory, sr_setup, sr_teardown },
    { "scanrange/chunk_stat_all_null",       test_chunk_stat_all_null,       sr_setup, sr_teardown },
    { "scanrange/chunk_stat_wide_sums",      test_chunk_stat_wide_sums,      sr_setup, sr_teardown },
    { "scanrange/chunk_stat_f64",            test_chunk_stat_f64,            sr_setup, sr_teardown },
    { "scanrange/byte_range_edges",          test_byte_range_edges,          sr_setup, sr_teardown },
    { "scanrange/advise_spans",              test_advise_spans,              sr_setup, sr_teardown },
    { "scanrange/expr_select_equal",      test_expr_select_equal,      srr_setup, srr_teardown },
    { "scanrange/expr_elementwise_equal", test_expr_elementwise_equal, srr_setup, srr_teardown },
    { "scanrange/expr_nulls_midvector",   test_expr_nulls_midvector,   srr_setup, srr_teardown },
    { "scanrange/expr_error",             test_expr_error,             srr_setup, srr_teardown },
    { "scanrange/count_fast_path",        test_count_fast_path,        srr_setup, srr_teardown },
    { "scanrange/count_fast_sym_nulls",   test_count_fast_sym_nulls,   sr_rt_setup, sr_rt_teardown },
    { "scanrange/expr_reuses_materialized", test_expr_reuses_materialized, srr_setup, srr_teardown },
    { "scanrange/expr_error_sites",       test_expr_error_sites,       srr_setup, srr_teardown },
    { "scanrange/decoded_bytes_pinned",   test_decoded_bytes_pinned,   srr_setup, srr_teardown },
    { "scanrange/count_native_kernels",   test_count_native_kernels,   sr_setup,  sr_teardown },
    { "scanrange/count_native_queries",   test_count_native_queries,   sr_rt_setup, sr_rt_teardown },
    { "scanrange/expr_widened_registers", test_expr_widened_registers, srr_setup, srr_teardown },
    { "scanrange/expr_parted_segments",   test_expr_parted_segments,   srr_setup, srr_teardown },
    { "scanrange/expr_parted_fail_lowest", test_expr_parted_fail_lowest, sr_rt_setup, sr_rt_teardown },
    { NULL, NULL, NULL, NULL },
};
