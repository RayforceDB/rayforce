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

/* Ragged parted tables (#724): a partition lacking a tail column reads as
 * that column's nulls, so every column has the table's row count.  Answers
 * on generation 0, generation 2 and mixed ragged tables equal those of the
 * same table written with explicit nulls in the missing rows. */

#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "core/runtime.h"
#include "lang/eval.h"
#include "table/sym.h"
#include "table/table.h"
#include "store/enccol.h"
#include "store/encw.h"
#include "vec/str.h"
#include "store/part.h"
#include "store/serde.h"
#include "store/splay.h"
#include "store/col.h"
#include <stdio.h>
#include <string.h>

#define PR_TMP "/tmp/rayforce_parted_ragged"

static void pr_setup(void)    { ray_runtime_create(0, NULL); (void)ray_test_rm_rf(ray_test_tmp(PR_TMP)); (void)ray_test_mkdir_p(ray_test_tmp(PR_TMP)); }
static void pr_teardown(void) { ray_test_hooks_off(); (void)ray_test_rm_rf(ray_test_tmp(PR_TMP)); ray_runtime_destroy(__RUNTIME); }

static void drop_out(ray_t* o) { if (!o) return; if (RAY_IS_ERR(o)) ray_error_free(o); else ray_release(o); }

static bool ev_ok(const char* src) {
    ray_t* r = ray_eval_str(src);
    bool ok = r && !RAY_IS_ERR(r);
    drop_out(r);
    return ok;
}

/* Same serialized value and same printed form. */
static bool same_value(ray_t* x, ray_t* y) {
    if (!x || !y || RAY_IS_ERR(x) || RAY_IS_ERR(y)) return false;
    ray_t* sx = ray_ser(x); ray_t* sy = ray_ser(y);
    bool same = sx && sy && !RAY_IS_ERR(sx) && !RAY_IS_ERR(sy) && sx->len == sy->len &&
                memcmp(ray_data(sx), ray_data(sy), (size_t)sx->len) == 0;
    if (sx && !RAY_IS_ERR(sx)) ray_release(sx);
    if (sy && !RAY_IS_ERR(sy)) ray_release(sy);
    if (!same) return false;
    ray_t* fx = ray_fmt(x, 0); ray_t* fy = ray_fmt(y, 0);
    same = fx && fy && ray_str_len(fx) == ray_str_len(fy) &&
           memcmp(ray_str_ptr(fx), ray_str_ptr(fy), ray_str_len(fx)) == 0;
    if (fx) ray_release(fx);
    if (fy) ray_release(fy);
    return same;
}

/* Three partitions of t under `root`; bit p of `gen2` writes partition p as
 * generation 2.  `ragged` writes the last partition with column a alone;
 * otherwise it carries the other columns' nulls explicitly (the reference). */
static bool write_db(const char* root, unsigned gen2, bool ragged) {
    static const char* const parts[3] = {
        "(table [a b s f str d flag] (list [1 2 3] [10 20 30] ['x 'y 'x] [1.5 2.5 3.5] [\"p\" \"q\" \"r\"] "
        "[2024.01.01 2024.01.02 2024.01.03] [true false true]))",
        "(table [a b s f str d flag] (list [4 5] [40 50] ['y 'z] [4.5 5.5] [\"s\" \"t\"] "
        "[2024.02.01 2024.02.02] [false true]))",
        "(table [a b s f str d flag] (list [6 7 8 9] (take [0Nl] 4) (take ['] 4) (take [0Nf] 4) "
        "(take [\"\"] 4) (take [0Nd] 4) (take [false] 4)))",
    };
    for (int p = 0; p < 3; p++) {
        char q[1024];
        const char* tbl = (ragged && p == 2) ? "(table [a] (list [6 7 8 9]))" : parts[p];
        snprintf(q, sizeof q, "(.db.splayed.set \"%s/2024.01.0%d/t/\" %s {compress: %s})",
                 root, p + 1, tbl, ((gen2 >> p) & 1) ? "true" : "false");
        if (!ev_ok(q)) return false;
    }
    return true;
}

static ray_t* load_db(const char* root) {
    ray_t* t = ray_read_parted(root, "t");
    return (t && !RAY_IS_ERR(t)) ? t : NULL;
}

static void bind_t(ray_t* t) { ray_env_set(ray_sym_intern("t", 1), t); }

static const char* const queries[] = {
    "(select {s: (sum b) from: t})",
    "(select {c: (count a) by: b from: t where: (> a 0)})",
    "(select {b: b from: t where: (>= date 2024.01.02)})",
    "(select {b: b s: s from: t where: (== date 2024.01.03)})",
    "(select {s: (sum b) from: t where: (== date 2024.01.03)})",
    "(select {date: date a: a b: b s: s f: f str: str d: d flag: flag from: t})",
    "(select {from: t where: (> a 4)})",
    "(select {s: (sum f) m: (max d) n: (count s) x: (min b) by: date from: t})",
    "(select {c: (count a) by: s from: t})",
    "(select {c: (count a) by: str from: t})",
    "(select {c: (count a) by: flag from: t})",
    "(select {x: (+ a b) y: (* f 2) from: t})",
    "(xasc (select {a: a b: b from: t}) 'b)",
    "(xdesc (select {a: a d: d from: t}) 'd)",
    "(select {s: (sum a) from: t where: (> b 15)})",
    "(select {a: a from: t where: (== s 'x)})",
    "(select {c: (count (distinct b)) by: date from: t})",
    "(select {r: (sums b) from: t})",
    "(at t 'b)",
    "(at t 's)",
    "(at t 'str)",
    "(at t 'flag)",
    "(sum (at t 'b))",
    "(count (at t 'f))",
    NULL,
};

/* Every query answers on the ragged table exactly as on the reference. */
static test_result_t ragged_matches(unsigned gen2) {
    char rr[128], rf[128];
    snprintf(rr, sizeof rr, ray_test_tmp(PR_TMP "/rag_%u"), gen2);
    snprintf(rf, sizeof rf, ray_test_tmp(PR_TMP "/ref_%u"), gen2);
    TEST_ASSERT_TRUE(write_db(rr, gen2, true));
    TEST_ASSERT_TRUE(write_db(rf, gen2, false));
    ray_t* tr = load_db(rr);
    ray_t* tf = load_db(rf);
    TEST_ASSERT_TRUE(tr && tf);
    for (int i = 0; queries[i]; i++) {
        bind_t(tf);
        ray_t* want = ray_eval_str(queries[i]);
        bind_t(tr);
        ray_t* got = ray_eval_str(queries[i]);
        bool ok = want && !RAY_IS_ERR(want);
        bool same = same_value(want, got);
        drop_out(want); drop_out(got);
        TEST_ASSERT_FMT(ok, "gen2 %u: %s: reference answers", gen2, queries[i]);
        TEST_ASSERT_FMT(same, "gen2 %u: %s: ragged equals reference", gen2, queries[i]);
    }
    bind_t(NULL);
    ray_release(tr); ray_release(tf);
    PASS();
}

static test_result_t test_ragged_gen0(void)  { return ragged_matches(0); }
static test_result_t test_ragged_gen2(void)  { return ragged_matches(7); }
static test_result_t test_ragged_mixed(void) {
    test_result_t r = ragged_matches(3);          /* the ragged partition is generation 0 */
    if (r.status != TEST_PASS) return r;
    return ragged_matches(4);                     /* only the ragged partition is generation 2 */
}

/* The issue's repro: pinned answers, not only reference equality. */
static test_result_t test_issue_repro(void) {
    TEST_ASSERT_TRUE(write_db(ray_test_tmp(PR_TMP "/repro"), 0, true));
    ray_t* t = load_db(ray_test_tmp(PR_TMP "/repro"));
    TEST_ASSERT_NOT_NULL(t);
    bind_t(t);
    ray_t* r = ray_eval_str("(at (select {s: (sum b) from: t}) 's)");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->type == RAY_I64 && r->len == 1);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(r))[0], 150);
    drop_out(r);
    /* the four rows of 2024.01.03 group under the null key */
    r = ray_eval_str("(at (select {c: (count a) by: b from: t where: (> a 0)}) 'c)");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->type == RAY_I64 && r->len == 6);
    int64_t total = 0, four = 0;
    for (int64_t i = 0; i < r->len; i++) { total += ((int64_t*)ray_data(r))[i]; four += ((int64_t*)ray_data(r))[i] == 4; }
    TEST_ASSERT_EQ_I(total, 9);
    TEST_ASSERT_EQ_I(four, 1);
    drop_out(r);
    r = ray_eval_str("(at (select {c: (count a) by: b from: t where: (> a 0)}) 'b)");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->type == RAY_I64 && r->len == 6);
    int64_t nulls = 0;
    for (int64_t i = 0; i < r->len; i++) nulls += ((int64_t*)ray_data(r))[i] == NULL_I64;
    TEST_ASSERT_EQ_I(nulls, 1);
    drop_out(r);
    /* the filter keeps 2024.01.02 and 2024.01.03 only */
    r = ray_eval_str("(at (select {b: b from: t where: (>= date 2024.01.02)}) 'b)");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->type == RAY_I64);
    TEST_ASSERT_EQ_I(r->len, 6);
    static const int64_t want[6] = { 40, 50, NULL_I64, NULL_I64, NULL_I64, NULL_I64 };
    for (int i = 0; i < 6; i++) TEST_ASSERT_EQ_I(((int64_t*)ray_data(r))[i], want[i]);
    drop_out(r);
    bind_t(NULL);
    ray_release(t);
    PASS();
}

/* A loaded ragged column has a segment per partition, each with its
 * partition's MAPCOMMON row count; ray_parted_nrows is the table's rows. */
static test_result_t test_segments_full(void) {
    for (unsigned gen2 = 0; gen2 < 8; gen2 += 4) {
        char root[128]; snprintf(root, sizeof root, ray_test_tmp(PR_TMP "/seg_%u"), gen2);
        TEST_ASSERT_TRUE(write_db(root, gen2 | 1, true));
        ray_t* t = load_db(root);
        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_EQ_I(ray_table_nrows(t), 9);
        ray_t* mc = ray_table_col_idx(t, 0);
        TEST_ASSERT_TRUE(mc && mc->type == RAY_MAPCOMMON);
        const int64_t* counts = (const int64_t*)ray_data(((ray_t**)ray_data(mc))[1]);
        for (int64_t c = 1; c < ray_table_ncols(t); c++) {
            ray_t* slot = ray_table_get_col_enc_idx(t, c);
            ray_t* w = slot ? NULL : ray_table_col_idx(t, c);
            TEST_ASSERT_EQ_I(ray_table_col_nrows(slot ? slot : w), 9);
            if (w) TEST_ASSERT_EQ_I(ray_parted_nrows(w), 9);
            for (int32_t p = 0; p < 3; p++) {
                ray_t* seg = slot ? ray_enccol_seg(slot, p) : ((ray_t**)ray_data(w))[p];
                TEST_ASSERT_FMT(seg != NULL, "gen2 %u col %lld seg %d present", gen2, (long long)c, p);
                TEST_ASSERT_EQ_I(ray_table_col_nrows(seg), counts[p]);
            }
        }
        ray_release(t);
    }
    PASS();
}

/* A 2-row partition beside a 1M-row partition lacking b: every answer is
 * the null-filled reference's (a read past the 2-row b would not be). */
static test_result_t test_large_ragged(void) {
    for (unsigned gen2 = 0; gen2 < 4; gen2++) {
        char rr[128], rf[128], q[512];
        snprintf(rr, sizeof rr, ray_test_tmp(PR_TMP "/lrag_%u"), gen2);
        snprintf(rf, sizeof rf, ray_test_tmp(PR_TMP "/lref_%u"), gen2);
        for (int k = 0; k < 2; k++) {
            const char* root = k ? rf : rr;
            snprintf(q, sizeof q, "(.db.splayed.set \"%s/2024.01.01/t/\" (table [a b] (list [1 2] [10 20])) {compress: %s})",
                     root, (gen2 & 1) ? "true" : "false");
            TEST_ASSERT_TRUE(ev_ok(q));
            snprintf(q, sizeof q, "(.db.splayed.set \"%s/2024.01.02/t/\" %s {compress: %s})", root,
                     k ? "(table [a b] (list (til 1000000) (take [0Nl] 1000000)))" : "(table [a] (list (til 1000000)))",
                     (gen2 & 2) ? "true" : "false");
            TEST_ASSERT_TRUE(ev_ok(q));
        }
        ray_t* tr = load_db(rr);
        ray_t* tf = load_db(rf);
        TEST_ASSERT_TRUE(tr && tf);
        static const char* const lq[] = {
            "(select {s: (sum b) c: (count b) m: (max b) from: t})",
            "(select {c: (count a) by: b from: t})",
            "(select {s: (sum a) from: t where: (> b 0)})",
            "(select {b: b from: t where: (< a 3)})",
            "(select {s: (sum b) by: date from: t})",
            NULL,
        };
        for (int i = 0; lq[i]; i++) {
            bind_t(tf);
            ray_t* want = ray_eval_str(lq[i]);
            bind_t(tr);
            ray_t* got = ray_eval_str(lq[i]);
            bool same = same_value(want, got);
            drop_out(want); drop_out(got);
            TEST_ASSERT_FMT(same, "gen2 %u: %s", gen2, lq[i]);
        }
        bind_t(tr);
        ray_t* s = ray_eval_str("(at (select {s: (sum b) from: t}) 's)");
        TEST_ASSERT_TRUE(s && !RAY_IS_ERR(s) && s->type == RAY_I64 && s->len == 1);
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(s))[0], 30);
        drop_out(s);
        bind_t(NULL);
        ray_release(tr); ray_release(tf);
    }
    PASS();
}

/* A hand-built parted column whose segments do not cover the table's rows
 * (here a missing segment, which ray_read_parted never produces) is refused
 * by the executor, never read past its end. */
static test_result_t test_short_column_refused(void) {
    TEST_ASSERT_TRUE(write_db(ray_test_tmp(PR_TMP "/hand"), 0, false));
    ray_t* src = load_db(ray_test_tmp(PR_TMP "/hand"));
    TEST_ASSERT_NOT_NULL(src);
    ray_t* b = ray_table_col(src, ray_sym_intern("b", 1));
    TEST_ASSERT_TRUE(b && RAY_IS_PARTED(b->type) && b->len == 3);
    ray_t* w = ray_alloc(3 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(w);
    w->type = b->type; w->len = 3; w->attrs = 0; memset(w->aux, 0, 16);
    ray_t** ws = (ray_t**)ray_data(w);
    for (int i = 0; i < 3; i++) {
        ws[i] = i == 2 ? NULL : ((ray_t**)ray_data(b))[i];
        if (ws[i]) ray_retain(ws[i]);
    }
    ray_t* t = ray_table_new(3);
    t = ray_table_add_col(t, ray_table_col_name(src, 0), ray_table_col_idx(src, 0));
    t = ray_table_add_col(t, ray_sym_intern("a", 1), ray_table_col(src, ray_sym_intern("a", 1)));
    t = ray_table_add_col(t, ray_sym_intern("b", 1), w);
    ray_release(w);
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    bind_t(t);
    static const char* const hq[] = {
        "(select {s: (sum b) from: t})",
        "(select {c: (count a) by: b from: t})",
        "(select {b: b from: t where: (>= date 2024.01.02)})",
        "(select {a: a b: b from: t})",
        NULL,
    };
    for (int i = 0; hq[i]; i++) {
        ray_t* r = ray_eval_str(hq[i]);
        bool err = r && RAY_IS_ERR(r) && strcmp(ray_err_code(r), "length") == 0;
        drop_out(r);
        TEST_ASSERT_FMT(err, "%s: refused with length", hq[i]);
    }
    bind_t(NULL);
    ray_release(t); ray_release(src);
    PASS();
}

/* A non-null column of `type` over n rows (LIST: I64 atoms). */
static ray_t* typed_col(int8_t type, int64_t n) {
    if (type == RAY_LIST) {
        ray_t* l = ray_list_new(n);
        for (int64_t i = 0; i < n; i++) { ray_t* a = ray_i64(i + 1); l = ray_list_append(l, a); ray_release(a); }
        return l;
    }
    if (type == RAY_STR) {
        ray_t* v = ray_vec_new(RAY_STR, n);
        for (int64_t i = 0; i < n; i++) v = ray_str_vec_append(v, "v", 1);
        return v;
    }
    if (type == RAY_SYM) {
        ray_t* v = ray_vec_new(RAY_SYM, n);
        for (int64_t i = 0; i < n; i++) { int64_t id = ray_sym_intern("k", 1); v = ray_vec_append(v, &id); }
        return v;
    }
    ray_t* v = ray_vec_new(type, n);
    v->len = n;
    uint8_t* d = (uint8_t*)ray_data(v);
    size_t esz = ray_elem_size(type);
    memset(d, 0, (size_t)n * esz);
    for (int64_t i = 0; i < n; i++) d[(size_t)i * esz] = 1;   /* 1 (true for BOOL): never a sentinel */
    if (type == RAY_F64) for (int64_t i = 0; i < n; i++) ((double*)d)[i] = (double)i + 0.5;
    if (type == RAY_F32) for (int64_t i = 0; i < n; i++) ((float*)d)[i] = (float)i + 0.5f;
    return v;
}

/* Is cell i of the filled segment the type's null fill? */
static bool fill_cell_ok(ray_t* seg, int64_t i) {
    const void* d = ray_data(seg);
    switch (seg->type) {
        case RAY_F64:  { double x = ((const double*)d)[i]; return x != x; }
        case RAY_F32:  { float x = ((const float*)d)[i]; return x != x; }
        case RAY_I64: case RAY_TIMESTAMP: return ((const int64_t*)d)[i] == NULL_I64;
        case RAY_I32: case RAY_DATE: case RAY_TIME: return ((const int32_t*)d)[i] == NULL_I32;
        case RAY_I16:  return ((const int16_t*)d)[i] == NULL_I16;
        case RAY_BOOL: case RAY_U8: return ((const uint8_t*)d)[i] == 0;
        case RAY_SYM:  return ray_read_sym(d, i, RAY_SYM, seg->attrs) == 0;
        case RAY_STR: case RAY_GUID: {
            static const uint8_t zero[16] = {0};
            return memcmp((const uint8_t*)d + i * 16, zero, 16) == 0;
        }
        case RAY_LIST: return ((ray_t* const*)d)[i] == RAY_NULL_OBJ;
        default: return false;
    }
}

/* Every column type a partition can lack fills with its null (the non-nullable
 * BOOL / U8 with 0, LIST with the null object): HAS_NULLS on exactly the
 * nullable vector types, the table's row count, ray_vec_is_null on every
 * nullable cell.  Generation 0 and generation 2 (where the type saves).
 * Every type generation 2 stores reads as a lazy all-null segment (a
 * RAY_ENCCOL) whose materialization is that fill; LIST stays a heap fill. */
static test_result_t test_fill_every_type(void) {
    static const int8_t types[] = {
        RAY_BOOL, RAY_U8, RAY_I16, RAY_I32, RAY_I64, RAY_F32, RAY_F64, RAY_DATE, RAY_TIME,
        RAY_TIMESTAMP, RAY_SYM, RAY_STR, RAY_GUID, RAY_LIST,
    };
    for (int gen = 0; gen < 2; gen++) {
        for (size_t k = 0; k < sizeof types / sizeof types[0]; k++) {
            int8_t ty = types[k];
            char root[160], dir[200], symp[200];
            snprintf(root, sizeof root, ray_test_tmp(PR_TMP "/ty_%d_%d"), gen, (int)ty);
            snprintf(symp, sizeof symp, "%s/.sym", root);
            for (int p = 0; p < 2; p++) {
                snprintf(dir, sizeof dir, "%s/2024.01.0%d/t", root, p + 1);
                int64_t n = p == 0 ? 3 : 5;
                ray_t* t = ray_table_new(2);
                ray_t* a = typed_col(RAY_I64, n);
                t = ray_table_add_col(t, ray_sym_intern("a", 1), a);
                ray_release(a);
                if (p == 0) {
                    ray_t* x = typed_col(ty, n);
                    TEST_ASSERT_TRUE(x && !RAY_IS_ERR(x) && x->len == n);
                    t = ray_table_add_col(t, ray_sym_intern("x", 1), x);
                    ray_release(x);
                }
                ray_err_t e = ray_splay_save_mode(t, dir, symp, gen ? RAY_COMPRESS_ON : RAY_COMPRESS_OFF);
                ray_release(t);
                TEST_ASSERT_FMT(e == RAY_OK, "gen %d type %d partition %d: save %d", gen, (int)ty, p, (int)e);
            }
            ray_enc_counters_reset();
            ray_t* t = ray_read_parted(root, "t");
            TEST_ASSERT_FMT(t && !RAY_IS_ERR(t), "gen %d type %d: load", gen, (int)ty);
            ray_t* slot = ray_table_get_col_enc(t, ray_sym_intern("x", 1));
            ray_t* w = slot ? NULL : ray_table_col(t, ray_sym_intern("x", 1));
            TEST_ASSERT_EQ_I(ray_table_col_nrows(slot ? slot : w), 8);
            ray_t* seg = slot ? ray_enccol_seg(slot, 1) : ((ray_t**)ray_data(w))[1];
            bool lazy = ty != RAY_LIST;
            TEST_ASSERT_FMT(seg && seg->type == (lazy ? RAY_ENCCOL : ty) && ray_table_col_nrows(seg) == 5,
                            "gen %d type %d: filled segment", gen, (int)ty);
            bool nullable = ty != RAY_BOOL && ty != RAY_U8 && ty != RAY_LIST;
            /* null presence answers without a decode (directory null counts) */
            TEST_ASSERT_FMT(ray_table_col_has_nulls(t, ray_sym_intern("x", 1)) == nullable,
                            "gen %d type %d: column has nulls", gen, (int)ty);
            ray_enc_counters_t cn; ray_enc_counters_get(&cn);
            TEST_ASSERT_FMT(cn.materialized == 0, "gen %d type %d: nothing decoded at open", gen, (int)ty);
            ray_t* plain = lazy ? ray_enccol_seg_col(slot, 1) : seg;
            TEST_ASSERT_FMT(plain && !RAY_IS_ERR(plain) && plain->type == ty && plain->len == 5,
                            "gen %d type %d: materialized fill", gen, (int)ty);
            TEST_ASSERT_FMT(((plain->attrs & RAY_ATTR_HAS_NULLS) != 0) == nullable, "gen %d type %d: HAS_NULLS", gen, (int)ty);
            for (int64_t i = 0; i < 5; i++) {
                TEST_ASSERT_FMT(fill_cell_ok(plain, i), "gen %d type %d: cell %lld", gen, (int)ty, (long long)i);
                if (nullable && ty != RAY_SYM) TEST_ASSERT_TRUE(ray_vec_is_null(plain, i));
            }
            if (ty == RAY_SYM) {                /* id 0 resolves over the db's domain */
                ray_t* s0 = slot ? ray_enccol_seg_col(slot, 0) : ((ray_t**)ray_data(w))[0];
                TEST_ASSERT_TRUE(s0 && !RAY_IS_ERR(s0) && s0->type == RAY_SYM);
                TEST_ASSERT_TRUE(plain->sym_domain == s0->sym_domain);
            }
            /* the column reads whole: present rows, then the fill */
            bind_t(t);
            ray_t* c = ray_eval_str("(count (at t 'x))");
            TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c) && c->i64 == 8);
            drop_out(c);
            bind_t(NULL);
            ray_release(t);
        }
    }
    PASS();
}

/* The all-null image at chunk and vector boundaries: it opens (every
 * structural check of a file load), materializes to nulls, and its mapping
 * is released exactly once (the mapped-bytes total returns to its start). */
static test_result_t test_null_image_shapes(void) {
    static const int64_t lens[] = { 0, 1, 1023, 1025, 65536, 65537, 131072, 200000 };
    static const int8_t types[] = { RAY_I64, RAY_STR, RAY_BOOL, RAY_SYM, RAY_F64, RAY_GUID };
    for (size_t k = 0; k < sizeof types / sizeof types[0]; k++) {
        for (size_t li = 0; li < sizeof lens / sizeof lens[0]; li++) {
            int8_t ty = types[k];
            int64_t n = lens[li];
            int64_t fn = n < 65536 ? n : 65536;
            ray_t* fill = ty == RAY_SYM ? ray_sym_vec_new(RAY_SYM_W8, fn) : ray_vec_new(ty, fn);
            TEST_ASSERT_TRUE(fill && !RAY_IS_ERR(fill));
            fill->len = fn;
            size_t esz = ray_sym_elem_size(ty, fill->attrs);
            memset(ray_data(fill), 0, (size_t)fn * esz);
            if (ty == RAY_I64) for (int64_t i = 0; i < fn; i++) ((int64_t*)ray_data(fill))[i] = NULL_I64;
            if (ty == RAY_F64) for (int64_t i = 0; i < fn; i++) ((double*)ray_data(fill))[i] = NULL_F64;
            ray_mem_stats_t m0, m1;
            ray_mem_stats(&m0);
            void* img = NULL; size_t size = 0;
            TEST_ASSERT_EQ_I(ray_encw_null_image(ty, fill->attrs, n, 0, ray_data(fill), &img, &size), RAY_OK);
            ray_release(fill);
            ray_t* v = ray_enccol_open(img, size, "null image", NULL, false);
            TEST_ASSERT_FMT(v && !RAY_IS_ERR(v), "type %d len %lld: opens", (int)ty, (long long)n);
            TEST_ASSERT_EQ_I((int64_t)ray_enccol_own_bytes(v), (int64_t)size);
            ray_col_meta_t m;
            ray_enccol_meta(v, &m);
            TEST_ASSERT_EQ_I(m.null_count, (ty == RAY_BOOL) ? 0 : n);
            ray_t* pv = ray_enccol_materialize(v);
            TEST_ASSERT_FMT(pv && !RAY_IS_ERR(pv) && pv->type == ty && pv->len == n,
                            "type %d len %lld: materializes", (int)ty, (long long)n);
            for (int64_t i = 0; i < n; i++) {
                bool ok = ty == RAY_BOOL ? ((uint8_t*)ray_data(pv))[i] == 0
                        : ty == RAY_SYM  ? ray_read_sym(ray_data(pv), i, RAY_SYM, pv->attrs) == 0
                        : ty == RAY_STR  ? ((ray_str_t*)ray_data(pv))[i].len == 0
                        : ray_vec_is_null(pv, i);
                if (!ok) TEST_ASSERT_FMT(ok, "type %d len %lld: row %lld null", (int)ty, (long long)n, (long long)i);
            }
            ray_release(v);
            ray_mem_stats(&m1);
            TEST_ASSERT_EQ_I((int64_t)m1.sys_mapped, (int64_t)m0.sys_mapped);
        }
    }
    PASS();
}

/* Resident set size in bytes (Linux /proc), 0 when unavailable. */
static int64_t rss_bytes(void) {
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long long pages = 0, res = 0;
    int got = fscanf(f, "%lld %lld", &pages, &res);
    fclose(f);
    return got == 2 ? (int64_t)res * 4096 : 0;
}

/* A 20M-row partition lacking an I64 and a STR column opens in O(chunks)
 * bytes, not O(rows) (the eager fill held 160 MB + 320 MB on the heap),
 * and nothing decodes until a query reads the column. */
static test_result_t test_lazy_open(void) {
    const int64_t N = 20000000;
    const char* root = ray_test_tmp(PR_TMP "/lazy");
    char q[512];
    snprintf(q, sizeof q, "(.db.splayed.set \"%s/2024.01.01/t/\" (table [a b str] (list [1 2 3] [10 20 30] "
             "[\"p\" \"q\" \"r\"])) {compress: false})", root);
    TEST_ASSERT_TRUE(ev_ok(q));
    snprintf(q, sizeof q, "(.db.splayed.set \"%s/2024.01.02/t/\" (table [a] (list (til %lld))) {compress: false})",
             root, (long long)N);
    TEST_ASSERT_TRUE(ev_ok(q));
    ray_heap_gc();
    ray_mem_stats_t m0, m1;
    ray_mem_stats(&m0);
    int64_t rss0 = rss_bytes();
    ray_enc_counters_reset();
    ray_t* t = load_db(root);
    TEST_ASSERT_NOT_NULL(t);
    ray_mem_stats(&m1);
    int64_t rss1 = rss_bytes();
    /* committed RAM: buddy pools, direct mmaps and sys allocs */
    int64_t heap = (int64_t)m1.sys_current - (int64_t)m0.sys_current;
    TEST_ASSERT_FMT(heap < (16 << 20), "committed growth at open %lld bytes", (long long)heap);
    TEST_ASSERT_FMT(rss1 - rss0 < (16 << 20), "RSS growth at open %lld bytes", (long long)(rss1 - rss0));
    static const char* const missing[] = { "b", "str" };
    for (int i = 0; i < 2; i++) {
        ray_t* slot = ray_table_get_col_enc(t, ray_sym_intern(missing[i], strlen(missing[i])));
        TEST_ASSERT_NOT_NULL(slot);
        ray_t* seg = ray_enccol_seg(slot, 1);
        TEST_ASSERT_TRUE(seg && seg->type == RAY_ENCCOL && seg->len == N);
        TEST_ASSERT_FMT(ray_enccol_own_bytes(seg) < (8u << 20), "%s image %zu bytes",
                        missing[i], ray_enccol_own_bytes(seg));
    }
    ray_enc_counters_t c;
    ray_enc_counters_get(&c);
    TEST_ASSERT_EQ_I((int64_t)c.materialized, 0);
    bind_t(t);
    /* a query that does not reference the column */
    ray_t* r = ray_eval_str("(at (select {s: (sum a) from: t}) 's)");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->type == RAY_I64);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(r))[0], 6 + (N - 1) * N / 2);
    drop_out(r);
    ray_enc_counters_get(&c);
    TEST_ASSERT_EQ_I((int64_t)c.materialized, 0);
    /* a pruned query that excludes the ragged partition */
    r = ray_eval_str("(at (select {s: (sum b) from: t where: (== date 2024.01.01)}) 's)");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->type == RAY_I64);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(r))[0], 60);
    drop_out(r);
    ray_enc_counters_get(&c);
    TEST_ASSERT_EQ_I((int64_t)c.materialized, 0);
    /* a query that reads the column: the partition's rows are nulls */
    r = ray_eval_str("(at (select {s: (sum b) from: t}) 's)");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->type == RAY_I64);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(r))[0], 60);
    drop_out(r);
    r = ray_eval_str("(count (select {b: b from: t where: (== b 0Nl)}))");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->i64, N);
    drop_out(r);
    bind_t(NULL);
    ray_release(t);
    PASS();
}

/* Saving what a lazy segment holds writes its nulls in either generation:
 * the flattened column, the pruned partition alone, and the segment object
 * itself through the column writer.  Each reloads equal to the same save
 * from the explicit-null reference. */
static test_result_t test_lazy_save(void) {
    static const char* const sq[] = {
        "(select {a: a b: b s: s f: f str: str d: d flag: flag from: t})",
        "(select {a: a b: b s: s f: f str: str d: d flag: flag from: t where: (== date 2024.01.03)})",
        NULL,
    };
    for (unsigned gen2 = 0; gen2 < 8; gen2 += 7) {
        char rr[128], rf[128];
        snprintf(rr, sizeof rr, ray_test_tmp(PR_TMP "/srag_%u"), gen2);
        snprintf(rf, sizeof rf, ray_test_tmp(PR_TMP "/sref_%u"), gen2);
        TEST_ASSERT_TRUE(write_db(rr, gen2, true));
        TEST_ASSERT_TRUE(write_db(rf, gen2, false));
        ray_t* tr = load_db(rr);
        ray_t* tf = load_db(rf);
        TEST_ASSERT_TRUE(tr && tf);
        for (int i = 0; sq[i]; i++) {
            for (int out_gen = 0; out_gen < 2; out_gen++) {
                ray_t* got[2] = { NULL, NULL };
                for (int k = 0; k < 2; k++) {
                    char q[768];
                    bind_t(k ? tf : tr);
                    snprintf(q, sizeof q, "(.db.splayed.set \"%s/out_%d_%d_%d/\" %s {compress: %s})",
                             k ? rf : rr, i, out_gen, k, sq[i], out_gen ? "true" : "false");
                    TEST_ASSERT_FMT(ev_ok(q), "gen2 %u: save %s", gen2, sq[i]);
                    snprintf(q, sizeof q, "(.db.splayed.get \"%s/out_%d_%d_%d/\")", k ? rf : rr, i, out_gen, k);
                    got[k] = ray_eval_str(q);
                }
                bool same = same_value(got[1], got[0]);
                drop_out(got[0]); drop_out(got[1]);
                TEST_ASSERT_FMT(same, "gen2 %u out gen %d: %s reloads equal", gen2, out_gen, sq[i]);
            }
        }
        /* the lazy segment itself, saved by the column writer */
        ray_t* slot = ray_table_get_col_enc(tr, ray_sym_intern("b", 1));
        TEST_ASSERT_NOT_NULL(slot);
        ray_t* seg = ray_enccol_seg(slot, 2);
        TEST_ASSERT_TRUE(seg && seg->type == RAY_ENCCOL);
        for (int out_gen = 0; out_gen < 2; out_gen++) {
            char path[200];
            snprintf(path, sizeof path, "%s/seg_b_%d", rr, out_gen);
            TEST_ASSERT_EQ_I(ray_col_save_gen(seg, path, out_gen ? 2 : 0), RAY_OK);
            TEST_ASSERT_EQ_I(ray_col_file_gen(path), out_gen ? 2 : 0);
            ray_t* v = ray_col_load(path);
            TEST_ASSERT_TRUE(v && !RAY_IS_ERR(v));
            ray_t* pv = v->type == RAY_ENCCOL ? ray_enccol_materialize(v) : v;
            TEST_ASSERT_TRUE(pv && !RAY_IS_ERR(pv) && pv->type == RAY_I64 && pv->len == 4);
            for (int64_t r = 0; r < 4; r++) TEST_ASSERT_EQ_I(((int64_t*)ray_data(pv))[r], NULL_I64);
            ray_release(v);
        }
        bind_t(NULL);
        ray_release(tr); ray_release(tf);
    }
    PASS();
}

const test_entry_t parted_ragged_entries[] = {
    { "parted_ragged/fill_every_type",      test_fill_every_type,      pr_setup, pr_teardown },
    { "parted_ragged/issue_repro",          test_issue_repro,          pr_setup, pr_teardown },
    { "parted_ragged/gen0",                 test_ragged_gen0,          pr_setup, pr_teardown },
    { "parted_ragged/gen2",                 test_ragged_gen2,          pr_setup, pr_teardown },
    { "parted_ragged/mixed",                test_ragged_mixed,         pr_setup, pr_teardown },
    { "parted_ragged/segments_full",        test_segments_full,        pr_setup, pr_teardown },
    { "parted_ragged/large",                test_large_ragged,         pr_setup, pr_teardown },
    { "parted_ragged/short_column_refused", test_short_column_refused, pr_setup, pr_teardown },
    { "parted_ragged/null_image_shapes",    test_null_image_shapes,    pr_setup, pr_teardown },
    { "parted_ragged/lazy_open",            test_lazy_open,            pr_setup, pr_teardown },
    { "parted_ragged/lazy_save",            test_lazy_save,            pr_setup, pr_teardown },
    { NULL, NULL, NULL, NULL },
};
