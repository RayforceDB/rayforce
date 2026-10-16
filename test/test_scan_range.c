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
#include "lang/env.h"
#include "lang/format.h"
#include "lang/parse.h"
#include "store/col.h"
#include "store/colrange.h"
#include "store/enccol.h"
#include "store/encfmt.h"
#include "store/encw.h"      /* ray_encw_test_force */
#include "store/splay.h"
#include "store/part.h"
#include "vec/str.h"
#include "vec/vec.h"
#include "ops/internal.h"
#include "ops/rowsel.h"
#include "ops/idxop.h"
#include "ops/fused_pred.h"
#include "ops/fused_topk.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Every path below goes through ray_test_tmp (a per-process root). */
/* Written as "/tmp/..." in the sources: every use goes through
 * ray_test_tmp (sr_root, sr_eval*, the helpers), which rewrites it under the
 * process's own root, so concurrent suites never share it. */
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
 *   h I64 = (row * 7919) % 1000003 (unclustered, no nulls: the lazy hash shape);
 *   e F32 (null every 17th) = row / 2. */
/* Bind `name` to an F32 vector of n rows: row * scale, null every
 * null_every-th row (0: none).  Rayfall has no F32 cast. */
static void sr_bind_f32(const char* name, int64_t n, int64_t null_every, float scale) {
    ray_t* v = ray_vec_new(RAY_F32, n); v->len = n;
    for (int64_t i = 0; i < n; i++)
        ((float*)ray_data(v))[i] = (null_every && i % null_every == 0) ? NULL_F32 : (float)i * scale;
    if (null_every) v->attrs |= RAY_ATTR_HAS_NULLS;
    ray_env_set(ray_sym_intern(name, strlen(name)), v);
    ray_release(v);
}
static void srr_setup(void) {
    sr_rt_setup();
    sr_eval_ok("(set i (til 200000))");
    sr_bind_f32("ef", 200000, 17, 0.5);
    sr_eval_ok("(set S (table [k g v f d s b u h e] (list i (as 'I16 (% i 7)) "
               "(as 'I32 (map (fn [j] (if (== 0 (% j 10)) 0N (- (* 3 j) 7))) i)) "
               "(as 'F64 (map (fn [j] (if (== 0 (% j 13)) 0N j)) i)) (as 'DATE (% i 900)) "
               "(take ['a 'b 'c 'd 'e] 200000) (== 0 (% i 3)) (as 'U8 (% i 200)) (% (* i 7919) 1000003) "
               "ef)))");
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
 * `want_mat` columns (UINT64_MAX: not checked). */
static test_result_t sr_same_on(const char* t0, const char* t2, const char* q, uint64_t want_mat) {
    ray_t* out[2] = { NULL, NULL };
    for (int gen = 0; gen < 2; gen++) {
        sr_evalf_ok("(set T %s)", gen ? t2 : t0);
        ray_enc_counters_reset();
        out[gen] = sr_eval(q);
        TEST_ASSERT_FMT(out[gen] && !RAY_IS_ERR(out[gen]), "gen %d: %s: %s", gen, q,
                        out[gen] ? ray_error_msg() : "null");
        if (gen && want_mat != UINT64_MAX) {   /* UINT64_MAX: answers only */
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
static test_result_t sr_same(const char* q, uint64_t want_mat) {
    return sr_same_on("(.db.splayed.get \"" SR_G0 "\")", "(.db.splayed.get \"" SR_G2 "\")", q, want_mat);
}
#define SR_SAME(q, m) do { test_result_t r_ = sr_same((q), (m)); if (r_.status != TEST_PASS) return r_; } while (0)

/* Selection from a fused predicate, compared segment by segment, over the
 * tables saved at dirs[0] (generation 0) and dirs[1] (generation 2). */
static test_result_t sr_sel_same_at(const char* const dirs[2], ray_op_t* (*build)(ray_graph_t*)) {
    ray_t* blk[2] = { NULL, NULL }; bool all[2] = { false, false };
    for (int gen = 0; gen < 2; gen++) {
        char q[600];
        snprintf(q, sizeof q, "(.db.splayed.get \"%s\")", dirs[gen]);
        ray_t* t = sr_eval(q);
        if (t && RAY_IS_ERR(t)) { ray_error_free(t); t = NULL; }
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
static test_result_t sr_sel_same(ray_op_t* (*build)(ray_graph_t*)) {
    static const char* const dirs[2] = { SR_G0, SR_G2 };
    return sr_sel_same_at(dirs, build);
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
 * materializing; a corrupt chunk the predicate reads is the query's error,
 * and one its directory decides is never read (spec "Filter with
 * skipping": a decided chunk touches no descriptor). */
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
        /* chunk 2 (rows 131072..196607) holds 150000: undecided, read */
        ray_t* r = sr_eval("(count (select {from: (.db.splayed.get \"" SR_TMP "/bad/\") where: (> k 150000)}))");
        TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
        TEST_ASSERT_TRUE(strstr(ray_error_msg(), path) != NULL);
        ray_error_free(r);
    }
    /* every row of chunk 2 is above 5: decided all-pass from the directory */
    ray_t* r = sr_eval("(count (select {from: (.db.splayed.get \"" SR_TMP "/bad/\") where: (> k 5)}))");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->i64 == 199994);
    ray_release(r);
    PASS();
}

/* Task 17i: the count fast path's tasks decode into a fixed vector lent
 * from their own stack, so a worker's first decode takes nothing from its
 * heap (a fresh worker heap maps a whole pool on its first allocation:
 * ClickBench q2's first run).  The answers equal generation 0's. */
static test_result_t test_count_fast_lent_scratch(void) {
    uint64_t f0 = ray_query_test_count_fast(), a0 = ray_col_test_fixed_allocs();
    SR_SAME("(count (select {from: T where: (== g 3)}))", 0);       /* I16 */
    SR_SAME("(count (select {from: T where: (> k 150000)}))", 0);   /* I64 */
    SR_SAME("(count (select {from: T where: (!= u 7)}))", 0);        /* U8 */
    SR_SAME("(count (select {from: T where: (== s 'c)}))", 0);       /* file-domain SYM */
    TEST_ASSERT_EQ_U(ray_query_test_count_fast() - f0, 8);
    TEST_ASSERT_FMT(ray_col_test_fixed_allocs() == a0, "%llu fixed vectors from the heap",
                    (unsigned long long)(ray_col_test_fixed_allocs() - a0));
    PASS();
}

/* ...and a group's tasks decode their first two encoded sources into
 * fixed vectors lent from their cursor sets (ClickBench q8's shape, and
 * a key beside an input): no fixed vector from the heap, the answers
 * generation 0's. */
static test_result_t test_group_lent_scratch(void) {
    ray_pool_destroy(); TEST_ASSERT_EQ_I(ray_pool_init_total(4), RAY_OK);
    uint64_t a0 = ray_col_test_fixed_allocs();
    SR_SAME("(select {g: g c: (count g) from: T where: (!= g 3) by: g desc: c take: 10})", UINT64_MAX);
    SR_SAME("(select {s: (sum u) from: T by: g})", UINT64_MAX);
    TEST_ASSERT_FMT(ray_col_test_fixed_allocs() == a0, "%llu fixed vectors from the heap",
                    (unsigned long long)(ray_col_test_fixed_allocs() - a0));
    /* Fix round 1: six encoded sources in one set (a key, five inputs):
     * the first two are lent, the rest take their vectors from the heap,
     * and no cursor is lent past the set's vectors (ASan). */
    SR_SAME("(select {a: (sum u) b: (sum v) c: (max f) d: (min k) e: (sum h) from: T by: g})", UINT64_MAX);
    TEST_ASSERT_FMT(ray_col_test_fixed_allocs() > a0, "six encoded sources, no fixed vector from the heap");
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
    ray_t* s = sr_eval("(at T 'v)");                                /* forced at the top: materializes v */
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
    TEST_ASSERT_EQ_U(k.decoded_bytes, 0);                                    /* P3 Task 3b: counted on its views */
    TEST_ASSERT_EQ_U(k.materialized, 0);
    ray_t* t = sr_eval("T");
    ray_t* eg = ray_table_get_col_enc(t, ray_sym_intern("g", 1));
    TEST_ASSERT_NOT_NULL(eg);
    uint64_t col0 = atomic_load(&ray_enccol_of(eg)->decoded_bytes);
    TEST_ASSERT_EQ_U(col0, 0);                                               /* per column too */
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
    /* one column with generation-2 segments and a synthetic null segment */
    sr_parted_xy(SR_TMP "/pmx", 3, true, false);
    sr_parted_xy(SR_TMP "/pmxref", 3, false, true);
    uint64_t mm = 99;
    ray_t* e = sr_parted_add(SR_TMP "/pmx", &mm);
    ray_t* f = sr_parted_add(SR_TMP "/pmxref", NULL);
    TEST_ASSERT_TRUE(sr_vec_eq(e, f));
    TEST_ASSERT_EQ_U(mm, 0);
    sr_drop(e); sr_drop(f);
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
    /* only the unpublished generation-2 segments are decoded by range:
     * x in partitions 1 and 3, y in partition 3 (y's partition 1 is read
     * from its published vector) */
    TEST_ASSERT_EQ_U(k.decoded_bytes, 3 * 70000 * 8);
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
 * row order across the whole parted column, every run.  Partition 1 fails
 * late (chunk 1, rows 65536..), partition 3 early (chunk 0): partition 1
 * wins.  The evaluator stops at the first failing segment, which alone
 * gives that answer; with the stop disabled (test hook) partition 3 runs
 * too and only the global failure keys (65536 against 140000, where
 * segment-local keys would be 65536 against 0) keep partition 1 the
 * answer.  With only partition 3 corrupt, its failure is the answer. */
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
    for (int i = 0; i < 8; i++) {
        ray_expr_test_parted_all_segments(i & 1);
        ray_error_clear();
        ray_t* v = sr_parted_add(SR_TMP "/pbad", NULL);
        TEST_ASSERT_TRUE(v && RAY_IS_ERR(v));
        TEST_ASSERT_STR_EQ(ray_err_code(v), "corrupt");
        TEST_ASSERT_FMT(strstr(ray_error_msg(), p1) != NULL, "run %d: not %s: %s", i, p1, ray_error_msg());
        ray_error_free(v);
    }
    ray_expr_test_parted_all_segments(false);
    PASS();
}

/* Compile (+ x y) over parted table t and evaluate it through the parted
 * evaluator directly (nrows rows). */
static ray_t* sr_parted_eval(ray_t* t, int64_t nrows) {
    ray_graph_t* g = ray_graph_new(t);
    ray_expr_t ex; ray_t* cerr = NULL;
    ray_t* v;
    if (expr_compile(g, t, ray_add(g, ray_scan(g, "x"), ray_scan(g, "y")), &ex, &cerr))
        v = expr_eval_full(&ex, nrows);
    else
        v = cerr ? cerr : ray_error("nyi", "sr_parted_eval: not compiled");
    ray_graph_free(g);
    return v;
}
/* A cancel ends the segment loop: two dispatched partitions (70000 rows,
 * their tasks skipped by the cancelled pool) and a corrupt third one small
 * enough to run inline; the answer is `cancel`, never the third's
 * `corrupt`. */
static test_result_t test_expr_parted_cancel(void) {
    for (int p = 0; p < 3; p++)
        sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/pcn/2024.01.0%d/t/\" %s {compress: true})", p + 1,
                    p == 2 ? "(table [x y] (list (til 1000) (% (til 1000) 97)))"
                           : "(table [x y] (list (til 70000) (% (til 70000) 97)))");
    char p3[600];
    sr_parted_bad(SR_TMP "/pcn", 3, "y", 0, p3, sizeof p3);
    sr_eval_ok("(set T (.db.parted.get \"" SR_TMP "/pcn\" 't))");
    ray_t* t = sr_eval("T");
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_t* v = sr_parted_eval(t, 141000);                     /* sound run: corrupt */
    TEST_ASSERT_TRUE(v && RAY_IS_ERR(v));
    TEST_ASSERT_STR_EQ(ray_err_code(v), "corrupt");
    ray_error_free(v);
    ray_request_interrupt();
    v = sr_parted_eval(t, 141000);
    ray_clear_interrupt();
    TEST_ASSERT_TRUE(v && RAY_IS_ERR(v));
    TEST_ASSERT_STR_EQ(ray_err_code(v), "cancel");
    ray_error_free(v);
    ray_release(t);
    PASS();
}

/* Parted registers whose segments differ in length (a hand-built table:
 * x splits 3000 + 5000, y 5000 + 3000) are a `length` error, never
 * clipped or read past a segment end. */
static test_result_t test_expr_parted_seg_lengths(void) {
    ray_t* wx = sr_parted_i64(2, 4000); ray_t* wy = sr_parted_i64(2, 4000);
    ray_t** sx = (ray_t**)ray_data(wx); ray_t** sy = (ray_t**)ray_data(wy);
    ray_release(sx[0]); ray_release(sx[1]); ray_release(sy[0]); ray_release(sy[1]);
    sx[0] = ray_vec_new(RAY_I64, 3000); sx[0]->len = 3000; memset(ray_data(sx[0]), 0, 3000 * 8);
    sx[1] = ray_vec_new(RAY_I64, 5000); sx[1]->len = 5000; memset(ray_data(sx[1]), 0, 5000 * 8);
    sy[0] = ray_vec_new(RAY_I64, 5000); sy[0]->len = 5000; memset(ray_data(sy[0]), 0, 5000 * 8);
    sy[1] = ray_vec_new(RAY_I64, 3000); sy[1]->len = 3000; memset(ray_data(sy[1]), 0, 3000 * 8);
    ray_t* t = ray_table_new(2);
    t = ray_table_add_col(t, ray_sym_intern("x", 1), wx);
    t = ray_table_add_col(t, ray_sym_intern("y", 1), wy);
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_t* v = sr_parted_eval(t, 8000);
    TEST_ASSERT_TRUE(v && RAY_IS_ERR(v));
    TEST_ASSERT_STR_EQ(ray_err_code(v), "length");
    ray_error_free(v);
    ray_release(t); ray_release(wx); ray_release(wy);
    PASS();
}

/* The directory gate decides whole chunks: a predicate true for no row of
 * chunks 1-3 reads none of them; inside chunk 0 the vectors' descriptors
 * decide vectors 1-63 (Task 17b), so only vector 0 is read.  k = row is
 * DELTA_BP, which has no view (P3 Task 3: a viewed vector would be
 * answered as an encoded leaf, decoding nothing), so vector 0 decodes. */
static test_result_t test_zone_dir_skips(void) {
    ray_t* t = sr_load(2);
    TEST_ASSERT_NOT_NULL(t);
    ray_graph_t* g = ray_graph_new(t);
    ray_enc_counters_reset();
    bool all = false;
    uint64_t dv0 = ray_enccol_test_desc_validations();
    ray_t* s = exec_pred_to_selection(g, ray_lt(g, ray_scan(g, "k"), ray_const_i64(g, 1000)), ray_table_nrows(t), &all);
    TEST_ASSERT_TRUE(s && !RAY_IS_ERR(s));
    TEST_ASSERT_EQ_I(ray_rowsel_meta(s)->total_pass, 1000);
    /* the chunk gate decides chunks 1-3 before their descriptors are read */
    TEST_ASSERT_EQ_U(ray_enccol_test_desc_validations() - dv0, 1);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 1024 * 8);        /* chunk 0's vector 0 only (DELTA_BP: no view) */
    /* why vector 0 decodes: DELTA_BP, which the view declines (checked
     * after the descriptor-validation pin, which this read would satisfy) */
    {
        ray_t* ek = ray_table_get_col_enc(t, ray_sym_intern("k", 1));
        TEST_ASSERT_NOT_NULL(ek);
        const ray_enc_desc_t* d;
        ray_t* derr = ray_enccol_chunk_descs_ref(ray_enccol_of(ek), 0, &d);
        TEST_ASSERT_TRUE(derr == NULL);
        TEST_ASSERT_EQ_I(d[0].encoding, RAY_ENC_DELTA_BP);
        ray_enc_vview_t* vb = malloc(sizeof *vb);
        ray_t* verr = NULL;
        TEST_ASSERT_EQ_I(ray_enccol_vview(ray_enccol_of(ek), 0, vb, &verr), 0);   /* declined: read by range */
        free(vb);
    }
    ray_rowsel_release(s); ray_graph_free(g); ray_release(t);
    /* the count fast path decides chunks the same way; vector 0 (DELTA_BP)
     * has no view, so it is read (P3 Task 3b counts viewed vectors only) */
    sr_eval_ok("(set T (.db.splayed.get \"" SR_G2 "\"))");
    ray_enc_counters_reset();
    dv0 = ray_enccol_test_desc_validations();
    ray_t* c = sr_eval("(count (select {from: T where: (< k 1000)}))");
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c) && c->i64 == 1000);
    ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 1024 * 8);
    TEST_ASSERT_EQ_U(ray_enccol_test_desc_validations() - dv0, 1);
    sr_drop(c);
    PASS();
}

/* Review focus 3: the null rule.  v has a null every 10th row, so no
 * chunk is decided all-pass for >, and every chunk can be decided
 * all-fail for > above its maximum.  Results equal generation 0. */
static test_result_t test_zone_dir_nulls(void) {
    test_result_t r;
    if ((r = sr_sel_same(pred_v_gt)).status != TEST_PASS) return r;
    SR_SAME("(count (select {from: T where: (> v 1000000)}))", 0);   /* every chunk all-fail */
    SR_SAME("(count (select {from: T where: (< v 1000000)}))", 0);   /* nulls pass < : mixed */
    SR_SAME("(count (select {from: T where: (>= d 899)}))", 0);
    PASS();
}

/* Rows of a graph FILTER over table t, with the materializations it made.
 * The node runs through exec_node, not ray_execute: the lazy filter returns
 * t with g->selection installed, and ray_execute's final compaction would
 * gather (today: materialize) every column of t. */
static int64_t sr_filter_rows(ray_t* t, ray_op_t* (*build)(ray_graph_t*), uint64_t* mat) {
    ray_graph_t* g = ray_graph_new(t);
    ray_op_t* root = ray_filter(g, ray_const_table(g, t), build(g));
    ray_enc_counters_reset();
    ray_t* r = exec_node(g, root);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    if (mat) *mat = k.materialized;
    int64_t n = (r && !RAY_IS_ERR(r)) ? (g->selection ? ray_rowsel_meta(g->selection)->total_pass : ray_table_nrows(r)) : -1;
    sr_drop(r); ray_graph_free(g);
    return n;
}
static ray_op_t* cpred_v_gt(ray_graph_t* g)  { return ray_gt(g, ray_scan(g, "v"), ray_const_i64(g, 100)); }
static ray_op_t* cpred_g_eq(ray_graph_t* g)  { return ray_eq(g, ray_scan(g, "g"), ray_const_i64(g, 3)); }
static ray_op_t* cpred_h_out(ray_graph_t* g) { return ray_eq(g, ray_scan(g, "h"), ray_const_i64(g, 1000004)); }
static ray_op_t* cpred_h_eq(ray_graph_t* g)  { return ray_eq(g, ray_scan(g, "h"), ray_const_i64(g, 7919)); }
static ray_op_t* cpred_h_rng(ray_graph_t* g) { return ray_and(g, ray_eq(g, ray_scan(g, "h"), ray_const_i64(g, 7919)),
                                                                  ray_ge(g, ray_scan(g, "k"), ray_const_i64(g, 0))); }

/* Finding C1: the OP_FILTER consults on a generation-2 column read the
 * directory: same rows as generation 0, nothing materialized; a key outside
 * the zone's range is answered as an index hit on both generations. */
static test_result_t test_filter_consult_dir(void) {
    ray_op_t* (*preds[])(ray_graph_t*) = { cpred_v_gt, cpred_g_eq, cpred_h_out, cpred_h_rng };
    for (size_t i = 0; i < sizeof preds / sizeof preds[0]; i++) {
        int64_t n[2]; uint64_t hits[2];
        for (int gen = 0; gen < 2; gen++) {
            ray_t* t = sr_load(gen);
            TEST_ASSERT_NOT_NULL(t);
            uint64_t h0 = ray_idx_hits[IDX_SITE_FILTER_ZONE];
            uint64_t mat = 0;
            n[gen] = sr_filter_rows(t, preds[i], &mat);
            hits[gen] = ray_idx_hits[IDX_SITE_FILTER_ZONE] - h0;
            if (gen) TEST_ASSERT_FMT(mat == 0, "predicate %zu: %llu materializations", i, (unsigned long long)mat);
            ray_release(t);
        }
        TEST_ASSERT_FMT(n[0] >= 0 && n[0] == n[1], "predicate %zu: %lld vs %lld rows", i, (long long)n[0], (long long)n[1]);
        TEST_ASSERT_EQ_U(hits[0], hits[1]);
    }
    PASS();
}

/* PI lazy hash on a generation-2 column: the first RAY_IDX_LAZY_BUILD_PROBES - 1
 * point lookups scan by range and materialize nothing; the next one builds
 * (materializing exactly the probed column once); later probes reuse it.
 * Every answer equals generation 0's. */
static test_result_t test_lazy_hash_enc(void) {
    ray_idx_lazy_clear();
    ray_t* t0 = sr_load(0);
    int64_t want = sr_filter_rows(t0, cpred_h_eq, NULL);
    ray_release(t0);
    TEST_ASSERT_EQ_I(want, 1);
    ray_idx_lazy_clear();
    ray_t* t = sr_load(2);                               /* one load: one encoded object */
    for (int p = 1; p < RAY_IDX_LAZY_BUILD_PROBES; p++) {
        uint64_t mat = 9;
        TEST_ASSERT_EQ_I(sr_filter_rows(t, cpred_h_eq, &mat), want);
        TEST_ASSERT_FMT(mat == 0, "probe %d materialized %llu", p, (unsigned long long)mat);
    }
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 0);
    uint64_t mat = 0;
    TEST_ASSERT_EQ_I(sr_filter_rows(t, cpred_h_eq, &mat), want);   /* the build */
    TEST_ASSERT_EQ_U(mat, 1);
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 1);
    TEST_ASSERT_EQ_I(sr_filter_rows(t, cpred_h_eq, &mat), want);   /* reuse */
    TEST_ASSERT_EQ_U(mat, 0);
    ray_release(t);
    ray_idx_lazy_clear();
    PASS();
}

/* The IN consult and the eq+range conjunction on generation 2, and the
 * .idx.uses count of a directory answer.  A set outside the chunk zone's
 * range is a zone hit on both generations; a set inside it is a pending
 * lazy probe (nothing materialized).  An eq+range list whose other
 * conjuncts do not fit the shape never probes h's lazy hash (generation 0
 * decodes the conjuncts and declines before the probe; generation 2 decides
 * that from metadata), a fitting one does. */
static ray_t* sr_i64_set(int64_t a, int64_t b) {
    ray_t* v = ray_vec_new(RAY_I64, 2);
    ((int64_t*)ray_data(v))[0] = a; ((int64_t*)ray_data(v))[1] = b;
    v->len = 2;
    return v;
}
static ray_op_t* cpred_h_in_out(ray_graph_t* g) {
    ray_t* v = sr_i64_set(1000004, 1000005);
    ray_op_t* o = ray_in(g, ray_scan(g, "h"), ray_const_vec(g, v));
    ray_release(v);
    return o;
}
static ray_op_t* cpred_h_in(ray_graph_t* g) {
    ray_t* v = sr_i64_set(7919, 15838);
    ray_op_t* o = ray_in(g, ray_scan(g, "h"), ray_const_vec(g, v));
    ray_release(v);
    return o;
}
static ray_op_t* cpred_h_eq_g(ray_graph_t* g) { return ray_and(g, ray_eq(g, ray_scan(g, "h"), ray_const_i64(g, 7919)),
                                                                  ray_eq(g, ray_scan(g, "g"), ray_const_i64(g, 3))); }
static ray_op_t* cpred_h_rng_v(ray_graph_t* g) { return ray_and(g, ray_eq(g, ray_scan(g, "h"), ray_const_i64(g, 7919)),
                                                                   ray_ge(g, ray_scan(g, "v"), ray_const_i64(g, 0))); }
static test_result_t test_filter_consult_enc_shapes(void) {
    ray_op_t* (*preds[])(ray_graph_t*) = { cpred_h_in_out, cpred_h_in, cpred_h_eq_g, cpred_h_rng_v, cpred_h_rng };
    const int64_t want_entries[] = { 0, 1, 0, 0, 1 };   /* lazy memos after one run */
    /* the consults decode nothing; the `in` scan that answers a pending
     * probe range-reads its column */
    const uint64_t want_mat[] = { 0, 0, 0, 0, 0 };
    for (size_t i = 0; i < sizeof preds / sizeof preds[0]; i++) {
        int64_t n[2]; uint64_t hits[2];
        for (int gen = 0; gen < 2; gen++) {
            ray_idx_lazy_clear();
            ray_t* t = sr_load(gen);
            TEST_ASSERT_NOT_NULL(t);
            uint64_t h0 = ray_idx_hits[IDX_SITE_FILTER_ZONE];
            uint64_t mat = 0;
            n[gen] = sr_filter_rows(t, preds[i], &mat);
            hits[gen] = ray_idx_hits[IDX_SITE_FILTER_ZONE] - h0;
            if (gen) TEST_ASSERT_FMT(mat == want_mat[i], "predicate %zu: %llu materializations", i, (unsigned long long)mat);
            TEST_ASSERT_FMT(ray_idx_lazy_entries() == want_entries[i], "predicate %zu gen %d: %lld lazy entries",
                            i, gen, (long long)ray_idx_lazy_entries());
            ray_release(t);
        }
        TEST_ASSERT_FMT(n[0] >= 0 && n[0] == n[1], "predicate %zu: %lld vs %lld rows", i, (long long)n[0], (long long)n[1]);
        TEST_ASSERT_EQ_U(hits[0], hits[1]);
        if (i == 0) TEST_ASSERT_EQ_U(hits[1], 1);
    }
    ray_idx_lazy_clear();
    /* a directory answer counts under its column */
    sr_eval_ok("(set T (.db.splayed.get \"" SR_G2 "\"))");
    sr_eval_ok("(do (.idx.uses T) null)");
    ray_t* c = sr_eval("(count (at (select {h: h from: T where: (== h 1000004)}) 'h))");
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c) && c->i64 == 0);
    sr_drop(c);
    ray_t* u = sr_eval("(at (.idx.uses T) 'h)");
    TEST_ASSERT_TRUE(u && !RAY_IS_ERR(u) && u->i64 == 1);
    sr_drop(u);
    PASS();
}

/* A pending probe on a generation-2 column is a memo of its encoded
 * payload that holds no reference: the table's release frees the column
 * (and its mapping) at once, and the memo goes with it; any header copy of
 * the column meets the same memo. */
static test_result_t test_lazy_memo_goes_with_table(void) {
    ray_idx_lazy_clear();
    int64_t live0 = ray_enccol_test_live();
    ray_t* t = sr_load(2);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_TRUE(ray_enccol_test_live() > live0);
    TEST_ASSERT_EQ_I(sr_filter_rows(t, cpred_h_eq, NULL), 1);
    TEST_ASSERT_EQ_I(ray_idx_lazy_entries(), 1);
    TEST_ASSERT_EQ_I(sr_filter_rows(t, cpred_h_eq, NULL), 1);
    TEST_ASSERT_EQ_I(ray_idx_lazy_entries(), 1);             /* one memo, its work grows */
    ray_release(t);
    TEST_ASSERT_EQ_I(ray_enccol_test_live(), live0);
    TEST_ASSERT_EQ_I(ray_idx_lazy_entries(), 0);
    PASS();
}

/* ---- Fix round 1 ---------------------------------------------------- */

/* Review I1: a column under 65536 rows records ZONE (not CHUNK_ZONE), so the
 * OP_FILTER single-compare consult answers it from ray_enc_zone_class.  A
 * 1000-row table, a = row (I64), f = row (F64, 0.0 first): ALL, NONE and
 * undecided compares, the F64 ones against -0.0 (== +0.0), give these rows
 * on both generations through a graph FILTER and through select; generation
 * 2 materializes nothing at the consult, and the zone hits are equal. */
#define SR_Z0 SR_TMP "/z0/"
#define SR_Z2 SR_TMP "/z2/"
static ray_op_t* zp_a_ge0(ray_graph_t* g)   { return ray_ge(g, ray_scan(g, "a"), ray_const_i64(g, 0)); }
static ray_op_t* zp_a_gt999(ray_graph_t* g) { return ray_gt(g, ray_scan(g, "a"), ray_const_i64(g, 999)); }
static ray_op_t* zp_a_ge499(ray_graph_t* g) { return ray_ge(g, ray_scan(g, "a"), ray_const_i64(g, 499)); }
static ray_op_t* zp_a_ne1000(ray_graph_t* g){ return ray_ne(g, ray_scan(g, "a"), ray_const_i64(g, 1000)); }
static ray_op_t* zp_a_eq1000(ray_graph_t* g){ return ray_eq(g, ray_scan(g, "a"), ray_const_i64(g, 1000)); }
static ray_op_t* zp_f_ge_nz(ray_graph_t* g) { return ray_ge(g, ray_scan(g, "f"), ray_const_f64(g, -0.0)); }
static ray_op_t* zp_f_lt_nz(ray_graph_t* g) { return ray_lt(g, ray_scan(g, "f"), ray_const_f64(g, -0.0)); }
static ray_op_t* zp_f_le_nz(ray_graph_t* g) { return ray_le(g, ray_scan(g, "f"), ray_const_f64(g, -0.0)); }
static ray_op_t* zp_f_gt_nz(ray_graph_t* g) { return ray_gt(g, ray_scan(g, "f"), ray_const_f64(g, -0.0)); }
static ray_op_t* zp_f_gt998(ray_graph_t* g) { return ray_gt(g, ray_scan(g, "f"), ray_const_f64(g, 998.5)); }
static test_result_t test_zone_class_dir_small(void) {
    /* under 65536 rows a save records only a zone the column carries (the
     * converters attach one to every such column) */
    sr_eval_ok("(set Z (table [a f] (list (.idx.zone (til 1000)) (.idx.zone (as 'F64 (til 1000))))))");
    sr_eval_ok("(.db.splayed.set \"" SR_Z0 "\" Z {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_Z2 "\" Z {compress: true})");
    static const struct { ray_op_t* (*build)(ray_graph_t*); const char* where; int64_t rows; uint64_t hit; } cs[] = {
        { zp_a_ge0,    "(>= a 0)",     1000, 1 },   /* ALL */
        { zp_a_gt999,  "(> a 999)",       0, 1 },   /* NONE */
        { zp_a_ge499,  "(>= a 499)",    501, 0 },   /* undecided: the scan */
        { zp_a_ne1000, "(!= a 1000)",  1000, 1 },   /* ALL */
        { zp_a_eq1000, "(== a 1000)",     0, 1 },   /* NONE */
        { zp_f_ge_nz,  "(>= f -0.0)",  1000, 1 },   /* ALL: 0.0 >= -0.0 */
        { zp_f_lt_nz,  "(< f -0.0)",      0, 1 },   /* NONE */
        { zp_f_le_nz,  "(<= f -0.0)",     1, 0 },   /* undecided: row 0 equals */
        { zp_f_gt_nz,  "(> f -0.0)",    999, 0 },   /* undecided */
        { zp_f_gt998,  "(> f 998.5)",     1, 0 },   /* undecided */
    };
    for (size_t i = 0; i < sizeof cs / sizeof cs[0]; i++) {
        for (int gen = 0; gen < 2; gen++) {
            ray_t* t = sr_eval(gen ? "(.db.splayed.get \"" SR_Z2 "\")" : "(.db.splayed.get \"" SR_Z0 "\")");
            TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
            ray_col_meta_t m;
            TEST_ASSERT_TRUE(ray_table_col_meta(t, ray_sym_intern("a", 1), &m));
            TEST_ASSERT_EQ_I(m.index_kind, RAY_IDX_ZONE);
            TEST_ASSERT_TRUE(gen ? m.enc != NULL : m.enc == NULL);
            TEST_ASSERT_TRUE(ray_table_col_meta(t, ray_sym_intern("f", 1), &m));
            TEST_ASSERT_EQ_I(m.index_kind, RAY_IDX_ZONE);
            uint64_t h0 = ray_idx_hits[IDX_SITE_FILTER_ZONE];
            uint64_t mat = 9;
            int64_t n = sr_filter_rows(t, cs[i].build, &mat);
            uint64_t hit = ray_idx_hits[IDX_SITE_FILTER_ZONE] - h0;
            TEST_ASSERT_FMT(n == cs[i].rows, "%s gen %d: %lld rows, want %lld", cs[i].where, gen,
                            (long long)n, (long long)cs[i].rows);
            TEST_ASSERT_FMT(hit == cs[i].hit, "%s gen %d: %llu zone hits", cs[i].where, gen, (unsigned long long)hit);
            if (gen) TEST_ASSERT_FMT(mat == 0, "%s: %llu materializations", cs[i].where, (unsigned long long)mat);
            ray_release(t);
            /* the same through select (the projection reads a or f after the filter) */
            sr_evalf_ok("(set T (.db.splayed.get \"%s\"))", gen ? SR_Z2 : SR_Z0);
            char q[160];
            snprintf(q, sizeof q, "(count (at (select {a: a from: T where: %s}) 'a))", cs[i].where);
            ray_t* r = sr_eval(q);
            TEST_ASSERT_FMT(r && !RAY_IS_ERR(r) && r->i64 == cs[i].rows, "%s gen %d: select gave %lld",
                            q, gen, r && !RAY_IS_ERR(r) ? (long long)r->i64 : -1LL);
            sr_drop(r);
        }
    }
    PASS();
}

/* Review minor 1: .idx.uses keys a directory answer by the encoded payload,
 * as the lazy cache does: an answer through a header copy of the column (a
 * second table sharing the payload) shows in the original table's uses. */
static ray_op_t* cpred_h_out2(ray_graph_t* g) { return ray_eq(g, ray_scan(g, "h"), ray_const_i64(g, -5)); }
static test_result_t test_uses_shared_payload(void) {
    ray_t* t = sr_load(2);
    TEST_ASSERT_NOT_NULL(t);
    ray_t* enc = ray_table_get_col_enc(t, ray_sym_intern("h", 1));
    TEST_ASSERT_NOT_NULL(enc);
    ray_enccol_ref(ray_enccol_of(enc));                  /* the reference the new header holds */
    ray_t* cp = ray_enccol_wrap(ray_enccol_of(enc));     /* a second header, one payload */
    TEST_ASSERT_TRUE(cp && !RAY_IS_ERR(cp) && cp != enc && ray_enccol_of(cp) == ray_enccol_of(enc));
    ray_t* t2 = ray_table_add_col(ray_table_new(1), ray_sym_intern("h", 1), cp);
    ray_release(cp);
    TEST_ASSERT_TRUE(t2 && !RAY_IS_ERR(t2));
    TEST_ASSERT_TRUE(ray_table_get_col_enc(t2, ray_sym_intern("h", 1)) != enc);
    ray_t* u = ray_idx_uses_fn(t);                       /* arms the counters, resets them */
    TEST_ASSERT_TRUE(u && !RAY_IS_ERR(u));
    ray_release(u);
    uint64_t mat = 9;
    TEST_ASSERT_EQ_I(sr_filter_rows(t2, cpred_h_out2, &mat), 0);   /* a zone exclusion on the copy */
    TEST_ASSERT_EQ_U(mat, 0);
    u = ray_idx_uses_fn(t);
    TEST_ASSERT_TRUE(u && !RAY_IS_ERR(u) && ray_dict_len(u) == 1);
    ray_t* key = ray_sym(ray_sym_intern("h", 1));
    ray_t* hv = ray_dict_get(u, key);
    TEST_ASSERT_TRUE(hv && !RAY_IS_ERR(hv) && hv->i64 == 1);
    sr_drop(hv); ray_release(key); ray_release(u);
    ray_release(t2); ray_release(t);
    PASS();
}

/* Review minor 5: compare literals on chunk boundaries of k (= row; chunk 1
 * is rows 65536..131071), through the fused selection, with absolute counts
 * and the vectors decoded: a decided chunk 1 decodes nothing; in an
 * undecided one the descriptors decide every vector but the one holding the
 * literal (Task 17b), so one vector is decoded.  A wrong boundary in
 * zone_cmp_core shows without a generation-0 comparison. */
static test_result_t test_zone_dir_boundaries(void) {
    static const struct { uint16_t op; int64_t lit; int64_t rows; bool decoded; } cs[] = {
        { OP_EQ, 65536, 1, true },       { OP_EQ, 131071, 1, true },
        { OP_NE, 65536, 199999, true },  { OP_NE, 131071, 199999, true },
        { OP_LT, 65536, 65536, false },  { OP_LT, 131071, 131071, true },    /* chunk min: chunk 1 all-fail */
        { OP_LE, 65536, 65537, true },   { OP_LE, 131071, 131072, false },   /* chunk max: chunk 1 all-pass */
        { OP_GT, 65536, 134463, true },  { OP_GT, 131071, 68928, false },    /* chunk max: chunk 1 all-fail */
        { OP_GE, 65536, 134464, false }, { OP_GE, 131071, 68929, true },     /* chunk min: chunk 1 all-pass */
    };
    ray_t* t = sr_load(2);
    TEST_ASSERT_NOT_NULL(t);
    for (size_t i = 0; i < sizeof cs / sizeof cs[0]; i++) {
        ray_graph_t* g = ray_graph_new(t);
        ray_op_t* k = ray_scan(g, "k");
        ray_op_t* c = ray_const_i64(g, cs[i].lit);
        ray_op_t* p = cs[i].op == OP_EQ ? ray_eq(g, k, c) : cs[i].op == OP_NE ? ray_ne(g, k, c)
                    : cs[i].op == OP_LT ? ray_lt(g, k, c) : cs[i].op == OP_LE ? ray_le(g, k, c)
                    : cs[i].op == OP_GT ? ray_gt(g, k, c) : ray_ge(g, k, c);
        ray_enc_counters_reset();
        bool all = false;
        ray_t* s = exec_pred_to_selection(g, p, ray_table_nrows(t), &all);
        TEST_ASSERT_FMT(!(s && RAY_IS_ERR(s)) && (s || all), "case %zu: no selection", i);
        int64_t n = s ? ray_rowsel_meta(s)->total_pass : ray_table_nrows(t);
        ray_enc_counters_t kc; ray_enc_counters_get(&kc);
        TEST_ASSERT_FMT(n == cs[i].rows, "case %zu (op %u, %lld): %lld rows, want %lld", i, cs[i].op,
                        (long long)cs[i].lit, (long long)n, (long long)cs[i].rows);
        TEST_ASSERT_FMT(kc.decoded_bytes == (cs[i].decoded ? 1024u * 8u : 0u), "case %zu: %llu bytes decoded",
                        i, (unsigned long long)kc.decoded_bytes);
        TEST_ASSERT_EQ_U(kc.materialized, 0);
        if (s) ray_rowsel_release(s);
        ray_graph_free(g);
    }
    ray_release(t);
    PASS();
}

/* ---- Task 6: fused predicates on range reads ------------------------- */

/* Compile a fused predicate over t0 and t2 and compare bits per morsel,
 * aligned and then shifted by 500 rows (a window straddles a vector
 * boundary: fp_eval_pred_rs splits it); nothing is materialized. */
static int64_t g_fp_pass;                                 /* passing rows of the aligned sweep */
static test_result_t sr_fp_same_on(ray_t* t0, ray_t* t2, ray_op_t* (*build)(ray_graph_t*)) {
    g_fp_pass = 0;
    TEST_ASSERT_NOT_NULL(t0); TEST_ASSERT_NOT_NULL(t2);
    ray_graph_t* g0 = ray_graph_new(t0); ray_graph_t* g2 = ray_graph_new(t2);
    fp_pred_t p0, p2;
    memset(&p0, 0, sizeof p0); memset(&p2, 0, sizeof p2);
    ray_t* e0 = NULL; ray_t* e2 = NULL;
    TEST_ASSERT_EQ_I(fp_compile_pred(g0, build(g0), t0, &p0, &e0), 0);
    TEST_ASSERT_NULL(e0);
    ray_enc_counters_reset();
    TEST_ASSERT_EQ_I(fp_compile_pred(g2, build(g2), t2, &p2, &e2), 0);
    TEST_ASSERT_NULL(e2);
    bool any_enc = false;                                          /* generation 2 reads by range */
    for (uint8_t i = 0; i < p2.n_children; i++) any_enc |= p2.children[i].enc;
    TEST_ASSERT_TRUE(any_enc);
    fp_scratch_t sc; fp_scratch_init(&sc);
    uint8_t a[RAY_MORSEL_ELEMS], b[RAY_MORSEL_ELEMS];
    int64_t n = ray_table_nrows(t0);
    for (int64_t off = 0; off <= 500; off += 500) {
        for (int64_t ms = off; ms < n; ms += RAY_MORSEL_ELEMS) {
            int64_t me = ms + RAY_MORSEL_ELEMS < n ? ms + RAY_MORSEL_ELEMS : n;
            fp_eval_pred(&p0, ms, me, a);
            ray_t* e = fp_eval_pred_rs(&p2, ms, me, b, &sc);
            TEST_ASSERT_NULL(e);
            TEST_ASSERT_FMT(memcmp(a, b, (size_t)(me - ms)) == 0, "morsel at %lld", (long long)ms);
            if (off == 0) for (int64_t i = 0; i < me - ms; i++) g_fp_pass += b[i];
        }
    }
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    fp_scratch_free(&sc); fp_pred_cleanup(&p0); fp_pred_cleanup(&p2);
    ray_graph_free(g0); ray_graph_free(g2);
    PASS();
}
static test_result_t sr_fp_same(ray_op_t* (*build)(ray_graph_t*)) {
    ray_t* t0 = sr_load(0); ray_t* t2 = sr_load(2);
    test_result_t r = sr_fp_same_on(t0, t2, build);
    if (t0) ray_release(t0);
    if (t2) ray_release(t2);
    return r;
}
static ray_op_t* fpp_s_eq(ray_graph_t* g) {
    ray_t* lit = ray_sym(ray_sym_intern("c", 1));
    ray_op_t* o = ray_eq(g, ray_scan(g, "s"), ray_const_atom(g, lit)); ray_release(lit); return o;
}
static ray_op_t* fpp_v_le(ray_graph_t* g) { return ray_le(g, ray_scan(g, "v"), ray_const_i64(g, 4000)); }
static ray_op_t* fpp_d_ge(ray_graph_t* g) {
    ray_t* lit = ray_date(10);                       /* DATE takes a DATE literal only */
    ray_op_t* o = ray_ge(g, ray_scan(g, "d"), ray_const_atom(g, lit)); ray_release(lit); return o;
}
static ray_op_t* fpp_and3(ray_graph_t* g) {
    return ray_and(g, ray_and(g, fpp_d_ge(g), fpp_s_eq(g)),
                   ray_gt(g, ray_scan(g, "f"), ray_const_f64(g, 120000.0)));
}
static ray_op_t* fpp_g_in(ray_graph_t* g) {
    ray_t* set = ray_vec_new(RAY_I64, 2); set->len = 2;
    ((int64_t*)ray_data(set))[0] = 2; ((int64_t*)ray_data(set))[1] = 5;
    ray_op_t* o = ray_in(g, ray_scan(g, "g"), ray_const_vec(g, set)); ray_release(set); return o;
}
static ray_op_t* fpp_s_like(ray_graph_t* g) {
    ray_t* pat = ray_str("[bd]*", 5);
    ray_op_t* o = ray_like(g, ray_scan(g, "s"), ray_const_atom(g, pat)); ray_release(pat); return o;
}
static test_result_t test_fp_range_equal(void) {
    test_result_t r;
    if ((r = sr_fp_same(fpp_s_eq)).status != TEST_PASS) return r;   /* file-domain SYM */
    if ((r = sr_fp_same(fpp_v_le)).status != TEST_PASS) return r;   /* nullable: typed compare */
    if ((r = sr_fp_same(fpp_and3)).status != TEST_PASS) return r;   /* three children */
    if ((r = sr_fp_same(fpp_g_in)).status != TEST_PASS) return r;   /* FP_IN arm */
    if ((r = sr_fp_same(fpp_s_like)).status != TEST_PASS) return r; /* FP_LIKE arm, file-domain LUT */
    PASS();
}

/* The directory gate decides whole chunks before a decode: k < 70000
 * passes chunk 0 and fails chunks 2-3 from their extrema; in chunk 1 the
 * descriptors decide every vector but vector 68 (rows 69632..70655, Task
 * 17b), the only one decoded. */
static ray_op_t* fpp_k_lt(ray_graph_t* g) { return ray_lt(g, ray_scan(g, "k"), ray_const_i64(g, 70000)); }
static test_result_t test_fp_range_dir_gate(void) {
    test_result_t r;
    if ((r = sr_fp_same(fpp_k_lt)).status != TEST_PASS) return r;
    ray_t* t = sr_load(2);
    TEST_ASSERT_NOT_NULL(t);
    ray_graph_t* g = ray_graph_new(t);
    fp_pred_t p; ray_t* e = NULL;
    TEST_ASSERT_EQ_I(fp_compile_pred(g, fpp_k_lt(g), t, &p, &e), 0);
    TEST_ASSERT_TRUE(p.children[0].enc);
    ray_enc_counters_reset();
    const uint64_t dv0 = ray_enccol_test_desc_validations();
    fp_scratch_t sc; fp_scratch_init(&sc);
    uint8_t b[RAY_MORSEL_ELEMS];
    int64_t n = ray_table_nrows(t), pass = 0;
    for (int64_t ms = 0; ms < n; ms += RAY_MORSEL_ELEMS) {
        int64_t me = ms + RAY_MORSEL_ELEMS < n ? ms + RAY_MORSEL_ELEMS : n;
        TEST_ASSERT_NULL(fp_eval_pred_rs(&p, ms, me, b, &sc));
        for (int64_t i = 0; i < me - ms; i++) pass += b[i];
    }
    fp_scratch_free(&sc);
    TEST_ASSERT_EQ_I(pass, 70000);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 1024 * 8);
    /* the chunk gate decides chunks 0, 2 and 3 before their descriptors
     * are read: only chunk 1's are validated */
    TEST_ASSERT_EQ_U(ray_enccol_test_desc_validations() - dv0, 1);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    fp_pred_cleanup(&p); ray_graph_free(g); ray_release(t);
    PASS();
}

/* A generation-2 column another consumer materialized is read as a plain
 * child over the published vector (no second decode), beside an encoded
 * child read by range. */
static ray_op_t* fpp_v_k(ray_graph_t* g) { return ray_and(g, fpp_v_le(g), fpp_k_lt(g)); }
static test_result_t test_fp_range_ready_child(void) {
    ray_t* t0 = sr_load(0); ray_t* t2 = sr_load(2);
    TEST_ASSERT_NOT_NULL(t0); TEST_ASSERT_NOT_NULL(t2);
    ray_col_src_t s; TEST_ASSERT_NULL(ray_table_col_src(t2, ray_sym_intern("v", 1), &s));
    ray_t* col = NULL;
    TEST_ASSERT_NULL(ray_col_src_plain(&s, &col));                 /* publishes v's materialization */
    ray_col_src_release(&s);
    ray_graph_t* g = ray_graph_new(t2);
    fp_pred_t p; ray_t* e = NULL;
    TEST_ASSERT_EQ_I(fp_compile_pred(g, fpp_v_k(g), t2, &p, &e), 0);
    TEST_ASSERT_EQ_I(p.n_children, 2);
    for (uint8_t i = 0; i < p.n_children; i++) {
        bool is_v = p.children[i].typed_compare;                   /* v is nullable: the typed leg */
        TEST_ASSERT_EQ_I(p.children[i].enc, !is_v);
        if (is_v) TEST_ASSERT_NOT_NULL(p.children[i].col_obj);
    }
    fp_pred_cleanup(&p); ray_graph_free(g);
    test_result_t r = sr_fp_same_on(t0, t2, fpp_v_k);             /* asserts no new materialization */
    ray_release(t0); ray_release(t2);
    return r;
}

/* A materialization published after the compile: the child stays encoded
 * (enc was decided at compile), so the per-morsel bind reads the published
 * vector in place.  k < 70000 leaves chunk 1 undecided by the directory
 * gate: without the in-place read its 64 vectors are decoded again. */
static test_result_t test_fp_range_late_materialized(void) {
    ray_t* t0 = sr_load(0); ray_t* t2 = sr_load(2);
    TEST_ASSERT_NOT_NULL(t0); TEST_ASSERT_NOT_NULL(t2);
    ray_graph_t* g0 = ray_graph_new(t0); ray_graph_t* g2 = ray_graph_new(t2);
    fp_pred_t p0, p2; ray_t* e = NULL;
    TEST_ASSERT_EQ_I(fp_compile_pred(g0, fpp_k_lt(g0), t0, &p0, &e), 0);
    TEST_ASSERT_EQ_I(fp_compile_pred(g2, fpp_k_lt(g2), t2, &p2, &e), 0);
    TEST_ASSERT_NULL(e);
    TEST_ASSERT_TRUE(p2.children[0].enc);                          /* compiled before the publish */
    ray_col_src_t s; TEST_ASSERT_NULL(ray_table_col_src(t2, ray_sym_intern("k", 1), &s));
    ray_t* col = NULL;
    TEST_ASSERT_NULL(ray_col_src_plain(&s, &col));                 /* publishes k's materialization */
    ray_col_src_release(&s);
    TEST_ASSERT_NOT_NULL(col);
    ray_enc_counters_reset();
    fp_scratch_t sc; fp_scratch_init(&sc);
    uint8_t a[RAY_MORSEL_ELEMS], b[RAY_MORSEL_ELEMS];
    int64_t n = ray_table_nrows(t0), pass = 0;
    for (int64_t off = 0; off <= 500; off += 500) {
        for (int64_t ms = off; ms < n; ms += RAY_MORSEL_ELEMS) {
            int64_t me = ms + RAY_MORSEL_ELEMS < n ? ms + RAY_MORSEL_ELEMS : n;
            fp_eval_pred(&p0, ms, me, a);
            TEST_ASSERT_NULL(fp_eval_pred_rs(&p2, ms, me, b, &sc));
            TEST_ASSERT_FMT(memcmp(a, b, (size_t)(me - ms)) == 0, "morsel at %lld", (long long)ms);
            if (off == 0) for (int64_t i = 0; i < me - ms; i++) pass += b[i];
        }
    }
    fp_scratch_free(&sc);
    TEST_ASSERT_EQ_I(pass, 70000);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 0);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    fp_pred_cleanup(&p0); fp_pred_cleanup(&p2);
    ray_graph_free(g0); ray_graph_free(g2);
    ray_release(t0); ray_release(t2);
    PASS();
}

/* An AND stops at the first child that fails the whole window: k == 500
 * (ordered first: an 8-byte EQ) passes in vector 0 only, so v <= 4000 is
 * read for that one vector (4 KB).  Without the stop v is decoded in every
 * window (nullable v takes the typed compare, which the directory gate does
 * not decide); k decodes vector 0 only (the gate decides chunks 1-3, the
 * descriptors chunk 0's other vectors, Task 17b). */
static ray_op_t* fpp_k_eq_v(ray_graph_t* g) {
    return ray_and(g, ray_eq(g, ray_scan(g, "k"), ray_const_i64(g, 500)), fpp_v_le(g));
}
static test_result_t test_fp_range_and_stops(void) {
    test_result_t r;
    if ((r = sr_fp_same(fpp_k_eq_v)).status != TEST_PASS) return r;
    TEST_ASSERT_EQ_I(g_fp_pass, 1);
    ray_t* t = sr_load(2);
    TEST_ASSERT_NOT_NULL(t);
    ray_graph_t* g = ray_graph_new(t);
    fp_pred_t p; ray_t* e = NULL;
    TEST_ASSERT_EQ_I(fp_compile_pred(g, fpp_k_eq_v(g), t, &p, &e), 0);
    TEST_ASSERT_EQ_I(p.n_children, 2);
    TEST_ASSERT_TRUE(p.children[0].enc && p.children[0].op == FP_EQ && p.children[0].col_esz == 8);
    TEST_ASSERT_TRUE(p.children[1].enc);
    ray_enc_counters_reset();
    fp_scratch_t sc; fp_scratch_init(&sc);
    uint8_t b[RAY_MORSEL_ELEMS];
    int64_t n = ray_table_nrows(t);
    for (int64_t ms = 0; ms < n; ms += RAY_MORSEL_ELEMS) {
        int64_t me = ms + RAY_MORSEL_ELEMS < n ? ms + RAY_MORSEL_ELEMS : n;
        TEST_ASSERT_NULL(fp_eval_pred_rs(&p, ms, me, b, &sc));
    }
    fp_scratch_free(&sc);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 1024 * 8 + 1024 * 4);        /* k and v: vector 0 */
    TEST_ASSERT_EQ_U(k.materialized, 0);
    fp_pred_cleanup(&p); ray_graph_free(g); ray_release(t);
    PASS();
}

/* STR children read through the view's pool: typed compares (EQ, LT on
 * the bytes; empty cells are null, below every value) and LIKE.  4000
 * rows: four vectors, the last partial. */
static ray_t* sr_str_tbl(int gen) {
    char dir[512], sym[512];
    snprintf(dir, sizeof dir, "%s/str%d", sr_root(), gen);
    snprintf(sym, sizeof sym, "%s/str%d.sym", sr_root(), gen);
    ray_t* v = ray_vec_new(RAY_STR, 4000);
    char buf[64];
    for (int i = 0; i < 4000; i++) {
        int n = i % 7 == 0 ? 0
              : i % 3 == 0 ? snprintf(buf, sizeof buf, "s%05d", i)
                           : snprintf(buf, sizeof buf, "a pooled string of row %05d", i);
        v = ray_str_vec_append(v, buf, (size_t)n);
    }
    if (ray_vec_is_null(v, 0)) v->attrs |= RAY_ATTR_HAS_NULLS;
    ray_t* t = ray_table_new(1);
    t = ray_table_add_col(t, ray_sym_intern("t", 1), v); ray_release(v);
    ray_err_t rc = ray_splay_save_mode(t, dir, sym, gen ? RAY_COMPRESS_ON : RAY_COMPRESS_OFF);
    ray_release(t);
    if (rc != RAY_OK) return NULL;
    t = ray_read_splayed(dir, sym);                                 /* mapped: generation 2 stays encoded */
    return (t && !RAY_IS_ERR(t)) ? t : NULL;
}
static ray_op_t* fpp_t_eq(ray_graph_t* g) {
    ray_t* lit = ray_str("s00039", 6);      /* 39 % 3 == 0, 39 % 7 != 0: a cell */
    ray_op_t* o = ray_eq(g, ray_scan(g, "t"), ray_const_atom(g, lit)); ray_release(lit); return o;
}
static ray_op_t* fpp_t_lt(ray_graph_t* g) {
    ray_t* lit = ray_str("s01000", 6);
    ray_op_t* o = ray_lt(g, ray_scan(g, "t"), ray_const_atom(g, lit)); ray_release(lit); return o;
}
static ray_op_t* fpp_t_like(ray_graph_t* g) {
    ray_t* pat = ray_str("*row 0*1", 8);
    ray_op_t* o = ray_like(g, ray_scan(g, "t"), ray_const_atom(g, pat)); ray_release(pat); return o;
}
static test_result_t test_fp_range_str(void) {
    ray_op_t* (*cases[])(ray_graph_t*) = { fpp_t_eq, fpp_t_lt, fpp_t_like };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        ray_t* t0 = sr_str_tbl(0); ray_t* t2 = sr_str_tbl(2);
        test_result_t r = sr_fp_same_on(t0, t2, cases[i]);
        if (t0) ray_release(t0);
        if (t2) ray_release(t2);
        if (r.status != TEST_PASS) return r;
        TEST_ASSERT_FMT(g_fp_pass > 0, "case %zu matches no row", i);   /* never vacuous */
    }
    PASS();
}

/* ---- Task 7: top-k and take scans on range reads -------------------- */

/* SR_SAME through the fused top-k / take: both generations take it. */
static test_result_t sr_fused_same(const char* q, uint64_t want_mat) {
    ray_fused_topk_counters_reset();
    test_result_t r = sr_same(q, want_mat);
    if (r.status != TEST_PASS) return r;
    uint64_t ran = 0, declined = 0;
    ray_fused_topk_counters(&ran, &declined);
    TEST_ASSERT_FMT(ran == 2 && declined == 0, "%s: fused ran %llu, declined %llu", q,
                    (unsigned long long)ran, (unsigned long long)declined);
    PASS();
}
#define SR_FUSED_SAME(q, m) do { test_result_t r_ = sr_fused_same((q), (m)); if (r_.status != TEST_PASS) return r_; } while (0)

/* Top-k: results equal generation 0; the materializations are exactly the
 * sort keys: the projected columns are gathered by row list (P3 Task 6). */
static test_result_t test_topk_materializes_keys(void) {
    SR_FUSED_SAME("(select {k: k from: T where: (> v 1000) desc: k take: 5})", 1);            /* k: key and output */
    SR_FUSED_SAME("(select {g: g k: k from: T where: (> v 1000) asc: [g k] take: 7})", 2);    /* two keys */
    SR_FUSED_SAME("(select {f: f from: T where: (== g 3) desc: k take: 3})", 1);              /* key k; f gathered */
    SR_FUSED_SAME("(select {k: k from: T where: (< v 50) take: 4})", 0);                      /* take scan: k gathered */
    SR_FUSED_SAME("(select {k: k from: T where: (< v 50) take: -4})", 0);                     /* take from the end */
    PASS();
}

/* A range read the top-k or take scan makes fails: the query's error, the
 * column's path in the message; an interrupted scan is `cancel`.  The
 * corrupt chunk is 2.  v > 400000 decides chunks 0 and 1 and leaves chunk
 * 2 undecided, so the ascending top-k and the forward take read it; v ==
 * 395000 (row 131669) leaves only chunk 2 undecided, so the backward take
 * and the descending top-k read it whatever the schedule. */
static test_result_t test_topk_take_error(void) {
    static const char* const qs[] = {
        "(select {k: k from: T where: (> v 400000) asc: k take: 5})",
        "(select {k: k from: T where: (> v 400000) take: 4})",
        "(select {k: k from: T where: (== v 395000) take: -4})",           /* the backward take */
        "(select {k: k from: T where: (== v 395000) desc: k take: 5})",
    };
    char path[600];
    sr_make_bad("v", path, sizeof path);
    for (int i = 0; i < 4; i++) {
        sr_eval_ok("(set T (.db.splayed.get \"" SR_TMP "/bad/\"))");
        ray_fused_topk_counters_reset();
        ray_error_clear();
        ray_t* r = sr_eval(qs[i]);
        TEST_ASSERT_FMT(r && RAY_IS_ERR(r), "%s: not an error", qs[i]);
        TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
        TEST_ASSERT_TRUE(strstr(ray_error_msg(), path) != NULL);
        ray_error_free(r);
        uint64_t ran = 0, declined = 0;
        ray_fused_topk_counters(&ran, &declined);
        TEST_ASSERT_EQ_U(ran, 1);
    }
    /* cancel: an interrupted scan is `cancel`, not a partial answer.
     * Generation 0: a generation-2 sort key's materialization would answer
     * the cancel before the scan runs. */
    ray_t* t = sr_load(0);
    TEST_ASSERT_NOT_NULL(t);
    ray_t* w = ray_parse("(> v 400000)");
    TEST_ASSERT_TRUE(w && !RAY_IS_ERR(w));
    int64_t ks = ray_sym_intern("k", 1);
    uint8_t asc = 0;
    for (int i = 0; i < 2; i++) {
        ray_request_interrupt();
        ray_t* r = i ? ray_fused_topk_select(t, w, &ks, &asc, 1, 5, &ks, NULL, 1)
                     : ray_fused_take_select(t, w, 4, &ks, NULL, 1);
        ray_clear_interrupt();
        TEST_ASSERT_FMT(r && RAY_IS_ERR(r), "%s: not an error", i ? "top-k" : "take");
        TEST_ASSERT_STR_EQ(ray_err_code(r), "cancel");
        ray_error_free(r);
    }
    ray_release(w); ray_release(t);
    PASS();
}

/* `in` / `not-in` selections (exec_in_to_selection) range-read a
 * generation-2 column: the same selection as generation 0, nothing
 * materialized, for an integer set, a nullable column under not-in, an F64
 * column and a file-domain SYM set with a literal absent from the domain. */
static ray_op_t* fpp_v_notin(ray_graph_t* g) {
    /* v = 3*row - 7: 293 and 2993 would be rows 100 and 1000, both null
     * (row % 10 == 0), so this set matches no row; not-in keeps every
     * non-null row (the SIMD small-set path) */
    ray_t* set = sr_i64_set(293, 2993);
    ray_op_t* o = ray_not_in(g, ray_scan(g, "v"), ray_const_vec(g, set)); ray_release(set); return o;
}
static ray_op_t* fpp_v_notin_big(ray_graph_t* g) {
    /* ten needles (the hashed path, past IN_SIMD_SET): rows 1-9 are
     * non-null members, 293 is a null row's would-be value; not-in must
     * still drop every null row */
    ray_t* set = ray_vec_new(RAY_I64, 10); set->len = 10;
    for (int j = 0; j < 9; j++) ((int64_t*)ray_data(set))[j] = 3 * (j + 1) - 7;
    ((int64_t*)ray_data(set))[9] = 293;
    ray_op_t* o = ray_not_in(g, ray_scan(g, "v"), ray_const_vec(g, set)); ray_release(set); return o;
}
static ray_op_t* fpp_f_notin(ray_graph_t* g) {
    ray_t* set = ray_vec_new(RAY_F64, 2); set->len = 2;          /* f = row, null every 13th */
    ((double*)ray_data(set))[0] = 12.0; ((double*)ray_data(set))[1] = 26.0;   /* 26 is a null row */
    ray_op_t* o = ray_not_in(g, ray_scan(g, "f"), ray_const_vec(g, set)); ray_release(set); return o;
}
static ray_op_t* fpp_w_in(ray_graph_t* g) {                   /* W16 SYM: 300 distinct */
    ray_t* set = ray_vec_new(RAY_SYM, 3); set->len = 3;
    ((int64_t*)ray_data(set))[0] = ray_sym_intern("7", 1);
    ((int64_t*)ray_data(set))[1] = ray_sym_intern("299", 3);
    ((int64_t*)ray_data(set))[2] = ray_sym_intern("nope", 4);
    ray_op_t* o = ray_in(g, ray_scan(g, "s"), ray_const_vec(g, set)); ray_release(set); return o;
}
static ray_op_t* fpp_f_in(ray_graph_t* g) {
    ray_t* set = ray_vec_new(RAY_F64, 3); set->len = 3;
    ((double*)ray_data(set))[0] = 12.0; ((double*)ray_data(set))[1] = 13.0; ((double*)ray_data(set))[2] = 150001.0;
    ray_op_t* o = ray_in(g, ray_scan(g, "f"), ray_const_vec(g, set)); ray_release(set); return o;
}
static ray_op_t* fpp_s_in(ray_graph_t* g) {
    ray_t* set = ray_vec_new(RAY_SYM, 3); set->len = 3;
    ((int64_t*)ray_data(set))[0] = ray_sym_intern("e", 1);
    ((int64_t*)ray_data(set))[1] = ray_sym_intern("zzq", 3);   /* absent from the file domain */
    ((int64_t*)ray_data(set))[2] = ray_sym_intern("b", 1);
    ray_op_t* o = ray_in(g, ray_scan(g, "s"), ray_const_vec(g, set)); ray_release(set); return o;
}
static test_result_t test_in_selection(void) {
    test_result_t r;
    if ((r = sr_sel_same(fpp_g_in)).status != TEST_PASS) return r;   /* (in g [2 5]) */
    if ((r = sr_sel_same(fpp_v_notin)).status != TEST_PASS) return r;
    if ((r = sr_sel_same(fpp_v_notin_big)).status != TEST_PASS) return r;
    if ((r = sr_sel_same(fpp_f_in)).status != TEST_PASS) return r;
    if ((r = sr_sel_same(fpp_f_notin)).status != TEST_PASS) return r;
    if ((r = sr_sel_same(fpp_s_in)).status != TEST_PASS) return r;
    /* a SYM column of 300 distinct ids is W16: the view's width class */
    sr_eval_ok("(set W (table [s k] (list (as 'SYM (as 'STR (% (til 70000) 300))) (til 70000))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/w0/\" W {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/w2/\" W {compress: true})");
    static const char* const wdirs[2] = { SR_TMP "/w0/", SR_TMP "/w2/" };
    if ((r = sr_sel_same_at(wdirs, fpp_w_in)).status != TEST_PASS) return r;
    SR_SAME("(count (select {from: T where: (in s ['a 'e])}))", 0);
    SR_SAME("(count (select {from: T where: (not-in g [0 1 2])}))", 0);
    /* the OP_FILTER IN consult (Task 5) then the fused selection: nothing decoded */
    int64_t want = 0;
    for (int64_t i = 0; i < 200000; i++) want += (i % 7 == 2 || i % 7 == 5);
    for (int gen = 0; gen < 2; gen++) {
        ray_t* t = sr_load(gen);
        uint64_t mat = 9;
        TEST_ASSERT_EQ_I(sr_filter_rows(t, fpp_g_in, &mat), want);
        if (gen) TEST_ASSERT_EQ_U(mat, 0);
        ray_release(t);
    }
    PASS();
}

/* A generation-2 column already materialized elsewhere is read in place
 * by the `in` selection: nothing decoded again. */
static test_result_t test_in_selection_reuses_materialized(void) {
    sr_evalf_ok("(set T (.db.splayed.get \"%s\"))", SR_G2);
    ray_t* s = sr_eval("(at T 'g)");                                /* forced at the top: materializes g */
    TEST_ASSERT_TRUE(s && !RAY_IS_ERR(s));
    ray_release(s);
    ray_t* t = sr_eval("T");
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    TEST_ASSERT_NOT_NULL(ray_enccol_decoded_peek(ray_table_get_col_enc(t, ray_sym_intern("g", 1))));
    ray_enc_counters_reset();
    ray_graph_t* g = ray_graph_new(t);
    bool all = false;
    ray_t* sel = exec_pred_to_selection(g, fpp_g_in(g), ray_table_nrows(t), &all);
    TEST_ASSERT_TRUE(sel && !RAY_IS_ERR(sel));
    TEST_ASSERT_EQ_I(ray_rowsel_meta(sel)->total_pass, 57143);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 0);
    ray_rowsel_release(sel);
    ray_graph_free(g); ray_release(t);
    PASS();
}

/* A corrupt chunk the `in` selection reads is its error, with the column
 * path; an interrupted selection is `cancel`, not a partial answer. */
static test_result_t test_in_selection_error(void) {
    char path[600];
    sr_make_bad("g", path, sizeof path);
    for (int i = 0; i < 2; i++) {
        ray_t* t = sr_eval("(.db.splayed.get \"" SR_TMP "/bad/\")");
        TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
        ray_graph_t* g = ray_graph_new(t);
        bool all = false;
        ray_error_clear();
        ray_t* s = exec_pred_to_selection(g, i ? fpp_g_in(g) : ray_not_in(g, ray_scan(g, "g"), ray_const_i64(g, 3)),
                                          ray_table_nrows(t), &all);
        TEST_ASSERT_TRUE(s && RAY_IS_ERR(s));
        TEST_ASSERT_STR_EQ(ray_err_code(s), "corrupt");
        TEST_ASSERT_TRUE(strstr(ray_error_msg(), path) != NULL);
        ray_error_free(s);
        ray_graph_free(g); ray_release(t);
    }
    for (int gen = 0; gen < 2; gen++) {
        ray_t* t = sr_load(gen);
        TEST_ASSERT_NOT_NULL(t);
        ray_graph_t* g = ray_graph_new(t);
        ray_op_t* pred = fpp_g_in(g);
        bool all = false;
        ray_request_interrupt();
        ray_t* s = exec_pred_to_selection(g, pred, ray_table_nrows(t), &all);
        ray_clear_interrupt();
        TEST_ASSERT_FMT(s && RAY_IS_ERR(s), "gen %d: not an error", gen);
        TEST_ASSERT_STR_EQ(ray_err_code(s), "cancel");
        ray_error_free(s);
        ray_graph_free(g); ray_release(t);
    }
    PASS();
}

/* ---- Task 9: reductions over a scan; (at tbl 'col) on an encoded slot ---- */

static test_result_t test_reduce_scan(void) {
    SR_SAME("(sum (at T 'k))", 0);
    SR_SAME("(avg (at T 'v))", 0);
    SR_SAME("(min (at T 'd))", 0);
    SR_SAME("(max (at T 'f))", 0);
    SR_SAME("(sum (at T 'u))", 0);
    SR_SAME("(first (at T 'v))", 0);
    SR_SAME("(last (at T 'f))", 0);
    /* stddev is not lazy-aware (it materializes its argument and computes
     * in two passes, unlike the one-pass OP_STDDEV): the column is read */
    SR_SAME("(stddev (at T 'g))", 1);
    SR_SAME("(sum (at T 'f))", 0);            /* nullable F64 over 200000 rows (integral values: exact in any order) */
    SR_SAME("(stddev (at T 'f))", 1);
    SR_SAME("(count (at T 'v))", 0);          /* count over the lazy: no force */
    SR_SAME("(at T 'k)", 1);                  /* a value handed to the user is a plain column */
    PASS();
}

/* Every served op and type over the lazy, and the shapes that keep reading
 * the column: same answers as generation 0. */
static test_result_t test_reduce_scan_ops(void) {
    static const char* const q0[] = {
        "(last (at T 'v))", "(first (at T 'f))", "(first (at T 'd))", "(last (at T 'd))",
        "(first (at T 'g))", "(last (at T 'b))", "(first (at T 'u))", "(first (at T 'k))",
        "(last (at T 'k))", "(min (at T 'b))", "(max (at T 'g))", "(min (at T 'v))",
        "(max (at T 'v))", "(min (at T 'f))", "(min (at T 'k))", "(max (at T 'h))",
        "(prod (at T 'b))", "(prod (at T 'g))", "(prod (at T 'f))", "(all (at T 'b))",
        "(any (at T 'b))", "(all (at T 'u))", "(any (at T 'v))", "(all (at T 'f))",
        "(avg (at T 'f))", "(avg (at T 'd))", "(avg (at T 'b))", "(sum (at T 'b))",
        "(sum (at T 'g))", "(sum (at T 'h))", "(avg (at T 'h))", "(count (at T 's))",
        "(count (at T 'd))", "(sum (at T 'e))", "(avg (at T 'e))", "(min (at T 'e))", "(max (at T 'e))",
        "(first (at T 'e))", "(last (at T 'e))", "(prod (at T 'e))", "(any (at T 'e))", NULL };
    for (int i = 0; q0[i]; i++) SR_SAME(q0[i], 0);
    /* F32: the answers' types are the vector builtins' (F64 for every one) */
    static const char* const qf[] = { "(sum (at T 'e))", "(avg (at T 'e))", "(min (at T 'e))", "(max (at T 'e))",
                                      "(first (at T 'e))", "(last (at T 'e))", NULL };
    for (int i = 0; qf[i]; i++) {
        int8_t ty[2];
        for (int gen = 0; gen < 2; gen++) {
            sr_evalf_ok("(set T (.db.splayed.get \"%s\"))", gen ? SR_G2 : SR_G0);
            ray_t* r = sr_eval(qf[i]);
            TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
            ty[gen] = r->type;
            sr_drop(r);
        }
        TEST_ASSERT_FMT(ty[0] == ty[1] && ty[0] == -RAY_F64, "%s: types %d / %d", qf[i], ty[0], ty[1]);
    }
    /* a reduction after a selection reads the selected rows, not the column */
    SR_SAME("(select {s: (sum v) m: (min f) from: T where: (> k 150000)})", 0);   /* the scalar route on range reads (Task 10; was 2) */
    /* not served by the range reduction: the column is materialized once */
    SR_SAME("(min (at T 's))", 1);
    SR_SAME("(count (distinct (at T 'g)))", 1);
    SR_SAME("(sum (reverse (at T 'k)))", 1);
    SR_SAME("(first (asc (at T 'v)))", 1);
    SR_SAME("(at T ['k 'v])", 2);             /* a list of plain columns, never of lazies */
    SR_SAME("(sum (+ 1 (at T 'k)))", 1);
    SR_SAME("(get T 'g)", 1);
    /* a column reference promoted to its scan by the other lazy-aware ops */
    SR_SAME("(count (distinct (at T 's)))", 1);
    SR_SAME("(first (upper (at T 's)))", 1);
    SR_SAME("(last (msum 3 (at T 'k)))", 1);
    SR_SAME("(sum ((fn [t] (at t 'k)) T))", 0);    /* returned from a lambda, still a reference */
    SR_SAME("(sum (at T 'k))", 0);
    SR_SAME("(do (set c (at T 'v)) (+ (sum c) (count c)))", 1);   /* a bound value is forced */
    PASS();
}

/* A builtin that rejects the column's type rejects the lazy with the
 * message it gives the column. */
static test_result_t test_reduce_scan_type_errors(void) {
    static const char* const q[] = { "(sum (at T 'd))", "(sum (at T 's))", "(avg (at T 's))",
                                     "(prod (at T 'd))", "(all (at T 'd))", "(any (at T 's))", NULL };
    for (int i = 0; q[i]; i++) {
        char msg[2][256]; const char* code[2];
        for (int gen = 0; gen < 2; gen++) {
            sr_evalf_ok("(set T (.db.splayed.get \"%s\"))", gen ? SR_G2 : SR_G0);
            ray_error_clear();
            ray_enc_counters_reset();
            ray_t* r = sr_eval(q[i]);
            TEST_ASSERT_FMT(r && RAY_IS_ERR(r), "gen %d: %s: not an error", gen, q[i]);
            code[gen] = ray_err_code(r);
            snprintf(msg[gen], sizeof msg[gen], "%s", ray_error_msg());
            ray_error_free(r);
            ray_enc_counters_t k; ray_enc_counters_get(&k);
            if (gen) TEST_ASSERT_FMT(k.materialized == 0, "%s materialized", q[i]);
        }
        TEST_ASSERT_STR_EQ(code[0], code[1]);
        TEST_ASSERT_FMT(strcmp(msg[0], msg[1]) == 0, "%s: '%s' vs '%s'", q[i], msg[0], msg[1]);
    }
    PASS();
}

/* All-null columns, wide integer sums (the low word wraps, avg reads the
 * exact 128-bit total) and an empty table. */
static test_result_t test_reduce_scan_edges(void) {
    sr_eval_ok("(set i (til 70000))");
    sr_eval_ok("(set E (table [n x z w] (list (as 'I32 (take [0N] 70000)) (as 'F64 (take [0N] 70000)) "
               "(* i 131762457669) (- 0 (* i 131762457669)))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/e0/\" E {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/e2/\" E {compress: true})");
    static const char* const q[] = {
        "(sum (at T 'n))", "(avg (at T 'n))", "(min (at T 'n))", "(max (at T 'n))", "(first (at T 'n))",
        "(last (at T 'n))", "(prod (at T 'n))", "(all (at T 'n))", "(any (at T 'n))", "(count (at T 'n))",
        "(sum (at T 'x))", "(avg (at T 'x))", "(min (at T 'x))", "(max (at T 'x))", "(first (at T 'x))",
        "(last (at T 'x))", "(sum (at T 'z))", "(avg (at T 'z))", "(min (at T 'z))", "(max (at T 'z))",
        "(sum (at T 'w))", "(avg (at T 'w))", "(min (at T 'w))", "(prod (at T 'z))", NULL };
    for (int i = 0; q[i]; i++) {
        test_result_t r = sr_same_on("(.db.splayed.get \"" SR_TMP "/e0/\")", "(.db.splayed.get \"" SR_TMP "/e2/\")", q[i], 0);
        if (r.status != TEST_PASS) return r;
    }
    /* the last non-null value is the first row of the last 1024-row view,
     * nulls after it */
    sr_eval_ok("(set L (table [l lf] (list (map (fn [j] (if (> j 69632) 0N j)) i) "
               "(as 'F64 (map (fn [j] (if (> j 69632) 0N j)) i)))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/l0/\" L {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/l2/\" L {compress: true})");
    static const char* const ql[] = { "(last (at T 'l))", "(last (at T 'lf))", "(first (at T 'l))", NULL };
    for (int i = 0; ql[i]; i++) {
        test_result_t r = sr_same_on("(.db.splayed.get \"" SR_TMP "/l0/\")", "(.db.splayed.get \"" SR_TMP "/l2/\")", ql[i], 0);
        if (r.status != TEST_PASS) return r;
    }
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/z0e/\" (take E 0) {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/z2e/\" (take E 0) {compress: true})");
    static const char* const qe[] = { "(sum (at T 'z))", "(avg (at T 'n))", "(min (at T 'x))", "(max (at T 'z))",
                                      "(first (at T 'n))", "(last (at T 'x))", "(count (at T 'z))", NULL };
    for (int i = 0; qe[i]; i++) {
        test_result_t r = sr_same_on("(.db.splayed.get \"" SR_TMP "/z0e/\")", "(.db.splayed.get \"" SR_TMP "/z2e/\")", qe[i], 0);
        if (r.status != TEST_PASS) return r;
    }
    PASS();
}

/* Finding C3: whole-column sum / avg / min / max of a CHUNK_ZONE column are
 * answered from the directory (generation 0 answers them from its zone):
 * nothing decoded at all. */
static test_result_t test_reduce_dir_answers(void) {
    static const char* const q[] = { "(sum (at T 'k))", "(avg (at T 'v))", "(min (at T 'd))",
                                     "(max (at T 'u))", "(avg (at T 'k))", "(sum (at T 'g))",
                                     "(min (at T 'f))", "(max (at T 'f))", "(min (at T 'b))",
                                     "(max (at T 'v))", "(count (at T 'k))", NULL };
    for (int i = 0; q[i]; i++) {
        SR_SAME(q[i], 0);
        sr_eval_ok("(set T (.db.splayed.get \"" SR_G0 "\"))");
        ray_t* r0 = sr_eval(q[i]);
        TEST_ASSERT_TRUE(r0 && !RAY_IS_ERR(r0));
        ray_t* f0 = ray_fmt(r0, 0);
        sr_eval_ok("(set T (.db.splayed.get \"" SR_G2 "\"))");
        /* twice on the same load: the second reads the cached directory totals */
        for (int rep = 0; rep < 2; rep++) {
            ray_enc_counters_reset();
            ray_t* r = sr_eval(q[i]);
            TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
            ray_enc_counters_t k; ray_enc_counters_get(&k);
            TEST_ASSERT_FMT(k.decoded_bytes == 0, "%s decoded %llu bytes", q[i], (unsigned long long)k.decoded_bytes);
            ray_t* f2 = ray_fmt(r, 0);
            TEST_ASSERT_FMT(ray_str_len(f0) == ray_str_len(f2) && memcmp(ray_str_ptr(f0), ray_str_ptr(f2), ray_str_len(f0)) == 0,
                            "%s run %d: %.*s vs %.*s", q[i], rep, (int)ray_str_len(f0), ray_str_ptr(f0), (int)ray_str_len(f2), ray_str_ptr(f2));
            ray_release(f2);
            sr_drop(r);
        }
        ray_release(f0); sr_drop(r0);
    }
    /* no directory answer: read by range, nothing materialized */
    sr_eval_ok("(set T (.db.splayed.get \"" SR_G2 "\"))");
    ray_enc_counters_reset();
    ray_t* r = sr_eval("(sum (at T 'f))");
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    TEST_ASSERT_EQ_U(k.materialized, 0);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 200000 * 8);
    sr_drop(r);
    PASS();
}

static test_result_t test_reduce_error(void) {
    char path[600];
    sr_make_bad("f", path, sizeof path);              /* F64: no directory answer, the walk reads chunk 2 */
    for (int i = 0; i < 2; i++) {
        ray_error_clear();
        ray_t* r = sr_eval("(sum (at (.db.splayed.get \"" SR_TMP "/bad/\") 'f))");
        TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
        TEST_ASSERT_TRUE(strstr(ray_error_msg(), path) != NULL);
        ray_error_free(r);
    }
    /* first / last read their row through the cursor: chunk 2 is never touched
     * by first; last reads chunk 3 then 2 only while the rows are null */
    ray_t* r = sr_eval("(first (at (.db.splayed.get \"" SR_TMP "/bad/\") 'f))");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    sr_drop(r);
    PASS();
}

/* Ruling Q1: a RAW float vector without nulls is read in place (nothing
 * decoded) once its chunk passed the float checks; a NaN in such a vector
 * is a null outside the bitmap: corrupt, on every read. */
static test_result_t test_raw_float_view(void) {
    sr_eval_ok("(set i (til 140000))");
    sr_bind_f32("yf", 140000, 0, 0.25f);
    sr_eval_ok("(set R (table [x y] (list (as 'F64 (% i 1000)) yf)))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/r0/\" R {compress: false})");
    /* P5: the RAW float view is this test's subject */
    ray_encw_test_force_float(RAY_ENC_RAW);
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/r2/\" R {compress: true})");
    ray_encw_test_force_float(-1);
    static const char* const q[] = { "(sum (at T 'x))", "(sum (at T 'y))", "(avg (at T 'y))", "(prod (at T 'x))",
                                     "(any (at T 'y))", "(first (at T 'x))", "(last (at T 'y))", NULL };
    for (int i = 0; q[i]; i++) {
        test_result_t r = sr_same_on("(.db.splayed.get \"" SR_TMP "/r0/\")", "(.db.splayed.get \"" SR_TMP "/r2/\")", q[i], 0);
        if (r.status != TEST_PASS) return r;
    }
    sr_eval_ok("(set T (.db.splayed.get \"" SR_TMP "/r2/\"))");
    for (int rep = 0; rep < 2; rep++) {
        ray_enc_counters_reset();
        ray_t* r = sr_eval("(sum (at T 'x))");
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
        TEST_ASSERT_EQ_U(k.decoded_bytes, 0);
        TEST_ASSERT_EQ_U(k.materialized, 0);
        sr_drop(r);
    }
    /* chunk 1, vector 3, element 5 of x set to a NaN */
    /* P5: the RAW float body is patched below */
    ray_encw_test_force_float(RAY_ENC_RAW);
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/rbad/\" R {compress: true})");
    ray_encw_test_force_float(-1);
    char dir[512], path[600];
    if (ray_splay_resolve_dir(ray_test_tmp(SR_TMP "/rbad/"), dir, sizeof dir) != RAY_OK) abort();
    snprintf(path, sizeof path, "%s/x", dir);
    uint64_t coff = sr_dirent_offset(path, 1);
    TEST_ASSERT_TRUE(coff != 0);
    ray_enc_desc_t de; memset(&de, 0, sizeof de);
    FILE* f = fopen(path, "r+b");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(fseek(f, (long)(coff + 3 * sizeof de), SEEK_SET) == 0 && fread(&de, sizeof de, 1, f) == 1);
    TEST_ASSERT_EQ_I(de.encoding, 0);                    /* RAW */
    TEST_ASSERT_EQ_I(de.n_nulls, 0);
    double nan = NAN;
    TEST_ASSERT_TRUE(fseek(f, (long)(coff + de.payload_off + 5 * 8), SEEK_SET) == 0 && fwrite(&nan, 8, 1, f) == 1);
    fclose(f);
    for (int rep = 0; rep < 2; rep++) {
        ray_error_clear();
        ray_t* r = sr_eval("(sum (at (.db.splayed.get \"" SR_TMP "/rbad/\") 'x))");
        TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
        TEST_ASSERT_TRUE(strstr(ray_error_msg(), path) != NULL);
        ray_error_free(r);
    }
    /* on one load: the failed chunk is never cached as checked */
    sr_eval_ok("(set B (.db.splayed.get \"" SR_TMP "/rbad/\"))");
    for (int rep = 0; rep < 2; rep++) {
        ray_t* r = sr_eval("(sum (at B 'x))");
        TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
        ray_error_free(r);
    }
    PASS();
}

/* Finding I11: (at P 'v) on a parted generation-2 table forces to the value
 * generation 0's parted_to_flat_vec gives; sum over it reduces by range. */
static test_result_t test_at_parted_value(void) {
    for (int gen = 0; gen <= 2; gen += 2)
        for (int p = 0; p < 2; p++)
            sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/ap%d/2024.01.0%d/t/\" "
                        "(table [v] (list (as 'I32 (map (fn [j] (if (== 0 (%% j 11)) 0N j)) (til 70000))))) {compress: %s})",
                        gen, p + 1, gen ? "true" : "false");
    ray_t* a = sr_eval("(at (.db.parted.get \"" SR_TMP "/ap0\" 't) 'v)");
    ray_enc_counters_reset();
    ray_t* s2 = sr_eval("(sum (at (.db.parted.get \"" SR_TMP "/ap2\" 't) 'v))");
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    ray_t* s0 = sr_eval("(sum (at (.db.parted.get \"" SR_TMP "/ap0\" 't) 'v))");
    ray_t* b = sr_eval("(at (.db.parted.get \"" SR_TMP "/ap2\" 't) 'v)");
    TEST_ASSERT_TRUE(sr_vec_eq(a, b));                 /* type, length, HAS_NULLS, payload */
    TEST_ASSERT_TRUE(s0 && s2 && !RAY_IS_ERR(s0) && !RAY_IS_ERR(s2) && s0->i64 == s2->i64);
    sr_drop(a); sr_drop(b); sr_drop(s0); sr_drop(s2);
    static const char* const q[] = { "(first (at T 'v))", "(last (at T 'v))", "(min (at T 'v))", "(max (at T 'v))",
                                     "(avg (at T 'v))", "(count (at T 'v))", "(prod (at T 'v))", "(any (at T 'v))", NULL };
    for (int i = 0; q[i]; i++) {
        test_result_t r = sr_same_on("(.db.parted.get \"" SR_TMP "/ap0\" 't)", "(.db.parted.get \"" SR_TMP "/ap2\" 't)", q[i], 0);
        if (r.status != TEST_PASS) return r;
    }
    /* ruling I5: a ragged generation-0 table's synthetic null segment reads
     * by range and answers as its explicit-null reference */
    sr_parted_xy(SR_TMP "/arg", 0, true, false);
    sr_parted_xy(SR_TMP "/aref", 0, false, true);
    sr_parted_xy(SR_TMP "/amx", 3, true, false);
    static const char* const qr[] = { "(sum (at T 'y))", "(avg (at T 'y))", "(min (at T 'y))", "(max (at T 'y))",
                                      "(first (at T 'y))", "(last (at T 'y))", "(count (at T 'y))", NULL };
    for (int i = 0; qr[i]; i++) {
        test_result_t r = sr_same_on("(.db.parted.get \"" SR_TMP "/aref\" 't)", "(.db.parted.get \"" SR_TMP "/arg\" 't)", qr[i], 0);
        if (r.status != TEST_PASS) return r;
        r = sr_same_on("(.db.parted.get \"" SR_TMP "/aref\" 't)", "(.db.parted.get \"" SR_TMP "/amx\" 't)", qr[i], 0);
        if (r.status != TEST_PASS) return r;
    }
    ray_t* fa = sr_eval("(at (.db.parted.get \"" SR_TMP "/aref\" 't) 'y)");
    ray_t* fb = sr_eval("(at (.db.parted.get \"" SR_TMP "/arg\" 't) 'y)");
    ray_t* fc = sr_eval("(at (.db.parted.get \"" SR_TMP "/amx\" 't) 'y)");
    TEST_ASSERT_TRUE(sr_vec_eq(fa, fb));
    TEST_ASSERT_TRUE(sr_vec_eq(fa, fc));
    sr_drop(fa); sr_drop(fb); sr_drop(fc);
    PASS();
}

/* ---- Task 10: the scalar aggregate route (a select of aggregates, no by:) ---- */
static test_result_t test_scalar_route(void) {
    SR_SAME("(select {s: (sum v) c: (count k) from: T})", 0);
    SR_SAME("(select {s: (sum v) c: (count k) a: (avg f) from: T where: (> g 2)})", 0);
    SR_SAME("(select {mn: (min d) mx: (max k) from: T where: (== b true)})", 0);
    SR_SAME("(select {s: (sum k) from: T where: (< k 70000)})", 0);                 /* under a selection: the generic loop */
    SR_SAME("(select {s: (sum (+ k 5)) from: T})", 0);                              /* affine plan */
    SR_SAME("(select {s: (sum (+ k g)) a: (avg (+ k g)) from: T})", 0);             /* linear: declined, fused expr */
    SR_SAME("(select {s: (sum (* f g)) from: T where: (> k 10)})", 0);              /* prod: declined, fused expr */
    SR_SAME("(select {l: (avg (strlen s)) from: T where: (> g 0)})", 0);            /* SYM strlen */
    SR_SAME("(select {s: (sum k) a: (avg k) from: T})", 0);                         /* one-base shortcut */
    /* finding C3: the one-base shortcut answers from the directory */
    sr_eval_ok("(set T (.db.splayed.get \"" SR_G2 "\"))");
    ray_enc_counters_reset();
    ray_t* r = sr_eval("(select {s: (sum k) a: (avg k) from: T})");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    ray_enc_counters_t kc; ray_enc_counters_get(&kc);
    TEST_ASSERT_EQ_U(kc.decoded_bytes, 0);
    sr_drop(r);
    PASS();
}

#include "ops/agg_engine.h"   /* agg_route_stats: the route hook */

/* The scalar route's staged shapes beyond the brief's lines. */
static test_result_t test_scalar_route_shapes(void) {
    /* finding C3 with the shortcut itself: two affine bases over one column */
    SR_SAME("(select {s: (sum (+ k 5)) a: (avg (+ k 5)) from: T})", 0);
    sr_eval_ok("(set T (.db.splayed.get \"" SR_G2 "\"))");
    ray_enc_counters_reset();
    ray_t* r = sr_eval("(select {s: (sum (+ k 5)) a: (avg (+ k 5)) from: T})");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    ray_enc_counters_t kc; ray_enc_counters_get(&kc);
    TEST_ASSERT_EQ_U(kc.decoded_bytes, 0);
    sr_drop(r);
    /* finding C3 at planning: a select of whole-column aggregates answers
     * from the directory where generation 0 answers from its CHUNK_ZONE */
    static const char* const qm = "(select {s: (sum v) c: (count k) a: (avg g) mn: (min d) mx: (max k) from: T})";
    SR_SAME(qm, 0);
    sr_eval_ok("(set T (.db.splayed.get \"" SR_G2 "\"))");
    ray_enc_counters_reset();
    r = sr_eval(qm);
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    ray_enc_counters_get(&kc);
    TEST_ASSERT_EQ_U(kc.decoded_bytes, 0);
    sr_drop(r);
    /* a nullable column's avg / min / max at planning */
    static const char* const qn = "(select {a: (avg v) mn: (min v) mx: (max v) c: (count v) from: T})";
    SR_SAME(qn, 0);
    sr_eval_ok("(set T (.db.splayed.get \"" SR_G2 "\"))");
    ray_enc_counters_reset();
    r = sr_eval(qn);
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    ray_enc_counters_get(&kc);
    TEST_ASSERT_EQ_U(kc.decoded_bytes, 0);
    sr_drop(r);
    /* float extremes are not answered there (the zone declines them) */
    SR_SAME("(select {mn: (min f) mx: (max e) from: T})", 0);
    /* SYM extremes in the domain's order (never by raw ids), widths per piece */
    SR_SAME("(select {mn: (min s) mx: (max s) fs: (first s) ls: (last s) from: T where: (> g 0)})", 0);
    /* serial FIRST / LAST, PROD, the second moments, BOOL / U8 inputs */
    SR_SAME("(select {f: (first v) l: (last f) p: (prod g) from: T where: (> k 199990)})", 0);
    SR_SAME("(select {vr: (var g) sd: (stddev v) al: (all b) an: (any u) from: T where: (> g 0)})", 0);
    SR_SAME("(select {mn: (min e) mx: (max e) s: (sum e) from: T where: (> g 3)})", 0);   /* F32 */
    SR_SAME("(select {mn: (min d) mx: (max d) a: (avg d) from: T where: (> g 3)})", 0);   /* DATE */
    /* a holistic input reads its plain column (R-RANDOM): only k */
    SR_SAME("(select {m: (med k) s: (sum v) from: T where: (> g 0)})", 1);
    PASS();
}

/* The tight F64 kernel sums by explicit lanes (row mod 8, combined in one
 * fixed order) on both generations: a serial run (fewer rows than the
 * parallel threshold: one task, a deterministic order) over non-integral
 * values (whose sum depends on the order: a plain row-order sum differs)
 * gives generation 0's bits from zero-copy pieces, and exactly the lane
 * reference computed here.  The kernel pins its lanes on every target a
 * GCC-compatible compiler builds (group.c, SC_F64_PIN), so this holds
 * under the release build's -fassociative-math too. */
static test_result_t test_scalar_route_f64_bits(void) {
    const int64_t n = 60000;
    sr_eval_ok("(set i (til 60000))");
    sr_eval_ok("(set R (table [x] (list (* (as 'F64 (% (* i 7919) 1000003)) 0.37))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/sf0/\" R {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/sf2/\" R {compress: true})");
    volatile double lane[8] = { 0 };
    for (int64_t r = 0; r < n; r++) lane[r & 7] += (double)((r * 7919) % 1000003) * 0.37;
    volatile double ab = (lane[0] + lane[1]), cd = (lane[2] + lane[3]);
    volatile double ef = (lane[4] + lane[5]), gh = (lane[6] + lane[7]);
    volatile double lo = ab + cd, hi = ef + gh;
    double ref = lo + hi;
    static const char* const q[] = { "(select {s: (sum x) from: T})",
                                     "(select {s: (sum (+ x 0.5)) from: T})",
                                     "(select {a: (avg (+ x 0.25)) from: T})", NULL };
    for (int i = 0; q[i]; i++) {
        double v[2];
        for (int gen = 0; gen < 2; gen++) {
            sr_evalf_ok("(set T (.db.splayed.get \"%s\"))", gen ? SR_TMP "/sf2/" : SR_TMP "/sf0/");
            ray_enc_counters_reset();
            char buf[256];
            snprintf(buf, sizeof buf, "(first (first (value %s)))", q[i]);
            ray_t* r = sr_eval(buf);
            TEST_ASSERT_FMT(r && !RAY_IS_ERR(r) && r->type == -RAY_F64, "%s: gen %d", q[i], gen);
            v[gen] = r->f64;
            sr_drop(r);
            ray_enc_counters_t k; ray_enc_counters_get(&k);
            if (gen) TEST_ASSERT_EQ_U(k.materialized, 0);
        }
        TEST_ASSERT_FMT(memcmp(&v[0], &v[1], sizeof v[0]) == 0, "%s: %.17g vs %.17g", q[i], v[0], v[1]);
        if (i == 0) TEST_ASSERT_FMT(memcmp(&v[0], &ref, sizeof ref) == 0, "lanes: %.17g vs reference %.17g", v[0], ref);
    }
    PASS();
}

/* Fix round 2: the lanes' combine order is the fixed tree, never a fold:
 * x[0] = 2^53 and x[1..7] = 1.0 (every other row 0) sum to exactly
 * 2^53 + 6 (lane 0's 2^53 + 1 rounds to 2^53, the other pairs are exact);
 * a left fold gives 2^53.  Both generations, serially. */
static test_result_t test_scalar_route_f64_combine(void) {
    const int64_t n = 60000;
    ray_t* xv = ray_vec_new(RAY_F64, n); xv->len = n;
    double* x = (double*)ray_data(xv);
    for (int64_t r = 0; r < n; r++) x[r] = r == 0 ? 9007199254740992.0 : r < 8 ? 1.0 : 0.0;
    ray_env_set(ray_sym_intern("cxv", 3), xv); ray_release(xv);
    sr_eval_ok("(set CX (table [x] (list cxv)))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/cx0/\" CX {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/cx2/\" CX {compress: true})");
    for (int gen = 0; gen < 2; gen++) {
        sr_evalf_ok("(set T (.db.splayed.get \"%s\"))", gen ? SR_TMP "/cx2/" : SR_TMP "/cx0/");
        ray_t* r = sr_eval("(first (first (value (select {s: (sum x) from: T}))))");
        TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->type == -RAY_F64);
        TEST_ASSERT_FMT(r->f64 == 9007199254740998.0, "gen %d: %.17g", gen, r->f64);
        sr_drop(r);
    }
    PASS();
}

/* Fix round 2: a piece starting at a row not a multiple of 8 realigns
 * through the head loop; the cut sum equals the whole block's and the lane
 * reference, on lane-weighted data where any lane mix-up changes the bits.
 * (The engine's tasks and pieces start 1024-aligned: the hook drives the
 * staged piece function directly.) */
static test_result_t test_scalar_route_f64_unaligned(void) {
    const int64_t n = 5000;
    static const double m[8] = { -11, 11, -1, 1, 11, 1, -13, 1 };
    static double d[5000];
    static const int64_t row0s[] = { 0, 3, 1021 };
    static const int64_t cuts[] = { 5, 13, 1000, 1027, 2049, 4099 };
    for (size_t k = 0; k < sizeof row0s / sizeof row0s[0]; k++) {
        int64_t row0 = row0s[k];
        volatile double lane[8] = { 0 };
        for (int64_t r = 0; r < n; r++) {
            d[r] = 0.37 * (double)(((r + row0) * 7919) % 1000003) * m[(r + row0) & 7];
            lane[(r + row0) & 7] += d[r];
        }
        volatile double ab = lane[0] + lane[1], cd = lane[2] + lane[3];
        volatile double ef = lane[4] + lane[5], gh = lane[6] + lane[7];
        volatile double lo = ab + cd, hi = ef + gh;
        double ref = lo + hi, whole, cut;
        ray_group_test_f64_lanes(d, row0, n, cuts, (int)(sizeof cuts / sizeof cuts[0]), &whole, &cut);
        TEST_ASSERT_FMT(memcmp(&whole, &ref, sizeof ref) == 0, "row0 %lld: whole %.17g vs ref %.17g",
                        (long long)row0, whole, ref);
        TEST_ASSERT_FMT(memcmp(&cut, &ref, sizeof ref) == 0, "row0 %lld: cut %.17g vs ref %.17g",
                        (long long)row0, cut, ref);
    }
    PASS();
}

/* R-FAIL: a corrupt chunk the scalar route reads is the query's error, on
 * every run; morsels the selection rejects whole are not read. */
static test_result_t test_scalar_route_error(void) {
    char path[600];
    static const struct { const char* col; const char* q; } cs[] = {
        { "f", "(select {s: (sum f) c: (count k) from: B where: (> g 0)})" },   /* generic loop */
        /* a whole-column I64 sum answers from the directory and never reads
         * the damaged chunk (the accepted residual, as for a zone); the chunk
         * route reads it by rows under a selection that keeps some of it */
        { "k", "(select {s: (sum (+ k 1)) m: (min k) from: B where: (> g 0)})" },
        { "k", "(select {s: (sum (+ k 1)) a: (avg (+ k 1)) from: B where: (> g 0)})" },
    };
    for (size_t c = 0; c < sizeof cs / sizeof cs[0]; c++) {
        if (c == 0 || strcmp(cs[c].col, cs[c - 1].col) != 0) sr_make_bad(cs[c].col, path, sizeof path);
        for (int i = 0; i < 2; i++) {
            sr_eval_ok("(set B (.db.splayed.get \"" SR_TMP "/bad/\"))");
            ray_error_clear();
            ray_t* r = sr_eval(cs[c].q);
            TEST_ASSERT_FMT(r && RAY_IS_ERR(r), "%s: no error", cs[c].q);
            TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
            TEST_ASSERT_TRUE(strstr(ray_error_msg(), path) != NULL);
            ray_error_free(r);
        }
    }
    /* chunk 2 (rows 131072..196607) of k is corrupt; a selection that ends
     * before it never reads it */
    test_result_t t = sr_same_on("(.db.splayed.get \"" SR_G0 "\")", "(.db.splayed.get \"" SR_TMP "/bad/\")",
                                 "(select {s: (sum (+ k 1)) m: (max k) from: T where: (< k 100000)})", 0);
    if (t.status != TEST_PASS) return t;
    /* the tight i64 kernel (scalar_sum_i64_fn): a staged TIMESTAMP avg, no
     * statistics rule (TIMESTAMP avg decodes), no selection, no nulls */
    sr_eval_ok("(set S (table [k p] (list i (as 'TIMESTAMP (* 1000 i)))))");
    sr_make_bad("p", path, sizeof path);
    for (int i = 0; i < 2; i++) {
        sr_eval_ok("(set B (.db.splayed.get \"" SR_TMP "/bad/\"))");
        ray_error_clear();
        ray_t* r = sr_eval("(select {a: (avg p) from: B})");
        TEST_ASSERT_FMT(r && RAY_IS_ERR(r), "timestamp avg: no error");
        TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
        TEST_ASSERT_TRUE(strstr(ray_error_msg(), path) != NULL);
        ray_error_free(r);
    }
    PASS();
}

/* The route hook: 1 takes the legacy ladder instead of the v2 engine; the
 * answers are the same; ray_test_hooks_off disarms it. */
static test_result_t test_group_route_hook(void) {
    sr_eval_ok("(set T (.db.splayed.get \"" SR_G0 "\"))");
    const char* q = "(select {s: (sum v) c: (count k) from: T by: g})";
    ray_t* out[4];
    agg_route_stats_t st[4];
    for (int route = 0; route < 4; route++) {
        ray_group_test_route(route);
        agg_route_reset();
        out[route] = sr_eval(q);
        st[route] = agg_route_stats();
        TEST_ASSERT_TRUE(out[route] && !RAY_IS_ERR(out[route]));
    }
    ray_test_hooks_off();
    TEST_ASSERT_EQ_U(st[0].routes[AGG_ROUTE_LEGACY], 0);
    for (int route = 1; route < 4; route++) {
        TEST_ASSERT_TRUE(st[route].routes[AGG_ROUTE_LEGACY] > 0);
        ray_t* fa = ray_fmt(out[0], 0); ray_t* fb = ray_fmt(out[route], 0);
        TEST_ASSERT_TRUE(ray_str_len(fa) == ray_str_len(fb) &&
                         memcmp(ray_str_ptr(fa), ray_str_ptr(fb), ray_str_len(fa)) == 0);
        ray_release(fa); ray_release(fb);
    }
    agg_route_reset();
    ray_t* r = sr_eval(q);
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    TEST_ASSERT_EQ_U(agg_route_stats().routes[AGG_ROUTE_LEGACY], 0);   /* disarmed */
    sr_drop(r);
    for (int route = 0; route < 4; route++) sr_drop(out[route]);
    /* 2 skips the direct array: over generation 2 both the direct array
     * and the legacy radix route read by range (no materialization) */
    uint64_t mat[3];
    for (int route = 1; route <= 2; route++) {
        sr_eval_ok("(set T (.db.splayed.get \"" SR_G2 "\"))");
        ray_group_test_route(route);
        ray_enc_counters_reset();
        r = sr_eval(q);
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        ray_group_test_route(0);
        TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
        sr_drop(r);
        mat[route] = k.materialized;
    }
    TEST_ASSERT_EQ_U(mat[1], 0);
    TEST_ASSERT_EQ_U(mat[2], 0);
    /* 3 skips the slice-group route: the armed hint folds into a selection */
    sr_eval_ok("(set KS (as 'SYM (at ['aa 'bb 'cc 'dd 'ee 'ff 'gg 'hh] (% (til 4000) 8))))");
    sr_eval_ok("(set TI (table [s f] (list (.attr.set 'grouped KS) (as 'F64 (% (til 4000) 64)))))");
    const char* qs = "(select {sv: (sum f) from: TI by: s asc: s where: (in s ['aa 'cc])})";
    ray_t* so[2];
    uint64_t sl[2];
    for (int i = 0; i < 2; i++) {
        ray_group_test_route(i ? 3 : 0);
        agg_route_reset();
        so[i] = sr_eval(qs);
        sl[i] = agg_route_stats().routes[AGG_ROUTE_SLICES];
        ray_group_test_route(0);
        TEST_ASSERT_TRUE(so[i] && !RAY_IS_ERR(so[i]) && ray_table_nrows(so[i]) == 2);
    }
    TEST_ASSERT_TRUE(sl[0] > 0);
    TEST_ASSERT_EQ_U(sl[1], 0);
    ray_t* fa = ray_fmt(so[0], 0); ray_t* fb = ray_fmt(so[1], 0);
    TEST_ASSERT_TRUE(ray_str_len(fa) == ray_str_len(fb) &&
                     memcmp(ray_str_ptr(fa), ray_str_ptr(fb), ray_str_len(fa)) == 0);
    ray_release(fa); ray_release(fb); sr_drop(so[0]); sr_drop(so[1]);
    PASS();
}

/* Fix round 1 (I1): a SYM input wider than W8 (3000 symbols: W16) is read
 * at each piece's width, with and without a selection; an encoded STR
 * strlen input is declined to the plain path (it materializes). */
static test_result_t test_scalar_route_wide_sym(void) {
    const int64_t n = 200000;
    ray_t* sv = ray_sym_vec_new(RAY_SYM_W64, n); sv->len = n;
    ray_t* tv = ray_vec_new(RAY_STR, n); tv->len = 0;
    for (int64_t i = 0; i < n; i++) {
        char b[24]; int nb = snprintf(b, sizeof b, "sym%lld", (long long)((i * 7) % 3000));
        ((int64_t*)ray_data(sv))[i] = ray_sym_intern(b, (size_t)nb);
        tv = ray_str_vec_append(tv, b, (size_t)nb);
    }
    ray_env_set(ray_sym_intern("wsv", 3), sv); ray_release(sv);
    ray_env_set(ray_sym_intern("wtv", 3), tv); ray_release(tv);
    sr_eval_ok("(set WS (table [s t g] (list wsv wtv (as 'I16 (% (til 200000) 7)))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/ws0/\" WS {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/ws2/\" WS {compress: true})");
    const char* t0 = "(.db.splayed.get \"" SR_TMP "/ws0/\")";
    const char* t2 = "(.db.splayed.get \"" SR_TMP "/ws2/\")";
    sr_evalf_ok("(set T %s)", t2);
    ray_t* T = sr_eval("T");
    ray_col_meta_t m;
    TEST_ASSERT_TRUE(T && ray_table_col_meta(T, ray_sym_intern("s", 1), &m) && m.enc &&
                     (m.attrs & RAY_SYM_W_MASK) == RAY_SYM_W16);
    sr_drop(T);
    static const struct { const char* q; uint64_t mat; } qs[] = {
        { "(select {mn: (min s) mx: (max s) l: (sum (strlen s)) from: T})", 0 },
        { "(select {mn: (min s) mx: (max s) l: (avg (strlen s)) f: (first s) from: T where: (> g 2)})", 0 },
        { "(select {l: (sum (strlen s)) from: T where: (> g 0)})", 0 },
        { "(select {l: (sum (strlen t)) c: (count g) from: T where: (> g 2)})", 1 },   /* STR: declined, t materializes */
    };
    for (size_t i = 0; i < sizeof qs / sizeof qs[0]; i++) {
        test_result_t r = sr_same_on(t0, t2, qs[i].q, qs[i].mat);
        if (r.status != TEST_PASS) return r;
    }
    PASS();
}

/* Fix round 1 (I2): plain inputs bound beside staged ones in one context:
 * an expression input, and a column an earlier query materialized. */
static test_result_t test_scalar_route_mixed(void) {
    SR_SAME("(select {s: (sum (+ k g)) m: (max v) from: T where: (> g 0)})", 0);
    SR_SAME("(select {s: (sum (* f g)) m: (min d) a: (avg v) from: T where: (> k 10)})", 0);
    SR_SAME("(select {s: (sum (+ k g)) a: (avg (* f 2.0)) l: (last v) from: T})", 0);
    /* k materialized before the query: read in place beside staged v */
    sr_eval_ok("(set T2M (.db.splayed.get \"" SR_G2 "\"))");
    ray_t* t = sr_eval("T2M");
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_t* kcol = ray_table_col(t, ray_sym_intern("k", 1));
    TEST_ASSERT_TRUE(kcol && !RAY_IS_ERR(kcol));
    sr_drop(t);
    static const char* const q[] = {   /* not sum / avg of k: a ready bare k would be a linear plan */
        "(select {mx: (max k) s: (sum v) a: (avg g) from: T where: (> g 0)})",
        "(select {mn: (min k) l: (last k) m: (max v) from: T where: (> g 2)})",
        NULL };
    for (int i = 0; q[i]; i++) {
        test_result_t r = sr_same_on("(.db.splayed.get \"" SR_G0 "\")", "T2M", q[i], 0);
        if (r.status != TEST_PASS) return r;
    }
    PASS();
}

/* ---- Task 11: the direct-array group route and its key prescan ---- */
static test_result_t test_da_route(void) {
    ray_group_test_route(1);                       /* skip v2: the legacy ladder chooses DA for a small key range */
    SR_SAME("(select {s: (sum v) c: (count k) from: T by: g})", 0);
    SR_SAME("(select {s: (sum f) mx: (max d) from: T where: (> k 1000) by: g})", 0);
    SR_SAME("(select {c: (count k) a: (avg v) from: T by: {g: g b: b}})", 0);   /* composite gid */
    SR_SAME("(select {c: (count k) from: T by: u})", 0);                          /* U8 key, 200 groups */
    ray_group_test_route(0);
    PASS();
}

/* The direct-array route's shapes beyond the brief's lines. */
static test_result_t test_da_route_shapes(void) {
    ray_group_test_route(1);
    /* a SYM key: slots over the domain's ids (never ordered by raw id) */
    SR_SAME("(select {c: (count k) s: (sum v) from: T by: s})", 0);
    /* SYM inputs (domain order, widths per piece), strlen, FIRST / LAST */
    SR_SAME("(select {mn: (min s) mx: (max s) l: (avg (strlen s)) f: (first v) z: (last f) from: T by: g})", 0);
    /* PROD, second moments, ALL / ANY, F32 / DATE / BOOL / U8 inputs */
    SR_SAME("(select {p: (prod b) vr: (var v) sd: (stddev f) al: (all b) an: (any u) from: T where: (> g 0) by: u})", 0);
    SR_SAME("(select {mn: (min e) mx: (max d) s: (sum e) a: (avg u) from: T by: {b: b g: g}})", 0);
    /* a selection whose directory superset range does not fit the slot
     * budget: the exact range by a scan, the route generation 0 takes */
    SR_SAME("(select {c: (count k) s: (sum v) from: T where: (< h 262000) by: h})", 0);
    /* a computed key beside a staged one (the key expression reads k plain) */
    SR_SAME("(select {c: (count k) s: (sum v) from: T by: {g: g m: (% k 3)}})", 1);
    /* the directory prescan decodes no key: u is read once, by the
     * accumulation (COUNT reads no input) */
    sr_eval_ok("(set T (.db.splayed.get \"" SR_G2 "\"))");
    ray_enc_counters_reset();
    ray_t* r = sr_eval("(select {c: (count k) from: T by: u})");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    ray_enc_counters_t kc; ray_enc_counters_get(&kc);
    TEST_ASSERT_EQ_U(kc.decoded_bytes, 200000);
    sr_drop(r);
    /* binary aggregates: both inputs staged, the y-side read by pieces */
    SR_SAME("(select {p: (pearson_corr f k) c: (cov k f) w: (wavg k f) ws: (wsum f h) from: T by: g})", 0);
    SR_SAME("(select {p: (pearson_corr k h) w: (wavg g f) from: T where: (> k 1000) by: {g: g b: b}})", 0);
    /* strlen of symbols of lengths 1-3 (a piece read at the wrong row
     * gives a different length) and binary aggregates over a second table */
    sr_eval_ok("(set Y (table [g w x y] (list (as 'I16 (% i 7)) (as 'SYM (as 'STR (% (* i 7) 400))) "
               "(* (as 'F64 i) 0.5) (% (* i 7919) 1003))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/y0/\" Y {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/y2/\" Y {compress: true})");
    static const char* const qy[] = {
        "(select {l: (avg (strlen w)) s: (sum (strlen w)) from: T by: g})",
        "(select {l: (sum (strlen w)) from: T where: (> y 10) by: g})",
        /* strlen beside min / max: the accumulation's general arm (a
         * strlen under min / max is an expression, not a strlen input) */
        "(select {l: (sum (strlen w)) a: (avg (strlen w)) m: (min y) x: (max x) from: T by: g})",
        "(select {p: (pearson_corr x y) c: (cov y x) w: (wavg y x) ws: (wsum x y) from: T by: g})",
        NULL };
    for (int q = 0; qy[q]; q++) {
        test_result_t t = sr_same_on("(.db.splayed.get \"" SR_TMP "/y0/\")", "(.db.splayed.get \"" SR_TMP "/y2/\")",
                                     qy[q], 0);
        if (t.status != TEST_PASS) { ray_group_test_route(0); return t; }
    }
    ray_group_test_route(0);
    PASS();
}

/* R-FAIL: a corrupt chunk of a key or an input the route reads is the
 * query's error, on every run. */
static test_result_t test_da_route_error(void) {
    char path[600];
    static const struct { const char* col; const char* q; } cs[] = {
        { "g", "(select {s: (sum v) c: (count k) from: B by: g})" },          /* key, accumulation */
        { "v", "(select {s: (sum v) c: (count k) from: B by: g})" },          /* input */
        { "v", "(select {s: (sum v) from: B where: (> k 5) by: {g: g b: b}})" },
    };
    ray_group_test_route(1);
    for (size_t c = 0; c < sizeof cs / sizeof cs[0]; c++) {
        if (c == 0 || strcmp(cs[c].col, cs[c - 1].col) != 0) sr_make_bad(cs[c].col, path, sizeof path);
        for (int i = 0; i < 2; i++) {
            sr_eval_ok("(set B (.db.splayed.get \"" SR_TMP "/bad/\"))");
            ray_error_clear();
            ray_t* r = sr_eval(cs[c].q);
            TEST_ASSERT_FMT(r && RAY_IS_ERR(r), "%s: no error", cs[c].q);
            TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
            TEST_ASSERT_TRUE(strstr(ray_error_msg(), path) != NULL);
            ray_error_free(r);
        }
    }
    /* chunk 2 (rows 131072..196607) of v is corrupt; a selection that ends
     * before it never reads it */
    test_result_t t = sr_same_on("(.db.splayed.get \"" SR_G0 "\")", "(.db.splayed.get \"" SR_TMP "/bad/\")",
                                 "(select {s: (sum v) c: (count k) from: T where: (< k 100000) by: g})", 0);
    ray_group_test_route(0);
    if (t.status != TEST_PASS) return t;
    PASS();
}

/* Partitioned generation-2 tables (exec_group_parted gives each partition's
 * segments to the route), and a ragged generation-0 table whose missing
 * column is a synthetic null segment, answer as their references.  Answers
 * only: the parted dispatch's own segment reads are Task 14's. */
static test_result_t test_da_route_parted(void) {
    for (int gen = 0; gen <= 2; gen += 2)
        for (int p = 0; p < 3; p++)
            sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/dp%d/2024.01.0%d/t/\" "
                        "(table [g v] (list (as 'I16 (%% (til 70000) 7)) (as 'I32 (map (fn [j] (if (== 0 (%% j 11)) 0N j)) (til 70000))))) "
                        "{compress: %s})", gen, p + 1, gen ? "true" : "false");
    for (int p = 0; p < 3; p++) {
        const char* tbl = p == 2 ? "(table [g] (list (as 'I16 (% (til 70000) 7))))"
                                 : "(table [g v] (list (as 'I16 (% (til 70000) 7)) (til 70000)))";
        const char* ref = p == 2 ? "(table [g v] (list (as 'I16 (% (til 70000) 7)) (take [0Nl] 70000)))" : tbl;
        sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/drg/2024.01.0%d/t/\" %s {compress: false})", p + 1, tbl);
        sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/drf/2024.01.0%d/t/\" %s {compress: false})", p + 1, ref);
    }
    ray_group_test_route(1);
    static const char* const q[] = { "(select {s: (sum v) c: (count v) mx: (max v) from: T by: g})",
                                     "(select {a: (avg v) f: (first v) from: T where: (> v 100) by: g})", NULL };
    for (int i = 0; q[i]; i++) {
        test_result_t r = sr_same_on("(.db.parted.get \"" SR_TMP "/dp0\" 't)", "(.db.parted.get \"" SR_TMP "/dp2\" 't)", q[i], UINT64_MAX);
        if (r.status == TEST_PASS)
            r = sr_same_on("(.db.parted.get \"" SR_TMP "/drf\" 't)", "(.db.parted.get \"" SR_TMP "/drg\" 't)", q[i], UINT64_MAX);
        if (r.status != TEST_PASS) { ray_group_test_route(0); return r; }
    }
    ray_group_test_route(0);
    PASS();
}

/* The sparse-dynamic count route (a top-N by count over one key, reached
 * when the direct array declines): staged keys and inputs by range. */
static test_result_t test_sp_dyn_route(void) {
    static const char* const q2[] = {
        "(select {c: (count k) from: T by: s desc: c take: 3})",                     /* SYM, count only */
        "(select {c: (count k) s: (sum k) a: (avg g) from: T by: u desc: c take: 3})", /* sums, first pass */
        "(select {c: (count k) s: (sum k) from: T by: s desc: c take: 3})",          /* SYM: the sum pass */
        "(select {c: (count k) s: (sum h) from: T where: (> g 0) by: s desc: c take: 2})",
        NULL };
    ray_group_test_route(2);                       /* skip v2 and the direct array */
    for (int i = 0; q2[i]; i++) SR_SAME(q2[i], 0);
    /* binary aggregates never take the sparse-dynamic count (SUM / AVG /
     * COUNT only): the hash route, on plain columns, answers as generation 0 */
    SR_SAME("(select {c: (count k) p: (pearson_corr f k) w: (wavg k f) from: T by: s desc: c take: 3})", UINT64_MAX);
    ray_group_test_route(1);
    SR_SAME("(select {c: (count k) from: T by: h desc: c take: 3})", 0);   /* the DA range does not fit */
    /* the parallel fill (262144 rows or more) walks the key */
    sr_eval_ok("(set W (table [s x] (list (take ['a 'b 'c 'd 'e 'f 'g] 300000) (til 300000))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/w0/\" W {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/w2/\" W {compress: true})");
    ray_group_test_route(2);
    test_result_t t = sr_same_on("(.db.splayed.get \"" SR_TMP "/w0/\")", "(.db.splayed.get \"" SR_TMP "/w2/\")",
                                 "(select {c: (count x) from: T by: s desc: c take: 3})", 0);
    if (t.status != TEST_PASS) { ray_group_test_route(0); return t; }
    /* R-FAIL on both passes and the parallel fill */
    char path[600];
    static const struct { const char* col; const char* q; } cs[] = {
        { "s", "(select {c: (count k) from: B by: s desc: c take: 3})" },
        { "s", "(select {c: (count k) s: (sum k) from: B by: s desc: c take: 3})" },
        { "k", "(select {c: (count k) s: (sum k) from: B by: s desc: c take: 3})" },
        { "k", "(select {c: (count k) s: (sum k) from: B by: u desc: c take: 3})" },
    };
    for (size_t c = 0; c < sizeof cs / sizeof cs[0]; c++) {
        if (c == 0 || strcmp(cs[c].col, cs[c - 1].col) != 0) sr_make_bad(cs[c].col, path, sizeof path);
        for (int i = 0; i < 2; i++) {
            sr_eval_ok("(set B (.db.splayed.get \"" SR_TMP "/bad/\"))");
            ray_error_clear();
            ray_t* r = sr_eval(cs[c].q);
            TEST_ASSERT_FMT(r && RAY_IS_ERR(r), "%s: no error", cs[c].q);
            TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
            TEST_ASSERT_TRUE(strstr(ray_error_msg(), path) != NULL);
            ray_error_free(r);
        }
    }
    /* the parallel fill's range error */
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/wbad/\" W {compress: true})");
    char dir[512];
    TEST_ASSERT_TRUE(ray_splay_resolve_dir(ray_test_tmp(SR_TMP "/wbad/"), dir, sizeof dir) == RAY_OK);
    snprintf(path, sizeof path, "%s/s", dir);
    uint64_t off = sr_dirent_offset(path, 2);
    TEST_ASSERT_TRUE(off != 0);
    sr_patch(path, (long)off + 6, 1);
    for (int i = 0; i < 2; i++) {
        sr_eval_ok("(set B (.db.splayed.get \"" SR_TMP "/wbad/\"))");
        ray_error_clear();
        ray_t* r = sr_eval("(select {c: (count x) from: B by: s desc: c take: 3})");
        TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
        TEST_ASSERT_TRUE(strstr(ray_error_msg(), path) != NULL);
        ray_error_free(r);
    }
    ray_group_test_route(0);
    PASS();
}

/* ---- Task 12: the legacy radix group route ---- */
static test_result_t test_radix_route(void) {
    ray_group_test_route(2);                       /* skip v2 and DA: legacy radix */
    SR_SAME("(select {c: (count k) s: (sum v) from: T by: k})", 0);                 /* 200000 groups */
    SR_SAME("(select {c: (count k) from: T where: (> v 1000) by: {k: k s: s}})", 0);  /* SYM in a composite key */
    SR_SAME("(select {a: (avg f) from: T by: d})", 0);
    SR_SAME("(select {m: (med v) from: T by: g})", 1);                              /* holistic input materializes */
    ray_group_test_route(0);
    PASS();
}

/* The legacy radix route's shapes beyond the brief's lines: the v2
 * sibling (null-free inputs, no SYM key) with a survivor list (R-SPARSE)
 * and a rowsel, every streaming op, binary aggregates, nullable keys, a
 * holistic input beside staged ones, wide keys, and a pre-materialized
 * column read in place. */
static test_result_t test_radix_route_shapes(void) {
    static const struct { const char* q; uint64_t mat; } qs[] = {
        /* v2: whole scan, survivor list (< half the rows), rowsel */
        { "(select {c: (count k) s: (sum k) a: (avg g) from: T by: {g: g u: u}})", 0 },
        { "(select {c: (count k) s: (sum h) from: T where: (> k 140000) by: {g: g u: u}})", 0 },
        { "(select {s: (sum h) a: (avg u) from: T where: (> k 50000) by: {d: d b: b}})", 0 },
        /* phase 1: a SYM key under a rowsel, every streaming op, strlen */
        { "(select {s: (sum v) mx: (max f) mn: (min d) from: T where: (> g 2) by: s})", 0 },
        { "(select {f: (first v) l: (last f) vr: (var v) p: (prod b) mn: (min s) mx: (max s) "
          "n: (sum (strlen s)) e: (avg e) sd: (stddev g) from: T by: {g: g b: b}})", 0 },
        { "(select {p: (pearson_corr f k) w: (wavg k f) c: (cov h g) from: T by: {g: g u: u}})", 0 },
        /* nullable keys: I32, F64, and in a composite */
        { "(select {c: (count k) s: (sum h) from: T by: v})", 0 },
        { "(select {c: (count k) from: T where: (> k 100) by: f})", 0 },
        { "(select {c: (count k) a: (avg e) from: T by: {v: v g: g}})", 0 },
        /* holistic: exactly its input materializes, the others are read by range */
        { "(select {m: (med v) s: (sum f) c: (count k) from: T by: g})", 1 },
        { "(select {m: (med v) a: (avg h) from: T where: (> k 1000) by: {g: g b: b}})", 1 },
        { "(select {q: (med f) x: (med v) from: T by: s})", 2 },
    };
    ray_group_test_route(2);
    for (size_t i = 0; i < sizeof qs / sizeof qs[0]; i++) {
        test_result_t r = sr_same(qs[i].q, qs[i].mat);
        if (r.status != TEST_PASS) { ray_group_test_route(0); return r; }
    }
    /* a STR key: grouped on its query-wide string-domain ids (strdom.c), so
     * no key materializes and the inputs are read by range; only the
     * holistic input (med) materializes */
    sr_eval_ok("(set Z (table [t g v f] (list (as 'STR (% i 50)) (as 'I16 (% i 7)) "
               "(as 'I32 (map (fn [j] (if (== 0 (% j 10)) 0N j)) i)) (as 'F64 i))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/z0/\" Z {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/z2/\" Z {compress: true})");
    static const struct { const char* q; uint64_t mat; } qz[] = {
        { "(select {s: (sum v) c: (count g) a: (avg f) from: T by: t})", 0 },
        { "(select {s: (sum v) m: (max f) from: T where: (> g 1) by: {t: t g: g}})", 0 },
        { "(select {m: (med f) s: (sum v) from: T by: t})", 1 },
    };
    for (size_t i = 0; i < sizeof qz / sizeof qz[0]; i++) {
        test_result_t r = sr_same_on("(.db.splayed.get \"" SR_TMP "/z0/\")", "(.db.splayed.get \"" SR_TMP "/z2/\")",
                                     qz[i].q, qz[i].mat);
        if (r.status != TEST_PASS) { ray_group_test_route(0); return r; }
    }
    /* k materialized before the query: read in place beside staged inputs */
    sr_eval_ok("(set T2M (.db.splayed.get \"" SR_G2 "\"))");
    ray_t* t = sr_eval("T2M");
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_t* kcol = ray_table_col(t, ray_sym_intern("k", 1));
    TEST_ASSERT_TRUE(kcol && !RAY_IS_ERR(kcol));
    sr_drop(t);
    /* h materialized too: the v2 survivor list (R-SPARSE) reads a plain
     * input in place at each run's first row */
    t = sr_eval("T2M");
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_t* hcol = ray_table_col(t, ray_sym_intern("h", 1));
    TEST_ASSERT_TRUE(hcol && !RAY_IS_ERR(hcol));
    sr_drop(t);
    static const char* const q2m[] = {
        "(select {c: (count v) s: (sum v) from: T by: k})",
        "(select {c: (count v) s: (sum f) from: T by: {k: k g: g}})",
        "(select {c: (count k) s: (sum h) from: T where: (> k 140000) by: {g: g u: u}})",
        NULL };
    for (int i = 0; q2m[i]; i++) {
        test_result_t r = sr_same_on("(.db.splayed.get \"" SR_G0 "\")", "T2M", q2m[i], 0);
        if (r.status != TEST_PASS) { ray_group_test_route(0); return r; }
    }
    /* a non-inline wide key (GUID) beside a fixed one: the keys stay plain
     * and unwalked, the inputs are read by range */
    sr_eval_ok("(set Q (table [q g v] (list (take (guid 37) 200000) (as 'I16 (% i 7)) i)))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/q0/\" Q {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/q2/\" Q {compress: true})");
    test_result_t rq = sr_same_on("(.db.splayed.get \"" SR_TMP "/q0/\")", "(.db.splayed.get \"" SR_TMP "/q2/\")",
                                  "(select {s: (sum v) c: (count v) from: T by: {q: q g: g}})", 2);
    ray_group_test_route(0);
    if (rq.status != TEST_PASS) return rq;
    PASS();
}

/* Fix round 1 (I3): the single-key sparse paths of the legacy route (the
 * emit-filter key-range scatter, the sparse i64 table and its heavy-hitter
 * sum pass) read staged keys and inputs by pieces, and their fallback to
 * the fat-entry routes takes them staged: nothing materializes. */
static test_result_t test_radix_route_sparse(void) {
    static const char* const qs[] = {
        /* the sparse i64 table, sum arm */
        "(select {c: (count k) s: (sum h) from: T by: k})",
        "(select {s: (sum h) a: (avg k) from: T by: h})",
        "(select {s: (sum h) from: T by: u})",
        "(select {c: (count k) s: (sum k) from: T where: (> k 100) by: h})",
        "(select {c: (count k) s: (sum h) a: (avg u) from: T by: s})",              /* SYM key, by piece width */
        /* the emit-filter key range under a sparse selection: bounds by a scan */
        "(select {c: (count k) s: (sum h) from: T where: (< k 1000) by: h desc: c take: 3})",
        "(select {c: (count k) s: (sum u) a: (avg h) from: T where: (> k 180000) by: h desc: c take: 3})",
        /* the emit-filter sparse table (a SYM key never takes the key range):
         * the count arm, then the heavy hitters' sums */
        "(select {c: (count k) s: (sum h) from: T where: (< k 5) by: s desc: c take: 2})",
        NULL };
    ray_group_test_route(2);
    for (int i = 0; qs[i]; i++) {
        test_result_t r = sr_same(qs[i], 0);
        if (r.status != TEST_PASS) { ray_group_test_route(0); return r; }
    }
    /* keys the dense count declines (negative, or past its cap) reach the
     * emit-filter key range without a selection: n's bounds come from the
     * directory, m's span takes the second (marker) sum pass, w's does not
     * fit (the sparse table), p / q are I32 with negatives (the range reads
     * them unsigned, so their directory bounds are not used), t is I16
     * (the two-byte key arm) */
    sr_eval_ok("(set N (table [n m w p q t x] (list (- (% (* i 7919) 1000003) 500000) "
               "(* (- (% i 1000) 500) 40000) (* (- (% i 1000) 500) 1000000) "
               "(as 'I32 (- (% i 1000) 2000)) (as 'I32 (- (% i 1000) 500)) "
               "(as 'I16 (- (% (* i 7919) 60001) 30000)) i)))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/n0/\" N {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/n2/\" N {compress: true})");
    static const char* const qn[] = {
        "(select {c: (count x) s: (sum x) from: T by: n desc: c take: 3})",
        "(select {c: (count x) s: (sum x) a: (avg x) from: T by: m desc: c take: 3})",
        "(select {c: (count x) s: (sum x) from: T by: w desc: c take: 3})",
        "(select {c: (count x) s: (sum x) from: T by: p desc: c take: 3})",
        "(select {c: (count x) s: (sum x) from: T by: q desc: c take: 3})",
        "(select {c: (count x) s: (sum x) from: T where: (> x 1000) by: m desc: c take: 3})",
        "(select {s: (sum x) a: (avg n) from: T by: w})",
        "(select {c: (count x) s: (sum x) from: T by: t})",
        "(select {c: (count x) s: (sum x) from: T by: t desc: c take: 3})",
        NULL };
    for (int i = 0; qn[i]; i++) {
        test_result_t r = sr_same_on("(.db.splayed.get \"" SR_TMP "/n0/\")", "(.db.splayed.get \"" SR_TMP "/n2/\")",
                                     qn[i], 0);
        if (r.status != TEST_PASS) { ray_group_test_route(0); return r; }
    }
    /* the fallback: an emit-filter sparse table over more than 2^21 rows
     * goes to the fat-entry routes, which read the key and input staged */
    sr_eval_ok("(set jf (til 2200000))");
    sr_eval_ok("(set F (table [w x] (list (* (% jf 1000) 1000000) jf)))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/f0/\" F {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/f2/\" F {compress: true})");
    test_result_t t = sr_same_on("(.db.splayed.get \"" SR_TMP "/f0/\")", "(.db.splayed.get \"" SR_TMP "/f2/\")",
                                 "(select {c: (count x) s: (sum x) from: T by: w desc: c take: 3})", 0);
    if (t.status != TEST_PASS) { ray_group_test_route(0); return t; }
    /* Fix round 2: a non-null F64 input read from a staged piece (the f64
     * arm rebases its row like the integer arms), under the sparse table's
     * sum pass and the emit-filter's heavy-hitter pass */
    sr_eval_ok("(set P (table [h w x] (list (% (* i 7919) 1000003) (* (% i 1000) 1000000) (as 'F64 i))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/p0/\" P {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/p2/\" P {compress: true})");
    static const char* const qp[] = {
        "(select {s: (sum x) a: (avg x) from: T by: h})",
        "(select {c: (count x) from: T by: w desc: c take: 3})",
        "(select {c: (count x) from: T where: (> x 1000) by: w desc: c take: 3})",
        "(select {c: (count x) s: (sum x) from: T where: (> x 1000) by: w desc: c take: 3})",
        NULL };
    for (int i = 0; qp[i]; i++) {
        test_result_t r = sr_same_on("(.db.splayed.get \"" SR_TMP "/p0/\")", "(.db.splayed.get \"" SR_TMP "/p2/\")",
                                     qp[i], 0);
        if (r.status != TEST_PASS) { ray_group_test_route(0); return r; }
    }
    /* A plain (materialized) key and a plain input beside staged columns:
     * the plain one is read at the absolute row, the staged at the piece's */
    sr_eval_ok("(set T2M (.db.splayed.get \"" SR_G2 "\"))");
    ray_t* tm = sr_eval("T2M");
    TEST_ASSERT_TRUE(tm && !RAY_IS_ERR(tm));
    ray_t* kc = ray_table_col(tm, ray_sym_intern("k", 1));
    ray_t* hc = ray_table_col(tm, ray_sym_intern("h", 1));
    TEST_ASSERT_TRUE(kc && !RAY_IS_ERR(kc) && hc && !RAY_IS_ERR(hc));
    sr_drop(tm);
    static const char* const qm[] = {
        "(select {c: (count u) s: (sum u) from: T by: k})",     /* plain key, staged input */
        "(select {c: (count k) s: (sum h) from: T by: u})",     /* plain inputs, staged key */
        NULL };
    for (int i = 0; qm[i]; i++) {
        test_result_t r = sr_same_on("(.db.splayed.get \"" SR_G0 "\")", "T2M", qm[i], 0);
        if (r.status != TEST_PASS) { ray_group_test_route(0); return r; }
    }
    /* R-FAIL: a corrupt chunk of the key or an input is the query's error */
    char path[600];
    static const struct { const char* col; const char* q; } cs[] = {
        { "h", "(select {c: (count k) s: (sum h) from: B by: k})" },                  /* input, sum arm */
        { "k", "(select {c: (count k) s: (sum h) from: B by: k})" },                  /* key, sum arm */
        { "h", "(select {c: (count k) s: (sum u) from: B where: (> k 180000) by: h desc: c take: 3})" },   /* key bounds */
        { "u", "(select {c: (count k) s: (sum u) from: B where: (> k 180000) by: h desc: c take: 3})" },   /* range input */
    };
    for (size_t c = 0; c < sizeof cs / sizeof cs[0]; c++) {
        if (c == 0 || strcmp(cs[c].col, cs[c - 1].col) != 0) sr_make_bad(cs[c].col, path, sizeof path);
        for (int i = 0; i < 2; i++) {
            sr_eval_ok("(set B (.db.splayed.get \"" SR_TMP "/bad/\"))");
            ray_error_clear();
            ray_t* r = sr_eval(cs[c].q);
            TEST_ASSERT_FMT(r && RAY_IS_ERR(r), "%s: no error", cs[c].q);
            TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
            TEST_ASSERT_TRUE(strstr(ray_error_msg(), path) != NULL);
            ray_error_free(r);
        }
    }
    ray_group_test_route(0);
    PASS();
}

/* Below the parallel threshold the serial builder (group_rows_range) and
 * the serial holistic reprobe read by pieces too (worker 0). */
static test_result_t test_radix_route_serial(void) {
    sr_eval_ok("(set j (til 50000))");
    sr_eval_ok("(set SM (table [k g v s f t] (list j (as 'I16 (% j 7)) "
               "(as 'I32 (map (fn [x] (if (== 0 (% x 10)) 0N x)) j)) (take ['a 'b 'c 'd] 50000) "
               "(as 'F64 (map (fn [x] (if (== 0 (% x 13)) 0N x)) j)) (as 'STR (% j 30)))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/sm0/\" SM {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/sm2/\" SM {compress: true})");
    static const struct { const char* q; uint64_t mat; } qs[] = {
        { "(select {c: (count k) s: (sum v) a: (avg f) from: T by: {g: g s: s}})", 0 },
        { "(select {c: (count k) s: (sum v) from: T where: (> k 100) by: v})", 0 },
        { "(select {m: (med v) s: (sum f) from: T by: s})", 1 },
        { "(select {m: (med f) from: T where: (> g 2) by: {g: g s: s}})", 1 },
        { "(select {s: (sum v) c: (count k) from: T by: t})", 0 },       /* STR key: string-domain ids */
        { "(select {m: (med v) a: (avg f) from: T by: {t: t g: g}})", 1 },
    };
    ray_group_test_route(2);
    for (size_t i = 0; i < sizeof qs / sizeof qs[0]; i++) {
        test_result_t r = sr_same_on("(.db.splayed.get \"" SR_TMP "/sm0/\")", "(.db.splayed.get \"" SR_TMP "/sm2/\")",
                                     qs[i].q, qs[i].mat);
        if (r.status != TEST_PASS) { ray_group_test_route(0); return r; }
    }
    ray_group_test_route(0);
    PASS();
}

/* R-FAIL: a corrupt chunk of a key or an input the radix builders read is
 * the query's error, on every run: phase 1, the v2 sibling (whole scan and
 * survivor list), the holistic route. */
static test_result_t test_radix_route_error(void) {
    char path[600];
    static const struct { const char* col; const char* q; } cs[] = {
        { "k", "(select {c: (count v) s: (sum v) from: B by: k})" },                         /* key, phase 1 */
        { "v", "(select {c: (count k) s: (sum v) from: B by: {k: k s: s}})" },               /* input, phase 1 */
        { "h", "(select {c: (count k) s: (sum h) from: B by: {g: g u: u}})" },               /* input, v2 */
        { "h", "(select {c: (count k) s: (sum h) from: B where: (> k 140000) by: {g: g u: u}})" },   /* v2 R-SPARSE: 59999 survivors, chunk 2 */
        { "g", "(select {m: (med v) from: B by: g})" },                                      /* key, holistic */
    };
    ray_group_test_route(2);
    for (size_t c = 0; c < sizeof cs / sizeof cs[0]; c++) {
        if (c == 0 || strcmp(cs[c].col, cs[c - 1].col) != 0) sr_make_bad(cs[c].col, path, sizeof path);
        for (int i = 0; i < 2; i++) {
            sr_eval_ok("(set B (.db.splayed.get \"" SR_TMP "/bad/\"))");
            ray_error_clear();
            ray_t* r = sr_eval(cs[c].q);
            TEST_ASSERT_FMT(r && RAY_IS_ERR(r), "%s: no error", cs[c].q);
            TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
            TEST_ASSERT_TRUE(strstr(ray_error_msg(), path) != NULL);
            ray_error_free(r);
        }
    }
    /* chunk 2 (rows 131072..196607) of g is corrupt; a survivor list that
     * ends before it never reads it */
    test_result_t t = sr_same_on("(.db.splayed.get \"" SR_G0 "\")", "(.db.splayed.get \"" SR_TMP "/bad/\")",
                                 "(select {c: (count k) s: (sum h) from: T where: (< k 60000) by: {g: g u: u}})", 0);
    ray_group_test_route(0);
    if (t.status != TEST_PASS) return t;
    PASS();
}

/* ---- Task 13: the v2 aggregate engine reads by pieces ---- */

static test_result_t test_v2_routes(void) {
    agg_route_reset();
    SR_SAME("(select {s: (sum v) c: (count k) from: T by: g})", 0);                     /* dense */
    SR_SAME("(select {s: (sum f) a: (avg v) from: T where: (> k 5000) by: g})", 0);     /* dense, sel */
    SR_SAME("(select {c: (count k) from: T by: {g: g s: s}})", 0);                      /* multi key */
    SR_SAME("(select {c: (count k) mn: (min v) from: T by: h})", 0);                    /* high cardinality: radix */
    SR_SAME("(select {c: (count k) from: T where: (== b true) by: d})", 0);
    SR_SAME("(select {x: (max f) from: T by: {d: d u: u}})", 0);
    agg_route_stats_t rs = agg_route_stats();
    TEST_ASSERT_TRUE(rs.routes[AGG_ROUTE_V2_DENSE] + rs.routes[AGG_ROUTE_V2_SERIAL_DENSE] > 0);
    TEST_ASSERT_TRUE(rs.routes[AGG_ROUTE_V2_RADIX] + rs.routes[AGG_ROUTE_V2_TABLESTACK] > 0);
    PASS();
}

/* Review focus 1: a 200001-row table on 3 workers gives dense blocks of
 * 66667 rows, none vector-aligned. */
static test_result_t test_v2_dense_unaligned(void) {
    sr_eval_ok("(set Q (table [g v] (list (as 'I16 (% (til 200001) 5)) (til 200001))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/q2/\" Q {compress: true})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/q0/\" Q {compress: false})");
    ray_pool_destroy(); TEST_ASSERT_EQ_I(ray_pool_init_total(3), RAY_OK);
    ray_t* a = sr_eval("(select {s: (sum v) from: (.db.splayed.get \"" SR_TMP "/q0/\") by: g})");
    ray_enc_counters_reset();
    ray_t* b = sr_eval("(select {s: (sum v) from: (.db.splayed.get \"" SR_TMP "/q2/\") by: g})");
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_TRUE(a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b));
    ray_t* fa = ray_fmt(a, 0); ray_t* fb = ray_fmt(b, 0);
    TEST_ASSERT_TRUE(ray_str_len(fa) == ray_str_len(fb) && memcmp(ray_str_ptr(fa), ray_str_ptr(fb), ray_str_len(fa)) == 0);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    ray_release(fa); ray_release(fb); ray_release(a); ray_release(b);
    PASS();
}
/* Task 13b: decodes per column of one generation-2 group-by.  Rows of
 * column `name` of table T decoded during query q, over the column's rows
 * (1.0: every vector decoded exactly once). */
static double sr_col_decodes(const char* name, uint64_t before) {
    ray_t* t = sr_eval("T");
    ray_t* ec = ray_table_get_col_enc(t, ray_sym_intern(name, strlen(name)));
    double r = -1;
    if (ec) {
        ray_enccol_t* e = ray_enccol_of(ec);
        r = (double)(atomic_load(&e->decoded_bytes) - before) / ((double)e->len * ray_enccol_esz(e));
    }
    sr_drop(t);
    return r;
}
static uint64_t sr_col_decoded(const char* name) {
    ray_t* t = sr_eval("T");
    ray_t* ec = ray_table_get_col_enc(t, ray_sym_intern(name, strlen(name)));
    uint64_t r = ec ? atomic_load(&ray_enccol_of(ec)->decoded_bytes) : 0;
    sr_drop(t);
    return r;
}

/* The samplers of the dense and table-stack plans read their sampled keys
 * without decoding vectors (point reads): the key decodes as often as the
 * aggregate input walked beside it.  A task walks its consecutive blocks
 * through one cursor set, so a vector is decoded once per task that reads
 * it: at most one extra vector per task (the one its range starts inside).
 * The same answers as generation 0. */
static test_result_t test_v2_decode_once(void) {
    sr_eval_ok("(set jd (til 600001))");
    sr_eval_ok("(set DQ (table [k v ts] (list (as 'I32 (% (* jd 7919) 1000)) jd (* (% (* jd 104729) 2000) 100003))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/dq2/\" DQ {compress: true})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/dq0/\" DQ {compress: false})");
    ray_pool_destroy(); TEST_ASSERT_EQ_I(ray_pool_init_total(4), RAY_OK);
    static const struct { const char* key; const char* q; int route; } qs[] = {
        { "k",  "(select {s: (sum v) c: (count v) from: T by: k})",  AGG_ROUTE_V2_DENSE },
        { "ts", "(select {s: (sum v) c: (count v) from: T by: ts})", AGG_ROUTE_V2_TABLESTACK },
    };
    for (size_t i = 0; i < sizeof qs / sizeof qs[0]; i++) {
        test_result_t r = sr_same_on("(.db.splayed.get \"" SR_TMP "/dq0/\")", "(.db.splayed.get \"" SR_TMP "/dq2/\")",
                                     qs[i].q, 0);
        if (r.status != TEST_PASS) return r;
        sr_eval_ok("(set T (.db.splayed.get \"" SR_TMP "/dq2/\"))");
        uint64_t k0 = sr_col_decoded(qs[i].key), v0 = sr_col_decoded("v");
        agg_route_reset();
        ray_t* res = sr_eval(qs[i].q);
        TEST_ASSERT_TRUE(res && !RAY_IS_ERR(res));
        ray_release(res);
        agg_route_stats_t st = agg_route_stats();
        TEST_ASSERT_FMT(st.routes[qs[i].route] == 1, "%s: route %d not taken", qs[i].q, qs[i].route);
        if (qs[i].route == AGG_ROUTE_V2_DENSE) TEST_ASSERT_EQ_I(st.dense_strategy, AGG_DENSE_TASK_LOCAL);
        double kd = sr_col_decodes(qs[i].key, k0), vd = sr_col_decodes("v", v0);
        TEST_ASSERT_FMT(kd == vd, "%s: key decoded %.4fx, input %.4fx", qs[i].q, kd, vd);
        uint32_t tasks = qs[i].route == AGG_ROUTE_V2_DENSE ? st.dense_tasks : st.ts_tasks;
        TEST_ASSERT_TRUE(tasks > 1);
        double cap = 1.0 + (double)tasks * RAY_ENC_VEC / 600001.0;
        TEST_ASSERT_FMT(vd >= 1.0 && vd <= cap, "%s: input decoded %.4fx over %u tasks (at most %.4fx)",
                        qs[i].q, vd, tasks, cap);
    }
    PASS();
}

static bool sr_same_text(ray_t* a, ray_t* b);

/* q over the tables t0 / t2: the same answer, the same routes, dense
 * strategy and dense-plan availability (agg_route_stats). */
static test_result_t sr_same_route(const char* t0, const char* t2, const char* q) {
    agg_route_stats_t st[2];
    ray_t* out[2] = { NULL, NULL };
    for (int gen = 0; gen < 2; gen++) {
        sr_evalf_ok("(set T %s)", gen ? t2 : t0);
        agg_route_reset();
        out[gen] = sr_eval(q);
        st[gen] = agg_route_stats();
        TEST_ASSERT_FMT(out[gen] && !RAY_IS_ERR(out[gen]), "gen %d: %s: %s", gen, q, out[gen] ? ray_error_msg() : "null");
    }
    for (int r = 0; r < AGG_ROUTE_COUNT; r++)
        TEST_ASSERT_FMT(st[0].routes[r] == st[1].routes[r], "%s: route %d taken %llu / %llu times", q, r,
                        (unsigned long long)st[0].routes[r], (unsigned long long)st[1].routes[r]);
    TEST_ASSERT_FMT(st[0].dense_strategy == st[1].dense_strategy, "%s: dense strategy %d / %d", q,
                    (int)st[0].dense_strategy, (int)st[1].dense_strategy);
    TEST_ASSERT_FMT(st[0].dense_plan_available == st[1].dense_plan_available, "%s: dense plan %d / %d", q,
                    (int)st[0].dense_plan_available, (int)st[1].dense_plan_available);
    TEST_ASSERT_FMT(st[0].dense_local_slots == st[1].dense_local_slots, "%s: dense slots %llu / %llu", q,
                    (unsigned long long)st[0].dense_local_slots, (unsigned long long)st[1].dense_local_slots);
    TEST_ASSERT_TRUE(sr_same_text(out[0], out[1]));
    ray_release(out[0]); ray_release(out[1]);
    PASS();
}

/* The selection prescan's key bounds read only the chunks whose
 * directory statistics can move them, and equal the full scan's: chunk 0
 * holds the lowest minimum and chunk 1 the highest maximum on unselected
 * rows, chunk 2's lowest selected key (2) lies below both of their
 * selected keys, the other chunks lie inside; a nullable key with an
 * all-null chunk and a chunk with nulls; filters of one row, of no row
 * and of an all-null chunk only.  Route, plan and answer as generation 0. */
static test_result_t test_v2_sel_bounds(void) {
    const int64_t n = 10 * 65536 + 1234;
    ray_t* k = ray_vec_new(RAY_I64, n); k->len = n;
    ray_t* kn = ray_vec_new(RAY_I64, n); kn->len = n;
    ray_t* s = ray_vec_new(RAY_I64, n); s->len = n;
    int64_t* kd = ray_data(k); int64_t* knd = ray_data(kn); int64_t* sd = ray_data(s);
    for (int64_t i = 0; i < n; i++) { kd[i] = 10 + (i * 7919) % 11; sd[i] = 1; }
    kd[0] = -5; sd[0] = 0; kd[1] = 50; sd[1] = 0;                 /* chunk 0: the lowest minimum, unselected */
    kd[65536] = 1000000; sd[65536] = 0;                           /* chunk 1: the highest maximum, unselected */
    kd[131072 + 5] = 2;                                           /* chunk 2: the lowest selected key */
    kd[131072 + 6] = 1; sd[131072 + 6] = 0; kd[131072 + 7] = 30; sd[131072 + 7] = 0;
    for (int64_t i = 0; i < n; i++)
        knd[i] = (i >= 4 * 65536 && i < 5 * 65536) || (i >= 5 * 65536 && i < 6 * 65536 && i % 100 == 0) ? NULL_I64 : kd[i];
    kn->attrs |= RAY_ATTR_HAS_NULLS;
    ray_env_set(ray_sym_intern("bk", 2), k); ray_env_set(ray_sym_intern("bkn", 3), kn); ray_env_set(ray_sym_intern("bs", 2), s);
    ray_release(k); ray_release(kn); ray_release(s);
    sr_eval_ok("(set SB (table [k kn s v w] (list bk bkn bs (til 656594) (% (til 656594) 3))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/sb2/\" SB {compress: true})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/sb0/\" SB {compress: false})");
    static const char* const qs[] = {
        "(select {c: (count v) s: (sum v) from: T where: (== s 1) by: k})",
        "(select {c: (count v) s: (sum v) from: T where: (== s 1) by: kn})",
        "(select {c: (count v) s: (sum v) from: T where: (== s 1) by: {k: k w: w}})",
        "(select {c: (count v) s: (sum v) from: T where: (== v 77) by: k})",                       /* one row */
        "(select {c: (count v) s: (sum v) from: T where: (== v -1) by: k})",                       /* no row */
        "(select {c: (count v) s: (sum v) from: T where: (and (>= v 262144) (< v 327680)) by: kn})",   /* the all-null chunk */
        "(select {c: (count v) s: (sum v) from: T where: (and (>= v 131072) (< v 196608)) by: k})",    /* one chunk */
        NULL };
    for (int i = 0; qs[i]; i++) {
        test_result_t r = sr_same_route("(.db.splayed.get \"" SR_TMP "/sb0/\")", "(.db.splayed.get \"" SR_TMP "/sb2/\")", qs[i]);
        if (r.status != TEST_PASS) return r;
    }
    /* the mirror of round 2 on the maximum: chunk 0 holds the lowest minimum
     * and the highest maximum, both on unselected rows (its selected keys are
     * 10..20); chunk 1's statistics are [10, 500] with 500 selected, so only
     * its maximum reaches outside the bounds round 1 found */
    {
        const int64_t m = 4 * 65536;
        ray_t* k2 = ray_vec_new(RAY_I64, m); k2->len = m;
        ray_t* s2 = ray_vec_new(RAY_I64, m); s2->len = m;
        int64_t* k2d = ray_data(k2); int64_t* s2d = ray_data(s2);
        for (int64_t i = 0; i < m; i++) { k2d[i] = 10 + (i * 7919) % 11; s2d[i] = 1; }
        k2d[0] = -5; s2d[0] = 0; k2d[1] = 100000; s2d[1] = 0;
        k2d[65536 + 3] = 500;
        ray_env_set(ray_sym_intern("bk2", 3), k2); ray_env_set(ray_sym_intern("bs2", 3), s2);
        ray_release(k2); ray_release(s2);
        sr_eval_ok("(set SM (table [k s v] (list bk2 bs2 (til 262144))))");
        sr_eval_ok("(.db.splayed.set \"" SR_TMP "/sm2/\" SM {compress: true})");
        sr_eval_ok("(.db.splayed.set \"" SR_TMP "/sm0/\" SM {compress: false})");
        test_result_t r = sr_same_route("(.db.splayed.get \"" SR_TMP "/sm0/\")", "(.db.splayed.get \"" SR_TMP "/sm2/\")",
                                        "(select {c: (count v) s: (sum v) from: T where: (== s 1) by: k})");
        if (r.status != TEST_PASS) return r;
    }
    /* the dense plan of the first: chunks 0, 1 and 2 are read for its bounds,
     * not all ten (a full prescan reads the key once more than the pass) */
    sr_eval_ok("(set T (.db.splayed.get \"" SR_TMP "/sb2/\"))");
    uint64_t k0 = sr_col_decoded("k");
    agg_route_reset();
    ray_t* res = sr_eval(qs[0]);
    TEST_ASSERT_TRUE(res && !RAY_IS_ERR(res));
    ray_release(res);
    TEST_ASSERT_TRUE(agg_route_stats().dense_plan_available);
    double kdec = sr_col_decodes("k", k0);
    TEST_ASSERT_FMT(kdec < 1.5, "key decoded %.3fx", kdec);
    PASS();
}

/* Task 17b: a selection prescan over a mixed key set.  The SYM key is read
 * exactly first; the staged I64 key k then takes the directory path as a
 * key set of its own (chunk pruning as above: g has 3 values), or, when the
 * SYM key's range alone exceeds the dense limit (h: about 13 000 selected
 * rows over 50 021 ids), is not read for bounds at all.  Route, plan,
 * slots and answer as generation 0. */
static test_result_t test_v2_sel_bounds_split(void) {
    const int64_t n = 10 * 65536 + 1234;
    ray_t* k = ray_vec_new(RAY_I64, n); k->len = n;
    int64_t* kd = ray_data(k);
    for (int64_t i = 0; i < n; i++) kd[i] = 10 + (i * 7919) % 11;
    kd[0] = -5; kd[65536] = 1000000;                         /* chunks 0 and 1: the extremes, unselected */
    /* m: every chunk's directory reaches past the selected rows' keys
     * (rows c * 65536 + 1 and + 2, never selected by v % 50 == 0), so the
     * directory path would read every chunk */
    ray_t* mk = ray_vec_new(RAY_I64, n); mk->len = n;
    int64_t* md = ray_data(mk);
    for (int64_t i = 0; i < n; i++) md[i] = i % 11;
    for (int64_t c = 0; c * 65536 + 2 < n; c++) { md[c * 65536 + 1] = -1000 - c; md[c * 65536 + 2] = 1000 + c; }
    ray_env_set(ray_sym_intern("pk", 2), k); ray_release(k);
    ray_env_set(ray_sym_intern("pm", 2), mk); ray_release(mk);
    sr_eval_ok("(set pi (til 656594))");
    sr_eval_ok("(set SP (table [k m g h v s] (list pk pm (as 'SYM (as 'STR (% pi 3))) "
               "(as 'SYM (as 'STR (% (* pi 7919) 50021))) pi (!= 0 (% pi 65536)))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/sp2/\" SP {compress: true})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/sp0/\" SP {compress: false})");
    static const struct { const char* q; bool dense; } qs[] = {
        { "(select {c: (count v) s: (sum v) from: T where: s by: [g k]})", true },
        { "(select {c: (count v) s: (sum v) from: T where: s by: [k g]})", true },
        { "(select {c: (count v) s: (sum v) from: T where: (== 0 (% v 50)) by: [k h]})", false },
        { "(select {c: (count v) s: (sum v) from: T where: (== 0 (% v 50)) by: [h k]})", false },
        { "(select {c: (count v) s: (sum v) from: T where: (== 0 (% v 50)) by: [m h]})", false },
        { "(select {c: (count v) s: (sum v) from: T where: (== 0 (% v 50)) by: [h m]})", false },
        { NULL, false },
    };
    for (int i = 0; qs[i].q; i++) {
        test_result_t r = sr_same_route("(.db.splayed.get \"" SR_TMP "/sp0/\")", "(.db.splayed.get \"" SR_TMP "/sp2/\")", qs[i].q);
        if (r.status != TEST_PASS) return r;
        sr_eval_ok("(set T (.db.splayed.get \"" SR_TMP "/sp2/\"))");
        const char* key = strstr(qs[i].q, "m h") || strstr(qs[i].q, "h m") ? "m" : "k";
        uint64_t k0 = sr_col_decoded(key), v0 = sr_col_decoded("v");
        agg_route_reset();
        ray_t* res = sr_eval(qs[i].q);
        TEST_ASSERT_TRUE(res && !RAY_IS_ERR(res));
        ray_release(res);
        /* the pass reads k once; a full prescan would read it once more */
        double kdec = sr_col_decodes(key, k0), vdec = sr_col_decodes("v", v0);
        TEST_ASSERT_FMT(agg_route_stats().dense_plan_available == qs[i].dense, "%s: dense plan %d", qs[i].q,
                        (int)agg_route_stats().dense_plan_available);
        TEST_ASSERT_FMT(kdec < 1.5 && vdec >= 1.0, "%s: k decoded %.4fx, v %.4fx", qs[i].q, kdec, vdec);
    }
    PASS();
}

/* Task 17b fix round 1: the dense-impossible shortcut of the split prescan
 * at its boundary.  g (SYM) holds one new symbol per row (two rows per
 * symbol in the nullable table), so its selected ids span exactly the
 * selected symbols; k (staged I64) is 5 on every selected row (the
 * nullable table: one null, so its range is 2).  The other keys' product
 * equals n_sel (the selection plan is dense, as on generation 0) or
 * exceeds it (no selection plan, k not read for bounds).  Route, plan and
 * slots as generation 0: an early verdict at the boundary would differ. */
static test_result_t test_v2_sel_bounds_boundary(void) {
    sr_eval_ok("(set bi (til 200000))");
    sr_eval_ok("(set BB (table [g k v] (list (as 'SYM (as 'STR (+ 100000 bi))) (take [5] 200000) bi)))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/bb2/\" BB {compress: true})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/bb0/\" BB {compress: false})");
    sr_eval_ok("(set BN (table [g k v] (list (as 'SYM (as 'STR (+ 100000 (div bi 2)))) "
               "(map (fn [j] (if (== j 7) 0Nl 5)) bi) bi)))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/bn2/\" BN {compress: true})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/bn0/\" BN {compress: false})");
    /* k_bounds: k is read for the selection bounds (the other keys' product
     * is at most n_sel).  When the selection plan fails the group plans over
     * the table, so the route stats alone cannot show the verdict. */
    static const struct { const char* t; const char* q; bool k_bounds; } qs[] = {
        /* 5000 rows, g spans 5000 ids: product == n_sel */
        { "bb", "(select {c: (count v) from: T where: (< v 5000) by: [g k]})", true },
        { "bb", "(select {c: (count v) from: T where: (< v 5000) by: [k g]})", true },
        /* 5000 rows, g spans 5001 ids (row 1 left out): product == n_sel + 1 */
        { "bb", "(select {c: (count v) from: T where: (and (<= v 5000) (!= v 1)) by: [g k]})", false },
        { "bb", "(select {c: (count v) from: T where: (and (<= v 5000) (!= v 1)) by: [k g]})", false },
        /* nullable k (range 2): 5000 rows, g spans 2500 ids: 2 x 2500 == n_sel */
        { "bn", "(select {c: (count v) from: T where: (< v 5000) by: [g k]})", true },
        /* 5000 rows (0, 3..5001), g spans 2501 ids: 2 x 2501 > n_sel, but g
         * alone fits, so k is read and the plan fails on the product (the
         * chunk-extremes probe of P3 Task 14b does not run: the chunk
         * holding g's extremes holds every selected row, more than its
         * quarter) */
        { "bn", "(select {c: (count v) from: T where: (and (<= v 5001) (!= v 1) (!= v 2)) by: [g k]})", true },
        { NULL, NULL, false },
    };
    for (int i = 0; qs[i].q; i++) {
        char t0[128], t2[128];
        snprintf(t0, sizeof t0, "(.db.splayed.get \"" SR_TMP "/%s0/\")", qs[i].t);
        snprintf(t2, sizeof t2, "(.db.splayed.get \"" SR_TMP "/%s2/\")", qs[i].t);
        test_result_t r = sr_same_route(t0, t2, qs[i].q);
        if (r.status != TEST_PASS) return r;
        sr_evalf_ok("(set T %s)", t2);
        uint64_t k0 = sr_col_decoded("k");
        ray_t* res = sr_eval(qs[i].q);
        TEST_ASSERT_TRUE(res && !RAY_IS_ERR(res));
        ray_release(res);
        /* the compact fallback gathers the five vectors of the selected
         * rows of k (P3 Task 6; before, it read all 200000 I64); the bounds
         * prescan, when it reads k (k_bounds), reads its CONST vectors on
         * their runs (P3 Task 3b; before, it decoded the five vectors once
         * more), all but bn's vector 0, whose null (row 7) gives it a
         * bitmap: no run view, so it is decoded */
        const bool nullk = strcmp(qs[i].t, "bn") == 0;
        const uint64_t want = 5 * RAY_ENC_VEC * 8 + (qs[i].k_bounds && nullk ? RAY_ENC_VEC * 8 : 0);
        TEST_ASSERT_FMT(sr_col_decoded("k") - k0 == want, "%s: %llu bytes of k decoded, want %llu", qs[i].q,
                        (unsigned long long)(sr_col_decoded("k") - k0), (unsigned long long)want);
    }
    PASS();
}

/* Task 13b fix round 1: directory statistics size the dense plans, so a
 * forged one must not reach memory.  Key k is j * 7 % 1000 in chunks 0-2
 * and j * 7 % 5001 in chunk 3; chunk 3's directory maximum is patched from
 * 5000 to 999 and the file still loads.  The filtered (the selection
 * bounds) and the unfiltered (the directory prescan) group-bys both
 * report `corrupt` naming the column, twice, and never write past the
 * slab (ASan). */
static void sr_patch64(const char* path, long off, uint64_t val) {
    FILE* f = fopen(path, "r+b"); if (!f) return;
    if (fseek(f, off, SEEK_SET) == 0) (void)fwrite(&val, 8, 1, f);
    fclose(f);
}
/* Chunk c of the generation-2 column file at path: its directory maximum
 * and every vector descriptor's maximum above x lowered to x, so the
 * statistics still nest and validate, and only the values lie above them
 * (the consumer guard's case).  False on an I/O error or a vector whose
 * minimum lies above x. */
static bool sr_forge_max(const char* path, int64_t c, int64_t x) {
    FILE* f = fopen(path, "r+b"); if (!f) return false;
    ray_enc_dirent_t de;
    long doff = (long)(RAY_ENC_DIR_OFF + (uint64_t)c * sizeof de);
    bool ok = fseek(f, doff, SEEK_SET) == 0 && fread(&de, sizeof de, 1, f) == 1 && !(de.flags & RAY_ENC_STAT_ABSENT);
    if (ok) { de.max = x; ok = fseek(f, doff, SEEK_SET) == 0 && fwrite(&de, sizeof de, 1, f) == 1; }
    for (int j = 0; ok && j < RAY_ENC_VECS_PER_CHUNK; j++) {
        ray_enc_desc_t d;
        long off = (long)(de.offset + (uint64_t)j * sizeof d);
        ok = fseek(f, off, SEEK_SET) == 0 && fread(&d, sizeof d, 1, f) == 1;
        if (!ok || (d.encoding == 0 && d.payload_bytes == 0)) break;     /* past the last vector */
        if (d.flags & RAY_ENC_STAT_ABSENT || d.max <= x) continue;
        if (d.min > x) { ok = false; break; }
        d.max = x;
        ok = fseek(f, off, SEEK_SET) == 0 && fwrite(&d, sizeof d, 1, f) == 1;
    }
    return fclose(f) == 0 && ok;
}
/* q over the table at dir (set as T) gives `corrupt` naming path, twice. */
static test_result_t sr_corrupt_twice(const char* dir_expr, const char* q, const char* path) {
    for (int rep = 0; rep < 2; rep++) {
        sr_evalf_ok("(set T %s)", dir_expr);
        ray_error_clear();
        ray_t* r = sr_eval(q);
        TEST_ASSERT_FMT(r && RAY_IS_ERR(r), "%s (read %d): answered", q, rep + 1);
        TEST_ASSERT_FMT(strcmp(ray_err_code(r), "corrupt") == 0 && ray_error_msg() && strstr(ray_error_msg(), path),
                        "%s (read %d): %s \"%s\"", q, rep + 1, ray_err_code(r), ray_error_msg() ? ray_error_msg() : "");
        ray_error_free(r);
    }
    PASS();
}

/* The consumer guards: every consumer kind that sizes memory from the
 * directory statistics, over a key whose chunk 3 statistics (directory
 * and descriptors, still nested) are lowered below its values.  The dense
 * plan unfiltered (the directory prescan) and filtered (the selection
 * bounds), the direct array, the sparse emit-filter range: each `corrupt`
 * naming the key, twice; the same queries on the unpatched table answer
 * as generation 0. */
static test_result_t test_v2_forged_consumers(void) {
    /* k: j*7 % 1000 in chunks 0-2, % 5001 in chunk 3 with a null every
     * 100 rows (so the samplers' point reads decline there and only the
     * guard sees the values); g: % 50, % 101; n: a wide I64 key (the
     * sparse range), + 400000 in chunk 3 */
    sr_eval_ok("(set jf (til 262144))");
    sr_eval_ok("(set ja (til 196608))");
    sr_eval_ok("(set jb (+ 196608 (til 65536)))");
    sr_eval_ok("(set FC (table [k g n v] (list (concat (% (* ja 7) 1000) (map (fn [j] (if (== 0 (% j 100)) 0N (% (* j 7) 5001))) jb)) "
               "(as 'I16 (concat (% (* ja 7) 50) (% (* jb 7) 101))) "
               "(concat (- (% (* ja 7919) 1000003) 500000) (+ 400000 (% (* jb 7919) 1000003))) jf)))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/fc0/\" FC {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/fc2/\" FC {compress: true})");
    static const struct { const char* col; int64_t x; int route; const char* q; } cs[] = {
        { "k", 999,    0, "(select {s: (sum v) c: (count v) from: T by: k})" },                     /* dense, directory prescan */
        { "k", 999,    0, "(select {s: (sum v) c: (count v) from: T where: (> v 0) by: k})" },      /* dense, selection bounds */
        { "g", 49,     1, "(select {s: (sum v) c: (count v) from: T by: g})" },                     /* the direct array */
        { "n", 500003, 2, "(select {c: (count v) s: (sum v) from: T by: n desc: c take: 3})" },     /* the sparse range */
    };
    for (size_t i = 0; i < sizeof cs / sizeof cs[0]; i++) {
        ray_group_test_route(cs[i].route);
        test_result_t r = sr_same_on("(.db.splayed.get \"" SR_TMP "/fc0/\")", "(.db.splayed.get \"" SR_TMP "/fc2/\")", cs[i].q, 0);
        if (r.status != TEST_PASS) { ray_group_test_route(0); return r; }
        sr_eval_ok("(.db.splayed.set \"" SR_TMP "/fcb/\" FC {compress: true})");
        char dir[512], path[600];
        if (ray_splay_resolve_dir(ray_test_tmp(SR_TMP "/fcb/"), dir, sizeof dir) != RAY_OK) { ray_group_test_route(0); FAIL("resolve"); }
        snprintf(path, sizeof path, "%s/%s", dir, cs[i].col);
        if (!sr_forge_max(path, 3, cs[i].x)) { ray_group_test_route(0); FAIL("forge"); }
        r = sr_corrupt_twice("(.db.splayed.get \"" SR_TMP "/fcb/\")", cs[i].q, path);
        ray_group_test_route(0);
        if (r.status != TEST_PASS) return r;
    }
    PASS();
}

/* Chunk c's directory maximum and every vector descriptor's statistics
 * above x capped at x (a minimum above x lowered to x too, so a CONST
 * vector's [v, v] becomes [x, x]): the statistics still nest and
 * validate, and the values above x lie outside them.  False on an I/O
 * error. */
static bool sr_forge_cap(const char* path, int64_t c, int64_t x) {
    FILE* f = fopen(path, "r+b"); if (!f) return false;
    ray_enc_dirent_t de;
    long doff = (long)(RAY_ENC_DIR_OFF + (uint64_t)c * sizeof de);
    bool ok = fseek(f, doff, SEEK_SET) == 0 && fread(&de, sizeof de, 1, f) == 1 && !(de.flags & RAY_ENC_STAT_ABSENT);
    if (ok) {
        de.max = x;
        if (de.min > x) de.min = x;
        ok = fseek(f, doff, SEEK_SET) == 0 && fwrite(&de, sizeof de, 1, f) == 1;
    }
    for (int j = 0; ok && j < RAY_ENC_VECS_PER_CHUNK; j++) {
        ray_enc_desc_t d;
        long off = (long)(de.offset + (uint64_t)j * sizeof d);
        ok = fseek(f, off, SEEK_SET) == 0 && fread(&d, sizeof d, 1, f) == 1;
        if (!ok || (d.encoding == 0 && d.payload_bytes == 0)) break;     /* past the last vector */
        if (d.flags & RAY_ENC_STAT_ABSENT || d.max <= x) continue;
        d.max = x;
        if (d.min > x) d.min = x;
        ok = fseek(f, off, SEEK_SET) == 0 && fwrite(&d, sizeof d, 1, f) == 1;
    }
    return fclose(f) == 0 && ok;
}

/* P3 Task 6b fix round 2: the run arm of the batched point read
 * (points_runs) under forged statistics.  k is an RLE key (runs of 5) and
 * a CONST one (one value per vector); chunk 3 holds values up to 5000 /
 * 4000 + 63, its statistics capped at 999.  A filter keeping every 97th
 * row reads k at about 10 rows per vector: point reads in the selection
 * bounds and the compact gather.  Each read reports `corrupt` naming k,
 * twice, with no sanitizer report; unpatched, the answers are generation
 * 0's and k decodes nothing. */
static test_result_t test_v2_forged_run_points(void) {
    sr_eval_ok("(set ja (til 196608))");
    sr_eval_ok("(set jb (+ 196608 (til 65536)))");
    static const struct { const char* name; const char* k; int enc; } cs[] = {
        { "fr", "(concat (% (* (div ja 5) 7) 1000) (% (* (div jb 5) 7) 5001))", RAY_ENC_RLE },
        { "fk", "(concat (% (div ja 1024) 1000) (+ 4000 (% (div jb 1024) 64)))", -1 },
    };
    static const char* const qs[] = {
        "(select {c: (count v) from: T where: (== 0 (% v 97)) by: k})",
        "(select {c: (count v) s: (sum v) from: T where: (== 0 (% v 97)) by: k desc: c take: 5})",
        NULL,
    };
    for (size_t i = 0; i < sizeof cs / sizeof cs[0]; i++) {
        sr_evalf_ok("(set FQ (table [k v] (list %s (til 262144))))", cs[i].k);
        sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/%s0/\" FQ {compress: false})", cs[i].name);
        ray_encw_test_force(cs[i].enc);
        sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/%s2/\" FQ {compress: true})", cs[i].name);
        sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/%sb/\" FQ {compress: true})", cs[i].name);
        ray_encw_test_force(-1);
        char t0[256], t2[256], tb[256], rel[256], dir[512], path[600];
        snprintf(t0, sizeof t0, "(.db.splayed.get \"" SR_TMP "/%s0/\")", cs[i].name);
        snprintf(t2, sizeof t2, "(.db.splayed.get \"" SR_TMP "/%s2/\")", cs[i].name);
        snprintf(tb, sizeof tb, "(.db.splayed.get \"" SR_TMP "/%sb/\")", cs[i].name);
        /* every vector of k in chunk 3 is the arm's codec */
        {
            ray_t* t = sr_eval(t2);
            ray_t* ec = ray_table_get_col_enc(t, ray_sym_intern("k", 1));
            TEST_ASSERT_NOT_NULL(ec);
            const ray_enc_desc_t* d;
            TEST_ASSERT_NULL(ray_enccol_chunk_descs_ref(ray_enccol_of(ec), 3, &d));
            for (int j = 0; j < RAY_ENC_VECS_PER_CHUNK; j++)
                TEST_ASSERT_FMT(d[j].encoding == (i ? RAY_ENC_CONST : RAY_ENC_RLE), "%s vector %d: %d", cs[i].name, j, d[j].encoding);
            sr_drop(t);
        }
        for (int q = 0; qs[q]; q++) {
            test_result_t r = sr_same_on(t0, t2, qs[q], 0);
            if (r.status != TEST_PASS) return r;
            TEST_ASSERT_FMT(sr_col_decoded("k") == 0, "%s %s: %llu bytes of k decoded", cs[i].name, qs[q],
                            (unsigned long long)sr_col_decoded("k"));
        }
        snprintf(rel, sizeof rel, SR_TMP "/%sb/", cs[i].name);
        if (ray_splay_resolve_dir(ray_test_tmp(rel), dir, sizeof dir) != RAY_OK) FAIL("resolve");
        snprintf(path, sizeof path, "%s/k", dir);
        if (!sr_forge_cap(path, 3, 999)) FAIL("forge");
        for (int q = 0; qs[q]; q++) {
            test_result_t r = sr_corrupt_twice(tb, qs[q], path);
            if (r.status != TEST_PASS) return r;
        }
    }
    PASS();
}

/* P3 Task 6b: point reads under a guard.  A selective filter (every 97th
 * row: a few rows per vector) groups by k, whose selected rows are
 * point-read (FOR_BP, no nulls) by the dense plan's selection bounds and
 * by the compact gather.  Chunk 3's statistics (directory and descriptors,
 * still nested) are lowered below its values: the dense plan sized from
 * them never sees a value outside them, since the point read checks each
 * value against its vector's statistics (`corrupt` naming k, twice, no
 * write past the slab under ASan).  The first-n passes over the same
 * forged key (an unordered take) report it too.  Unpatched, the answer is
 * generation 0's and nothing is materialized. */
static test_result_t test_v2_forged_points(void) {
    sr_eval_ok("(set ja (til 196608))");
    sr_eval_ok("(set jb (+ 196608 (til 65536)))");
    sr_eval_ok("(set FP (table [k v] (list (concat (% (* ja 7) 1000) (% (* jb 7) 5001)) (til 262144))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/fp0/\" FP {compress: false})");
    ray_encw_test_force(RAY_ENC_FOR_BP);
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/fp2/\" FP {compress: true})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/fpb/\" FP {compress: true})");
    ray_encw_test_force(-1);
    static const char* const qs[] = {
        "(select {c: (count v) from: T where: (== 0 (% v 97)) by: k})",
        "(select {c: (count v) s: (sum v) from: T where: (== 0 (% v 97)) by: k desc: c take: 5})",
        "(select {c: (count v) from: T by: k take: 5})",   /* first-n: pass 2 reads every vector */
        NULL,
    };
    for (int i = 0; qs[i]; i++) {
        test_result_t r = sr_same_on("(.db.splayed.get \"" SR_TMP "/fp0/\")", "(.db.splayed.get \"" SR_TMP "/fp2/\")", qs[i], 0);
        if (r.status != TEST_PASS) return r;
        if (i < 2) TEST_ASSERT_FMT(sr_col_decoded("k") == 0, "%s: %llu bytes of k decoded", qs[i], (unsigned long long)sr_col_decoded("k"));
    }
    char dir[512], path[600];
    if (ray_splay_resolve_dir(ray_test_tmp(SR_TMP "/fpb/"), dir, sizeof dir) != RAY_OK) FAIL("resolve");
    snprintf(path, sizeof path, "%s/k", dir);
    if (!sr_forge_max(path, 3, 999)) FAIL("forge");
    for (int i = 0; qs[i]; i++) {
        test_result_t r = sr_corrupt_twice("(.db.splayed.get \"" SR_TMP "/fpb/\")", qs[i], path);
        if (r.status != TEST_PASS) return r;
    }
    PASS();
}

/* Task 17b fix round 1: the split prescan over a forged directory.  A mixed
 * key set (SYM g scanned exactly, staged I64 k through the directory path
 * as a set of its own) whose k chunk 3 maximum (directory and descriptors)
 * is lowered below its values: `corrupt` naming k, twice, and no write past
 * a slab (ASan).  Unpatched, the answers are generation 0's. */
static test_result_t test_v2_forged_split(void) {
    sr_eval_ok("(set jf (til 262144))");
    sr_eval_ok("(set ja (til 196608))");
    sr_eval_ok("(set jb (+ 196608 (til 65536)))");
    sr_eval_ok("(set FS (table [k g v] (list (concat (% (* ja 7) 1000) (map (fn [j] (if (== 0 (% j 100)) 0N (% (* j 7) 5001))) jb)) "
               "(as 'SYM (as 'STR (% jf 3))) jf)))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/fs0/\" FS {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/fs2/\" FS {compress: true})");
    static const char* const qs[] = {
        "(select {s: (sum v) c: (count v) from: T where: (> v 0) by: [g k]})",
        "(select {s: (sum v) c: (count v) from: T where: (> v 0) by: [k g]})",
        NULL,
    };
    for (int i = 0; qs[i]; i++) {
        test_result_t r = sr_same_route("(.db.splayed.get \"" SR_TMP "/fs0/\")", "(.db.splayed.get \"" SR_TMP "/fs2/\")", qs[i]);
        if (r.status != TEST_PASS) return r;
        sr_eval_ok("(.db.splayed.set \"" SR_TMP "/fsb/\" FS {compress: true})");
        char dir[512], path[600];
        if (ray_splay_resolve_dir(ray_test_tmp(SR_TMP "/fsb/"), dir, sizeof dir) != RAY_OK) FAIL("resolve");
        snprintf(path, sizeof path, "%s/k", dir);
        if (!sr_forge_max(path, 3, 999)) FAIL("forge");
        r = sr_corrupt_twice("(.db.splayed.get \"" SR_TMP "/fsb/\")", qs[i], path);
        if (r.status != TEST_PASS) return r;
    }
    PASS();
}

/* The mirror of sr_forge_max on the minimum: chunk c's directory minimum
 * and every vector descriptor's minimum below x raised to x.  False on an
 * I/O error or a vector whose maximum lies below x. */
static bool sr_forge_min(const char* path, int64_t c, int64_t x) {
    FILE* f = fopen(path, "r+b"); if (!f) return false;
    ray_enc_dirent_t de;
    long doff = (long)(RAY_ENC_DIR_OFF + (uint64_t)c * sizeof de);
    bool ok = fseek(f, doff, SEEK_SET) == 0 && fread(&de, sizeof de, 1, f) == 1 && !(de.flags & RAY_ENC_STAT_ABSENT);
    if (ok) { de.min = x; ok = fseek(f, doff, SEEK_SET) == 0 && fwrite(&de, sizeof de, 1, f) == 1; }
    for (int j = 0; ok && j < RAY_ENC_VECS_PER_CHUNK; j++) {
        ray_enc_desc_t d;
        long off = (long)(de.offset + (uint64_t)j * sizeof d);
        ok = fseek(f, off, SEEK_SET) == 0 && fread(&d, sizeof d, 1, f) == 1;
        if (!ok || (d.encoding == 0 && d.payload_bytes == 0)) break;     /* past the last vector */
        if (d.flags & RAY_ENC_STAT_ABSENT || d.min >= x) continue;
        if (d.max < x) { ok = false; break; }
        d.min = x;
        ok = fseek(f, off, SEEK_SET) == 0 && fwrite(&d, sizeof d, 1, f) == 1;
    }
    return fclose(f) == 0 && ok;
}

/* Fix round 3: the serial dense path.  One chunk of 60000 rows, under
 * RAY_PARALLEL_THRESHOLD, so the group-by sizes its dense plan from the
 * directory and then takes the serial path, which reads the key's whole
 * materialization (agg_keys_unstage -> ray_col_src_plain).  Key k is
 * j * 7 % 5001 (FOR_BP); kn the same with a null every 100 rows.  The
 * chunk's statistics (directory and descriptors, still nested) are moved
 * inside the values: the maximum lowered to 999, or the minimum raised to
 * 4000.  Each query gives `corrupt` naming the key, twice, and never
 * indexes the slot map past the plan; unpatched it answers as generation
 * 0 on the serial dense route.  A buffered aggregate (med) is admitted to
 * v2 only as an indexed shape: its keys are unstaged before any plan, so
 * nothing is sized from the statistics and the forged table answers as
 * generation 0, small (one chunk) and large (four chunks, with a pool). */
static test_result_t test_v2_forged_serial_dense(void) {
    sr_eval_ok("(set js (til 60000))");
    sr_eval_ok("(set FS (table [k kn v] (list (% (* js 7) 5001) "
               "(map (fn [j] (if (== 0 (% j 100)) 0N (% (* j 7) 5001))) js) js)))");
    sr_eval_ok("(set jm (til 262144))");
    sr_eval_ok("(set FM (table [k v] (list (% (* jm 7) 5001) jm)))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/fs0/\" FS {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/fs2/\" FS {compress: true})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/fm0/\" FM {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/fm2/\" FM {compress: true})");
    static const struct { const char* tbl; const char* col; int64_t chunk; bool raise; int64_t x; int route; const char* q; } cs[] = {
        { "fs", "k",  0, false, 999,  AGG_ROUTE_V2_SERIAL_DENSE, "(select {s: (sum v) c: (count v) from: T by: k})" },
        { "fs", "kn", 0, false, 999,  AGG_ROUTE_V2_SERIAL_DENSE, "(select {s: (sum v) c: (count v) from: T by: kn})" },
        { "fs", "k",  0, true,  4000, AGG_ROUTE_V2_SERIAL_DENSE, "(select {s: (sum v) c: (count v) from: T by: k})" },
        { "fs", "k",  0, false, 999,  AGG_ROUTE_V2_SERIAL_DENSE, "(select {s: (sum v) c: (count v) from: T by: k take: 4})" },
        { "fs", "k",  0, false, 999,  AGG_ROUTE_V2_INDEXED,      "(select {m: (med v) c: (count v) from: T by: k})" },
        { "fm", "k",  2, false, 999,  AGG_ROUTE_V2_INDEXED,      "(select {m: (med v) c: (count v) from: T by: k})" },
    };
    for (size_t i = 0; i < sizeof cs / sizeof cs[0]; i++) {
        char t0[256], t2[256], tb[256], src[256];
        snprintf(t0, sizeof t0, "(.db.splayed.get \"" SR_TMP "/%s0/\")", cs[i].tbl);
        snprintf(t2, sizeof t2, "(.db.splayed.get \"" SR_TMP "/%s2/\")", cs[i].tbl);
        snprintf(tb, sizeof tb, "(.db.splayed.get \"" SR_TMP "/%sb/\")", cs[i].tbl);
        agg_route_reset();
        test_result_t r = sr_same_on(t0, t2, cs[i].q, UINT64_MAX);
        if (r.status != TEST_PASS) return r;
        TEST_ASSERT_FMT(agg_route_stats().routes[cs[i].route] > 0, "%s: not route %d", cs[i].q, cs[i].route);
        snprintf(src, sizeof src, "(.db.splayed.set \"" SR_TMP "/%sb/\" %s {compress: true})", cs[i].tbl,
                 strcmp(cs[i].tbl, "fs") == 0 ? "FS" : "FM");
        sr_eval_ok(src);
        char rel[256], dir[512], path[600];
        snprintf(rel, sizeof rel, SR_TMP "/%sb/", cs[i].tbl);
        if (ray_splay_resolve_dir(ray_test_tmp(rel), dir, sizeof dir) != RAY_OK) FAIL("resolve");
        snprintf(path, sizeof path, "%s/%s", dir, cs[i].col);
        if (!(cs[i].raise ? sr_forge_min(path, cs[i].chunk, cs[i].x) : sr_forge_max(path, cs[i].chunk, cs[i].x)))
            FAIL("forge");
        r = cs[i].route == AGG_ROUTE_V2_INDEXED ? sr_same_on(t0, tb, cs[i].q, UINT64_MAX)
                                                : sr_corrupt_twice(tb, cs[i].q, path);
        if (r.status != TEST_PASS) return r;
    }
    PASS();
}

static test_result_t test_v2_forged_dir_max(void) {
    sr_eval_ok("(set BF (table [k v] (list (concat (% (* (til 196608) 7) 1000) (% (* (+ 196608 (til 65536)) 7) 5001)) (til 262144))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/fm/\" BF {compress: true})");
    char dir[512], path[600];
    TEST_ASSERT_EQ_I(ray_splay_resolve_dir(ray_test_tmp(SR_TMP "/fm/"), dir, sizeof dir), RAY_OK);
    snprintf(path, sizeof path, "%s/k", dir);
    ray_enc_dirent_t de; memset(&de, 0, sizeof de);
    FILE* f = fopen(path, "rb");
    TEST_ASSERT_TRUE(f != NULL);
    TEST_ASSERT_TRUE(fseek(f, (long)(RAY_ENC_DIR_OFF + 3 * sizeof de), SEEK_SET) == 0 && fread(&de, sizeof de, 1, f) == 1);
    fclose(f);
    TEST_ASSERT_EQ_I(de.max, 5000);
    TEST_ASSERT_EQ_I(de.flags & RAY_ENC_STAT_ABSENT, 0);
    sr_patch64(path, (long)(RAY_ENC_DIR_OFF + 3 * sizeof de + offsetof(ray_enc_dirent_t, max)), 999);
    static const char* const qs[] = {
        "(count (select {s: (sum v) from: T where: (> v 0) by: k}))",
        "(count (select {s: (sum v) from: T by: k}))",
        NULL };
    for (int i = 0; qs[i]; i++)
        for (int rep = 0; rep < 2; rep++) {
            sr_eval_ok("(set T (.db.splayed.get \"" SR_TMP "/fm/\"))");
            ray_error_clear();
            ray_t* r = sr_eval(qs[i]);
            TEST_ASSERT_FMT(r && RAY_IS_ERR(r), "%s (read %d): answered", qs[i], rep + 1);
            TEST_ASSERT_FMT(strcmp(ray_err_code(r), "corrupt") == 0 && ray_error_msg() && strstr(ray_error_msg(), path),
                            "%s (read %d): %s \"%s\"", qs[i], rep + 1, ray_err_code(r), ray_error_msg() ? ray_error_msg() : "");
            ray_error_free(r);
        }
    PASS();
}

/* The selected rows' key bounds of every key width (I64, I32 with nulls
 * and negatives, I16 with negatives, DATE, BOOL, U8, SYM): a filtered group-by answers as the same
 * group-by over the filtered table (both ordered by the key: the two
 * routes emit groups in different orders), in both generations. */
static test_result_t test_v2_sel_key_types(void) {
    sr_eval_ok("(set KT (table [a n g d b u s h] (list (+ 5000000000 (% (* i 13) 500)) "
               "(as 'I32 (map (fn [j] (if (== 0 (% j 10)) 0N (+ 70000 (% j 300)))) i)) (as 'I16 (- (% i 7) 3)) "
               "(as 'DATE (+ 20000 (% i 900))) (== 0 (% i 3)) (as 'U8 (+ 50 (% i 200))) (take ['a 'b 'c 'd 'e] 200000) "
               "(% (* i 7919) 1000003))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/kt2/\" KT {compress: true})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/kt0/\" KT {compress: false})");
    static const char* const keys[] = { "a", "n", "g", "d", "b", "u", "s", NULL };
    for (int gen = 0; gen < 2; gen++) {
        sr_evalf_ok("(set T (.db.splayed.get \"%s\"))", gen ? SR_TMP "/kt2/" : SR_TMP "/kt0/");
        for (int i = 0; keys[i]; i++) {
            char a[256], b[256];
            snprintf(a, sizeof a, "(select {c: (count h) s: (sum h) from: T where: (> h 500000) by: %s asc: %s})", keys[i], keys[i]);
            snprintf(b, sizeof b, "(select {c: (count h) s: (sum h) from: (select {from: T where: (> h 500000)}) by: %s asc: %s})",
                     keys[i], keys[i]);
            agg_route_reset();
            ray_t* ra = sr_eval(a);
            agg_route_stats_t sa = agg_route_stats();
            agg_route_reset();
            ray_t* rb = sr_eval(b);
            agg_route_stats_t sb = agg_route_stats();
            TEST_ASSERT_FMT(sr_same_text(ra, rb), "gen %d: %s differs from the filtered table's", gen * 2, a);
            TEST_ASSERT_FMT(sa.dense_plan_available && sb.dense_plan_available, "gen %d: %s: no dense plan", gen * 2, a);
            /* the unfiltered plan sizes a SYM key from its domain, not its ids */
            TEST_ASSERT_FMT(i == 6 || sa.dense_local_slots == sb.dense_local_slots, "gen %d: %s: %llu slots, the filtered table's %llu",
                            gen * 2, a, (unsigned long long)sa.dense_local_slots, (unsigned long long)sb.dense_local_slots);
            ray_release(ra); ray_release(rb);
        }
    }
    PASS();
}

/* The pool this test resized is restored even when an assert fails first
 * (finding M11; test/test_parquet.c:262 resizes the pool the same way). */
static void srr_pool_teardown(void) { ray_pool_destroy(); (void)ray_pool_init_total(0); srr_teardown(); }

/* Review focus 2: a SYM key over two partitions of different width classes. */
static test_result_t test_v2_sym_key_parted(void) {
    for (int gen = 0; gen <= 2; gen += 2)
        for (int p = 0; p < 2; p++)
            sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/sp%d/2024.01.0%d/t/\" "
                        "(table [s v] (list (as 'SYMBOL (map (fn [j] (format \"w%%\" (%% j %d))) (til 70000))) (til 70000))) "
                        "\"" SR_TMP "/sp%d/.sym\" {compress: %s})", gen, p + 1, p ? 300 : 20, gen, gen ? "true" : "false");
    ray_t* a = sr_eval("(select {c: (count v) s: (sum v) from: (.db.parted.get \"" SR_TMP "/sp0\" 't) by: s})");
    ray_enc_counters_reset();
    ray_t* b = sr_eval("(select {c: (count v) s: (sum v) from: (.db.parted.get \"" SR_TMP "/sp2\" 't) by: s})");
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_TRUE(a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b));
    ray_t* fa = ray_fmt(a, 0); ray_t* fb = ray_fmt(b, 0);
    TEST_ASSERT_TRUE(ray_str_len(fa) == ray_str_len(fb) && memcmp(ray_str_ptr(fa), ray_str_ptr(fb), ray_str_len(fa)) == 0);
    TEST_ASSERT_EQ_U(k.materialized, 0);   /* the cardinality sample reads segment 0 of s by range */
    TEST_ASSERT_EQ_I(ray_table_nrows(b), 300);
    ray_release(fa); ray_release(fb); ray_release(a); ray_release(b);
    PASS();
}

/* Findings I5, I6, I8: parted group-bys on encoded segments go
 * per-partition (each sub-table holds encoded segment slots); the partition
 * key keeps today's path; a ragged generation-0 table answers as its
 * explicit-null reference.  Nothing materializes. */
static ray_t* sr_q(const char* q, uint64_t* mat) {
    ray_enc_counters_reset();
    ray_t* r = sr_eval(q);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    if (mat) *mat = k.materialized;
    return r;
}
static bool sr_same_text(ray_t* a, ray_t* b) {
    if (!a || !b || RAY_IS_ERR(a) || RAY_IS_ERR(b)) return false;
    ray_t* fa = ray_fmt(a, 0); ray_t* fb = ray_fmt(b, 0);
    bool same = ray_str_len(fa) == ray_str_len(fb) && memcmp(ray_str_ptr(fa), ray_str_ptr(fb), ray_str_len(fa)) == 0;
    ray_release(fa); ray_release(fb);
    return same;
}
static test_result_t test_v2_parted_routes(void) {
    sr_parted_xy(SR_TMP "/vp2", 7, false, false);          /* every partition generation 2 */
    sr_parted_xy(SR_TMP "/vp0", 0, false, false);
    sr_parted_xy(SR_TMP "/vrg", 0, true, false);
    sr_parted_xy(SR_TMP "/vrf", 0, false, true);
    static const char* const q[] = {
        "(select {s: (sum x) c: (count x) from: (.db.parted.get \"%s\" 't) by: y})",   /* low cardinality: per partition */
        "(select {s: (sum x) from: (.db.parted.get \"%s\" 't) by: date})",            /* the partition key (MAPCOMMON) */
        "(select {m: (max x) from: (.db.parted.get \"%s\" 't) where: (> x 100) by: y})",
        NULL,
    };
    for (int i = 0; q[i]; i++) {
        char a[512], b[512];
        snprintf(a, sizeof a, q[i], SR_TMP "/vp0"); snprintf(b, sizeof b, q[i], SR_TMP "/vp2");
        int64_t pp0 = ray_group_perpart_runs();
        uint64_t m = 9;
        ray_t* r0 = sr_q(a, NULL); ray_t* r2 = sr_q(b, &m);
        TEST_ASSERT_FMT(sr_same_text(r0, r2), "%s", q[i]);
        /* exec_group_parted's cardinality sample reads the key's segment 0
         * by range; the per-partition v2 runs materialize nothing. */
        TEST_ASSERT_FMT(m == 0, "%s: %llu materializations", q[i], (unsigned long long)m);
        if (i != 1) TEST_ASSERT_TRUE(ray_group_perpart_runs() - pp0 >= 2);  /* both generations per partition */
        sr_drop(r0); sr_drop(r2);
        snprintf(a, sizeof a, q[i], SR_TMP "/vrf"); snprintf(b, sizeof b, q[i], SR_TMP "/vrg");
        r0 = sr_q(a, NULL); r2 = sr_q(b, &m);
        TEST_ASSERT_FMT(sr_same_text(r0, r2), "ragged: %s", q[i]);
        TEST_ASSERT_EQ_U(m, 0);
        sr_drop(r0); sr_drop(r2);
    }
    PASS();
}

/* Task 14: a 3-partition table with a high-cardinality key (u: unique per
 * row) and low ones (m; g, whose second value first appears at row 3000,
 * past the first vector and its scratch), generation 0 and generation 2. */
static void sr_parted_keys(const char* root, int gen) {
    for (int p = 0; p < 3; p++)
        sr_evalf_ok("(.db.splayed.set \"%s/2024.01.0%d/t/\" "
                    "(table [u m x f g] (list (+ %d (til 70000)) (%% (til 70000) 5) (til 70000) (as 'F64 (%% (til 70000) 13)) "
                    "(div (til 70000) 3000))) {compress: %s})",
                    root, p + 1, p * 70000, gen ? "true" : "false");
}
static test_result_t test_parted_concat_in_place(void) {
    sr_parted_keys(SR_TMP "/kc0", 0);
    sr_parted_keys(SR_TMP "/kc2", 2);
    static const char* const q[] = {
        "(select {u: u c: (count u) from: (.db.parted.get \"%s\" 't) by: u desc: c take: 10})",   /* q16's shape */
        "(select {s: (sum x) a: (avg f) from: (.db.parted.get \"%s\" 't) by: u})",
        "(select {c: (count x) from: (.db.parted.get \"%s\" 't) by: {u: u m: m}})",
        NULL,
    };
    for (int i = 0; q[i]; i++) {
        char a[512], b[512];
        snprintf(a, sizeof a, q[i], SR_TMP "/kc0"); snprintf(b, sizeof b, q[i], SR_TMP "/kc2");
        int64_t s0 = ray_group_segstream_runs();
        uint64_t m = 9;
        ray_t* r0 = sr_q(a, NULL);
        TEST_ASSERT_FMT(ray_group_segstream_runs() - s0 == 1, "gen 0 in place: %s", q[i]);
        ray_t* r2 = sr_q(b, &m);
        TEST_ASSERT_FMT(ray_group_segstream_runs() - s0 == 2, "gen 2 in place: %s", q[i]);
        TEST_ASSERT_FMT(sr_same_text(r0, r2), "%s", q[i]);
        TEST_ASSERT_FMT(m == 0, "%s: %llu materializations", q[i], (unsigned long long)m);
        sr_drop(r0); sr_drop(r2);
    }
    PASS();
}

/* The same answers as the flatten: the in-place route is checked against
 * the flat route forced by an expression aggregate the in-place concat
 * does not take (z), projected away.  A where clause would not force it: a
 * parted table's filter hands the group a filtered parted table. */
static test_result_t test_parted_concat_matches_flat(void) {
    sr_parted_keys(SR_TMP "/kf0", 0);
    int64_t s0 = ray_group_segstream_runs();
    ray_t* a = sr_eval("(select {s: (sum x) a: (avg f) mn: (min m) from: (.db.parted.get \"" SR_TMP "/kf0\" 't) by: u})");
    TEST_ASSERT_TRUE(ray_group_segstream_runs() - s0 == 1);
    ray_t* b = sr_eval("(select {u: u s: s a: a mn: mn from: (select {s: (sum x) a: (avg f) mn: (min m) z: (sum (+ x 0)) "
                       "from: (.db.parted.get \"" SR_TMP "/kf0\" 't) by: u})})");
    TEST_ASSERT_TRUE(ray_group_segstream_runs() - s0 == 1);
    TEST_ASSERT_TRUE(sr_same_text(a, b));
    sr_drop(a); sr_drop(b);
    PASS();
}

/* Sample and first-key reads of generation-2 segments never materialize. */
static test_result_t test_parted_sample_reads(void) {
    sr_parted_keys(SR_TMP "/ks0", 0);
    sr_parted_keys(SR_TMP "/ks2", 2);
    static const char* const q[] = {
        "(select {c: (count x) from: (.db.parted.get \"%s\" 't) by: m})",            /* sample: low cardinality */
        "(select {c: (count x) from: (.db.parted.get \"%s\" 't) by: u take: 5})",    /* unordered take: first keys */
        "(select {c: (count x) from: (.db.parted.get \"%s\" 't) by: m take: 3})",    /* per partition: grp_first_keys_scan */
        "(select {c: (count x) from: (.db.parted.get \"%s\" 't) by: g take: 3})",    /* first keys past the first vector */
        NULL,
    };
    for (int i = 0; q[i]; i++) {
        char a[512], b[512];
        snprintf(a, sizeof a, q[i], SR_TMP "/ks0"); snprintf(b, sizeof b, q[i], SR_TMP "/ks2");
        uint64_t m = 9;
        int64_t s0 = ray_group_segstream_runs(), f0 = ray_group_firstkeys_runs();
        ray_t* r0 = sr_q(a, NULL); ray_t* r2 = sr_q(b, &m);
        /* low cardinality goes per partition, not in place; the unordered
         * take keeps the flatten on generation 0 and runs in place over
         * generation 2's keys (its first-n passes read the parted sources,
         * P3 Task 6b) */
        TEST_ASSERT_FMT(ray_group_segstream_runs() - s0 == (i == 1 ? 1 : 0), "in place: %s", q[i]);
        /* the per-partition takes keep the first keys read up front (a wrong
         * read declines to the concat: same answer, so the route is pinned) */
        TEST_ASSERT_FMT(ray_group_firstkeys_runs() - f0 == (i >= 2 ? 2 : 0), "first keys: %s", q[i]);
        TEST_ASSERT_FMT(sr_same_text(r0, r2), "%s", q[i]);
        TEST_ASSERT_FMT(m == 0, "%s: %llu materializations", q[i], (unsigned long long)m);
        sr_drop(r0); sr_drop(r2);
    }
    PASS();
}

/* The strategies that read keys or inputs at stored rows or whole (R-RANDOM)
 * run an in-place concat over a flat copy of the table read by range: the
 * indexed route (first / last) and the serial route (a small table).
 * Answers equal the flatten's (forced by an expression aggregate, projected
 * away); nothing materializes. */
static void sr_parted_small(const char* root, int gen) {
    for (int p = 0; p < 3; p++)
        sr_evalf_ok("(.db.splayed.set \"%s/2024.01.0%d/t/\" "
                    "(table [u m x f] (list (+ %d (til 3000)) (%% (til 3000) 5) (til 3000) (as 'F64 (%% (til 3000) 13)))) {compress: %s})",
                    root, p + 1, p * 3000, gen ? "true" : "false");
}
static test_result_t test_parted_in_place_random(void) {
    sr_parted_keys(SR_TMP "/kr0", 0);
    sr_parted_keys(SR_TMP "/kr2", 2);
    sr_parted_small(SR_TMP "/kq0", 0);
    sr_parted_small(SR_TMP "/kq2", 2);
    static const struct { const char* root; const char* q; const char* flat; agg_route_t route; } c[] = {
        { "/kr", "(select {a: (first x) b: (last f) from: (.db.parted.get \"" SR_TMP "%s%d\" 't) by: u})",
          "(select {u: u a: a b: b from: (select {a: (first x) b: (last f) z: (sum (+ x 0)) from: (.db.parted.get \"" SR_TMP "%s%d\" 't) by: u})})",
          AGG_ROUTE_V2_INDEXED },
        { "/kq", "(select {s: (sum x) a: (avg f) from: (.db.parted.get \"" SR_TMP "%s%d\" 't) by: u})",
          "(select {u: u s: s a: a from: (select {s: (sum x) a: (avg f) z: (sum (+ x 0)) from: (.db.parted.get \"" SR_TMP "%s%d\" 't) by: u})})",
          AGG_ROUTE_V2_SERIAL_DENSE },
    };
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        char a[600], b[600], f[600];
        snprintf(a, sizeof a, c[i].q, c[i].root, 0); snprintf(b, sizeof b, c[i].q, c[i].root, 2);
        snprintf(f, sizeof f, c[i].flat, c[i].root, 0);
        int64_t s0 = ray_group_segstream_runs();
        uint64_t m = 9;
        agg_route_reset();
        ray_t* r0 = sr_q(a, NULL);
        agg_route_stats_t st = agg_route_stats();
        TEST_ASSERT_FMT(st.routes[c[i].route] >= 1, "route: %s", a);
        ray_t* r2 = sr_q(b, &m);
        TEST_ASSERT_FMT(ray_group_segstream_runs() - s0 == 2, "in place: %s", a);
        ray_t* rf = sr_eval(f);
        TEST_ASSERT_FMT(ray_group_segstream_runs() - s0 == 2, "flattened: %s", f);
        TEST_ASSERT_FMT(sr_same_text(r0, r2), "%s", a);
        TEST_ASSERT_FMT(sr_same_text(r0, rf), "flatten: %s", a);
        TEST_ASSERT_FMT(m == 0, "%s: %llu materializations", b, (unsigned long long)m);
        sr_drop(r0); sr_drop(r2); sr_drop(rf);
    }
    PASS();
}

/* Fix round 1, I1: a factorized-expand shaped parted table (_src key,
 * _count weights).  Over the flatten, v2 declines the shape (its _count
 * probe) and exec_group's shortcut answers from _count; the in-place
 * concat must decline it too.  The shortcut's answer: one row per _src in
 * table order, every aggregate the row's _count. */
static test_result_t test_parted_factorized_count(void) {
    for (int p = 0; p < 3; p++)
        sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/fc0/2024.01.0%d/t/\" "
                    "(table [_src _count v] (list (+ %d (til 50000)) (+ 1 (%% (til 50000) 3)) (til 50000))) {compress: false})",
                    p + 1, p * 50000);
    sr_eval_ok("(set P (.db.parted.get \"" SR_TMP "/fc0\" 't))");
    static const char* const q[] = {
        "(select {c: (count v) s: (sum _count) from: P by: _src})",
        "(select {c: (count _count) from: P by: _src})",
        NULL,
    };
    for (int i = 0; q[i]; i++) {
        int64_t s0 = ray_group_segstream_runs();
        char b[256];
        snprintf(b, sizeof b, "(set R %s)", q[i]);
        sr_eval_ok(b);
        TEST_ASSERT_FMT(ray_group_segstream_runs() == s0, "not in place: %s", q[i]);
        ray_t* a = sr_eval("(at R 'c)");
        ray_t* w = sr_eval("(at P '_count)");
        TEST_ASSERT_FMT(sr_same_text(a, w), "%s: c is the row's _count", q[i]);
        ray_t* k = sr_eval("(at R '_src)");
        ray_t* t = sr_eval("(til 150000)");
        TEST_ASSERT_FMT(sr_same_text(k, t), "%s: table order", q[i]);
        ray_t* n = sr_eval("(sum (at R 'c))");
        TEST_ASSERT_FMT(n && !RAY_IS_ERR(n) && n->i64 == 299997, "%s: weighted count", q[i]);
        sr_drop(a); sr_drop(w); sr_drop(k); sr_drop(t); sr_drop(n);
    }
    PASS();
}

/* Fix round 1, I2: the cardinality sample partition is the narrowest (250
 * symbols: W8) and a later one wider (1000 and 5000 symbols: W16): the
 * stand-in, the dense plan and the flat copy take the widest segment's
 * width, not the first's. */
static test_result_t test_parted_sym_narrow_first(void) {
    static const int nsym[3] = { 250, 1000, 5000 };
    for (int gen = 0; gen <= 2; gen += 2)
        for (int p = 0; p < 3; p++)
            sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/sn%d/2024.01.0%d/t/\" "
                        "(table [s u v] (list (as 'SYMBOL (map (fn [j] (format \"n%%\" (%% j %d))) (til 22000))) (+ %d (til 22000)) (til 22000))) "
                        "\"" SR_TMP "/sn%d/.sym\" {compress: %s})", gen, p + 1, nsym[p], p * 22000, gen, gen ? "true" : "false");
    static const char* const q[] = {
        "(select {c: (count v) t: (sum v) from: (.db.parted.get \"" SR_TMP "/sn%d\" 't) by: s})",
        "(select {c: (count v) t: (sum v) from: (.db.parted.get \"" SR_TMP "/sn%d\" 't) by: {s: s u: u}})",
        NULL,
    };
    static const char* const fq[] = {
        "(select {s: s c: c t: t from: (select {c: (count v) t: (sum v) z: (sum (+ v 0)) from: (.db.parted.get \"" SR_TMP "/sn0\" 't) by: s})})",
        "(select {s: s u: u c: c t: t from: (select {c: (count v) t: (sum v) z: (sum (+ v 0)) from: (.db.parted.get \"" SR_TMP "/sn0\" 't) by: {s: s u: u}})})",
    };
    for (int i = 0; q[i]; i++) {
        char a[512], b[512];
        snprintf(a, sizeof a, q[i], 0); snprintf(b, sizeof b, q[i], 2);
        int64_t s0 = ray_group_segstream_runs();
        uint64_t m = 9;
        ray_t* r0 = sr_q(a, NULL); ray_t* r2 = sr_q(b, &m);
        TEST_ASSERT_FMT(ray_group_segstream_runs() - s0 == 2, "in place: %s", q[i]);
        ray_t* rf = sr_eval(fq[i]);
        TEST_ASSERT_FMT(ray_group_segstream_runs() - s0 == 2, "flattened: %s", fq[i]);
        TEST_ASSERT_FMT(sr_same_text(r0, r2), "%s", q[i]);
        TEST_ASSERT_FMT(sr_same_text(r0, rf), "flatten: %s", q[i]);
        TEST_ASSERT_FMT(m == 0, "%s: %llu materializations", q[i], (unsigned long long)m);
        TEST_ASSERT_EQ_I(ray_table_nrows(r0), i ? 66000 : 5000);
        sr_drop(r0); sr_drop(r2); sr_drop(rf);
    }
    PASS();
}

/* Fix round 1, minor 3: nulls in a flattened and an in-place input keep
 * HAS_NULLS (a sum skips them).  u pairs rows; f is null every 7th row.
 * The reference is the same rows grouped as one in-memory table. */
static test_result_t test_parted_flatten_nulls(void) {
    for (int p = 0; p < 3; p++)
        sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/fn0/2024.01.0%d/t/\" "
                    "(table [u x f] (list (div (+ %d (til 30000)) 2) (til 30000) "
                    "(as 'F64 (map (fn [j] (if (== 0 (%% j 7)) 0N j)) (til 30000))))) {compress: false})",
                    p + 1, p * 30000);
    sr_eval_ok("(set M (table [u x f] (list (div (til 90000) 2) (% (til 90000) 30000) "
               "(as 'F64 (map (fn [j] (if (== 0 (% (% j 30000) 7)) 0N (% j 30000))) (til 90000))))))");
    ray_t* ref = sr_eval("(select {s: (sum f) from: M by: u})");
    int64_t s0 = ray_group_segstream_runs();
    ray_t* in = sr_eval("(select {s: (sum f) from: (.db.parted.get \"" SR_TMP "/fn0\" 't) by: u})");
    TEST_ASSERT_TRUE(ray_group_segstream_runs() - s0 == 1);
    ray_t* fl = sr_eval("(select {u: u s: s from: (select {s: (sum f) z: (sum (+ x 0)) from: (.db.parted.get \"" SR_TMP "/fn0\" 't) by: u})})");
    TEST_ASSERT_TRUE(ray_group_segstream_runs() - s0 == 1);
    TEST_ASSERT_TRUE(sr_same_text(ref, in));
    TEST_ASSERT_TRUE(sr_same_text(ref, fl));
    sr_drop(ref); sr_drop(in); sr_drop(fl);
    PASS();
}

/* Carry from Task 13: a SYM key over partitions of different width
 * classes, grouped in place.  Partition 2 is saved first (20 symbols: W8),
 * partition 1 after it over 1000 symbols (W16); partition 1 is the
 * cardinality sample (1000 groups over 20000 rows: the concat). */
static test_result_t test_parted_sym_widths_in_place(void) {
    for (int gen = 0; gen <= 2; gen += 2)
        for (int p = 1; p >= 0; p--)
            sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/sw%d/2024.01.0%d/t/\" "
                        "(table [s v] (list (as 'SYMBOL (map (fn [j] (format \"q%%\" (%% j %d))) (til 70000))) (til 70000))) "
                        "\"" SR_TMP "/sw%d/.sym\" {compress: %s})", gen, p + 1, p ? 20 : 1000, gen, gen ? "true" : "false");
    static const char* const q[] = {
        "(select {c: (count v) t: (sum v) from: (.db.parted.get \"" SR_TMP "/sw%d\" 't) by: s})",
        "(select {c: (count v) t: (sum v) from: (.db.parted.get \"" SR_TMP "/sw%d\" 't) by: s desc: t take: 7})",   /* sums are distinct: no ties */
        NULL,
    };
    for (int i = 0; q[i]; i++) {
        char a[512], b[512];
        snprintf(a, sizeof a, q[i], 0); snprintf(b, sizeof b, q[i], 2);
        int64_t s0 = ray_group_segstream_runs();
        uint64_t m = 9;
        ray_t* r0 = sr_q(a, NULL); ray_t* r2 = sr_q(b, &m);
        TEST_ASSERT_FMT(ray_group_segstream_runs() - s0 == 2, "in place: %s", q[i]);
        TEST_ASSERT_FMT(sr_same_text(r0, r2), "%s", q[i]);
        TEST_ASSERT_FMT(m == 0, "%s: %llu materializations", q[i], (unsigned long long)m);
        TEST_ASSERT_EQ_I(ray_table_nrows(r0), i ? 7 : 1000);
        /* the flatten (forced by an expression aggregate) answers the same */
        char f[600];
        snprintf(f, sizeof f, "(select {s: s c: c t: t from: (select {c: (count v) t: (sum v) z: (sum (+ v 0)) "
                 "from: (.db.parted.get \"" SR_TMP "/sw0\" 't) by: s%s})})", i ? " desc: t take: 7" : "");
        s0 = ray_group_segstream_runs();
        ray_t* rf = sr_eval(f);
        TEST_ASSERT_FMT(ray_group_segstream_runs() == s0, "flattened: %s", f);
        TEST_ASSERT_FMT(sr_same_text(r0, rf), "flatten: %s", q[i]);
        sr_drop(r0); sr_drop(r2); sr_drop(rf);
    }
    PASS();
}

/* Review focus 4: a corrupt chunk under a group-by. */
static test_result_t test_v2_error(void) {
    char path[600];
    sr_make_bad("v", path, sizeof path);
    ray_error_clear();
    ray_t* r = sr_eval("(select {s: (sum v) from: (.db.splayed.get \"" SR_TMP "/bad/\") by: g})");
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
    TEST_ASSERT_TRUE(strstr(ray_error_msg(), path) != NULL);
    ray_error_free(r);
    PASS();
}

/* Table W: a column for every v2 strategy's shape (200000 rows).
 *   sk  skewed key, 200000-slot range, mostly 0 (shared dense: max n)
 *   t   1000 sparse groups (table stack)      h   near-unique (radix)
 *   c1, c2  composite whose raw range product overflows (compacted dense)
 *   nk  nullable I32 key of 50 values           f   nullable F64 with fractions */
static void sr_w_tables(void) {
    sr_eval_ok("(set W (table [k g s f n sk t c1 c2 nk h d u] (list i (as 'I16 (% i 7)) "
               "(take ['a 'b 'c 'd 'e] 200000) "
               "(as 'F64 (map (fn [j] (if (== 0 (% j 13)) 0N (* j 0.37))) i)) (as 'I32 (- 0 i)) "
               "(map (fn [j] (if (== 0 (% j 100)) j 0)) i) (* (% i 1000) 1000003) "
               "(* (% i 100) 1000) (* (% i 50) 1000) "
               "(as 'I32 (map (fn [j] (if (== 0 (% j 10)) 0N (% j 50))) i)) (% (* i 7919) 1000003) "
               "(as 'DATE (% i 900)) (as 'U8 (% i 200)))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/w0/\" W {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/w2/\" W {compress: true})");
}
#define SR_W0 "(.db.splayed.get \"" SR_TMP "/w0/\")"
#define SR_W2 "(.db.splayed.get \"" SR_TMP "/w2/\")"

/* The F64 column `col` of q's result, generation 0 against 2: equal bits. */
static test_result_t sr_f64_bits_same(const char* q, const char* col) {
    ray_t* out[2];
    for (int gen = 0; gen < 2; gen++) {
        sr_evalf_ok("(set T %s)", gen ? SR_W2 : SR_W0);
        out[gen] = sr_eval(q);
        TEST_ASSERT_FMT(out[gen] && !RAY_IS_ERR(out[gen]), "gen %d: %s", gen, q);
    }
    ray_t* a = ray_table_get_col(out[0], ray_sym_intern(col, strlen(col)));
    ray_t* b = ray_table_get_col(out[1], ray_sym_intern(col, strlen(col)));
    TEST_ASSERT_TRUE(a && b && a->type == RAY_F64 && b->type == RAY_F64 && a->len == b->len && a->len > 0);
    TEST_ASSERT_FMT(memcmp(ray_data(a), ray_data(b), (size_t)a->len * 8) == 0, "%s: %s bits differ", q, col);
    ray_release(out[0]); ray_release(out[1]);
    PASS();
}

/* Every v2 strategy over staged keys and inputs: equal answers, nothing
 * materialized, and the strategy the line names is the one that ran. */
static test_result_t test_v2_route_shapes(void) {
    sr_w_tables();
    /* The strategy choice weighs replicated slabs against partition traffic
     * per worker (1 worker: no partitions; 8: {nk s} partitions), so the
     * routes are pinned at 3 workers; srr_pool_teardown restores the pool. */
    ray_pool_destroy(); TEST_ASSERT_EQ_I(ray_pool_init_total(3), RAY_OK);
    enum { R_LOCAL = 1, R_PART, R_SHARED, R_RADIX, R_TS, R_SH };
    static const struct { const char* q; int route; } qs[] = {
        { "(select {mx: (max n) from: T by: sk})", R_SHARED },
        { "(select {c: (count k) s: (sum f) a: (avg g) from: T by: t})", R_TS },
        { "(select {c: (count k) mx: (max f) from: T where: (> k 1000) by: t})", R_TS },
        { "(select {c: (count k) s: (sum g) from: T where: (> k 1000) by: h})", R_RADIX },
        { "(select {p: (pearson_corr f g) c: (count k) from: T by: {h: h g: g}})", R_RADIX },
        { "(select {c: (count k) s: (sum f) from: T by: {c1: c1 c2: c2}})", R_LOCAL },
        { "(select {c: (count k) s: (sum f) mn: (min n) from: T by: nk})", R_LOCAL },
        { "(select {s: (sum f) v: (var n) from: T where: (> g 2) by: s})", R_LOCAL },
        { "(select {x: (max f) c: (count k) from: T where: (> k 1000) by: {d: d u: u}})", R_PART },
        { "(select {x: (sum f) p: (pearson_corr f n) from: T by: {d: d u: u}})", R_PART },
        { "(select {c: (count k) a: (avg nk) from: T by: {nk: nk s: s}})", R_LOCAL },
        { "(select {p: (pearson_corr f g) w: (wavg g f) from: T by: g})", R_LOCAL },    /* binary inputs */
    };
    for (size_t i = 0; i < sizeof qs / sizeof qs[0]; i++) {
        agg_route_reset();
        test_result_t r = sr_same_on(SR_W0, SR_W2, qs[i].q, 0);
        if (r.status != TEST_PASS) return r;
        agg_route_stats_t rs = agg_route_stats();   /* the generation-2 run */
        bool ok = false;
        switch (qs[i].route) {
            case R_LOCAL:  ok = rs.routes[AGG_ROUTE_V2_DENSE] && rs.dense_strategy == AGG_DENSE_TASK_LOCAL; break;
            case R_PART:   ok = rs.routes[AGG_ROUTE_V2_DENSE] && rs.dense_strategy == AGG_DENSE_PARTITIONED; break;
            case R_SHARED: ok = rs.routes[AGG_ROUTE_V2_DENSE] && rs.dense_strategy == AGG_DENSE_SHARED; break;
            case R_RADIX:  ok = rs.routes[AGG_ROUTE_V2_RADIX] > 0; break;
            case R_TS:     ok = rs.routes[AGG_ROUTE_V2_TABLESTACK] > 0; break;
        }
        TEST_ASSERT_FMT(ok, "%s: not on route %d (dense strategy %d)", qs[i].q, qs[i].route, (int)rs.dense_strategy);
    }
    /* the small hash (the radix strategy's allocation fallback), forced:
     * staged keys are grouped by key words, merged and emitted from them.
     * Its group order follows the workers' ranges, so the lines sort. */
    static const char* const qsh[] = {
        "(select {from: (select {c: (count k) s: (sum f) from: T by: t}) asc: t})",
        "(select {from: (select {c: (count k) mx: (max f) from: T where: (> k 1000) by: {t: t s: s}}) asc: t})",
        "(select {from: (select {c: (count k) s: (sum g) from: T by: {nk: nk h: h}}) asc: h})",
        NULL };
    agg_test_force_smallhash(true);
    for (int i = 0; qsh[i]; i++) {
        agg_route_reset();
        test_result_t r = sr_same_on(SR_W0, SR_W2, qsh[i], 0);
        if (r.status != TEST_PASS) { agg_test_force_smallhash(false); return r; }
        TEST_ASSERT_FMT(agg_route_stats().routes[AGG_ROUTE_V2_SMALLHASH] > 0, "%s: not small hash", qsh[i]);
    }
    agg_test_force_smallhash(false);
    /* random strategies take plain columns: the serial path (below the
     * parallel threshold), the indexed driver (median), first-N */
    sr_eval_ok("(set W5 (take W 50000))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/w50/\" W5 {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/w52/\" W5 {compress: true})");
    test_result_t r = sr_same_on("(.db.splayed.get \"" SR_TMP "/w50/\")", "(.db.splayed.get \"" SR_TMP "/w52/\")",
                                 "(select {c: (count k) s: (sum f) from: T by: {g: g s: s}})", UINT64_MAX);
    if (r.status != TEST_PASS) return r;
    if ((r = sr_same_on(SR_W0, SR_W2, "(select {m: (med f) c: (count k) from: T by: g})", UINT64_MAX)).status != TEST_PASS) return r;
    if ((r = sr_same_on(SR_W0, SR_W2, "(select {c: (count k) s: (sum n) from: T by: h take: 5})", UINT64_MAX)).status != TEST_PASS) return r;
    /* k materialized before the query: read in place beside staged inputs */
    sr_eval_ok("(set W2M " SR_W2 ")");
    ray_t* t = sr_eval("W2M");
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_t* tcol = ray_table_col(t, ray_sym_intern("t", 1));
    TEST_ASSERT_TRUE(tcol && !RAY_IS_ERR(tcol));
    sr_drop(t);
    if ((r = sr_same_on(SR_W0, "W2M", "(select {c: (count k) s: (sum f) from: T by: t})", 0)).status != TEST_PASS) return r;
    PASS();
}

/* A SYM key over a shared symfile larger than the table: its codes are
 * positions in a 400000-symbol domain, so the dense plan bounds the key from
 * the directory statistics.  Those exclude the null rows, whose code is 0,
 * far below the used codes [90000, 139000]: the bounds must still span it
 * (139001 slots, within the row count, so the plan stays dense). */
static test_result_t test_v2_sym_shared_dom(void) {
    sr_eval_ok("(set A (table [s v] (list (as 'SYMBOL (map (fn [j] (format \"y%\" j)) (til 400000))) (til 400000))))");
    sr_eval_ok("(set B (table [s v] (list (as 'SYMBOL (map (fn [j] (if (== 0 (% j 11)) \"\" "
               "(format \"y%\" (+ 90000 (* 1000 (% j 50)))))) (til 150000))) (til 150000))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/sda/\" A \"" SR_TMP "/sd.sym\" {compress: true})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/sd0/\" B \"" SR_TMP "/sd.sym\" {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/sd2/\" B \"" SR_TMP "/sd.sym\" {compress: true})");
    ray_pool_destroy(); TEST_ASSERT_EQ_I(ray_pool_init_total(3), RAY_OK);   /* dense at any core count */
    static const char* const qs[] = {
        "(select {c: (count v) s: (sum v) from: T by: s})",
        "(select {c: (count v) mx: (max v) from: T where: (> v 1000) by: s})",
        NULL };
    for (int i = 0; qs[i]; i++) {
        agg_route_reset();
        test_result_t r = sr_same_on("(.db.splayed.get \"" SR_TMP "/sd0/\" \"" SR_TMP "/sd.sym\")",
                                     "(.db.splayed.get \"" SR_TMP "/sd2/\" \"" SR_TMP "/sd.sym\")", qs[i], 0);
        if (r.status != TEST_PASS) return r;
        agg_route_stats_t rs = agg_route_stats();
        TEST_ASSERT_FMT(rs.routes[AGG_ROUTE_V2_DENSE] > 0, "%s: not dense (plan %d)", qs[i], (int)rs.dense_plan_available);
        ray_t* out = sr_eval(qs[i]);   /* T is the generation-2 table */
        TEST_ASSERT_FMT(out && !RAY_IS_ERR(out) && ray_table_nrows(out) == 51, "%s: groups", qs[i]);
        sr_drop(out);
    }
    PASS();
}

/* Float sums keep their bits across generations on every strategy: the
 * aggregates fold row by row into their states, in row order, whatever
 * the pieces. */
static test_result_t test_v2_f64_bits(void) {
    sr_w_tables();
    static const char* const qs[] = {
        "(select {s: (sum f) from: T by: g})",                              /* task-local dense */
        "(select {s: (sum f) from: T where: (> k 77) by: g})",              /* dense, selection */
        "(select {s: (sum f) from: T by: {d: d u: u}})",                    /* partitioned */
        "(select {s: (sum f) from: T by: h})",                              /* radix */
        "(select {s: (sum f) from: T by: t})",                              /* table stack */
        "(select {s: (sum f) from: T where: (> k 77) by: t})",
        NULL };
    /* (the small hash folds worker partials whose rows follow the pool's
     * scheduling: its float bits vary run to run in either generation) */
    for (int i = 0; qs[i]; i++) {
        test_result_t r = sr_f64_bits_same(qs[i], "s");
        if (r.status != TEST_PASS) return r;
    }
    PASS();
}

/* R-FAIL on every strategy: a corrupt chunk of a key or an input is the
 * query's error (code and path), on every run. */
static test_result_t test_v2_route_error(void) {
    sr_w_tables();
    char path[600];
    /* Grouped by column (one corrupt copy each).  A sampler that reads the
     * corrupt vector first (the shared-slab sample, the table stack gate)
     * answers the same error. */
    static const struct { const char* col; const char* q; bool sh; } cs[] = {
        { "g",  "(select {s: (sum f) from: B by: g})", false },                          /* dense key */
        { "f",  "(select {s: (sum f) from: B where: (> k 1000) by: g})", false },         /* dense sel input */
        { "f",  "(select {x: (sum f) from: B by: {d: d u: u}})", false },                 /* partitioned input */
        { "f",  "(select {x: (max f) from: B where: (> k 1000) by: {d: d u: u}})", false },
        { "f",  "(select {s: (sum f) from: B by: h})", false },                           /* radix scatter */
        { "f",  "(select {s: (sum f) from: B where: (> k 1000) by: h})", false },
        { "f",  "(select {c: (count k) s: (sum f) from: B by: t})", false },              /* table stack */
        { "f",  "(select {c: (count k) s: (sum f) from: B where: (> k 1000) by: t})", false },
        { "f",  "(select {s: (sum f) from: B where: (> k 1000) by: t})", true },          /* small hash */
        { "f",  "(select {s: (sum f) from: B by: t})", true },
        { "u",  "(select {x: (sum f) from: B by: {d: d u: u}})", false },                 /* partitioned key */
        { "n",  "(select {mx: (max n) from: B by: sk})", false },                         /* shared input */
        { "h",  "(select {c: (count k) from: B by: h})", false },                         /* radix key */
        { "h",  "(select {c: (count k) from: B where: (> k 1000) by: h})", false },
        { "t",  "(select {c: (count k) s: (sum f) from: B by: t})", false },              /* table stack key */
        { "t",  "(select {c: (count k) from: B by: t})", true },                          /* small hash key */
    };
    for (size_t c = 0; c < sizeof cs / sizeof cs[0]; c++) {
        if (c == 0 || strcmp(cs[c].col, cs[c - 1].col) != 0) {
            sr_eval_ok("(.db.splayed.set \"" SR_TMP "/wbad/\" W {compress: true})");
            char dir[512];
            if (ray_splay_resolve_dir(ray_test_tmp(SR_TMP "/wbad/"), dir, sizeof dir) != RAY_OK) abort();
            snprintf(path, sizeof path, "%s/%s", dir, cs[c].col);
            uint64_t off = sr_dirent_offset(path, 2);
            TEST_ASSERT_TRUE(off != 0);
            sr_patch(path, (long)off + 6, 1);
        }
        agg_test_force_smallhash(cs[c].sh);
        for (int i = 0; i < 2; i++) {
            sr_eval_ok("(set B (.db.splayed.get \"" SR_TMP "/wbad/\"))");
            ray_error_clear();
            ray_t* r = sr_eval(cs[c].q);
            if (!(r && RAY_IS_ERR(r))) agg_test_force_smallhash(false);
            TEST_ASSERT_FMT(r && RAY_IS_ERR(r), "%s (bad %s): no error", cs[c].q, cs[c].col);
            TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
            TEST_ASSERT_FMT(strstr(ray_error_msg(), path) != NULL, "%s: %s", cs[c].q, ray_error_msg());
            ray_error_free(r);
        }
        agg_test_force_smallhash(false);
    }
    PASS();
}

/* R-FAIL of the shared dense slab's own dispatch: the slab's sample reads
 * rows i * step + jitter (step = rows / 1024), so with 197108 rows it never
 * reaches row 196608 = 1024 * 192, the first row of chunk 3.  A corrupt
 * vector there is read only by the shared update pass. */
static test_result_t test_v2_shared_fail(void) {
    sr_eval_ok("(set X (table [sk n] (list (map (fn [j] (if (== 0 (% j 100)) j 0)) (til 197108)) "
               "(as 'I32 (- 0 (til 197108))))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/xsh/\" X {compress: true})");
    ray_pool_destroy(); TEST_ASSERT_EQ_I(ray_pool_init_total(3), RAY_OK);
    static const char* const q = "(select {mx: (max n) from: B by: sk})";
    sr_eval_ok("(set B (.db.splayed.get \"" SR_TMP "/xsh/\"))");
    agg_route_reset();
    ray_t* ok = sr_eval(q);
    TEST_ASSERT_TRUE(ok && !RAY_IS_ERR(ok));
    sr_drop(ok);
    TEST_ASSERT_EQ_I((int)agg_route_stats().dense_strategy, AGG_DENSE_SHARED);
    char dir[512], path[600];
    if (ray_splay_resolve_dir(ray_test_tmp(SR_TMP "/xsh/"), dir, sizeof dir) != RAY_OK) abort();
    snprintf(path, sizeof path, "%s/n", dir);
    uint64_t off = sr_dirent_offset(path, 3);
    TEST_ASSERT_TRUE(off != 0);
    sr_patch(path, (long)off + 6, 1);
    for (int i = 0; i < 2; i++) {
        sr_eval_ok("(set B (.db.splayed.get \"" SR_TMP "/xsh/\"))");
        agg_route_reset();
        ray_error_clear();
        ray_t* r = sr_eval(q);
        TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
        TEST_ASSERT_FMT(strstr(ray_error_msg(), path) != NULL, "%s", ray_error_msg());
        ray_error_free(r);
        TEST_ASSERT_TRUE(agg_route_stats().routes[AGG_ROUTE_V2_DENSE] > 0);
    }
    PASS();
}

/* ---- Task 14b: group keys decoded once per run ---- */

/* RQ: 600000 rows; k a near-unique wide I64 key (j*7919 mod 600011 is
 * injective over the rows, times 1000003: a range far past the rows, so
 * no dense plan), m = j % 7, v = j, f = j / 8, p = j*7919 mod 500009 (a
 * wide dense key: the partitioned strategy); saved splayed (rq0, rq2)
 * and as three 200000-row partitions (rp0, rp2). */
static void sr_radix_tables(void) {
    sr_eval_ok("(set jr (til 600000))");
    sr_eval_ok("(set RQ (table [k m v f p] (list (* (% (* jr 7919) 600011) 1000003) (% jr 7) jr (/ (as 'F64 jr) 8.0) "
               "(% (* jr 7919) 500009))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/rq0/\" RQ {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/rq2/\" RQ {compress: true})");
    for (int p = 0; p < 3; p++)
        for (int gen = 0; gen <= 2; gen += 2)
            sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/rp%d/2024.01.0%d/t/\" (take (drop RQ %d) 200000) {compress: %s})",
                        gen, p + 1, p * 200000, gen ? "true" : "false");
}

/* Bytes decoded so far of column `name` of T: its own count, or the sum
 * over a parted column's encoded segments; *full: the column's
 * generation-0 bytes. */
static uint64_t sr_col_dec_any(const char* name, uint64_t* full) {
    ray_t* t = sr_eval("T");
    ray_t* ec = ray_table_get_col_enc(t, ray_sym_intern(name, strlen(name)));
    uint64_t r = 0;
    *full = 0;
    if (ec) {
        ray_enccol_t* e = ray_enccol_of(ec);
        if (e->kind == RAY_ENCCOL_SINGLE) {
            r = atomic_load(&e->decoded_bytes);
            *full = (uint64_t)e->len * ray_enccol_esz(e);
        } else for (int64_t i = 0; e->wrapper && i < e->wrapper->len; i++) {
            ray_t* sg = ray_enccol_seg(ec, i);
            if (!sg || sg->type != RAY_ENCCOL) continue;
            ray_enccol_t* se = ray_enccol_of(sg);
            r += atomic_load(&se->decoded_bytes);
            *full += (uint64_t)se->len * ray_enccol_esz(se);
        }
    }
    sr_drop(t);
    return r;
}

/* q16's shape, a two-input aggregate and two keys on the v2 radix route,
 * and a wide dense key on the partitioned strategy (its histogram reads the
 * key, its scatter the input),
 * splayed and parted generation 2: each column q reads decodes once per
 * run (the key once across the count and the scatter, the parted key's
 * bounds and its cardinality sample from its segments' directories, the
 * second key's compaction not attempted when it cannot fit), within 5% of
 * its bytes (the samplers' point reads, the vectors a chunk starts
 * inside); the answers and routes are generation 0's.  The 1.05 bound
 * holds because k here is FOR_BP, which the samplers' point reads serve:
 * a key whose vectors are mostly RLE (the ClickBench UserID) decodes
 * 1.052-1.055 per run, the table-stack precheck's sampled rows each
 * decoding a vector (the point read does not serve RLE). */
static test_result_t test_v2_radix_decode_once(void) {
    sr_radix_tables();
    ray_pool_destroy(); TEST_ASSERT_EQ_I(ray_pool_init_total(4), RAY_OK);
    static const struct { const char* q; const char* cols[3]; bool partitioned; } qs[] = {
        { "(select {k: k c: (count k) from: T by: k desc: c take: 10})", { "k", NULL }, false },
        { "(select {c: (count v) s: (sum v) from: T by: k})", { "k", "v", NULL }, false },
        { "(select {c: (count v) s: (sum v) from: T by: {k: k m: m}})", { "k", "m", "v" }, false },
        { "(select {c: (count v) s: (sum v) from: T by: p})", { "p", "v", NULL }, true },
    };
    static const char* const t2[] = { "(.db.splayed.get \"" SR_TMP "/rq2/\")", "(.db.parted.get \"" SR_TMP "/rp2\" 't)" };
    static const char* const t0[] = { "(.db.splayed.get \"" SR_TMP "/rq0/\")", "(.db.parted.get \"" SR_TMP "/rp0\" 't)" };
    for (size_t i = 0; i < sizeof qs / sizeof qs[0]; i++)
        for (int s = 0; s < 2; s++) {
            test_result_t r = sr_same_route(t0[s], t2[s], qs[i].q);
            if (r.status != TEST_PASS) return r;
            sr_evalf_ok("(set T %s)", t2[s]);
            sr_drop(sr_eval(qs[i].q));                    /* binds and loads once: the pin is per run */
            uint64_t before[3], full[3];
            for (int c = 0; c < 3 && qs[i].cols[c]; c++) before[c] = sr_col_dec_any(qs[i].cols[c], &full[c]);
            agg_route_reset();
            ray_t* res = sr_eval(qs[i].q);
            agg_route_stats_t st = agg_route_stats();
            TEST_ASSERT_FMT(res && !RAY_IS_ERR(res), "%s: %s", qs[i].q, ray_error_msg() ? ray_error_msg() : "");
            sr_drop(res);
            if (qs[i].partitioned)
                TEST_ASSERT_FMT(st.routes[AGG_ROUTE_V2_DENSE] == 1 && st.dense_strategy == AGG_DENSE_PARTITIONED,
                                "%s (%s): not the partitioned dense route", qs[i].q, s ? "parted" : "splayed");
            else
                TEST_ASSERT_FMT(st.routes[AGG_ROUTE_V2_RADIX] == 1, "%s (%s): not the radix route", qs[i].q, s ? "parted" : "splayed");
            for (int c = 0; c < 3 && qs[i].cols[c]; c++) {
                uint64_t f;
                double x = (double)(sr_col_dec_any(qs[i].cols[c], &f) - before[c]) / (double)full[c];
                TEST_ASSERT_FMT(full[c] > 0 && x >= 1.0 && x <= 1.05, "%s (%s): %s decoded %.4fx", qs[i].q,
                                s ? "parted" : "splayed", qs[i].cols[c], x);
            }
        }
    /* Staging that would not fit the last-level cache keeps the two passes:
     * the key decodes twice (count and scatter), the input once (the
     * scatter alone reads it; each pass re-reads the vector its chunks start
 * inside); the same answer. */
    ray_cache_llc_set_for_test(4096);
    static const char* const q2 = "(select {c: (count v) s: (sum v) from: T by: k})";
    test_result_t r = sr_same_route(t0[0], t2[0], q2);
    if (r.status != TEST_PASS) { ray_cache_llc_set_for_test(0); return r; }
    sr_evalf_ok("(set T %s)", t2[0]);
    uint64_t fk, fv, k0 = sr_col_dec_any("k", &fk), v0 = sr_col_dec_any("v", &fv);
    agg_route_reset();
    ray_t* res = sr_eval(q2);
    agg_route_stats_t st = agg_route_stats();
    ray_cache_llc_set_for_test(0);
    TEST_ASSERT_TRUE(res && !RAY_IS_ERR(res));
    sr_drop(res);
    TEST_ASSERT_TRUE(st.routes[AGG_ROUTE_V2_RADIX] == 1);
    double xk = (double)(sr_col_dec_any("k", &fk) - k0) / (double)fk, xv = (double)(sr_col_dec_any("v", &fv) - v0) / (double)fv;
    TEST_ASSERT_FMT(xk >= 2.0 && xk <= 2.1 && xv >= 1.0 && xv <= 1.05, "two passes: k %.4fx, v %.4fx", xk, xv);
    PASS();
}

/* Byte-equal answers (.csv.write) of generation 0 and 2 over the radix
 * route: one and two keys, a SYM key beside a wide one, F64 sums (their
 * bits: the records of a partition keep input order), q16's top 10, and a
 * ragged parted table (partition 3 lacks v and f: synthetic null
 * segments) against its generation-0 twin. */
static bool sr_csv_of(const char* q, const char* path, char** out, size_t* n) {
    char e[1024];
    snprintf(e, sizeof e, "(.csv.write %s \"%s\")", q, path);
    ray_t* r = sr_eval(e);
    bool ok = r && !RAY_IS_ERR(r);
    sr_drop(r);
    FILE* f = ok ? fopen(path, "rb") : NULL;
    if (!f) return false;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    *out = malloc((size_t)len + 1);
    *n = fread(*out, 1, (size_t)len, f);
    fclose(f);
    return *n == (size_t)len;
}
static test_result_t sr_csv_same(const char* t0, const char* t2, const char* q) {
    char p0[512], p2[512];
    snprintf(p0, sizeof p0, "%s/a0.csv", sr_root()); snprintf(p2, sizeof p2, "%s/a2.csv", sr_root());
    char* b[2] = { NULL, NULL };
    size_t n[2] = { 0, 0 };
    for (int gen = 0; gen < 2; gen++) {
        sr_evalf_ok("(set T %s)", gen ? t2 : t0);
        if (!sr_csv_of(q, gen ? p2 : p0, &b[gen], &n[gen])) { free(b[0]); free(b[1]); FAIL("csv"); }
    }
    bool same = n[0] == n[1] && n[0] > 0 && memcmp(b[0], b[1], n[0]) == 0;
    free(b[0]); free(b[1]);
    TEST_ASSERT_FMT(same, "%s: generation 0 and 2 differ (%zu / %zu bytes)", q, n[0], n[1]);
    PASS();
}
static test_result_t test_v2_radix_answers(void) {
    sr_radix_tables();
    sr_eval_ok("(set RS (table [s k v f] (list (as 'SYMBOL (map (fn [j] (format \"w%\" (% j 50))) jr)) "
               "(* (% (* jr 7919) 600011) 1000003) jr (/ (as 'F64 jr) 8.0))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/rs0/\" RS {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/rs2/\" RS {compress: true})");
    for (int gen = 0; gen <= 2; gen += 2)
        for (int p = 0; p < 3; p++)
            sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/rr%d/2024.01.0%d/t/\" (select {%s from: (take (drop RQ %d) 200000)}) {compress: %s})",
                        gen, p + 1, p == 2 ? "k: k m: m" : "k: k m: m v: v f: f", p * 200000, gen ? "true" : "false");
    ray_pool_destroy(); TEST_ASSERT_EQ_I(ray_pool_init_total(4), RAY_OK);
    static const char* const qs[] = {
        "(select {c: (count v) s: (sum v) a: (sum f) from: T by: k})",
        "(select {c: (count v) a: (sum f) mx: (max v) from: T by: {k: k m: m}})",
        "(select {k: k c: (count k) from: T by: k desc: c take: 10})",
        "(select {c: (count v) a: (sum f) from: T where: (> v 1000) by: k})",
        NULL,
    };
    static const char* const t2[] = { "(.db.splayed.get \"" SR_TMP "/rq2/\")", "(.db.parted.get \"" SR_TMP "/rp2\" 't)" };
    static const char* const t0[] = { "(.db.splayed.get \"" SR_TMP "/rq0/\")", "(.db.parted.get \"" SR_TMP "/rp0\" 't)" };
    for (int i = 0; qs[i]; i++)
        for (int s = 0; s < 2; s++) {
            agg_route_reset();
            test_result_t r = sr_csv_same(t0[s], t2[s], qs[i]);
            if (r.status != TEST_PASS) return r;
            TEST_ASSERT_FMT(agg_route_stats().routes[AGG_ROUTE_V2_RADIX] == 2, "%s: not radix on both", qs[i]);
        }
    agg_route_reset();
    test_result_t r = sr_csv_same("(.db.splayed.get \"" SR_TMP "/rs0/\")", "(.db.splayed.get \"" SR_TMP "/rs2/\")",
                                  "(select {c: (count v) a: (sum f) from: T by: {s: s k: k}})");
    if (r.status != TEST_PASS) return r;
    TEST_ASSERT_TRUE(agg_route_stats().routes[AGG_ROUTE_V2_RADIX] == 2);
    agg_route_reset();
    r = sr_csv_same("(.db.parted.get \"" SR_TMP "/rr0\" 't)", "(.db.parted.get \"" SR_TMP "/rr2\" 't)",
                    "(select {c: (count v) s: (sum v) a: (sum f) from: T by: k})");
    if (r.status != TEST_PASS) return r;
    TEST_ASSERT_TRUE(agg_route_stats().routes[AGG_ROUTE_V2_RADIX] == 2);
    PASS();
}

/* Fix round 1: the record order of a radix partition.  RO: 600000 rows,
 * 40000 groups of 15 rows each spread over the whole table (k = (j *
 * 7919 mod 40000) * 1000003, a wide key: radix), s a SYM of the group, f = j / 7.0
 * (an inexact sum: its bits follow the order the records arrive in).  At
 * 2 and 8 workers, `by k` and `by {s k}` answer byte for byte the same
 * (.csv.write) on generation 0, on generation 2 through the fused pass
 * (the key decodes once), and on generation 2 through the two passes
 * (forced by a 4 KiB last-level cache: the key decodes twice); the fused
 * pass also on three partitions.  A phase 2 that walked a partition's
 * chunk runs in any other order changes the sums. */
static test_result_t test_v2_radix_record_order(void) {
    sr_eval_ok("(set jo (til 600000))");
    sr_eval_ok("(set RO (table [s k f] (list (as 'SYMBOL (map (fn [j] (format \"w%\" (% (% (* j 7919) 40000) 50))) jo)) "
               "(* (% (* jo 7919) 40000) 1000003) (/ (as 'F64 jo) 7.0))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/ro0/\" RO {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/ro2/\" RO {compress: true})");
    for (int p = 0; p < 3; p++)
        sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/rop/2024.01.0%d/t/\" (take (drop RO %d) 200000) {compress: true})",
                    p + 1, p * 200000);
    static const char* const qs[] = {
        "(select {c: (count f) a: (sum f) from: T by: k})",
        "(select {c: (count f) a: (sum f) from: T by: {s: s k: k}})",
    };
    /* layouts: generation 0, fused generation 2, two-pass generation 2, fused parted generation 2 */
    static const char* const tb[] = {
        "(.db.splayed.get \"" SR_TMP "/ro0/\")", "(.db.splayed.get \"" SR_TMP "/ro2/\")",
        "(.db.splayed.get \"" SR_TMP "/ro2/\")", "(.db.parted.get \"" SR_TMP "/rop\" 't)",
    };
    static const char* const tn[] = { "gen 0", "gen 2 fused", "gen 2 two passes", "gen 2 parted fused" };
    static const int workers[] = { 2, 8 };
    char path[512];
    snprintf(path, sizeof path, "%s/ro.csv", sr_root());
    for (int w = 0; w < 2; w++) {
        ray_pool_destroy(); TEST_ASSERT_EQ_I(ray_pool_init_total(workers[w]), RAY_OK);
        for (int i = 0; i < 2; i++) {
            char* ref = NULL; size_t nref = 0;
            for (int l = 0; l < 4; l++) {
                sr_evalf_ok("(set T %s)", tb[l]);
                sr_drop(sr_eval(qs[i]));                      /* binds and loads once */
                uint64_t fk, k0 = sr_col_dec_any("k", &fk);
                if (l == 2) ray_cache_llc_set_for_test(4096);
                agg_route_reset();
                char* b = NULL; size_t n = 0;
                bool ok = sr_csv_of(qs[i], path, &b, &n);
                agg_route_stats_t st = agg_route_stats();
                ray_cache_llc_set_for_test(0);
                double xk = l ? (double)(sr_col_dec_any("k", &fk) - k0) / (double)fk : 0.0;
                bool route = st.routes[AGG_ROUTE_V2_RADIX] == 1 &&
                             (l == 0 || (l == 2 ? xk >= 2.0 && xk <= 2.2 : xk >= 1.0 && xk <= 1.05));
                bool same = ok && (l == 0 || (n == nref && memcmp(b, ref, n) == 0));
                if (l == 0 && ok) { ref = b; nref = n; } else free(b);
                if (!ok || !route || !same) {
                    free(ref);
                    TEST_ASSERT_FMT(ok, "%s, %d workers, %s: csv", qs[i], workers[w], tn[l]);
                    TEST_ASSERT_FMT(route, "%s, %d workers, %s: radix %llu, key decoded %.4fx", qs[i], workers[w], tn[l],
                                    (unsigned long long)st.routes[AGG_ROUTE_V2_RADIX], xk);
                    TEST_ASSERT_FMT(same, "%s, %d workers: %s differs from gen 0 (%zu / %zu bytes)", qs[i], workers[w], tn[l], n, nref);
                }
            }
            free(ref);
        }
    }
    PASS();
}

/* Task 17i: the fused pass stages only the sources it decodes; a plain
 * column and the positions are read in place by its scatter.  RC: 300000
 * rows, k a wide encoded I64, s an encoded SYM, g an I16, m, v and f encoded; the
 * computed key (+ m 1) and inputs (* v 3), (* f 2.0) are plain, and so
 * are g and s once their materializations are published (`at`), as the
 * keys of every width the scatter widens in place.  Each shape answers
 * byte for byte (.csv.write) as generation 0 at 2 and 4 workers, splayed
 * and on three partitions, on the radix route with the compact fused
 * phase 1 (whole records under a where: sel mode); on partitions the
 * route is checked for the shape without a plain column or a where (a
 * computed column takes the per-partition route there, a where the two
 * passes). */
static test_result_t test_v2_radix_compact_staging(void) {
    sr_eval_ok("(set jc (til 300000))");
    sr_eval_ok("(set RC (table [s k m g v f w x y z] (list (as 'SYMBOL (map (fn [j] (format \"w%\" (% j 70))) jc)) "
               "(* (% (* jc 7919) 600011) 1000003) (% jc 5) (as 'I16 (- (% jc 9) 4)) jc (/ (as 'F64 jc) 9.0) "
               "(* (% jc 40000) 1000003) (/ (as 'F64 (% (* jc 31) 1009)) 7.0) "
               "(as 'I32 (- (% (* jc 13) 2003) 1000)) (as 'I16 (- (% (* jc 17) 401) 200)))))");
    for (int gen = 0; gen <= 2; gen += 2) {
        sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/rc%d/\" RC {compress: %s})", gen, gen ? "true" : "false");
        for (int p = 0; p < 3; p++)
            sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/rcp%d/2024.01.0%d/t/\" (take (drop RC %d) 100000) {compress: %s})",
                        gen, p + 1, p * 100000, gen ? "true" : "false");
    }
    static const struct { const char* q; uint8_t p1; bool parted_route; bool pub; bool splayed_only; } qs[] = {
        { "(select {c: (count v) a: (sum f) x: (sum (* v 3)) from: T by: {k: k j: (+ m 1)}})", AGG_RADIX_P1_FUSED_COMPACT, false, false, false },
        { "(select {x: (sum (* v 3)) y: (sum (* f 2.0)) mx: (max v) from: T by: {s: s k: k}})", AGG_RADIX_P1_FUSED_COMPACT, false, false, false },
        { "(select {c: (count v) a: (sum f) mx: (max v) from: T by: {s: s k: k}})", AGG_RADIX_P1_FUSED_COMPACT, true, false, false },
        { "(select {c: (count v) a: (sum f) y: (sum g) from: T by: {g: g k: k s: s}})", AGG_RADIX_P1_FUSED_COMPACT, false, true, false },
        { "(select {c: (count v) a: (sum f) from: T where: (> v 1000) by: {s: s k: k}})", AGG_RADIX_P1_FUSED_RECORDS, false, false, false },
        /* Fix round 1: two-input aggregates first, in the middle and last,
         * beside inputs of other widths (F64 x, I32 y, I16 z): each input
         * staged at its own field (the record's order).  Splayed only: a
         * parted table's two-input aggregates are wrong on generation 0
         * itself (and on the base), a separate defect. */
        { "(select {c: (pearson_corr x y) s: (sum z) mx: (max z) from: T by: w})", AGG_RADIX_P1_FUSED_COMPACT, false, false, true },
        { "(select {s: (sum z) c: (wsum y x) mx: (max y) from: T by: w})", AGG_RADIX_P1_FUSED_COMPACT, false, false, true },
        { "(select {s: (sum z) a: (sum y) c: (cov z y) from: T by: w})", AGG_RADIX_P1_FUSED_COMPACT, false, false, true },
        { "(select {a: (wavg z x) b: (scov y z) c: (sum x) d: (pearson_corr x z) from: T by: {w: w s: s}})", AGG_RADIX_P1_FUSED_COMPACT, false, false, true },
    };
    static const char* const t0[] = { "(.db.splayed.get \"" SR_TMP "/rc0/\")", "(.db.parted.get \"" SR_TMP "/rcp0\" 't)" };
    static const char* const t2[] = { "(.db.splayed.get \"" SR_TMP "/rc2/\")", "(.db.parted.get \"" SR_TMP "/rcp2\" 't)" };
    static const int workers[] = { 2, 4 };
    char path[512];
    snprintf(path, sizeof path, "%s/rc.csv", sr_root());
    for (int w = 0; w < 2; w++) {
        ray_pool_destroy(); TEST_ASSERT_EQ_I(ray_pool_init_total(workers[w]), RAY_OK);
        for (size_t i = 0; i < sizeof qs / sizeof qs[0]; i++)
            for (int l = 0; l < (qs[i].pub || qs[i].splayed_only ? 1 : 2); l++) {
                char* b[2] = { NULL, NULL };
                size_t n[2] = { 0, 0 };
                agg_route_stats_t st = { 0 };
                for (int gen = 0; gen < 2; gen++) {
                    sr_evalf_ok("(set T %s)", gen ? t2[l] : t0[l]);
                    if (qs[i].pub) { sr_eval_ok("(set z (+ (at T 'g) 0))"); sr_eval_ok("(set z (first (at T 's)))"); }
                    agg_route_reset();
                    bool ok = sr_csv_of(qs[i].q, path, &b[gen], &n[gen]);
                    st = agg_route_stats();
                    if (!ok) { free(b[0]); free(b[1]); FAIL("csv"); }
                }
                bool same = n[0] == n[1] && n[0] > 0 && memcmp(b[0], b[1], n[0]) == 0;
                free(b[0]); free(b[1]);
                TEST_ASSERT_FMT(same, "%s (%s, %d workers): generation 0 and 2 differ (%zu / %zu bytes)", qs[i].q,
                                l ? "parted" : "splayed", workers[w], n[0], n[1]);
                if (l && !qs[i].parted_route) continue;
                TEST_ASSERT_FMT(st.routes[AGG_ROUTE_V2_RADIX] == 1 && st.radix_phase1 == qs[i].p1,
                                "%s (%s, %d workers): radix %llu, phase 1 %u, want %u", qs[i].q,
                                l ? "parted" : "splayed", workers[w],
                                (unsigned long long)st.routes[AGG_ROUTE_V2_RADIX], st.radix_phase1, qs[i].p1);
            }
    }
    PASS();
}

/* The parted directory consumer (design (b)): a parted key's dense bounds
 * come from its segments' statistics, so its passes read under the guard,
 * which checks every segment against its own chunks.  Three partitions of
 * 70000 rows; k = j*7 % 1000, except partition 2's chunk 0, j*7 % 5001
 * with a null every 100 rows (the samplers' point reads decline there).
 * 1000 groups take the in-place concat.  Partition 2's chunk 0 statistics
 * (directory and descriptors, still nested) lowered to 999: `corrupt`
 * naming that segment's file, twice, through range reads and through the
 * published materialization; unpatched, generation 0's answer. */
static void sr_forged_parted(const char* root, int gen) {
    for (int p = 0; p < 3; p++)
        sr_evalf_ok("(.db.splayed.set \"%s/2024.01.0%d/t/\" (table [k v] (list %s (til 70000))) {compress: %s})",
                    root, p + 1,
                    p == 1 ? "(map (fn [j] (if (< j 65536) (if (== 0 (% j 100)) 0N (% (* j 7) 5001)) (% (* j 7) 1000))) (til 70000))"
                           : "(% (* (til 70000) 7) 1000)",
                    gen ? "true" : "false");
}
static test_result_t test_idx_info_table(void) {
    sr_eval_ok("(set T (.db.splayed.get \"" SR_G2 "\"))");
    ray_enc_counters_reset();
    ray_t* d = sr_eval("(.idx.info T 'k)");
    TEST_ASSERT_TRUE(d && !RAY_IS_ERR(d));
    ray_t* g = sr_eval("(at (.idx.info T 'k) 'gen)");
    TEST_ASSERT_EQ_I(g->i64, 2);
    ray_t* m = sr_eval("(at (.idx.info T 'k) 'materialized)");
    TEST_ASSERT_FALSE(m->b8);
    ray_t* e = sr_eval("(sum (value (at (.idx.info T 'g) 'enc)))");             /* every vector has one encoding */
    TEST_ASSERT_TRUE(e && !RAY_IS_ERR(e) && e->i64 == 196);            /* 200000 rows: 196 vectors */
    ray_t* rb = sr_eval("(at (.idx.info T 'k) 'raw_bytes)");
    TEST_ASSERT_EQ_I(rb->i64, 200000 * 8);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 0);
    sr_eval_ok("(sum (at T 'g))");                                      /* range reads: still not materialized */
    ray_t* m2 = sr_eval("(at (.idx.info T 'g) 'materialized)");
    TEST_ASSERT_FALSE(m2->b8);
    sr_eval_ok("(set gg (at T 'g))");                                   /* set forces the lazy: materializes */
    sr_eval_ok("(count gg)");
    ray_t* m3 = sr_eval("(at (.idx.info T 'g) 'materialized)");
    TEST_ASSERT_TRUE(m3->b8);
    ray_t* g0 = sr_eval("(at (.idx.info (.db.splayed.get \"" SR_G0 "\") 'k) 'gen)");
    TEST_ASSERT_EQ_I(g0->i64, 0);
    /* ruling I5: a ragged generation-0 parted column reports generation 0 */
    sr_parted_xy(SR_TMP "/irg", 0, true, false);
    ray_t* gr = sr_eval("(at (.idx.info (.db.parted.get \"" SR_TMP "/irg\" 't) 'y) 'gen)");
    TEST_ASSERT_TRUE(gr && !RAY_IS_ERR(gr) && gr->i64 == 0);
    sr_drop(d); sr_drop(g); sr_drop(m); sr_drop(e); sr_drop(rb);
    sr_drop(m2); sr_drop(m3); sr_drop(g0); sr_drop(gr);
    PASS();
}

/* Fix round 1: chunk c's directory minimum (set_min) or maximum of the
 * generation-2 column file at path set to x, the descriptors untouched.
 * False on an I/O error or a chunk without statistics. */
static bool sr_forge_dirent(const char* path, int64_t c, bool set_min, int64_t x) {
    FILE* f = fopen(path, "r+b"); if (!f) return false;
    ray_enc_dirent_t de;
    long doff = (long)(RAY_ENC_DIR_OFF + (uint64_t)c * sizeof de);
    bool ok = fseek(f, doff, SEEK_SET) == 0 && fread(&de, sizeof de, 1, f) == 1 && !(de.flags & RAY_ENC_STAT_ABSENT);
    if (ok) { if (set_min) de.min = x; else de.max = x; }
    ok = ok && fseek(f, doff, SEEK_SET) == 0 && fwrite(&de, sizeof de, 1, f) == 1;
    return fclose(f) == 0 && ok;
}
/* The table at dir_expr, loaded and queried with q, gives `corrupt` naming
 * path (at the load or at the query), twice. */
static test_result_t sr_corrupt_load_twice(const char* dir_expr, const char* q, const char* path) {
    for (int rep = 0; rep < 2; rep++) {
        ray_error_clear();
        char e[1024];
        snprintf(e, sizeof e, "(set T %s)", dir_expr);
        ray_t* r = sr_eval(e);
        if (r && !RAY_IS_ERR(r)) { sr_drop(r); ray_error_clear(); r = sr_eval(q); }
        TEST_ASSERT_FMT(r && RAY_IS_ERR(r), "%s (read %d): answered", q, rep + 1);
        TEST_ASSERT_FMT(strcmp(ray_err_code(r), "corrupt") == 0 && ray_error_msg() && strstr(ray_error_msg(), path),
                        "%s (read %d): %s \"%s\" (want %s)", q, rep + 1, ray_err_code(r), ray_error_msg() ? ray_error_msg() : "", path);
        ray_error_free(r);
    }
    PASS();
}

/* The reported number of `src` (an integer-valued rayfall expression). */
static int64_t ii_i64(const char* src) {
    ray_t* r = sr_eval(src);
    int64_t v = (r && !RAY_IS_ERR(r)) ? r->i64 : INT64_MIN;
    sr_drop(r);
    return v;
}
static bool ii_err(const char* src, const char* code, const char* needle) {
    ray_t* r = sr_eval(src);
    bool ok = r && RAY_IS_ERR(r) && strcmp(ray_err_code(r), code) == 0 &&
              ray_error_msg() && strstr(ray_error_msg(), needle);
    sr_drop(r);
    return ok;
}

/* STR pool bytes, parted sums and segments, a mixed parted column, the
 * index key and the argument errors of (.idx.info table 'col). */
static test_result_t test_idx_info_report(void) {
    /* STR: len * 16 plus the pool, in both generations, and the pool counts */
    sr_eval_ok("(set ST (table [s] (list (as 'STR (map (fn [j] (format \"a-long-string-value-%\" (% j 977))) (til 70000))))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/is2/\" ST {compress: true})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/is0/\" ST {compress: false})");
    sr_eval_ok("(set S2 (.db.splayed.get \"" SR_TMP "/is2/\"))");
    sr_eval_ok("(set S0 (.db.splayed.get \"" SR_TMP "/is0/\"))");
    int64_t r2 = ii_i64("(at (.idx.info S2 's) 'raw_bytes)");
    int64_t r0 = ii_i64("(at (.idx.info S0 's) 'raw_bytes)");
    int64_t e0 = ii_i64("(at (.idx.info S0 's) 'encoded_bytes)");
    TEST_ASSERT_TRUE(r0 > 70000 * 16);               /* the pool is counted */
    TEST_ASSERT_EQ_I(r2, r0);
    TEST_ASSERT_EQ_I(e0, r0);                        /* plain: encoded is raw */
    TEST_ASSERT_EQ_I(ii_i64("(at (.idx.info S2 's) 'gen)"), 2);
    TEST_ASSERT_TRUE(ii_i64("(at (.idx.info S2 's) 'encoded_bytes)") > 0);

    /* parted, every segment generation 2: sums over the segments */
    sr_parted_xy(SR_TMP "/ip2", 7, false, false);
    sr_eval_ok("(set P2 (.db.parted.get \"" SR_TMP "/ip2\" 't))");
    TEST_ASSERT_EQ_I(ii_i64("(at (.idx.info P2 'x) 'gen)"), 2);
    TEST_ASSERT_EQ_I(ii_i64("(at (.idx.info P2 'x) 'segments)"), 3);
    TEST_ASSERT_EQ_I(ii_i64("(at (.idx.info P2 'x) 'raw_bytes)"), 3 * 70000 * 8);
    TEST_ASSERT_EQ_I(ii_i64("(sum (value (at (.idx.info P2 'x) 'enc)))"), 3 * 69);
    int64_t seg = 0;
    for (int p = 1; p <= 3; p++) {
        char q[256];
        snprintf(q, sizeof q, "(at (.idx.info (.db.splayed.get \"" SR_TMP "/ip2/2024.01.0%d/t/\") 'x) 'encoded_bytes)", p);
        seg += ii_i64(q);
    }
    TEST_ASSERT_TRUE(seg > 0);
    TEST_ASSERT_EQ_I(ii_i64("(at (.idx.info P2 'x) 'encoded_bytes)"), seg);

    /* parted, every segment plain: raw and encoded are the segment sums */
    sr_parted_xy(SR_TMP "/ip0", 0, false, false);
    sr_eval_ok("(set P0 (.db.parted.get \"" SR_TMP "/ip0\" 't))");
    TEST_ASSERT_EQ_I(ii_i64("(at (.idx.info P0 'x) 'gen)"), 0);
    TEST_ASSERT_EQ_I(ii_i64("(at (.idx.info P0 'x) 'segments)"), 3);
    TEST_ASSERT_EQ_I(ii_i64("(at (.idx.info P0 'x) 'raw_bytes)"), 3 * 70000 * 8);
    TEST_ASSERT_EQ_I(ii_i64("(at (.idx.info P0 'x) 'encoded_bytes)"), 3 * 70000 * 8);

    /* mixed: two compressed segments and a synthetic one report generation 2;
     * the synthetic stand-in adds raw bytes but no file bytes */
    sr_parted_xy(SR_TMP "/imx", 3, true, false);
    sr_eval_ok("(set PM (.db.parted.get \"" SR_TMP "/imx\" 't))");
    TEST_ASSERT_EQ_I(ii_i64("(at (.idx.info PM 'y) 'gen)"), 2);
    TEST_ASSERT_EQ_I(ii_i64("(at (.idx.info PM 'y) 'raw_bytes)"), 3 * 70000 * 8);
    TEST_ASSERT_EQ_I(ii_i64("(sum (value (at (.idx.info PM 'y) 'enc)))"), 2 * 69);
    /* ragged all-plain: synthetic stand-in contributes no file bytes */
    sr_parted_xy(SR_TMP "/irg0", 0, true, false);
    sr_eval_ok("(set PR (.db.parted.get \"" SR_TMP "/irg0\" 't))");
    TEST_ASSERT_EQ_I(ii_i64("(at (.idx.info PR 'y) 'encoded_bytes)"), 2 * 70000 * 8);
    TEST_ASSERT_EQ_I(ii_i64("(sum (value (at (.idx.info PR 'y) 'enc)))"), 0);

    /* the index key: generation 2 is the recorded kind, generation 0 the attached index */
    sr_eval_ok("(set T (.db.splayed.get \"" SR_G2 "\"))");
    ray_t* ik = sr_eval("(at (.idx.info T 'k) 'index)");
    TEST_ASSERT_TRUE(ik && !RAY_IS_ERR(ik) && ik->type == -RAY_SYM &&
                     strcmp(ray_str_ptr(ray_sym_str(ik->i64)), "chunk_zone") == 0);
    sr_drop(ik);
    sr_eval_ok("(set IH (table [a] (list (.idx.hash [1 2 3 1]))))");
    ray_t* hk = sr_eval("(at (at (.idx.info IH 'a) 'index) 'kind)");
    TEST_ASSERT_TRUE(hk && !RAY_IS_ERR(hk) && hk->type == -RAY_SYM &&
                     strcmp(ray_str_ptr(ray_sym_str(hk->i64)), "hash") == 0);
    sr_drop(hk);
    ray_t* nk = sr_eval("(at (.idx.info (table [a] (list [1 2 3])) 'a) 'index)");
    TEST_ASSERT_TRUE(nk == RAY_NULL_OBJ);

    /* argument errors */
    TEST_ASSERT_TRUE(ii_err("(.idx.info T 'k 'k)", "arity", "expected"));
    TEST_ASSERT_TRUE(ii_err("(.idx.info)", "arity", "expected"));
    TEST_ASSERT_TRUE(ii_err("(.idx.info [1 2 3] 'k)", "type", "table"));
    TEST_ASSERT_TRUE(ii_err("(.idx.info T \"k\")", "type", "symbol"));
    TEST_ASSERT_TRUE(ii_err("(.idx.info T 'nope)", "domain", "no such column"));
    PASS();
}

/* Fix round 1: ray_col_range_into over a guarded source declines, so a
 * stats-guarded consumer reads by range, which checks each vector.  The
 * forged parted table of v2_forged_parted_segment (partition 2's chunk 0
 * maximum lowered to 999; its rows start at 70000): unguarded, that
 * segment's first vector is delivered straight (the control); guarded,
 * nothing is delivered and the range read of the same rows is `corrupt`
 * naming the segment's file, twice. */
static test_result_t test_v2_range_into_guarded(void) {
    sr_forged_parted(SR_TMP "/fg", 2);
    char dir[512], path[600];
    if (ray_splay_resolve_dir(ray_test_tmp(SR_TMP "/fg/2024.01.02/t/"), dir, sizeof dir) != RAY_OK) FAIL("resolve");
    size_t dl = strlen(dir);
    snprintf(path, sizeof path, "%s%sk", dir, (dl && dir[dl - 1] == '/') ? "" : "/");
    if (!sr_forge_max(path, 0, 999)) FAIL("forge");
    ray_t* t = sr_eval("(.db.parted.get \"" SR_TMP "/fg\" 't)");
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    int64_t* dst = malloc(1024 * sizeof(int64_t));
    for (int guard = 0; guard < 2; guard++)
        for (int rep = 0; rep < (guard ? 2 : 1); rep++) {
            ray_col_src_t src;
            ray_t* err = ray_col_src_bind(ray_table_slot(t, ray_sym_intern("k", 1)), &src);
            TEST_ASSERT_TRUE(err == NULL);
            src.stats_guard = guard;
            ray_col_scratch_t sc;
            ray_col_scratch_init(&sc);
            int64_t got = -1;
            err = ray_col_range_into(&src, 70000, 1024, &sc, 0, dst, &got);
            bool into_ok = err == NULL && got == (guard ? 0 : 1024);
            if (err) ray_error_free(err);
            ray_error_clear();
            ray_col_range_t v;
            ray_t* rerr = guard ? ray_col_range(&src, 70000, 1024, &sc, &v) : NULL;
            bool corrupt = !guard || (rerr && RAY_IS_ERR(rerr) && strcmp(ray_err_code(rerr), "corrupt") == 0 &&
                                      ray_error_msg() && strstr(ray_error_msg(), path));
            if (rerr) ray_error_free(rerr);
            ray_col_scratch_free(&sc);
            ray_col_src_release(&src);
            if (!into_ok || !corrupt) { free(dst); sr_drop(t); }
            TEST_ASSERT_FMT(into_ok, "guard %d: delivered %lld, want %d", guard, (long long)got, guard ? 0 : 1024);
            TEST_ASSERT_FMT(corrupt, "guarded range read (read %d): not corrupt naming %s", rep + 1, path);
        }
    free(dst);
    sr_drop(t);
    PASS();
}

static test_result_t test_v2_forged_parted_segment(void) {
    sr_forged_parted(SR_TMP "/fp0", 0);
    sr_forged_parted(SR_TMP "/fp2", 2);
    static const char* const q = "(select {s: (sum v) c: (count v) from: T by: k})";
    int64_t s0 = ray_group_segstream_runs();
    test_result_t r = sr_same_route("(.db.parted.get \"" SR_TMP "/fp0\" 't)", "(.db.parted.get \"" SR_TMP "/fp2\" 't)", q);
    if (r.status != TEST_PASS) return r;
    TEST_ASSERT_TRUE(ray_group_segstream_runs() - s0 == 2);          /* both in place */
    sr_forged_parted(SR_TMP "/fpb", 2);
    char dir[512], path[600];
    if (ray_splay_resolve_dir(ray_test_tmp(SR_TMP "/fpb/2024.01.02/t/"), dir, sizeof dir) != RAY_OK) FAIL("resolve");
    size_t dl = strlen(dir);
    snprintf(path, sizeof path, "%s%sk", dir, (dl && dir[dl - 1] == '/') ? "" : "/");
    if (!sr_forge_max(path, 0, 999)) FAIL("forge");
    r = sr_corrupt_twice("(.db.parted.get \"" SR_TMP "/fpb\" 't)", q, path);
    if (r.status != TEST_PASS) return r;
    /* the parted materialization published first: read in place only once
     * every segment lies inside its statistics */
    for (int rep = 0; rep < 2; rep++) {
        sr_eval_ok("(set T (.db.parted.get \"" SR_TMP "/fpb\" 't))");
        ray_t* t = sr_eval("T");
        ray_t* ec = ray_table_get_col_enc(t, ray_sym_intern("k", 1));
        TEST_ASSERT_TRUE(ec && ray_enccol_is_parted(ec));
        ray_t* m = ray_enccol_materialize(ec);
        TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
        sr_drop(t);
        ray_error_clear();
        ray_t* res = sr_eval(q);
        TEST_ASSERT_FMT(res && RAY_IS_ERR(res), "materialized (read %d): answered", rep + 1);
        TEST_ASSERT_FMT(strcmp(ray_err_code(res), "corrupt") == 0 && ray_error_msg() && strstr(ray_error_msg(), path),
                        "materialized (read %d): %s \"%s\"", rep + 1, ray_err_code(res), ray_error_msg() ? ray_error_msg() : "");
        ray_error_free(res);
    }
    /* Fix round 1: a directory statistic outside its column's type is
     * `corrupt` at open.  An I32 key's first segment (the cardinality sample) chunk 0 maximum raised to
     * INT64_MAX, its minimum lowered to INT64_MIN, and its maximum raised to
     * 2^31 (a 64-bit value past I32): the cardinality sample and the dense
     * bounds both read these, so each is `corrupt` naming the file, twice. */
    static const char* const qi = "(select {s: (sum v) c: (count v) from: T by: i})";
    for (int p = 0; p < 3; p++)
        for (int gen = 0; gen <= 2; gen += 2)
            sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/fi%d/2024.01.0%d/t/\" (table [i v] (list (as 'I32 (%% (* (til 70000) 7) 1000)) "
                        "(til 70000))) {compress: %s})", gen, p + 1, gen ? "true" : "false");
    r = sr_same_route("(.db.parted.get \"" SR_TMP "/fi0\" 't)", "(.db.parted.get \"" SR_TMP "/fi2\" 't)", qi);
    if (r.status != TEST_PASS) return r;
    static const struct { bool set_min; int64_t x; } fi[] = {
        { false, INT64_MAX }, { true, INT64_MIN }, { false, (int64_t)INT32_MAX + 1 },
    };
    for (size_t i = 0; i < sizeof fi / sizeof fi[0]; i++) {
        for (int p = 0; p < 3; p++)
            sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/fib/2024.01.0%d/t/\" (table [i v] (list (as 'I32 (%% (* (til 70000) 7) 1000)) "
                        "(til 70000))) {compress: true})", p + 1);
        if (ray_splay_resolve_dir(ray_test_tmp(SR_TMP "/fib/2024.01.01/t"), dir, sizeof dir) != RAY_OK) FAIL("resolve");
        dl = strlen(dir);
        snprintf(path, sizeof path, "%s%si", dir, (dl && dir[dl - 1] == '/') ? "" : "/");
        if (!sr_forge_dirent(path, 0, fi[i].set_min, fi[i].x)) FAIL("forge");
        r = sr_corrupt_load_twice("(.db.parted.get \"" SR_TMP "/fib\" 't)", qi, path);
        if (r.status != TEST_PASS) return r;
    }
    PASS();
}

/* The ClickBench scan-only set of the P2 gate (the plan's Task 17
 * SCAN_ONLY), each written over the battery tables' columns with the same
 * operators, key widths and clauses: k and h stand for the I64 columns
 * (WatchID, UserID), v for the nullable I32 (ClientIP), g and u for the
 * narrow integers, d for EventDate and s for the SYM columns.  q18, q39,
 * the compact fallback and the conjunctive count were pinned in sr_gaps
 * until each stopped materializing (P3 Tasks 6 and 6b, Task 15b); the P3
 * gate (Task 12) moved them here, at zero. */
#define SR_Q18 "(select {c: (count h) from: T by: {h: h s: s} take: 10})"
#define SR_Q35 "(select {one: 1 s: s c: (count s) from: T by: s desc: c take: 10})"
#define SR_Q39 "(take (select {s: s p: (count s) from: T where: (and (== g 3) (>= d 100) (<= d 400) " \
               "(== b false) (!= u 0)) by: s desc: p take: 1010}) -10)"
#define SR_COMPACT_GROUP "(select {c: (count k) from: T where: (< k 1000) by: g})"
#define SR_AND_COUNT "(count (select {from: T where: (and (== g 3) (!= u 0))}))"
static const char* const sr_clickbench_scan[] = {
    "(count T)",                                                                          /* q1 */
    "(count (select {from: T where: (!= u 0)}))",                                         /* q2 */
    "(table [a b c] (list (list (sum (at T 'g))) (list (count T)) (list (avg (at T 'v)))))", /* q3 */
    "(avg (at T 'h))",                                                                    /* q4 */
    "(table [a b] (list (list (min (at T 'd))) (list (max (at T 'd)))))",                 /* q7 */
    "(select {u: u c: (count u) from: T where: (!= u 0) by: u desc: c take: 10})",        /* q8 */
    "(select {h: h c: (count h) from: T by: h desc: c take: 10})",                        /* q16 */
    "(select {c: (count h) from: T by: {h: h s: s} desc: c take: 10})",                   /* q17 */
    "(select {c: (count v) s: (sum g) a: (avg u) from: T by: {k: k v: v} desc: c take: 10})", /* q33 */
    "(select {s: s c: (count s) from: T by: s desc: c take: 10})",                        /* q34 */
    SR_Q35,                                                                               /* q35 */
    /* q18: a group-by take without an order runs the first-n pre-pass
     * (agg_first_n_run) over the keys' sources: rows read through task
     * cursors, the sample point-read, the groups' rows gathered by range
     * (P3 Task 6b); on a parted table in place over the segments */
    SR_Q18,                                                                               /* q18 */
    /* q39: the conjunction's later filter refines a selective first one by
     * compacting its predicate column (exec_node OP_FILTER -> sel_compact),
     * and the v2 group over fewer survivors than the parallel threshold
     * compacts its key (AGG_RUN_COMPACT_FALLBACK -> agg_build_compact):
     * both gather by range since P3 Task 6, per segment on a parted table */
    SR_Q39,                                                                               /* q39 */
    /* the v2 compact fallback alone: a filtered group-by under the parallel
     * threshold gathers its key and input by range (agg_build_compact) */
    SR_COMPACT_GROUP,
    /* a conjunctive where: is counted off its selection (Task 15b) */
    SR_AND_COUNT,
    /* q41: TraficSourceID IN (-1, 6) later in a chain of == conjuncts: on a
     * parted table the `in` conjunct reads each segment by range and is
     * intersected with the selection (exec_in_parted_selection, P3 Task 12) */
    "(select {h: h d: d c: (count h) from: T where: (and (== g 3) (>= d 100) (<= d 400) (== b false) "
    "(in u [1 6]) (!= h 0)) by: {h: h d: d} desc: c take: 10})",                         /* q41 */
    NULL,
};

/* Each shape answers as over generation 0 with `want` materializations on
 * the generation-2 table (dirs t0 / t2, written as T); at_first_exec is
 * latched only by ray_execute (scalar and filtered grouped shapes
 * pre-execute through exec_node, P1 Task 33), so it is checked only where
 * it latched (M8). */
static test_result_t sr_battery(const char* t0, const char* t2, const char* const* shapes, uint64_t want,
                                const char* skip) {
    for (int i = 0; shapes[i]; i++) {
        if (skip && strcmp(shapes[i], skip) == 0) continue;
        test_result_t r = sr_same_on(t0, t2, shapes[i], want);
        if (r.status != TEST_PASS) return r;
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        TEST_ASSERT_FMT(k.at_first_exec == 0 || k.at_first_exec == UINT64_MAX,
                        "%s: %llu materializations before execution", shapes[i],
                        (unsigned long long)k.at_first_exec);
    }
    PASS();
}

/* A shape and its materialization count on generation 2. */
typedef struct { const char* q; uint64_t mat; } sr_gap_t;

/* Review focus 5: every scan-only shape, planning included, leaves the
 * materialization counter at zero on a generation-2 table; the binding
 * assert is the final count (sr_same).  Then the ClickBench scan-only set
 * (the former gaps among it, at zero) and the row-returning filter: both
 * ray_execute of a bare FILTER (the API caller, Task 5) and a language
 * select that returns the filtered table compact the selection through
 * sel_compact, which gathers every column by range (P3 Task 6): nothing
 * materializes, as when counting the same filter (the first shape). */
static test_result_t test_no_materialization(void) {
    static const char* const shapes[] = {
        "(count (select {from: T where: (!= g 0)}))",
        "(count (select {from: T where: (> v 1000)}))",
        "(select {c: (count k) from: T where: (and (>= k 1000) (< k 150000) (== b true))})",
        "(select {s: (sum v) a: (avg f) mn: (min d) mx: (max u) from: T})",
        "(sum (at T 'v))",
        "(avg (at T 'f))",
        "(min (at T 'd))",
        "(select {s: (sum v) c: (count k) from: T by: g})",
        "(select {c: (count k) from: T where: (> v 100) by: {g: g s: s}})",
        "(select {c: (count k) from: T by: k})",
        "(select {c: (count k) from: T where: (== h 1000004)})",
        "(select {s: (sum (+ k 5)) t: (sum (* f 2.0)) from: T where: (< g 4)})",
        "(select {l: (avg (strlen s)) from: T})",
        NULL,
    };
    const char* t0 = "(.db.splayed.get \"" SR_G0 "\")";
    const char* t2 = "(.db.splayed.get \"" SR_G2 "\")";
    test_result_t r = sr_battery(t0, t2, shapes, 0, NULL);
    if (r.status == TEST_PASS) r = sr_battery(t0, t2, sr_clickbench_scan, 0, NULL);
    if (r.status != TEST_PASS) return r;

    uint64_t mat[2] = { 0, 0 };
    int64_t rows[2] = { -1, -1 };
    for (int lang = 0; lang < 2; lang++) {
        ray_t* t = sr_load(1);
        TEST_ASSERT_NOT_NULL(t);
        if (lang) {
            ray_env_set(ray_sym_intern("T", 1), t);
            ray_enc_counters_reset();
            ray_t* q = sr_eval("(select {from: T where: (> v 100)})");
            TEST_ASSERT_TRUE(q && !RAY_IS_ERR(q) && q->type == RAY_TABLE);
            rows[lang] = ray_table_nrows(q);
            sr_drop(q);
        } else {
            ray_graph_t* g = ray_graph_new(t);
            ray_op_t* root = ray_filter(g, ray_const_table(g, t), cpred_v_gt(g));
            ray_enc_counters_reset();
            ray_t* q = ray_execute(g, ray_optimize(g, root));
            TEST_ASSERT_TRUE(q && !RAY_IS_ERR(q) && q->type == RAY_TABLE);
            rows[lang] = ray_table_nrows(q);
            sr_drop(q); ray_graph_free(g);
        }
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        mat[lang] = k.materialized;
        ray_release(t);
    }
    TEST_ASSERT_EQ_I(rows[0], rows[1]);
    TEST_ASSERT_FMT(mat[0] == 0, "ray_execute of a bare FILTER: %llu materializations, want 0",
                    (unsigned long long)mat[0]);
    TEST_ASSERT_FMT(mat[1] == 0, "select of the same filter: %llu materializations, want 0",
                    (unsigned long long)mat[1]);
    PASS();
}

/* The parted battery tables: three partitions of eight columns, written as
 * generation 0 (np0) and generation 2 (np2). */
static void sr_np_tables(void) {
    for (int gen = 0; gen <= 2; gen += 2)
        for (int p = 0; p < 3; p++)
            sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/np%d/2024.01.0%d/t/\" "
                        "(table [k g v d s b u h] (list (+ (til 70000) %d) (as 'I16 (%% (til 70000) 7)) "
                        "(as 'I32 (map (fn [j] (if (== 0 (%% j 10)) 0N (- (* 3 j) 7))) (til 70000))) "
                        "(as 'DATE (%% (til 70000) 900)) (take ['a 'b 'c 'd 'e] 70000) (== 0 (%% (til 70000) 3)) "
                        "(as 'U8 (%% (til 70000) 200)) (%% (* (+ (til 70000) %d) 7919) 1000003))) "
                        "\"" SR_TMP "/np%d/.sym\" {compress: %s})",
                        gen, p + 1, p * 70000, p * 70000, gen, gen ? "true" : "false");
}
#define SR_NP0 "(.db.parted.get \"" SR_TMP "/np0\" 't)"
#define SR_NP2 "(.db.parted.get \"" SR_TMP "/np2\" 't)"

/* The parted arm of the gate: the same shapes over three partitions of a
 * generation-2 parted table (the in-place routes) against generation 0. */
static test_result_t test_no_materialization_parted(void) {
    sr_np_tables();
    static const char* const shapes[] = {
        "(count (select {from: T where: (!= g 0)}))",
        "(sum (at T 'v))",
        "(select {s: (sum v) c: (count k) from: T by: g})",
        "(select {c: (count k) from: T where: (> v 100) by: {g: g s: s}})",
        "(select {c: (count k) from: T by: k})",
        /* q41's `in` conjunct where the optimizer puts it first, and after a
         * conjunct that keeps most rows (the chain's full-width step):
         * each segment by range (exec_in_parted_selection, P3 Task 12) */
        "(count (select {from: T where: (in u [1 6])}))",
        "(select {c: (count h) from: T where: (and (!= g 0) (in u [1 6])) by: d desc: c take: 10})",
        NULL,
    };
    const char* t0 = SR_NP0;
    const char* t2 = SR_NP2;
    test_result_t r = sr_battery(t0, t2, shapes, 0, NULL);
    if (r.status == TEST_PASS) r = sr_battery(t0, t2, sr_clickbench_scan, 0, NULL);
    return r;
}

/* Task 17e (#769): a parted select that groups after a where: gathers the
 * selected rows of the columns its by: and outputs read, not of every
 * column.  The ClickBench q11, q14 and q23 shapes (a count(distinct)
 * output, so the where: runs as its own filter: the materialize path) and
 * q12 (an implicit group under an outer projection: the eval-level group)
 * over the parted battery tables gather exactly their own columns on
 * generation 2, by range since P3 Task 6 (nothing materializes, and the
 * columns they never read decode nothing), the where:-only columns read in
 * place by the filter.  Shapes whose reads cannot be named keep every
 * column and answer as before: a lambda output, the implicit-all group
 * (first of every column), a computed key; and an all-pass filter, a
 * filter that passes nothing, and a nested group whose consumer names
 * columns answer as generation 0. */
static test_result_t test_parted_eval_group_keep(void) {
    sr_np_tables();
    /* each shape's materializations, and the columns it never reads: they
     * decode nothing, so the keep set still narrows the compaction */
    static const struct { const char* q; uint64_t mat; const char* unread; } own[] = {
        /* q11: key s, distinct h; v in the where: only */
        { "(select {s: s u: (count (distinct h)) from: T where: (> v 3) by: s desc: u take: 10})", 0, "kgdbu" },
        /* q12: the inner implicit group feeds an outer that reads g, s, h */
        { "(select {c: (count h) from: (select {from: T where: (!= u 0) by: {g: g s: s h: h}}) "
          "by: {g: g s: s} desc: c take: 10})", 0, "kvdb" },
        /* q14: key g, distinct k; u in the where: only */
        { "(select {g: g u: (count (distinct k)) from: T where: (!= u 0) by: g desc: u take: 10})", 0, "vdsbh" },
        /* q23: four outputs over s, h, k; g and v in the where: only */
        { "(select {s: s u: (count h) t: (count k) c: (count s) d: (count (distinct h)) from: T "
          "where: (and (!= g 1) (> v 3)) by: s desc: c take: 10})", 0, "dbu" },
        /* an all-pass filter flattens the same columns */
        { "(select {g: g u: (count (distinct k)) m: (max d) from: T where: (>= u 0) by: g})", 9, "" },
        { NULL, 0, NULL },
    };
    for (int i = 0; own[i].q; i++) {
        test_result_t r = sr_same_on(SR_NP0, SR_NP2, own[i].q, own[i].mat);
        if (r.status != TEST_PASS) return r;
        for (const char* c = own[i].unread; *c; c++) {     /* T: the generation-2 table */
            char nm[2] = { *c, 0 };
            uint64_t full = 0, dec = sr_col_dec_any(nm, &full);
            TEST_ASSERT_FMT(full > 0 && dec == 0, "%s: unread column %s decoded %llu bytes", own[i].q, nm,
                            (unsigned long long)dec);
        }
    }
    static const char* const all[] = {
        "(select {s: s u: (count (distinct h)) w: ((fn [x] (count x)) k) from: T where: (> v 3) by: s})",
        "(select {from: T where: (> v 3) by: [g s]})",
        "(select {from: T where: (> v 3) by: g})",
        "(select {u: (count (distinct h)) from: T where: (> v 3) by: (% k 3)})",
        "(select {g: g u: (count (distinct k)) from: T where: (> u 250) by: g})",
        "(select {m: (max u) from: (select {from: T where: (> v 3) by: {g: g s: s}}) by: g})",
        NULL,
    };
    for (int i = 0; all[i]; i++) {
        test_result_t r = sr_same_on(SR_NP0, SR_NP2, all[i], UINT64_MAX);
        if (r.status != TEST_PASS) return r;
    }
    /* Fix round 1: an implicit group under an outer projection hint keeps
     * every column when it sorts or takes (only from/where/by narrow it,
     * as filt_compact_keep requires): the sort column, an outer lambda's
     * dynamic read and a quoted name are not in proj_keep.  Expected values
     * are 80a28e49's answers over the generation-0 tables; both
     * generations must give them. */
    static const struct { const char* q; const char* want; } base_ans[] = {
        { "(at (select {g: g from: (select {from: T where: (> v 3) by: [g s] desc: d take: 3})}) 'g)", "[2 6 3]" },
        { "(sum (at (select {g: g from: (select {from: T where: (> v 3) by: [g s] desc: d})}) 'g))", "105" },
        { "(count (select {g: g from: (select {from: T where: (> v 3) by: [g s] desc: d})}))", "35" },
        { "(at (select {g: g from: (select {from: T where: (> v 3) by: {g: g s: s} asc: k take: 3})}) 'g)", "[4 5 6]" },
        { "(at (select {g: g from: (select {from: T where: (> v 3) by: [g s] asc: [u k] take: 3})}) 'g)", "[4 5 6]" },
        { "(at (select {c: (count (distinct g)) from: (select {from: T where: (> v 3) by: [g s] desc: d take: 3})}) 'c)", "[3]" },
        { "(do (set fu (fn [z] (+ z u))) "
          "(at (select {m: (sum (fu g)) from: (select {from: T where: (> v 3) by: [g s] take: 5})}) 'm))", "[46]" },
        { "(at (select {m: (sum 'u) from: (select {from: T where: (> v 3) by: [g s] take: 5})}) 'm)", "[30]" },
        /* the narrowed shape (from/where/by only) answers as before */
        { "(at (select {g: g from: (select {from: T where: (> v 3) by: [g s]})}) 'g)",
          "[4 5 6 0 1 2 4 5 6 0 1 2 3 4 5 0 1 2 3 4 5 6 0 1 3 4 5 6 0 1 2 3 3 6 2]" },
        { NULL, NULL },
    };
    for (int gen = 0; gen < 2; gen++) {
        sr_evalf_ok("(set T %s)", gen ? SR_NP2 : SR_NP0);
        for (int i = 0; base_ans[i].q; i++) {
            ray_t* got = sr_eval(base_ans[i].q);
            ray_t* want = sr_eval(base_ans[i].want);
            bool same = sr_same_text(got, want);
            ray_t* fg = (got && !RAY_IS_ERR(got)) ? ray_fmt(got, 0) : NULL;
            TEST_ASSERT_FMT(same, "gen %d: %s: %.*s, base %s", gen ? 2 : 0, base_ans[i].q,
                            fg ? (int)ray_str_len(fg) : 5, fg ? ray_str_ptr(fg) : (got ? ray_err_code(got) : "null"),
                            base_ans[i].want);
            if (fg) ray_release(fg);
            sr_drop(got); ray_release(want);
        }
    }
    PASS();
}

/* Gap 5 (Task 15b): a count over a conjunctive filter is answered from the
 * selection, gathering no column, on splayed and parted tables of either
 * generation.  Each count equals the row count of the select the shortcut
 * stands for (the gathering select, read through `at`), on both
 * generations; conjunctions with nullable columns, SYM equality and SYM
 * order (#738), three conjuncts and an all-false one are covered.  The
 * single-compare shortcut is untouched (test_count_fast_path). */
static test_result_t sr_count_and_on(const char* t0, const char* t2, const char* w, uint64_t want_mat) {
    char q[512], ref[512];
    snprintf(q, sizeof q, "(count (select {from: T where: %s}))", w);
    snprintf(ref, sizeof ref, "(count (at (select {from: T where: %s}) 'k))", w);
    test_result_t r = sr_same_on(t0, t2, q, want_mat);
    if (r.status != TEST_PASS) return r;
    for (int gen = 0; gen < 2; gen++) {
        sr_evalf_ok("(set T %s)", gen ? t2 : t0);
        ray_t* a = sr_eval(q);
        ray_t* b = sr_eval(ref);
        TEST_ASSERT_FMT(a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b) && a->type == -RAY_I64 &&
                        b->type == -RAY_I64 && a->i64 == b->i64,
                        "gen %d %s: %lld, the select has %lld rows", gen, w,
                        (long long)(a && !RAY_IS_ERR(a) ? a->i64 : -1),
                        (long long)(b && !RAY_IS_ERR(b) ? b->i64 : -1));
        sr_drop(a); sr_drop(b);
    }
    PASS();
}
/* A conjunction the shortcut must leave to the select (Fix round 1): the
 * count equals the gathering select's row count, or both fail with the same
 * error, on both generations. */
static test_result_t sr_count_and_declines(const char* t0, const char* t2, const char* w) {
    char q[512], ref[512];
    snprintf(q, sizeof q, "(count (select {from: T where: %s}))", w);
    snprintf(ref, sizeof ref, "(count (at (select {from: T where: %s}) 'k))", w);
    for (int gen = 0; gen < 2; gen++) {
        sr_evalf_ok("(set T %s)", gen ? t2 : t0);
        ray_error_clear();
        ray_t* a = sr_eval(q);
        char am[256]; snprintf(am, sizeof am, "%s", (a && RAY_IS_ERR(a) && ray_error_msg()) ? ray_error_msg() : "");
        char ac[64]; snprintf(ac, sizeof ac, "%s", (a && RAY_IS_ERR(a)) ? ray_err_code(a) : "");
        ray_error_clear();
        ray_t* b = sr_eval(ref);
        bool ae = !a || RAY_IS_ERR(a), be = !b || RAY_IS_ERR(b);
        if (ae || be) {
            TEST_ASSERT_FMT(ae && be && a && b && strcmp(ac, ray_err_code(b)) == 0 &&
                            strcmp(am, ray_error_msg() ? ray_error_msg() : "") == 0,
                            "gen %d %s: \"%s: %s\", the select \"%s: %s\"", gen, w, ac, am,
                            be && b ? ray_err_code(b) : "-", be && ray_error_msg() ? ray_error_msg() : "");
        } else {
            TEST_ASSERT_FMT(a->type == -RAY_I64 && b->type == -RAY_I64 && a->i64 == b->i64,
                            "gen %d %s: %lld, the select has %lld rows", gen, w,
                            (long long)a->i64, (long long)b->i64);
        }
        sr_drop(a); sr_drop(b);
    }
    PASS();
}
/* Conjuncts outside the whitelist: constants nested in `or` / `and`, an
 * aggregate (the select's chained filters refine its input), arithmetic,
 * `if`, a non-bool conjunct (the select's error text), an ordered SYM
 * compare, a quoted symbol naming a column, and `in`. */
static const char* const sr_and_declined[] = {
    "(and (> k 2) (or 0.0 (> g 1)))",
    "(and (> k 2) (or (> g 1) 0.0))",
    "(and (> k 2) (and (> g 1) 1.5))",
    "(and (> k 2) (> k (avg k)))",
    "(and (> k 2) (> (+ k 1) 5))",
    "(and (> k 2) (if true (> g 1) (> g 2)))",
    "(and (> k 2) (+ 1 (* 0 k)))",
    "(and (< s 'c) (== b true))",                           /* SYM order is by string (#738) */
    "(and (== s 'b) (> k 1000))",                           /* 'b names a column: a column compare */
    "(and (> k 2) (in g [1 2]))",
    NULL,
};

static test_result_t test_count_and(void) {
    static const char* const splayed[] = {
        "(and (== g 3) (!= u 0))",
        "(and (> v 100) (< f 150000.0))",                    /* nullable I32 and F64 */
        "(and (== g 3) (>= d 100) (<= d 400))",
        "(and (== g 3) (== g 4))",                           /* all false */
        "(and (> k 199990) (== b true) (!= v 0))",
        NULL,
    };
    const char* t0 = "(.db.splayed.get \"" SR_G0 "\")";
    const char* t2 = "(.db.splayed.get \"" SR_G2 "\")";
    for (int i = 0; splayed[i]; i++) {
        test_result_t r = sr_count_and_on(t0, t2, splayed[i], 0);
        if (r.status != TEST_PASS) return r;
    }
    /* A file-domain SYM equality is scan-only since P3 Task 4: the fused
     * program compares the column's own ids against the constant
     * translated into its domain, so nothing is materialized. */
    static const sr_gap_t sym[] = {
        { "(and (== s 'c) (> k 1000))", 0 },
        { NULL, 0 },
    };
    for (int i = 0; sym[i].q; i++) {
        test_result_t r = sr_count_and_on(t0, t2, sym[i].q, sym[i].mat);
        if (r.status != TEST_PASS) return r;
    }
    for (int i = 0; sr_and_declined[i]; i++) {
        test_result_t r = sr_count_and_declines(t0, t2, sr_and_declined[i]);
        if (r.status != TEST_PASS) return r;
    }
    sr_np_tables();
    for (int i = 0; sr_and_declined[i]; i++) {
        test_result_t r = sr_count_and_declines(SR_NP0, SR_NP2, sr_and_declined[i]);
        if (r.status != TEST_PASS) return r;
    }
    static const char* const parted[] = {
        "(and (== g 3) (!= u 0))",
        "(and (> v 100) (< v 150000))",                      /* nullable I32 */
        "(and (== g 3) (== g 4))",
        "(and (>= k 69990) (< k 70010) (!= v 0))",           /* across a partition edge */
        NULL,
    };
    for (int i = 0; parted[i]; i++) {
        test_result_t r = sr_count_and_on(SR_NP0, SR_NP2, parted[i], 0);
        if (r.status != TEST_PASS) return r;
    }
    /* parted: the same, per encoded segment (one domain for every segment) */
    static const sr_gap_t sym_parted[] = {
        { "(and (== s 'c) (> k 1000))", 0 },
        { NULL, 0 },
    };
    for (int i = 0; sym_parted[i].q; i++) {
        test_result_t r = sr_count_and_on(SR_NP0, SR_NP2, sym_parted[i].q, sym_parted[i].mat);
        if (r.status != TEST_PASS) return r;
    }
    PASS();
}

/* Gap 4 (Task 15b): a constant output beside a parted group-by groups the
 * table in place (the segment-streaming route of Task 14) and broadcasts
 * the constant over the result rows; the table is never flattened.  Each
 * shape answers as over generation 0, gathers nothing on generation 2,
 * runs in place on both, and equals the same group-by without its constant
 * outputs with the constants added (their type and length included).  The
 * generation-2 count is the proof that nothing flattened (a flatten reads
 * every segment of every column); the in-place counter shows the constant
 * leaves the bare group-by's route unchanged on both generations. */
static test_result_t test_const_beside_parted_group(void) {
    sr_np_tables();
    static const struct { const char* q; const char* bare; const char* checks; uint64_t mat; } shapes[] = {
        { SR_Q35, "(select {s: s c: (count s) from: T by: s desc: c take: 10})",
          "(and (== (type (at R 'one)) 'I64) (all (== (at R 'one) (take 1 (count R)))))", 0 },
        { "(select {one: 1 f: 2.5 sy: 'x st: \"ab\" dt: 2024.01.02 bo: true c: (count k) from: T by: g})",
          "(select {c: (count k) from: T by: g})",
          "(and (== (type (at R 'f)) 'F64) (== (type (at R 'sy)) 'SYM) (== (type (at R 'st)) 'STR) "
          "(== (type (at R 'dt)) 'DATE) (== (type (at R 'bo)) 'B8) (== (count (at R 'st)) 7) "
          "(all (== (at R 'f) (take 2.5 7))) (all (== (at R 'sy) (take 'x 7))))", 0 },
        { "(select {one: 1 c: (count k) from: T where: (!= u 0) by: s})",
          "(select {c: (count k) from: T where: (!= u 0) by: s})",
          "(all (== (at R 'one) (take 1 5)))", 0 },
        /* no group: the compact fallback gathers the key and input by
         * range (SR_COMPACT_GROUP, P3 Task 6), with or without the
         * constant */
        { "(select {one: 1 c: (count k) from: T where: (== g 9) by: s})",
          "(select {c: (count k) from: T where: (== g 9) by: s})",
          "(and (== (type (at R 'one)) 'I64) (== 0 (count (at R 'one))))", 0 },
        { "(select {c: (sum v) one: 1 from: T by: k desc: c take: 5})",
          "(select {c: (sum v) from: T by: k desc: c take: 5})",
          "(all (== (at R 'one) (take 1 5)))", 0 },
        { "(select {one: (+ 1 2) c: (count k) from: T by: g})",                 /* a scalar expression */
          "(select {c: (count k) from: T by: g})",
          "(and (== (type (at R 'one)) 'LIST) (== 7 (count (at R 'one))) (== 3 (first (at R 'one))))", 0 },
    };
    for (size_t i = 0; i < sizeof shapes / sizeof shapes[0]; i++) {
        int64_t s0 = ray_group_segstream_runs();
        test_result_t r = sr_same_on(SR_NP0, SR_NP2, shapes[i].bare, shapes[i].mat);
        if (r.status != TEST_PASS) return r;
        int64_t s1 = ray_group_segstream_runs();
        r = sr_same_on(SR_NP0, SR_NP2, shapes[i].q, shapes[i].mat);
        if (r.status != TEST_PASS) return r;
        /* the constant leaves the route as it is without it */
        TEST_ASSERT_FMT(ray_group_segstream_runs() - s1 == s1 - s0, "%s: not the bare group's route", shapes[i].q);
        sr_eval_ok("(set T " SR_NP2 ")");
        sr_evalf_ok("(set R %s)", shapes[i].q);
        sr_evalf_ok("(set B %s)", shapes[i].bare);
        ray_t* ok = sr_eval(shapes[i].checks);
        TEST_ASSERT_FMT(ok && ok->type == -RAY_BOOL && ok->b8, "%s: constant columns", shapes[i].q);
        sr_drop(ok);
        /* the other columns are the bare group-by's */
        ray_t* eq = sr_eval("(all (map (fn [c] (and (== (count (at R c)) (count (at B c))) (all (== (at R c) (at B c))))) (key B)))");
        TEST_ASSERT_FMT(eq && eq->type == -RAY_BOOL && eq->b8, "%s: differs from %s", shapes[i].q, shapes[i].bare);
        sr_drop(eq);
    }
    PASS();
}

/* Issue #767: a per-partition group-by divides avg and var/dev by the
 * input's non-null count (it divided by the row count).  Three partitions
 * of 35000 rows, an I64 key of five values (the per-partition route), a
 * nullable F64 and a nullable I64, against the same rows in memory, with
 * and without a where, a constant beside the group, and groups whose every
 * input is null (null averages, as in memory); generation 0 and 2. */
static test_result_t test_parted_nullable_stats(void) {
    for (int gen = 0; gen <= 2; gen += 2)
        for (int p = 0; p < 3; p++)
            sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/ns%d/2024.01.0%d/t/\" "
                        "(table [g x y] (list (%% (til 35000) 5) "
                        "(as 'F64 (map (fn [i] (if (== 0 (%% i 4)) 0N (* 1.0 i))) (til 35000))) "
                        "(map (fn [i] (if (== 0 (%% i 3)) 0N i)) (til 35000)))) "
                        "\"" SR_TMP "/ns%d/.sym\" {compress: %s})", gen, p + 1, gen, gen ? "true" : "false");
    sr_eval_ok("(set jj (concat (til 35000) (concat (til 35000) (til 35000))))");
    sr_eval_ok("(set M (table [g x y] (list (% jj 5) (as 'F64 (map (fn [i] (if (== 0 (% i 4)) 0N (* 1.0 i))) jj)) "
               "(map (fn [i] (if (== 0 (% i 3)) 0N i)) jj))))");
    static const char* const qs[] = {
        "(select {a: (avg x) b: (avg y) from: T by: g asc: g})",
        "(select {v: (var x) w: (var y) vp: (var_pop x) wp: (var_pop y) from: T by: g asc: g})",
        "(select {d: (dev x) e: (dev y) s: (stddev x) t: (stddev y) sp: (stddev_pop y) from: T by: g asc: g})",
        "(select {a: (avg x) b: (avg y) v: (var x) d: (dev y) from: T where: (> y 100) by: g asc: g})",
        "(select {one: 1 a: (avg x) v: (var y) from: T by: g asc: g})",
        "(select {a: (avg x) v: (var x) d: (dev x) c: (count x) from: T where: (== 0 (% y 4)) by: g asc: g})",
        NULL,
    };
    for (int gen = 0; gen <= 2; gen += 2) {
        char tp[256];
        snprintf(tp, sizeof tp, "(.db.parted.get \"" SR_TMP "/ns%d\" 't)", gen);
        for (int i = 0; qs[i]; i++) {
            int64_t r0 = ray_group_perpart_runs();
            int64_t n0 = ray_group_perpart_nn_slots();
            test_result_t r = sr_same_on("M", tp, qs[i], UINT64_MAX);
            if (r.status != TEST_PASS) return r;
            TEST_ASSERT_FMT(ray_group_perpart_runs() > r0, "gen %d %s: not the per-partition route", gen, qs[i]);
            TEST_ASSERT_FMT(ray_group_perpart_nn_slots() > n0, "gen %d %s: nullable input counted by rows", gen, qs[i]);
        }
    }
    /* Fix round 2: a null-free input (g, and z, no null) keeps the row
     * count: no non-null count slot is built, and the answers still equal
     * memory's.  A nullable one beside it builds only its own. */
    sr_eval_ok("(set M (table [g x y z] (list (% jj 5) (as 'F64 (map (fn [i] (if (== 0 (% i 4)) 0N (* 1.0 i))) jj)) "
               "(map (fn [i] (if (== 0 (% i 3)) 0N i)) jj) (as 'F64 jj))))");
    for (int gen = 0; gen <= 2; gen += 2)
        for (int p = 0; p < 3; p++)
            sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/nz%d/2024.01.0%d/t/\" "
                        "(table [g x y z] (list (%% (til 35000) 5) "
                        "(as 'F64 (map (fn [i] (if (== 0 (%% i 4)) 0N (* 1.0 i))) (til 35000))) "
                        "(map (fn [i] (if (== 0 (%% i 3)) 0N i)) (til 35000)) (as 'F64 (til 35000)))) "
                        "\"" SR_TMP "/nz%d/.sym\" {compress: %s})", gen, p + 1, gen, gen ? "true" : "false");
    static const struct { const char* q; int64_t nn; } zs[] = {
        { "(select {a: (avg z) b: (avg g) v: (var z) d: (dev g) from: T by: g asc: g})", 0 },
        { "(select {a: (avg z) v: (var_pop z) from: T where: (> z 100.0) by: g asc: g})", 0 },
        { "(select {a: (avg z) b: (avg x) from: T by: g asc: g})", 3 },      /* x's slot, per partition */
        { NULL, 0 },
    };
    for (int gen = 0; gen <= 2; gen += 2) {
        char tp[256];
        snprintf(tp, sizeof tp, "(.db.parted.get \"" SR_TMP "/nz%d\" 't)", gen);
        for (int i = 0; zs[i].q; i++) {
            int64_t r0 = ray_group_perpart_runs();
            int64_t n0 = ray_group_perpart_nn_slots();
            test_result_t r = sr_same_on("M", tp, zs[i].q, UINT64_MAX);
            if (r.status != TEST_PASS) return r;
            TEST_ASSERT_FMT(ray_group_perpart_runs() > r0, "gen %d %s: not the per-partition route", gen, zs[i].q);
            /* sr_same_on runs the query once on the parted table */
            TEST_ASSERT_FMT(ray_group_perpart_nn_slots() - n0 == zs[i].nn, "gen %d %s: %lld non-null count slots, want %lld",
                            gen, zs[i].q, (long long)(ray_group_perpart_nn_slots() - n0), (long long)zs[i].nn);
        }
    }
    PASS();
}

/* Task 17d: a per-partition avg over an expression builds a non-null
 * count slot only when the expression can yield a null.  `avg`/`sum` of
 * (strlen c) reads a null cell as length 0 on every grouped route (the
 * inline strlen), so it keeps the row count; a cast of a null-free integer
 * to F64 is null-free.  Arithmetic over strlen and var/dev of strlen
 * (evaluated, a null cell a null length) still count non-nulls.  Answers
 * equal the same rows in memory, generation 0 and 2. */
static test_result_t test_parted_expr_null_free(void) {
    for (int gen = 0; gen <= 2; gen += 2)
        for (int p = 0; p < 3; p++)
            sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/ne%d/2024.01.0%d/t/\" "
                        "(table [g s y z] (list (%% (til 35000) 5) "
                        "(map (fn [i] (if (== 0 (%% i 4)) \"\" (if (== 0 (%% i 3)) \"abcdef\" \"ab\"))) (til 35000)) "
                        "(map (fn [i] (if (== 0 (%% i 6)) (as 'SYM \"\") (if (== 0 (%% i 5)) 'xyz 'q))) (til 35000)) "
                        "(til 35000))) "
                        "\"" SR_TMP "/ne%d/.sym\" {compress: %s})", gen, p + 1, gen, gen ? "true" : "false");
    sr_eval_ok("(set jj (concat (til 35000) (concat (til 35000) (til 35000))))");
    sr_eval_ok("(set M (table [g s y z] (list (% jj 5) "
               "(map (fn [i] (if (== 0 (% i 4)) \"\" (if (== 0 (% i 3)) \"abcdef\" \"ab\"))) jj) "
               "(map (fn [i] (if (== 0 (% i 6)) (as 'SYM \"\") (if (== 0 (% i 5)) 'xyz 'q))) jj) jj)))");
    static const struct { const char* q; int64_t nn; } qs[] = {
        { "(select {a: (avg (strlen s)) from: T by: g asc: g})", 0 },
        { "(select {a: (avg (strlen y)) b: (sum (strlen s)) c: (count s) from: T by: g asc: g})", 0 },
        { "(select {a: (avg (strlen s)) from: T where: (!= s \"\") by: g asc: g})", 0 },
        { "(select {a: (avg (as 'F64 z)) b: (avg (as 'I64 g)) from: T by: g asc: g})", 0 },
        { "(select {a: (avg (+ 1 (strlen s))) from: T by: g asc: g})", 3 },   /* one slot per partition */
        { "(select {v: (var (strlen s)) from: T by: g asc: g})", 3 },
        { NULL, 0 },
    };
    for (int gen = 0; gen <= 2; gen += 2) {
        char tp[256];
        snprintf(tp, sizeof tp, "(.db.parted.get \"" SR_TMP "/ne%d\" 't)", gen);
        for (int i = 0; qs[i].q; i++) {
            int64_t r0 = ray_group_perpart_runs();
            int64_t n0 = ray_group_perpart_nn_slots();
            test_result_t r = sr_same_on("M", tp, qs[i].q, UINT64_MAX);
            if (r.status != TEST_PASS) return r;
            TEST_ASSERT_FMT(ray_group_perpart_runs() > r0, "gen %d %s: not the per-partition route", gen, qs[i].q);
            TEST_ASSERT_FMT(ray_group_perpart_nn_slots() - n0 == qs[i].nn, "gen %d %s: %lld non-null count slots, want %lld",
                            gen, qs[i].q, (long long)(ray_group_perpart_nn_slots() - n0), (long long)qs[i].nn);
        }
    }
    /* Fix round 1: the pre-range-read base (80a28e49) answers this parted
     * avg over the rows, an empty string a length-0 row: 2.5, 2.5,
     * 2.4994285714, 2.5005714286, 2.5 (base's (* 1e7 a) as I64:
     * 25000000 25000000 24994285 25005714 25000000).  Generation 0 and 2
     * give exactly that value, per partition. */
    int64_t lsum[5] = { 0 }, lcnt[5] = { 0 };
    for (int p = 0; p < 3; p++)
        for (int64_t i = 0; i < 35000; i++) {
            lsum[i % 5] += i % 4 == 0 ? 0 : i % 3 == 0 ? 6 : 2;
            lcnt[i % 5]++;
        }
    for (int gen = 0; gen <= 2; gen += 2) {
        sr_evalf_ok("(set T (.db.parted.get \"" SR_TMP "/ne%d\" 't))", gen);
        int64_t r0 = ray_group_perpart_runs();
        ray_t* a = sr_eval("(at (select {a: (avg (strlen s)) from: T by: g asc: g}) 'a)");
        TEST_ASSERT_FMT(ray_group_perpart_runs() > r0, "gen %d: not the per-partition route", gen);
        TEST_ASSERT_FMT(a && !RAY_IS_ERR(a) && a->type == RAY_F64 && a->len == 5, "gen %d: avg (strlen s)", gen);
        for (int k = 0; k < 5; k++)
            TEST_ASSERT_FMT(((double*)ray_data(a))[k] == (double)lsum[k] / (double)lcnt[k],
                            "gen %d group %d: %.17g, base %.17g", gen, k, ((double*)ray_data(a))[k],
                            (double)lsum[k] / (double)lcnt[k]);
        sr_drop(a);
    }
    PASS();
}

/* Task 17g: sum / avg of (strlen s) over a generation-2 STR column give
 * generation 0's answers and the same rows' in memory on every route: the
 * scalar route, the normal routing (the v2 engine declines a strlen input:
 * the legacy ladder), the direct array, legacy radix (a wide key), under a
 * selection, beside a staged SYM strlen, and per partition.  The encoded
 * STR is read through its plain column (one materialization); declining it
 * left the input to the expression path, whose strlen makes an empty
 * string (the STR null) a null that the average skipped, where the scalar
 * and direct-array routes count it as length 0 (checked against the
 * reference below).  With the direct array skipped, the legacy ladder's
 * divisor skips the STR null on every source alike (pre-existing; only the
 * sources' agreement is checked there).  The lengths differ by group, so a
 * mixed-up group shows. */
static int64_t sr_strlen_len(int64_t r) {
    switch (r % 3) {
    case 0:  return (r / 3) % 2 ? 0 : 4;
    case 1:  return r % 5;
    default: return (r / 7) % 3 == 0 ? 0 : 7;
    }
}
static test_result_t test_strlen_str_gen2_routes(void) {
    const int64_t n = 70000;
    static const char abc[] = "abcdefg";
    ray_t* sv = ray_vec_new(RAY_STR, n); sv->len = 0;
    int64_t gsum[3] = { 0 }, gcnt[3] = { 0 }, tsum = 0;
    for (int64_t r = 0; r < n; r++) {
        int64_t l = sr_strlen_len(r);
        sv = ray_str_vec_append(sv, abc, (size_t)l);
        gsum[r % 3] += l; gcnt[r % 3]++; tsum += l;
    }
    ray_env_set(ray_sym_intern("slv", 3), sv); ray_release(sv);
    sr_eval_ok("(set i (til 70000))");
    sr_eval_ok("(set M (table [g h s y] (list (% i 3) (% (* i 7919) 50021) slv (as 'SYM slv))))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/sl0/\" M {compress: false})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/sl2/\" M {compress: true})");
    const char* t0 = "(.db.splayed.get \"" SR_TMP "/sl0/\")";
    const char* t2 = "(.db.splayed.get \"" SR_TMP "/sl2/\")";
    /* the reference itself on the scalar and direct-array routes: an empty
     * string is a length-0 row */
    for (int k3 = 0; k3 < 6; k3++) {
        int gen = k3 % 3;
        ray_group_test_route(k3 / 3);
        sr_evalf_ok("(set T %s)", gen == 0 ? "M" : gen == 1 ? t0 : t2);
        ray_t* a = sr_eval("(at (select {a: (avg (strlen s)) from: T by: g asc: g}) 'a)");
        TEST_ASSERT_FMT(a && !RAY_IS_ERR(a) && a->type == RAY_F64 && a->len == 3, "route %d source %d: grouped avg", k3 / 3, gen);
        for (int k = 0; k < 3; k++)
            TEST_ASSERT_FMT(fabs(((double*)ray_data(a))[k] - (double)gsum[k] / (double)gcnt[k]) < 1e-9,
                            "route %d source %d group %d: %.17g, want %.17g", k3 / 3, gen, k, ((double*)ray_data(a))[k],
                            (double)gsum[k] / (double)gcnt[k]);
        sr_drop(a);
        ray_t* s = sr_eval("(first (at (select {a: (avg (strlen s)) from: T}) 'a))");
        ray_group_test_route(0);
        TEST_ASSERT_FMT(s && !RAY_IS_ERR(s) && s->type == -RAY_F64 &&
                        fabs(s->f64 - (double)tsum / (double)n) < 1e-9, "source %d: scalar avg", gen);
        sr_drop(s);
    }
    static const struct { const char* q; uint64_t mat; } qs[] = {
        { "(select {a: (avg (strlen s)) b: (sum (strlen s)) c: (count s) from: T by: g asc: g})", 1 },
        { "(select {a: (avg (strlen s)) b: (sum (strlen s)) c: (count s) from: T})", 1 },
        { "(select {a: (avg (strlen s)) from: T where: (> g 0)})", 1 },
        { "(select {a: (avg (strlen s)) b: (sum (strlen s)) from: T where: (!= g 1) by: g asc: g})", 1 },
        { "(select {a: (avg (strlen s)) l: (avg (strlen y)) b: (sum (strlen y)) from: T by: g asc: g})", 1 },
        { "(select {a: (avg (strlen s)) c: (count g) from: T by: h asc: h})", 1 },   /* wide key */
        { "(select {mn: (min (strlen s)) mx: (max (strlen s)) a: (avg (strlen s)) from: T by: g asc: g})", 1 },
        { NULL, 0 },
    };
    for (int route = 0; route <= 2; route++) {
        for (int i = 0; qs[i].q; i++) {
            ray_group_test_route(route);
            agg_route_reset();
            test_result_t r = sr_same_on(t0, t2, qs[i].q, qs[i].mat);
            if (r.status == TEST_PASS) r = sr_same_on("M", t2, qs[i].q, UINT64_MAX);
            agg_route_stats_t st = agg_route_stats();
            ray_group_test_route(0);
            if (r.status != TEST_PASS) return r;
            if (route) TEST_ASSERT_FMT(st.routes[AGG_ROUTE_LEGACY] > 0, "route %d %s: not the legacy ladder",
                                       route, qs[i].q);
        }
    }
    /* per partition: three partitions of the same rows */
    for (int gen = 0; gen <= 2; gen += 2)
        for (int p = 0; p < 3; p++)
            sr_evalf_ok("(.db.splayed.set \"" SR_TMP "/slp%d/2024.01.0%d/t/\" M \"" SR_TMP "/slp%d/.sym\" "
                        "{compress: %s})", gen, p + 1, gen, gen ? "true" : "false");
    sr_eval_ok("(set M3 (table [g h s y] (list (concat (concat (% i 3) (% i 3)) (% i 3)) "
               "(concat (concat (at M 'h) (at M 'h)) (at M 'h)) (concat (concat slv slv) slv) "
               "(concat (concat (at M 'y) (at M 'y)) (at M 'y)))))");
    for (int gen = 0; gen <= 2; gen += 2) {
        char tp[256];
        snprintf(tp, sizeof tp, "(.db.parted.get \"" SR_TMP "/slp%d\" 't)", gen);
        for (int i = 0; qs[i].q; i++) {
            int64_t r0 = ray_group_perpart_runs();
            test_result_t r = sr_same_on("M3", tp, qs[i].q, UINT64_MAX);
            if (r.status != TEST_PASS) return r;
            if (strstr(qs[i].q, "by: g"))
                TEST_ASSERT_FMT(ray_group_perpart_runs() > r0, "gen %d %s: not the per-partition route", gen, qs[i].q);
        }
    }
    PASS();
}

/* Task 17d: a ragged generation-0 table's synthetic null segment is
 * decided from its statistics, never decoded.  A compare over the column
 * fills its rows from the all-null rule (the predicate and the count of a
 * select); the in-place group reads the plain segments where they are and
 * the stand-in as a fill of nulls.  Answers equal the explicit-null
 * reference, nothing is decoded or materialized. */
static test_result_t test_ragged_gen0_no_decode(void) {
    sr_parted_xy(SR_TMP "/rnd", 0, true, false);
    sr_parted_xy(SR_TMP "/rnf", 0, false, true);
    /* skips: the stand-in's 70000 rows are 69 morsels, each decided by the
     * all-null rule (the real partitions' y spans 0..96 in every chunk, so
     * no other morsel is decided); fill: the group's null fill, 70000 I64 */
    static const char* const q[] = {
        "(count (select {from: T where: (!= y 0)}))",
        "(count (select {from: T where: (== y 5)}))",
        "(count (select {from: T where: (< y 3)}))",
        "(count (select {from: T where: (<= y 3)}))",
        "(count (select {from: T where: (> y 3)}))",
        "(count (select {from: T where: (>= y 3)}))",
        "(count (select {from: T where: (and (> x 100) (!= y 7))}))",
        "(select {s: (sum x) from: T where: (< y 50)})",
        "(select {c: (count y) from: T where: (!= y 0) by: y desc: c take: 5})",
        "(select {c: (count x) s: (sum x) from: T by: y asc: y take: 3})",
        "(select {s: (sum y) c: (count y) from: T by: x desc: s take: 3})",
        "(select {s: (sum x) from: T by: y desc: s take: 4})",
        /* a null constant: the zone rule leaves every morsel mixed (ZT_MIXED),
         * the stand-in included; the compare reads the stand-in's nulls (its
         * materialization: the decode pins do not apply to these three) */
        "(count (select {from: T where: (== y 0Nl)}))",
        "(count (select {from: T where: (!= y 0Nl)}))",
        "(count (select {from: T where: (and (> x 100) (== y 0Nl))}))",
        NULL,
    };
    static const int64_t skips[] = { 69, 69, 69, 69, 69, 69, 0, 69, 0, 0, 0, 0, 0, 0, 0 };
    static const int64_t fill[]  = { 0, 0, 0, 0, 0, 0, 0, 0, 560000, 560000, 560000, 560000, 0, 0, 0 };
    for (int i = 0; q[i]; i++) {
        sr_eval_ok("(set T (.db.parted.get \"" SR_TMP "/rnf\" 't))");
        ray_t* r0 = sr_eval(q[i]);
        sr_eval_ok("(set T (.db.parted.get \"" SR_TMP "/rnd\" 't))");
        ray_enc_counters_reset();
        int64_t z0 = ray_expr_test_zone_skips(), f0 = ray_group_null_fill_bytes();
        ray_t* r1 = sr_eval(q[i]);
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        TEST_ASSERT_FMT(sr_same_text(r0, r1), "ragged: %s", q[i]);
        TEST_ASSERT_FMT(i >= 12 || (k.decoded_bytes == 0 && k.materialized == 0), "%s: %llu bytes decoded, %llu materialized",
                        q[i], (unsigned long long)k.decoded_bytes, (unsigned long long)k.materialized);
        int64_t dz = ray_expr_test_zone_skips() - z0, df = ray_group_null_fill_bytes() - f0;
        TEST_ASSERT_FMT(dz == skips[i], "%s: %lld morsels decided from statistics, want %lld", q[i],
                        (long long)dz, (long long)skips[i]);
        TEST_ASSERT_FMT(df == fill[i], "%s: %lld bytes of null fill, want %lld", q[i], (long long)df, (long long)fill[i]);
        sr_drop(r0); sr_drop(r1);
    }
    /* Fix round 1: the all-null rules trust only a synthetic stand-in.  The
     * same nulls saved encoded (the reference's third partition, generation
     * 2) are decoded, and so validated, though their directory says every
     * row is null: no morsel is decided by the rule, and the answer is the
     * same. */
    sr_parted_xy(SR_TMP "/rne", 4, false, true);
    for (int i = 0; i < 6; i++) {
        sr_eval_ok("(set T (.db.parted.get \"" SR_TMP "/rnf\" 't))");
        ray_t* r0 = sr_eval(q[i]);
        sr_eval_ok("(set T (.db.parted.get \"" SR_TMP "/rne\" 't))");
        ray_enc_counters_reset();
        int64_t z0 = ray_expr_test_zone_skips();
        ray_t* r1 = sr_eval(q[i]);
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        TEST_ASSERT_FMT(sr_same_text(r0, r1), "saved nulls: %s", q[i]);
        /* P3 Task 3: the all-null rule decides no morsel; the predicate's
         * vectors are viewed instead of decoded (an encoded leaf), and a
         * view makes every check that decode would: nothing is decoded or
         * materialized */
        TEST_ASSERT_FMT(ray_expr_test_zone_skips() == z0 && k.decoded_bytes == 0 && k.materialized == 0,
                        "saved nulls %s: %lld morsels decided, %llu bytes decoded, %llu materialized", q[i],
                        (long long)(ray_expr_test_zone_skips() - z0), (unsigned long long)k.decoded_bytes,
                        (unsigned long long)k.materialized);
        sr_drop(r0); sr_drop(r1);
    }
    PASS();
}

/* Fix round 1 of Task 17d: the group's null fill for a ragged stand-in is
 * transient memory of rows x width, so one query builds at most the group's
 * transient budget (AGG_COMPACT_BITMAP_BUDGET) of it.  A stand-in of more
 * rows than that holds of I64 is grouped by the piece walk instead: no fill
 * byte, nothing materialized, the explicit-null reference's answers, on the
 * in-place group and per partition (each query's route pinned).  The budget
 * is the query's, shared by its columns: two stand-ins that each fit but not
 * together get one fill, the other is walked. */
static test_result_t sr_fill_route(const char* ref, const char* dut, const char* q, bool in_place,
                                   int64_t want_fill) {
    sr_evalf_ok("(set T (.db.parted.get \"%s\" 't))", ref);
    ray_t* r0 = sr_eval(q);
    sr_evalf_ok("(set T (.db.parted.get \"%s\" 't))", dut);
    ray_enc_counters_reset();
    int64_t f0 = ray_group_null_fill_bytes(), s0 = ray_group_segstream_runs(), p0 = ray_group_perpart_runs();
    ray_t* r1 = sr_eval(q);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    int64_t ds = ray_group_segstream_runs() - s0, dp = ray_group_perpart_runs() - p0;
    TEST_ASSERT_FMT(sr_same_text(r0, r1), "ragged: %s", q);
    TEST_ASSERT_FMT(ray_group_null_fill_bytes() - f0 == want_fill, "%s: %lld bytes of null fill, want %lld", q,
                    (long long)(ray_group_null_fill_bytes() - f0), (long long)want_fill);
    TEST_ASSERT_FMT(k.materialized == 0, "%s: %llu materialized", q, (unsigned long long)k.materialized);
    TEST_ASSERT_FMT(in_place ? (ds == 1 && dp == 0) : (ds == 0 && dp == 1), "%s: in place %lld, per partition %lld",
                    q, (long long)ds, (long long)dp);
    sr_drop(r0); sr_drop(r1);
    PASS();
}
static test_result_t test_ragged_fill_bounded(void) {
    const int64_t big = (int64_t)(AGG_COMPACT_BITMAP_BUDGET / sizeof(int64_t)) + 65536;
    for (int ref = 0; ref < 2; ref++) {
        const char* root = ref ? SR_TMP "/rbf" : SR_TMP "/rbd";
        sr_evalf_ok("(.db.splayed.set \"%s/2024.01.01/t/\" (table [x k y] (list (%% (til 70000) 7) (til 70000) "
                    "(%% (til 70000) 97))) {compress: false})", root);
        sr_evalf_ok("(.db.splayed.set \"%s/2024.01.02/t/\" %s {compress: false})", root,
                    ref ? "(table [x k y] (list (% (til 4259840) 7) (til 4259840) (take [0Nl] 4259840)))"
                        : "(table [x k] (list (% (til 4259840) 7) (til 4259840)))");
    }
    TEST_ASSERT_FMT(big == 4259840, "budget rows %lld: update the partition size", (long long)big);
    /* a key of few groups is grouped per partition; a key of a group per
     * row in place */
    static const struct { const char* q; bool in_place; } q[] = {
        { "(select {s: (sum y) c: (count y) from: T by: x asc: x})", false },
        { "(select {s: (sum y) c: (count y) a: (avg y) from: T by: x asc: x})", false },
        { "(select {c: (count x) s: (sum x) from: T by: y asc: y take: 3})", false },
        { "(select {s: (sum x) from: T by: y desc: s take: 4})", false },
        { "(select {s: (sum y) c: (count y) from: T by: k desc: s take: 3})", true },
        { "(select {c: (count y) from: T by: k desc: c take: 3})", true },
        { NULL, false },
    };
    test_result_t r;
    for (int i = 0; q[i].q; i++)
        if ((r = sr_fill_route(SR_TMP "/rbf", SR_TMP "/rbd", q[i].q, q[i].in_place, 0)).status != TEST_PASS) return r;
    /* two stand-ins of 3M I64 rows (24 MB each, 48 MB together) */
    const int64_t two = 3000000;
    TEST_ASSERT_TRUE(two * 8 <= (int64_t)AGG_COMPACT_BITMAP_BUDGET && 2 * two * 8 > (int64_t)AGG_COMPACT_BITMAP_BUDGET);
    for (int ref = 0; ref < 2; ref++) {
        const char* root = ref ? SR_TMP "/r2f" : SR_TMP "/r2d";
        sr_evalf_ok("(.db.splayed.set \"%s/2024.01.01/t/\" (table [x k y z] (list (%% (til 70000) 7) (til 70000) "
                    "(%% (til 70000) 97) (%% (til 70000) 89))) {compress: false})", root);
        sr_evalf_ok("(.db.splayed.set \"%s/2024.01.02/t/\" %s {compress: false})", root,
                    ref ? "(table [x k y z] (list (% (til 3000000) 7) (til 3000000) (take [0Nl] 3000000) (take [0Nl] 3000000)))"
                        : "(table [x k] (list (% (til 3000000) 7) (til 3000000)))");
    }
    static const struct { const char* q; bool in_place; } q2[] = {
        { "(select {s: (sum y) t: (sum z) from: T by: x asc: x})", false },
        { "(select {s: (sum y) t: (sum z) c: (count z) from: T by: k desc: s take: 3})", true },
        { NULL, false },
    };
    for (int i = 0; q2[i].q; i++)
        if ((r = sr_fill_route(SR_TMP "/r2f", SR_TMP "/r2d", q2[i].q, q2[i].in_place, two * 8)).status != TEST_PASS)
            return r;
    PASS();
}

/* Task 17b: vectors decided from their 1024-row descriptors where the
 * 64K-row directory leaves the chunk undecided.  a (I16): every fourth
 * vector runs of 64 rows of 0 and 7 (RLE, statistics [0, 7]), the others
 * all 0 (CONST); every chunk is [0, 7], so no chunk is decided.  c (I32):
 * every third vector 5 with one null, the others 9.  The count fast path,
 * the expression gate and the fused predicate skip (or fill) the decided
 * vectors and decode only the 49 RLE vectors of a, once each (P3: the
 * count fast path counts them on their runs, decoding none); the answers
 * are generation 0's. */
#define VD_N    200000
#define VD_RLE  49                  /* vectors v % 4 == 1 of 196 */
static void sr_vd_tables(void) {
    ray_t* a = ray_vec_new(RAY_I16, VD_N); a->len = VD_N;
    ray_t* b = ray_vec_new(RAY_I64, VD_N); b->len = VD_N;
    ray_t* c = ray_vec_new(RAY_I32, VD_N); c->len = VD_N;
    for (int64_t i = 0; i < VD_N; i++) {
        const int64_t v = i / RAY_ENC_VEC;
        ((int16_t*)ray_data(a))[i] = v % 4 == 1 ? (int16_t)((i / 64) % 2 * 7) : 0;
        ((int64_t*)ray_data(b))[i] = i;
        ((int32_t*)ray_data(c))[i] = v % 3 == 0 ? (i % RAY_ENC_VEC == 17 ? NULL_I32 : 5) : 9;
    }
    c->attrs |= RAY_ATTR_HAS_NULLS;
    ray_env_set(ray_sym_intern("vda", 3), a); ray_release(a);
    ray_env_set(ray_sym_intern("vdb", 3), b); ray_release(b);
    ray_env_set(ray_sym_intern("vdc", 3), c); ray_release(c);
    sr_eval_ok("(set VD (table [a b c] (list vda vdb vdc)))");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/vd2/\" VD {compress: true})");
    sr_eval_ok("(.db.splayed.set \"" SR_TMP "/vd0/\" VD {compress: false})");
}
static ray_op_t* vd_a_ne0(ray_graph_t* g) { return ray_ne(g, ray_scan(g, "a"), ray_const_i64(g, 0)); }
static ray_op_t* vd_a_gt3(ray_graph_t* g) { return ray_gt(g, ray_scan(g, "a"), ray_const_i64(g, 3)); }
static ray_op_t* vd_a_eq0(ray_graph_t* g) { return ray_eq(g, ray_scan(g, "a"), ray_const_i64(g, 0)); }
static ray_op_t* vd_c_eq5(ray_graph_t* g) { return ray_eq(g, ray_scan(g, "c"), ray_const_i64(g, 5)); }
static test_result_t test_vec_stat_decide(void) {
    sr_vd_tables();
    const char* t0 = "(.db.splayed.get \"" SR_TMP "/vd0/\")";
    const char* t2 = "(.db.splayed.get \"" SR_TMP "/vd2/\")";
    const uint64_t rle_bytes = (uint64_t)VD_RLE * RAY_ENC_VEC * 2;
    /* the count fast path */
    static const char* const cq[] = {
        "(count (select {from: T where: (!= a 0)}))", "(count (select {from: T where: (== a 0)}))",
        "(count (select {from: T where: (> a 3)}))",  "(count (select {from: T where: (<= a 0)}))",
        "(count (select {from: T where: (< 3 a)}))",  NULL,
    };
    for (int i = 0; cq[i]; i++) {
        test_result_t r = sr_same_on(t0, t2, cq[i], 0);
        if (r.status != TEST_PASS) return r;
        sr_evalf_ok("(set T %s)", t2);
        uint64_t a0 = sr_col_decoded("a"), f0 = ray_query_test_count_fast();
        ray_t* res = sr_eval(cq[i]);
        TEST_ASSERT_TRUE(res && !RAY_IS_ERR(res));
        sr_drop(res);
        TEST_ASSERT_FMT(ray_query_test_count_fast() - f0 == 1, "%s: not the count fast path", cq[i]);
        /* P3 Task 3b: the undecided RLE vectors count on their runs */
        TEST_ASSERT_FMT(sr_col_decoded("a") - a0 == 0, "%s: %llu bytes of a decoded, want 0", cq[i],
                        (unsigned long long)(sr_col_decoded("a") - a0));
    }
    /* the expression gate (a select over the predicate) */
    /* c: the 66 vectors of 5 (the last one 320 rows) hold a null, so they
     * stay undecided; the 130 vectors of 9 are decided.  (== c 9) is decided
     * on every vector.  The OR is checked for its answer only.  P3 Task 3:
     * the undecided vectors (a's RLE runs, c's nullable CONST 5) are
     * answered as encoded leaves on their views, so neither decodes (the
     * a / c flags below now pin 0 bytes; c5_bytes stays for the record). */
    const uint64_t c5_bytes = 65 * RAY_ENC_VEC * 4 + 320 * 4;
    static const struct { const char* q; int64_t a, c, skips; } eq[] = {
        { "(select {s: (sum b) from: T where: (!= a 0)})", 0, 0, 147 },
        { "(select {s: (sum b) c: (count b) from: T where: (> a 3)})", 0, 0, 147 },
        { "(select {s: (sum b) from: T where: (== c 5)})", 0, 0, 130 },
        { "(select {s: (sum b) from: T where: (!= c 9)})", 0, 0, 130 },
        /* (== c 9) first (196 decided); then (== a 0) refines its selection
         * on the segments it keeps, answered by the encoded leaves on their
         * views, which count no zone decision (P3 Task 6b: a chained filter
         * over an encoded column evaluates only the kept segments; before,
         * it was evaluated over every morsel, 147 decided) */
        { "(select {s: (sum b) from: T where: (and (== a 0) (== c 9))})", 0, 0, 196 },
        { "(select {s: (sum b) from: T where: (or (> a 3) (== c 5))})", -1, -1, -1 },
        { NULL, 0, 0, 0 },
    };
    for (int i = 0; eq[i].q; i++) {
        test_result_t r = sr_same_on(t0, t2, eq[i].q, UINT64_MAX);
        if (r.status != TEST_PASS) return r;
        sr_evalf_ok("(set T %s)", t2);
        uint64_t a0 = sr_col_decoded("a"), c0 = sr_col_decoded("c");
        int64_t z0 = ray_expr_test_zone_skips();
        ray_t* res = sr_eval(eq[i].q);
        TEST_ASSERT_TRUE(res && !RAY_IS_ERR(res));
        sr_drop(res);
        if (eq[i].skips < 0) continue;
        uint64_t da = sr_col_decoded("a") - a0, dc = sr_col_decoded("c") - c0;
        int64_t dz = ray_expr_test_zone_skips() - z0;
        TEST_ASSERT_FMT(da == (eq[i].a ? rle_bytes : 0) && dc == (eq[i].c ? c5_bytes : 0) && dz == eq[i].skips,
                        "%s: a %llu, c %llu bytes decoded, %lld morsels decided", eq[i].q,
                        (unsigned long long)da, (unsigned long long)dc, (long long)dz);
    }
    /* the fused predicate, morsel by morsel */
    ray_op_t* (*const fb[])(ray_graph_t*) = { vd_a_ne0, vd_a_gt3, vd_a_eq0, vd_c_eq5 };
    ray_t* g0t = sr_eval(t0); ray_t* g2t = sr_eval(t2);
    for (size_t i = 0; i < sizeof fb / sizeof fb[0]; i++) {
        ray_graph_t* g0 = ray_graph_new(g0t); ray_graph_t* g2 = ray_graph_new(g2t);
        fp_pred_t p0, p2;
        memset(&p0, 0, sizeof p0); memset(&p2, 0, sizeof p2);
        ray_t* e0 = NULL; ray_t* e2 = NULL;
        TEST_ASSERT_EQ_I(fp_compile_pred(g0, fb[i](g0), g0t, &p0, &e0), 0);
        TEST_ASSERT_EQ_I(fp_compile_pred(g2, fb[i](g2), g2t, &p2, &e2), 0);
        TEST_ASSERT_TRUE(!e0 && !e2 && p2.n_children == 1 && p2.children[0].enc);
        fp_scratch_t sc; fp_scratch_init(&sc);
        uint8_t x[RAY_MORSEL_ELEMS], y[RAY_MORSEL_ELEMS];
        ray_enc_counters_reset();
        for (int64_t ms = 0; ms < VD_N; ms += RAY_MORSEL_ELEMS) {
            int64_t me = ms + RAY_MORSEL_ELEMS < VD_N ? ms + RAY_MORSEL_ELEMS : VD_N;
            fp_eval_pred(&p0, ms, me, x);
            TEST_ASSERT_NULL(fp_eval_pred_rs(&p2, ms, me, y, &sc));
            TEST_ASSERT_FMT(memcmp(x, y, (size_t)(me - ms)) == 0, "fused %zu: morsel at %lld", i, (long long)ms);
        }
        fp_scratch_free(&sc);
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        /* c is nullable: its typed compare reads every vector (no gate);
         * a's RLE vectors are answered on their views (P3 Task 3) */
        const uint64_t want = i < 3 ? 0 : (uint64_t)VD_N * 4;
        TEST_ASSERT_FMT(k.decoded_bytes == want, "fused %zu: %llu bytes decoded, want %llu", i,
                        (unsigned long long)k.decoded_bytes, (unsigned long long)want);
        fp_pred_cleanup(&p0); fp_pred_cleanup(&p2);
        ray_graph_free(g0); ray_graph_free(g2);
    }
    sr_drop(g0t); sr_drop(g2t);
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
    { "scanrange/count_fast_lent_scratch", test_count_fast_lent_scratch, srr_setup, srr_teardown },
    { "scanrange/group_lent_scratch", test_group_lent_scratch, srr_setup, srr_pool_teardown },
    { "scanrange/count_fast_sym_nulls",   test_count_fast_sym_nulls,   sr_rt_setup, sr_rt_teardown },
    { "scanrange/expr_reuses_materialized", test_expr_reuses_materialized, srr_setup, srr_teardown },
    { "scanrange/expr_error_sites",       test_expr_error_sites,       srr_setup, srr_teardown },
    { "scanrange/decoded_bytes_pinned",   test_decoded_bytes_pinned,   srr_setup, srr_teardown },
    { "scanrange/count_native_kernels",   test_count_native_kernels,   sr_setup,  sr_teardown },
    { "scanrange/count_native_queries",   test_count_native_queries,   sr_rt_setup, sr_rt_teardown },
    { "scanrange/expr_widened_registers", test_expr_widened_registers, srr_setup, srr_teardown },
    { "scanrange/expr_parted_segments",   test_expr_parted_segments,   srr_setup, srr_teardown },
    { "scanrange/expr_parted_fail_lowest", test_expr_parted_fail_lowest, sr_rt_setup, sr_rt_teardown },
    { "scanrange/expr_parted_cancel",     test_expr_parted_cancel,     sr_rt_setup, sr_rt_teardown },
    { "scanrange/expr_parted_seg_lengths", test_expr_parted_seg_lengths, sr_rt_setup, sr_rt_teardown },
    { "scanrange/zone_dir_skips",      test_zone_dir_skips,      srr_setup, srr_teardown },
    { "scanrange/zone_dir_nulls",      test_zone_dir_nulls,      srr_setup, srr_teardown },
    { "scanrange/filter_consult_dir",  test_filter_consult_dir,  srr_setup, srr_teardown },
    { "scanrange/lazy_hash_enc",       test_lazy_hash_enc,       srr_setup, srr_teardown },
    { "scanrange/filter_consult_enc_shapes", test_filter_consult_enc_shapes, srr_setup, srr_teardown },
    { "scanrange/lazy_memo_goes_with_table", test_lazy_memo_goes_with_table, srr_setup, srr_teardown },
    { "scanrange/zone_class_dir_small",  test_zone_class_dir_small,  sr_rt_setup, sr_rt_teardown },
    { "scanrange/uses_shared_payload",   test_uses_shared_payload,   srr_setup, srr_teardown },
    { "scanrange/zone_dir_boundaries",   test_zone_dir_boundaries,   srr_setup, srr_teardown },
    { "scanrange/fp_range_equal",        test_fp_range_equal,        srr_setup, srr_teardown },
    { "scanrange/fp_range_dir_gate",     test_fp_range_dir_gate,     srr_setup, srr_teardown },
    { "scanrange/fp_range_ready_child",  test_fp_range_ready_child,  srr_setup, srr_teardown },
    { "scanrange/fp_range_str",          test_fp_range_str,          sr_rt_setup, sr_rt_teardown },
    { "scanrange/fp_range_late_materialized", test_fp_range_late_materialized, srr_setup, srr_teardown },
    { "scanrange/fp_range_and_stops",    test_fp_range_and_stops,    srr_setup, srr_teardown },
    { "scanrange/topk_materializes_keys", test_topk_materializes_keys, srr_setup, srr_teardown },
    { "scanrange/topk_take_error",       test_topk_take_error,       srr_setup, srr_teardown },
    { "scanrange/in_selection",          test_in_selection,          srr_setup, srr_teardown },
    { "scanrange/in_selection_reuses_materialized", test_in_selection_reuses_materialized, srr_setup, srr_teardown },
    { "scanrange/in_selection_error",    test_in_selection_error,    srr_setup, srr_teardown },
    { "scanrange/reduce_scan",        test_reduce_scan,        srr_setup, srr_teardown },
    { "scanrange/reduce_scan_ops",    test_reduce_scan_ops,    srr_setup, srr_teardown },
    { "scanrange/reduce_scan_type_errors", test_reduce_scan_type_errors, srr_setup, srr_teardown },
    { "scanrange/reduce_scan_edges",  test_reduce_scan_edges,  srr_setup, srr_teardown },
    { "scanrange/reduce_dir_answers", test_reduce_dir_answers, srr_setup, srr_teardown },
    { "scanrange/reduce_error",       test_reduce_error,       srr_setup, srr_teardown },
    { "scanrange/at_parted_value",    test_at_parted_value,    srr_setup, srr_teardown },
    { "scanrange/raw_float_view",     test_raw_float_view,     srr_setup, srr_teardown },
    { "scanrange/scalar_route",       test_scalar_route,       srr_setup, srr_teardown },
    { "scanrange/scalar_route_shapes", test_scalar_route_shapes, srr_setup, srr_teardown },
    { "scanrange/scalar_route_f64_bits", test_scalar_route_f64_bits, srr_setup, srr_teardown },
    { "scanrange/scalar_route_error", test_scalar_route_error, srr_setup, srr_teardown },
    { "scanrange/group_route_hook",   test_group_route_hook,   srr_setup, srr_teardown },
    { "scanrange/scalar_route_wide_sym", test_scalar_route_wide_sym, srr_setup, srr_teardown },
    { "scanrange/scalar_route_mixed", test_scalar_route_mixed, srr_setup, srr_teardown },
    { "scanrange/scalar_route_f64_combine", test_scalar_route_f64_combine, srr_setup, srr_teardown },
    { "scanrange/scalar_route_f64_unaligned", test_scalar_route_f64_unaligned, srr_setup, srr_teardown },
    { "scanrange/da_route",           test_da_route,           srr_setup, srr_teardown },
    { "scanrange/da_route_shapes",    test_da_route_shapes,    srr_setup, srr_teardown },
    { "scanrange/da_route_error",     test_da_route_error,     srr_setup, srr_teardown },
    { "scanrange/da_route_parted",    test_da_route_parted,    srr_setup, srr_teardown },
    { "scanrange/sp_dyn_route",       test_sp_dyn_route,       srr_setup, srr_teardown },
    { "scanrange/radix_route",        test_radix_route,        srr_setup, srr_teardown },
    { "scanrange/radix_route_shapes", test_radix_route_shapes, srr_setup, srr_teardown },
    { "scanrange/radix_route_sparse", test_radix_route_sparse, srr_setup, srr_teardown },
    { "scanrange/radix_route_serial", test_radix_route_serial, srr_setup, srr_teardown },
    { "scanrange/radix_route_error",  test_radix_route_error,  srr_setup, srr_teardown },
    { "scanrange/v2_routes",          test_v2_routes,          srr_setup, srr_teardown },
    { "scanrange/v2_dense_unaligned", test_v2_dense_unaligned, srr_setup, srr_pool_teardown },
    { "scanrange/v2_sym_key_parted",  test_v2_sym_key_parted,  srr_setup, srr_teardown },
    { "scanrange/v2_parted_routes",   test_v2_parted_routes,   srr_setup, srr_teardown },
    { "scanrange/parted_concat_in_place",     test_parted_concat_in_place,     srr_setup, srr_teardown },
    { "scanrange/parted_concat_matches_flat", test_parted_concat_matches_flat, srr_setup, srr_teardown },
    { "scanrange/parted_sample_reads",        test_parted_sample_reads,        srr_setup, srr_teardown },
    { "scanrange/parted_sym_widths_in_place", test_parted_sym_widths_in_place, srr_setup, srr_teardown },
    { "scanrange/parted_in_place_random",     test_parted_in_place_random,     srr_setup, srr_teardown },
    { "scanrange/parted_factorized_count",    test_parted_factorized_count,    srr_setup, srr_teardown },
    { "scanrange/parted_sym_narrow_first",    test_parted_sym_narrow_first,    srr_setup, srr_teardown },
    { "scanrange/parted_flatten_nulls",       test_parted_flatten_nulls,       srr_setup, srr_teardown },
    { "scanrange/v2_error",           test_v2_error,           srr_setup, srr_teardown },
    { "scanrange/v2_route_shapes",    test_v2_route_shapes,    srr_setup, srr_pool_teardown },
    { "scanrange/v2_sym_shared_dom",  test_v2_sym_shared_dom,  srr_setup, srr_pool_teardown },
    { "scanrange/v2_shared_fail",     test_v2_shared_fail,     srr_setup, srr_pool_teardown },
    { "scanrange/v2_f64_bits",        test_v2_f64_bits,        srr_setup, srr_teardown },
    { "scanrange/v2_route_error",     test_v2_route_error,     srr_setup, srr_teardown },
    { "scanrange/v2_decode_once",     test_v2_decode_once,     srr_setup, srr_pool_teardown },
    { "scanrange/v2_sel_bounds",      test_v2_sel_bounds,      srr_setup, srr_teardown },
    { "scanrange/v2_forged_dir_max",  test_v2_forged_dir_max,  srr_setup, srr_teardown },
    { "scanrange/v2_forged_consumers", test_v2_forged_consumers, srr_setup, srr_teardown },
    { "scanrange/v2_forged_points",    test_v2_forged_points,    srr_setup, srr_teardown },
    { "scanrange/v2_forged_run_points", test_v2_forged_run_points, srr_setup, srr_teardown },
    { "scanrange/v2_forged_serial_dense", test_v2_forged_serial_dense, srr_setup, srr_teardown },
    { "scanrange/v2_sel_key_types",   test_v2_sel_key_types,   srr_setup, srr_teardown },
    { "scanrange/v2_radix_decode_once", test_v2_radix_decode_once, sr_rt_setup, srr_pool_teardown },
    { "scanrange/v2_radix_answers",   test_v2_radix_answers,   sr_rt_setup, srr_pool_teardown },
    { "scanrange/v2_radix_record_order", test_v2_radix_record_order, sr_rt_setup, srr_pool_teardown },
    { "scanrange/v2_radix_compact_staging", test_v2_radix_compact_staging, sr_rt_setup, srr_pool_teardown },
    { "scanrange/v2_forged_parted_segment", test_v2_forged_parted_segment, sr_rt_setup, srr_teardown },
    { "scanrange/v2_range_into_guarded", test_v2_range_into_guarded, sr_rt_setup, srr_teardown },
    { "scanrange/idx_info_table",     test_idx_info_table,     srr_setup, srr_teardown },
    { "scanrange/idx_info_report",    test_idx_info_report,    srr_setup, srr_teardown },
    { "scanrange/no_materialization", test_no_materialization, srr_setup, srr_teardown },
    { "scanrange/no_materialization_parted", test_no_materialization_parted, srr_setup, srr_teardown },
    { "scanrange/parted_eval_group_keep", test_parted_eval_group_keep, srr_setup, srr_teardown },
    { "scanrange/count_and",          test_count_and,          srr_setup, srr_teardown },
    { "scanrange/parted_nullable_stats", test_parted_nullable_stats, srr_setup, srr_teardown },
    { "scanrange/parted_expr_null_free", test_parted_expr_null_free, srr_setup, srr_teardown },
    { "scanrange/strlen_str_gen2_routes", test_strlen_str_gen2_routes, sr_rt_setup, sr_rt_teardown },
    { "scanrange/ragged_gen0_no_decode", test_ragged_gen0_no_decode, srr_setup, srr_teardown },
    { "scanrange/ragged_fill_bounded",   test_ragged_fill_bounded,   srr_setup, srr_teardown },
    { "scanrange/vec_stat_decide",       test_vec_stat_decide,       sr_rt_setup, sr_rt_teardown },
    { "scanrange/v2_sel_bounds_split",   test_v2_sel_bounds_split,   sr_rt_setup, sr_rt_teardown },
    { "scanrange/v2_sel_bounds_boundary", test_v2_sel_bounds_boundary, sr_rt_setup, sr_rt_teardown },
    { "scanrange/v2_forged_split",       test_v2_forged_split,       sr_rt_setup, sr_rt_teardown },
    { "scanrange/const_beside_parted_group", test_const_beside_parted_group, srr_setup, srr_teardown },
    { NULL, NULL, NULL, NULL },
};
