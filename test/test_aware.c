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
#include "ops/internal.h"
#include "ops/rowsel.h"
#include "ops/fused_topk.h"
#include "table/sym.h"
#include "table/table.h"
#include "core/runtime.h"
#include "lang/format.h"
#include "store/col.h"
#include "ops/idxop.h"      /* RAY_IDX_DICT */
#include "store/enc.h"
#include "store/encfmt.h"
#include "store/encw.h"
#include "store/enccol.h"
#include "store/colrange.h"
#include "store/enceval.h"
#include "lang/env.h"
#include "store/splay.h"
#include "store/part.h"
#include "ops/colgather.h"
#include "ops/agg_engine.h"   /* agg_route_stats: the first-n route */
#include "core/pool.h"      /* ray_cancel */
#include "vec/str.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Written as "/tmp/..." in the sources; every use goes through
 * ray_test_tmp (a per-process root), so concurrent suites never share it. */
#define AW_TMP "/tmp/rayforce_aware"
/* Set while a test runs under ray_group_test_scalar_grain: answers are checked, the
 * decoded-byte counts (which then include task-edge vectors) are not. */
static bool g_aw_unaligned;
static const char* aw_root(void) { return ray_test_tmp(AW_TMP); }
static ray_runtime_t* g_awrt;
static uint8_t* g_img; static size_t g_len;
static void aw_setup(void)    { g_awrt = ray_runtime_create(0, NULL); (void)ray_test_rm_rf(aw_root()); (void)ray_test_mkdir_p(aw_root()); }
static void aw_teardown(void) {
    ray_test_hooks_off(); ray_encw_test_force(-1);
    free(g_img); g_img = NULL; g_len = 0;
    (void)ray_test_rm_rf(aw_root()); ray_runtime_destroy(g_awrt);
}

/* Every Rayfall string with a "/tmp" path goes through ray_test_tmp. */
__attribute__((unused)) static ray_t* aw_eval(const char* src) { return ray_eval_str(ray_test_tmp(src)); }
static void aw_eval_ok(const char* src) {
    ray_t* r = ray_eval_str(ray_test_tmp(src));
    if (r && RAY_IS_ERR(r)) { fprintf(stderr, "aw_eval_ok: %s: %s\n", src, ray_error_msg()); ray_error_free(r); abort(); }
    if (r) ray_release(r);
}

/* Save the rayfall table expression `tbl` twice: <name>0 as generation 0 and
 * <name>2 as generation 2 with every vector's top-level encoding forced to
 * `enc` (-1: the encoder's own choice). */
static void aw_save(const char* name, const char* tbl, int enc) {
    char q[4096];
    snprintf(q, sizeof q, "(set W %s)", tbl);
    aw_eval_ok(q);
    snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/%s0/\" W {compress: false})", name);
    aw_eval_ok(q);
    ray_encw_test_force(enc);
    snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/%s2/\" W {compress: true})", name);
    aw_eval_ok(q);
    ray_encw_test_force(-1);
}

/* Table <name><gen> loaded with .db.splayed.get (owned), or NULL. */
static ray_t* aw_load(const char* name, int gen) {
    char q[512];
    snprintf(q, sizeof q, "(.db.splayed.get \"" AW_TMP "/%s%d/\")", name, gen);
    ray_t* t = ray_eval_str(ray_test_tmp(q));
    return (t && !RAY_IS_ERR(t)) ? t : NULL;
}

/* Encoding of vector `vec` of column `col` of table <name>2 (its
 * descriptor, read in place), or -1. */
__attribute__((unused)) static int aw_vec_enc(const char* name, const char* col, int64_t vec) {
    ray_t* t = aw_load(name, 2);
    ray_t* encv = t ? ray_table_get_col_enc(t, ray_sym_intern(col, strlen(col))) : NULL;
    int enc = -1;
    if (encv) {
        enc = ray_enccol_descs(ray_enccol_of(encv), vec / RAY_ENC_VECS_PER_CHUNK)[vec % RAY_ENC_VECS_PER_CHUNK].encoding;
    }
    if (t) ray_release(t);
    return enc;
}

/* Runs `q` (table as T) on <name>0 and <name>2; equal formatted results and
 * exactly want_mat materializations on generation 2.  Leaves the
 * generation-2 table loaded as T (aw_col_decoded reads it). */
__attribute__((unused)) static test_result_t aw_same(const char* name, const char* q, uint64_t want_mat) {
    char buf[2048]; ray_t* out[2];
    for (int gen = 0; gen < 2; gen++) {
        snprintf(buf, sizeof buf, "(set T (.db.splayed.get \"" AW_TMP "/%s%d/\"))", name, gen ? 2 : 0);
        aw_eval_ok(buf);
        ray_enc_counters_reset();
        out[gen] = ray_eval_str(ray_test_tmp(q));
        TEST_ASSERT_FMT(out[gen] && !RAY_IS_ERR(out[gen]), "%s gen %d: %s", q, gen, ray_error_msg());
        if (gen) {
            ray_enc_counters_t k; ray_enc_counters_get(&k);
            TEST_ASSERT_FMT(k.materialized == want_mat, "%s: %llu materializations, want %llu", q,
                            (unsigned long long)k.materialized, (unsigned long long)want_mat);
        }
    }
    ray_t* fa = ray_fmt(out[0], 0); ray_t* fb = ray_fmt(out[1], 0);
    TEST_ASSERT_FMT(ray_str_len(fa) == ray_str_len(fb) && memcmp(ray_str_ptr(fa), ray_str_ptr(fb), ray_str_len(fa)) == 0,
                    "%s differs", q);
    ray_release(fa); ray_release(fb); ray_release(out[0]); ray_release(out[1]);
    PASS();
}
#define AW_SAME(n, q, m) do { test_result_t r_ = aw_same((n), (q), (m)); if (r_.status != TEST_PASS) return r_; } while (0)

/* aw_same with no materialization, then exactly want_decoded bytes decoded
 * by the generation-2 run (-1: not checked). */
__attribute__((unused)) static test_result_t aw_reduce(const char* name, const char* q, int64_t want_decoded) {
    test_result_t r = aw_same(name, q, 0);
    if (r.status != TEST_PASS) return r;
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    if (want_decoded >= 0 && !g_aw_unaligned)
        TEST_ASSERT_FMT(k.decoded_bytes == (uint64_t)want_decoded, "%s: decoded %llu, want %lld", q,
                        (unsigned long long)k.decoded_bytes, (long long)want_decoded);
    PASS();
}

/* Bytes decoded from column `col` of the loaded table T (P1's per-column
 * counter, ray_enccol_t.decoded_bytes, counted when a scratch moves on). */
__attribute__((unused)) static uint64_t aw_col_decoded(const char* col) {
    ray_t* t = ray_eval_str("T");
    ray_t* encv = t && !RAY_IS_ERR(t) ? ray_table_get_col_enc(t, ray_sym_intern(col, strlen(col))) : NULL;
    uint64_t b = encv ? atomic_load(&ray_enccol_of(encv)->decoded_bytes) : 0;
    if (t && !RAY_IS_ERR(t)) ray_release(t);
    return b;
}

/* Logical values of a view, expanded here only for the test oracle
 * (ray_enc_unpack is fine in a test). */
static uint64_t aw_lane(const ray_enc_lanes_t* l, int T, uint32_t i) {
    switch (T) { case 8: return l->b[i]; case 16: return l->h[i]; case 32: return l->s[i]; default: return l->q[i]; }
}
static void aw_view_values(const ray_enc_vview_t* v, int64_t* out) {
    uint64_t u[RAY_ENC_VEC];
    const uint64_t m = v->k.T == 64 ? ~0ull : ((1ull << v->k.T) - 1);
    switch (v->enc) {
    case RAY_ENC_CONST: for (int i = 0; i < v->n; i++) out[i] = ray_enc_logical(v->cbits, v->k); break;
    case RAY_ENC_FOR_BP:
        ray_enc_unpack(v->packed, v->k.T, v->width, u);
        for (int i = 0; i < v->n; i++) out[i] = ray_enc_logical((v->base + u[i]) & m, v->k);
        break;
    case RAY_ENC_RLE: {
        int i = 0;
        for (uint32_t r = 0; r < v->n_runs; r++)
            for (; i < v->run_ends.h[r]; i++) out[i] = ray_enc_logical(aw_lane(&v->run_vals, v->k.T, r), v->k);
        break;
    }
    case RAY_ENC_DICT: for (int i = 0; i < v->n; i++) out[i] = ray_enc_logical(v->dict[v->codes[i]], v->k); break;
    default: break;
    }
}

/* The residual lanes ray_enccol_unpack_resid writes equal the generic
 * unpack's (the oracle), at the kind's width. */
static bool aw_resid_ok(const ray_enc_vview_t* v) {
    _Alignas(64) ray_enc_lanes_t res;
    uint64_t u[RAY_ENC_VEC];
    ray_enccol_unpack_resid(v, &res);
    ray_enc_unpack(v->packed, v->k.T, v->width, u);
    for (int i = 0; i < v->n; i++) if (aw_lane(&res, v->k.T, (uint32_t)i) != u[i]) return false;
    return true;
}

static test_result_t aw_check_view(const char* name, const char* col, int enc) {
    ray_t* t2 = aw_load(name, 2); ray_t* t0 = aw_load(name, 0);
    TEST_ASSERT_TRUE(t2 && t0);
    int64_t sym = ray_sym_intern(col, strlen(col));
    ray_t* encv = ray_table_get_col_enc(t2, sym);
    TEST_ASSERT_NOT_NULL(encv);
    ray_t* plain = ray_table_col(t0, sym);
    ray_enccol_t* e = ray_enccol_of(encv);
    ray_enc_vview_t v;                                   /* on the stack, as a scan task holds it */
    int64_t vals[RAY_ENC_VEC];
    ray_enc_counters_reset();
    for (int64_t vec = 0; vec < ray_enc_nvecs(e->len); vec++) {
        ray_t* err = NULL;
        TEST_ASSERT_EQ_I(ray_enccol_vview(e, vec, &v, &err), 1);
        TEST_ASSERT_EQ_I(v.enc, enc);
        TEST_ASSERT_EQ_I(v.n, ray_enc_vec_len(e->len, vec));
        if (v.enc == RAY_ENC_FOR_BP) TEST_ASSERT_TRUE(aw_resid_ok(&v));
        aw_view_values(&v, vals);
        int nn = 0;
        for (int i = 0; i < v.n; i++) {
            int64_t r = vec * RAY_ENC_VEC + i;
            bool isnull = v.nullmap && ((v.nullmap[i >> 3] >> (i & 7)) & 1);
            nn += isnull;
            if (isnull) { TEST_ASSERT_TRUE(ray_vec_is_null(plain, r)); continue; }
            TEST_ASSERT_FMT(vals[i] == read_col_i64(ray_data(plain), r, plain->type, plain->attrs),
                            "%s row %lld", col, (long long)r);
        }
        if (v.nullmap) TEST_ASSERT_EQ_I(nn, v.n_nulls);
    }
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 0);                 /* a view is not a decode */
    TEST_ASSERT_EQ_U(k.materialized, 0);
    TEST_ASSERT_EQ_U(atomic_load(&e->decoded_bytes), 0);
    ray_release(t2); ray_release(t0);
    PASS();
}

static test_result_t test_vview_encodings(void) {
    /* 70000 rows: two chunks, nulls every 9th row in x */
    const char* tbl = "(table [x] (list (as 'I32 (map (fn [j] (if (== 0 (% j 9)) 0N (- (% j 50) 20))) (til 70000)))))";
    const char* cst = "(table [x] (list (as 'I32 (map (fn [j] (if (== 0 (% j 9)) 0N (div j 1024))) (til 70000)))))";
    const char* sym = "(table [s] (list (as 'SYMBOL (map (fn [j] (if (== 0 (% j 7)) \"\" (format \"w%\" (% j 13)))) (til 70000)))))";
    test_result_t r;
    aw_save("vf", tbl, RAY_ENC_FOR_BP); if ((r = aw_check_view("vf", "x", RAY_ENC_FOR_BP)).status != TEST_PASS) return r;
    aw_save("vr", tbl, RAY_ENC_RLE);    if ((r = aw_check_view("vr", "x", RAY_ENC_RLE)).status != TEST_PASS) return r;
    aw_save("vd", tbl, RAY_ENC_DICT);   if ((r = aw_check_view("vd", "x", RAY_ENC_DICT)).status != TEST_PASS) return r;
    aw_save("vc", cst, RAY_ENC_CONST);  if ((r = aw_check_view("vc", "x", RAY_ENC_CONST)).status != TEST_PASS) return r;
    aw_save("vs", sym, RAY_ENC_FOR_BP); if ((r = aw_check_view("vs", "s", RAY_ENC_FOR_BP)).status != TEST_PASS) return r;
    aw_save("vt", sym, RAY_ENC_RLE);    if ((r = aw_check_view("vt", "s", RAY_ENC_RLE)).status != TEST_PASS) return r;
    PASS();
}

/* FOR_BP vectors of column `col` of <name>2 whose worst case base +
 * (2^w - 1) passes the kind's largest value (the no-wrap check then reads
 * the actual residuals), or -1. */
static int64_t aw_near_top(const char* name, const char* col) {
    ray_t* t = aw_load(name, 2);
    ray_t* encv = t ? ray_table_get_col_enc(t, ray_sym_intern(col, strlen(col))) : NULL;
    if (!encv) { if (t) ray_release(t); return -1; }
    ray_enccol_t* e = ray_enccol_of(encv);
    ray_enc_vview_t v;
    int64_t hits = 0;
    for (int64_t vec = 0; vec < ray_enc_nvecs(e->len); vec++) {
        ray_t* err = NULL;
        if (ray_enccol_vview(e, vec, &v, &err) != 1 || v.enc != RAY_ENC_FOR_BP) { if (err) ray_error_free(err); continue; }
        const uint64_t m = v.k.T == 64 ? ~0ull : (1ull << v.k.T) - 1;
        const uint64_t umax = v.width >= 64 ? ~0ull : (1ull << v.width) - 1;
        const uint64_t room = v.k.is_signed ? (uint64_t)(int64_t)(m >> 1) - (uint64_t)ray_enc_logical(v.base, v.k) : m - v.base;
        hits += umax > room;
    }
    ray_release(t);
    return hits;
}

/* Sound FOR_BP vectors near the top of their kind: the writer's base is
 * the minimum, so base + (2^w - 1) may pass the largest value while every
 * actual residual stays inside it.  Each vouches and equals the decode. */
static test_result_t test_vview_near_top(void) {
    static const struct { const char* name; const char* col; const char* tbl; } fx[] = {
        { "nu", "x", "(table [x] (list (as 'U8 (map (fn [j] (+ 200 (% j 56))) (til 3000)))))" },
        { "n16", "x", "(table [x] (list (as 'I16 (map (fn [j] (if (== 0 (% j 9)) 0N (- 32767 (% j 48)))) (til 3000)))))" },
        { "n32", "x", "(table [x] (list (as 'I32 (map (fn [j] (if (== 0 (% j 9)) 0N (- 2147483647 (% j 48)))) (til 3000)))))" },
        { "n64", "x", "(table [x] (list (map (fn [j] (if (== 0 (% j 9)) 0N (- 9223372036854775807 (% j 48)))) (til 3000))))" },
        /* T = 64, w = 64: the least and the largest non-null value */
        { "nw", "x", "(table [x] (list (map (fn [j] (if (== 0 (% j 2)) -9223372036854775807 9223372036854775807)) (til 3000))))" },
        /* SYM W8: ids 1..250 in the first vector (the domain), 130..250 after */
        { "ns", "s", "(table [s] (list (as 'SYMBOL (map (fn [j] (format \"a%\" (if (< j 1024) (% j 250) (+ 129 (% j 121))))) (til 4000)))))" },
    };
    for (size_t i = 0; i < sizeof fx / sizeof fx[0]; i++) {
        aw_save(fx[i].name, fx[i].tbl, RAY_ENC_FOR_BP);
        test_result_t r = aw_check_view(fx[i].name, fx[i].col, RAY_ENC_FOR_BP);
        if (r.status != TEST_PASS) return r;
        TEST_ASSERT_FMT(aw_near_top(fx[i].name, fx[i].col) > 0, "%s: no vector near the top", fx[i].name);
    }
    PASS();
}

/* ---- Declines: single-column files patched in place (the P2 corrupt-test
 * pattern of test_enc_corrupt.c: the directory at byte 96, 64 bytes per
 * chunk; 48-byte descriptors; payloads after the descriptor area) ---- */

static bool img_load(const char* p) {
    free(g_img); g_img = NULL; g_len = 0;
    FILE* f = fopen(p, "rb"); if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long sz = ftell(f); if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
    g_len = (size_t)sz; g_img = malloc(g_len ? g_len : 1);
    bool ok = g_img && fread(g_img, 1, g_len, f) == g_len;
    fclose(f);
    return ok;
}
static bool img_store(const char* p) {
    FILE* f = fopen(p, "wb"); if (!f) return false;
    bool ok = fwrite(g_img, 1, g_len, f) == g_len;
    return fclose(f) == 0 && ok;
}
static uint64_t rd64(size_t off) { uint64_t x; memcpy(&x, g_img + off, 8); return x; }
#define DIR(c) (96 + 64 * (size_t)(c))
#define EH 32
static size_t desc_at(int j)    { return (size_t)rd64(DIR(0)) + 48 * (size_t)j; }
/* Save v (owned) as one generation-2 column file, top-level encoding forced
 * to `enc` (-1: the encoder's choice), and load its image. */
static bool dv_save(ray_t* v, int enc, const char* p) {
    ray_encw_test_force(enc);
    ray_err_t e = ray_col_save_gen(v, p, 2);
    ray_encw_test_force(-1);
    ray_release(v);
    return e == RAY_OK && img_load(p);
}
/* A SYM W8 column of n ids with saved count 200 (ids 200 and up are outside the domain). */
static bool dv_save_sym_n(const uint8_t* ids, int n, int enc, uint32_t count, const char* p) {
    FILE* f = fopen(p, "wb+");
    if (!f) return false;
    ray_encw_t w;
    ray_encw_test_force(enc);
    bool ok = ray_encw_begin(&w, f, RAY_SYM, RAY_SYM_W8, n, count, 0, 0) == RAY_OK &&
              ray_encw_chunk(&w, ids, NULL, n) == RAY_OK && ray_encw_finish(&w) == RAY_OK;
    ray_encw_test_force(-1);
    ray_encw_free(&w); fclose(f);
    return ok && img_load(p);
}

static bool dv_save_sym(const uint8_t* ids, int n, int enc, const char* p) { return dv_save_sym_n(ids, n, enc, 200, p); }

static bool msg_has(const char* needle) {
    const char* m = ray_error_msg();
    return m && strstr(m, needle) != NULL;
}

/* Store the image; the view of vector 0 returns `want` (-1: an error
 * `corrupt` naming `needle`); then a range read of the vector reports
 * `corrupt` naming `needle` (NULL: the read succeeds). */
static const char* dv_expect(const char* p, int want, const char* needle) {
    static char why[512];
    if (!img_store(p)) return "store";
    ray_t* c = ray_col_mmap(p);
    if (!c || RAY_IS_ERR(c)) {
        snprintf(why, sizeof why, "load failed: %s", c ? ray_error_msg() : "null");
        if (c) ray_error_free(c);
        return why;
    }
    const char* res = NULL;
    ray_enc_vview_t v; ray_t* err = NULL;
    ray_error_clear();
    int got = ray_enccol_vview(ray_enccol_of(c), 0, &v, &err);
    if (got != want) {
        snprintf(why, sizeof why, "view gave %d (%s), want %d [%s]", got, err ? ray_error_msg() : "", want, needle ? needle : "");
        res = why;
    } else if (want == -1 && !(err && strcmp(ray_err_code(err), "corrupt") == 0 && msg_has(needle))) {
        snprintf(why, sizeof why, "view error %s: \"%s\", want corrupt [%s]", err ? ray_err_code(err) : "none",
                 err ? ray_error_msg() : "", needle);
        res = why;
    } else if (want != -1 && err) {
        res = "an error beside a 0 / 1";
    }
    if (err) ray_error_free(err);
    if (!res) {
        ray_col_src_t src;
        ray_t* m = ray_col_src_bind(c, &src);
        if (m) { ray_error_free(m); res = "bind failed"; }
        else {
            ray_col_scratch_t sc; ray_col_scratch_init(&sc);
            ray_col_range_t r;
            ray_error_clear();
            m = ray_col_range(&src, 0, src.len < RAY_ENC_VEC ? src.len : RAY_ENC_VEC, &sc, &r);
            bool is_err = m && RAY_IS_ERR(m);
            if (needle ? !(is_err && strcmp(ray_err_code(m), "corrupt") == 0 && msg_has(needle)) : is_err) {
                snprintf(why, sizeof why, "range read gave %s: \"%s\", want %s [%s]", is_err ? ray_err_code(m) : "a view",
                         is_err && ray_error_msg() ? ray_error_msg() : "", needle ? "corrupt" : "a view", needle ? needle : "");
                res = why;
            }
            if (is_err) ray_error_free(m);
            ray_col_scratch_free(&sc); ray_col_src_release(&src);
        }
    }
    ray_release(c);
    return res;
}
#define DV(p, want, needle) do { const char* w_ = dv_expect((p), (want), (needle)); \
    TEST_ASSERT_FMT(w_ == NULL, "%s:%d %s", __FILE__, __LINE__, w_); } while (0)
#define DV_SAVE(v, enc, p) TEST_ASSERT_TRUE(dv_save((v), (enc), (p)))
#define DV_SAVE_SYM(ids, n, enc, p) TEST_ASSERT_TRUE(dv_save_sym((ids), (n), (enc), (p)))
#define NEED_ENC(j, enc) TEST_ASSERT_FMT(g_img[desc_at(j)] == (enc), \
    "descriptor %d is encoding %d, the test pins %d", j, g_img[desc_at(j)], enc)

/* 1024 rows of `type` (U8 / I16 / I32): base + a hash of i in [0, 64). */
static ray_t* dv_hash(int8_t type, int base, int null_every) {
    ray_t* v = ray_vec_new(type, 1024);
    if (!v || RAY_IS_ERR(v)) return NULL;
    v->len = 1024;
    for (int i = 0; i < 1024; i++) {
        int x = base + (int)(((uint32_t)i * 2654435761u) >> 26);
        bool nul = null_every && i % null_every == 0;
        if (type == RAY_U8) ((uint8_t*)ray_data(v))[i] = (uint8_t)x;
        else if (type == RAY_I16) ((int16_t*)ray_data(v))[i] = nul ? NULL_I16 : (int16_t)x;
        else ((int32_t*)ray_data(v))[i] = nul ? NULL_I32 : x;
    }
    if (null_every) v->attrs |= RAY_ATTR_HAS_NULLS;
    return v;
}
/* 1024 I32 rows: runs of 100 (values 7 + run), nulls every 10th row when asked. */
static ray_t* dv_runs(int null_every, bool constant) {
    ray_t* v = ray_vec_new(RAY_I32, 1024); v->len = 1024;
    for (int i = 0; i < 1024; i++)
        ((int32_t*)ray_data(v))[i] = null_every && i % null_every == 0 ? NULL_I32 : 7 + (constant ? 0 : i / 100);
    if (null_every) v->attrs |= RAY_ATTR_HAS_NULLS;
    return v;
}
/* Four wide values at hash-scattered rows: DICT over a FOR_BP w=2 code child. */
static ray_t* dv_dict(void) {
    static const int64_t vals[4] = { 11, 900000000001ll, 77, 123456789012345ll };
    ray_t* v = ray_vec_new(RAY_I64, 1024); v->len = 1024;
    for (int i = 0; i < 1024; i++) ((int64_t*)ray_data(v))[i] = vals[(uint32_t)((uint32_t)i * 2654435761u) >> 30];
    return v;
}
static ray_t* dv_f64(bool constant) {
    ray_t* v = ray_vec_new(RAY_F64, 1024); v->len = 1024;
    for (int i = 0; i < 1024; i++) ((double*)ray_data(v))[i] = constant ? 2.5 : (double)i * 0.25;
    return v;
}

/* RAW and DELTA_BP have no view; a view never vouches for a vector its
 * decode rejects.  One sub-case per rule of the view's validation: each
 * declines (0) or reports what the decode reports (-1), and the range read
 * of the same vector then reports it `corrupt` with its exact rule. */
static test_result_t test_vview_declines(void) {
    const char* p = ray_test_tmp(AW_TMP "/dv");
    for (int i = 0; i < 256; i++) { char b[8]; int n = snprintf(b, sizeof b, "s%d", i); (void)ray_sym_intern(b, (size_t)n); }
    /* no view: RAW, DELTA_BP, a float other than CONST */
    DV_SAVE(dv_hash(RAY_I32, 5000, 0), RAY_ENC_RAW, p); NEED_ENC(0, RAY_ENC_RAW);   DV(p, 0, NULL);
    {
        ray_t* v = ray_vec_new(RAY_I32, 1024); v->len = 1024;
        for (int i = 0; i < 1024; i++) ((int32_t*)ray_data(v))[i] = i * 3 + (i * 7) % 5;
        DV_SAVE(v, RAY_ENC_DELTA_BP, p); NEED_ENC(0, RAY_ENC_DELTA_BP); DV(p, 0, NULL);
    }
    DV_SAVE(dv_f64(false), RAY_ENC_RAW, p); NEED_ENC(0, RAY_ENC_RAW); DV(p, 0, NULL);
    /* a view: FOR_BP (with nulls too), CONST, RLE, SYM, DICT, float CONST */
    DV_SAVE(dv_hash(RAY_I32, 5000, 0), RAY_ENC_FOR_BP, p); NEED_ENC(0, RAY_ENC_FOR_BP); DV(p, 1, NULL);
    DV_SAVE(dv_hash(RAY_I32, 5000, 10), RAY_ENC_FOR_BP, p); NEED_ENC(0, RAY_ENC_FOR_BP); DV(p, 1, NULL);
    DV_SAVE(dv_hash(RAY_I16, 5000, 10), RAY_ENC_FOR_BP, p); NEED_ENC(0, RAY_ENC_FOR_BP); DV(p, 1, NULL);
    DV_SAVE(dv_hash(RAY_U8, 0, 0), RAY_ENC_FOR_BP, p); NEED_ENC(0, RAY_ENC_FOR_BP); DV(p, 1, NULL);
    DV_SAVE(dv_runs(0, true), RAY_ENC_CONST, p); NEED_ENC(0, RAY_ENC_CONST); DV(p, 1, NULL);
    DV_SAVE(dv_runs(10, true), RAY_ENC_CONST, p); NEED_ENC(0, RAY_ENC_CONST); DV(p, 1, NULL);
    DV_SAVE(dv_runs(0, false), RAY_ENC_RLE, p); NEED_ENC(0, RAY_ENC_RLE); DV(p, 1, NULL);
    {
        uint8_t ids[1024];
        for (int i = 0; i < 1024; i++) ids[i] = 5;
        DV_SAVE_SYM(ids, 1024, RAY_ENC_CONST, p); NEED_ENC(0, RAY_ENC_CONST); DV(p, 1, NULL);
        for (int i = 0; i < 1024; i++) ids[i] = (uint8_t)(1 + i / 100);
        DV_SAVE_SYM(ids, 1024, RAY_ENC_RLE, p); NEED_ENC(0, RAY_ENC_RLE); DV(p, 1, NULL);
        for (int i = 0; i < 1024; i++) ids[i] = (uint8_t)(100 + i % 16);
        DV_SAVE_SYM(ids, 1024, RAY_ENC_FOR_BP, p); NEED_ENC(0, RAY_ENC_FOR_BP); DV(p, 1, NULL);
    }
    DV_SAVE(dv_dict(), RAY_ENC_DICT, p); NEED_ENC(0, RAY_ENC_DICT); DV(p, 1, NULL);
    DV_SAVE(dv_f64(true), RAY_ENC_CONST, p); NEED_ENC(0, RAY_ENC_CONST); DV(p, 1, NULL);
    PASS();
}

/* ray_col_src_vview: a single-file generation-2 source gives its views; a
 * generation-0 source and a published materialization decline (read them
 * in place). */
static test_result_t test_vview_source(void) {
    aw_save("vq", "(table [x] (list (as 'I32 (map (fn [j] (% j 50)) (til 3000)))))", RAY_ENC_FOR_BP);
    for (int gen = 0; gen < 2; gen++) {
        ray_t* t = aw_load("vq", gen ? 2 : 0);
        TEST_ASSERT_NOT_NULL(t);
        ray_col_src_t s;
        ray_t* err = ray_table_col_src(t, ray_sym_intern("x", 1), &s);
        TEST_ASSERT_NULL(err);
        ray_enc_vview_t v;
        for (int64_t vec = 0; vec < 3; vec++) {
            TEST_ASSERT_EQ_I(ray_col_src_vview(&s, vec, &v, &err), gen ? 1 : 0);
            TEST_ASSERT_NULL(err);
            if (gen) { TEST_ASSERT_EQ_I(v.enc, RAY_ENC_FOR_BP); TEST_ASSERT_EQ_I(v.n, vec < 2 ? 1024 : 3000 - 2048); }
        }
        TEST_ASSERT_EQ_I(ray_col_src_vview(&s, 3, &v, &err), 0);     /* past the end */
        TEST_ASSERT_EQ_I(ray_col_src_vview(&s, -1, &v, &err), 0);
        if (gen) {
            ray_t* m = ray_enccol_materialize(s.obj);
            TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
            TEST_ASSERT_EQ_I(ray_col_src_vview(&s, 0, &v, &err), 0);   /* published: read it in place */
        }
        ray_col_src_release(&s);
        ray_release(t);
    }
    PASS();
}

/* ---- Encoded comparison and membership evaluators (store/enceval.h) ---- */

static int aw_ref_cmp(int64_t x, uint16_t op, int64_t c) {
    switch (op) {
    case OP_EQ: return x == c; case OP_NE: return x != c;
    case OP_LT: return x < c;  case OP_LE: return x <= c;
    case OP_GT: return x > c;  case OP_GE: return x >= c;
    default: return 0;
    }
}
/* null_result as the fused kernels give it: a null ranks below every value. */
static uint8_t aw_null_result(uint16_t op) { return op == OP_NE || op == OP_LT || op == OP_LE; }

static test_result_t aw_eval_all(const char* name, int enc, const int64_t* consts, int nc) {
    ray_t* t = aw_load(name, 2);
    TEST_ASSERT_NOT_NULL(t);
    ray_enccol_t* e = ray_enccol_of(ray_table_get_col_enc(t, ray_sym_intern("x", 1)));
    ray_enc_vview_t vb, *v = &vb;                 /* a scan task's stack view */
    ray_t* verr = NULL;
    int64_t vals[RAY_ENC_VEC]; uint8_t bits[RAY_ENC_VEC];
    static const uint16_t ops[] = { OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE };
    ray_enc_counters_reset();
    for (int64_t vec = 0; vec < ray_enc_nvecs(e->len); vec++) {
        TEST_ASSERT_EQ_I(ray_enccol_vview(e, vec, v, &verr), 1);
        TEST_ASSERT_EQ_I(v->enc, enc);
        aw_view_values(v, vals);
        for (int oi = 0; oi < 6; oi++) for (int ci = 0; ci < nc; ci++) {
            uint8_t nr = aw_null_result(ops[oi]);
            TEST_ASSERT_TRUE(ray_enc_eval_cmp(v, ops[oi], consts[ci], nr, bits));
            for (int i = 0; i < v->n; i++) {
                bool isnull = v->nullmap && ((v->nullmap[i >> 3] >> (i & 7)) & 1);
                int want = isnull ? nr : aw_ref_cmp(vals[i], ops[oi], consts[ci]);
                TEST_ASSERT_FMT(bits[i] == want, "%s vec %lld op %u c %lld row %d", name,
                                (long long)vec, ops[oi], (long long)consts[ci], i);
            }
        }
    }
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 0);         /* answered from the views, nothing decoded */
    TEST_ASSERT_EQ_U(k.materialized, 0);
    ray_release(t);
    PASS();
}

static test_result_t test_eval_encodings(void) {
    const char* tbl = "(table [x] (list (as 'I32 (map (fn [j] (if (== 0 (% j 9)) 0N (- (% j 50) 20))) (til 3000)))))";
    static const int64_t cs[] = { -100, -21, -20, -1, 0, 7, 29, 30, 1000 };
    test_result_t r;
    aw_save("ef", tbl, RAY_ENC_FOR_BP); if ((r = aw_eval_all("ef", RAY_ENC_FOR_BP, cs, 9)).status != TEST_PASS) return r;
    aw_save("er", tbl, RAY_ENC_RLE);    if ((r = aw_eval_all("er", RAY_ENC_RLE, cs, 9)).status != TEST_PASS) return r;
    aw_save("ed", tbl, RAY_ENC_DICT);   if ((r = aw_eval_all("ed", RAY_ENC_DICT, cs, 9)).status != TEST_PASS) return r;
    aw_save("ec", "(table [x] (list (as 'I32 (map (fn [j] (if (== 0 (% j 9)) 0N 7)) (til 3000)))))", RAY_ENC_CONST);
    if ((r = aw_eval_all("ec", RAY_ENC_CONST, cs, 9)).status != TEST_PASS) return r;
    /* I64 at the domain edges, residuals at 64 bits */
    static const int64_t c64[] = { INT64_MIN + 1, -9223372036854775806LL, -1, 0, 9223372036854775806LL, INT64_MAX };
    aw_save("e64", "(table [x] (list (as 'I64 (map (fn [j] (if (== 0 (% j 3)) -9223372036854775807 (if (== 1 (% j 3)) 9223372036854775807 0))) (til 3000)))))",
            RAY_ENC_FOR_BP);
    if ((r = aw_eval_all("e64", RAY_ENC_FOR_BP, c64, 6)).status != TEST_PASS) return r;
    PASS();
}

/* Review focus 2: signedness and the domain edges. */
static test_result_t test_eval_for_edges(void) {
    static const int64_t c16[] = { -32768, -32767, -151, -150, -129, -128, 0, 127, 128, 149, 150, 32767 };
    test_result_t r;
    aw_save("e16", "(table [x] (list (as 'I16 (- (% (til 3000) 300) 150))))", RAY_ENC_FOR_BP);
    if ((r = aw_eval_all("e16", RAY_ENC_FOR_BP, c16, 12)).status != TEST_PASS) return r;
    static const int64_t cu8[] = { -1, 0, 1, 99, 100, 126, 127, 128, 129, 159, 160, 255, 256 };
    aw_save("eu8", "(table [x] (list (as 'U8 (+ 100 (% (til 3000) 60)))))", RAY_ENC_FOR_BP);   /* 100..159 crosses 127/128 */
    if ((r = aw_eval_all("eu8", RAY_ENC_FOR_BP, cu8, 13)).status != TEST_PASS) return r;
    static const int64_t c32[] = { -2147483647LL, -1000001, -1000000, -999999, -999994, -999993, 0, 2147483647LL };
    aw_save("e32", "(table [x] (list (as 'I32 (- (% (til 3000) 7) 1000000))))", RAY_ENC_FOR_BP);
    if ((r = aw_eval_all("e32", RAY_ENC_FOR_BP, c32, 8)).status != TEST_PASS) return r;
    PASS();
}

/* Review focus 1: a nullable CONST vector. */
static test_result_t test_eval_const_nulls(void) {
    aw_save("cn", "(table [x] (list (as 'I32 (map (fn [j] (if (== 0 (% j 5)) 0N 10)) (til 1024)))))", RAY_ENC_CONST);
    ray_t* t = aw_load("cn", 2);
    TEST_ASSERT_NOT_NULL(t);
    ray_enccol_t* e = ray_enccol_of(ray_table_get_col_enc(t, ray_sym_intern("x", 1)));
    ray_enc_vview_t vb, *v = &vb;                 /* a scan task's stack view */
    ray_t* verr = NULL;
    TEST_ASSERT_EQ_I(ray_enccol_vview(e, 0, v, &verr), 1);
    TEST_ASSERT_EQ_I(v->enc, RAY_ENC_CONST);
    TEST_ASSERT_EQ_I(ray_enc_const_decision(v, OP_LT, 20, 1), 1);    /* 10 < 20 and null < 20: ALL */
    TEST_ASSERT_EQ_I(ray_enc_const_decision(v, OP_GT, 5, 0), -1);    /* 10 > 5, nulls fail: MIX */
    TEST_ASSERT_EQ_I(ray_enc_const_decision(v, OP_EQ, 3, 0), 0);     /* nothing passes: NONE */
    TEST_ASSERT_EQ_I(ray_enc_const_decision(v, OP_NE, 3, 1), 1);     /* 10 != 3 and null != 3: ALL */
    TEST_ASSERT_EQ_I(ray_enc_const_decision(v, OP_GE, 20, 1), -1);   /* 10 fails, nulls pass: MIX */
    TEST_ASSERT_EQ_I(ray_enc_const_decision_f64(v, OP_LT, 20.0, 1), -1);   /* not a float view */
    TEST_ASSERT_EQ_I(ray_enc_const_decision(v, OP_EQ - 1, 10, 0), -1);     /* not a comparison */
    TEST_ASSERT_EQ_I(ray_enc_const_decision(v, OP_GE + 1, 10, 0), -1);
    { uint8_t bits[RAY_ENC_VEC]; TEST_ASSERT_FALSE(ray_enc_eval_cmp(v, OP_GE + 1, 10, 0, bits)); }
    ray_release(t);
    /* every row null: every row is null_result */
    aw_save("ca", "(table [x] (list (as 'I32 (map (fn [j] 0N) (til 1024)))))", RAY_ENC_CONST);
    t = aw_load("ca", 2);
    TEST_ASSERT_NOT_NULL(t);
    e = ray_enccol_of(ray_table_get_col_enc(t, ray_sym_intern("x", 1)));
    TEST_ASSERT_EQ_I(ray_enccol_vview(e, 0, v, &verr), 1);
    TEST_ASSERT_EQ_I(v->enc, RAY_ENC_CONST);
    TEST_ASSERT_EQ_I(ray_enc_const_decision(v, OP_GT, 5, 0), 0);
    TEST_ASSERT_EQ_I(ray_enc_const_decision(v, OP_LT, 5, 1), 1);
    ray_release(t);
    /* no nulls: decided whole either way */
    aw_save("cv", "(table [x] (list (as 'I32 (map (fn [j] 10) (til 1024)))))", RAY_ENC_CONST);
    t = aw_load("cv", 2);
    TEST_ASSERT_NOT_NULL(t);
    e = ray_enccol_of(ray_table_get_col_enc(t, ray_sym_intern("x", 1)));
    TEST_ASSERT_EQ_I(ray_enccol_vview(e, 0, v, &verr), 1);
    TEST_ASSERT_EQ_I(ray_enc_const_decision(v, OP_GT, 5, 0), 1);
    TEST_ASSERT_EQ_I(ray_enc_const_decision(v, OP_LT, 5, 1), 0);
    ray_release(t);
    PASS();
}

static test_result_t test_eval_in(void) {
    aw_save("ei", "(table [x] (list (as 'I32 (map (fn [j] (if (== 0 (% j 11)) 0N (% j 40))) (til 3000)))))", RAY_ENC_DICT);
    aw_save("eiF", "(table [x] (list (as 'I32 (map (fn [j] (if (== 0 (% j 11)) 0N (% j 40))) (til 3000)))))", RAY_ENC_FOR_BP);
    aw_save("eiR", "(table [x] (list (as 'I32 (map (fn [j] (if (== 0 (% j 11)) 0N (% j 40))) (til 3000)))))", RAY_ENC_RLE);
    aw_save("eiC", "(table [x] (list (as 'I32 (map (fn [j] (if (== 0 (% j 11)) 0N 17)) (til 3000)))))", RAY_ENC_CONST);
    static const char* names[] = { "ei", "eiF", "eiR", "eiC" };
    static const int encs[] = { RAY_ENC_DICT, RAY_ENC_FOR_BP, RAY_ENC_RLE, RAY_ENC_CONST };
    static const int64_t set[] = { -5, 3, 17, 39, 41 };
    ray_enc_counters_reset();
    for (int s = 0; s < 4; s++) {
        ray_t* t = aw_load(names[s], 2);
        TEST_ASSERT_NOT_NULL(t);
        ray_enccol_t* e = ray_enccol_of(ray_table_get_col_enc(t, ray_sym_intern("x", 1)));
        ray_enc_vview_t vb, *v = &vb;             /* a scan task's stack view */
        ray_t* verr = NULL;
        int64_t vals[RAY_ENC_VEC]; uint8_t bits[RAY_ENC_VEC];
        for (int64_t vec = 0; vec < 3; vec++) {
            TEST_ASSERT_EQ_I(ray_enccol_vview(e, vec, v, &verr), 1);
            TEST_ASSERT_EQ_I(v->enc, encs[s]);
            aw_view_values(v, vals);
            for (uint8_t nr = 0; nr < 2; nr++) {
                TEST_ASSERT_TRUE(ray_enc_eval_in(v, set, 5, nr, bits));
                for (int i = 0; i < v->n; i++) {
                    bool isnull = v->nullmap && ((v->nullmap[i >> 3] >> (i & 7)) & 1);
                    int want = isnull ? nr : (vals[i] == 3 || vals[i] == 17 || vals[i] == 39);
                    TEST_ASSERT_FMT(bits[i] == want, "%s vec %lld row %d", names[s], (long long)vec, i);
                }
                TEST_ASSERT_TRUE(ray_enc_eval_in(v, set, 0, nr, bits));   /* the empty set */
                for (int i = 0; i < v->n; i++) {
                    bool isnull = v->nullmap && ((v->nullmap[i >> 3] >> (i & 7)) & 1);
                    TEST_ASSERT_EQ_I(bits[i], isnull ? nr : 0);
                }
            }
        }
        ray_release(t);
    }
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 0);         /* answered from the views, nothing decoded */
    TEST_ASSERT_EQ_U(k.materialized, 0);
    PASS();
}

/* The integer evaluators against the generation-0 engine itself: the
 * vector comparison of the gen-0 copy, row for row, on a nullable I16 and
 * a U8 column under every encoding, with constants inside, at the edges
 * of and outside the vectors' range (and the type's). */
static test_result_t test_eval_gen0(void) {
    static const uint16_t ops[] = { OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE };
    static const char* opname[] = { "==", "!=", "<", "<=", ">", ">=" };
    static const int encs[] = { RAY_ENC_FOR_BP, RAY_ENC_RLE, RAY_ENC_DICT };
    static const struct { const char* tbl; int64_t cs[8]; int nc; bool nullable; } cols[] = {
        { "(table [x] (list (as 'I16 (map (fn [j] (if (== 0 (% j 13)) 0N (- (% (div j 3) 300) 150))) (til 3000)))))",
          { -40000, -32768, -151, -150, 0, 149, 150, 40000 }, 8, true },
        { "(table [x] (list (as 'U8 (+ 100 (% (div (til 3000) 3) 60)))))",
          { -1, 0, 99, 100, 127, 128, 159, 300 }, 8, false },
    };
    uint8_t bits[RAY_ENC_VEC];
    for (int ci = 0; ci < 2; ci++) for (int ei = 0; ei < 3; ei++) {
        if (encs[ei] == RAY_ENC_DICT && !cols[ci].nullable) continue;   /* DICT views are signed kinds only */
        aw_save("eg", cols[ci].tbl, encs[ei]);
        ray_t* t = aw_load("eg", 2);
        TEST_ASSERT_NOT_NULL(t);
        ray_enccol_t* e = ray_enccol_of(ray_table_get_col_enc(t, ray_sym_intern("x", 1)));
        ray_enc_vview_t vb, *v = &vb;
        ray_t* verr = NULL;
        for (int oi = 0; oi < 6; oi++) for (int k = 0; k < cols[ci].nc; k++) {
            char q[256];
            snprintf(q, sizeof q, "(%s (at (.db.splayed.get \"" AW_TMP "/eg0/\") 'x) %lld)", opname[oi],
                     (long long)cols[ci].cs[k]);
            ray_t* r = aw_eval(q);
            TEST_ASSERT_FMT(r && !RAY_IS_ERR(r) && r->type == RAY_BOOL && r->len == e->len, "%s", q);
            const uint8_t* rb = (const uint8_t*)ray_data(r);
            const uint8_t nr = aw_null_result(ops[oi]);
            for (int64_t vec = 0; vec < ray_enc_nvecs(e->len); vec++) {
                TEST_ASSERT_EQ_I(ray_enccol_vview(e, vec, v, &verr), 1);
                TEST_ASSERT_EQ_I(v->enc, encs[ei]);
                TEST_ASSERT_TRUE(ray_enc_eval_cmp(v, ops[oi], cols[ci].cs[k], nr, bits));
                for (int i = 0; i < v->n; i++)
                    TEST_ASSERT_FMT(bits[i] == rb[vec * RAY_ENC_VEC + i], "%s enc %d vec %lld row %d", q, encs[ei],
                                    (long long)vec, i);
            }
            ray_release(r);
        }
        ray_release(t);
        char rm[256];
        snprintf(rm, sizeof rm, "%s/eg0", aw_root()); (void)ray_test_rm_rf(rm);
        snprintf(rm, sizeof rm, "%s/eg2", aw_root()); (void)ray_test_rm_rf(rm);
    }
    PASS();
}

/* SYM: ids compare as unsigned, == / != / in only; an ordered operator
 * declines (never order ids, #738). */
static test_result_t test_eval_sym(void) {
    static const int encs[] = { RAY_ENC_FOR_BP, RAY_ENC_RLE, RAY_ENC_CONST };
    static const char* tbls[] = {
        "(table [x] (list (as 'SYM (map (fn [j] (if (== 0 (% j 7)) (as 'SYM \"\") (at ['a 'b 'c 'd 'e] (% j 5)))) (til 3000)))))",
        "(table [x] (list (as 'SYM (map (fn [j] (if (== 0 (% (div j 100) 7)) (as 'SYM \"\") (at ['a 'b 'c 'd 'e] (% (div j 100) 5)))) (til 3000)))))",
        "(table [x] (list (take ['b] 3000)))",
    };
    static const uint16_t ord[] = { OP_LT, OP_LE, OP_GT, OP_GE };
    for (int s = 0; s < 3; s++) {
        char nm[8]; snprintf(nm, sizeof nm, "es%d", s);
        aw_save(nm, tbls[s], encs[s]);
        ray_t* t = aw_load(nm, 2);
        ray_enc_counters_reset();
        TEST_ASSERT_NOT_NULL(t);
        ray_enccol_t* e = ray_enccol_of(ray_table_get_col_enc(t, ray_sym_intern("x", 1)));
        TEST_ASSERT_EQ_I(e->type, RAY_SYM);
        ray_enc_vview_t vb, *v = &vb;
        ray_t* verr = NULL;
        int64_t vals[RAY_ENC_VEC]; uint8_t bits[RAY_ENC_VEC];
        for (int64_t vec = 0; vec < ray_enc_nvecs(e->len); vec++) {
            TEST_ASSERT_EQ_I(ray_enccol_vview(e, vec, v, &verr), 1);
            TEST_ASSERT_EQ_I(v->enc, encs[s]);
            aw_view_values(v, vals);
            for (int o = 0; o < 4; o++) TEST_ASSERT_FALSE(ray_enc_eval_cmp(v, ord[o], vals[0], 0, bits));
            /* constants: every id of the vector, "" (id 0) and an id past the bound */
            int64_t cs[RAY_ENC_VEC + 2]; int nc = 0;
            cs[nc++] = 0; cs[nc++] = ray_sym_domain_count(e->dom) + 5;
            for (int i = 0; i < v->n && nc < 8; i++) { bool dup = false; for (int q = 0; q < nc; q++) dup |= cs[q] == vals[i]; if (!dup) cs[nc++] = vals[i]; }
            for (int ci = 0; ci < nc; ci++) for (int o = 0; o < 2; o++) {
                const uint16_t op = o ? OP_NE : OP_EQ;
                TEST_ASSERT_TRUE(ray_enc_eval_cmp(v, op, cs[ci], 0, bits));
                for (int i = 0; i < v->n; i++) TEST_ASSERT_EQ_I(bits[i], aw_ref_cmp(vals[i], op, cs[ci]));
            }
            int64_t set[8]; int ns = 0;                 /* sorted, distinct */
            for (int ci = 0; ci < nc; ci += 2) set[ns++] = cs[ci];
            for (int a = 1; a < ns; a++) for (int b = a; b > 0 && set[b - 1] > set[b]; b--) { int64_t x = set[b]; set[b] = set[b - 1]; set[b - 1] = x; }
            TEST_ASSERT_TRUE(ray_enc_eval_in(v, set, ns, 0, bits));
            for (int i = 0; i < v->n; i++) {
                int want = 0; for (int q = 0; q < ns; q++) want |= vals[i] == set[q];
                TEST_ASSERT_EQ_I(bits[i], want);
            }
        }
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        TEST_ASSERT_EQ_U(k.decoded_bytes, 0);
        TEST_ASSERT_EQ_U(k.materialized, 0);
        ray_release(t);
    }
    PASS();
}

/* ray_enc_const_decision_f64 on CONST -0.0, +0.0 and nullable 1.5 vectors
 * (F64 and F32) against both zero literals under all six operators: the
 * answer equals the generation-0 comparison's over the vector's rows
 * (1 all pass, 0 none, -1 mixed). */
static ray_t* aw_fvec(int8_t type, double x, int null_every) {
    ray_t* v = ray_vec_new(type, 1024);
    if (!v || RAY_IS_ERR(v)) return NULL;
    v->len = 1024;
    for (int i = 0; i < 1024; i++) {
        if (type == RAY_F64) ((double*)ray_data(v))[i] = x; else ((float*)ray_data(v))[i] = (float)x;
    }
    if (null_every) for (int i = 0; i < 1024; i += null_every) ray_vec_set_null(v, i, true);
    return v;
}
static test_result_t test_eval_float_const(void) {
    static const uint16_t ops[] = { OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE };
    static const char* opname[] = { "==", "!=", "<", "<=", ">", ">=" };
    static const double xs[] = { -0.0, 0.0, 1.5 };
    static const int nulls[] = { 0, 0, 5 };
    static const int8_t types[] = { RAY_F64, RAY_F32 };
    static const double cs[] = { -0.0, 0.0 };
    static const char* clit[] = { "-0.0", "0.0" };
    for (int ti = 0; ti < 2; ti++) for (int xi = 0; xi < 3; xi++) {
        ray_t* col = aw_fvec(types[ti], xs[xi], nulls[xi]);
        TEST_ASSERT_NOT_NULL(col);
        ray_t* tb = ray_table_new(1);
        tb = ray_table_add_col(tb, ray_sym_intern("x", 1), col); ray_release(col);
        TEST_ASSERT_TRUE(tb && !RAY_IS_ERR(tb));
        TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("WF", 2), tb), RAY_OK);
        ray_release(tb);
        aw_save("fc", "WF", RAY_ENC_CONST);
        ray_t* t = aw_load("fc", 2);
        TEST_ASSERT_NOT_NULL(t);
        ray_enccol_t* e = ray_enccol_of(ray_table_get_col_enc(t, ray_sym_intern("x", 1)));
        ray_enc_vview_t vb, *v = &vb;
        ray_t* verr = NULL;
        TEST_ASSERT_EQ_I(ray_enccol_vview(e, 0, v, &verr), 1);
        TEST_ASSERT_EQ_I(v->enc, RAY_ENC_CONST);
        if (xi == 0) TEST_ASSERT_EQ_U(v->cbits, types[ti] == RAY_F64 ? 0x8000000000000000ull : 0x80000000ull);
        uint8_t bits[RAY_ENC_VEC];
        TEST_ASSERT_FALSE(ray_enc_eval_cmp(v, OP_EQ, 0, 0, bits));       /* floats: no integer evaluator */
        TEST_ASSERT_EQ_I(ray_enc_const_decision(v, OP_EQ, 0, 0), -1);
        for (int oi = 0; oi < 6; oi++)                                   /* a NaN constant is the null one */
            TEST_ASSERT_EQ_I(ray_enc_const_decision_f64(v, ops[oi], NAN, aw_null_result(ops[oi])), -1);
        TEST_ASSERT_EQ_I(ray_enc_const_decision_f64(v, OP_EQ - 1, 0.0, 0), -1);   /* not a comparison */
        TEST_ASSERT_EQ_I(ray_enc_const_decision_f64(v, OP_GE + 1, 0.0, 0), -1);
        for (int oi = 0; oi < 6; oi++) for (int ci = 0; ci < 2; ci++) {
            char q[256];
            snprintf(q, sizeof q, "(%s (at (.db.splayed.get \"" AW_TMP "/fc0/\") 'x) %s)", opname[oi], clit[ci]);
            ray_t* r = aw_eval(q);
            TEST_ASSERT_FMT(r && !RAY_IS_ERR(r) && r->type == RAY_BOOL && r->len == 1024, "%s", q);
            const uint8_t* rb = (const uint8_t*)ray_data(r);
            int all = 1, none = 1; uint8_t nr = aw_null_result(ops[oi]);
            for (int i = 0; i < 1024; i++) {
                all &= rb[i] != 0; none &= rb[i] == 0;
                if (nulls[xi] && i % nulls[xi] == 0) TEST_ASSERT_EQ_I(rb[i], nr);   /* a null ranks below */
            }
            ray_release(r);
            int want = all ? 1 : none ? 0 : -1;
            TEST_ASSERT_FMT(ray_enc_const_decision_f64(v, ops[oi], cs[ci], nr) == want,
                            "type %d x %g %s %s", types[ti], xs[xi], opname[oi], clit[ci]);
        }
        ray_release(t);
        char rm[256];
        snprintf(rm, sizeof rm, "%s/fc0", aw_root()); (void)ray_test_rm_rf(rm);
        snprintf(rm, sizeof rm, "%s/fc2", aw_root()); (void)ray_test_rm_rf(rm);
    }
    PASS();
}

/* ---- Task 3: fused filters on encoded leaves ---- */

/* Fused selection over <name>0 and <name>2: equal rowsels; the generation-2
 * run materializes nothing and decodes exactly `want_decoded` bytes
 * (-1: do not check). */
static test_result_t aw_sel_same(const char* name, ray_op_t* (*build)(ray_graph_t*), int64_t want_decoded) {
    ray_t* blk[2] = { NULL, NULL }; bool all[2] = { false, false };
    for (int gen = 0; gen <= 2; gen += 2) {
        ray_t* t = aw_load(name, gen);
        TEST_ASSERT_NOT_NULL(t);
        ray_graph_t* g = ray_graph_new(t);
        ray_op_t* pred = build(g);
        ray_enc_counters_reset();
        blk[gen / 2] = exec_pred_to_selection(g, pred, ray_table_nrows(t), &all[gen / 2]);
        TEST_ASSERT_FMT(!(blk[gen / 2] && RAY_IS_ERR(blk[gen / 2])), "%s gen %d: %s", name, gen, ray_error_msg());
        TEST_ASSERT_FMT(blk[gen / 2] || all[gen / 2], "%s gen %d: the fused path declined", name, gen);
        if (gen == 2) {
            ray_enc_counters_t k; ray_enc_counters_get(&k);
            TEST_ASSERT_EQ_U(k.materialized, 0);
            if (want_decoded >= 0)
                TEST_ASSERT_FMT(k.decoded_bytes == (uint64_t)want_decoded, "%s: decoded %llu, want %lld",
                                name, (unsigned long long)k.decoded_bytes, (long long)want_decoded);
        }
        ray_graph_free(g); ray_release(t);
    }
    TEST_ASSERT_EQ_I(all[0], all[1]);
    if (blk[0]) {
        ray_rowsel_t* a = ray_rowsel_meta(blk[0]); ray_rowsel_t* b = ray_rowsel_meta(blk[1]);
        TEST_ASSERT_EQ_I(a->total_pass, b->total_pass);
        TEST_ASSERT_TRUE(memcmp(ray_rowsel_flags(blk[0]), ray_rowsel_flags(blk[1]), a->n_segs) == 0);
        uint32_t nidx = ray_rowsel_offsets(blk[0])[a->n_segs];
        TEST_ASSERT_TRUE(memcmp(ray_rowsel_idx(blk[0]), ray_rowsel_idx(blk[1]), (size_t)nidx * 2) == 0);
        ray_rowsel_release(blk[0]); ray_rowsel_release(blk[1]);
    }
    PASS();
}
static ray_op_t* ap_x_eq7(ray_graph_t* g)  { return ray_eq(g, ray_scan(g, "x"), ray_const_i64(g, 7)); }
static ray_op_t* ap_x_gt10(ray_graph_t* g) { return ray_gt(g, ray_scan(g, "x"), ray_const_i64(g, 10)); }
static ray_op_t* ap_x_lt1000(ray_graph_t* g) { return ray_lt(g, ray_scan(g, "x"), ray_const_i64(g, 1000)); }
static ray_op_t* ap_band(ray_graph_t* g) {
    return ray_and(g, ray_ge(g, ray_scan(g, "x"), ray_const_i64(g, -5)), ray_le(g, ray_scan(g, "x"), ray_const_i64(g, 12)));
}
static ray_op_t* ap_f_gt(ray_graph_t* g) { return ray_gt(g, ray_scan(g, "f"), ray_const_f64(g, 0.0)); }

static test_result_t test_filter_encoded_leaves(void) {
    const char* tbl = "(table [x] (list (as 'I32 (map (fn [j] (if (== 0 (% j 9)) 0N (- (% j 50) 20))) (til 70000)))))";
    test_result_t r;
    static const int enc[] = { RAY_ENC_FOR_BP, RAY_ENC_RLE, RAY_ENC_DICT };
    static const char* nm[] = { "lf", "lr", "ld" };
    for (int i = 0; i < 3; i++) {
        aw_save(nm[i], tbl, enc[i]);
        TEST_ASSERT_EQ_I(aw_vec_enc(nm[i], "x", 0), enc[i]);
        if ((r = aw_sel_same(nm[i], ap_x_eq7, 0)).status != TEST_PASS) return r;   /* codes answer: no decode */
        if ((r = aw_sel_same(nm[i], ap_band, 0)).status != TEST_PASS) return r;
    }
    /* RAW: no evaluator; every vector holds 7 and other values, so none is
     * decided and every one decodes: all 70000 rows x 4 bytes */
    aw_save("lw", tbl, RAY_ENC_RAW);
    if ((r = aw_sel_same("lw", ap_x_eq7, 70000 * 4)).status != TEST_PASS) return r;
    PASS();
}

/* Vector-level decisions: x = row; a predicate true for rows 0..999 decides
 * every vector from its descriptor except vector 0 (rows 0..1023), which the
 * FOR_BP leaf answers without a decode.  The descriptor half passes at HEAD
 * (17b); vector 0's leaf is this task's. */
static test_result_t test_filter_vector_decisions(void) {
    aw_save("vd1", "(table [x] (list (til 70000)))", RAY_ENC_FOR_BP);
    return aw_sel_same("vd1", ap_x_lt1000, 0);
}

/* Review focus 1: nullable CONST vectors and null masking.  x is constant
 * per vector (vector index) with every 5th row null. */
static test_result_t test_filter_nulls_masked(void) {
    aw_save("nm", "(table [x] (list (as 'I32 (map (fn [j] (if (== 0 (% j 5)) 0N (div j 1024))) (til 70000)))))", RAY_ENC_CONST);
    test_result_t r;
    if ((r = aw_sel_same("nm", ap_x_lt1000, 0)).status != TEST_PASS) return r;   /* value < c and null < c: every vector ALL */
    if ((r = aw_sel_same("nm", ap_x_gt10, 0)).status != TEST_PASS) return r;     /* passing vectors are MIX (nulls fail >) */
    PASS();
}

/* Mixed generations: x generation 2 (a leaf), y generation 0 (a plain
 * register in the same program).  The generation-0 save of the pair is the
 * reference. */
static ray_op_t* ap_mixed(ray_graph_t* g) {
    return ray_and(g, ray_eq(g, ray_scan(g, "x"), ray_const_i64(g, 7)), ray_gt(g, ray_scan(g, "y"), ray_const_i64(g, 300)));
}
static test_result_t test_filter_mixed_generations(void) {
    aw_eval_ok("(set W (table [x y] (list (as 'I32 (- (% (til 70000) 50) 20)) (% (* (til 70000) 7) 1000))))");
    aw_eval_ok("(.db.splayed.set \"" AW_TMP "/mx0/\" W {compress: false})");
    /* generation 2 for x only: save compressed, then put y back as generation 0 */
    aw_eval_ok("(.db.splayed.set \"" AW_TMP "/mx2/\" W {compress: true})");
    char dir[512];
    TEST_ASSERT_EQ_I(ray_splay_resolve_dir(ray_test_tmp(AW_TMP "/mx2"), dir, sizeof dir), RAY_OK);
    char ypath[600]; snprintf(ypath, sizeof ypath, "%s/y", dir);
    ray_t* y0 = aw_eval("(at W 'y)");
    TEST_ASSERT_TRUE(y0 && !RAY_IS_ERR(y0));
    TEST_ASSERT_EQ_I(ray_col_save_gen(y0, ypath, 0), RAY_OK);
    ray_release(y0);
    ray_t* t = aw_load("mx", 2);
    TEST_ASSERT_NOT_NULL(t);
    ray_col_meta_t mx, my;
    TEST_ASSERT_TRUE(ray_table_col_meta(t, ray_sym_intern("x", 1), &mx) && mx.enc);
    TEST_ASSERT_TRUE(ray_table_col_meta(t, ray_sym_intern("y", 1), &my) && !my.enc);
    ray_release(t);
    return aw_sel_same("mx", ap_mixed, 0);   /* x: leaves; y: generation 0, read in place */
}

/* A parted table: the plan is built per segment. */
static test_result_t test_filter_parted_leaves(void) {
    for (int gen = 0; gen <= 2; gen += 2)
        for (int p = 0; p < 2; p++) {
            char q[600];
            snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/pl%d/2024.01.0%d/t/\" "
                     "(table [x] (list (as 'I32 (- (%% (til 70000) 50) 20)))) {compress: %s})",
                     gen, p + 1, gen ? "true" : "false");
            aw_eval_ok(q);
        }
    ray_t* a = aw_eval("(count (select {from: (.db.parted.get \"" AW_TMP "/pl0\" 't) where: (== x 7)}))");
    ray_enc_counters_reset();
    ray_t* b = aw_eval("(count (select {from: (.db.parted.get \"" AW_TMP "/pl2\" 't) where: (== x 7)}))");
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_TRUE(a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b));
    TEST_ASSERT_EQ_I(a->i64, b->i64);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 0);        /* leaves on the encoded segments */
    ray_release(a); ray_release(b);
    PASS();
}

/* Floats: f is -0.0 in vectors 0-1, +0.0 in vectors 2-3, null in vector 4
 * (rows 4096..5119), then j.  Vectors 0-3 are CONST and decided by their
 * descriptors (-0.0 == +0.0: max <= 0.0 fails > 0.0); vector 4 is an
 * all-null CONST vector without statistics, which only the float leaf
 * answers (ray_enc_const_decision_f64: every row null_result, 0 for >,
 * 1 for <); the RAW vectors of j are decided by their descriptors and
 * chunk 1 by its directory.  Nothing decodes. */
static ray_op_t* ap_f_lt(ray_graph_t* g) { return ray_lt(g, ray_scan(g, "f"), ray_const_f64(g, 1e12)); }
static test_result_t test_filter_floats(void) {
    aw_save("fl", "(table [f] (list (as 'F64 (map (fn [j] (if (< j 2048) -0.0 (if (< j 4096) 0.0 (if (< j 5120) 0Nf (as 'F64 j))))) "
                  "(til 70000)))))", -1);
    TEST_ASSERT_EQ_I(aw_vec_enc("fl", "f", 1), RAY_ENC_CONST);
    TEST_ASSERT_EQ_I(aw_vec_enc("fl", "f", 4), RAY_ENC_CONST);
    test_result_t r;
    if ((r = aw_sel_same("fl", ap_f_gt, 0)).status != TEST_PASS) return r;
    return aw_sel_same("fl", ap_f_lt, 0);
}

/* D8: the fused top-k predicate.  x nullable (the leaf fixtures): the sort
 * key x is materialized once, so the predicate reads it in place and adds
 * no decode.  x without nulls beside a sort key k: the predicate leaf is
 * answered on x's views, so x decodes nothing at all. */
static test_result_t test_fused_pred_leaves(void) {
    const char* tbl = "(table [x] (list (as 'I32 (map (fn [j] (if (== 0 (% j 9)) 0N (- (% j 50) 20))) (til 70000)))))";
    const char* tbk = "(table [x k] (list (as 'I32 (- (% (til 70000) 50) 20)) (% (* (til 70000) 7) 200)))";
    static const int enc[] = { RAY_ENC_FOR_BP, RAY_ENC_RLE, RAY_ENC_DICT };
    static const char* nm[] = { "lf", "lr", "ld" };
    static const char* nk[] = { "kf", "kr", "kd" };
    for (int i = 0; i < 3; i++) {
        aw_save(nm[i], tbl, enc[i]);
        uint64_t ran, dec;
        ray_fused_topk_counters_reset();
        test_result_t r = aw_same(nm[i], "(select {x: x from: T where: (== x 7) desc: x take: 3})", 0);
        if (r.status != TEST_PASS) return r;
        ray_fused_topk_counters(&ran, &dec);
        TEST_ASSERT_EQ_U(ran, 2);                                /* both generations take the fused top-k */
        /* the sort key is read by range (fpk_rk_t), never materialized: a
         * vector with a passing row is decoded by the predicate and by the
         * key's cursor, at most twice the column */
        TEST_ASSERT_TRUE(aw_col_decoded("x") <= 2 * 70000 * 4);

        aw_save(nk[i], tbk, enc[i]);
        TEST_ASSERT_EQ_I(aw_vec_enc(nk[i], "x", 0), enc[i]);
        ray_fused_topk_counters_reset();
        r = aw_same(nk[i], "(select {k: k from: T where: (== x 7) desc: k take: 3})", 0);
        if (r.status != TEST_PASS) return r;
        ray_fused_topk_counters(&ran, &dec);
        TEST_ASSERT_EQ_U(ran, 2);
        TEST_ASSERT_EQ_U(aw_col_decoded("x"), 0);                /* the leaf reads x's views */
        ray_fused_topk_counters_reset();
        r = aw_same(nk[i], "(select {k: k from: T where: (> x 25) take: 5})", 0);   /* the fused take (k: gathered by range, Task 6) */
        if (r.status != TEST_PASS) return r;
        ray_fused_topk_counters(&ran, &dec);
        TEST_ASSERT_EQ_U(ran, 2);
        TEST_ASSERT_EQ_U(aw_col_decoded("x"), 0);
        /* from the end: windows end at the table's last row, so they lie
         * inside a vector (the leaf's whole-vector answer is cut) */
        ray_fused_topk_counters_reset();
        r = aw_same(nk[i], "(select {k: k from: T where: (> x 25) take: -1500})", 0);
        if (r.status != TEST_PASS) return r;
        ray_fused_topk_counters(&ran, &dec);
        TEST_ASSERT_EQ_U(ran, 2);
        TEST_ASSERT_EQ_U(aw_col_decoded("x"), 0);
    }
    PASS();
}

/* Every operator, both operand orders, constants below, inside and above
 * the I16 range and the values, on a nullable I16 column under each
 * encoding with a view (FOR_BP, RLE, DICT, CONST per vector): the leaf
 * answers equal generation 0's and nothing decodes. */
static uint16_t g_lop; static int64_t g_lc; static bool g_lsw;
static ray_op_t* ap_leaf_op(ray_graph_t* g) {
    ray_op_t* x = ray_scan(g, "x"); ray_op_t* c = ray_const_i64(g, g_lc);
    ray_op_t* a = g_lsw ? c : x; ray_op_t* b = g_lsw ? x : c;
    switch (g_lop) {
    case OP_EQ: return ray_eq(g, a, b); case OP_NE: return ray_ne(g, a, b);
    case OP_LT: return ray_lt(g, a, b); case OP_LE: return ray_le(g, a, b);
    case OP_GT: return ray_gt(g, a, b); default:    return ray_ge(g, a, b);
    }
}
static test_result_t test_filter_leaf_ops(void) {
    const char* tbl = "(table [x] (list (as 'I16 (map (fn [j] (if (== 0 (% j 7)) 0N (- (% j 50) 20))) (til 9000)))))";
    const char* tbc = "(table [x] (list (as 'I16 (map (fn [j] (if (== 0 (% j 7)) 0N (- (div j 1024) 4))) (til 9000)))))";
    static const int enc[] = { RAY_ENC_FOR_BP, RAY_ENC_RLE, RAY_ENC_DICT, RAY_ENC_CONST };
    static const int64_t cs[] = { -100000, -32768, -21, -20, 3, 29, 30, 32767, 100000 };
    static const uint16_t ops[] = { OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE };
    for (int e = 0; e < 4; e++) {
        aw_save("lo", e == 3 ? tbc : tbl, enc[e]);
        TEST_ASSERT_EQ_I(aw_vec_enc("lo", "x", 1), enc[e]);
        for (int o = 0; o < 6; o++)
            for (size_t ci = 0; ci < sizeof cs / sizeof cs[0]; ci++)
                for (int sw = 0; sw < 2; sw++) {
                    g_lop = ops[o]; g_lc = cs[ci]; g_lsw = sw;
                    test_result_t r = aw_sel_same("lo", ap_leaf_op, 0);
                    if (r.status != TEST_PASS) {
                        fprintf(stderr, "enc %d op %u c %lld swapped %d\n", enc[e], ops[o], (long long)cs[ci], sw);
                        return r;
                    }
                }
        char rm[256];
        snprintf(rm, sizeof rm, "%s/lo0", aw_root()); (void)ray_test_rm_rf(rm);
        snprintf(rm, sizeof rm, "%s/lo2", aw_root()); (void)ray_test_rm_rf(rm);
    }
    PASS();
}

/* ---- Task 4: SYM equality and membership on ids ---- */

static ray_op_t* ap_s_eq(ray_graph_t* g) {
    ray_t* lit = ray_str("w7", 2);
    ray_op_t* o = ray_eq(g, ray_scan(g, "s"), ray_const_atom(g, lit)); ray_release(lit); return o;
}
static ray_op_t* ap_s_ne_empty(ray_graph_t* g) {
    ray_t* lit = ray_str("", 0);
    ray_op_t* o = ray_ne(g, ray_scan(g, "s"), ray_const_atom(g, lit)); ray_release(lit); return o;
}
static ray_op_t* ap_s_eq_absent(ray_graph_t* g) {
    ray_t* lit = ray_str("not-a-symbol", 12);
    ray_op_t* o = ray_eq(g, ray_scan(g, "s"), ray_const_atom(g, lit)); ray_release(lit); return o;
}
static ray_op_t* ap_s_ne_absent(ray_graph_t* g) {
    ray_t* lit = ray_str("not-a-symbol", 12);
    ray_op_t* o = ray_ne(g, ray_scan(g, "s"), ray_const_atom(g, lit)); ray_release(lit); return o;
}
static ray_op_t* ap_s_eq_empty(ray_graph_t* g) {
    ray_t* lit = ray_str("", 0);
    ray_op_t* o = ray_eq(g, ray_scan(g, "s"), ray_const_atom(g, lit)); ray_release(lit); return o;
}
static ray_op_t* ap_s_lt(ray_graph_t* g) {
    ray_t* lit = ray_str("w5", 2);
    ray_op_t* o = ray_lt(g, ray_scan(g, "s"), ray_const_atom(g, lit)); ray_release(lit); return o;
}
/* the constant on the left, as a SYM atom (a runtime id, translated too) */
static ray_op_t* ap_s_sym_left(ray_graph_t* g) {
    ray_t* lit = ray_sym(ray_sym_intern("w2", 2));
    ray_op_t* o = ray_ne(g, ray_const_atom(g, lit), ray_scan(g, "s")); ray_release(lit); return o;
}
/* one constant node feeding two compares */
static ray_op_t* ap_s_shared(ray_graph_t* g) {
    ray_t* lit = ray_str("w5", 2);
    ray_op_t* c = ray_const_atom(g, lit); ray_release(lit);
    return ray_or(g, ray_eq(g, ray_scan(g, "s"), c), ray_and(g, ray_ne(g, ray_scan(g, "s"), c),
                                                             ray_ne(g, ray_scan(g, "s"), ray_const_str(g, "", 0))));
}
static ray_t* aw_sym_set(const char* const* xs, int n) {
    ray_t* set = ray_vec_new(RAY_SYM, n); set->len = n;
    for (int i = 0; i < n; i++) ((int64_t*)ray_data(set))[i] = ray_sym_intern(xs[i], strlen(xs[i]));
    return set;
}
static const char* const g_sin_w7[] = { "w7", "w1", "zz-absent" };
static const char* const g_sin_empty[] = { "w7", "" };
static const char* const g_sin_far[] = { "w13", "w15" };
static ray_op_t* ap_s_in(ray_graph_t* g) {
    ray_t* set = aw_sym_set(g_sin_w7, 3);
    ray_op_t* o = ray_in(g, ray_scan(g, "s"), ray_const_vec(g, set)); ray_release(set); return o;
}
static ray_op_t* ap_s_notin(ray_graph_t* g) {
    ray_t* set = aw_sym_set(g_sin_w7, 3);
    ray_op_t* o = ray_not_in(g, ray_scan(g, "s"), ray_const_vec(g, set)); ray_release(set); return o;
}
static ray_op_t* ap_s_in_empty(ray_graph_t* g) {
    ray_t* set = aw_sym_set(g_sin_empty, 2);
    ray_op_t* o = ray_in(g, ray_scan(g, "s"), ray_const_vec(g, set)); ray_release(set); return o;
}
static ray_op_t* ap_s_notin_empty(ray_graph_t* g) {
    ray_t* set = aw_sym_set(g_sin_empty, 2);
    ray_op_t* o = ray_not_in(g, ray_scan(g, "s"), ray_const_vec(g, set)); ray_release(set); return o;
}
/* every member absent from the file domain: an empty probe, so no verdict
 * LUT is built and the nullable column takes exec_in's null-checked loop
 * (id-0 rows pass neither `in` nor `not in`) */
static const char* const g_sin_none[] = { "zz-absent", "zz-absent2" };
static ray_op_t* ap_s_notin_none(ray_graph_t* g) {
    ray_t* set = aw_sym_set(g_sin_none, 2);
    ray_op_t* o = ray_not_in(g, ray_scan(g, "s"), ray_const_vec(g, set)); ray_release(set); return o;
}
static ray_op_t* ap_s_notin_far(ray_graph_t* g) {
    ray_t* set = aw_sym_set(g_sin_far, 2);
    ray_op_t* o = ray_not_in(g, ray_scan(g, "s"), ray_const_vec(g, set)); ray_release(set); return o;
}

static const int64_t* g_iset; static int g_ineg;
static ray_op_t* ap_x_in(ray_graph_t* g) {
    ray_t* set = ray_vec_new(RAY_I32, 3); set->len = 3;
    for (int i = 0; i < 3; i++) ((int32_t*)ray_data(set))[i] = (int32_t)g_iset[i];
    ray_op_t* c = ray_const_vec(g, set); ray_release(set);
    return g_ineg ? ray_not_in(g, ray_scan(g, "x"), c) : ray_in(g, ray_scan(g, "x"), c);
}

/* The symbol index of row j of the "sy" fixture: -1 for "" (id 0). */
static int aw_sy_val(int64_t j) {
    if (j >= 65536 && j % 3 == 0) return -1;
    return (int)(4 * (j / 65536) + j % 4);
}
/* Fused selection of <name><gen>: its pass count, the decoded bytes and
 * materializations in *k; -1 when the fused path declines. */
static int64_t aw_sel_count(const char* name, int gen, ray_op_t* (*build)(ray_graph_t*), ray_enc_counters_t* k) {
    ray_t* t = aw_load(name, gen);
    if (!t) return -2;
    ray_graph_t* g = ray_graph_new(t);
    ray_enc_counters_reset();
    bool all = false;
    ray_t* s = exec_pred_to_selection(g, build(g), ray_table_nrows(t), &all);
    if (k) ray_enc_counters_get(k);
    int64_t n = s && !RAY_IS_ERR(s) ? ray_rowsel_meta(s)->total_pass : all ? ray_table_nrows(t) : -1;
    if (s && RAY_IS_ERR(s)) { ray_error_free(s); n = -3; }
    else if (s) ray_rowsel_release(s);
    ray_graph_free(g); ray_release(t);
    return n;
}
/* The count of rayfall `q` (table as T) on <name>0 through the unfused
 * executor: the reference every fused answer is held to. */
static int64_t aw_unfused_count(const char* name, const char* q) {
    char buf[512];
    snprintf(buf, sizeof buf, "(set T (.db.splayed.get \"" AW_TMP "/%s0/\"))", name);
    aw_eval_ok(buf);
    ray_expr_disable = true;
    ray_t* r = ray_eval_str(ray_test_tmp(q));
    ray_expr_disable = false;
    int64_t n = r && !RAY_IS_ERR(r) ? r->i64 : -1;
    if (r && RAY_IS_ERR(r)) ray_error_free(r); else if (r) ray_release(r);
    return n;
}

static test_result_t test_sym_membership(void) {
    /* s clustered: each chunk holds 4 distinct symbols, empty strings (null) in chunks 1-3 */
    aw_save("sy", "(table [s] (list (as 'SYMBOL (map (fn [j] (if (and (>= j 65536) (== 0 (% j 3))) \"\" "
                  "(format \"w%\" (+ (* 4 (div j 65536)) (% j 4))))) (til 200000)))))", -1);
    test_result_t r;
    if ((r = aw_sel_same("sy", ap_s_eq, -1)).status != TEST_PASS) return r;
    if ((r = aw_sel_same("sy", ap_s_ne_empty, -1)).status != TEST_PASS) return r;
    if ((r = aw_sel_same("sy", ap_s_eq_absent, 0)).status != TEST_PASS) return r;
    if ((r = aw_sel_same("sy", ap_s_ne_absent, 0)).status != TEST_PASS) return r;
    /* "" (id 0): chunk 0 has no "" rows (== "" all-fail there, from the
     * null count), chunks 1-3 have some (decided per vector by n_nulls or
     * evaluated on ids) */
    if ((r = aw_sel_same("sy", ap_s_eq_empty, -1)).status != TEST_PASS) return r;
    if ((r = aw_sel_same("sy", ap_s_sym_left, -1)).status != TEST_PASS) return r;
    if ((r = aw_sel_same("sy", ap_s_shared, -1)).status != TEST_PASS) return r;
    /* the answers by construction (raw-id semantics: "" == "" holds and
     * "" != c holds for every other c) */
    int64_t n7 = 0, ne = 0, n2 = 0;
    for (int64_t j = 0; j < 200000; j++) {
        int v = aw_sy_val(j);
        n7 += v == 7; ne += v < 0; n2 += v == 2;
    }
    /* the short last vector (rows 199680..199999, chunk 3, with "" rows)
     * is RAW: no view, so a predicate its statistics leave open decodes
     * exactly its 320 W8 ids; every other vector is answered on ids */
    TEST_ASSERT_EQ_I(aw_vec_enc("sy", "s", 195), RAY_ENC_RAW);
    ray_enc_counters_t k;
    for (int gen = 0; gen <= 2; gen += 2) {
        TEST_ASSERT_EQ_I(aw_sel_count("sy", gen, ap_s_eq, &k), n7);
        if (gen) { TEST_ASSERT_EQ_U(k.decoded_bytes, 0); TEST_ASSERT_EQ_U(k.materialized, 0); }
        TEST_ASSERT_EQ_I(aw_sel_count("sy", gen, ap_s_ne_empty, &k), 200000 - ne);
        if (gen) { TEST_ASSERT_EQ_U(k.decoded_bytes, 320); TEST_ASSERT_EQ_U(k.materialized, 0); }
        TEST_ASSERT_EQ_I(aw_sel_count("sy", gen, ap_s_eq_empty, &k), ne);
        if (gen) TEST_ASSERT_EQ_U(k.decoded_bytes, 320);
        TEST_ASSERT_EQ_I(aw_sel_count("sy", gen, ap_s_eq_absent, NULL), 0);
        TEST_ASSERT_EQ_I(aw_sel_count("sy", gen, ap_s_ne_absent, NULL), 200000);
        TEST_ASSERT_EQ_I(aw_sel_count("sy", gen, ap_s_sym_left, NULL), 200000 - n2);
        TEST_ASSERT_EQ_I(aw_sel_count("sy", gen, ap_s_shared, NULL), 200000 - ne);
    }
    /* the same answers through the unfused executor (generation 0) and
     * through rayfall on both generations, STR and SYM literals */
    static const char* const qs[] = {
        "(count (select {from: T where: (== s \"w7\")}))",
        "(count (select {from: T where: (== s 'w7)}))",
        "(count (select {from: T where: (!= s \"\")}))",
        "(count (select {from: T where: (== s \"\")}))",
        "(count (select {from: T where: (== s \"not-a-symbol\")}))",
        "(count (select {from: T where: (!= s \"not-a-symbol\")}))",
        "(count (select {from: T where: (!= s 'w2)}))",
        "(count (select {from: T where: (or (== s 'w5) (== s 'w9))}))",
    };
    for (size_t i = 0; i < sizeof qs / sizeof qs[0]; i++) {
        int64_t want = aw_unfused_count("sy", qs[i]);
        TEST_ASSERT_FMT(want >= 0, "%s: unfused", qs[i]);
        AW_SAME("sy", qs[i], 0);
        ray_t* got = aw_eval(qs[i]);                          /* T: generation 2 (aw_same) */
        TEST_ASSERT_FMT(got && !RAY_IS_ERR(got) && got->i64 == want, "%s: %lld, unfused %lld", qs[i],
                        got && !RAY_IS_ERR(got) ? (long long)got->i64 : -1LL, (long long)want);
        ray_release(got);
    }
    /* ordered compares on a file-domain SYM column keep
     * EXPR_BAIL_SYM_DOMAIN (the fused program never orders ids).  This pins
     * only the bail and generation 0 == generation 2 == the unfused
     * executor, which orders file-domain SYM by string (#738). */
    {
        ray_t* t = aw_load("sy", 0);
        ray_graph_t* g = ray_graph_new(t);
        ray_expr_t ex; ray_t* cerr = NULL;
        uint64_t before = ray_expr_bail_counts[EXPR_BAIL_SYM_DOMAIN];
        TEST_ASSERT_FALSE(expr_compile(g, t, ap_s_lt(g), &ex, &cerr));
        TEST_ASSERT_NULL(cerr);
        TEST_ASSERT_EQ_U(ray_expr_bail_counts[EXPR_BAIL_SYM_DOMAIN], before + 1);
        ray_graph_free(g); ray_release(t);
    }
    int64_t wlt = aw_unfused_count("sy", "(count (select {from: T where: (< s 'w5)}))");
    AW_SAME("sy", "(count (select {from: T where: (< s 'w5)}))", 1);
    {
        ray_t* got = aw_eval("(count (select {from: T where: (< s 'w5)}))");
        TEST_ASSERT_TRUE(got && !RAY_IS_ERR(got));
        TEST_ASSERT_EQ_I(got->i64, wlt);
        ray_release(got);
    }
    /* w7 lives only in chunk 1: chunks 0, 2 and 3 are decided from their id
     * bounds without a decode, chunk 1's vectors are answered on ids */
    ray_t* t = aw_load("sy", 2);
    ray_graph_t* g = ray_graph_new(t);
    ray_enc_counters_reset();
    bool all = false;
    ray_t* s = exec_pred_to_selection(g, ap_s_eq(g), ray_table_nrows(t), &all);
    TEST_ASSERT_TRUE(s && !RAY_IS_ERR(s));
    ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 0);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    ray_rowsel_release(s); ray_graph_free(g); ray_release(t);
    PASS();
}

/* The null SYM literal (', the SYM null "", id 0) against a generation-2
 * column with no index is decided on ids, as "" is: no materialization,
 * equality and inequality as on generation 0, splayed and parted.
 * Generation 0 keeps its fallback for it (the compile bails as before). */
static ray_op_t* ap_s_eq_null(ray_graph_t* g) {
    ray_t* lit = ray_sym(0);
    ray_op_t* o = ray_eq(g, ray_scan(g, "s"), ray_const_atom(g, lit)); ray_release(lit); return o;
}
static ray_op_t* ap_s_ne_null(ray_graph_t* g) {
    ray_t* lit = ray_sym(0);
    ray_op_t* o = ray_ne(g, ray_scan(g, "s"), ray_const_atom(g, lit)); ray_release(lit); return o;
}
static test_result_t test_sym_null_literal(void) {
    aw_save("sn", "(table [s v] (list (as 'SYMBOL (map (fn [j] (if (and (>= j 65536) (== 0 (% j 3))) \"\" "
                  "(format \"w%\" (+ (* 4 (div j 65536)) (% j 4))))) (til 200000))) (til 200000)))", -1);
    int64_t ne = 0, vs = 0;
    for (int64_t j = 0; j < 200000; j++) if (aw_sy_val(j) < 0) { ne++; vs += j; }
    static const char* const qs[] = {
        "(count (select {from: T where: (== s ')}))",
        "(count (select {from: T where: (!= s ')}))",
        "(sum (at (select {from: T where: (== s ')}) 'v))",
        "(count (select {from: T where: (and (== s ') (> v 100000))}))",
        "(count (select {from: T where: (or (!= s ') (== v 3))}))",
        NULL,
    };
    for (int i = 0; qs[i]; i++) {
        int64_t want = aw_unfused_count("sn", qs[i]);
        TEST_ASSERT_FMT(want >= 0, "%s: unfused", qs[i]);
        AW_SAME("sn", qs[i], 0);
        ray_t* got = aw_eval(qs[i]);                          /* T: generation 2 (aw_same) */
        TEST_ASSERT_FMT(got && !RAY_IS_ERR(got) && got->i64 == want, "%s: %lld, unfused %lld", qs[i],
                        got && !RAY_IS_ERR(got) ? (long long)got->i64 : -1LL, (long long)want);
        ray_release(got);
    }
    TEST_ASSERT_EQ_I(aw_unfused_count("sn", qs[0]), ne);       /* null == null holds */
    TEST_ASSERT_EQ_I(aw_unfused_count("sn", qs[1]), 200000 - ne);
    TEST_ASSERT_EQ_I(aw_unfused_count("sn", qs[2]), vs);
    /* the fused selection on generation 2: on ids, nothing materialized */
    ray_enc_counters_t k;
    TEST_ASSERT_EQ_I(aw_sel_count("sn", 2, ap_s_eq_null, &k), ne);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    TEST_ASSERT_EQ_I(aw_sel_count("sn", 2, ap_s_ne_null, &k), 200000 - ne);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    /* generation 0 is unchanged: the compile still declines this constant */
    {
        ray_t* t = aw_load("sn", 0);
        TEST_ASSERT_NOT_NULL(t);
        ray_graph_t* g = ray_graph_new(t);
        ray_expr_t ex; ray_t* cerr = NULL;
        uint64_t before = ray_expr_bail_counts[EXPR_BAIL_SYM_DOMAIN];
        TEST_ASSERT_FALSE(expr_compile(g, t, ap_s_eq_null(g), &ex, &cerr));
        TEST_ASSERT_NULL(cerr);
        TEST_ASSERT_EQ_U(ray_expr_bail_counts[EXPR_BAIL_SYM_DOMAIN], before + 1);
        ray_graph_free(g); ray_release(t);
    }
    /* parted: two partitions over one symfile, "" rows in the second */
    for (int gen = 0; gen <= 2; gen += 2)
        for (int p = 0; p < 2; p++) {
            char q[900];
            snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/pn%d/2024.01.0%d/t/\" "
                     "(table [s v] (list (as 'SYMBOL (map (fn [j] (if (and (== %d 1) (== 0 (%% j 5))) \"\" (format \"w%%\" (%% j 9)))) (til 70000))) "
                     "(til 70000))) \"" AW_TMP "/pn%d/.sym\" {compress: %s})", gen, p + 1, p, gen, gen ? "true" : "false");
            aw_eval_ok(q);
        }
    static const char* const pq[] = {
        "(count (select {from: (.db.parted.get \"" AW_TMP "/pn%d\" 't) where: (== s ')}))",
        "(count (select {from: (.db.parted.get \"" AW_TMP "/pn%d\" 't) where: (!= s ')}))",
        "(sum (at (select {from: (.db.parted.get \"" AW_TMP "/pn%d\" 't) where: (== s ')}) 'v))",
        NULL,
    };
    for (int i = 0; pq[i]; i++) {
        char q0[400], q2[400];
        snprintf(q0, sizeof q0, pq[i], 0); snprintf(q2, sizeof q2, pq[i], 2);
        ray_t* a = aw_eval(q0);
        ray_enc_counters_reset();
        ray_t* b = aw_eval(q2);
        ray_enc_counters_get(&k);
        TEST_ASSERT_FMT(a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b) && a->i64 == b->i64, "%s: %lld vs %lld", pq[i],
                        a && !RAY_IS_ERR(a) ? (long long)a->i64 : -1LL, b && !RAY_IS_ERR(b) ? (long long)b->i64 : -1LL);
        TEST_ASSERT_FMT(k.materialized == 0, "%s: %llu materializations", pq[i], (unsigned long long)k.materialized);
        if (i == 0) TEST_ASSERT_EQ_I(a->i64, 14000);
        ray_release(a); ray_release(b);
    }
    PASS();
}

/* `in` / `not in` by membership bounds: a set none of whose ids lies in a
 * chunk's or vector's [min, max] decides it without a read (`not in` only
 * without "" rows); the rest is answered on the views' ids.  Generation 0
 * (the unchanged in-place kernel) is the reference. */
static test_result_t test_sym_in_bounds(void) {
    aw_save("sb", "(table [s] (list (as 'SYMBOL (map (fn [j] (if (and (>= j 65536) (== 0 (% j 3))) \"\" "
                  "(format \"w%\" (+ (* 4 (div j 65536)) (% j 4))))) (til 200000)))))", -1);
    test_result_t r;
    /* decoded bytes: only the RAW tail vector (320 W8 ids, with "" rows)
     * when its chunk's bounds leave it open.  `in` decides chunk 3 from its
     * ids (a "" member of a SYM set is a null element, dropped from the
     * probe as exec_in does, so it matches no row); the `not in` shapes
     * cannot (chunk 3 holds "" rows) */
    ray_op_t* (*const shapes[])(ray_graph_t*) = { ap_s_in, ap_s_notin, ap_s_in_empty, ap_s_notin_empty, ap_s_notin_far };
    static const int64_t dec[] = { 0, 320, 0, 320, 320 };
    for (size_t i = 0; i < sizeof shapes / sizeof shapes[0]; i++)
        if ((r = aw_sel_same("sb", shapes[i], dec[i])).status != TEST_PASS) { fprintf(stderr, "shape %zu\n", i); return r; }
    int64_t nin = 0;
    for (int64_t j = 0; j < 200000; j++) { int v = aw_sy_val(j); nin += v == 7 || v == 1; }
    TEST_ASSERT_EQ_I(aw_sel_count("sb", 2, ap_s_in, NULL), nin);
    /* without a LUT the null-checked loop answers id-0 rows 0 under
     * `not in`, as the LUT does (#739); the views clear their id-0 rows
     * the same way, so only the RAW tail vector is decoded; chunk 0,
     * without "", is decided from its statistics */
    if ((r = aw_sel_same("sb", ap_s_notin_none, 320)).status != TEST_PASS) return r;
    {
        int64_t want = 0;
        for (int64_t j = 0; j < 200000; j++) want += aw_sy_val(j) >= 0;
        TEST_ASSERT_EQ_I(aw_sel_count("sb", 2, ap_s_notin_none, NULL), want);
    }
    /* integers: a nullable I32 column, sets inside and outside the bounds */
    aw_save("ib", "(table [x] (list (as 'I32 (map (fn [j] (if (and (>= j 65536) (== 0 (% j 7))) 0N (+ (div j 1000) (% j 3)))) (til 140000)))))", -1);
    static const int64_t sets[][3] = { { 5, 70, 9000 }, { -4, 1000, 2000 }, { 66, 67, 68 } };
    for (int si = 0; si < 3; si++)
        for (int neg = 0; neg < 2; neg++) {
            g_iset = sets[si]; g_ineg = neg;
            if ((r = aw_sel_same("ib", ap_x_in, 0)).status != TEST_PASS) {
                fprintf(stderr, "set %d negate %d\n", si, neg);
                return r;
            }
        }
    PASS();
}

/* Float membership bounds: the probe as doubles (NaN members match
 * nothing, -0.0 equals +0.0), no view evaluator (a float vector is read).
 * f: -0.0 in vector 0, null every 13th row in chunk 1, else the row; g the
 * same as F32; x an I32 column against a float set (the widened integer
 * bounds).  Generation 0 (the unchanged kernel) is the reference. */
static const double* g_fset; static int g_fnset; static int g_fneg; static const char* g_fcol;
static ray_op_t* ap_f_in(ray_graph_t* g) {
    ray_t* set = ray_vec_new(RAY_F64, g_fnset); set->len = g_fnset;
    for (int i = 0; i < g_fnset; i++) ((double*)ray_data(set))[i] = g_fset[i];
    ray_op_t* c = ray_const_vec(g, set); ray_release(set);
    return g_fneg ? ray_not_in(g, ray_scan(g, g_fcol), c) : ray_in(g, ray_scan(g, g_fcol), c);
}
static test_result_t test_float_in_bounds(void) {
    aw_save("fb", "(select {f: f g: (as 'F32 f) x: x from: (table [f x] (list "
                  "(as 'F64 (map (fn [j] (if (< j 1024) -0.0 (if (and (>= j 65536) (< j 131072) (== 0 (% j 13))) 0Nf (as 'F64 j)))) (til 140000))) "
                  "(as 'I32 (div (til 140000) 1000))))})", -1);
    static const double s_zero[] = { 0.0, 1e9, NAN };   /* +0.0 matches vector 0's -0.0 */
    static const double s_12[] = { 12.0 };              /* row 12 is -0.0: matches nothing */
    static const double s_x[] = { 5.0, 70.5, 139.0 };
    static const char* const cols[] = { "f", "g" };
    test_result_t r;
    for (int ci = 0; ci < 2; ci++) {
        g_fcol = cols[ci];
        const size_t esz = ci == 0 ? 8 : 4;
        /* `in` +0.0: only vector 0 (bounds [-0.0, -0.0] hold +0.0) is read */
        g_fset = s_zero; g_fnset = 3; g_fneg = 0;
        if ((r = aw_sel_same("fb", ap_f_in, (int64_t)(1024 * esz))).status != TEST_PASS) return r;
        TEST_ASSERT_EQ_I(aw_sel_count("fb", 2, ap_f_in, NULL), 1024);
        /* `not in` +0.0: vector 0 read; chunk 1's vectors all hold a null
         * (every 13th row), so they are read; the rest decided */
        g_fneg = 1;
        if ((r = aw_sel_same("fb", ap_f_in, (int64_t)((1024 + 64 * 1024) * esz))).status != TEST_PASS) return r;
        /* 12.0: no range holds it; `in` reads nothing, `not in` only the
         * null-bearing chunk 1 */
        g_fset = s_12; g_fnset = 1; g_fneg = 0;
        if ((r = aw_sel_same("fb", ap_f_in, 0)).status != TEST_PASS) return r;
        TEST_ASSERT_EQ_I(aw_sel_count("fb", 2, ap_f_in, NULL), 0);
        g_fneg = 1;
        if ((r = aw_sel_same("fb", ap_f_in, (int64_t)(64 * 1024 * esz))).status != TEST_PASS) return r;
    }
    /* an integer column against a float set */
    g_fcol = "x"; g_fset = s_x; g_fnset = 3;
    for (g_fneg = 0; g_fneg < 2; g_fneg++)
        if ((r = aw_sel_same("fb", ap_f_in, -1)).status != TEST_PASS) return r;
    PASS();
}

/* The same shapes route through the fused path on generation 0 now. */
static test_result_t test_sym_fused_gen0(void) {
    aw_save("sz", "(table [s] (list (as 'SYMBOL (map (fn [j] (format \"w%\" (% j 5))) (til 10000)))))", -1);
    ray_t* t = aw_load("sz", 0);
    ray_graph_t* g = ray_graph_new(t);
    ray_expr_t ex;
    ray_t* cerr = NULL;
    TEST_ASSERT_TRUE(expr_compile(g, t, ap_s_eq(g), &ex, &cerr));   /* no EXPR_BAIL_SYM_DOMAIN */
    TEST_ASSERT_NULL(cerr);
    ray_graph_free(g); ray_release(t);
    PASS();
}

/* ---- Task 5: gathers under the output contract ---- */

/* Review focus 4: non-monotone, repeated ids across vectors and two
 * partitions; pooled STR in two vectors whose scratch is reused; the result
 * outlives the sources and every scratch. */
static test_result_t test_gather_rows_contract(void) {
    for (int p = 0; p < 2; p++) {
        char q[600];
        snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/gp/2024.01.0%d/t/\" "
                 "(table [s y v] (list (map (fn [j] (format \"a long pooled string number %%\" (+ j %d))) (til 3000)) "
                 "(as 'SYMBOL (map (fn [j] (format \"y%%\" (%% j %d))) (til 3000))) (+ %d (til 3000)))) "
                 "\"" AW_TMP "/gp/.sym\" {compress: %s})", p + 1, p * 3000, p ? 400 : 30, p * 3000,
                 p ? "false" : "true");                  /* partition 0 generation 2, partition 1 generation 0 */
        aw_eval_ok(q);
    }
    ray_t* t = aw_eval("(.db.parted.get \"" AW_TMP "/gp\" 't)");
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    static const int64_t ids[] = { 5000, 3, 1030, 3, 2999, 3000, 1030, 5999, 0, 4100 };
    const int64_t n = 10;
    ray_t* res[3] = { NULL, NULL, NULL };
    static const char* cols[] = { "s", "y", "v" };
    ray_enc_counters_reset();
    for (int c = 0; c < 3; c++) {
        ray_col_src_t s;
        TEST_ASSERT_NULL(ray_table_col_src(t, ray_sym_intern(cols[c], 1), &s));
        TEST_ASSERT_NULL(ray_col_gather_rows(&s, ids, n, &res[c]));
        ray_col_src_release(&s);
    }
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    /* v's generation-2 segment: ids 0, 3 (vector 0), 1030 (vector 1), 2999
     * (vector 2: 952 rows), each vector read once despite repeats and order.
     * Its vectors are DELTA_BP, which a point read declines (Task 6b), so
     * the few ids of each still decode it (gather_points pins the served
     * encodings) */
    ray_t* venc = ray_table_get_col_enc(t, ray_sym_intern("v", 1));
    TEST_ASSERT_NOT_NULL(venc);
    ray_t* vseg0 = ray_enccol_seg(venc, 0);
    TEST_ASSERT_TRUE(vseg0 && vseg0->type == RAY_ENCCOL);
    const ray_enc_desc_t* vd = ray_enccol_descs(ray_enccol_of(vseg0), 0);
    for (int j = 0; j < 3; j++) TEST_ASSERT_EQ_I(vd[j].encoding, RAY_ENC_DELTA_BP);
    TEST_ASSERT_EQ_U(atomic_load(&ray_enccol_of(vseg0)->decoded_bytes), (1024 + 1024 + 952) * 8);
    ray_release(t);                                       /* sources gone: results must stand alone */
    for (int64_t i = 0; i < n; i++) {
        char want[64];
        int wl = snprintf(want, sizeof want, "a long pooled string number %lld", (long long)ids[i]);
        size_t len = 0; const char* got = ray_str_vec_get(res[0], i, &len);
        TEST_ASSERT_FMT(len == (size_t)wl && memcmp(got, want, len) == 0, "s[%lld]", (long long)i);
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(res[2]))[i], ids[i]);
        int64_t yid = ray_read_sym(ray_data(res[1]), i, RAY_SYM, res[1]->attrs);
        ray_t* ys = ray_sym_domain_str(ray_sym_vec_domain(res[1]), yid);
        int64_t local = ids[i] < 3000 ? ids[i] % 30 : (ids[i] - 3000) % 400;
        int yl = snprintf(want, sizeof want, "y%lld", (long long)local);
        TEST_ASSERT_TRUE(ys && ray_str_len(ys) == (size_t)yl && memcmp(ray_str_ptr(ys), want, (size_t)yl) == 0);
    }
    TEST_ASSERT_EQ_I(res[1]->attrs & RAY_SYM_W_MASK, RAY_SYM_W16);   /* widened for the 400-symbol segment */
    for (int c = 0; c < 3; c++) ray_release(res[c]);
    PASS();
}

/* Rowsel compaction over the mixed parted table: rows 3, 1030, 2999, 3000
 * and 4100; rowsel segment 2 spans the partition boundary, yet each local
 * vector is read once; results checked after the source is released. */
static test_result_t test_gather_sel_parted(void) {
    for (int p = 0; p < 2; p++) {
        char q[600];
        snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/gq/2024.01.0%d/t/\" "
                 "(table [s v] (list (map (fn [j] (format \"a long pooled string number %%\" (+ j %d))) (til 3000)) "
                 "(+ %d (til 3000)))) \"" AW_TMP "/gq/.sym\" {compress: %s})", p + 1, p * 3000, p * 3000, p ? "false" : "true");
        aw_eval_ok(q);
    }
    ray_t* t = aw_eval("(.db.parted.get \"" AW_TMP "/gq\" 't)");
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_t* pred = ray_vec_new(RAY_BOOL, 6000); pred->len = 6000;
    memset(ray_data(pred), 0, 6000);
    static const int64_t rows[] = { 3, 1030, 2999, 3000, 4100 };
    for (int i = 0; i < 5; i++) ((uint8_t*)ray_data(pred))[rows[i]] = 1;
    ray_t* sel = ray_rowsel_from_pred(pred);
    TEST_ASSERT_NOT_NULL(sel);
    ray_t* res[2] = { NULL, NULL };
    static const char* cols[] = { "s", "v" };
    for (int c = 0; c < 2; c++) {
        ray_col_src_t s;
        TEST_ASSERT_NULL(ray_table_col_src(t, ray_sym_intern(cols[c], 1), &s));
        TEST_ASSERT_NULL(ray_col_gather_sel(&s, sel, &res[c]));
        ray_col_src_release(&s);
    }
    /* DELTA_BP vectors (as in gather_rows_contract): declined point reads,
     * each touched vector decoded once */
    ray_t* venc = ray_table_get_col_enc(t, ray_sym_intern("v", 1));
    const ray_enc_desc_t* vd = ray_enccol_descs(ray_enccol_of(ray_enccol_seg(venc, 0)), 0);
    for (int j = 0; j < 3; j++) TEST_ASSERT_EQ_I(vd[j].encoding, RAY_ENC_DELTA_BP);
    TEST_ASSERT_EQ_U(atomic_load(&ray_enccol_of(ray_enccol_seg(venc, 0))->decoded_bytes), (1024 + 1024 + 952) * 8);
    ray_release(t); ray_rowsel_release(sel); ray_release(pred);
    for (int i = 0; i < 5; i++) {
        char want[64];
        int wl = snprintf(want, sizeof want, "a long pooled string number %lld", (long long)rows[i]);
        size_t len = 0; const char* got = ray_str_vec_get(res[0], i, &len);
        TEST_ASSERT_TRUE(len == (size_t)wl && memcmp(got, want, len) == 0);
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(res[1]))[i], rows[i]);
    }
    ray_release(res[0]); ray_release(res[1]);
    PASS();
}

static test_result_t test_gather_sel_skips_none(void) {
    aw_save("gs", "(table [x] (list (til 70000)))", RAY_ENC_FOR_BP);
    ray_t* t = aw_load("gs", 2);
    ray_graph_t* g = ray_graph_new(t);
    bool all = false;
    ray_t* sel = exec_pred_to_selection(g, ray_lt(g, ray_scan(g, "x"), ray_const_i64(g, 1500)), 70000, &all);
    ray_col_src_t s; TEST_ASSERT_NULL(ray_table_col_src(t, ray_sym_intern("x", 1), &s));
    ray_enc_counters_reset();
    ray_t* out = NULL;
    TEST_ASSERT_NULL(ray_col_gather_sel(&s, sel, &out));
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_I(out->len, 1500);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 2 * 1024 * 8);     /* only vectors 0 and 1 are touched */
    for (int64_t i = 0; i < 1500; i++) TEST_ASSERT_EQ_I(((int64_t*)ray_data(out))[i], i);
    ray_release(out); ray_col_src_release(&s); ray_rowsel_release(sel); ray_graph_free(g); ray_release(t);
    PASS();
}

/* The generation-0 answer, bit for bit: every type's gather from the
 * generation-2 copy equals the same gather from the generation-0 copy
 * (data bytes, HAS_NULLS, STR pool bytes, SYM width and domain), for a
 * row list and a rowsel, nulls (canonical sentinels) included. */
static test_result_t aw_vec_same(ray_t* a, ray_t* b, const char* what) {
    TEST_ASSERT_FMT(a->type == b->type && a->len == b->len && a->attrs == b->attrs,
                    "%s: type %d/%d len %lld/%lld attrs %x/%x", what, a->type, b->type,
                    (long long)a->len, (long long)b->len, a->attrs, b->attrs);
    size_t esz = ray_sym_elem_size(a->type, a->attrs);
    if (a->type != RAY_SYM)
        TEST_ASSERT_FMT(memcmp(ray_data(a), ray_data(b), (size_t)a->len * esz) == 0, "%s: data", what);
    if (a->type == RAY_STR) {
        TEST_ASSERT_FMT(!a->str_pool == !b->str_pool, "%s: pool presence", what);
        if (a->str_pool)
            TEST_ASSERT_FMT(a->str_pool->len == b->str_pool->len &&
                            memcmp(ray_data(a->str_pool), ray_data(b->str_pool), (size_t)a->str_pool->len) == 0,
                            "%s: pool", what);
    }
    for (int64_t i = 0; a->type == RAY_SYM && i < a->len; i++) {   /* each copy has its own symfile */
        ray_t* sa = ray_sym_domain_str(ray_sym_vec_domain(a), ray_read_sym(ray_data(a), i, RAY_SYM, a->attrs));
        ray_t* sb = ray_sym_domain_str(ray_sym_vec_domain(b), ray_read_sym(ray_data(b), i, RAY_SYM, b->attrs));
        TEST_ASSERT_FMT(sa && sb && ray_str_len(sa) == ray_str_len(sb) &&
                        memcmp(ray_str_ptr(sa), ray_str_ptr(sb), ray_str_len(sa)) == 0, "%s[%lld]: symbol", what, (long long)i);
    }
    PASS();
}
/* A gather result against its generation-0 reference (`at`): type, length,
 * HAS_NULLS, SYM width, values (STR by bytes; SYM by raw id and domain when
 * both resolve over one symfile, else by symbol). */
static test_result_t aw_gather_ref(ray_t* got, ray_t* ref, bool one_symfile, const char* what) {
    /* a parted SYM `at` widens to W64; the contract's width is the widest
     * segment's (parted_sym_width), pinned by gather_rows_contract */
    uint8_t wm = (ref->type == RAY_SYM && (ref->attrs & RAY_SYM_W_MASK) == RAY_SYM_W64) ? 0 : RAY_SYM_W_MASK;
    TEST_ASSERT_FMT(got->len == ref->len && got->type == ref->type &&
                    (got->attrs & (RAY_ATTR_HAS_NULLS | wm)) == (ref->attrs & (RAY_ATTR_HAS_NULLS | wm)),
                    "%s: type %d/%d len %lld/%lld attrs %x/%x", what, got->type, ref->type,
                    (long long)got->len, (long long)ref->len, got->attrs, ref->attrs);
    for (int64_t i = 0; i < ref->len; i++) {
        if (ref->type == RAY_STR) {
            size_t la = 0, lb = 0;
            const char* pa = ray_str_vec_get(got, i, &la);
            const char* pb = ray_str_vec_get(ref, i, &lb);
            TEST_ASSERT_FMT(la == lb && (la == 0 || memcmp(pa, pb, la) == 0), "%s[%lld]", what, (long long)i);
        } else if (ref->type == RAY_SYM && !one_symfile) {
            ray_t* sa = ray_sym_domain_str(ray_sym_vec_domain(got), ray_read_sym(ray_data(got), i, RAY_SYM, got->attrs));
            ray_t* sb = ray_sym_domain_str(ray_sym_vec_domain(ref), ray_read_sym(ray_data(ref), i, RAY_SYM, ref->attrs));
            TEST_ASSERT_FMT(sa && sb && ray_str_len(sa) == ray_str_len(sb) &&
                            memcmp(ray_str_ptr(sa), ray_str_ptr(sb), ray_str_len(sa)) == 0, "%s[%lld]: symbol", what, (long long)i);
        } else {
            size_t esz = ray_sym_elem_size(ref->type, ref->attrs);
            TEST_ASSERT_FMT(memcmp((char*)ray_data(got) + i * esz, (char*)ray_data(ref) + i * esz, esz) == 0,
                            "%s[%lld]", what, (long long)i);
        }
    }
    if (ref->type == RAY_SYM && one_symfile) TEST_ASSERT_TRUE(ray_sym_vec_domain(got) == ray_sym_vec_domain(ref));
    PASS();
}
#define AW_GATHER_REF(g, r, one, w) do { test_result_t r_ = aw_gather_ref((g), (r), (one), (w)); if (r_.status != TEST_PASS) return r_; } while (0)

static test_result_t test_gather_gen0_parity(void) {
    /* (as 'F32 ...) evaluates only on a select's compiled path (I16 / F32
     * casts); TIMESTAMP and GUID columns are built outside it */
    aw_save("gz", "(select {i: i f: f s: s y: y d: d h: (as 'I16 i) g: (as 'F32 f) ts: ts from: "
            "(table [i f s y d ts] (list "
            "(map (fn [j] (if (== 0 (% j 7)) 0Nl (* j 3))) (til 5000)) "
            "(map (fn [j] (if (== 0 (% j 11)) 0Nf (* j 0.5))) (til 5000)) "
            "(map (fn [j] (if (== 0 (% j 5)) \"\" (if (== 0 (% j 2)) \"short\" (format \"a pooled string of row %\" j)))) (til 5000)) "
            "(as 'SYMBOL (map (fn [j] (if (== 0 (% j 13)) \"\" (format \"k%\" (% j 300)))) (til 5000))) "
            "(as 'DATE (til 5000)) "
            "(as 'TIMESTAMP (map (fn [j] (if (== 0 (% j 9)) 0Nl (* j 1000))) (til 5000)))))})", -1);
    aw_save("gu", "(table [u] (list (take (as 'GUID (list \"00000000-0000-0000-0000-000000000001\" "
            "\"00000000-0000-0000-0000-0000000000a2\" \"\" \"10000000-0000-0000-0000-000000000003\")) 5000)))", -1);
    for (int tb = 0; tb < 2; tb++) {
    const char* tn = tb ? "gu" : "gz";
    ray_t* t0 = aw_load(tn, 0); ray_t* t2 = aw_load(tn, 2);
    TEST_ASSERT_TRUE(t0 && t2);
    static const int64_t ids[] = { 4999, 0, 7, 2048, 7, 13, 1100, 3, 4096, 2047, 35, 0 };
    ray_t* pred = ray_vec_new(RAY_BOOL, 5000); pred->len = 5000;
    for (int64_t r = 0; r < 5000; r++) ((uint8_t*)ray_data(pred))[r] = (r < 1024) || (r % 3 == 0 && r > 3000);
    ray_t* sel = ray_rowsel_from_pred(pred);
    TEST_ASSERT_NOT_NULL(sel);
    static const char* cols[2][8] = { { "i", "f", "s", "y", "d", "h", "g", "ts" }, { "u" } };
    for (int c = 0; c < (tb ? 1 : 8); c++) {
        const char* cn = cols[tb][c];
        ray_t* out[2][2];
        int64_t nm = ray_sym_intern(cn, strlen(cn));
        for (int gen = 0; gen < 2; gen++) {
            ray_col_src_t s;
            TEST_ASSERT_NULL(ray_table_col_src(gen ? t2 : t0, nm, &s));
            TEST_ASSERT_FMT(s.type == ray_table_get_col(t0, nm)->type, "%s", cn);
            TEST_ASSERT_NULL(ray_col_gather_rows(&s, ids, 12, &out[gen][0]));
            TEST_ASSERT_NULL(ray_col_gather_sel(&s, sel, &out[gen][1]));
            ray_col_src_release(&s);
        }
        /* the reference: today's generation-0 index gather, `at` */
        char q[512];
        for (int w = 0; w < 2; w++) {
            snprintf(q, sizeof q, "(at (at (.db.splayed.get \"" AW_TMP "/%s0/\") '%s) %s)", tn, cn,
                     w ? "(where (or (< (til 5000) 1024) (and (> (til 5000) 3000) (== 0 (% (til 5000) 3)))))"
                       : "[4999 0 7 2048 7 13 1100 3 4096 2047 35 0]");
            ray_t* ref = aw_eval(q);
            TEST_ASSERT_FMT(ref && !RAY_IS_ERR(ref), "%s: %s", q, ray_error_msg());
            AW_GATHER_REF(out[0][w], ref, true, cn);
            AW_GATHER_REF(out[1][w], ref, false, cn);    /* <name>2 has its own symfile */
            ray_release(ref);
            test_result_t r = aw_vec_same(out[0][w], out[1][w], cn);
            if (r.status != TEST_PASS) return r;
            ray_release(out[0][w]); ray_release(out[1][w]);
        }
    }
    ray_rowsel_release(sel); ray_release(pred);
    if (tb) { ray_release(t0); ray_release(t2); continue; }
    ray_t* e = NULL;
    ray_col_src_t s; TEST_ASSERT_NULL(ray_table_col_src(t2, ray_sym_intern("i", 1), &s));
    static const int64_t bad[] = { 1, 5000 };
    ray_t* err = ray_col_gather_rows(&s, bad, 2, &e);
    TEST_ASSERT_TRUE(err && RAY_IS_ERR(err) && e == NULL);
    TEST_ASSERT_STR_EQ(ray_err_code(err), "range");
    ray_error_free(err); ray_col_src_release(&s);
    ray_release(t0); ray_release(t2);
    }
    PASS();
}

/* Important 1 (review): the in-order row list.  Sorted ids with repeats and
 * runs crossing a vector boundary (1023..1025) and a partition boundary
 * (2999..3001), both partitions generation 2, against generation 0's `at`
 * over an all-generation-0 copy; each touched vector decoded once. */
static test_result_t test_gather_rows_sorted(void) {
    for (int gen = 0; gen < 3; gen += 2)
        for (int p = 0; p < 2; p++) {
            char q[700];
            snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/gr%d/2024.01.0%d/t/\" "
                     "(table [s v y] (list (map (fn [j] (format \"a long pooled string number %%\" (+ j %d))) (til 3000)) "
                     "(+ %d (til 3000)) (as 'SYMBOL (map (fn [j] (format \"y%%\" (%% j %d))) (til 3000))))) "
                     "\"" AW_TMP "/gr%d/.sym\" {compress: %s})", gen, p + 1, p * 3000, p * 3000, p ? 400 : 30,
                     gen, gen ? "true" : "false");
            aw_eval_ok(q);
        }
    ray_t* t = aw_eval("(.db.parted.get \"" AW_TMP "/gr2\" 't)");
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    static const int64_t ids[] = { 0, 1, 2, 2, 1023, 1024, 1025, 2999, 3000, 3001, 3001, 5999 };
    static const char* cols[] = { "s", "v", "y" };
    ray_enc_counters_reset();
    for (int c = 0; c < 3; c++) {
        ray_col_src_t s;
        TEST_ASSERT_NULL(ray_table_col_src(t, ray_sym_intern(cols[c], 1), &s));
        ray_t* got = NULL;
        TEST_ASSERT_NULL(ray_col_gather_rows(&s, ids, 12, &got));
        ray_col_src_release(&s);
        char q[300];
        snprintf(q, sizeof q, "(at (at (.db.parted.get \"" AW_TMP "/gr0\" 't) '%s) [0 1 2 2 1023 1024 1025 2999 3000 3001 3001 5999])", cols[c]);
        ray_t* ref = aw_eval(q);
        TEST_ASSERT_FMT(ref && !RAY_IS_ERR(ref), "%s: %s", q, ray_error_msg());
        AW_GATHER_REF(got, ref, false, cols[c]);
        ray_release(ref); ray_release(got);
    }
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    /* v: partition 1 vectors 0, 1, 2 (952 rows); partition 2 vectors 0, 2 */
    ray_t* venc = ray_table_get_col_enc(t, ray_sym_intern("v", 1));
    TEST_ASSERT_NOT_NULL(venc);
    TEST_ASSERT_EQ_U(atomic_load(&ray_enccol_of(ray_enccol_seg(venc, 0))->decoded_bytes), (1024 + 1024 + 952) * 8);
    TEST_ASSERT_EQ_U(atomic_load(&ray_enccol_of(ray_enccol_seg(venc, 1))->decoded_bytes), (1024 + 952) * 8);
    /* Task 6 fix round 2: sorted ids whose repeats balance a gap ({0,0,2}:
     * its endpoints span two steps as a run would), all repeats, and a
     * repeat inside a run are value lists, never one copy */
    static const struct { int64_t ids[6]; int n; const char* lit; } rs[] = {
        { { 0, 0, 2 }, 3, "[0 0 2]" },
        { { 1, 1, 1 }, 3, "[1 1 1]" },
        { { 3, 4, 4, 5, 7 }, 5, "[3 4 4 5 7]" },
        { { 3000, 3000, 3002 }, 3, "[3000 3000 3002]" },
    };
    for (size_t i = 0; i < sizeof rs / sizeof rs[0]; i++)
        for (int c = 0; c < 3; c++) {
            ray_col_src_t s;
            TEST_ASSERT_NULL(ray_table_col_src(t, ray_sym_intern(cols[c], 1), &s));
            ray_t* got = NULL;
            TEST_ASSERT_NULL(ray_col_gather_rows(&s, rs[i].ids, rs[i].n, &got));
            ray_col_src_release(&s);
            char q[300];
            snprintf(q, sizeof q, "(at (at (.db.parted.get \"" AW_TMP "/gr0\" 't) '%s) %s)", cols[c], rs[i].lit);
            ray_t* ref = aw_eval(q);
            TEST_ASSERT_FMT(ref && !RAY_IS_ERR(ref), "%s: %s", q, ray_error_msg());
            AW_GATHER_REF(got, ref, false, rs[i].lit);
            ray_release(ref); ray_release(got);
        }
    ray_release(t);
    PASS();
}

/* Minor 5 (review): empty requests and all-null columns.  n = 0 and an
 * empty selection read nothing and give empty vectors; an all-empty STR
 * column (no pool) and an all-"" SYM column (a CONST-0 vector) give
 * generation 0's nulls and HAS_NULLS.  A rowsel whose segments hold more
 * rows than its total_pass is `corrupt` before any row past it is written. */
static test_result_t test_gather_empty_and_nulls(void) {
    aw_save("ge", "(table [e y v] (list (map (fn [j] \"\") (til 3000)) (as 'SYMBOL (map (fn [j] \"\") (til 3000))) (til 3000)))", -1);
    TEST_ASSERT_EQ_I(aw_vec_enc("ge", "y", 1), RAY_ENC_CONST);
    ray_t* t0 = aw_load("ge", 0); ray_t* t2 = aw_load("ge", 2);
    TEST_ASSERT_TRUE(t0 && t2);
    ray_t* pred = ray_vec_new(RAY_BOOL, 3000); pred->len = 3000;
    memset(ray_data(pred), 0, 3000);
    ray_t* none = ray_rowsel_from_pred(pred);
    TEST_ASSERT_NOT_NULL(none);
    for (int64_t r = 0; r < 3000; r++) ((uint8_t*)ray_data(pred))[r] = r % 4 == 1;
    ray_t* sel = ray_rowsel_from_pred(pred);
    static const int64_t ids[] = { 2999, 5, 1500, 5 };
    static const char* cols[] = { "e", "y", "v" };
    for (int c = 0; c < 3; c++) {
        ray_col_src_t s;
        TEST_ASSERT_NULL(ray_table_col_src(t2, ray_sym_intern(cols[c], 1), &s));
        ray_enc_counters_reset();
        ray_t* a = NULL; ray_t* b = NULL;
        TEST_ASSERT_NULL(ray_col_gather_rows(&s, ids, 0, &a));
        TEST_ASSERT_NULL(ray_col_gather_sel(&s, none, &b));
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        TEST_ASSERT_EQ_U(k.decoded_bytes, 0);
        for (int w = 0; w < 2; w++) {
            ray_t* o = w ? b : a;
            TEST_ASSERT_FMT(o->len == 0 && o->type == s.type && !(o->attrs & RAY_ATTR_HAS_NULLS) &&
                            (o->type != RAY_STR || !o->str_pool),
                            "%s %d: len %lld type %d/%d attrs %x pool %p", cols[c], w, (long long)o->len, o->type, s.type,
                            o->attrs, (void*)o->str_pool);
            ray_release(o);
        }
        if (c < 2) {                                      /* all null */
            ray_t* g[2] = { NULL, NULL };
            TEST_ASSERT_NULL(ray_col_gather_rows(&s, ids, 4, &g[0]));
            TEST_ASSERT_NULL(ray_col_gather_sel(&s, sel, &g[1]));
            for (int w = 0; w < 2; w++) {
                char q[300];
                snprintf(q, sizeof q, "(at (at (.db.splayed.get \"" AW_TMP "/ge0/\") '%s) %s)", cols[c],
                         w ? "(where (== 1 (% (til 3000) 4)))" : "[2999 5 1500 5]");
                ray_t* ref = aw_eval(q);
                TEST_ASSERT_FMT(ref && !RAY_IS_ERR(ref), "%s: %s", q, ray_error_msg());
                AW_GATHER_REF(g[w], ref, false, cols[c]);
                TEST_ASSERT_TRUE((g[w]->attrs & RAY_ATTR_HAS_NULLS) && (g[w]->type != RAY_STR || !g[w]->str_pool));
                ray_release(ref); ray_release(g[w]);
            }
        }
        if (c == 2) {                                     /* Minor 2: a rowsel holding more rows than it says */
            int64_t tp = ray_rowsel_meta(sel)->total_pass;
            ray_rowsel_meta(sel)->total_pass = 1;          /* a one-row output, 750 rows in the segments */
            ray_t* o = NULL;
            ray_t* err = ray_col_gather_sel(&s, sel, &o);
            ray_rowsel_meta(sel)->total_pass = tp;
            TEST_ASSERT_TRUE(err && RAY_IS_ERR(err) && o == NULL);
            TEST_ASSERT_STR_EQ(ray_err_code(err), "corrupt");
            /* refused by the per-segment bound, before the write (the end
             * check alone would report it after writing past the output) */
            TEST_ASSERT_FMT(strstr(ray_error_msg(), "more rows than") != NULL, "%s", ray_error_msg());
            ray_error_free(err);
        }
        ray_col_src_release(&s);
    }
    ray_rowsel_release(none); ray_rowsel_release(sel); ray_release(pred); ray_release(t0); ray_release(t2);
    PASS();
}

/* Minor 5 (review): a ragged parted source.  Partition 2 lacks w, so the
 * load gives it a synthetic all-null segment; the gather across both
 * equals generation 0's `at` over an explicit-null copy. */
static test_result_t test_gather_ragged(void) {
    static const char* roots[] = { "gw", "gn" };
    for (int r = 0; r < 2; r++)
        for (int p = 0; p < 2; p++) {
            char q[600];
            const char* tbl = p == 0 ? "(table [x w] (list (til 3000) (* 2 (til 3000))))"
                            : r == 0 ? "(table [x] (list (til 3000)))"
                                     : "(table [x w] (list (til 3000) (take [0Nl] 3000)))";
            snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/%s/2024.01.0%d/t/\" %s {compress: %s})",
                     roots[r], p + 1, tbl, r == 0 ? "true" : "false");
            aw_eval_ok(q);
        }
    ray_t* t = aw_eval("(.db.parted.get \"" AW_TMP "/gw\" 't)");
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    static const int64_t ids[] = { 5999, 1, 3000, 2999, 4500, 1 };
    ray_t* pred = ray_vec_new(RAY_BOOL, 6000); pred->len = 6000;
    for (int64_t r = 0; r < 6000; r++) ((uint8_t*)ray_data(pred))[r] = r % 5 == 0 || (r > 2000 && r < 4100);
    ray_t* sel = ray_rowsel_from_pred(pred);
    ray_col_src_t s;
    TEST_ASSERT_NULL(ray_table_col_src(t, ray_sym_intern("w", 1), &s));
    ray_t* g[2] = { NULL, NULL };
    TEST_ASSERT_NULL(ray_col_gather_rows(&s, ids, 6, &g[0]));
    TEST_ASSERT_NULL(ray_col_gather_sel(&s, sel, &g[1]));
    ray_col_src_release(&s);
    ray_release(t);
    for (int w = 0; w < 2; w++) {
        ray_t* ref = aw_eval(w ? "(at (at (.db.parted.get \"" AW_TMP "/gn\" 't) 'w) "
                                 "(where (or (== 0 (% (til 6000) 5)) (and (> (til 6000) 2000) (< (til 6000) 4100)))))"
                               : "(at (at (.db.parted.get \"" AW_TMP "/gn\" 't) 'w) [5999 1 3000 2999 4500 1])");
        TEST_ASSERT_FMT(ref && !RAY_IS_ERR(ref), "ragged ref %d: %s", w, ray_error_msg());
        AW_GATHER_REF(g[w], ref, true, "w");
        TEST_ASSERT_TRUE(g[w]->attrs & RAY_ATTR_HAS_NULLS);
        ray_release(ref); ray_release(g[w]);
    }
    ray_rowsel_release(sel); ray_release(pred);
    PASS();
}

/* ---- Task 6: gathers in the operators ---------------------------------- */

/* aw_same / AW_SAME: Task 1. */
static const char* AW_WIDE =
    "(table [k g v s t] (list (til 200000) (as 'I16 (% (til 200000) 7)) "
    "(as 'I32 (map (fn [j] (if (== 0 (% j 10)) 0N j)) (til 200000))) "
    "(take ['a 'b 'c] 200000) (map (fn [j] (format \"text value %\" (% j 333))) (til 200000))))";

/* q's answer as .csv.write bytes of table expression q over T (malloc'd;
 * NULL on a failure). */
static char* aw_csv(const char* q, const char* tag, size_t* len) {
    char path[600], e[2048];
    snprintf(path, sizeof path, "%s/%s.csv", aw_root(), tag);
    snprintf(e, sizeof e, "(.csv.write %s \"%s\")", q, path);
    ray_t* r = ray_eval_str(e);
    if (!r || RAY_IS_ERR(r)) { if (r) ray_error_free(r); return NULL; }
    ray_release(r);
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char* b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

/* Result tables a (generation 0) and b (generation 2) of `what`: the same
 * column types and every SYM column at the same width. */
static test_result_t aw_widths_same(const char* what, ray_t* a, ray_t* b) {
    TEST_ASSERT_EQ_I(ray_table_ncols(a), ray_table_ncols(b));
    for (int64_t c = 0; c < ray_table_ncols(a); c++) {
        ray_t* x = ray_table_col_idx(a, c);
        ray_t* y = ray_table_col_idx(b, c);
        TEST_ASSERT_TRUE(x && y && !RAY_IS_ERR(x) && !RAY_IS_ERR(y));
        TEST_ASSERT_EQ_I(x->type, y->type);
        if (x->type == RAY_SYM)
            TEST_ASSERT_FMT((x->attrs & RAY_SYM_W_MASK) == (y->attrs & RAY_SYM_W_MASK), "%s: SYM width %d vs %d",
                            what, x->attrs & RAY_SYM_W_MASK, y->attrs & RAY_SYM_W_MASK);
    }
    PASS();
}

/* `q` over the generation-0 and generation-2 tables tbl0 / tbl2 (Rayfall
 * expressions, loaded as T): byte-equal .csv.write answers, exactly
 * want_mat materializations on generation 2, and every SYM column of the
 * two results at the same width. */
static test_result_t aw_csv_same(const char* tbl0, const char* tbl2, const char* q, uint64_t want_mat) {
    char buf[1024]; char* csv[2]; size_t len[2]; ray_t* res[2];
    for (int gen = 0; gen < 2; gen++) {
        snprintf(buf, sizeof buf, "(set T %s)", gen ? tbl2 : tbl0);
        aw_eval_ok(buf);
        ray_enc_counters_reset();
        res[gen] = ray_eval_str(ray_test_tmp(q));
        TEST_ASSERT_FMT(res[gen] && !RAY_IS_ERR(res[gen]) && res[gen]->type == RAY_TABLE, "%s gen %d: %s",
                        q, gen, ray_error_msg());
        if (gen) {
            ray_enc_counters_t k; ray_enc_counters_get(&k);
            TEST_ASSERT_FMT(k.materialized == want_mat, "%s: %llu materializations, want %llu", q,
                            (unsigned long long)k.materialized, (unsigned long long)want_mat);
        }
        ray_env_set(ray_sym_intern("R", 1), res[gen]);
        csv[gen] = aw_csv("R", gen ? "r2" : "r0", &len[gen]);
    }
    bool same = csv[0] && csv[1] && len[0] == len[1] && memcmp(csv[0], csv[1], len[0]) == 0;
    free(csv[0]); free(csv[1]);
    TEST_ASSERT_FMT(same, "%s: csv differs", q);
    aw_eval_ok("(set R 0)");
    test_result_t r = aw_widths_same(q, res[0], res[1]);
    ray_release(res[0]); ray_release(res[1]);
    return r;
}
#define AW_CSV_SAME(a, b, q, m) do { test_result_t r_ = aw_csv_same((a), (b), (q), (m)); if (r_.status != TEST_PASS) return r_; } while (0)

static test_result_t test_gather_operators(void) {
    aw_save("go", AW_WIDE, -1);
    AW_SAME("go", "(select {from: T where: (< k 100)})", 0);                        /* selection gather, every column */
    AW_SAME("go", "(select {k: k t: t from: T where: (== g 3) take: 7})", 0);         /* take: filter head */
    AW_SAME("go", "(select {k: k v: v t: t from: T where: (> v 100) desc: k take: 5})", 0);   /* top-k: the key read by range */
    AW_SAME("go", "(select {t: t from: T where: (> v 100) asc: [g k] take: 4})", 0);           /* two sort keys */
    /* the same answers byte for byte through .csv.write */
    const char* t0 = "(.db.splayed.get \"" AW_TMP "/go0/\")";
    const char* t2 = "(.db.splayed.get \"" AW_TMP "/go2/\")";
    AW_CSV_SAME(t0, t2, "(select {from: T where: (< k 100)})", 0);
    AW_CSV_SAME(t0, t2, "(select {k: k t: t from: T where: (== g 3) take: 7})", 0);
    AW_CSV_SAME(t0, t2, "(select {k: k v: v s: s t: t from: T where: (> v 100) desc: k take: 5})", 0);
    AW_CSV_SAME(t0, t2, "(select {k: k s: s t: t from: T where: (< v 50) take: -6})", 0);
    /* no row passes: the empty table with every column's type */
    AW_CSV_SAME(t0, t2, "(select {from: T where: (< k 0)})", 0);
    AW_CSV_SAME(t0, t2, "(select {k: k s: s t: t from: T where: (and (< k 100) (> v 1000))})", 0);
    PASS();
}

/* SR_COMPACT_GROUP's shape: a filtered group under the parallel threshold
 * gathers its key and input through agg_build_compact. */
static test_result_t test_gather_compact_group(void) {
    aw_save("gc", AW_WIDE, -1);
    AW_SAME("gc", "(select {c: (count k) from: T where: (< k 1000) by: g})", 0);
    AW_SAME("gc", "(select {c: (count k) m: (max v) from: T where: (< k 1000) by: s})", 0);
    PASS();
}

/* q39's shape: a conjunction whose first conjunct passes <= 25% of the
 * rows; the later conjuncts refine its selection, compacting their
 * predicate columns (sel_compact). */
static test_result_t test_gather_refine(void) {
    aw_save("gr", AW_WIDE, -1);
    AW_SAME("gr", "(select {c: (count k) from: T where: (and (< k 30000) (> v 100) (!= g 0))})", 0);
    AW_SAME("gr", "(select {k: k t: t from: T where: (and (< k 30000) (> v 100) (!= g 0))})", 0);
    AW_SAME("gr", "(take (select {s: s p: (count s) from: T where: (and (< k 30000) (== g 3) (> v 100)) "
                  "by: s desc: p take: 2}) -1)", 0);
    PASS();
}

/* LIST cells beside generation-2 columns: gathered by a selection, the
 * result read after the source table is released (ASan); then a partial
 * failure (k's chunk-0 descriptors corrupted, so its gather fails after p
 * and l were gathered) releases every retained cell and the partial
 * columns once. */
static test_result_t test_gather_list_cells(void) {
    aw_eval_ok("(set W (table [p l k] (list (til 3000) (map (fn [j] (list j (* 2 j))) (til 3000)) (til 3000))))");
    aw_eval_ok("(.db.splayed.set \"" AW_TMP "/li2/\" W {compress: true})");
    aw_eval_ok("(set T (.db.splayed.get \"" AW_TMP "/li2/\"))");
    ray_t* r = aw_eval("(select {p: p l: l k: k from: T where: (< p 10)})");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    aw_eval_ok("(set T 0)");                              /* the source table is gone */
    ray_t* fr = ray_fmt(r, 0);
    ray_t* want = aw_eval("(select {p: p l: l k: k from: W where: (< p 10)})");
    ray_t* fw = ray_fmt(want, 0);
    TEST_ASSERT_TRUE(ray_str_len(fr) == ray_str_len(fw) && memcmp(ray_str_ptr(fr), ray_str_ptr(fw), ray_str_len(fr)) == 0);
    ray_release(fr); ray_release(fw); ray_release(r); ray_release(want);
    PASS();
}

/* An index row list: nothing materializes (the HASH region is probed where
 * it is mapped, ray_enc_hash_eq_rowsel). */
static test_result_t test_gather_index_rows(void) {
    aw_eval_ok("(set W (table [h v t] (list (.idx.hash (% (* (til 100000) 7919) 1000003)) (til 100000) "
               "(map (fn [j] (format \"r%\" j)) (til 100000)))))");
    aw_eval_ok("(.db.splayed.set \"" AW_TMP "/ix0/\" W {compress: false})");
    aw_eval_ok("(.db.splayed.set \"" AW_TMP "/ix2/\" W {compress: true})");
    /* the HASH key is probed in place; v and t are gathered by rows */
    AW_SAME("ix", "(select {v: v t: t from: T where: (== h 7919)})", 0);
    PASS();
}

/* Parted SYM outputs: partition 1 holds 3 symbols (W8), partition 2 adds
 * 400 (W16).  Every row-returning route (selection compaction, filter
 * head, filter refinement, the sequential filter) answers byte for byte as
 * generation 0 and at generation 0's width (the widest segment's), with
 * nothing materialized. */
static void aw_gather_parted_sym_tables(void) {
    for (int gen = 0; gen <= 2; gen += 2) {
        char q[900];
        snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/ps%d/2024.01.01/t/\" "
                 "(table [k s v] (list (til 70000) (take ['a 'b 'c] 70000) (as 'I32 (%% (til 70000) 50)))) "
                 "\"" AW_TMP "/ps%d/.sym\" {compress: %s})", gen, gen, gen ? "true" : "false");
        aw_eval_ok(q);
        snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/ps%d/2024.01.02/t/\" "
                 "(table [k s v] (list (+ 70000 (til 70000)) (as 'SYMBOL (map (fn [j] (format \"w%%\" (%% j 400))) (til 70000))) "
                 "(as 'I32 (%% (til 70000) 50)))) \"" AW_TMP "/ps%d/.sym\" {compress: %s})", gen, gen, gen ? "true" : "false");
        aw_eval_ok(q);
    }
}
static test_result_t test_gather_parted_sym(void) {
    aw_gather_parted_sym_tables();
    const char* t0 = "(.db.parted.get \"" AW_TMP "/ps0\" 't)";
    const char* t2 = "(.db.parted.get \"" AW_TMP "/ps2\" 't)";
    static const char* const qs[] = {
        "(select {k: k s: s from: T where: (or (< k 20) (> k 139990))})",
        "(select {k: k s: s from: T where: (> k 69990) take: 20})",
        "(select {k: k s: s from: T where: (and (> k 69000) (== v 7) (< k 71000))})",
        "(select {from: T where: (== v 7)})",
        NULL,
    };
    for (int i = 0; qs[i]; i++) AW_CSV_SAME(t0, t2, qs[i], 0);
    /* the parted output's width is the widest segment's */
    aw_eval_ok("(set T (.db.parted.get \"" AW_TMP "/ps2\" 't))");
    ray_t* r = aw_eval("(select {s: s from: T where: (or (< k 3) (> k 139997))})");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    ray_t* s = ray_table_col_idx(r, 0);
    TEST_ASSERT_TRUE(s && s->type == RAY_SYM);
    TEST_ASSERT_EQ_I(s->attrs & RAY_SYM_W_MASK, RAY_SYM_W16);
    ray_release(r);
    PASS();
}

/* Fix round 1: a selection of at least RAY_PARALLEL_THRESHOLD rows is
 * gathered by pool tasks.  300000 rows (five chunks) under ALL, MIX and
 * NONE segments, every type a view carries (nulls, NaN, in-band SYM and
 * STR nulls, pooled and inline STR): each result equals generation 0's
 * `at` and the generation-0 source's own gather (pool bytes included, so
 * the task pools are appended in row order), and the dispatch ran. */
#define AW_GP_PRED "(or (< (til 300000) 70000) (and (>= (til 300000) 100000) (< (til 300000) 260000) (!= 0 (% (til 300000) 5))) (> (til 300000) 290000))"
static test_result_t test_gather_sel_parallel(void) {
    aw_save("gp", "(table [i f s y d h] (list "
            "(map (fn [j] (if (== 0 (% j 7)) 0Nl (* j 3))) (til 300000)) "
            "(map (fn [j] (if (== 0 (% j 11)) 0Nf (* j 0.5))) (til 300000)) "
            "(map (fn [j] (if (== 0 (% j 5)) \"\" (if (== 0 (% j 2)) \"short\" (format \"a pooled string of row %\" j)))) (til 300000)) "
            "(as 'SYMBOL (map (fn [j] (if (== 0 (% j 13)) \"\" (format \"k%\" (% j 300)))) (til 300000))) "
            "(as 'DATE (til 300000)) (% (til 300000) 251)))", -1);
    ray_t* t0 = aw_load("gp", 0); ray_t* t2 = aw_load("gp", 2);
    TEST_ASSERT_TRUE(t0 && t2);
    ray_t* pred = aw_eval(AW_GP_PRED);
    TEST_ASSERT_TRUE(pred && !RAY_IS_ERR(pred) && pred->type == RAY_BOOL);
    ray_t* sel = ray_rowsel_from_pred(pred);
    TEST_ASSERT_NOT_NULL(sel);
    TEST_ASSERT_TRUE(ray_rowsel_meta(sel)->total_pass >= RAY_PARALLEL_THRESHOLD);
    static const char* cols[] = { "i", "f", "s", "y", "d", "h" };
    for (int c = 0; c < 6; c++) {
        int64_t nm = ray_sym_intern(cols[c], 1);
        ray_t* out[2];
        for (int gen = 0; gen < 2; gen++) {
            ray_col_src_t s;
            TEST_ASSERT_NULL(ray_table_col_src(gen ? t2 : t0, nm, &s));
            uint64_t p0 = ray_col_test_gather_par();
            TEST_ASSERT_NULL(ray_col_gather_sel(&s, sel, &out[gen]));
            TEST_ASSERT_EQ_U(ray_col_test_gather_par(), p0 + 1);      /* the pool tasks ran */
            ray_col_src_release(&s);
        }
        char q[700];
        snprintf(q, sizeof q, "(at (at (.db.splayed.get \"" AW_TMP "/gp0/\") '%s) (where %s))", cols[c], AW_GP_PRED);
        ray_t* ref = aw_eval(q);
        TEST_ASSERT_FMT(ref && !RAY_IS_ERR(ref), "%s: %s", q, ray_error_msg());
        AW_GATHER_REF(out[0], ref, true, cols[c]);
        AW_GATHER_REF(out[1], ref, false, cols[c]);
        ray_release(ref);
        test_result_t r = aw_vec_same(out[0], out[1], cols[c]);
        if (r.status != TEST_PASS) return r;
        if (cols[c][0] == 's') TEST_ASSERT_TRUE(out[1]->str_pool && out[1]->str_pool->len > 0);
        ray_release(out[0]); ray_release(out[1]);
    }
    ray_rowsel_release(sel); ray_release(pred);
    ray_release(t0); ray_release(t2);
    PASS();
}

/* P3 Task 6b fix round 1: a long ascending row list (repeats included) is
 * gathered by pool tasks over position ranges (grows_par), each with its
 * own cursor; STR pools merge in position order.  Every type equals the
 * generation-0 reference and the generation-0 source's own gather. */
static test_result_t test_gather_rows_parallel(void) {
    aw_save("gr", "(table [i s y h] (list "
            "(map (fn [j] (if (== 0 (% j 7)) 0Nl (* j 3))) (til 300000)) "
            "(map (fn [j] (if (== 0 (% j 5)) \"\" (if (== 0 (% j 2)) \"short\" (format \"a pooled string of row %\" j)))) (til 300000)) "
            "(as 'SYMBOL (map (fn [j] (if (== 0 (% j 13)) \"\" (format \"k%\" (% j 300)))) (til 300000))) "
            "(% (til 300000) 251)))", -1);
    ray_t* t0 = aw_load("gr", 0); ray_t* t2 = aw_load("gr", 2);
    TEST_ASSERT_TRUE(t0 && t2);
    const int64_t n = 2 * 100000;
    ray_t* idv = ray_vec_new(RAY_I64, n); idv->len = n;
    int64_t* ids = (int64_t*)ray_data(idv);
    for (int64_t k = 0; k < n; k++) ids[k] = (k / 2) * 3;        /* every third row, twice */
    TEST_ASSERT_TRUE(n >= RAY_PARALLEL_THRESHOLD);
    static const char* cols[] = { "i", "s", "y", "h" };
    for (int c = 0; c < 4; c++) {
        int64_t nm = ray_sym_intern(cols[c], 1);
        ray_t* out[2];
        for (int gen = 0; gen < 2; gen++) {
            ray_col_src_t sr;
            TEST_ASSERT_NULL(ray_table_col_src(gen ? t2 : t0, nm, &sr));
            TEST_ASSERT_NULL(ray_col_gather_rows(&sr, ids, n, &out[gen]));
            ray_col_src_release(&sr);
        }
        ray_t* ref = ray_at_fn(ray_table_col(t0, nm), idv);
        TEST_ASSERT_TRUE(ref && !RAY_IS_ERR(ref));
        AW_GATHER_REF(out[0], ref, true, cols[c]);
        AW_GATHER_REF(out[1], ref, false, cols[c]);
        ray_release(ref);
        test_result_t r = aw_vec_same(out[0], out[1], cols[c]);
        if (r.status != TEST_PASS) return r;
        ray_release(out[0]); ray_release(out[1]);
    }
    ray_release(idv); ray_release(t0); ray_release(t2);
    PASS();
}

/* A chained filter built through the API, the q41 order: FILTER(in or
 * not in of `col` against `set`, FILTER(k < 3000, T)), unoptimized so the
 * membership test arrives over the first filter's selection (1.5% of the
 * rows) and is answered by the refinement. */
static ray_t* aw_refine_in_run(ray_t* t, const char* col, bool neg, ray_t* set, uint64_t* mat) {
    ray_graph_t* g = ray_graph_new(t);
    ray_op_t* f1 = ray_filter(g, ray_const_table(g, t), ray_lt(g, ray_scan(g, "k"), ray_const_i64(g, 3000)));
    ray_op_t* m = neg ? ray_not_in(g, ray_scan(g, col), ray_const_vec(g, set))
                      : ray_in(g, ray_scan(g, col), ray_const_vec(g, set));
    ray_op_t* f2 = ray_filter(g, f1, m);
    ray_enc_counters_reset();
    ray_t* r = ray_execute(g, f2);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    *mat = k.materialized;
    ray_graph_free(g);
    return r;
}

/* Fix round 2: the parallel gather's other exits.  i holds one null, at
 * row 250001 (a late task's segment only): HAS_NULLS is set and the value
 * is null, equal to generation 0's `at`.  Under ray_cancel() the pool
 * skips the tasks: both the I64 and the pooled STR gather answer `cancel`,
 * with no output and nothing left allocated. */
static test_result_t test_gather_sel_late_null_cancel(void) {
    aw_save("gl", "(table [i t u] (list (map (fn [j] (if (== j 250001) 0Nl j)) (til 300000)) "
                  "(map (fn [j] (format \"a pooled string of row %\" j)) (til 300000)) "
                  "(map (fn [j] (if (> j 295000) (format \"a late pooled string %\" j) \"s\")) (til 300000))))", -1);
    ray_t* t0 = aw_load("gl", 0); ray_t* t2 = aw_load("gl", 2);
    TEST_ASSERT_TRUE(t0 && t2);
    ray_t* pred = ray_vec_new(RAY_BOOL, 300000); pred->len = 300000;
    for (int64_t r = 0; r < 300000; r++) ((uint8_t*)ray_data(pred))[r] = r % 3 != 1;
    ray_t* sel = ray_rowsel_from_pred(pred);
    TEST_ASSERT_NOT_NULL(sel);
    ray_col_src_t s;
    TEST_ASSERT_NULL(ray_table_col_src(t2, ray_sym_intern("i", 1), &s));
    uint64_t p0 = ray_col_test_gather_par();
    ray_t* got = NULL;
    TEST_ASSERT_NULL(ray_col_gather_sel(&s, sel, &got));
    TEST_ASSERT_EQ_U(ray_col_test_gather_par(), p0 + 1);
    ray_col_src_release(&s);
    TEST_ASSERT_TRUE(got->attrs & RAY_ATTR_HAS_NULLS);
    int64_t at = 0;
    for (int64_t r = 0; r < 250001; r++) at += r % 3 != 1;
    TEST_ASSERT_TRUE(ray_vec_is_null(got, at));
    ray_t* ref = aw_eval("(at (at (.db.splayed.get \"" AW_TMP "/gl0/\") 'i) (where (!= 1 (% (til 300000) 3))))");
    TEST_ASSERT_TRUE(ref && !RAY_IS_ERR(ref));
    AW_GATHER_REF(got, ref, true, "i");
    ray_release(ref); ray_release(got);
    /* u is pooled only in the last task's rows (295001-299999): its pool, adopted */
    TEST_ASSERT_NULL(ray_table_col_src(t2, ray_sym_intern("u", 1), &s));
    TEST_ASSERT_NULL(ray_col_gather_sel(&s, sel, &got));
    ray_col_src_release(&s);
    ref = aw_eval("(at (at (.db.splayed.get \"" AW_TMP "/gl0/\") 'u) (where (!= 1 (% (til 300000) 3))))");
    TEST_ASSERT_TRUE(ref && !RAY_IS_ERR(ref));
    AW_GATHER_REF(got, ref, true, "u");
    ray_release(ref); ray_release(got);
    static const char* cols[] = { "i", "t" };
    for (int c = 0; c < 2; c++) {
        TEST_ASSERT_NULL(ray_table_col_src(t2, ray_sym_intern(cols[c], 1), &s));
        ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
        p0 = ray_col_test_gather_par();
        ray_t* g = NULL;
        ray_cancel();
        ray_t* e = ray_col_gather_sel(&s, sel, &g);
        ray_cancel_reset();
        TEST_ASSERT_EQ_U(ray_col_test_gather_par(), p0 + 1);
        TEST_ASSERT_FMT(e && RAY_IS_ERR(e) && !g, "%s: not an error", cols[c]);
        TEST_ASSERT_STR_EQ(ray_err_code(e), "cancel");
        ray_error_free(e);
        ray_mem_stats(&m1);
        TEST_ASSERT_EQ_U(m1.bytes_allocated, m0.bytes_allocated);
        ray_col_src_release(&s);
    }
    ray_rowsel_release(sel); ray_release(pred); ray_release(t0); ray_release(t2);
    PASS();
}

/* Fix round 1 (q41's TraficSourceID): a later conjunct `in` / `not in`
 * over a selective first one is answered by the refinement on the
 * survivors' gathered values, so the column is not materialized; nullable
 * members answer as generation 0. */
static test_result_t test_refine_in(void) {
    aw_save("ri", AW_WIDE, -1);
    AW_SAME("ri", "(select {c: (count k) from: T where: (and (== g 3) (in k [3 10 17 24 1000 5000]))})", 0);
    AW_SAME("ri", "(select {k: k v: v from: T where: (and (== g 3) (not-in v [3 10 17 24]))})", 0);
    AW_SAME("ri", "(select {c: (count k) from: T where: (and (== g 3) (in v [0N 101 115 20001]))})", 0);
    AW_SAME("ri", "(select {c: (count k) by: s from: T where: (and (== g 3) (in v [0N 101 115 20001]))})", 0);
    /* the refinement itself: I16 and nullable I32 members, in and not in */
    ray_t* t0 = aw_load("ri", 0); ray_t* t2 = aw_load("ri", 2);
    TEST_ASSERT_TRUE(t0 && t2);
    static const struct { const char* col; bool neg; const char* set; } cs[] = {
        { "g", false, "[-1 6 3]" }, { "g", true, "[-1 6 3]" },
        { "v", false, "(as 'I32 [0N 101 115 2001])" }, { "v", true, "(as 'I32 [101 115 2001])" },
    };
    for (int i = 0; i < 4; i++) {
        ray_t* set = aw_eval(cs[i].set);
        TEST_ASSERT_TRUE(set && !RAY_IS_ERR(set));
        uint64_t m0 = 0, m2 = 0, h0 = ray_exec_test_refine_in();
        ray_t* r0 = aw_refine_in_run(t0, cs[i].col, cs[i].neg, set, &m0);
        TEST_ASSERT_EQ_U(ray_exec_test_refine_in(), h0);                 /* generation 0 keeps its route */
        ray_t* r2 = aw_refine_in_run(t2, cs[i].col, cs[i].neg, set, &m2);
        TEST_ASSERT_EQ_U(ray_exec_test_refine_in(), h0 + 1);             /* the refinement answered it */
        TEST_ASSERT_TRUE(r0 && r2 && !RAY_IS_ERR(r0) && !RAY_IS_ERR(r2) && r0->type == RAY_TABLE);
        TEST_ASSERT_FMT(m2 == 0, "%s %d: %llu materializations", cs[i].col, cs[i].neg, (unsigned long long)m2);
        TEST_ASSERT_TRUE(ray_table_nrows(r0) > 0 && ray_table_nrows(r0) < 3000);
        ray_t* fa = ray_fmt(r0, 0); ray_t* fb = ray_fmt(r2, 0);
        TEST_ASSERT_FMT(ray_str_len(fa) == ray_str_len(fb) && memcmp(ray_str_ptr(fa), ray_str_ptr(fb), ray_str_len(fa)) == 0,
                        "%s %d differs", cs[i].col, cs[i].neg);
        ray_release(fa); ray_release(fb); ray_release(r0); ray_release(r2); ray_release(set);
    }
    /* SYM `in` is not admitted by the refinement (exec_in does not
     * translate the file domain): this direct refinement call keeps the
     * full-width path.  Through the language the later conjunct is read by
     * range over the installed selection (P3 Task 14b, the single-file arm
     * of exec_in_parted_selection, whose probe translates the set into the
     * column's domain): 0 materializations (it read s whole before, 1); as
     * a first conjunct Task 4's fused leaf answers it on ids
     * (sym_membership) */
    {
        ray_t* set = aw_eval("['a 'c 'zz]");
        TEST_ASSERT_TRUE(set && !RAY_IS_ERR(set));
        uint64_t m0 = 0, m2 = 0, h0 = ray_exec_test_refine_in();
        ray_t* r0 = aw_refine_in_run(t0, "s", false, set, &m0);
        ray_t* r2 = aw_refine_in_run(t2, "s", false, set, &m2);
        TEST_ASSERT_EQ_U(ray_exec_test_refine_in(), h0);
        TEST_ASSERT_TRUE(r0 && r2 && !RAY_IS_ERR(r0) && !RAY_IS_ERR(r2));
        TEST_ASSERT_TRUE(ray_table_nrows(r0) > 0);
        ray_t* fa = ray_fmt(r0, 0); ray_t* fb = ray_fmt(r2, 0);
        TEST_ASSERT_TRUE(ray_str_len(fa) == ray_str_len(fb) && memcmp(ray_str_ptr(fa), ray_str_ptr(fb), ray_str_len(fa)) == 0);
        ray_release(fa); ray_release(fb); ray_release(r0); ray_release(r2); ray_release(set);
        AW_SAME("ri", "(select {c: (count k) by: g from: T where: (and (== g 3) (in s ['a 'c 'zz]))})", 0);
    }
    ray_release(t0); ray_release(t2);
    /* F64 members: NaN nulls, -0.0 against +0.0, through exec_in on the
     * survivors, equal to generation 0 */
    aw_save("rf", "(table [k f] (list (til 200000) (map (fn [j] (if (== 0 (% j 10)) 0Nf (if (== 0 (% j 7)) -0.0 (* 0.5 (% j 9))))) (til 200000))))", -1);
    t0 = aw_load("rf", 0); t2 = aw_load("rf", 2);
    TEST_ASSERT_TRUE(t0 && t2);
    static const struct { bool neg; const char* set; } fs[] = {
        { false, "[0.0 2.5 0Nf]" }, { true, "[0.0 2.5]" }, { false, "[-0.0 4.0]" }, { true, "[0Nf 1.0]" },
    };
    for (int i = 0; i < 4; i++) {
        ray_t* set = aw_eval(fs[i].set);
        TEST_ASSERT_TRUE(set && !RAY_IS_ERR(set) && set->type == RAY_F64);
        uint64_t m0 = 0, m2 = 0, h0 = ray_exec_test_refine_in();
        ray_t* r0 = aw_refine_in_run(t0, "f", fs[i].neg, set, &m0);
        ray_t* r2 = aw_refine_in_run(t2, "f", fs[i].neg, set, &m2);
        TEST_ASSERT_EQ_U(ray_exec_test_refine_in(), h0 + 1);
        TEST_ASSERT_TRUE(r0 && r2 && !RAY_IS_ERR(r0) && !RAY_IS_ERR(r2));
        TEST_ASSERT_FMT(m2 == 0, "f %s: %llu materializations", fs[i].set, (unsigned long long)m2);
        TEST_ASSERT_TRUE(ray_table_nrows(r0) > 0);
        ray_t* fa = ray_fmt(r0, 0); ray_t* fb = ray_fmt(r2, 0);
        TEST_ASSERT_FMT(ray_str_len(fa) == ray_str_len(fb) && memcmp(ray_str_ptr(fa), ray_str_ptr(fb), ray_str_len(fa)) == 0,
                        "f %s differs", fs[i].set);
        ray_release(fa); ray_release(fb); ray_release(r0); ray_release(r2); ray_release(set);
    }
    ray_release(t0); ray_release(t2);
    /* q41's shape: a grouped take, whose conjuncts run as a filter chain */
    AW_SAME("ri", "(take (select {c: (count k) from: T where: (and (== g 3) (>= k 100) (<= k 150000) "
                  "(in v [101 115 136 20001 0N]) (!= s 'q)) by: {s: s k: k} desc: c take: 20}) -10)", 0);
    AW_SAME("ri", "(take (select {c: (count k) from: T where: (and (== g 3) (>= k 100) (<= k 150000) "
                  "(not-in v [101 115 136 20001])) by: {s: s k: k} desc: c take: 20}) -10)", 0);
    PASS();
}

/* The operators called directly on generation-0 and generation-2 copies
 * of one table: exec_filter (its parallel gather over 200000 rows and the
 * parted arm over 140000; its sequential arm under the parallel
 * threshold), exec_filter_head and the sort top-K (topk_gather_rows, the
 * key passed as a plain vector).  Generation 2 answers byte for byte
 * (.csv.write) as generation 0's operator, with SYM at the same width and
 * nothing materialized; the filters also equal generation 0's selection
 * compaction of the same rows (the language route, sel_compact), so a null
 * bit lost by a flat or parted gather shows here: exec_filter's flat
 * gathers and exec_filter_head's flat and parted ones copied values
 * without their null bits before P3 Task 6. */
static test_result_t aw_csv_eq(const char* what, int op, ray_t* a, ray_t* b) {
    char* csv[2]; size_t len[2];
    ray_t* r[2] = { a, b };
    for (int i = 0; i < 2; i++) {
        ray_env_set(ray_sym_intern("R", 1), r[i]);
        csv[i] = aw_csv("R", i ? "o2" : "o0", &len[i]);
    }
    aw_eval_ok("(set R 0)");
    bool same = csv[0] && csv[1] && len[0] == len[1] && memcmp(csv[0], csv[1], len[0]) == 0;
    free(csv[0]); free(csv[1]);
    TEST_ASSERT_FMT(same, "%s op %d: csv differs", what, op);
    PASS();
}
static test_result_t aw_ops_same(const char* tbl0, const char* tbl2, bool topk) {
    char buf[1024];
    ray_t* t[2];
    for (int gen = 0; gen < 2; gen++) {
        snprintf(buf, sizeof buf, "%s", gen ? tbl2 : tbl0);
        t[gen] = aw_eval(buf);
        TEST_ASSERT_FMT(t[gen] && !RAY_IS_ERR(t[gen]), "%s: %s", buf, ray_error_msg());
    }
    int64_t n = ray_table_nrows(t[0]);
    ray_t* pred = ray_vec_new(RAY_BOOL, n); pred->len = n;
    ray_t* head = ray_vec_new(RAY_BOOL, n); head->len = n;        /* the first 7 passing rows */
    int64_t kept = 0;
    for (int64_t r = 0; r < n; r++) {
        uint8_t b = r < 5 || r % 97 == 3 || r > n - 3;
        ((uint8_t*)ray_data(pred))[r] = b;
        ((uint8_t*)ray_data(head))[r] = b && kept < 7;
        kept += b;
    }
    ray_t* key = ray_table_col(t[0], ray_sym_intern("k", 1));       /* generation 0's, a plain vector */
    TEST_ASSERT_TRUE(key && !RAY_IS_ERR(key));
    for (int op = 0; op < (topk ? 3 : 2); op++) {
        ray_t* res[2];
        for (int gen = 0; gen < 2; gen++) {
            ray_enc_counters_reset();
            res[gen] = op == 0 ? exec_filter(NULL, NULL, t[gen], pred)
                     : op == 1 ? exec_filter_head(t[gen], pred, 7)
                               : ray_topk_table(t[gen], key, 1, 0, 6);
            ray_enc_counters_t k; ray_enc_counters_get(&k);
            if (gen) TEST_ASSERT_FMT(k.materialized == 0, "%s op %d: %llu materializations", tbl2, op,
                                     (unsigned long long)k.materialized);
            TEST_ASSERT_FMT(res[gen] && !RAY_IS_ERR(res[gen]) && res[gen]->type == RAY_TABLE, "%s op %d gen %d: %s",
                            tbl2, op, gen, ray_error_msg());
        }
        test_result_t r = aw_csv_eq(tbl2, op, res[0], res[1]);
        if (r.status == TEST_PASS && op < 2) {
            ray_t* sel = ray_rowsel_from_pred(op ? head : pred);
            TEST_ASSERT_NOT_NULL(sel);
            ray_t* ref = sel_compact(NULL, t[0], sel, NULL, 0);
            ray_rowsel_release(sel);
            TEST_ASSERT_TRUE(ref && !RAY_IS_ERR(ref));
            r = aw_csv_eq("selection compaction", op, ref, res[1]);
            ray_release(ref);
        }
        if (r.status == TEST_PASS)
            TEST_ASSERT_EQ_I(ray_table_nrows(res[1]), op == 0 ? kept : op == 1 ? 7 : 6);
        if (r.status == TEST_PASS) r = aw_widths_same(tbl2, res[0], res[1]);
        ray_release(res[0]); ray_release(res[1]);
        if (r.status != TEST_PASS) return r;
    }
    ray_release(pred); ray_release(head); ray_release(t[0]); ray_release(t[1]);
    PASS();
}

static test_result_t test_gather_filter_ops(void) {
    aw_save("fo", AW_WIDE, -1);
    test_result_t r = aw_ops_same("(.db.splayed.get \"" AW_TMP "/fo0/\")", "(.db.splayed.get \"" AW_TMP "/fo2/\")", true);
    if (r.status != TEST_PASS) return r;
    aw_save("fs", "(table [k g v s t] (list (til 3000) (as 'I16 (% (til 3000) 7)) "
                  "(as 'I32 (map (fn [j] (if (== 0 (% j 10)) 0N j)) (til 3000))) "
                  "(as 'SYMBOL (map (fn [j] (format \"y%\" (% j 300))) (til 3000))) "
                  "(map (fn [j] (format \"a longer text value %\" (% j 333))) (til 3000))))", -1);
    r = aw_ops_same("(.db.splayed.get \"" AW_TMP "/fs0/\")", "(.db.splayed.get \"" AW_TMP "/fs2/\")", true);
    if (r.status != TEST_PASS) return r;
    aw_gather_parted_sym_tables();
    r = aw_ops_same("(.db.parted.get \"" AW_TMP "/ps0\" 't)", "(.db.parted.get \"" AW_TMP "/ps2\" 't)", false);
    if (r.status != TEST_PASS) return r;
    /* the language route to exec_filter_head (a parted take under a where:
     * on the partition key): the null at row 0 stays null on both
     * generations (generation 0 printed -9223372036854775808 before) */
    for (int gen = 0; gen <= 2; gen += 2)
        for (int p = 0; p < 2; p++) {
            char q[600];
            snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/hn%d/2024.01.0%d/t/\" "
                     "(table [a b] (list (as 'I64 [0N 1 2 0N 4]) (as 'I32 [5 0N 7 8 9]))) {compress: %s})",
                     gen, p + 1, gen ? "true" : "false");
            aw_eval_ok(q);
        }
    for (int gen = 0; gen <= 2; gen += 2) {
        char q[600];
        snprintf(q, sizeof q, "(set T (.db.parted.get \"" AW_TMP "/hn%d\" 't))", gen);
        aw_eval_ok(q);
        ray_t* got = aw_eval("(select {from: T where: (not (== date 2024.01.02)) take: 3})");
        ray_t* want = aw_eval("(table [date a b] (list (take 2024.01.01 3) (as 'I64 [0N 1 2]) (as 'I32 [5 0N 7])))");
        TEST_ASSERT_TRUE(got && want && !RAY_IS_ERR(got) && !RAY_IS_ERR(want));
        ray_t* fg = ray_fmt(got, 0); ray_t* fw = ray_fmt(want, 0);
        bool same = ray_str_len(fg) == ray_str_len(fw) && memcmp(ray_str_ptr(fg), ray_str_ptr(fw), ray_str_len(fg)) == 0;
        TEST_ASSERT_FMT(same, "gen %d: %.*s", gen, (int)ray_str_len(fg), ray_str_ptr(fg));
        ray_release(fg); ray_release(fw); ray_release(got); ray_release(want);
    }
    /* no row passes the operators: empty tables equal to generation 0's */
    for (int big = 0; big < 2; big++) {
        ray_t* tz[2];
        for (int gen = 0; gen < 2; gen++) {
            tz[gen] = aw_load(big ? "fo" : "fs", gen ? 2 : 0);
            TEST_ASSERT_NOT_NULL(tz[gen]);
        }
        int64_t n = ray_table_nrows(tz[0]);
        ray_t* zp = ray_vec_new(RAY_BOOL, n); zp->len = n;
        memset(ray_data(zp), 0, (size_t)n);
        for (int op = 0; op < 2; op++) {
            ray_t* res[2];
            for (int gen = 0; gen < 2; gen++) {
                ray_enc_counters_reset();
                res[gen] = op ? exec_filter_head(tz[gen], zp, 5) : exec_filter(NULL, NULL, tz[gen], zp);
                TEST_ASSERT_TRUE(res[gen] && !RAY_IS_ERR(res[gen]) && ray_table_nrows(res[gen]) == 0);
                ray_enc_counters_t k; ray_enc_counters_get(&k);
                if (gen) TEST_ASSERT_EQ_U(k.materialized, 0);
            }
            r = aw_csv_eq("no row passes", op, res[0], res[1]);
            if (r.status == TEST_PASS) r = aw_widths_same("no row passes", res[0], res[1]);
            ray_release(res[0]); ray_release(res[1]);
            if (r.status != TEST_PASS) return r;
        }
        ray_release(zp); ray_release(tz[0]); ray_release(tz[1]);
    }
    PASS();
}

/* ---- Task 6b: group keys read at rows ---- */

/* q18's shape (Task 15 gap 1): a group-by take without an order runs the
 * first-n pre-pass over the keys' sources, nothing unstaged. */
static test_result_t test_firstn_by_range(void) {
    aw_save("fn", "(table [h s v] (list (% (* (til 300000) 7919) 100003) "
                  "(as 'SYMBOL (map (fn [j] (format \"p%\" (% j 97))) (til 300000))) (til 300000)))", -1);
    static const char* const qs[] = {
        "(select {c: (count h) from: T by: {h: h s: s} take: 10})",
        "(select {c: (count h) s: (sum v) from: T by: h take: 25})",
        NULL,
    };
    for (int i = 0; qs[i]; i++) {
        agg_route_reset();
        AW_SAME("fn", qs[i], 0);
        TEST_ASSERT_FMT(agg_route_stats().first_n == 2, "%s: first-n answered %llu of 2 runs", qs[i],
                        (unsigned long long)agg_route_stats().first_n);
    }
    PASS();
}

/* The parted form: two partitions of the fn table under a generation-0 and
 * a generation-2 root.  The first-n passes run over the parted sources (no
 * flat copy), nothing is materialized, answers and group order are
 * generation 0's. */
static void aw_firstn_parted_tables(void) {
    for (int gen = 0; gen <= 2; gen += 2)
        for (int p = 0; p < 2; p++) {
            char q[900];
            snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/fp%d/2024.01.0%d/t/\" "
                     "(table [h s v] (list (%% (* (+ (til 150000) %d) 7919) 100003) "
                     "(as 'SYMBOL (map (fn [j] (format \"p%%\" (%% (+ j %d) 97))) (til 150000))) (+ (til 150000) %d))) "
                     "\"" AW_TMP "/fp%d/.sym\" {compress: %s})",
                     gen, p + 1, p * 150000, p * 150000, p * 150000, gen, gen ? "true" : "false");
            aw_eval_ok(q);
        }
}
static test_result_t test_firstn_parted(void) {
    aw_firstn_parted_tables();
    /* a mixed root: partition 1 generation 0, partition 2 generation 2 */
    for (int p = 0; p < 2; p++) {
        char q[900];
        snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/fpm/2024.01.0%d/t/\" "
                 "(table [h s v] (list (%% (* (+ (til 150000) %d) 7919) 100003) "
                 "(as 'SYMBOL (map (fn [j] (format \"p%%\" (%% (+ j %d) 97))) (til 150000))) (+ (til 150000) %d))) "
                 "\"" AW_TMP "/fpm/.sym\" {compress: %s})",
                 p + 1, p * 150000, p * 150000, p * 150000, p ? "true" : "false");
        aw_eval_ok(q);
    }
    /* the mixed root, and the declines over a segment-streamed table (too
     * few groups: 97 symbols for a take of 200; heavy: 3 of 97 groups
     * hold a share above 1/64), each answered as generation 0 */
    static const char* const ds[] = {
        "(select {c: (count h) from: T by: {h: h s: s} take: 10})",
        "(select {c: (count h) s: (sum v) from: T by: s take: 200})",
        "(select {c: (count h) s: (sum v) from: T by: s take: 3})",
        NULL,
    };
    static const char* const roots[] = { "fpm", "fp2", "fp2" };
    for (int i = 0; ds[i]; i++) {
        ray_t* out[2];
        for (int gen = 0; gen < 2; gen++) {
            char b[256];
            snprintf(b, sizeof b, "(set T (.db.parted.get \"" AW_TMP "/%s\" 't))", gen ? roots[i] : "fp0");
            aw_eval_ok(b);
            ray_enc_counters_reset();
            out[gen] = aw_eval(ds[i]);
            TEST_ASSERT_FMT(out[gen] && !RAY_IS_ERR(out[gen]), "%s gen %d: %s", ds[i], gen, ray_error_msg());
            if (gen) { ray_enc_counters_t k; ray_enc_counters_get(&k); TEST_ASSERT_EQ_U(k.materialized, 0); }
        }
        ray_t* fa = ray_fmt(out[0], 0); ray_t* fb = ray_fmt(out[1], 0);
        TEST_ASSERT_FMT(ray_str_len(fa) == ray_str_len(fb) && memcmp(ray_str_ptr(fa), ray_str_ptr(fb), ray_str_len(fa)) == 0,
                        "%s (%s) differs", ds[i], roots[i]);
        ray_release(fa); ray_release(fb); ray_release(out[0]); ray_release(out[1]);
    }
    static const char* const qs[] = {
        "(select {c: (count h) from: T by: {h: h s: s} take: 10})",
        "(select {c: (count h) s: (sum v) from: T by: h take: 25})",
        NULL,
    };
    for (int i = 0; qs[i]; i++) {
        ray_t* out[2];
        for (int gen = 0; gen < 2; gen++) {
            char b[256];
            snprintf(b, sizeof b, "(set T (.db.parted.get \"" AW_TMP "/fp%d\" 't))", gen ? 2 : 0);
            aw_eval_ok(b);
            ray_enc_counters_reset();
            agg_route_reset();
            int64_t f0 = ray_group_test_flat_copies(), s0 = ray_group_segstream_runs();
            out[gen] = aw_eval(qs[i]);
            TEST_ASSERT_FMT(out[gen] && !RAY_IS_ERR(out[gen]), "%s gen %d: %s", qs[i], gen, ray_error_msg());
            if (!gen) continue;
            ray_enc_counters_t k; ray_enc_counters_get(&k);
            TEST_ASSERT_FMT(k.materialized == 0, "%s: %llu materializations", qs[i], (unsigned long long)k.materialized);
            TEST_ASSERT_FMT(ray_group_test_flat_copies() == f0, "%s: %lld flat copies", qs[i],
                            (long long)(ray_group_test_flat_copies() - f0));
            TEST_ASSERT_FMT(ray_group_segstream_runs() - s0 == 1, "%s: not grouped in place", qs[i]);
            TEST_ASSERT_FMT(agg_route_stats().first_n >= 1, "%s: not first-n", qs[i]);
        }
        ray_t* fa = ray_fmt(out[0], 0); ray_t* fb = ray_fmt(out[1], 0);
        TEST_ASSERT_FMT(ray_str_len(fa) == ray_str_len(fb) && memcmp(ray_str_ptr(fa), ray_str_ptr(fb), ray_str_len(fa)) == 0,
                        "%s differs", qs[i]);
        ray_release(fa); ray_release(fb); ray_release(out[0]); ray_release(out[1]);
    }
    PASS();
}

/* q42's shape: a selective filter, keys read at a few rows of a few
 * vectors: point reads, no vector decoded for the keys.  w and h are runs
 * (RLE) or packed (FOR_BP) without nulls, the encodings a point read
 * serves.  c is answered from its statistics; the second conjunct is not
 * a fused leaf, so it compacts w over c's 3000 rows (vectors 181-184, each
 * holding more than RAY_COL_POINT_ROWS of them: decoded once, by the
 * filter).  The group's key reads (the dense plan's selection bounds and
 * the compact gather of its ~60 survivors) decode nothing. */
static test_result_t test_sparse_key_points(void) {
    aw_save("sp", "(table [c w h] (list (div (til 300000) 3000) (as 'I16 (% (div (til 300000) 7) 300)) "
                  "(as 'I16 (% (div (til 300000) 11) 200))))", -1);
    for (int64_t v = 181; v <= 184; v++) {               /* c == 62: rows 186000..188999 */
        int ew = aw_vec_enc("sp", "w", v), eh = aw_vec_enc("sp", "h", v);
        TEST_ASSERT_FMT(ew == RAY_ENC_RLE || ew == RAY_ENC_FOR_BP || ew == RAY_ENC_CONST, "w vector %lld: %d", (long long)v, ew);
        TEST_ASSERT_FMT(eh == RAY_ENC_RLE || eh == RAY_ENC_FOR_BP || eh == RAY_ENC_CONST, "h vector %lld: %d", (long long)v, eh);
    }
    AW_SAME("sp", "(select {n: (count w) from: T where: (and (== c 62) (== 0 (% w 50))) by: {w: w h: h} desc: n take: 10})", 0);
    TEST_ASSERT_FMT(aw_col_decoded("w") == 4 * 1024 * 2 && aw_col_decoded("h") == 0, "w %llu h %llu",
                    (unsigned long long)aw_col_decoded("w"), (unsigned long long)aw_col_decoded("h"));
    PASS();
}

/* Point reads in the row-list and selection gathers (open decision D2,
 * per codec in Task 10b): per encoding, a vector holding at most its
 * codec's limit of requested rows (vector 0: lim rows, vector 5: one row,
 * repeated) is point-read and decodes nothing, one holding more (vector 1:
 * lim + 1 rows) is decoded; a declined read (DELTA_BP) decodes every
 * touched vector.  A vector with a null bitmap is point-read on every
 * codec (pn, pw), its nulls as the decode writes them.  Ids sorted,
 * reversed and repeated; values equal generation 0's. */
static test_result_t test_gather_points(void) {
    /* lim: the codec's limit (RAY_ENC_VEC / 32 packed lanes, / 16 runs,
     * DICT and any vector with a bitmap); 0: the codec declines */
    static const struct { const char* name; const char* x; int enc; int64_t lim; } cs[] = {
        { "pf", "(% (* (til 70000) 7919) 1000)", RAY_ENC_FOR_BP, RAY_ENC_VEC / 32 },
        { "pr", "(div (til 70000) 5)", RAY_ENC_RLE, RAY_ENC_VEC / 16 },
        { "pc", "(div (til 70000) 2048)", RAY_ENC_CONST, RAY_ENC_VEC / 16 },
        { "pd", "(* 1000003 (% (til 70000) 5))", RAY_ENC_DICT, RAY_ENC_VEC / 16 },
        { "pe", "(% (* (til 70000) 7919) 1000)", RAY_ENC_DELTA_BP, 0 },
        { "pn", "(map (fn [j] (if (== 0 (% j 100)) 0N (% (* j 7919) 1000))) (til 70000))", RAY_ENC_FOR_BP, RAY_ENC_VEC / 16 },
        { "pw", "(map (fn [j] (if (== 0 (% j 100)) 0N (* j 7919))) (til 70000))", RAY_ENC_RAW, RAY_ENC_VEC / 16 },
        { "pq", "(map (fn [j] (if (== 0 (% j 100)) 0N (* 1000003 (% j 5)))) (til 70000))", RAY_ENC_DICT, RAY_ENC_VEC / 16 },
    };
    for (size_t c = 0; c < sizeof cs / sizeof cs[0]; c++) {
        /* vector 0: lim rows (point-read when lim > 0), vector 1: lim + 1
         * rows (decoded), vector 5: one row, twice; a declining codec uses
         * the RLE limit's counts */
        const int64_t L = cs[c].lim ? cs[c].lim : RAY_ENC_VEC / 16;
        int64_t ids[RAY_ENC_VEC / 16 * 2 + 3];
        int64_t n = 0;
        for (int64_t i = 0; i < L; i++) ids[n++] = 2 * i;
        for (int64_t i = 0; i <= L; i++) ids[n++] = 1024 + i;
        ids[n++] = 5 * 1024 + 7; ids[n++] = 5 * 1024 + 7;
        int64_t rev[sizeof ids / sizeof ids[0]];
        for (int64_t i = 0; i < n; i++) rev[i] = ids[n - 1 - i];
        const bool point = cs[c].lim > 0;
        char tq[256];
        snprintf(tq, sizeof tq, "(table [x] (list %s))", cs[c].x);
        aw_save(cs[c].name, tq, cs[c].enc);
        for (int64_t v = 0; v <= 5; v += 5) TEST_ASSERT_EQ_I(aw_vec_enc(cs[c].name, "x", v), cs[c].enc);
        ray_t* t0 = aw_load(cs[c].name, 0);
        ray_t* t2 = aw_load(cs[c].name, 2);
        TEST_ASSERT_TRUE(t0 && t2);
        ray_t* ref = ray_table_col(t0, ray_sym_intern("x", 1));
        const int64_t* rv = (const int64_t*)ray_data(ref);
        ray_t* encv = ray_table_get_col_enc(t2, ray_sym_intern("x", 1));
        TEST_ASSERT_NOT_NULL(encv);
        const uint64_t want = (point ? 1 : 3) * 1024 * 8;   /* vector 1, or vectors 0, 1 and 5 */
        for (int pass = 0; pass < 3; pass++) {
            ray_col_src_t s;
            TEST_ASSERT_NULL(ray_table_col_src(t2, ray_sym_intern("x", 1), &s));
            uint64_t d0 = atomic_load(&ray_enccol_of(encv)->decoded_bytes);
            ray_t* out = NULL;
            ray_t* sel = NULL; ray_t* pred = NULL;
            const int64_t* want_ids = pass == 1 ? rev : ids;
            int64_t want_n = n;
            if (pass < 2) {
                TEST_ASSERT_NULL(ray_col_gather_rows(&s, want_ids, n, &out));
            } else {                                  /* a rowsel of the distinct rows */
                pred = ray_vec_new(RAY_BOOL, 70000); pred->len = 70000;
                memset(ray_data(pred), 0, 70000);
                for (int64_t i = 0; i < n; i++) ((uint8_t*)ray_data(pred))[ids[i]] = 1;
                sel = ray_rowsel_from_pred(pred);
                TEST_ASSERT_NOT_NULL(sel);
                TEST_ASSERT_NULL(ray_col_gather_sel(&s, sel, &out));
                want_n = n - 1;
            }
            ray_col_src_release(&s);
            uint64_t got = atomic_load(&ray_enccol_of(encv)->decoded_bytes) - d0;
            TEST_ASSERT_FMT(got == want, "%s pass %d: decoded %llu, want %llu", cs[c].name, pass,
                            (unsigned long long)got, (unsigned long long)want);
            TEST_ASSERT_EQ_I(out->len, want_n);
            TEST_ASSERT_EQ_I(out->attrs & RAY_ATTR_HAS_NULLS, ref->attrs & RAY_ATTR_HAS_NULLS);
            for (int64_t i = 0; i < want_n; i++)
                TEST_ASSERT_FMT(((int64_t*)ray_data(out))[i] == rv[want_ids[i]], "%s pass %d row %lld",
                                cs[c].name, pass, (long long)i);
            ray_release(out);
            if (sel) ray_rowsel_release(sel);
            if (pred) ray_release(pred);
        }
        ray_release(t0); ray_release(t2);
    }
    /* parted: segment-local vectors (partitions of 3000 rows, so vectors
     * and segments straddle the global morsels); few ids per local vector,
     * unsorted and repeated, as a row list and as a rowsel: nothing
     * decoded */
    ray_encw_test_force(RAY_ENC_FOR_BP);
    for (int p = 0; p < 2; p++) {
        char q[600];
        snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/gpp/2024.01.0%d/t/\" "
                 "(table [x] (list (%% (* (+ (til 3000) %d) 7919) 1000))) {compress: true})", p + 1, p * 3000);
        aw_eval_ok(q);
    }
    ray_encw_test_force(-1);
    ray_t* tp = aw_eval("(.db.parted.get \"" AW_TMP "/gpp\" 't)");
    TEST_ASSERT_TRUE(tp && !RAY_IS_ERR(tp));
    static const int64_t pids[] = { 3075, 3070, 5999, 3075, 10, 2999, 3000, 4100 };
    const int64_t pn = 8;
    ray_t* penc = ray_table_get_col_enc(tp, ray_sym_intern("x", 1));
    TEST_ASSERT_NOT_NULL(penc);
    for (int sg = 0; sg < 2; sg++) {
        const ray_enc_desc_t* d = ray_enccol_descs(ray_enccol_of(ray_enccol_seg(penc, sg)), 0);
        for (int j = 0; j < 3; j++) TEST_ASSERT_EQ_I(d[j].encoding, RAY_ENC_FOR_BP);
    }
    ray_t* ppred = ray_vec_new(RAY_BOOL, 6000); ppred->len = 6000;
    memset(ray_data(ppred), 0, 6000);
    for (int64_t i = 0; i < pn; i++) ((uint8_t*)ray_data(ppred))[pids[i]] = 1;
    ray_t* psel = ray_rowsel_from_pred(ppred);
    TEST_ASSERT_NOT_NULL(psel);
    static const int64_t psorted[] = { 10, 2999, 3000, 3070, 3075, 4100, 5999 };
    for (int pass = 0; pass < 2; pass++) {
        ray_col_src_t s;
        TEST_ASSERT_NULL(ray_table_col_src(tp, ray_sym_intern("x", 1), &s));
        ray_enc_counters_reset();
        ray_t* out = NULL;
        TEST_ASSERT_NULL(pass ? ray_col_gather_sel(&s, psel, &out) : ray_col_gather_rows(&s, pids, pn, &out));
        ray_col_src_release(&s);
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        TEST_ASSERT_FMT(k.decoded_bytes == 0 && k.materialized == 0, "parted pass %d: decoded %llu", pass,
                        (unsigned long long)k.decoded_bytes);
        const int64_t* w = pass ? psorted : pids;
        TEST_ASSERT_EQ_I(out->len, pass ? 7 : pn);
        for (int64_t i = 0; i < out->len; i++)
            TEST_ASSERT_EQ_I(((int64_t*)ray_data(out))[i], (w[i] * 7919) % 1000);
        ray_release(out);
    }
    ray_rowsel_release(psel); ray_release(ppred); ray_release(tp);
    /* parted DICT with a null bitmap (Task 10b): the rows on both sides of
     * the partition boundary at 3000 (one global morsel over two segments'
     * vectors), as a rowsel and as a row list, point-read: nothing decoded,
     * nulls where generation 0 has them */
    ray_encw_test_force(RAY_ENC_DICT);
    for (int p = 0; p < 2; p++) {
        char q[700];
        snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/gpd/2024.01.0%d/t/\" "
                 "(table [x] (list (map (fn [j] (if (== 0 (%% j 100)) 0N (* 1000003 (%% j 5)))) (+ (til 3000) %d)))) "
                 "{compress: true})", p + 1, p * 3000);
        aw_eval_ok(q);
    }
    ray_encw_test_force(-1);
    ray_t* td = aw_eval("(.db.parted.get \"" AW_TMP "/gpd\" 't)");
    TEST_ASSERT_TRUE(td && !RAY_IS_ERR(td));
    {
        ray_t* denc = ray_table_get_col_enc(td, ray_sym_intern("x", 1));
        TEST_ASSERT_NOT_NULL(denc);
        for (int sg = 0; sg < 2; sg++) {
            const ray_enc_desc_t* d = ray_enccol_descs(ray_enccol_of(ray_enccol_seg(denc, sg)), 0);
            TEST_ASSERT_TRUE(d[2].encoding == RAY_ENC_DICT && d[2].n_nulls > 0);
            TEST_ASSERT_TRUE(d[0].encoding == RAY_ENC_DICT && d[0].n_nulls > 0);
        }
    }
    static const int64_t dids[] = { 2900, 2990, 2999, 3000, 3005, 3100, 4100 };
    ray_t* dpred = ray_vec_new(RAY_BOOL, 6000); dpred->len = 6000;
    memset(ray_data(dpred), 0, 6000);
    for (int i = 0; i < 7; i++) ((uint8_t*)ray_data(dpred))[dids[i]] = 1;
    ray_t* dsel = ray_rowsel_from_pred(dpred);
    TEST_ASSERT_NOT_NULL(dsel);
    for (int pass = 0; pass < 2; pass++) {
        ray_col_src_t s;
        TEST_ASSERT_NULL(ray_table_col_src(td, ray_sym_intern("x", 1), &s));
        ray_enc_counters_reset();
        ray_t* out = NULL;
        TEST_ASSERT_NULL(pass ? ray_col_gather_sel(&s, dsel, &out) : ray_col_gather_rows(&s, dids, 7, &out));
        ray_col_src_release(&s);
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        TEST_ASSERT_FMT(k.decoded_bytes == 0, "parted DICT pass %d: decoded %llu", pass, (unsigned long long)k.decoded_bytes);
        TEST_ASSERT_EQ_I(out->len, 7);
        TEST_ASSERT_TRUE(out->attrs & RAY_ATTR_HAS_NULLS);
        for (int i = 0; i < 7; i++)
            TEST_ASSERT_EQ_I(((int64_t*)ray_data(out))[i], dids[i] % 100 == 0 ? NULL_I64 : 1000003 * (dids[i] % 5));
        ray_release(out);
    }
    ray_rowsel_release(dsel); ray_release(dpred); ray_release(td);
    PASS();
}

static bool aw_forge_absent(const char* path, int64_t c);

/* Task 10b: a RAW vector without nulls is read in place (the view, which
 * counts the vector's bytes), however few of its rows a gather asks for. */
static test_result_t test_gather_raw_points(void) {
    aw_save("pv", "(table [x] (list (* (til 70000) 7919)))", RAY_ENC_RAW);
    for (int64_t v = 0; v <= 5; v++) TEST_ASSERT_EQ_I(aw_vec_enc("pv", "x", v), RAY_ENC_RAW);
    ray_t* t0 = aw_load("pv", 0); ray_t* t2 = aw_load("pv", 2);
    TEST_ASSERT_TRUE(t0 && t2);
    const int64_t* rv = (const int64_t*)ray_data(ray_table_col(t0, ray_sym_intern("x", 1)));
    ray_t* encv = ray_table_get_col_enc(t2, ray_sym_intern("x", 1));
    TEST_ASSERT_NOT_NULL(encv);
    ray_enccol_t* e = ray_enccol_of(encv);
    static const struct { int64_t v, n; uint64_t want; } rd[] = {
        { 5, RAY_ENC_VEC / 128, 1024 * 8 },
        { 1, RAY_ENC_VEC / 128 + 1, 1024 * 8 },
        { 5, 1, 1024 * 8 },
    };
    for (int r = 0; r < 3; r++) {
        int64_t ids[RAY_ENC_VEC / 128 + 1];
        for (int64_t q = 0; q < rd[r].n; q++) ids[q] = rd[r].v * 1024 + 3 * q;
        ray_col_src_t s;
        TEST_ASSERT_NULL(ray_table_col_src(t2, ray_sym_intern("x", 1), &s));
        uint64_t d0 = atomic_load(&e->decoded_bytes);
        ray_t* out = NULL;
        TEST_ASSERT_NULL(ray_col_gather_rows(&s, ids, rd[r].n, &out));
        ray_col_src_release(&s);
        uint64_t got = atomic_load(&e->decoded_bytes) - d0;
        TEST_ASSERT_FMT(got == rd[r].want, "read %d: decoded %llu, want %llu", r, (unsigned long long)got,
                        (unsigned long long)rd[r].want);
        for (int64_t q = 0; q < rd[r].n; q++) TEST_ASSERT_EQ_I(((int64_t*)ray_data(out))[q], rv[ids[q]]);
        ray_release(out);
    }
    ray_release(t0); ray_release(t2);
    PASS();
}

/* Encodings of every vector of column `col` of the parted table `t`
 * (segment-local descriptors): per encoding, how many. */
static void aw_parted_encs(ray_t* t, const char* col, int64_t cnt[8]) {
    memset(cnt, 0, 8 * sizeof(int64_t));
    ray_t* encv = ray_table_get_col_enc(t, ray_sym_intern(col, strlen(col)));
    if (!encv) return;
    for (int64_t sg = 0; ; sg++) {
        ray_t* seg = ray_enccol_seg(encv, sg);
        if (!seg) break;
        ray_enccol_t* e = ray_enccol_of(seg);
        if (!e || e->kind != RAY_ENCCOL_SINGLE) continue;
        for (int64_t v = 0; v * RAY_ENC_VEC < e->len; v++) {
            const ray_enc_desc_t* d = ray_enccol_descs(e, v / RAY_ENC_VECS_PER_CHUNK);
            int enc = d[v % RAY_ENC_VECS_PER_CHUNK].encoding;
            if (enc >= 0 && enc < 8) cnt[enc]++;
        }
    }
}

/* Task 10b: SYM ids point-read by the selection and row-list gathers of a
 * parted source.  Two partitions (3000 and 67000 rows, so segment vectors
 * straddle the global morsels): a in vector-wide constants, b in runs of 7
 * with every 13th run "" (the in-band null), c cycling over 300 symbols,
 * so CONST, RLE and packed vectors all occur.  A few rows per vector
 * (every 997th row and the rows at the partition boundary), sorted and
 * unsorted: nothing decoded (a RAW vector is read in place), and the
 * result equals generation 0's own gather and `at` (width, domain, nulls
 * and symbols). */
static test_result_t test_gather_sym_points(void) {
    static const int64_t np[2] = { 3000, 67000 };
    for (int gen = 0; gen <= 2; gen += 2)
        for (int p = 0; p < 2; p++) {
            char q[1600];
            snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/sy%d/2024.01.0%d/t/\" "
                     "(table [k a b c] (list (+ %lld (til %lld)) "
                     "(as 'SYMBOL (map (fn [j] (format \"c%%\" (div j 4096))) (til %lld))) "
                     "(as 'SYMBOL (map (fn [j] (if (== 0 (%% (div j 7) 13)) \"\" (format \"r%%\" (%% (div j 7) 97)))) (til %lld))) "
                     "(as 'SYMBOL (map (fn [j] (format \"w%%\" (%% (* j 7919) 300))) (til %lld))))) "
                     "\"" AW_TMP "/sy%d/.sym\" {compress: %s})", gen, p + 1, (long long)(p ? np[0] : 0),
                     (long long)np[p], (long long)np[p], (long long)np[p], (long long)np[p], gen, gen ? "true" : "false");
            aw_eval_ok(q);
        }
    ray_t* tp[2] = { aw_eval("(.db.parted.get \"" AW_TMP "/sy0\" 't)"), aw_eval("(.db.parted.get \"" AW_TMP "/sy2\" 't)") };
    TEST_ASSERT_TRUE(tp[0] && tp[1] && !RAY_IS_ERR(tp[0]) && !RAY_IS_ERR(tp[1]));
    int64_t ca[8], cb[8], cc[8];
    aw_parted_encs(tp[1], "a", ca); aw_parted_encs(tp[1], "b", cb); aw_parted_encs(tp[1], "c", cc);
    TEST_ASSERT_FMT(ca[RAY_ENC_CONST] > 0 && cb[RAY_ENC_RLE] > 0 && cc[RAY_ENC_FOR_BP] + cc[RAY_ENC_DICT] > 0,
                    "a CONST %lld, b RLE %lld, c FOR_BP %lld DICT %lld RAW %lld", (long long)ca[RAY_ENC_CONST],
                    (long long)cb[RAY_ENC_RLE], (long long)cc[RAY_ENC_FOR_BP], (long long)cc[RAY_ENC_DICT],
                    (long long)cc[RAY_ENC_RAW]);
    const int64_t n = np[0] + np[1];
    ray_t* pred = ray_vec_new(RAY_BOOL, n); pred->len = n;
    memset(ray_data(pred), 0, (size_t)n);
    int64_t ids[128], ni = 0;
    for (int64_t r = 0; r < n; r++)
        if (r % 997 == 5 || (r >= 2998 && r <= 3001) || r == n - 1) {
            ((uint8_t*)ray_data(pred))[r] = 1;
            if (ni < 128) ids[ni++] = r;
        }
    for (int64_t i = 0; i < ni / 2; i++) { int64_t x = ids[i]; ids[i] = ids[ni - 1 - i]; ids[ni - 1 - i] = x; }  /* unsorted */
    ray_t* sel = ray_rowsel_from_pred(pred);
    TEST_ASSERT_NOT_NULL(sel);
    static const char* const cols[] = { "a", "b", "c" };
    for (int c = 0; c < 3; c++) {
        ray_t* out[2][2];
        for (int g = 0; g < 2; g++) {
            ray_col_src_t s;
            TEST_ASSERT_NULL(ray_table_col_src(tp[g], ray_sym_intern(cols[c], 1), &s));
            ray_enc_counters_reset();
            TEST_ASSERT_NULL(ray_col_gather_sel(&s, sel, &out[g][0]));
            TEST_ASSERT_NULL(ray_col_gather_rows(&s, ids, ni, &out[g][1]));
            ray_col_src_release(&s);
            ray_enc_counters_t k; ray_enc_counters_get(&k);
            /* a RAW vector without nulls is read in place, its bytes
             * counted as delivered (a view, not a decode) */
            const bool raw = (c == 0 ? ca : c == 1 ? cb : cc)[RAY_ENC_RAW] > 0;
            if (g && !g_aw_unaligned)
                TEST_ASSERT_FMT((raw || k.decoded_bytes == 0) && k.materialized == 0, "%s: decoded %llu", cols[c],
                                (unsigned long long)k.decoded_bytes);
        }
        for (int w = 0; w < 2; w++) {
            char q[1600];
            if (w) {
                char lst[128 * 8] = "[";
                for (int64_t i = 0; i < ni; i++)
                    snprintf(lst + strlen(lst), sizeof lst - strlen(lst), "%s%lld", i ? " " : "", (long long)ids[i]);
                strcat(lst, "]");
                snprintf(q, sizeof q, "(at (at (.db.parted.get \"" AW_TMP "/sy0\" 't) '%s) %s)", cols[c], lst);
            } else {
                snprintf(q, sizeof q, "(at (at (.db.parted.get \"" AW_TMP "/sy0\" 't) '%s) (where (or (== 5 (%% (til %lld) 997)) "
                         "(and (>= (til %lld) 2998) (<= (til %lld) 3001)) (== (til %lld) %lld))))", cols[c],
                         (long long)n, (long long)n, (long long)n, (long long)n, (long long)(n - 1));
            }
            ray_t* ref = aw_eval(q);
            TEST_ASSERT_FMT(ref && !RAY_IS_ERR(ref), "%s: %s", q, ray_error_msg());
            AW_GATHER_REF(out[0][w], ref, false, cols[c]);
            AW_GATHER_REF(out[1][w], ref, false, cols[c]);
            ray_release(ref);
            test_result_t r = aw_vec_same(out[0][w], out[1][w], cols[c]);
            if (r.status != TEST_PASS) return r;
            ray_release(out[0][w]); ray_release(out[1][w]);
        }
    }
    ray_rowsel_release(sel); ray_release(pred);
    ray_release(tp[0]); ray_release(tp[1]);
    PASS();
}

/* Task 10b: HEAD(SORT) under a lazy selection over a parted table (late
 * materialization).  The keys are compacted and top-k'd, then only the
 * winners' rows of every column gathered: the answer is the full sort's
 * first rows (ties kept by row order, nulls where the sort puts them),
 * on both generations, and generation 2's equals generation 0's.  k has
 * ties and nulls, s is a SYM key (string order), f an F64 key with nulls,
 * w a STR key and payload, t the row number; one selection holds whole
 * segments (ALL) beside partial ones.  Takes at the selection's rows minus
 * one (late) and at or past them, an empty selection and a LIST column
 * keep the old route (counter unchanged). */
static test_result_t aw_late_same(const char* t0, const char* t2, const char* q, const char* ref, bool late) {
    uint64_t h0 = ray_sort_test_late_heads();
    AW_CSV_SAME(t0, t2, q, 0);
    uint64_t got = ray_sort_test_late_heads() - h0;
    TEST_ASSERT_FMT(got == (late ? 2u : 0u), "%s: %llu late heads", q, (unsigned long long)got);
    /* generation 0's answer is the full sort's first rows */
    char buf[600]; char* csv[2]; size_t len[2];
    snprintf(buf, sizeof buf, "(set T %s)", t0);
    aw_eval_ok(buf);
    for (int w = 0; w < 2; w++) {
        ray_t* r = ray_eval_str(w ? ref : q);
        TEST_ASSERT_FMT(r && !RAY_IS_ERR(r), "%s: %s", w ? ref : q, ray_error_msg());
        ray_env_set(ray_sym_intern("R", 1), r);
        ray_release(r);
        csv[w] = aw_csv("R", w ? "lr" : "lq", &len[w]);
    }
    bool same = csv[0] && csv[1] && len[0] == len[1] && memcmp(csv[0], csv[1], len[0]) == 0;
    free(csv[0]); free(csv[1]);
    aw_eval_ok("(set R 0)");
    TEST_ASSERT_FMT(same, "%s: differs from the full sort's first rows", q);
    PASS();
}
#define AW_LATE_SAME(a, b, q, r, l) do { test_result_t r_ = aw_late_same((a), (b), (q), (r), (l)); if (r_.status != TEST_PASS) return r_; } while (0)

static test_result_t test_sort_head_late(void) {
    static const int64_t np[2] = { 3000, 37000 };
    for (int gen = 0; gen <= 2; gen += 2)
        for (int p = 0; p < 2; p++) {
            char q[2400];
            snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/sl%d/2024.01.0%d/t/\" "
                     "(table [k v s t f w] (list (map (fn [j] (if (== 0 (%% j 101)) 0N (%% (* j 7919) 50))) (til %lld)) "
                     "(as 'I32 (%% (* (til %lld) 31) 1000)) "
                     "(as 'SYMBOL (map (fn [j] (format \"s%%\" (%% j 37))) (til %lld))) (+ %lld (til %lld)) "
                     "(map (fn [j] (if (== 0 (%% j 89)) 0Nf (* 0.5 (%% (* j 13) 400)))) (til %lld)) "
                     "(map (fn [j] (if (== 0 (%% j 71)) \"\" (format \"a pooled string %%\" (%% (* j 17) 53)))) (til %lld)))) "
                     "\"" AW_TMP "/sl%d/.sym\" {compress: %s})", gen, p + 1, (long long)np[p], (long long)np[p],
                     (long long)np[p], (long long)(p ? np[0] : 0), (long long)np[p], (long long)np[p], (long long)np[p],
                     gen, gen ? "true" : "false");
            aw_eval_ok(q);
        }
    const char* t0 = "(.db.parted.get \"" AW_TMP "/sl0\" 't)";
    const char* t2 = "(.db.parted.get \"" AW_TMP "/sl2\" 't)";
    static const struct { const char* q; const char* ref; bool late; } qs[] = {
        { "(select {from: T where: (> v 900) asc: k take: 25})",
          "(take (select {from: T where: (> v 900) asc: k}) 25)", true },
        { "(select {from: T where: (> v 900) desc: k take: 25})",
          "(take (select {from: T where: (> v 900) desc: k}) 25)", true },
        { "(select {from: T where: (< v 500) asc: [k s] take: 40})",
          "(take (select {from: T where: (< v 500) asc: [k s]}) 40)", true },
        { "(select {from: T where: (== (% v 7) 3) desc: s take: 12})",
          "(take (select {from: T where: (== (% v 7) 3) desc: s}) 12)", true },
        { "(select {from: T where: (> v 700) asc: w take: 15})",           /* a STR key */
          "(take (select {from: T where: (> v 700) asc: w}) 15)", true },
        { "(select {from: T where: (< v 300) desc: f take: 9})",           /* an F64 key with nulls */
          "(take (select {from: T where: (< v 300) desc: f}) 9)", true },
        { "(select {from: T where: (< v 300) asc: [f w] take: 30})",
          "(take (select {from: T where: (< v 300) asc: [f w]}) 30)", true },
        { "(select {from: T where: (or (< t 2048) (== 0 (% v 3))) asc: t take: 2100})",
          "(take (select {from: T where: (or (< t 2048) (== 0 (% v 3))) asc: t}) 2100)", true },
        { "(select {from: T where: (or (< t 2048) (== 0 (% v 3))) desc: v take: 50})",   /* part of the ALL segments */
          "(take (select {from: T where: (or (< t 2048) (== 0 (% v 3))) desc: v}) 50)", true },
        { "(select {from: T where: (> v 998) asc: k take: 5000})",
          "(select {from: T where: (> v 998) asc: k})", false },          /* a longer take would cycle */
        { "(select {from: T where: (> v 5000) asc: k take: 5})",          /* an empty selection */
          "(select {from: T where: (> v 5000) asc: k})", false },
    };
    for (size_t i = 0; i < sizeof qs / sizeof *qs; i++) AW_LATE_SAME(t0, t2, qs[i].q, qs[i].ref, qs[i].late);
    /* K at the selection's rows minus one (the late route) and at them (the
     * old route: the shortcut needs K below the rows) */
    aw_eval_ok("(set T (.db.parted.get \"" AW_TMP "/sl0\" 't))");
    ray_t* cnt = ray_eval_str("(count (select {from: T where: (> v 990)}))");
    TEST_ASSERT_TRUE(cnt && !RAY_IS_ERR(cnt) && cnt->i64 > 2);
    const int64_t pass = cnt->i64;
    ray_release(cnt);
    for (int e = 0; e < 2; e++) {
        char q[300], ref[300];
        snprintf(q, sizeof q, "(select {from: T where: (> v 990) desc: k take: %lld})", (long long)(pass - 1 + e));
        if (e) snprintf(ref, sizeof ref, "(select {from: T where: (> v 990) desc: k})");
        else   snprintf(ref, sizeof ref, "(take (select {from: T where: (> v 990) desc: k}) %lld)", (long long)(pass - 1));
        AW_LATE_SAME(t0, t2, q, ref, e == 0);
    }
    /* a LIST column: ray_topk_table declines, so the late route does too
     * (generation 0: LIST stays plain in either store) */
    for (int p = 0; p < 2; p++) {
        char q[600];
        snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/sll/2024.01.0%d/t/\" "
                 "(table [k v l] (list (%% (* (til 3000) 7919) 50) (%% (til 3000) 10) (map (fn [j] (list j)) (til 3000)))))",
                 p + 1);
        aw_eval_ok(q);
    }
    aw_eval_ok("(set T (.db.parted.get \"" AW_TMP "/sll\" 't))");
    {
        uint64_t h0 = ray_sort_test_late_heads();
        ray_t* a = ray_eval_str("(select {from: T where: (> v 5) asc: k take: 7})");
        ray_t* r = ray_eval_str("(take (select {from: T where: (> v 5) asc: k}) 7)");
        TEST_ASSERT_FMT(a && r && !RAY_IS_ERR(a) && !RAY_IS_ERR(r), "LIST: %s", ray_error_msg());
        TEST_ASSERT_EQ_U(ray_sort_test_late_heads(), h0);
        ray_t* fa = ray_fmt(a, 0); ray_t* fr = ray_fmt(r, 0);
        TEST_ASSERT_FMT(ray_str_len(fa) == ray_str_len(fr) && memcmp(ray_str_ptr(fa), ray_str_ptr(fr), ray_str_len(fa)) == 0,
                        "LIST: differs from the full sort's first rows");
        ray_release(fa); ray_release(fr); ray_release(a); ray_release(r);
    }
    PASS();
}

/* First-n on runs: keys whose vectors are run views (h: runs of 11; s: a
 * SYM in runs of 7, every 13th run "", the in-band null) are probed once
 * per span of equal tuples, without decoding (agg_firstn_runs).  h decodes
 * only the vector pass 1 reads through its cursor; the sample point-reads
 * it and pass 2 reads its runs.  With a where: the table is compacted
 * first (firstn_sel installs a selection itself); answers and order are
 * generation 0's. */
static test_result_t test_firstn_runs(void) {
    aw_save("fr", "(table [h s] (list (% (* (div (til 300000) 11) 7919) 100003) "
                  "(as 'SYMBOL (map (fn [j] (if (== 0 (% (div j 7) 13)) \"\" (format \"r%\" (% (div j 7) 97)))) (til 300000)))))",
            RAY_ENC_RLE);
    for (int64_t v = 0; v < 290; v += 41) {
        TEST_ASSERT_EQ_I(aw_vec_enc("fr", "h", v), RAY_ENC_RLE);
        TEST_ASSERT_EQ_I(aw_vec_enc("fr", "s", v), RAY_ENC_RLE);
    }
    static const char* const qs[] = {
        "(select {c: (count h) from: T by: {h: h s: s} take: 10})",
        "(select {c: (count h) m: (max h) from: T where: (!= s 'r5) by: {s: s h: h} take: 7})",
        NULL,
    };
    for (int i = 0; qs[i]; i++) {
        agg_route_reset();
        AW_SAME("fr", qs[i], 0);
        TEST_ASSERT_FMT(agg_route_stats().first_n == 2, "%s: first-n %llu", qs[i], (unsigned long long)agg_route_stats().first_n);
        if (i == 0)   /* the where: compacts the table first (HEAD(GROUP) over a selection): all of h gathered */
            TEST_ASSERT_FMT(aw_col_decoded("h") == 1024 * 8, "%s: h decoded %llu", qs[i], (unsigned long long)aw_col_decoded("h"));
    }
    PASS();
}

/* The parallel strategies' selection reads (agg_idx_walk) with few rows
 * per vector: every 40th row of 3M (25 or 26 per vector, under both
 * codecs' D2 limits; above the parallel threshold) grouped by two I32
 * keys read at those rows by point reads into the walk's sparse views,
 * by the table-stack sampler and by the dense plan's bounds: neither key
 * decodes a vector. */
static test_result_t test_sparse_walk_points(void) {
    aw_save("sw", "(table [a b v w] (list (as 'I32 (% (* (div (til 3000000) 7) 7919) 1000)) (as 'I32 (% (div (til 3000000) 50000) 7)) "
                  "(til 3000000) (% (* (til 3000000) 7919) 3000017)))", -1);
    for (int64_t v = 0; v < 2900; v += 397) {
        int ea = aw_vec_enc("sw", "a", v), eb = aw_vec_enc("sw", "b", v);
        TEST_ASSERT_FMT(ea == RAY_ENC_RLE || ea == RAY_ENC_FOR_BP, "a vector %lld: %d", (long long)v, ea);
        TEST_ASSERT_FMT(eb == RAY_ENC_RLE || eb == RAY_ENC_CONST, "b vector %lld: %d", (long long)v, eb);
    }
    static const char* const qs[] = {
        "(select {n: (count a) from: T where: (== 0 (% v 40)) by: {a: a b: b} desc: n take: 10})",
        "(select {n: (count a) s: (sum v) from: T where: (== 0 (% v 40)) by: a})",
        NULL,
    };
    for (int i = 0; qs[i]; i++) {
        AW_SAME("sw", qs[i], 0);
        TEST_ASSERT_FMT(aw_col_decoded("a") == 0 && aw_col_decoded("b") == 0, "%s: a %llu b %llu", qs[i],
                        (unsigned long long)aw_col_decoded("a"), (unsigned long long)aw_col_decoded("b"));
    }
    /* the dense plan's row probe (agg_sel_bounds_impossible): by {a w},
     * w spread (FOR_BP, point-read), so 64 probed rows already span more
     * slots than the 75000 selected rows; the plan is declined before any
     * bounds scan, the answer is generation 0's, and the plan of {a b}
     * (it fits) is not */
    uint64_t p0 = ray_agg_test_sel_probe_hits();
    AW_SAME("sw", "(select {n: (count a) from: T where: (== 0 (% v 40)) by: {a: a w: w} desc: n take: 5})", 0);
    TEST_ASSERT_FMT(ray_agg_test_sel_probe_hits() == p0 + 1, "probe hits %llu", (unsigned long long)(ray_agg_test_sel_probe_hits() - p0));
    p0 = ray_agg_test_sel_probe_hits();
    AW_SAME("sw", qs[0], 0);
    TEST_ASSERT_EQ_U(ray_agg_test_sel_probe_hits(), p0);
    PASS();
}

/* A SYM group key read like ClickBench q13 (P3 Task 14b).  Its rows are
 * 600000 (ten chunks): the selected third (f == 0) holds 100 bulk symbols
 * and, once each, the symbol interned first (row 0) and the last selected
 * row's symbol, interned after 400000 unselected ones.  The exact id range
 * of the selected rows exceeds their count, so the dense plan is
 * impossible; 64 evenly spaced selected rows span only the bulk (and row
 * 0), so the row probe alone is undecided, and the chunks holding the
 * directory's lowest minimum and highest maximum (two of ten) decide it:
 * no bounds scan of every selected row.  The answer (a tied top 5: emit
 * order) is generation 0's; nothing is materialized. */
static test_result_t test_sym_key_chunk_extremes(void) {
    aw_save("pq", "(table [s f] (list (as 'SYMBOL (map (fn [j] (if (== j 0) \"plo\" (if (== j 599997) \"phi\" "
                  "(if (== 0 (% j 3)) (format \"b%\" (% j 100)) (format \"u%\" j))))) (til 600000))) "
                  "(as 'I32 (% (til 600000) 3))))", -1);
    const char* q = "(select {c: (count s) from: T where: (== f 0) by: s desc: c take: 5})";
    uint64_t p0 = ray_agg_test_sel_probe_hits();
    AW_SAME("pq", q, 0);
    TEST_ASSERT_FMT(ray_agg_test_sel_probe_hits() == p0 + 1, "probe hits %llu", (unsigned long long)(ray_agg_test_sel_probe_hits() - p0));
    /* without the two extremes selected (f == 1: u symbols only, ids
     * within the selected count of each other) the plan is not proved
     * impossible, and the answer still agrees */
    p0 = ray_agg_test_sel_probe_hits();
    AW_SAME("pq", "(select {c: (count s) from: T where: (and (== f 0) (!= s 'plo) (!= s 'phi)) by: s desc: c take: 5})", 0);
    TEST_ASSERT_EQ_U(ray_agg_test_sel_probe_hits(), p0);
    PASS();
}

/* SYM keys read by point reads (agg_src_points: ids through
 * ray_col_src_gather_points) in the selection walk, the table-stack
 * samplers and the dense plan's probe: every 20th of 1.5M rows (51 per
 * vector, under the RLE limit), a SYM key on runs of 7 and an I32 one.
 * The SYM key decodes no vector; the answers are generation 0's. */
static test_result_t test_sym_key_points(void) {
    aw_save("sp", "(table [k b v] (list (at (as 'SYMBOL (map (fn [x] (format \"k%\" x)) (til 1000))) "
                  "(% (div (til 1500000) 7) 1000)) (as 'I32 (% (div (til 1500000) 50000) 7)) (til 1500000)))", -1);
    for (int64_t v = 0; v < 1400; v += 197)
        TEST_ASSERT_FMT(aw_vec_enc("sp", "k", v) == RAY_ENC_RLE, "k vector %lld: %d", (long long)v, aw_vec_enc("sp", "k", v));
    static const char* const qs[] = {
        "(select {n: (count k) from: T where: (== 0 (% v 20)) by: {k: k b: b} desc: n take: 10})",
        "(select {n: (count k) s: (sum v) from: T where: (== 0 (% v 20)) by: k})",
        NULL,
    };
    for (int i = 0; qs[i]; i++) {
        AW_SAME("sp", qs[i], 0);
        TEST_ASSERT_FMT(aw_col_decoded("k") == 0, "%s: k decoded %llu", qs[i], (unsigned long long)aw_col_decoded("k"));
    }
    PASS();
}

/* A selection whose program is one encoded leaf is emitted from the
 * vector's view (expr_leaf_emit): an RLE vector from its runs, a CONST
 * one as a uniform segment, any other through the evaluator's bools.
 * The selection is byte-for-byte generation 0's under every operator,
 * for an I32 column on runs of 3 and a SYM one alternating "" runs with
 * symbols (both forced RLE); a nullable RLE column (a bitmap) keeps the
 * bools. */
static ray_op_t* ap_x_lt500(ray_graph_t* g) { return ray_lt(g, ray_scan(g, "x"), ray_const_i64(g, 500)); }
static test_result_t test_leaf_emit_runs(void) {
    aw_save("lr", "(table [x s] (list (as 'I32 (div (% (til 300000) 3000) 3)) "
                  "(as 'SYMBOL (map (fn [j] (if (< (% j 10) 6) \"\" (format \"w%\" (% (div j 10) 50)))) (til 300000)))))", RAY_ENC_RLE);
    TEST_ASSERT_EQ_I(aw_vec_enc("lr", "x", 5), RAY_ENC_RLE);
    TEST_ASSERT_EQ_I(aw_vec_enc("lr", "s", 5), RAY_ENC_RLE);
    ray_op_t* (*const ps[])(ray_graph_t*) = { ap_x_eq7, ap_x_gt10, ap_x_lt500, ap_s_ne_empty, ap_s_eq_empty, ap_s_eq, NULL };
    for (int i = 0; ps[i]; i++) {
        int64_t r0 = ray_expr_test_leaf_runs();
        test_result_t r = aw_sel_same("lr", ps[i], -1);
        if (r.status != TEST_PASS) return r;
        TEST_ASSERT_FMT(ray_expr_test_leaf_runs() > r0, "predicate %d: no segment from runs", i);
    }
    aw_save("ln", "(table [x] (list (as 'I32 (map (fn [j] (if (== 0 (% j 50)) 0N (div (% j 3000) 3))) (til 300000)))))", RAY_ENC_RLE);
    TEST_ASSERT_EQ_I(aw_vec_enc("ln", "x", 5), RAY_ENC_RLE);
    ray_op_t* (*const pn[])(ray_graph_t*) = { ap_x_eq7, ap_x_gt10, ap_x_lt500, NULL };
    for (int i = 0; pn[i]; i++) {
        int64_t r0 = ray_expr_test_leaf_runs();
        test_result_t r = aw_sel_same("ln", pn[i], -1);
        if (r.status != TEST_PASS) return r;
        TEST_ASSERT_EQ_I(ray_expr_test_leaf_runs(), r0);     /* a bitmap: the bools */
    }
    PASS();
}

/* The direct-array prescan of a staged key that cannot fit (ClickBench
 * q29: by a wide SYM key beside avg (strlen key), the group.c route) stops
 * as the plain scan does, once a worker's bounds exceed the slot budget:
 * 900000 rows, the first 300000 interning 300000 symbols in row order
 * (unselected), the selected 600000 spreading their ids over all of them
 * in every vector.  The walk reads eight vectors at a time and checks the
 * worker's bounds after each piece, so it stops within a few vectors per
 * worker, not after every selected row.  The answer is generation 0's. */
static test_result_t test_da_prescan_abort_staged(void) {
    aw_save("dp", "(table [s f] (list (at (as 'SYMBOL (map (fn [x] (format \"r%\" x)) (til 300000))) "
                  "(raze (list (til 300000) (% (* (til 600000) 7919) 300000)))) (til 900000)))", -1);
    const char* q = "(select {n: (count l) t: (sum l) m: (max c) from: (select {l: (avg (strlen s)) c: (count s) "
                    "from: T where: (>= f 300000) by: s})})";
    int64_t r0 = ray_group_test_minmax_staged_rows();
    AW_SAME("dp", q, 0);
    int64_t dr = ray_group_test_minmax_staged_rows() - r0;
    TEST_ASSERT_FMT(dr > 0 && dr <= 150000, "staged prescan rows %lld", (long long)dr);
    PASS();
}

/* The first-n passes under a selection (the language compacts a where:
 * before an unordered take, so a C caller installs the rowsel): every
 * third row dropped (MIX segments), keys on runs (agg_firstn_runs) and,
 * for a key set over the RLE keys plus a FOR_BP one, through the cursor
 * pieces (agg_firstn_piece).  Results and order equal generation 0's;
 * nothing is materialized. */
static ray_t* aw_firstn_sel_run(ray_t* t, int with_v, int64_t limit) {
    ray_graph_t* g = ray_graph_new(t);
    ray_t* pred = ray_vec_new(RAY_BOOL, 300000); pred->len = 300000;
    for (int64_t r = 0; r < 300000; r++) ((uint8_t*)ray_data(pred))[r] = r % 3 != 0;
    g->selection = ray_rowsel_from_pred(pred);
    ray_release(pred);
    ray_op_t* keys[3] = { ray_scan(g, "h"), ray_scan(g, "s"), with_v ? ray_scan(g, "v") : NULL };
    ray_op_t* ins[2] = { ray_scan(g, "h"), ray_scan(g, "h") };
    uint16_t ops[2] = { OP_COUNT, OP_MAX };
    ray_op_t* grp = ray_group(g, keys, with_v ? 3 : 2, ops, ins, 2);
    ray_t* r = exec_group(g, grp, t, limit);
    ray_graph_free(g);
    return r;
}
static test_result_t test_firstn_sel(void) {
    aw_save("fs", "(table [h s v] (list (% (* (div (til 300000) 11) 7919) 100003) "
                  "(as 'SYMBOL (map (fn [j] (if (== 0 (% (div j 7) 13)) \"\" (format \"r%\" (% (div j 7) 97)))) (til 300000))) "
                  "(% (div (til 300000) 3) 5)))", RAY_ENC_RLE);
    ray_t* t[2] = { aw_load("fs", 0), aw_load("fs", 2) };
    TEST_ASSERT_TRUE(t[0] && t[1]);
    for (int with_v = 0; with_v < 2; with_v++)
        for (int64_t limit = 5; limit <= 40; limit += 35) {
            ray_t* out[2];
            for (int gen = 0; gen < 2; gen++) {
                ray_enc_counters_reset();
                agg_route_reset();
                out[gen] = aw_firstn_sel_run(t[gen], with_v, limit);
                TEST_ASSERT_FMT(out[gen] && !RAY_IS_ERR(out[gen]), "gen %d v %d: %s", gen, with_v, ray_error_msg());
                TEST_ASSERT_FMT(agg_route_stats().first_n == 1, "gen %d v %d: not first-n", gen, with_v);
                ray_enc_counters_t k; ray_enc_counters_get(&k);
                if (gen) TEST_ASSERT_EQ_U(k.materialized, 0);
            }
            TEST_ASSERT_EQ_I(ray_table_nrows(out[0]), limit);
            ray_t* fa = ray_fmt(out[0], 0); ray_t* fb = ray_fmt(out[1], 0);
            TEST_ASSERT_FMT(ray_str_len(fa) == ray_str_len(fb) && memcmp(ray_str_ptr(fa), ray_str_ptr(fb), ray_str_len(fa)) == 0,
                            "v %d limit %lld differs", with_v, (long long)limit);
            ray_release(fa); ray_release(fb); ray_release(out[0]); ray_release(out[1]);
        }
    ray_release(t[0]); ray_release(t[1]);
    PASS();
}

/* q42's filter: a chained where: over generation-2 predicate columns is
 * refined on the segments the selection keeps, through the encoded leaves
 * (exec_pred_refine_selection), so no predicate value is gathered: p,
 * r and the nullable q (every vector RLE) decode nothing; answers equal
 * generation 0's, nulls failing their compare. */
static test_result_t test_refine_on_views(void) {
    aw_save("rv", "(table [c p r q g v] (list (div (til 300000) 3000) (% (* (til 300000) 7) 13) (div (til 300000) 300) "
                  "(map (fn [j] (if (== 0 (% j 11)) 0N (% j 6))) (til 300000)) (% (til 300000) 5) (til 300000)))", RAY_ENC_RLE);
    static const char* const qs[] = {
        "(select {n: (count v) s: (sum v) from: T where: (and (== c 62) (== p 3)) by: g})",
        "(select {n: (count v) from: T where: (and (>= c 40) (<= c 41) (> r 405) (< p 9)) by: g})",
        "(select {n: (count v) from: T where: (and (== c 7) (> q 2)) by: g})",
        "(select {n: (count v) from: T where: (and (== c 7) (== p 100)) by: g})",
        NULL,
    };
    for (int64_t v = 0; v < 290; v += 37) {
        TEST_ASSERT_EQ_I(aw_vec_enc("rv", "r", v), RAY_ENC_RLE);
        TEST_ASSERT_EQ_I(aw_vec_enc("rv", "p", v), RAY_ENC_RLE);
    }
    for (int i = 0; qs[i]; i++) {
        AW_SAME("rv", qs[i], 0);
        TEST_ASSERT_FMT(aw_col_decoded("p") == 0 && aw_col_decoded("r") == 0 && aw_col_decoded("q") == 0,
                        "%s: p %llu r %llu q %llu", qs[i], (unsigned long long)aw_col_decoded("p"),
                        (unsigned long long)aw_col_decoded("r"), (unsigned long long)aw_col_decoded("q"));
    }
    PASS();
}

/* Equality on runs: answered on the vectors' run views, a run scan
 * deciding a vector whose runs never hold the constant without per-run
 * fills (enceval's runs_hold).  I64 runs with RAW values (h: spread
 * hashes) and I16 runs with packed ones (s); the constant held in some
 * vectors, in none, outside the I16 range; == and !=, run twice.  Answers
 * equal generation 0's and nothing decodes. */
static test_result_t test_eq_runs_lack(void) {
    aw_save("rl", "(table [h s v] (list (* 1000000007 (% (* (div (til 70000) 3) 7919) 100003)) "
                  "(as 'I16 (% (div (til 70000) 5) 300)) (til 70000)))", RAY_ENC_RLE);
    TEST_ASSERT_EQ_I(aw_vec_enc("rl", "h", 3), RAY_ENC_RLE);
    TEST_ASSERT_EQ_I(aw_vec_enc("rl", "s", 3), RAY_ENC_RLE);
    static const char* const qs[] = {
        "(select {n: (count v) s: (sum v) from: T where: (== h 7919000055433)})",      /* j div 3 = 1 */
        "(select {n: (count v) from: T where: (== h 5)})",
        "(select {n: (count v) s: (sum v) from: T where: (!= h 7919000055433)})",
        "(select {n: (count v) s: (sum v) from: T where: (== s 17)})",
        "(select {n: (count v) from: T where: (== s 100000)})",
        "(select {n: (count v) from: T where: (!= s 100000)})",
        NULL,
    };
    for (int rep = 0; rep < 2; rep++)
        for (int i = 0; qs[i]; i++) {
            AW_SAME("rl", qs[i], 0);
            TEST_ASSERT_FMT(g_aw_unaligned || (aw_col_decoded("h") == 0 && aw_col_decoded("s") == 0), "%s: h %llu s %llu", qs[i],
                            (unsigned long long)aw_col_decoded("h"), (unsigned long long)aw_col_decoded("s"));
        }
    PASS();
}

/* Review focus 5: sums at the I64 extremes with nulls, from the directory,
 * on a column without a recorded CHUNK_ZONE (the C3 gate does not cover it). */
static test_result_t test_sum_extremes(void) {
    /* the converter records a CHUNK_ZONE for a plain column of this size; a
     * plain ZONE already on it stands in, so the column has none to answer from */
    aw_save("sx", "(table [x] (list (.idx.zone (as 'I64 (map (fn [j] (if (== 0 (% j 7)) 0N (if (== 0 (% j 2)) 9223372036854775807 -9223372036854775807))) (til 200000))))))", -1);
    test_result_t r;
    if ((r = aw_reduce("sx", "(sum (at T 'x))", 0)).status != TEST_PASS) return r;
    ray_t* ix = ray_eval_str(ray_test_tmp("(at (.idx.info T 'x) 'index)"));
    TEST_ASSERT_TRUE(ix && !RAY_IS_ERR(ix) && ix->type == -RAY_SYM &&
                     ix->i64 == ray_sym_intern("zone", 4));   /* not chunk_zone: no recorded CHUNK_ZONE */
    ray_release(ix);
    if ((r = aw_reduce("sx", "(avg (at T 'x))", 0)).status != TEST_PASS) return r;
    if ((r = aw_reduce("sx", "(count (at T 'x))", 0)).status != TEST_PASS) return r;
    if ((r = aw_reduce("sx", "(min (at T 'x))", 0)).status != TEST_PASS) return r;
    if ((r = aw_reduce("sx", "(max (at T 'x))", 0)).status != TEST_PASS) return r;
    /* a total beyond INT64_MAX: the wrapped low word for sum, the exact 128-bit mean for avg */
    aw_save("sw", "(table [x] (list (.idx.zone (as 'I64 (map (fn [j] (if (== 0 (% j 5)) 0N 9223372036854775807)) (til 200000))))))", -1);
    static const char* const swag[] = { "sum", "avg", "min", "max" };
    for (int i = 0; i < 4; i++) {
        char q[80]; snprintf(q, sizeof q, "(%s (at T 'x))", swag[i]);
        if ((r = aw_reduce("sw", q, 0)).status != TEST_PASS) return r;
    }
    /* planning-time answers keep generation 0's CHUNK_ZONE gate (D5): this one decodes, equal */
    if ((r = aw_reduce("sx", "(select {s: (sum x) a: (avg x) mx: (max x) from: T})", -1)).status != TEST_PASS) return r;
    PASS();
}

/* Parted: two generation-2 partitions and one generation-0 partition. */
static test_result_t test_sum_parted_dir(void) {
    for (int p = 0; p < 3; p++) {
        char q[600];
        snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/sp/2024.01.0%d/t/\" "
                 "(table [x y] (list (as 'I32 (map (fn [j] (if (== 0 (%% j 11)) 0N (- j 50000))) (til 90000))) "
                 "(map (fn [j] (if (== 0 (%% j 11)) 0N (- 9223372036854775807 (%% j 3)))) (til 90000)))) {compress: %s})",
                 p + 1, p == 1 ? "false" : "true");
        aw_eval_ok(q);
        snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/sq/2024.01.0%d/t/\" "
                 "(table [x y] (list (as 'I32 (map (fn [j] (if (== 0 (%% j 11)) 0N (- j 50000))) (til 90000))) "
                 "(map (fn [j] (if (== 0 (%% j 11)) 0N (- 9223372036854775807 (%% j 3)))) (til 90000)))) {compress: false})", p + 1);
        aw_eval_ok(q);
    }
    static const char* const aggs[] = { "sum", "avg", "min", "max", "count", NULL };
    for (int c = 0; c < 2; c++)
    for (int i = 0; aggs[i]; i++) {
        char a[300], b[300];
        snprintf(a, sizeof a, "(%s (at (.db.parted.get \"" AW_TMP "/sq\" 't) '%c))", aggs[i], c ? 'y' : 'x');
        snprintf(b, sizeof b, "(%s (at (.db.parted.get \"" AW_TMP "/sp\" 't) '%c))", aggs[i], c ? 'y' : 'x');
        ray_t* x = aw_eval(a);
        ray_enc_counters_reset();
        ray_t* y = aw_eval(b);
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        TEST_ASSERT_TRUE(x && y && !RAY_IS_ERR(x) && !RAY_IS_ERR(y));
        ray_t* fx = ray_fmt(x, 0); ray_t* fy = ray_fmt(y, 0);
        TEST_ASSERT_FMT(ray_str_len(fx) == ray_str_len(fy) && memcmp(ray_str_ptr(fx), ray_str_ptr(fy), ray_str_len(fx)) == 0, "%s", aggs[i]);
        TEST_ASSERT_EQ_U(k.materialized, 0);
        TEST_ASSERT_EQ_U(k.decoded_bytes, 0);   /* the generation-0 partition is read in place */
        ray_release(fx); ray_release(fy); ray_release(x); ray_release(y);
    }
    PASS();
}

/* A ragged parted table: partition 2 lacks the column (a synthetic stand-in),
 * 1 and 3 are generation 2, 4 generation 0.  Results equal the
 * all-generation-0 table's.  A nullable stand-in is all null and decodes
 * nothing; a BOOL / U8 one is non-null zeros (ray_null_fill), so min is 0
 * and avg counts its rows, and the select form agrees with the scalar one. */
static test_result_t test_sum_parted_ragged(void) {
    static const struct { const char* name; const char* col; const char* zero; bool nullable; } ty[] = {
        { "i64", "(map (fn [j] (if (== 0 (% j 13)) 0N (- j 70000))) (til 80000))", NULL, true },
        { "u8",  "(as 'U8 (+ 1 (% (til 80000) 200)))", "(as 'U8 0)", false },
        { "bool", "(>= (til 80000) 0)", "(== 1 0)", false },
    };
    static const char* roots[] = { "rw", "rn" };
    static const char* const aggs[] = { "sum", "avg", "min", "max", NULL };
    for (int t = 0; t < 3; t++) {
        (void)ray_test_rm_rf(aw_root()); (void)ray_test_mkdir_p(aw_root());
        for (int r = 0; r < 2; r++)
            for (int p = 0; p < 4; p++) {
                char q[900];
                snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/%s/2024.01.0%d/t/\" %s%s%s {compress: %s})",
                         roots[r], p + 1,
                         p == 1 ? "(table [y] (list (til 3000)))" : "(table [y x] (list (til 80000) ",
                         p == 1 ? "" : ty[t].col, p == 1 ? "" : "))",
                         r == 0 && p != 3 ? "true" : "false");
                aw_eval_ok(q);
            }
        aw_eval_ok("(set TW (.db.parted.get \"" AW_TMP "/rw\" 't))");
        aw_eval_ok("(set TN (.db.parted.get \"" AW_TMP "/rn\" 't))");
        for (int i = 0; aggs[i]; i++) {
            char a[300], b[300], c[300];
            snprintf(a, sizeof a, "(%s (at TN 'x))", aggs[i]);
            snprintf(b, sizeof b, "(%s (at TW 'x))", aggs[i]);
            snprintf(c, sizeof c, "(first (at (select {m: (%s x) from: TW}) 'm))", aggs[i]);
            ray_t* x = aw_eval(a);
            ray_enc_counters_reset();
            ray_t* y = aw_eval(b);
            ray_enc_counters_t k; ray_enc_counters_get(&k);
            ray_t* z = aw_eval(c);
            TEST_ASSERT_TRUE(x && y && z && !RAY_IS_ERR(x) && !RAY_IS_ERR(y) && !RAY_IS_ERR(z));
            ray_t* fx = ray_fmt(x, 0); ray_t* fy = ray_fmt(y, 0); ray_t* fz = ray_fmt(z, 0);
            TEST_ASSERT_FMT(ray_str_len(fx) == ray_str_len(fy) && memcmp(ray_str_ptr(fx), ray_str_ptr(fy), ray_str_len(fx)) == 0,
                            "%s %s: gen 0 vs mixed", ty[t].name, aggs[i]);
            TEST_ASSERT_FMT(ray_str_len(fz) == ray_str_len(fy) && memcmp(ray_str_ptr(fz), ray_str_ptr(fy), ray_str_len(fz)) == 0,
                            "%s %s: select vs scalar", ty[t].name, aggs[i]);
            TEST_ASSERT_EQ_U(k.materialized, 0);
            if (ty[t].nullable) TEST_ASSERT_EQ_U(k.decoded_bytes, 0);
            if (ty[t].zero && i == 2) {                      /* min: the stand-in's zero wins */
                ray_t* zr = aw_eval(ty[t].zero);
                ray_t* fr = ray_fmt(zr, 0);
                TEST_ASSERT_FMT(ray_str_len(fr) == ray_str_len(fy) && memcmp(ray_str_ptr(fr), ray_str_ptr(fy), ray_str_len(fr)) == 0,
                                "%s min is the stand-in's zero", ty[t].name);
                ray_release(fr); ray_release(zr);
            }
            ray_release(fx); ray_release(fy); ray_release(fz); ray_release(x); ray_release(y); ray_release(z);
        }
    }
    PASS();
}

/* `q` on <name>0 and <name>2: the result atoms' f64 bits are equal (ray_fmt
 * clears the sign of zero, so comparing text pins nothing), no
 * materialization, and want_decoded bytes decoded by generation 2 (-1: not
 * checked). */
static test_result_t aw_f64_bits(const char* name, const char* q, int64_t want_decoded) {
    ray_t* out[2];
    uint64_t dec = 0;
    for (int gen = 0; gen < 2; gen++) {
        char buf[512];
        snprintf(buf, sizeof buf, "(set T (.db.splayed.get \"" AW_TMP "/%s%d/\"))", name, gen ? 2 : 0);
        aw_eval_ok(buf);
        ray_enc_counters_reset();
        out[gen] = ray_eval_str(ray_test_tmp(q));
        TEST_ASSERT_FMT(out[gen] && !RAY_IS_ERR(out[gen]) && out[gen]->type == -RAY_F64, "%s gen %d", q, gen);
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        if (gen) { dec = k.decoded_bytes; TEST_ASSERT_EQ_U(k.materialized, 0); }
    }
    TEST_ASSERT_FMT(memcmp(&out[0]->f64, &out[1]->f64, sizeof(double)) == 0, "%s: f64 bits differ (%g vs %g)", q,
                    out[0]->f64, out[1]->f64);
    if (want_decoded >= 0) TEST_ASSERT_FMT(dec == (uint64_t)want_decoded, "%s: decoded %llu, want %lld", q,
                                           (unsigned long long)dec, (long long)want_decoded);
    ray_release(out[0]); ray_release(out[1]);
    PASS();
}

/* Float sums decode and stay bit-identical, -0.0 included; float min / max
 * come from the directory (D4: decoded 0) with the same bits. */
static test_result_t test_float_sum_decoded(void) {
    test_result_t r;
    aw_save("fs", "(table [f] (list (map (fn [j] (if (< j 1024) -0.0 (* 0.1 j))) (til 3000))))", -1);
    if ((r = aw_f64_bits("fs", "(sum (at T 'f))", -1)).status != TEST_PASS) return r;
    if ((r = aw_f64_bits("fs", "(avg (at T 'f))", -1)).status != TEST_PASS) return r;
    aw_save("fz", "(table [f] (list (take [-0.0] 2048)))", -1);
    if ((r = aw_f64_bits("fz", "(sum (at T 'f))", -1)).status != TEST_PASS) return r;   /* the sign of zero */
    {   /* the literal is a negative zero (the stored value and the D4 min carry the
         * sign), so the bit compares pin the sign; the sum is +0.0 on both
         * generations: the kernel starts from +0.0 and +0.0 + -0.0 is +0.0 */
        ray_t* z0 = ray_eval_str("(first (at T 'f))");
        ray_t* zm = ray_eval_str("(min (at T 'f))");
        ray_t* zs = ray_eval_str("(sum (at T 'f))");
        TEST_ASSERT_TRUE(z0 && !RAY_IS_ERR(z0) && z0->type == -RAY_F64 && z0->f64 == 0.0 && signbit(z0->f64));
        TEST_ASSERT_TRUE(zm && !RAY_IS_ERR(zm) && zm->type == -RAY_F64 && zm->f64 == 0.0 && signbit(zm->f64));
        TEST_ASSERT_TRUE(zs && !RAY_IS_ERR(zs) && zs->type == -RAY_F64 && zs->f64 == 0.0 && !signbit(zs->f64));
        ray_release(z0); ray_release(zm); ray_release(zs);
    }
    if ((r = aw_f64_bits("fs", "(min (at T 'f))", 0)).status != TEST_PASS) return r;    /* -0.0 under D4 */
    if ((r = aw_f64_bits("fs", "(max (at T 'f))", 0)).status != TEST_PASS) return r;
    aw_save("fm", "(table [f] (list (map (fn [j] (if (== 0 (% j 2)) -0.0 0.0)) (til 3000))))", -1);
    if ((r = aw_f64_bits("fm", "(min (at T 'f))", 0)).status != TEST_PASS) return r;    /* which zero: the first */
    if ((r = aw_f64_bits("fm", "(max (at T 'f))", 0)).status != TEST_PASS) return r;
    PASS();
}

/* Fully selected vectors are answered from their encoding; the selection
 * (< k 1500) keeps vector 0 whole (ALL) and part of vector 1 (MIX), and no
 * chunk is fully selected, so the chunk path cannot answer. */
static test_result_t test_sum_encoded_vectors(void) {
    const char* q = "(select {s: (sum x) a: (avg x) from: T where: (< k 1500)})";
    static const struct { const char* name; const char* x; int force; int x_enc; uint64_t want; } cs[] = {
        { "sf", "(map (fn [j] (if (== 0 (% j 9)) 0N (- (% j 50) 20))) (til 200000))", RAY_ENC_FOR_BP, RAY_ENC_FOR_BP, 1024 * 4 },
        { "sc", "(map (fn [j] (if (== 0 (% j 9)) 0N (div j 1024))) (til 200000))",    -1,             RAY_ENC_CONST,  1024 * 4 },
        { "sr", "(div (til 200000) 100)",                                             RAY_ENC_RLE,    RAY_ENC_RLE,    1024 * 4 },
        { "sn", "(map (fn [j] (if (== 0 (% j 9)) 0N (div j 100))) (til 200000))",     RAY_ENC_RLE,    RAY_ENC_RLE,    2 * 1024 * 4 },  /* nullable RLE decodes */
    };
    for (int i = 0; i < 4; i++) {
        char tbl[512];
        snprintf(tbl, sizeof tbl, "(table [k x] (list (til 200000) (as 'I32 %s)))", cs[i].x);
        aw_save(cs[i].name, tbl, cs[i].force);
        TEST_ASSERT_EQ_I(aw_vec_enc(cs[i].name, "x", 0), cs[i].x_enc);
        test_result_t r = aw_same(cs[i].name, q, 0);
        if (r.status != TEST_PASS) return r;
        TEST_ASSERT_FMT(g_aw_unaligned || aw_col_decoded("x") == cs[i].want, "%s: x decoded %llu, want %llu", cs[i].name,
                        (unsigned long long)aw_col_decoded("x"), (unsigned long long)cs[i].want);
    }
    PASS();
}

/* With a selection: ALL segments from vectors, MIX segments decoded. */
static test_result_t test_agg_under_selection(void) {
    aw_save("as", "(table [k x] (list (til 200000) (as 'I32 (map (fn [j] (if (== 0 (% j 9)) 0N (% j 50))) (til 200000)))))", -1);
    test_result_t r;
    if ((r = aw_reduce("as", "(select {s: (sum x) a: (avg x) c: (count x) from: T where: (< k 65536)})", -1)).status != TEST_PASS) return r;
    if ((r = aw_reduce("as", "(select {s: (sum x) c: (count x) from: T where: (< k 1500)})", -1)).status != TEST_PASS) return r;
    if ((r = aw_reduce("as", "(select {m: (max x) from: T where: (== x 3)})", -1)).status != TEST_PASS) return r;
    PASS();
}

static test_result_t test_scalar_route_stats(void) {
    aw_save("sq", "(table [k g x f] (list (til 200000) (as 'I16 (% (til 200000) 7)) "
                  "(as 'I32 (map (fn [j] (if (== 0 (% j 9)) 0N (% j 50))) (til 200000))) (* 0.5 (til 200000))))", -1);
    test_result_t r;
    if ((r = aw_reduce("sq", "(select {s: (sum x) c: (count k) a: (avg x) from: T where: (< k 131072)})", 0)).status != TEST_PASS) return r;
    if ((r = aw_reduce("sq", "(select {mn: (min x) mx: (max k) from: T where: (>= k 65536)})", 0)).status != TEST_PASS) return r;
    if ((r = aw_reduce("sq", "(select {s: (sum f) from: T where: (< k 131072)})", -1)).status != TEST_PASS) return r;   /* floats: P2's RAW float view (uncounted) or a decode, never statistics */
    if ((r = aw_reduce("sq", "(select {s: (sum x) from: T where: (== g 3)})", -1)).status != TEST_PASS) return r;            /* MIX everywhere */
    PASS();
}

/* Several inputs of one select, each by its own rule under one selection:
 * integer sums and extremes from statistics, a float extreme from
 * statistics (D4), a float sum and a SYM extreme decoded (they keep P2's
 * dispatch), BOOL / U8 inputs as non-null values, the I64 extremes with a
 * total beyond INT64_MAX, an all-null chunk, and selections that cut
 * chunks and vectors at odd rows.  Every answer equals generation 0. */
static test_result_t test_scalar_route_mixed(void) {
    aw_save("sm", "(table [k x y b u f d s n] (list (til 300000) "
                  "(as 'I32 (map (fn [j] (if (== 0 (% j 9)) 0N (- (% j 50) 20))) (til 300000))) "
                  "(map (fn [j] (if (== 0 (% j 5)) 0N 9223372036854775807)) (til 300000)) "
                  "(== 0 (% (til 300000) 3)) (as 'U8 (% (til 300000) 251)) "
                  "(map (fn [j] (if (== 0 (% j 7)) 0N (- (* 0.25 j) 1000.0))) (til 300000)) "
                  "(as 'DATE (+ 7000 (div (til 300000) 1000))) "
                  "(as 'SYM (map (fn [j] (if (== 0 (% j 4)) \"b\" \"a\")) (til 300000))) "
                  "(as 'I16 (map (fn [j] (if (and (>= j 65536) (< j 131072)) 0N (% j 300))) (til 300000)))))", -1);
    static const char* const qs[] = {
        "(select {s: (sum x) a: (avg x) mn: (min x) mx: (max x) c: (count x) from: T where: (< k 196608)})",
        "(select {s: (sum y) a: (avg y) mx: (max y) from: T where: (>= k 70000)})",
        "(select {s: (sum b) a: (avg b) mn: (min b) mx: (max u) su: (sum u) au: (avg u) from: T where: (< k 131072)})",
        "(select {mn: (min f) mx: (max f) mx2: (max d) mn2: (min d) from: T where: (< k 196608)})",
        "(select {s: (sum n) a: (avg n) mn: (min n) mx: (max n) c: (count n) from: T where: (< k 196608)})",
        "(select {s: (sum x) a: (avg x) mn: (min x) mx: (max x) c: (count x) from: T where: (< k 200000)})",
        "(select {s: (sum x) mn: (min n) mx: (max f) from: T where: (and (>= k 1000) (< k 250001))})",
        "(select {s: (sum x) a: (avg x) mx: (max n) from: T where: (== (% k 3) 1)})",
        "(select {s: (sum x) sf: (sum f) from: T where: (< k 131072)})",
        "(select {s: (sum x) ms: (min s) from: T where: (< k 131072)})",
        "(select {s: (sum x) mx: (max y) a: (avg n) from: T})",
        "(select {s: (sum x) c: (count k) from: T where: (> k 400000)})",
        NULL,
    };
    for (int i = 0; qs[i]; i++) {
        test_result_t r = aw_reduce("sm", qs[i], -1);
        if (r.status != TEST_PASS) return r;
    }
    /* whole chunks from the directory and vectors from statistics: nothing decodes */
    test_result_t r;
    if ((r = aw_reduce("sm", qs[0], 0)).status != TEST_PASS) return r;
    if ((r = aw_reduce("sm", qs[3], 0)).status != TEST_PASS) return r;
    if ((r = aw_reduce("sm", qs[4], 0)).status != TEST_PASS) return r;
    PASS();
}

/* TIME sums and extremes and TIMESTAMP extremes under selections: whole
 * chunks from the directory, whole vectors (the partial last vector of the
 * partial last chunk included, (>= k 199680)) from their views and
 * descriptors, every answer generation 0's. */
static test_result_t test_scalar_route_time(void) {
    static const struct { const char* name; int force; } fs[] = {
        { "tf", RAY_ENC_FOR_BP }, { "tr", RAY_ENC_RLE }, { "tm", -1 },
    };
    for (int f = 0; f < 3; f++) {
        aw_save(fs[f].name, "(table [k t c p] (list (til 200000) "
                "(as 'TIME (map (fn [j] (if (== 0 (% j 13)) 0N (* 1000 (- (div j 3) 30000)))) (til 200000))) "
                "(as 'TIME (* 1000 (div (til 200000) 1024))) "
                "(as 'TIMESTAMP (map (fn [j] (if (== 0 (% j 17)) 0N (* 1000000007 (- j 90000)))) (til 200000)))))",
                fs[f].force);
        static const char* const qs[] = {
            "(select {s: (sum t) a: (avg t) mn: (min t) mx: (max t) sc: (sum c) mc: (max c) pn: (min p) px: (max p) from: T where: (< k 131072)})",
            "(select {s: (sum t) a: (avg t) mn: (min t) mx: (max t) sc: (sum c) ac: (avg c) pn: (min p) px: (max p) from: T where: (>= k 199680)})",
            "(select {s: (sum t) mx: (max p) sc: (sum c) from: T where: (>= k 197000)})",
            "(select {s: (sum t) mn: (min p) from: T where: (== (% k 5) 0)})",
            NULL,
        };
        for (int i = 0; qs[i]; i++) {
            test_result_t r = aw_reduce(fs[f].name, qs[i], -1);
            if (r.status != TEST_PASS) return r;
        }
        /* the partial last vector [199680, 200000) answers at vector level */
        if (fs[f].force == RAY_ENC_FOR_BP) {
            test_result_t r = aw_reduce(fs[f].name, qs[1], 0);
            if (r.status != TEST_PASS) return r;
        }
    }
    PASS();
}

/* Infinite float extremes: generation 0's strict compares from +/-DBL_MAX
 * never take +inf as a min or -inf as a max (an all-+inf min is DBL_MAX).
 * The statistics hold the infinities; the route reads a non-finite one
 * (D4: finite only), and its fold's same strict compares would agree. */
static test_result_t test_scalar_route_float_inf(void) {
    const int64_t n = 200000;
    ray_t* k = ray_vec_new(RAY_I64, n);
    ray_t* f = ray_vec_new(RAY_F64, n);
    TEST_ASSERT_TRUE(k && f && !RAY_IS_ERR(k) && !RAY_IS_ERR(f));
    k->len = f->len = n;
    for (int64_t i = 0; i < n; i++) {
        ((int64_t*)ray_data(k))[i] = i;
        ((double*)ray_data(f))[i] = i < 70000 ? INFINITY : i < 140000 ? -INFINITY : 0.25 * (double)i;
    }
    ray_t* tb = ray_table_new(2);
    tb = ray_table_add_col(tb, ray_sym_intern("k", 1), k); ray_release(k);
    tb = ray_table_add_col(tb, ray_sym_intern("f", 1), f); ray_release(f);
    TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("W", 1), tb), RAY_OK);
    ray_release(tb);
    aw_eval_ok("(.db.splayed.set \"" AW_TMP "/fi0/\" W {compress: false})");
    aw_eval_ok("(.db.splayed.set \"" AW_TMP "/fi2/\" W {compress: true})");
    static const char* const qs[] = {
        "(select {mn: (min f) mx: (max f) from: T where: (< k 65536)})",
        "(select {mn: (min f) mx: (max f) from: T where: (and (>= k 65536) (< k 196608))})",
        "(select {mn: (min f) mx: (max f) from: T where: (>= k 1024)})",
        NULL,
    };
    for (int i = 0; qs[i]; i++) {
        test_result_t r = aw_reduce("fi", qs[i], -1);
        if (r.status != TEST_PASS) return r;
    }
    PASS();
}

/* A ragged parted table under a selection: partition 2 lacks the column (a
 * synthetic stand-in), 1 and 3 are generation 2, 4 generation 0.  A
 * nullable stand-in is all null; a BOOL / U8 one is non-null zeros
 * (ray_null_fill), so min is 0 and avg counts its rows.  Results equal the
 * all-generation-0 table's, and the selected select form equals the scalar
 * form over the selected rows. */
static test_result_t test_scalar_route_parted(void) {
    static const struct { const char* name; const char* col; bool nullable; } ty[] = {
        { "i32",  "(as 'I32 (map (fn [j] (if (== 0 (% j 13)) 0N (- j 70000))) (til 80000)))", true },
        { "u8",   "(as 'U8 (+ 1 (% (til 80000) 200)))", false },
        { "bool", "(>= (til 80000) 0)", false },
    };
    static const char* roots[] = { "rw", "rn" };
    static const char* const aggs[] = { "sum", "avg", "min", "max", "count", NULL };
    static const char* const wh[] = { "(>= y 0)", "(< y 70000)", "(>= y 65536)", "(== (% y 5) 2)", NULL };
    for (int t = 0; t < 3; t++) {
        (void)ray_test_rm_rf(aw_root()); (void)ray_test_mkdir_p(aw_root());
        for (int r = 0; r < 2; r++)
            for (int p = 0; p < 4; p++) {
                char q[900];
                snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/%s/2024.01.0%d/t/\" %s%s%s {compress: %s})",
                         roots[r], p + 1,
                         p == 1 ? "(table [y] (list (til 3000)))" : "(table [y x] (list (til 80000) ",
                         p == 1 ? "" : ty[t].col, p == 1 ? "" : "))",
                         r == 0 && p != 3 ? "true" : "false");
                aw_eval_ok(q);
            }
        aw_eval_ok("(set TW (.db.parted.get \"" AW_TMP "/rw\" 't))");
        aw_eval_ok("(set TN (.db.parted.get \"" AW_TMP "/rn\" 't))");
        for (int w = 0; wh[w]; w++)
            for (int i = 0; aggs[i]; i++) {
                char a[400], b[400], c[400];
                snprintf(a, sizeof a, "(first (at (select {m: (%s x) from: TN where: %s}) 'm))", aggs[i], wh[w]);
                snprintf(b, sizeof b, "(first (at (select {m: (%s x) from: TW where: %s}) 'm))", aggs[i], wh[w]);
                snprintf(c, sizeof c, "(%s (at (select {x: x from: TW where: %s}) 'x))", aggs[i], wh[w]);
                ray_t* x = aw_eval(a);
                ray_enc_counters_reset();
                ray_t* y = aw_eval(b);
                ray_enc_counters_t k; ray_enc_counters_get(&k);
                ray_t* z = aw_eval(c);
                TEST_ASSERT_FMT(x && y && z && !RAY_IS_ERR(x) && !RAY_IS_ERR(y) && !RAY_IS_ERR(z), "%s %s %s", ty[t].name, aggs[i], wh[w]);
                ray_t* fx = ray_fmt(x, 0); ray_t* fy = ray_fmt(y, 0); ray_t* fz = ray_fmt(z, 0);
                TEST_ASSERT_FMT(ray_str_len(fx) == ray_str_len(fy) && memcmp(ray_str_ptr(fx), ray_str_ptr(fy), ray_str_len(fx)) == 0,
                                "%s %s %s: gen 0 vs mixed", ty[t].name, aggs[i], wh[w]);
                TEST_ASSERT_FMT(ray_str_len(fz) == ray_str_len(fy) && memcmp(ray_str_ptr(fz), ray_str_ptr(fy), ray_str_len(fz)) == 0,
                                "%s %s %s: select vs scalar", ty[t].name, aggs[i], wh[w]);
                TEST_ASSERT_EQ_U(k.materialized, 0);
                ray_release(fx); ray_release(fy); ray_release(fz); ray_release(x); ray_release(y); ray_release(z);
            }
    }
    PASS();
}

/* Bytes decoded from column `col` of the parted table named `tbl` (each
 * generation-2 segment's own counter). */
static uint64_t aw_parted_col_decoded(const char* tbl, const char* col) {
    ray_t* t = ray_eval_str(tbl);
    ray_t* encv = t && !RAY_IS_ERR(t) ? ray_table_get_col_enc(t, ray_sym_intern(col, strlen(col))) : NULL;
    uint64_t b = 0;
    ray_enccol_t* e = encv && encv->type == RAY_ENCCOL ? ray_enccol_of(encv) : NULL;
    if (e && e->kind == RAY_ENCCOL_PARTED && e->wrapper) {
        ray_t** segs = (ray_t**)ray_data(e->wrapper);
        for (int64_t i = 0; i < e->wrapper->len; i++)
            if (segs[i] && segs[i]->type == RAY_ENCCOL) b += atomic_load(&ray_enccol_of(segs[i])->decoded_bytes);
    }
    if (t && !RAY_IS_ERR(t)) ray_release(t);
    return b;
}

/* Parted, every partition generation 2 (each runs the route on its own
 * rows): whole chunks under the selection answer from the directory and
 * whole vectors from their statistics, so the aggregate input decodes
 * nothing; a partly selected vector is read. */
static test_result_t test_scalar_route_parted_stats(void) {
    for (int r = 0; r < 2; r++)
        for (int p = 0; p < 3; p++) {
            char q[700];
            snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/%s/2024.01.0%d/t/\" "
                     "(table [y x] (list (til 70001) (as 'I32 (map (fn [j] (if (== 0 (%% j 11)) 0N (- (%% j 50) 20))) (til 70001))))) {compress: %s})",
                     r ? "pn" : "pw", p + 1, r ? "false" : "true");
            aw_eval_ok(q);
        }
    aw_eval_ok("(set TW (.db.parted.get \"" AW_TMP "/pw\" 't))");
    aw_eval_ok("(set TN (.db.parted.get \"" AW_TMP "/pn\" 't))");
    static const char* const qs[] = {
        "(select {s: (sum x) mn: (min x) mx: (max x) c: (count x) from: %s where: (< y 65536)})",
        "(select {s: (sum x) mn: (min x) mx: (max x) c: (count x) from: %s where: (>= y 2048)})",
        "(select {s: (sum x) mx: (max x) from: %s where: (== (%% y 4) 1)})",
        /* a nullable avg counts its non-nulls per partition through an
         * indicator expression (#767), evaluated over every row: equal only */
        "(select {s: (sum x) a: (avg x) from: %s where: (< y 65536)})",
        NULL,
    };
    for (int i = 0; qs[i]; i++) {
        char a[400], b[400];
        snprintf(a, sizeof a, qs[i], "TN");
        snprintf(b, sizeof b, qs[i], "TW");
        ray_t* x = aw_eval(a);
        ray_enc_counters_reset();
        ray_t* y = aw_eval(b);
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        TEST_ASSERT_FMT(x && y && !RAY_IS_ERR(x) && !RAY_IS_ERR(y), "%s", b);
        ray_t* fx = ray_fmt(x, 0); ray_t* fy = ray_fmt(y, 0);
        TEST_ASSERT_FMT(ray_str_len(fx) == ray_str_len(fy) && memcmp(ray_str_ptr(fx), ray_str_ptr(fy), ray_str_len(fx)) == 0, "%s", b);
        TEST_ASSERT_EQ_U(k.materialized, 0);
        if (i < 2 && !g_aw_unaligned) TEST_ASSERT_FMT(aw_parted_col_decoded("TW", "x") == 0, "%s: x decoded %llu", b,
                                   (unsigned long long)aw_parted_col_decoded("TW", "x"));
        ray_release(fx); ray_release(fy); ray_release(x); ray_release(y);
    }
    PASS();
}

/* q2's shape: count over a column of CONST and RLE vectors with no nulls;
 * the count is answered from the runs, no byte decoded; FOR_BP vectors
 * count on their packed residuals, DICT vectors on their codes, SYM RLE
 * vectors on their ids (== / != only; an absent symbol matches nothing;
 * the "" literal is the null atom, which the fast path declines). */
static test_result_t test_count_on_runs(void) {
    /* a: CONST 0 in 3 of 4 vectors, RLE [0 7 0] in the fourth (the
     * AdvEngineID shape); b: FOR_BP 0..49 */
    aw_save("cr", "(table [a b] (list (as 'I16 (map (fn [j] (if (and (== 3 (% (div j 1024) 4)) (> (% j 1024) 100) (< (% j 1024) 300)) 7 0)) (til 200000))) "
                  "(as 'I32 (% (til 200000) 50))))", -1);
    static const char* const q[] = {
        "(count (select {from: T where: (!= a 0)}))",
        "(count (select {from: T where: (== a 7)}))",
        "(count (select {from: T where: (> b 20)}))",
        "(count (select {from: T where: (== b 7)}))",
        "(count (select {from: T where: (< a 7)}))",
        "(count (select {from: T where: (>= b 49)}))",
        "(count (select {from: T where: (<= b -1)}))",
        "(count (select {from: T where: (!= b 50)}))",
        "(count (select {from: T where: (< b 13)}))",
        "(count (select {from: T where: (<= b 13)}))",
        "(count (select {from: T where: (!= b 13)}))",
        "(count (select {from: T where: (>= b 0)}))",
        "(count (select {from: T where: (< 30 b)}))",
        NULL,
    };
    uint64_t f0 = ray_query_test_count_fast();
    for (int i = 0; q[i]; i++) {
        test_result_t r = aw_reduce("cr", q[i], 0);     /* equal answers, 0 decoded bytes */
        if (r.status != TEST_PASS) return r;
    }
    TEST_ASSERT_TRUE(ray_query_test_count_fast() > f0);   /* the fast path ran */
    TEST_ASSERT_EQ_I(aw_vec_enc("cr", "a", 3), RAY_ENC_RLE);
    TEST_ASSERT_EQ_I(aw_vec_enc("cr", "b", 3), RAY_ENC_FOR_BP);
    /* DICT (forced): 37 wide values, every operator around and on them */
    aw_save("cd", "(table [d] (list (* 1000003 (- (% (* (til 200000) 7) 37) 18))))", RAY_ENC_DICT);
    TEST_ASSERT_EQ_I(aw_vec_enc("cd", "d", 5), RAY_ENC_DICT);
    static const char* const dq[] = {
        "(count (select {from: T where: (== d 3000009)}))", "(count (select {from: T where: (!= d 3000009)}))",
        "(count (select {from: T where: (< d 3000009)}))",  "(count (select {from: T where: (<= d 3000009)}))",
        "(count (select {from: T where: (> d 3000010)}))",  "(count (select {from: T where: (>= d -18000054)}))",
        "(count (select {from: T where: (== d 5)}))",       "(count (select {from: T where: (!= d 5)}))",
        NULL,
    };
    for (int i = 0; dq[i]; i++) {
        test_result_t r = aw_reduce("cd", dq[i], 0);
        if (r.status != TEST_PASS) return r;
    }
    /* SYM: runs of 300 rows over 7 symbols and "" */
    aw_save("cs", "(table [s] (list (as 'SYMBOL (map (fn [j] (if (== 3 (% (div j 300) 8)) \"\" (format \"s%\" (% (div j 300) 7)))) (til 200000)))))", -1);
    static const char* const sq[] = {
        "(count (select {from: T where: (== s 's3)}))", "(count (select {from: T where: (!= s 's3)}))",
        "(count (select {from: T where: (== s 'zz)}))", "(count (select {from: T where: (!= s 'zz)}))",
        NULL,
    };
    for (int i = 0; sq[i]; i++) {
        test_result_t r = aw_reduce("cs", sq[i], 0);
        if (r.status != TEST_PASS) return r;
    }
    PASS();
}

/* q8's shape: a COUNT group by the same key under its own != 0 filter.
 * The RLE vectors' runs add to their slots (and fold the selection
 * bounds); nothing decoded.  Also unfiltered (no selection), a selection
 * that cuts runs (MIX segments), COUNT beside a group of no count (the
 * per-row path), a SYM key, and a parted table, every partition
 * generation 2 or one generation 0, ragged (a partition without the key:
 * an I16 stand-in is all null, a U8 one zeros). */
static test_result_t test_group_count_on_runs(void) {
    /* the RLE vectors' runs hold over 65536 selected rows: the parallel
     * dense route (below the threshold the selection is compacted) */
    aw_save("gr", "(table [a] (list (as 'I16 (map (fn [j] (if (and (== 3 (% (div j 1024) 4)) (> (% j 1024) 100) (< (% j 1024) 900)) (+ 1 (% (div j 4096) 5)) 0)) (til 400000)))))", -1);
    test_result_t r = aw_reduce("gr", "(select {a: a c: (count a) from: T where: (!= a 0) by: a desc: c take: 10})", 0);
    if (r.status != TEST_PASS) return r;
    TEST_ASSERT_TRUE(g_aw_unaligned || agg_route_stats().dense_strategy == AGG_DENSE_TASK_LOCAL);
    if ((r = aw_reduce("gr", "(select {c: (count a) from: T by: a})", 0)).status != TEST_PASS) return r;
    if ((r = aw_reduce("gr", "(select {c: (count a) from: T where: (!= a 3) by: a})", 0)).status != TEST_PASS) return r;
    /* a selection that cuts runs (MIX segments): rows of the run where
     * a second predicate fails */
    aw_save("gm", "(table [a k] (list (as 'I16 (map (fn [j] (if (and (> (% j 1024) 100) (< (% j 1024) 900)) (+ 1 (% (div j 1024) 5)) 0)) (til 300000))) (til 300000)))", -1);
    if ((r = aw_reduce("gm", "(select {a: a c: (count a) from: T where: (and (!= a 0) (!= 0 (% k 3))) by: a desc: c take: 10})", -1)).status != TEST_PASS) return r;
    if (!g_aw_unaligned) TEST_ASSERT_FMT(aw_col_decoded("a") == 0, "gm: a decoded %llu", (unsigned long long)aw_col_decoded("a"));
    if ((r = aw_reduce("gm", "(select {c: (count k) d: (count a) from: T where: (< (% k 7) 4) by: a})", -1)).status != TEST_PASS) return r;
    if (!g_aw_unaligned) TEST_ASSERT_FMT(aw_col_decoded("a") == 0, "gm 2: a decoded %llu", (unsigned long long)aw_col_decoded("a"));
    /* not COUNT only: the per-row path, same answers */
    if ((r = aw_reduce("gm", "(select {c: (count a) s: (sum k) from: T where: (!= a 0) by: a})", -1)).status != TEST_PASS) return r;
    /* a nullable key in runs: a vector with nulls carries a bitmap, so its
     * run view declines (counting it by runs would put the null rows under
     * the run's stored value: 5 groups instead of 6) */
    aw_save("gn", "(table [a] (list (as 'I16 (map (fn [j] (if (== 500 (% j 4096)) 0Nh (+ 1 (% (div j 1024) 5)))) (til 300000)))))", -1);
    if ((r = aw_reduce("gn", "(select {c: (count a) from: T by: a})", -1)).status != TEST_PASS) return r;
    {
        aw_eval_ok("(set R (select {c: (count a) from: T by: a}))");
        ray_t* n = ray_eval_str("(count R)");
        TEST_ASSERT_TRUE(n && !RAY_IS_ERR(n) && n->i64 == 6);
        ray_release(n);
    }
    /* packed keys (every vector FOR_BP, or DICT): no run view, so each
     * vector is decoded and counted per row, as before (D6); the same
     * answers, nothing materialized */
    static const struct { const char* name; int enc; } pk[] = { { "gp", RAY_ENC_FOR_BP }, { "gd", RAY_ENC_DICT } };
    for (int i = 0; i < 2; i++) {
        aw_save(pk[i].name, "(table [a v] (list (as 'I32 (- (* 100 (% (* (til 300000) 7) 13)) 600)) (% (til 300000) 7)))", pk[i].enc);
        TEST_ASSERT_EQ_I(aw_vec_enc(pk[i].name, "a", 7), pk[i].enc);
        if ((r = aw_reduce(pk[i].name, "(select {a: a c: (count a) from: T where: (!= a 0) by: a desc: c take: 10})", -1)).status != TEST_PASS) return r;
        if ((r = aw_reduce(pk[i].name, "(select {c: (count a) from: T by: a})", -1)).status != TEST_PASS) return r;
        if ((r = aw_reduce(pk[i].name, "(select {c: (count a) from: T where: (!= v 0) by: a})", -1)).status != TEST_PASS) return r;
    }
    /* a SYM key in runs, "" (id 0) among them */
    aw_save("gs", "(table [s] (list (as 'SYMBOL (map (fn [j] (if (== 3 (% (div j 300) 8)) \"\" (format \"s%\" (% (div j 300) 7)))) (til 300000)))))", -1);
    if ((r = aw_reduce("gs", "(select {c: (count s) from: T by: s})", -1)).status != TEST_PASS) return r;
    if ((r = aw_reduce("gs", "(select {c: (count s) from: T where: (!= s 's2) by: s desc: c take: 3})", -1)).status != TEST_PASS) return r;
    /* parted: g (every partition generation 2), m (partition 2 generation
     * 0), d (I16, partition 3 without a: all-null stand-in), u (U8,
     * partition 3 without a: zeros) */
    static const struct { const char* root; const char* ty; int gen0, ragged; } pt[] = {
        { "pg", "I16", -1, -1 }, { "pm", "I16", 1, -1 }, { "pd", "I16", -1, 2 }, { "pu", "U8", -1, 2 },
    };
    for (int t = 0; t < 4; t++)
        for (int gen = 0; gen <= 2; gen += 2)
            for (int p = 0; p < 3; p++) {
                char q[1200];
                const bool g2 = gen == 2 && p != pt[t].gen0;
                if (p == pt[t].ragged)
                    snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/%s%d/2024.01.0%d/t/\" (table [k] (list (til 70000))) {compress: %s})",
                             pt[t].root, gen, p + 1, g2 ? "true" : "false");
                else
                    snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/%s%d/2024.01.0%d/t/\" "
                             "(table [k a] (list (til 100000) (as '%s (map (fn [j] (if (and (> (%% j 1024) 100) (< (%% j 1024) 900)) (+ 1 (%% (div j 1024) 5)) 0)) (+ (til 100000) %d))))) "
                             "{compress: %s})", pt[t].root, gen, p + 1, pt[t].ty, p * 777, g2 ? "true" : "false");
                aw_eval_ok(q);
            }
    static const char* const pq[] = {
        "(select {a: a c: (count a) from: T where: (!= a 0) by: a desc: c take: 10})",
        "(select {c: (count a) from: T by: a})",
        NULL,
    };
    for (int t = 0; t < 4; t++)
        for (int i = 0; pq[i]; i++) {
            char b[512];
            ray_t* out[2];
            for (int gen = 0; gen <= 2; gen += 2) {
                snprintf(b, sizeof b, "(set T (.db.parted.get \"" AW_TMP "/%s%d\" 't))", pt[t].root, gen);
                aw_eval_ok(b);
                ray_enc_counters_reset();
                out[gen / 2] = ray_eval_str(pq[i]);
                TEST_ASSERT_FMT(out[gen / 2] && !RAY_IS_ERR(out[gen / 2]), "%s %s: %s", pt[t].root, pq[i], ray_error_msg());
            }
            ray_enc_counters_t k; ray_enc_counters_get(&k);
            TEST_ASSERT_EQ_U(k.materialized, 0);
            if (!g_aw_unaligned && pt[t].ragged < 0)      /* a stand-in segment is read (decoded) per row */
                TEST_ASSERT_FMT(aw_parted_col_decoded("T", "a") == 0, "%s %s: a decoded %llu", pt[t].root, pq[i],
                                (unsigned long long)aw_parted_col_decoded("T", "a"));
            ray_t* fa = ray_fmt(out[0], 0); ray_t* fb = ray_fmt(out[1], 0);
            TEST_ASSERT_FMT(ray_str_len(fa) == ray_str_len(fb) && memcmp(ray_str_ptr(fa), ray_str_ptr(fb), ray_str_len(fa)) == 0,
                            "%s %s differs", pt[t].root, pq[i]);
            ray_release(fa); ray_release(fb); ray_release(out[0]); ray_release(out[1]);
        }
    PASS();
}

/* The scalar chunk route cut into tasks that are no multiple of a vector or
 * a chunk (the grain ray_pool_dispatch re-derives when it clamps to its
 * ring): every answer still equals generation 0's. */
static test_result_t test_unaligned_grain(void) {
    static test_result_t (*const fns[])(void) = {
        test_scalar_route_mixed, test_eq_runs_lack, test_scalar_route_parted_stats, test_scalar_route_time,
    };
    static const int64_t grains[] = { 5000, 70001 };    /* inside a chunk, across chunks */
    for (size_t g = 0; g < sizeof grains / sizeof *grains; g++)
        for (size_t i = 0; i < sizeof fns / sizeof *fns; i++) {
            if (g > 0 && i > 1) break;                       /* the second cut: two shapes */
            (void)ray_test_rm_rf(aw_root()); (void)ray_test_mkdir_p(aw_root());
            ray_group_test_scalar_grain(grains[g]);
            g_aw_unaligned = true;
            test_result_t r = fns[i]();
            g_aw_unaligned = false;
            ray_group_test_scalar_grain(0);
            if (r.status != TEST_PASS) return r;
        }
    PASS();
}

/* Chunk c of the generation-2 column file at `path` rewritten without
 * statistics: its directory entry and every vector that has values carry
 * RAY_ENC_STAT_ABSENT and zero bounds (a valid file: the writer may omit
 * them; all-null vectors never carry any). */
static bool aw_forge_absent(const char* path, int64_t c) {
    FILE* f = fopen(path, "r+b"); if (!f) return false;
    ray_enc_dirent_t de;
    long doff = (long)(RAY_ENC_DIR_OFF + (uint64_t)c * sizeof de);
    bool ok = fseek(f, doff, SEEK_SET) == 0 && fread(&de, sizeof de, 1, f) == 1 && !(de.flags & RAY_ENC_STAT_ABSENT);
    if (ok) {
        de.flags |= RAY_ENC_STAT_ABSENT; de.min = 0; de.max = 0;
        ok = fseek(f, doff, SEEK_SET) == 0 && fwrite(&de, sizeof de, 1, f) == 1;
    }
    for (int j = 0; ok && j < RAY_ENC_VECS_PER_CHUNK; j++) {
        ray_enc_desc_t d;
        long off = (long)(de.offset + (uint64_t)j * sizeof d);
        ok = fseek(f, off, SEEK_SET) == 0 && fread(&d, sizeof d, 1, f) == 1;
        if (!ok || (d.encoding == 0 && d.payload_bytes == 0)) break;     /* past the last vector */
        if (d.flags & RAY_ENC_STAT_ABSENT) continue;
        d.flags |= RAY_ENC_STAT_ABSENT; d.min = 0; d.max = 0;
        ok = fseek(f, off, SEEK_SET) == 0 && fwrite(&d, sizeof d, 1, f) == 1;
    }
    return fclose(f) == 0 && ok;
}

/* Review focus 6: every pruning case, results and order equal to
 * generation 0; nothing materializes (the sort key is read by range).  kn has a
 * null every 1000 rows and chunk 2 all null; its values climb to 300004
 * at row 299999 and restart at 5. */
static test_result_t test_topk_prune_cases(void) {
    aw_save("tp", "(table [kn v] (list (as 'I64 (map (fn [j] (if (or (== 0 (% j 1000)) (and (>= j 131072) (< j 196608))) 0N "
                  "(+ 5 (% j 300000)))) (til 400000))) (% (* (til 400000) 13) 1000)))", -1);
    static const char* const q[] = {
        "(select {kn: kn v: v from: T where: (> v 100) asc: kn take: 20})",
        "(select {kn: kn v: v from: T where: (> v 100) desc: kn take: 20})",
        "(select {kn: kn v: v from: T where: (== v 7) asc: [kn v] take: 5})",   /* two sort keys */
        "(select {kn: kn from: T desc: kn take: 401})",          /* K above the null count of a chunk */
        "(select {kn: kn v: v from: T asc: kn take: 8000})",     /* near FPK_MAX_K (8192), past the leading nulls */
        "(select {kn: kn v: v from: T where: (> v 900) asc: kn take: 3})",     /* the answer: nulls of chunk 2 */
        "(select {kn: kn v: v from: T where: (< v 50) desc: kn take: 7})",
        NULL,
    };
    static const uint64_t mat[] = { 0, 0, 0, 0, 0, 0, 0 };      /* the sort keys are read by range */
    for (int i = 0; q[i]; i++) AW_SAME("tp", q[i], mat[i]);
    /* Ties on the first key at the bound: k1 is constant over 4096 rows, so
     * the first morsel fills the heap and publishes k1 = 0 while the next
     * vectors (minimum 0, equal to the bound) still hold better rows by k2. */
    aw_save("tq", "(table [k1 k2] (list (div (til 200000) 4096) (- 200000 (til 200000))))", -1);
    AW_SAME("tq", "(select {k1: k1 k2: k2 from: T asc: [k1 k2] take: 3})", 0);
    AW_SAME("tq", "(select {k1: k1 k2: k2 from: T where: (> k2 0) desc: [k1 k2] take: 3})", 0);
    /* pruning happened at both levels on the generation-2 key */
    aw_eval_ok("(set T (.db.splayed.get \"" AW_TMP "/tp2/\"))");
    aw_eval_ok("(select {kn: kn v: v from: T where: (> v 100) desc: kn take: 20})");
    uint64_t pc = 0, pv = 0;
    ray_fused_topk_test_pruned(&pc, &pv);
    /* chunk-slot prunes only on aligned grains: a clamped ring may hand the
     * best chunks to one task, whose bound then never undercuts a later
     * chunk (as the tr cases below) */
    if (!g_aw_unaligned) TEST_ASSERT_TRUE(pc > 0);
    TEST_ASSERT_TRUE(pv > 0);                                   /* also under a clamped grain */
    /* I32 and TIMESTAMP first keys: negative values climbing through zero
     * (row j holds j - 150000, the timestamp that many seconds from the
     * epoch), a null every 5000 rows (every chunk has some, most vectors
     * none), and chunk 3 all null.  Ascending prunes vectors only (every
     * chunk has nulls); descending prunes at both levels. */
    aw_save("tr", "(table [i t v] (list "
                  "(as 'I32 (map (fn [j] (if (or (== 0 (% j 5000)) (and (>= j 196608) (< j 262144))) 0N (- j 150000))) (til 300000))) "
                  "(as 'TIMESTAMP (map (fn [j] (if (or (== 0 (% j 5000)) (and (>= j 196608) (< j 262144))) 0N (* (- j 150000) 1000000000))) (til 300000))) "
                  "(% (* (til 300000) 13) 1000)))", -1);
    static const char* const qr[] = {
        "(select {i: i v: v from: T where: (> v 100) asc: i take: 12})",
        "(select {i: i v: v from: T where: (> v 100) desc: i take: 12})",
        "(select {t: t v: v from: T where: (> v 100) asc: t take: 12})",
        "(select {t: t v: v from: T where: (> v 100) desc: t take: 12})",
        "(select {i: i v: v from: T asc: i take: 70})",          /* past the 60 nulls outside chunk 3 */
        "(select {t: t i: i from: T where: (< v 20) desc: [t i] take: 9})",
        NULL,
    };
    static const uint64_t matr[] = { 0, 0, 0, 0, 0, 0 };
    static const bool descr[] = { false, true, false, true, false, true };
    for (int i = 0; qr[i]; i++) {
        AW_SAME("tr", qr[i], matr[i]);
        /* q5's answer is nulls (no bound is published); a clamped grain may
         * leave the best chunk to one task that scans it in rising order */
        if (i == 4 || g_aw_unaligned) continue;
        ray_fused_topk_test_pruned(&pc, &pv);
        TEST_ASSERT_FMT(pv > 0 && (!descr[i] || pc > 0), "%s: pruned %llu chunk slots, %llu vectors", qr[i],
                        (unsigned long long)pc, (unsigned long long)pv);
    }
    /* generation 0 never takes the directory path */
    aw_eval_ok("(set T (.db.splayed.get \"" AW_TMP "/tp0/\"))");
    aw_eval_ok("(select {kn: kn v: v from: T where: (> v 100) desc: kn take: 20})");
    ray_fused_topk_test_pruned(&pc, &pv);
    TEST_ASSERT_EQ_I(pc + pv, 0);
    /* A chunk without statistics (chunk 4, which holds the largest keys) is
     * never pruned: visited after the ordered chunks, it still wins. */
    {
        char dir[512], path[600];
        TEST_ASSERT_EQ_I(ray_splay_resolve_dir(ray_test_tmp(AW_TMP "/tp2/"), dir, sizeof dir), RAY_OK);
        snprintf(path, sizeof path, "%s/kn", dir);
        TEST_ASSERT_TRUE(aw_forge_absent(path, 4));
        AW_SAME("tp", q[1], 0);
        AW_SAME("tp", q[3], 0);
        AW_SAME("tp", "(select {kn: kn v: v from: T where: (< v 500) asc: kn take: 9})", 0);
    }
    PASS();
}

/* Range-read sort keys (fpk_rk_t): a generation-2 key is never
 * materialized for the top-k, its vectors are read by range and the heaps
 * carry the values; answers, order and ties equal generation 0's.  300000
 * rows (five chunks): t a TIMESTAMP with every value on three rows and a
 * null every 5003 rows, s a SYM of 50 words ("" among them), k2 an I32 of
 * five values, f an F64 of 1000 values with a NaN null every 13 rows, v
 * the predicate column, u a STR, ts a sorted TIMESTAMP. */
static test_result_t test_topk_range_keys(void) {
    aw_eval_ok("(set PH (as 'SYMBOL (concat [\"\"] (map (fn [j] (format \"w%\" (- 60 j))) (til 49)))))");
    aw_save("tk", "(table [t s k2 f v u ts] (list "
                  "(as 'TIMESTAMP (map (fn [j] (if (== 0 (% j 5003)) 0N (* (div (% (* j 7919) 300007) 3) 1000000000))) (til 300000))) "
                  "(at PH (% (* (til 300000) 7) 50)) (as 'I32 (% (til 300000) 5)) "
                  "(as 'F64 (map (fn [j] (if (== 0 (% j 13)) 0N (% (* j 31) 1000))) (til 300000))) "
                  "(% (* (til 300000) 13) 1000) (as 'STR (map (fn [j] (format \"u%\" (% j 97))) (til 300000))) "
                  "(as 'TIMESTAMP (* (til 300000) 1000000000))))", -1);
    static const char* const q[] = {
        "(select {t: t v: v from: T where: (> v 100) asc: t take: 20})",          /* nulls first, by row */
        "(select {t: t v: v from: T where: (> v 100) desc: t take: 20})",
        "(select {t: t v: v from: T where: (> v 100) asc: t take: 200})",         /* past the 60 nulls: ties of three */
        "(select {t: t s: s from: T where: (!= s ') asc: [t s] take: 30})",       /* a SYM second key */
        "(select {t: t s: s from: T where: (!= s ') desc: [t s] take: 30})",
        "(select {k2: k2 t: t from: T asc: [k2 t] take: 50})",                    /* five-value first key */
        "(select {k2: k2 v: v from: T asc: k2 desc: v take: 12})",                /* mixed directions */
        "(select {f: f v: v from: T where: (< v 500) desc: [f v] take: 15})",     /* a float key */
        "(select {f: f v: v from: T where: (< v 500) asc: [f v] take: 15})",      /* NaN nulls first */
        "(select {s: s v: v from: T asc: [s v] take: 10})",                       /* a SYM first key */
        "(select {s: s v: v from: T desc: [s v] take: 10})",
        "(select {t: t v: v from: T where: (== v 7) asc: t take: 5000})",         /* every passing row */
        "(select {t: t k2: k2 from: T where: (> v 998) desc: [k2 t] take: 7})",
        NULL,
    };
    for (int i = 0; q[i]; i++) AW_SAME("tk", q[i], 0);              /* no key materialized */
    /* a sorted key (ts, the row's second) is read only where it can win */
    AW_SAME("tk", "(select {ts: ts v: v from: T where: (> v 100) asc: ts take: 10})", 0);
    TEST_ASSERT_FMT(g_aw_unaligned || aw_col_decoded("ts") < 300000 * 8 / 2, "ts decoded %llu",
                    (unsigned long long)aw_col_decoded("ts"));
    AW_SAME("tk", "(select {ts: ts t: t from: T desc: [ts t] take: 10})", 0);
    /* the first key's directory still prunes the range-read key */
    aw_eval_ok("(set T (.db.splayed.get \"" AW_TMP "/tk2/\"))");
    aw_eval_ok("(select {t: t v: v from: T where: (> v 100) desc: t take: 20})");
    uint64_t pc = 0, pv = 0;
    ray_fused_topk_test_pruned(&pc, &pv);
    TEST_ASSERT_TRUE(g_aw_unaligned || pv > 0);
    /* a STR key keeps the materialized path: both keys */
    AW_SAME("tk", "(select {u: u v: v from: T where: (> v 990) asc: [u v] take: 6})", 2);
    PASS();
}

/* One cut of test_group_topn_ties: `shape` at take n over the parted P
 * equals ref (the stable full sort's first n rows) and the splayed copy's
 * answer, on every iteration and on the radix ladder. */
static test_result_t aw_topn_case(const char* shape, int n, int gen, int iters, ray_t* ref) {
    char q[600], body[300];
    snprintf(body, sizeof body, shape, "S", n);
    snprintf(q, sizeof q, "(select %s)", body);
    ray_t* spl = aw_eval(q);
    TEST_ASSERT_FMT(spl && !RAY_IS_ERR(spl), "%s: %s", q, ray_error_msg());
    test_result_t r = aw_csv_eq(q, gen, ref, spl);
    ray_release(spl);
    if (r.status != TEST_PASS) return r;
    snprintf(body, sizeof body, shape, "P", n);
    snprintf(q, sizeof q, "(select %s)", body);
    for (int it = 0; it < iters; it++) {
        agg_route_reset();
        ray_t* got = aw_eval(q);
        TEST_ASSERT_FMT(got && !RAY_IS_ERR(got), "%s: %s", q, ray_error_msg());
        agg_route_stats_t st = agg_route_stats();
        /* the flattened parted group, on the radix ladder, not v2 */
        bool radix = st.routes[AGG_ROUTE_PARTED] > 0 && st.routes[AGG_ROUTE_LEGACY] > 0 && !st.topn_native;
        if (!radix) ray_release(got);
        TEST_ASSERT_FMT(radix, "%s: not on the radix ladder", q);
        r = aw_csv_eq(q, it, ref, got);
        ray_release(got);
        if (r.status != TEST_PASS) return r;
    }
    PASS();
}

/* aw_topn_case against the reference built here: the first n rows of the
 * same shape over P at take: 1025 (above the filter's cap: a full sort). */
static test_result_t aw_topn_check(const char* shape, int n, int gen, int iters) {
    char q[600], body[300];
    snprintf(body, sizeof body, shape, "P", 1025);
    snprintf(q, sizeof q, "(take (select %s) %d)", body, n);
    ray_t* ref = aw_eval(q);
    TEST_ASSERT_FMT(ref && !RAY_IS_ERR(ref), "%s: %s", q, ray_error_msg());
    int64_t ref_rows = ray_table_nrows(ref);
    if (ref_rows != n) ray_release(ref);
    TEST_ASSERT_FMT(ref_rows == n, "%s: %lld rows, %d wanted", q, (long long)ref_rows, n);
    test_result_t r = aw_topn_case(shape, n, gen, iters, ref);
    ray_release(ref);
    return r;
}

/* #801: a grouped take (`by … desc: <agg> take: N`, N <= 1024) arms the
 * radix group's top-N emit filter, which cuts the groups to exactly N.
 * Groups tied at the cut value are decided by the earliest source row, the
 * order the radix route's phase 3 and the stable sort follow: the answer
 * equals the stable full sort's first N rows (take: 1025 is above the
 * filter's cap, so it sorts everything) and the splayed copy's answer (the
 * same ladder over one unpartitioned table, whose dispatch cuts differ), at
 * every iteration.  The row-to-group map
 * is aperiodic against the 10050-row partitions and the dispatch tasks:
 * rows [0, 20000) hold group i*i mod 10007 (about 4 rows each) and rows
 * [20000, 40000) hold 10007 + i*i mod 5003 (about 8 rows each), groups
 * that first appear past the first task, so their earliest row is found
 * only by taking the minimum across workers and partitions.  50 more rows
 * go to groups 0..49 at the end.  Every cut falls inside a tie set of
 * thousands of groups.  The SYM keys beside a computed one take the
 * flattened parted group onto the radix fat-entry path; the integer keys
 * take its v2 direct-insert phase 2. */
static test_result_t test_group_topn_ties(void) {
    aw_eval_ok("(set K (concat (concat (% (* (til 20000) (til 20000)) 10007) (+ 10007 (% (* (til 20000) (til 20000)) 5003))) "
               "(% (til 200) 50)))");
    aw_eval_ok("(set W (table [s u v g] (list (at (as 'SYMBOL (map (fn [j] (format \"w%\" j)) (til 15010))) K) "
               "(% K 3) (til 40200) K)))");
    for (int gen = 0; gen <= 2; gen += 2)
        for (int p = 0; p < 4; p++) {
            char q[600];
            snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/tt%d/2024.01.0%d/t/\" (take (drop W %d) 10050) "
                     "\"" AW_TMP "/tt%d/.sym\" {compress: %s})", gen, p + 1, p * 10050, gen, gen ? "true" : "false");
            aw_eval_ok(q);
        }
    aw_eval_ok("(.db.splayed.set \"" AW_TMP "/ts/t/\" W \"" AW_TMP "/ts/.sym\")");
    aw_eval_ok("(set S (.db.splayed.get \"" AW_TMP "/ts/t/\" \"" AW_TMP "/ts/.sym\"))");
    static const char* const shapes[] = {
        "{c: (count v) from: %s by: {s: s z: (%% v 1) t: s} desc: c take: %d}",
        "{c: (count v) from: %s by: {s: s z: (%% v 1) t: s} asc: c take: %d}",
        "{m: (min u) c: (count v) from: %s by: {s: s z: (%% v 1) t: s} desc: m take: %d}",
        "{q: (sum u) from: %s by: {s: s z: (%% v 1) t: s} asc: q take: %d}",
        "{q: (max u) from: %s by: {s: s z: (%% v 1) t: s} desc: q take: %d}",
        "{c: (count v) from: %s by: {g: g z: (%% v 1) h: g} desc: c take: %d}",
        "{q: (sum u) from: %s by: {g: g z: (%% v 1) h: g} asc: q take: %d}",
    };
    static const int takes[] = { 100, 7 };
    for (int gen = 0; gen <= 2; gen += 2) {
        char q[600];
        snprintf(q, sizeof q, "(set P (.db.parted.get \"" AW_TMP "/tt%d\" 't))", gen);
        aw_eval_ok(q);
        for (size_t si = 0; si < sizeof shapes / sizeof *shapes; si++)
            for (size_t ti = 0; ti < sizeof takes / sizeof *takes; ti++) {
                /* the race needs repeats; the other cuts are checked a few times */
                int iters = si == 0 && ti == 0 && !g_aw_unaligned ? 20 : 3;
                test_result_t r = aw_topn_check(shapes[si], takes[ti], gen, iters);
                if (r.status != TEST_PASS) return r;
            }
    }
    aw_eval_ok("(set P 0)"); aw_eval_ok("(set S 0)"); aw_eval_ok("(set W 0)"); aw_eval_ok("(set K 0)");
    PASS();
}

/* #808: an all-null group's aggregate is a null, and the stable sort ranks
 * nulls below every value: first under asc:, last under desc:.  The radix
 * top-N ranks such a group there too, not by the slot's init sentinel (MIN's
 * INT64_MAX put all-null groups at the top of a desc: take and dropped them
 * from an asc: one).  2000 groups over two partitions, 400 of them all-null
 * in w; take 5 runs the fused per-partition scan, take 30 the serial one. */
static test_result_t test_group_topn_nulls(void) {
    aw_eval_ok("(set K (% (* (til 8000) 7919) 2000))");
    aw_eval_ok("(set W (table [s w v] (list (at (as 'SYMBOL (map (fn [j] (format \"w%\" j)) (til 2000))) K) "
               "(as 'I64 (map (fn [k] (if (< (% k 5) 1) 0Nl (% k 13))) K)) (til 8000))))");
    aw_eval_ok("(.db.splayed.set \"" AW_TMP "/tn/2024.01.01/t/\" (take W 4000) \"" AW_TMP "/tn/.sym\")");
    aw_eval_ok("(.db.splayed.set \"" AW_TMP "/tn/2024.01.02/t/\" (drop W 4000) \"" AW_TMP "/tn/.sym\")");
    aw_eval_ok("(.db.splayed.set \"" AW_TMP "/tns/t/\" W \"" AW_TMP "/tns/.sym\")");
    aw_eval_ok("(set P (.db.parted.get \"" AW_TMP "/tn\" 't))");
    aw_eval_ok("(set S (.db.splayed.get \"" AW_TMP "/tns/t/\" \"" AW_TMP "/tns/.sym\"))");
    static const char* const shapes[] = {
        "{m: (min w) from: %s by: {s: s z: (%% v 1) t: s} desc: m take: %d}",
        "{m: (min w) from: %s by: {s: s z: (%% v 1) t: s} asc: m take: %d}",
        "{m: (max w) from: %s by: {s: s z: (%% v 1) t: s} desc: m take: %d}",
        "{m: (max w) from: %s by: {s: s z: (%% v 1) t: s} asc: m take: %d}",
        "{m: (sum w) from: %s by: {s: s z: (%% v 1) t: s} desc: m take: %d}",
        "{m: (sum w) from: %s by: {s: s z: (%% v 1) t: s} asc: m take: %d}",
        "{m: (avg w) from: %s by: {s: s z: (%% v 1) t: s} desc: m take: %d}",
        "{m: (avg w) from: %s by: {s: s z: (%% v 1) t: s} asc: m take: %d}",
    };
    static const int takes[] = { 5, 30 };
    for (size_t si = 0; si < sizeof shapes / sizeof *shapes; si++)
        for (size_t ti = 0; ti < sizeof takes / sizeof *takes; ti++) {
            test_result_t r = aw_topn_check(shapes[si], takes[ti], 0, 2);
            if (r.status != TEST_PASS) return r;
        }
    aw_eval_ok("(set P 0)"); aw_eval_ok("(set S 0)"); aw_eval_ok("(set W 0)"); aw_eval_ok("(set K 0)");
    PASS();
}

/* #813: SUM/AVG of `col +/- const` aggregate the column and add
 * bias * count at emit; the radix top-N ranks by that final value too, not
 * by the column's sum.  Groups 0..49 hold 60 rows each with w = 0, the
 * others about 2.5 rows each: with a +100 bias the large groups lead, with
 * -100 they trail, and the raw sum ranks them the other way.  Two
 * partitions; take 5 runs the fused per-partition scan, take 20 the serial
 * one. */
static test_result_t test_group_topn_affine(void) {
    aw_eval_ok("(set K (map (fn [i] (if (< i 3000) (% i 50) (% (* i 7919) 2000))) (til 8000)))");
    /* wn is nullable so the ordering SUM's non-null counts sit beside
     * another slot's: the heap must read its own slot's (agg_slot > 0) */
    aw_eval_ok("(set W (table [s w v wn] (list (at (as 'SYMBOL (map (fn [j] (format \"w%\" j)) (til 2000))) K) "
               "(map (fn [k] (if (< k 50) 0 (% k 13))) K) (til 8000) "
               "(as 'I64 (map (fn [k] (if (< (% k 5) 1) 0Nl (% k 7))) K)))))");
    aw_eval_ok("(.db.splayed.set \"" AW_TMP "/ta/2024.01.01/t/\" (take W 4000) \"" AW_TMP "/ta/.sym\")");
    aw_eval_ok("(.db.splayed.set \"" AW_TMP "/ta/2024.01.02/t/\" (drop W 4000) \"" AW_TMP "/ta/.sym\")");
    aw_eval_ok("(.db.splayed.set \"" AW_TMP "/tas/t/\" W \"" AW_TMP "/tas/.sym\")");
    aw_eval_ok("(set P (.db.parted.get \"" AW_TMP "/ta\" 't))");
    aw_eval_ok("(set S (.db.splayed.get \"" AW_TMP "/tas/t/\" \"" AW_TMP "/tas/.sym\"))");
    static const char* const shapes[] = {
        "{m: (sum (+ w 100)) from: %s by: {s: s z: (%% v 1) t: s} desc: m take: %d}",
        "{m: (sum (+ w 100)) from: %s by: {s: s z: (%% v 1) t: s} asc: m take: %d}",
        "{m: (sum (- w 100)) from: %s by: {s: s z: (%% v 1) t: s} desc: m take: %d}",
        "{m: (sum (- w 100)) from: %s by: {s: s z: (%% v 1) t: s} asc: m take: %d}",
        "{m: (sum (+ -100 w)) from: %s by: {s: s z: (%% v 1) t: s} desc: m take: %d}",
        "{m: (avg (+ w 100)) from: %s by: {s: s z: (%% v 1) t: s} desc: m take: %d}",
        "{m: (avg (+ w 100)) from: %s by: {s: s z: (%% v 1) t: s} asc: m take: %d}",
        "{m: (avg (- w 100)) from: %s by: {s: s z: (%% v 1) t: s} desc: m take: %d}",
        "{m: (avg (- w 100)) from: %s by: {s: s z: (%% v 1) t: s} asc: m take: %d}",
        "{a: (min wn) m: (sum (+ w 100)) from: %s by: {s: s z: (%% v 1) t: s} desc: m take: %d}",
        "{a: (min wn) m: (sum (+ w 100)) from: %s by: {s: s z: (%% v 1) t: s} asc: m take: %d}",
    };
    static const int takes[] = { 20, 5 };
    for (size_t si = 0; si < sizeof shapes / sizeof *shapes; si++)
        for (size_t ti = 0; ti < sizeof takes / sizeof *takes; ti++) {
            test_result_t r = aw_topn_check(shapes[si], takes[ti], 0, 2);
            if (r.status != TEST_PASS) return r;
        }
    aw_eval_ok("(set P 0)"); aw_eval_ok("(set S 0)"); aw_eval_ok("(set W 0)"); aw_eval_ok("(set K 0)");
    PASS();
}

/* csv-equal answers of two table expressions, under `what`. */
static test_result_t aw_tables_same(const char* what, const char* qa, const char* qb) {
    ray_t* a = aw_eval(qa);
    TEST_ASSERT_FMT(a && !RAY_IS_ERR(a), "%s: %s", qa, ray_error_msg());
    ray_t* b = aw_eval(qb);
    if (!b || RAY_IS_ERR(b)) ray_release(a);
    TEST_ASSERT_FMT(b && !RAY_IS_ERR(b), "%s: %s", qb, ray_error_msg());
    test_result_t r = aw_csv_eq(what, 0, a, b);
    ray_release(a); ray_release(b);
    return r;
}

/* #814: SUM of `col +/- c` over a nullable column is the sum of the
 * non-null rows' col + c, so the bias counts only the non-null rows (an
 * all-null group sums to 0), and AVG adds c to the non-null average.  wn is
 * all-null in every group with k mod 5 == 0 (the #808 fixture), wp is null
 * on every third row (partially null groups), wf is wp as F64.  Checked against the
 * in-memory table's `by: s` (the v2 engine): the parted `by: s`, the
 * 3-key parted shape (the serial emit), and the radix top-N (its values
 * against the in-memory full sort's first n, its rows against take: 1025).
 * The no-key answers, with and without a where:, over the in-memory,
 * splayed and parted tables, against the sum of the materialized input. */
static test_result_t test_group_affine_nulls(void) {
    aw_eval_ok("(set K (% (* (til 8000) 7919) 2000))");
    aw_eval_ok("(set W (table [s wn wp v] (list (at (as 'SYMBOL (map (fn [j] (format \"w%\" j)) (til 2000))) K) "
               "(as 'I64 (map (fn [k] (if (< (% k 5) 1) 0Nl (% k 13))) K)) "
               "(as 'I64 (map (fn [i] (if (== 0 (% i 3)) 0Nl (% i 11))) (til 8000))) (til 8000))))");
    aw_eval_ok("(set W (select {s: s wn: wn wp: wp v: v wf: (as 'F64 wp) from: W}))");
    aw_eval_ok("(.db.splayed.set \"" AW_TMP "/tz/2024.01.01/t/\" (take W 4000) \"" AW_TMP "/tz/.sym\")");
    aw_eval_ok("(.db.splayed.set \"" AW_TMP "/tz/2024.01.02/t/\" (drop W 4000) \"" AW_TMP "/tz/.sym\")");
    aw_eval_ok("(.db.splayed.set \"" AW_TMP "/tzs/t/\" W \"" AW_TMP "/tzs/.sym\")");
    aw_eval_ok("(set P (.db.parted.get \"" AW_TMP "/tz\" 't))");
    aw_eval_ok("(set S (.db.splayed.get \"" AW_TMP "/tzs/t/\" \"" AW_TMP "/tzs/.sym\"))");
    /* the inputs materialized: the reference for the scalar (no-key) answers */
    aw_eval_ok("(set X (select {a: (+ wn 100) b: (- wn 100) c: (+ wp 100) d: (- wp 100) e: (+ wf 100) v: v from: W}))");
    static const char* const aggs[] = {
        "(sum (+ wn 100))", "(sum (- wn 100))", "(sum (+ wp 100))", "(sum (- wp 100))",
        "(avg (+ wn 100))", "(avg (- wn 100))", "(avg (+ wp 100))", "(avg (- wp 100))",
        "(sum (+ wf 100))", "(avg (+ wf 100))",
    };
    static const char* const mats[] = {
        "(sum a)", "(sum b)", "(sum c)", "(sum d)", "(avg a)", "(avg b)", "(avg c)", "(avg d)",
        "(sum e)", "(avg e)",
    };
    static const char* const tbls[] = { "W", "S", "P" };
    static const char* const dirs[] = { "desc", "asc" };
    static const int takes[] = { 5, 20 };
    for (size_t ai = 0; ai < sizeof aggs / sizeof *aggs; ai++) {
        char qa[700], qb[700], shape[300];
        const char* ag = aggs[ai];
        /* no key, and under a where: */
        for (int t = 0; t < 3; t++) {
            snprintf(qa, sizeof qa, "(select {m: %s from: X})", mats[ai]);
            snprintf(qb, sizeof qb, "(select {m: %s from: %s})", ag, tbls[t]);
            test_result_t r = aw_tables_same(qb, qa, qb);
            if (r.status != TEST_PASS) return r;
            snprintf(qa, sizeof qa, "(select {m: %s from: X where: (< v 5000)})", mats[ai]);
            snprintf(qb, sizeof qb, "(select {m: %s from: %s where: (< v 5000)})", ag, tbls[t]);
            r = aw_tables_same(qb, qa, qb);
            if (r.status != TEST_PASS) return r;
        }
        /* every group's value, by key */
        snprintf(qa, sizeof qa, "(select {s: s m: m from: (select {m: %s from: W by: s}) asc: s})", ag);
        snprintf(qb, sizeof qb, "(select {s: s m: m from: (select {m: %s from: P by: s}) asc: s})", ag);
        test_result_t r = aw_tables_same(qb, qa, qb);
        if (r.status != TEST_PASS) return r;
        snprintf(qb, sizeof qb, "(select {s: s m: m from: (select {m: %s from: P by: {s: s z: (%% v 1) t: s}}) asc: s})", ag);
        r = aw_tables_same(qb, qa, qb);
        if (r.status != TEST_PASS) return r;
        for (int d = 0; d < 2; d++)
            for (size_t ti = 0; ti < sizeof takes / sizeof *takes; ti++) {
                int n = takes[ti];
                /* the cut's values are the full sort's first n, ties aside */
                snprintf(qa, sizeof qa, "(table [m] (list (take (at (select {m: %s from: W by: s %s: m}) 'm) %d)))",
                         ag, dirs[d], n);
                snprintf(qb, sizeof qb, "(table [m] (list (at (select {m: %s from: P by: {s: s z: (%% v 1) t: s} "
                         "%s: m take: %d}) 'm)))", ag, dirs[d], n);
                r = aw_tables_same(qb, qa, qb);
                if (r.status != TEST_PASS) return r;
                snprintf(shape, sizeof shape, "{m: %s from: %%s by: {s: s z: (%%%% v 1) t: s} %s: m take: %%d}",
                         ag, dirs[d]);
                r = aw_topn_check(shape, n, 0, 2);
                if (r.status != TEST_PASS) return r;
            }
    }
    aw_eval_ok("(set P 0)"); aw_eval_ok("(set S 0)"); aw_eval_ok("(set W 0)"); aw_eval_ok("(set K 0)");
    aw_eval_ok("(set X 0)");
    PASS();
}

/* Group-by on a generation-2 STR key through a query-wide string domain
 * (strdom.c): the key column t is never materialized and the groups equal
 * generation 0's. */
static void aw_str_key_tables(const char* tag, int n_part, int n_flat) {
    /* STR key with vectors of one repeated string and vectors of many (CONST
     * and RAW before P6; P6 selection makes every chunk STR_DICT, and the
     * forced-RAW twin keeps the RAW vectors),
     * equal strings crossing every vector boundary; two partitions, the
     * second offset and longer, a string in the first only */
    for (int gen = 0; gen <= 2; gen += 2)
        for (int p = 0; p < 2; p++) {
            char q[1000];
            int n = n_part + p * 1000, off = p * 777;
            snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/%sk%d/2024.01.0%d/t/\" "
                     "(table [t v] (list (map (fn [j] (if (and (== %d 0) (== (%% j 4000) 17)) \"in the first partition only\" "
                     "(if (< (%% j 2048) 1024) \"constant run text\" (format \"long key %%\" (%% j 37))))) (+ %d (til %d))) "
                     "(+ %d (til %d)))) {compress: %s})",
                     tag, gen, p + 1, p, off, n, off, n, gen ? "true" : "false");
            aw_eval_ok(q);
        }
    /* the unparted case: one column mixing CONST and RAW vectors */
    for (int gen = 0; gen <= 2; gen += 2) {
        char q[800];
        snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/%su%d/\" "
                 "(table [t v] (list (map (fn [j] (if (< (%% j 2048) 1024) \"constant run text\" "
                 "(format \"long key %%\" (%% j 37)))) (til %d)) (til %d))) {compress: %s})",
                 tag, gen, n_flat, n_flat, gen ? "true" : "false");
        aw_eval_ok(q);
    }
    /* CONST vectors beside a non-dictionary vector (P6): each chunk opens
     * with 1024 distinct long keys (no dictionary chunk, so that vector is
     * FSST or RAW), then runs of two constants, one per vector (CONST) */
    for (int gen = 0; gen <= 2; gen += 2) {
        char q[900];
        snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/%sc%d/\" "
                 "(table [t v] (list (map (fn [j] (if (< (%% j 65536) 1024) "
                 "(format \"https://www.example.com/path/%%/item?id=%%\" j (%% (* j 7919) 100003)) "
                 "(if (< (%% j 2048) 1024) \"constant run text\" \"another constant run\"))) (til %d)) (til %d))) "
                 "{compress: %s})",
                 tag, gen, n_flat, n_flat, gen ? "true" : "false");
        aw_eval_ok(q);
    }
    /* nulls: "" is the STR null (its canonical empty payload): the null and
     * "" rows are one group in either generation, beside short (inline) and
     * long (pooled) keys */
    for (int gen = 0; gen <= 2; gen += 2) {
        char q[800];
        snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/%sn%d/\" "
                 "(table [t v] (list (map (fn [j] (if (== (%% j 7) 3) \"\" (if (== (%% j 7) 1) "
                 "(format \"a long pooled key %%\" (%% j 3)) (format \"k%%\" (%% j 5))))) (til %d)) (til %d))) "
                 "{compress: %s})",
                 tag, gen, n_flat, n_flat, gen ? "true" : "false");
        aw_eval_ok(q);
    }
    /* first occurrence over a selection: z0 z1 z2, then the cycle b a m;
     * a filter past the z rows sees a m b first, in two partitions */
    for (int gen = 0; gen <= 2; gen += 2)
        for (int p = 0; p < 2; p++) {
            char q[800];
            snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/%so%d/2024.01.0%d/t/\" "
                     "(table [t v] (list (map (fn [j] (if (< j 3) (format \"z%%\" j) (at [\"b\" \"a\" \"m\"] (%% j 3)))) "
                     "(til %d)) (til %d))) {compress: %s})",
                     tag, gen, p + 1, n_part, n_part, gen ? "true" : "false");
            aw_eval_ok(q);
        }
}

/* Each query on table T, loaded as <tag><kind><gen> (k and o parted), on
 * both generations: equal formatted results.  AW_SK_KEY (and AW_SK_STRICT):
 * generation 2 leaves t unmaterialized (its vectors as `mix` says).
 * AW_SK_STRICT on a large table: nothing at all materializes; the others
 * may materialize v, as below RAY_PARALLEL_THRESHOLD rows the serial
 * whole-column strategy (R-RANDOM) reads the aggregates' column whole
 * whatever the key, and a composite key's expression is evaluated over its
 * materialized column (bind_referenced_columns).  AW_SK_DECLINE: shapes
 * the string-domain swap declines (the key column read by an aggregate or
 * another key), answers only. */
enum { AW_SK_DECLINE, AW_SK_KEY, AW_SK_STRICT };
typedef struct { char kind; int mode; const char* q; } aw_sk_q_t;
/* The STR key's encoding mix on generation 2 (kinds k and u; kind c holds
 * CONST vectors beside FSST / RAW ones under selection): what
 * selection gives the data (P6: every chunk a dictionary chunk, so every
 * vector STR_DICT, the constant runs included), or every vector RAW when
 * the tables were saved forced RAW (the RAW cursor path). */
enum { AW_SK_MIX_SELECT, AW_SK_MIX_RAW };
static test_result_t aw_str_key_check(const char* tag, const aw_sk_q_t* qs, bool strict_ok, int mix) {
    int64_t tsym = ray_sym_intern("t", 1);
    for (int i = 0; qs[i].q; i++) {
        ray_t* out[2];
        for (int gen = 0; gen <= 2; gen += 2) {
            char b[512];
            if (qs[i].kind == 'k' || qs[i].kind == 'o')
                snprintf(b, sizeof b, "(set T (.db.parted.get \"" AW_TMP "/%s%c%d\" 't))", tag, qs[i].kind, gen);
            else
                snprintf(b, sizeof b, "(set T (.db.splayed.get \"" AW_TMP "/%s%c%d/\"))", tag, qs[i].kind, gen);
            aw_eval_ok(b);
            ray_enc_counters_reset();
            out[gen / 2] = ray_eval_str(qs[i].q);
            TEST_ASSERT_FMT(out[gen / 2] && !RAY_IS_ERR(out[gen / 2]), "%s%c%d %s: %s", tag, qs[i].kind, gen,
                            qs[i].q, ray_error_msg());
        }
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        ray_t* t = ray_eval_str("T");
        ray_t* encv = ray_table_get_col_enc(t, tsym);
        ray_enccol_info_t info;
        TEST_ASSERT_TRUE(encv && !ray_enccol_info(encv, &info));
        ray_release(t);
        if (qs[i].mode != AW_SK_DECLINE) {
            TEST_ASSERT_FMT(!info.materialized, "%s%c %s: t materialized", tag, qs[i].kind, qs[i].q);
            if (qs[i].kind == 'c' && mix == AW_SK_MIX_SELECT)
                TEST_ASSERT_FMT(info.vectors[RAY_ENC_CONST] > 0 &&
                                info.vectors[RAY_ENC_FSST] + info.vectors[RAY_ENC_RAW] > 0,
                                "%sc: RAW %lld CONST %lld STR_DICT %lld FSST %lld", tag,
                                (long long)info.vectors[RAY_ENC_RAW], (long long)info.vectors[RAY_ENC_CONST],
                                (long long)info.vectors[RAY_ENC_STR_DICT], (long long)info.vectors[RAY_ENC_FSST]);
            else if (qs[i].kind != 'n' && qs[i].kind != 'o') {
                if (mix == AW_SK_MIX_SELECT)
                    TEST_ASSERT_FMT(info.vectors[RAY_ENC_STR_DICT] > 0 && info.vectors[RAY_ENC_RAW] == 0 &&
                                    info.vectors[RAY_ENC_CONST] == 0 && info.vectors[RAY_ENC_FSST] == 0,
                                    "%s%c: RAW %lld CONST %lld STR_DICT %lld FSST %lld", tag, qs[i].kind,
                                    (long long)info.vectors[RAY_ENC_RAW], (long long)info.vectors[RAY_ENC_CONST],
                                    (long long)info.vectors[RAY_ENC_STR_DICT], (long long)info.vectors[RAY_ENC_FSST]);
                else
                    TEST_ASSERT_FMT(info.vectors[RAY_ENC_RAW] > 0 && info.vectors[RAY_ENC_CONST] == 0 &&
                                    info.vectors[RAY_ENC_STR_DICT] == 0 && info.vectors[RAY_ENC_FSST] == 0,
                                    "%s%c: not all RAW", tag, qs[i].kind);
            }
        }
        if (strict_ok && qs[i].mode == AW_SK_STRICT)
            TEST_ASSERT_FMT(k.materialized == 0, "%s%c %s: %llu materializations", tag, qs[i].kind, qs[i].q,
                            (unsigned long long)k.materialized);
        ray_t* fx = ray_fmt(out[0], 0); ray_t* fy = ray_fmt(out[1], 0);
        TEST_ASSERT_FMT(ray_str_len(fx) == ray_str_len(fy) && memcmp(ray_str_ptr(fx), ray_str_ptr(fy), ray_str_len(fx)) == 0,
                        "%s%c %s differs", tag, qs[i].kind, qs[i].q);
        ray_release(fx); ray_release(fy); ray_release(out[0]); ray_release(out[1]);
    }
    aw_eval_ok("(set T 0)");
    PASS();
}

static const aw_sk_q_t* aw_str_key_queries(void);
static test_result_t aw_str_key_run(const char* ts, const char* tl, int mix) {
    /* ts: the brief's sizes (the serial strategy); tl: above the parallel
     * threshold, every partition too */
    aw_str_key_tables(ts, 5000, 10000);
    aw_str_key_tables(tl, 70000, 140000);
    test_result_t r = aw_str_key_check(ts, aw_str_key_queries(), false, mix);
    if (r.status != TEST_PASS) return r;
    return aw_str_key_check(tl, aw_str_key_queries(), true, mix);
}

/* The key column as selection writes it (P6: all STR_DICT).  Subject: the
 * string-domain path over STR_DICT vectors; the persisted DICT would take
 * the large tables' keys (P6b), so they are saved without it. */
static test_result_t test_str_key_domain(void) {
    ray_col_test_no_dict(true);
    test_result_t r = aw_str_key_run("s", "l", AW_SK_MIX_SELECT);
    ray_col_test_no_dict(false);
    return r;
}

/* P6: RAW STR vectors are this twin's subject (the cursor path the key took
 * before STR_DICT): the same tables saved with every STR vector forced RAW,
 * and without the persisted DICT, which would take the large tables' keys
 * (P6b). */
static test_result_t test_str_key_domain_raw(void) {
    ray_encw_test_force_str(RAY_ENC_RAW);
    ray_col_test_no_dict(true);
    test_result_t r = aw_str_key_run("rs", "rl", AW_SK_MIX_RAW);
    ray_col_test_no_dict(false);
    ray_encw_test_force_str(-1);
    return r;
}

/* P6b: the same tables as saved, the large ones with the persisted DICT:
 * a single-file key groups on its codes, a parted one remaps each DICT
 * segment's codes; the same answers, nothing of t materialized. */
static test_result_t test_str_key_domain_dict(void) {
    test_result_t r = aw_str_key_run("ds", "dl", AW_SK_MIX_SELECT);
    if (r.status != TEST_PASS) return r;
    /* not vacuous: a large single-file key and a large partition record it */
    static const char* const lds[] = {
        "(.db.splayed.get \"" AW_TMP "/dlu2/\")", "(.db.splayed.get \"" AW_TMP "/dlk2/2024.01.01/t/\")",
    };
    for (int i = 0; i < 2; i++) {
        ray_t* t = ray_eval_str(ray_test_tmp(lds[i]));
        TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
        ray_col_meta_t m;
        const bool ok = ray_table_col_meta(t, ray_sym_intern("t", 1), &m) && m.enc && m.index_kind == RAY_IDX_DICT;
        ray_release(t);
        TEST_ASSERT_FMT(ok, "%s: no persisted DICT", lds[i]);
    }
    PASS();
}

static const aw_sk_q_t* aw_str_key_queries(void) {
    /* group order is the route's; asc: makes the comparison order-insensitive */
    static const aw_sk_q_t qs[] = {
        { 'k', AW_SK_STRICT,  "(select {c: (count v) s: (sum v) from: T by: t asc: t})" },
        { 'k', AW_SK_KEY,     "(select {c: (count v) from: T by: {t: t m: (% v 3)} asc: [t m]})" },   /* composite */
        { 'u', AW_SK_STRICT,  "(select {c: (count v) s: (sum v) from: T by: t asc: t})" },
        { 'u', AW_SK_KEY,     "(select {c: (count v) from: T by: {t: t m: (% v 3)} asc: [t m]})" },
        { 'n', AW_SK_STRICT,  "(select {c: (count v) s: (sum v) from: T by: t asc: t})" },
        { 'n', AW_SK_KEY,     "(select {c: (count v) from: T by: {t: t m: (% v 3)} asc: [t m]})" },
        /* no asc: the ids are in first-occurrence order over the rows
         * read (the selection, under a filter), so a route emitting groups
         * by first occurrence (or by id) emits generation 0's order */
        { 'n', AW_SK_STRICT,  "(select {c: (count v) s: (sum v) from: T by: t})" },
        { 'u', AW_SK_STRICT,  "(select {c: (count v) s: (sum v) from: T by: t})" },
        { 'k', AW_SK_STRICT,  "(select {c: (count v) s: (sum v) from: T by: t})" },
        { 'o', AW_SK_STRICT,  "(select {c: (count v) from: T where: (> v 3) by: t})" },
        { 'o', AW_SK_STRICT,  "(select {c: (count v) from: T where: (> v 100) by: t})" },
        { 'u', AW_SK_STRICT,  "(select {c: (count v) from: T where: (> v 3000) by: t})" },
        /* a composite without asc: on a single-file column generation 0
         * groups on its DICT codes (RAY_DICT_MIN_ROWS rows and up) or as STR,
         * and the ids take the same routes; a parted one is not compared
         * unsorted (generation 0's segments group as STR, by first
         * occurrence of the tuple, where the ids take the dense route, in
         * slot order) */
        { 'u', AW_SK_KEY,     "(select {c: (count v) from: T by: {k: (% v 4) t: t}})" },
        { 'n', AW_SK_KEY,     "(select {c: (count v) from: T where: (> v 50) by: {t: t k: (% v 4)}})" },
        /* the swap declines: the key column is read by an aggregate or by
         * another key's expression */
        { 'u', AW_SK_DECLINE, "(select {f: (first t) c: (count v) from: T by: t asc: t})" },
        { 'n', AW_SK_DECLINE, "(select {c: (count t) from: T by: t asc: t})" },
        { 'k', AW_SK_DECLINE, "(select {d: (count (distinct t)) from: T by: t asc: t})" },
        { 'k', AW_SK_DECLINE, "(select {c: (count v) from: T by: {t: t u: (concat t \"x\")} asc: t})" },
        { 'n', AW_SK_DECLINE, "(select {c: (count v) from: T by: {t: t l: (strlen t)} asc: [t l]})" },
        /* CONST vectors (kind c): the key path's cursor over CONST and
         * FSST / RAW vectors, sorted and in first-occurrence order */
        { 'c', AW_SK_STRICT,  "(select {c: (count v) s: (sum v) from: T by: t asc: t})" },
        { 'c', AW_SK_STRICT,  "(select {c: (count v) s: (sum v) from: T by: t})" },
        { 'c', AW_SK_STRICT,  "(select {c: (count v) from: T where: (> v 1000) by: t})" },
        { 'c', AW_SK_KEY,     "(select {c: (count v) from: T by: {t: t m: (% v 3)} asc: [t m]})" },
        { 0, 0, NULL },
    };
    return qs;
}

/* ray_pool_dispatch clamped to a ring whose growth fails (#792; shrunk to a
 * few slots so the clamp runs at test sizes): fused where: filters
 * (pred_sel_drive and the expression selection, whose rowsel builders are
 * per grain), the scalar route and the other grain users answer as
 * unclamped. */
static test_result_t test_pool_clamp(void) {
    static test_result_t (*const fns[])(void) = {
        test_scalar_route_mixed, test_eq_runs_lack, test_filter_encoded_leaves, test_scalar_route_stats,
        test_count_on_runs, test_group_count_on_runs, test_topk_prune_cases,
        test_gather_sym_points, test_sort_head_late, test_group_topn_ties, test_group_topn_nulls,
        test_group_topn_affine, test_group_affine_nulls,
    };
    static const uint32_t caps[] = { 7, 3 };
    for (size_t c = 0; c < sizeof caps / sizeof *caps; c++)
        for (size_t i = 0; i < sizeof fns / sizeof *fns; i++) {
            (void)ray_test_rm_rf(aw_root()); (void)ray_test_mkdir_p(aw_root());
            ray_pool_test_ring_fail(caps[c]);
            g_aw_unaligned = true;
            test_result_t r = fns[i]();
            g_aw_unaligned = false;
            ray_pool_test_ring_fail(0);
            if (r.status != TEST_PASS) return r;
        }
    /* generation-0 callers of the grain: fused where (pred_sel_drive and the
     * expression selection), count distinct by group (cdpg_*), and the sort's
     * sortedness estimate, on an in-memory table */
    aw_eval_ok("(set M (table [k g x] (list (til 300000) (% (* (til 300000) 7919) 97) (% (* (til 300000) 31) 1009))))");
    static const char* const qs[] = {
        "(select {c: (count k) s: (sum x) from: M where: (< x 500)})",
        "(select {c: (count k) s: (sum x) from: M where: (in x [1 2 3 500 777])})",   /* pred_sel_worker */
        "(select {c: (count k) from: M where: (and (> x 100) (< g 50))})",
        "(select {u: (count (distinct x)) by: g from: M})",
        "(take (select {k: k x: x from: M asc: x}) 20)",
        "(take (select {k: k from: M where: (== (% x 3) 0) desc: k}) 5)",
        NULL,
    };
    for (int i = 0; qs[i]; i++) {
        ray_t* a = ray_eval_str(qs[i]);
        ray_pool_test_ring_fail(3);
        ray_t* b = ray_eval_str(qs[i]);
        ray_pool_test_ring_fail(0);
        TEST_ASSERT_FMT(a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b), "%s", qs[i]);
        ray_t* fa = ray_fmt(a, 0); ray_t* fb = ray_fmt(b, 0);
        TEST_ASSERT_FMT(ray_str_len(fa) == ray_str_len(fb) && memcmp(ray_str_ptr(fa), ray_str_ptr(fb), ray_str_len(fa)) == 0,
                        "%s: clamped dispatch differs", qs[i]);
        ray_release(fa); ray_release(fb); ray_release(a); ray_release(b);
    }
    /* count distinct per group over 60001 groups: the partitioned kernel
     * (cdpg_*), whose per-task counters follow the clamped geometry */
    {
        const int64_t n = 300000, ng = 60001;
        ray_t* src = ray_vec_new(RAY_I64, n);
        int64_t* gid = (int64_t*)malloc((size_t)n * sizeof(int64_t));
        TEST_ASSERT_TRUE(src && !RAY_IS_ERR(src) && gid);
        src->len = n;
        for (int64_t i = 0; i < n; i++) { ((int64_t*)ray_data(src))[i] = (i * 31) % 1009; gid[i] = (i * 7) % ng; }
        ray_t* a = ray_count_distinct_per_group(src, gid, n, ng);
        ray_pool_test_ring_fail(3);
        ray_t* b = ray_count_distinct_per_group(src, gid, n, ng);
        ray_pool_test_ring_fail(0);
        TEST_ASSERT_TRUE(a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b) && a->len == ng && b->len == ng);
        TEST_ASSERT_TRUE(memcmp(ray_data(a), ray_data(b), (size_t)ng * sizeof(int64_t)) == 0);
        ray_release(a); ray_release(b); ray_release(src); free(gid);
    }
    PASS();
}

/* ---- Task 12: the P3 gate --------------------------------------------- */

/* P3 gate: filtered, rowsel-gather and aggregate shapes, planning included. */
static test_result_t test_gate_materialization(void) {
    aw_save("gm", AW_WIDE, -1);
    static const char* const q[] = {
        "(select {from: T where: (and (>= k 1000) (< k 3000))})",                   /* filter + gather */
        "(select {k: k t: t from: T where: (== s 'b) take: 9})",                    /* SYM equality + take */
        "(select {s: (sum v) a: (avg v) c: (count t) from: T where: (< g 3)})",     /* aggregates under a filter */
        "(select {c: (count k) from: T where: (!= s 'a) by: g})",
        "(select {c: (count k) from: T by: t})",                                    /* STR key */
        "(sum (at T 'v))",
        NULL,
    };
    for (int i = 0; q[i]; i++) AW_SAME("gm", q[i], 0);
    AW_SAME("gm", "(select {t: t from: T where: (> v 10) desc: v take: 3})", 0);   /* top-k: the sort key read by range */
    /* a staged scalar count reads no values: t decodes nothing, and ""
     * (STR null) rows count, as on generation 0 */
    AW_SAME("gm", "(select {c: (count t) from: T where: (< g 3)})", 0);
    TEST_ASSERT_FMT(aw_col_decoded("t") == 0, "t decoded %llu", (unsigned long long)aw_col_decoded("t"));
    aw_save("gn", "(table [g t] (list (as 'I16 (% (til 70000) 7)) "
                  "(map (fn [j] (if (== 0 (% j 5)) \"\" (format \"text value %\" (% j 333)))) (til 70000))))", -1);
    AW_SAME("gn", "(select {c: (count t) d: (count g) from: T where: (< g 3)})", 0);
    TEST_ASSERT_FMT(aw_col_decoded("t") == 0, "t decoded %llu", (unsigned long long)aw_col_decoded("t"));
    PASS();
}

/* Decoded bytes: zero where statistics or codes answer, exact otherwise. */
static test_result_t test_gate_decoded_bytes(void) {
    aw_save("gd", "(table [k x] (list (til 70000) (as 'I32 (% (til 70000) 50))))", RAY_ENC_FOR_BP);
    test_result_t r;
    if ((r = aw_reduce("gd", "(select {c: (count k) from: T where: (== x 7)})", 0)).status != TEST_PASS) return r;
    if ((r = aw_reduce("gd", "(select {s: (sum x) from: T})", 0)).status != TEST_PASS) return r;
    /* k's leaf runs on FOR_BP.  10 selected rows in x's vector 0 are under
     * the FOR_BP point-read limit (RAY_ENC_VEC/32, Task 6b's D2), so they
     * are read in place and nothing decodes; 100 rows are over it, so x's
     * vector 0 (1024 I32 rows) is decoded and gathered. */
    if ((r = aw_reduce("gd", "(select {x: x from: T where: (< k 10)})", 0)).status != TEST_PASS) return r;
    if ((r = aw_reduce("gd", "(select {x: x from: T where: (< k 100)})", 1024 * 4)).status != TEST_PASS) return r;
    PASS();
}

/* Cross-segment codes: integer VALUE_DICT per partition, the same value
 * under different codes and the same code for different values. */
static test_result_t test_cross_segment_codes(void) {
    for (int p = 0; p < 2; p++) {
        char q[600];
        /* v repeats within 997 values: every column of the table is forced
         * to DICT, and the chunk-0 dictionary (65536 entries at most) must
         * cover chunk 1's values */
        snprintf(q, sizeof q, "(table [x v] (list (as 'I32 %s) (%% (til 70000) 997)))",
                 p ? "(- (% (til 70000) 40) 1000)" : "(% (til 70000) 40)");   /* -1000..-961 / 0..39 */
        char name[16]; snprintf(name, sizeof name, "cs%d", p);
        aw_save(name, q, RAY_ENC_DICT);
    }
    /* assemble both generation-2 tables as partitions of one parted table */
    ray_encw_test_force(RAY_ENC_DICT);          /* the re-encode keeps each partition's own VALUE_DICT */
    aw_eval_ok("(.db.splayed.set \"" AW_TMP "/csp/2024.01.01/t/\" (.db.splayed.get \"" AW_TMP "/cs02/\") \"" AW_TMP "/csp/.sym\")");
    aw_eval_ok("(.db.splayed.set \"" AW_TMP "/csp/2024.01.02/t/\" (.db.splayed.get \"" AW_TMP "/cs12/\") \"" AW_TMP "/csp/.sym\")");
    ray_encw_test_force(-1);
    aw_eval_ok("(.db.splayed.set \"" AW_TMP "/csq/2024.01.01/t/\" (.db.splayed.get \"" AW_TMP "/cs00/\") \"" AW_TMP "/csq/.sym\")");
    aw_eval_ok("(.db.splayed.set \"" AW_TMP "/csq/2024.01.02/t/\" (.db.splayed.get \"" AW_TMP "/cs10/\") \"" AW_TMP "/csq/.sym\")");
    /* the re-saves inherit generation 2 from their encoded sources, and
     * generation 0 from their plain ones */
    for (int gen = 0; gen <= 2; gen += 2) {
        ray_t* g = aw_eval(gen ? "(at (.idx.info (.db.parted.get \"" AW_TMP "/csp\" 't) 'x) 'gen)"
                               : "(at (.idx.info (.db.parted.get \"" AW_TMP "/csq\" 't) 'x) 'gen)");
        TEST_ASSERT_FMT(g && !RAY_IS_ERR(g) && g->i64 == gen, "generation %d: %lld", gen,
                        g && !RAY_IS_ERR(g) ? (long long)g->i64 : -1LL);
        ray_release(g);
    }
    /* every vector of x in both partitions (2 x 69) is DICT over its own
     * partition's dictionary */
    ray_t* nd = aw_eval("(at (at (.idx.info (.db.parted.get \"" AW_TMP "/csp\" 't) 'x) 'enc) 'dict)");
    TEST_ASSERT_FMT(nd && !RAY_IS_ERR(nd) && nd->i64 == 138, "DICT vectors: %lld",
                    nd && !RAY_IS_ERR(nd) ? (long long)nd->i64 : -1LL);
    ray_release(nd);
    static const char* const qs[] = {
        "(count (select {from: (.db.parted.get \"" AW_TMP "/%s\" 't) where: (== x 5)}))",
        "(count (select {from: (.db.parted.get \"" AW_TMP "/%s\" 't) where: (< x -990)}))",
        "(select {c: (count v) from: (.db.parted.get \"" AW_TMP "/%s\" 't) by: x asc: x})",
        "(select {v: v from: (.db.parted.get \"" AW_TMP "/%s\" 't) where: (in x [3 -997])})",
        NULL,
    };
    for (int i = 0; qs[i]; i++) {
        char a[512], b[512];
        snprintf(a, sizeof a, qs[i], "csq"); snprintf(b, sizeof b, qs[i], "csp");
        ray_t* x = aw_eval(a);
        ray_enc_counters_reset();
        ray_t* y = aw_eval(b);
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        TEST_ASSERT_TRUE(x && y && !RAY_IS_ERR(x) && !RAY_IS_ERR(y));
        TEST_ASSERT_FMT(k.materialized == 0, "query %d materialized %llu", i, (unsigned long long)k.materialized);
        /* `5` matches 1750 rows of partition 0, `< -990` 17500 of partition
         * 1: equal counts of zero would pin nothing (checked on the
         * generation-0 answer; the generation-2 one equals it below) */
        if (i < 2) TEST_ASSERT_FMT(x->i64 == (i ? 17500 : 1750), "query %d counted %lld", i, (long long)x->i64);
        ray_t* fx = ray_fmt(x, 0); ray_t* fy = ray_fmt(y, 0);
        TEST_ASSERT_FMT(ray_str_len(fx) == ray_str_len(fy) && memcmp(ray_str_ptr(fx), ray_str_ptr(fy), ray_str_len(fx)) == 0, "query %d", i);
        ray_release(fx); ray_release(fy); ray_release(x); ray_release(y);
    }
    PASS();
}

/* A parted `in` / `not in` filter over generation 2 reads each segment by
 * range through its own source and probe (exec_in_parted_selection), at
 * any position in the where: chain: first (the fused selection), later
 * (intersected with the selection installed), after a selective `==` (the
 * survivors' compact), grouped and row-returning.  Three partitions of
 * 70000 rows (segments end inside a morsel); x's values differ per
 * partition; nullable integer, SYM and F64 columns, and w with nulls only
 * after partition 0 (each segment's own probe and null flag).  The chained
 * shapes run under a selection with dropped (NONE) morsels and under one
 * mixed in every morsel.  Answers equal generation 0's; nothing
 * materializes. */
static test_result_t test_parted_in(void) {
    for (int gen = 0; gen <= 2; gen += 2)
        for (int p = 0; p < 3; p++) {
            char q[1600];
            snprintf(q, sizeof q,
                     "(.db.splayed.set \"" AW_TMP "/pi%d/2024.01.0%d/t/\" (table [k x v s f w] (list (+ (til 70000) %d) "
                     "(as 'I32 (- (%% (til 70000) 40) %d)) "
                     "(as 'I32 (map (fn [j] (if (== 0 (%% j 9)) 0N (%% j 1000))) (til 70000))) "
                     "(take ['a 'b 'c 'd 'e] 70000) (as 'F64 (%% (til 70000) 50)) %s)) "
                     "\"" AW_TMP "/pi%d/.sym\" {compress: %s})",
                     gen, p + 1, p * 70000, p * 500,
                     p ? "(as 'I32 (map (fn [j] (if (== 0 (% j 9)) 0N (% j 1000))) (til 70000)))"
                       : "(as 'I32 (% (til 70000) 1000))",
                     gen, gen ? "true" : "false");
            aw_eval_ok(q);
        }
    static const char* const qs[] = {
        "(select {from: T where: (in x [3 -497 -980 5])})",                              /* first, row-returning */
        "(count (select {from: T where: (not-in x [3 -497 -980 5])}))",
        "(select {c: (count k) s: (sum v) from: T where: (and (> v 10) (in x [3 -497 -980])) by: x asc: x})",   /* second, grouped */
        "(select {c: (count k) from: T where: (and (> v 10) (in x [3 -497]) (< v 900)) by: s asc: s})",          /* middle */
        "(select {c: (count k) from: T where: (and (> v 10) (not-in x [3 -497 -998]) (< v 900))})",
        "(select {k: k v: v from: T where: (and (== k 70005) (in x [5 -495 -1000]))})",   /* after a selective == */
        "(select {c: (count k) from: T where: (in v [1 2 3 9 500])})",                    /* nullable column */
        "(select {c: (count k) from: T where: (not-in v [1 2 3 9 500])})",
        "(select {c: (count k) from: T where: (and (> k 1000) (in s ['a 'c])) by: s asc: s})",
        "(select {c: (count k) from: T where: (not-in s ['a 'c])})",
        "(select {c: (count k) from: T where: (and (> k 1000) (in f [1.0 7.0 -0.0]))})",
        /* w has nulls only after partition 0: each segment answers with its
         * own null flag */
        "(select {c: (count k) from: T where: (in w [1 2 3 9 500])})",
        "(select {c: (count k) from: T where: (not-in w [1 2 3 9 500])})",
        "(select {c: (count k) from: T where: (and (> k 1000) (not-in w [1 2 3 9 500])) by: s asc: s})",
        /* x matches partition 0 only, so it is installed first and the
         * selection drops every morsel of partitions 1-2 (NONE): the s
         * conjunct runs through the chained route, in both written orders
         * and with not-in */
        "(count (select {from: T where: (and (in x [0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39]) (in s ['a 'c]))}))",
        "(count (select {from: T where: (and (in s ['a 'c]) (in x [0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39]))}))",
        "(count (select {from: T where: (and (not-in x [-500 -499 -498 -497 -496 -495 -494 -493 -492 -491 -490 -489 -488 -487 -486 -485 -484 -483 -482 -481 -480 -479 -478 -477 -476 -475 -474 -473 -472 -471 -470 -469 -468 -467 -466 -465 -464 -463 -462 -461 -1000 -999 -998 -997 -996 -995 -994 -993 -992 -991 -990 -989 -988 -987 -986 -985 -984 -983 -982 -981 -980 -979 -978 -977 -976 -975 -974 -973 -972 -971 -970 -969 -968 -967 -966 -965 -964 -963 -962 -961]) (in s ['a 'c]))}))",
        "(count (select {from: T where: (and (in s ['a 'c]) (not-in x [-500 -499 -498 -497 -496 -495 -494 -493 -492 -491 -490 -489 -488 -487 -486 -485 -484 -483 -482 -481 -480 -479 -478 -477 -476 -475 -474 -473 -472 -471 -470 -469 -468 -467 -466 -465 -464 -463 -462 -461 -1000 -999 -998 -997 -996 -995 -994 -993 -992 -991 -990 -989 -988 -987 -986 -985 -984 -983 -982 -981 -980 -979 -978 -977 -976 -975 -974 -973 -972 -971 -970 -969 -968 -967 -966 -965 -964 -963 -962 -961]))}))",
        "(count (select {from: T where: (and (in x [0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39]) (not-in s ['a 'c]))}))",
        /* s is installed first (mixed in every morsel) and x runs chained:
         * x == 4 rows are all 'e, outside the selection */
        "(count (select {from: T where: (and (in s ['a 'b 'c 'd]) (in x [3 4 -497 -496 -980]))}))",
        "(select {c: (count k) from: T where: (and (in s ['a 'b 'c 'd]) (in x [3 4 -497 -496 -980])) by: x asc: x})",
        "(select {c: (count k) from: T where: (in x [12345])})",                          /* matches nothing */
        "(select {c: (count k) from: T where: (and (> k 1000) (not-in x [100000]))})",    /* every row */
        NULL,
    };
    for (int i = 0; qs[i]; i++) {
        ray_t* out[2];
        for (int gen = 0; gen <= 2; gen += 2) {
            char b[256];
            snprintf(b, sizeof b, "(set T (.db.parted.get \"" AW_TMP "/pi%d\" 't))", gen);
            aw_eval_ok(b);
            ray_enc_counters_reset();
            out[gen / 2] = ray_eval_str(qs[i]);
            TEST_ASSERT_FMT(out[gen / 2] && !RAY_IS_ERR(out[gen / 2]), "%s gen %d: %s", qs[i], gen, ray_error_msg());
            if (gen) {
                ray_enc_counters_t k; ray_enc_counters_get(&k);
                TEST_ASSERT_FMT(k.materialized == 0, "%s: %llu materializations", qs[i], (unsigned long long)k.materialized);
            }
        }
        ray_t* fa = ray_fmt(out[0], 0); ray_t* fb = ray_fmt(out[1], 0);
        TEST_ASSERT_FMT(ray_str_len(fa) == ray_str_len(fb) && memcmp(ray_str_ptr(fa), ray_str_ptr(fb), ray_str_len(fa)) == 0,
                        "%s differs", qs[i]);
        ray_release(fa); ray_release(fb); ray_release(out[0]); ray_release(out[1]);
    }
    aw_eval_ok("(set T 0)");
    PASS();
}

/* The results of `q` in generation 0 (a) and generation 2 (b), both owned:
 * either the same error code, or the same type and formatted result. */
static test_result_t aw_cmp_pair(const char* q, ray_t* a, ray_t* b) {
    TEST_ASSERT_FMT(a && b, "%s: no result (gen 0 %p, gen 2 %p)", q, (void*)a, (void*)b);
    bool e0 = RAY_IS_ERR(a), e2 = RAY_IS_ERR(b);
    TEST_ASSERT_FMT(e0 == e2, "%s: error in one generation only", q);
    if (e0) {
        TEST_ASSERT_FMT(strcmp(ray_err_code(a), ray_err_code(b)) == 0, "%s: %s vs %s", q,
                        ray_err_code(a), ray_err_code(b));
        ray_error_free(a); ray_error_free(b);
        PASS();
    }
    TEST_ASSERT_FMT(a->type == b->type, "%s: type %d vs %d", q, a->type, b->type);
    ray_t* fa = ray_fmt(a, 0); ray_t* fb = ray_fmt(b, 0);
    TEST_ASSERT_FMT(ray_str_len(fa) == ray_str_len(fb) && memcmp(ray_str_ptr(fa), ray_str_ptr(fb), ray_str_len(fa)) == 0, "%s differs", q);
    ray_release(fa); ray_release(fb); ray_release(a); ray_release(b);
    PASS();
}

/* Either the same error code in both generations, or equal formatted results. */
static test_result_t aw_same_or_err(const char* name, const char* q) {
    char buf[2048]; ray_t* out[2];
    for (int gen = 0; gen < 2; gen++) {
        snprintf(buf, sizeof buf, "(set T (.db.splayed.get \"" AW_TMP "/%s%d/\"))", name, gen ? 2 : 0);
        aw_eval_ok(buf);
        out[gen] = aw_eval(q);
    }
    return aw_cmp_pair(q, out[0], out[1]);
}
#define AW_SAME_OR_ERR(n, q) do { test_result_t r_ = aw_same_or_err((n), (q)); if (r_.status != TEST_PASS) return r_; } while (0)

/* `q` formats as `want` in both generations: an absolute pin, for the
 * properties gen 0 and gen 2 share code for (a shared mistake passes
 * aw_same_or_err). */
static test_result_t aw_expect(const char* name, const char* q, const char* want) {
    char buf[2048];
    for (int gen = 0; gen < 2; gen++) {
        snprintf(buf, sizeof buf, "(set T (.db.splayed.get \"" AW_TMP "/%s%d/\"))", name, gen ? 2 : 0);
        aw_eval_ok(buf);
        ray_t* r = aw_eval(q);
        TEST_ASSERT_FMT(r && !RAY_IS_ERR(r), "%s gen %d: %s", q, gen ? 2 : 0, r ? ray_error_msg() : "no result");
        ray_t* f = ray_fmt(r, 0);
        bool ok = ray_str_len(f) == strlen(want) && memcmp(ray_str_ptr(f), want, strlen(want)) == 0;
        TEST_ASSERT_FMT(ok, "%s gen %d: %.*s, want %s", q, gen ? 2 : 0, (int)ray_str_len(f), ray_str_ptr(f), want);
        ray_release(f); ray_release(r);
    }
    PASS();
}
#define AW_EXPECT(n, q, w) do { test_result_t r_ = aw_expect((n), (q), (w)); if (r_.status != TEST_PASS) return r_; } while (0)

/* Saved SYM width class of column `col` of table <name>2, or -1. */
static int aw_sym_width(const char* name, const char* col) {
    ray_t* t = aw_load(name, 2);
    ray_t* encv = t ? ray_table_get_col_enc(t, ray_sym_intern(col, strlen(col))) : NULL;
    ray_enccol_t* e = encv ? ray_enccol_of(encv) : NULL;
    int w = e ? (int)(e->attrs & RAY_SYM_W_MASK) : -1;
    if (t) ray_release(t);
    return w;
}

/* Rows of column f of the loaded T holding -0.0 / +0.0 (by sign bit), read
 * from the evaluated column; -1 if it does not come back as a plain F64. */
static void aw_zero_signs(int64_t* neg, int64_t* pos) {
    *neg = *pos = -1;
    ray_t* r = ray_eval_str("(at T 'f)");
    if (r && !RAY_IS_ERR(r) && r->type == RAY_F64) {
        const double* d = (const double*)ray_data(r);
        *neg = *pos = 0;
        for (int64_t i = 0; i < r->len; i++)
            if (d[i] == 0.0) { if (signbit(d[i])) (*neg)++; else (*pos)++; }
    }
    if (r && RAY_IS_ERR(r)) ray_error_free(r); else if (r) ray_release(r);
}

/* Both generations of <name> hold exactly `neg` -0.0 and `pos` +0.0 rows. */
static test_result_t aw_zero_rows(const char* name, int64_t neg, int64_t pos) {
    char buf[512];
    for (int gen = 0; gen < 2; gen++) {
        snprintf(buf, sizeof buf, "(set T (.db.splayed.get \"" AW_TMP "/%s%d/\"))", name, gen ? 2 : 0);
        aw_eval_ok(buf);
        int64_t n, p; aw_zero_signs(&n, &p);
        TEST_ASSERT_FMT(n == neg && p == pos, "%s gen %d: %lld -0.0 / %lld +0.0 rows, want %lld / %lld", name,
                        gen ? 2 : 0, (long long)n, (long long)p, (long long)neg, (long long)pos);
    }
    PASS();
}
#define AW_ZERO_ROWS(n, a, b) do { test_result_t r_ = aw_zero_rows((n), (a), (b)); if (r_.status != TEST_PASS) return r_; } while (0)

/* The sign bit of the F64 result of `q` (an atom or a one-element vector)
 * is the same in both generations. */
static test_result_t aw_zero_mm(const char* name, const char* q) {
    char buf[512]; int sb[2];
    for (int gen = 0; gen < 2; gen++) {
        snprintf(buf, sizeof buf, "(set T (.db.splayed.get \"" AW_TMP "/%s%d/\"))", name, gen ? 2 : 0);
        aw_eval_ok(buf);
        ray_t* r = aw_eval(q);
        TEST_ASSERT_FMT(r && !RAY_IS_ERR(r), "%s gen %d failed", q, gen ? 2 : 0);
        double x;
        if (r->type == -RAY_F64) x = r->f64;
        else { TEST_ASSERT_FMT(r->type == RAY_F64 && r->len == 1, "%s: type %d", q, r->type); x = ((const double*)ray_data(r))[0]; }
        TEST_ASSERT_FMT(x == 0.0, "%s gen %d: not a zero", q, gen ? 2 : 0);
        sb[gen] = signbit(x) ? 1 : 0;
        ray_release(r);
    }
    TEST_ASSERT_FMT(sb[0] == sb[1], "%s: sign bit %d in gen 0, %d in gen 2", q, sb[0], sb[1]);
    PASS();
}
#define AW_ZERO_MM(n, q) do { test_result_t r_ = aw_zero_mm((n), (q)); if (r_.status != TEST_PASS) return r_; } while (0)

/* The szc table <name>2 (vector v of 1024 rows: -0.0 when v % 3 == 0,
 * +0.0 when 1, alternating -0.0 / +0.0 when 2), over 64 Ki rows: min / max
 * of a selection keep the first selected row's zero under the scalar-grain
 * hook (row order, one task). */
static test_result_t aw_szc_first_zero(const char* name) {
    char q[256];
    snprintf(q, sizeof q, "(set T (.db.splayed.get \"" AW_TMP "/%s2/\"))", name);
    aw_eval_ok(q);
    static const int64_t los[] = { -1, 0, 1000, 1023, 1024, 2047, 64511, 65000, 65535, 65536, 66000 };
    static const int64_t his[] = { 131072, 65536, 65537, 66560, 70000 };
    static const int64_t grains[] = { RAY_COL_BLOCK_ROWS, 131072 };
    for (int g = 0; g < 2; g++)
    for (int l = 0; l < 11; l++) for (int h = 0; h < 5; h++) for (int m = 0; m < 2; m++) {
        if (his[h] <= los[l] + 1) continue;
        int64_t r0 = los[l] + 1, v0 = r0 / 1024;           /* the first selected row */
        bool want_neg = v0 % 3 == 0 || (v0 % 3 == 2 && r0 % 2 == 0);
        snprintf(q, sizeof q, "(at (select {r: (%s f) from: T where: (and (> k %lld) (< k %lld))}) 'r)",
                 m ? "max" : "min", (long long)los[l], (long long)his[h]);
        ray_group_test_scalar_grain(grains[g]);
        ray_t* r = aw_eval(q);
        ray_group_test_scalar_grain(0);
        TEST_ASSERT_FMT(r && !RAY_IS_ERR(r) && r->type == RAY_F64 && r->len == 1, "%s failed", q);
        double x = ((const double*)ray_data(r))[0];
        ray_release(r);
        TEST_ASSERT_FMT(x == 0.0 && (signbit(x) != 0) == want_neg, "%s, grain %lld: %s0.0, want %s0.0", q,
                        (long long)grains[g], signbit(x) ? "-" : "+", want_neg ? "-" : "+");
    }
    PASS();
}

/* The signed-zero tables [k f] built in C and saved as aw_save does: the
 * zeros are bit patterns (D10).  Rayfall cannot build them in a release
 * build, where the literal 0.0 parses as -0.0 (#850, -fno-signed-zeros) and
 * (neg 0.0) is then +0.0.  shape 0 (sz): rows < 1024 -0.0, < 2048 +0.0, then
 * -0.0 on even rows and 0.5 * (j - 2600) on odd ones; shape 1 (sz0): the
 * same with +0.0 on the odd rows; shape 2 (szc): by vector v = j / 1024,
 * -0.0 when v % 3 == 0, +0.0 when 1, -0.0 / +0.0 on even / odd rows when 2. */
static void aw_save_zeros(const char* name, int shape, int64_t n, int enc) {
    const uint64_t neg0 = 1ull << 63, pos0 = 0;
    ray_t* k = ray_vec_new(RAY_I64, n);
    ray_t* f = ray_vec_new(RAY_F64, n);
    if (!k || !f) { fprintf(stderr, "aw_save_zeros: oom\n"); abort(); }
    k->len = f->len = n;
    for (int64_t j = 0; j < n; j++) {
        ((int64_t*)ray_data(k))[j] = j;
        uint64_t b;
        if (shape == 2) {
            int64_t v = j / 1024;
            b = v % 3 == 0 ? neg0 : v % 3 == 1 ? pos0 : (j % 2 == 0 ? neg0 : pos0);
        } else if (j < 1024) b = neg0;
        else if (j < 2048)   b = pos0;
        else if (j % 2 == 0) b = neg0;
        else if (shape == 1) b = pos0;
        else { double x = 0.5 * (double)(j - 2600); memcpy(&b, &x, 8); }
        memcpy((double*)ray_data(f) + j, &b, 8);
    }
    ray_t* tb = ray_table_new(2);
    tb = ray_table_add_col(tb, ray_sym_intern("k", 1), k); ray_release(k);
    tb = ray_table_add_col(tb, ray_sym_intern("f", 1), f); ray_release(f);
    if (ray_env_set(ray_sym_intern("W", 1), tb) != RAY_OK) { fprintf(stderr, "aw_save_zeros: bind\n"); abort(); }
    ray_release(tb);
    char q[512];
    snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/%s0/\" W {compress: false})", name);
    aw_eval_ok(q);
    ray_encw_test_force(enc);
    snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/%s2/\" W {compress: true})", name);
    aw_eval_ok(q);
    ray_encw_test_force(-1);
}

/* Signed zeros: CONST vectors of -0.0 and of +0.0 next to a RAW vector
 * mixing both; every comparison with both zero literals, in, min, max (which
 * zero) and a float top-k. */
static test_result_t test_signed_zeros(void) {
    /* The zeros are built in C (aw_save_zeros): inside one lambda the
     * literals -0.0 and 0.0 share a constant-pool slot (compile.c
     * add_constant compares by ==, #824), and in a release build the literal
     * 0.0 is itself -0.0 (#850), so no rayfall form holds both zeros there. */
    aw_save_zeros("sz", 0, 3072, -1);
    TEST_ASSERT_EQ_I(aw_vec_enc("sz", "f", 0), RAY_ENC_CONST);
    TEST_ASSERT_EQ_I(aw_vec_enc("sz", "f", 1), RAY_ENC_CONST);
    AW_ZERO_ROWS("sz", 1024 + 512, 1024);
    static const char* const ops[] = { "==", "!=", "<", "<=", ">", ">=" };
    static const char* const lits[] = { "0.0", "-0.0" };
    char q[256];
    for (int o = 0; o < 6; o++) for (int l = 0; l < 2; l++) {
        snprintf(q, sizeof q, "(count (select {from: T where: (%s f %s)}))", ops[o], lits[l]);
        AW_SAME_OR_ERR("sz", q);
    }
    AW_SAME_OR_ERR("sz", "(count (select {from: T where: (in f [0.0 1.5])}))");
    AW_SAME_OR_ERR("sz", "(count (select {from: T where: (in f [-0.0])}))");
    AW_SAME_OR_ERR("sz", "(min (at T 'f))");
    AW_SAME_OR_ERR("sz", "(max (at T 'f))");
    AW_SAME_OR_ERR("sz", "(select {f: f k: k from: T where: (> k 1000) asc: f take: 5})");
    AW_SAME_OR_ERR("sz", "(select {f: f k: k from: T where: (> k 1000) desc: f take: 5})");
    /* Zeros only: which zero min / max return (1/x tells them apart) and how
     * the top-k orders -0.0 against 0.0 (k shows the rows taken). */
    aw_save_zeros("sz0", 1, 3072, -1);
    TEST_ASSERT_EQ_I(aw_vec_enc("sz0", "f", 0), RAY_ENC_CONST);
    TEST_ASSERT_EQ_I(aw_vec_enc("sz0", "f", 1), RAY_ENC_CONST);
    /* P5: the mixed +/-0.0 vector has two distinct values, so selection now
     * takes DICT (it was RAW); the sign answers below hold for either body. */
    TEST_ASSERT_EQ_I(aw_vec_enc("sz0", "f", 2), RAY_ENC_DICT);
    AW_ZERO_ROWS("sz0", 1024 + 512, 1024 + 512);
    /* Rayfall prints both zeros as 0.0 and 1/0 is null, so the sign of a
     * min / max result is read in C: aw_zero_mm compares it across generations. */
    AW_ZERO_MM("sz0", "(min (at T 'f))");
    AW_ZERO_MM("sz0", "(max (at T 'f))");
    AW_ZERO_MM("sz0", "(at (select {r: (min f) from: T where: (> k 1000)}) 'r)");
    AW_ZERO_MM("sz0", "(at (select {r: (max f) from: T where: (> k 1000)}) 'r)");
    AW_SAME_OR_ERR("sz0", "(select {f: f k: k from: T where: (> k 1000) asc: f take: 5})");
    AW_SAME_OR_ERR("sz0", "(select {f: f k: k from: T where: (> k 1000) desc: f take: 5})");
    AW_SAME_OR_ERR("sz0", "(select {f: f k: k from: T asc: f take: 5})");
    AW_SAME_OR_ERR("sz0", "(select {f: f k: k from: T desc: f take: 5})");
    /* P5: the same mix as a RAW body (the pre-P5 shape): every vector RAW */
    aw_save_zeros("sz0r", 1, 3072, RAY_ENC_RAW);
    TEST_ASSERT_EQ_I(aw_vec_enc("sz0r", "f", 2), RAY_ENC_RAW);
    AW_ZERO_ROWS("sz0r", 1024 + 512, 1024 + 512);
    AW_ZERO_MM("sz0r", "(min (at T 'f))");
    AW_ZERO_MM("sz0r", "(max (at T 'f))");
    AW_ZERO_MM("sz0r", "(at (select {r: (min f) from: T where: (> k 1000)}) 'r)");
    AW_ZERO_MM("sz0r", "(at (select {r: (max f) from: T where: (> k 1000)}) 'r)");
    AW_SAME_OR_ERR("sz0r", "(select {f: f k: k from: T where: (> k 1000) asc: f take: 5})");
    AW_SAME_OR_ERR("sz0r", "(select {f: f k: k from: T where: (> k 1000) desc: f take: 5})");
    AW_SAME_OR_ERR("sz0r", "(select {f: f k: k from: T asc: f take: 5})");
    AW_SAME_OR_ERR("sz0r", "(select {f: f k: k from: T desc: f take: 5})");
    /* Two chunks of zero vectors: -0.0 CONST, +0.0 CONST, a RAW mix, in
     * turn.  Selections cut inside vectors, at vector and chunk edges, so
     * vectors answered from statistics, row-read runs and whole chunks
     * alternate in every order.  Over 64 Ki rows both generations dispatch
     * in parallel and the merge of the workers' zeros is not row order, so
     * generation 2 runs in row order under the scalar-grain hook (whole
     * blocks, and one task) and must keep the first selected row's zero. */
    aw_save_zeros("szc", 2, 131072, -1);
    TEST_ASSERT_EQ_I(aw_vec_enc("szc", "f", 63), RAY_ENC_CONST);
    TEST_ASSERT_EQ_I(aw_vec_enc("szc", "f", 64), RAY_ENC_CONST);
    /* P5: the +/-0.0 mix now selects DICT (it was RAW) */
    TEST_ASSERT_EQ_I(aw_vec_enc("szc", "f", 65), RAY_ENC_DICT);
    {   test_result_t r_ = aw_szc_first_zero("szc"); if (r_.status != TEST_PASS) return r_; }
    /* P5 (Task 7): the same vectors as RAW bodies (the pre-P5 shape of
     * vector 65, and CONST vectors 63 / 64 read as rows) */
    aw_save_zeros("szcr", 2, 131072, RAY_ENC_RAW);
    TEST_ASSERT_EQ_I(aw_vec_enc("szcr", "f", 63), RAY_ENC_RAW);
    TEST_ASSERT_EQ_I(aw_vec_enc("szcr", "f", 64), RAY_ENC_RAW);
    TEST_ASSERT_EQ_I(aw_vec_enc("szcr", "f", 65), RAY_ENC_RAW);
    {   test_result_t r_ = aw_szc_first_zero("szcr"); if (r_.status != TEST_PASS) return r_; }
    PASS();
}

/* Symbol order is lexical, not by id: symbols first seen as z, a, m, SYM
 * forced to FOR_BP; range operators, LIKE, min, max and top-k equal
 * generation 0; equality and in still decide by ids. */
static test_result_t test_symbol_order(void) {
    aw_save("so", "(table [k s] (list (til 3000) (as 'SYMBOL (map (fn [j] (at [\"z\" \"a\" \"m\" \"b\"] (% j 4))) (til 3000)))))", RAY_ENC_FOR_BP);
    TEST_ASSERT_EQ_I(aw_vec_enc("so", "s", 0), RAY_ENC_FOR_BP);
    TEST_ASSERT_EQ_I(aw_vec_enc("so", "s", 2), RAY_ENC_FOR_BP);
    TEST_ASSERT_EQ_I(aw_sym_width("so", "s"), RAY_SYM_W8);
    /* Absolute pins: gen 0 and gen 2 share the string-order code of min, max,
     * LIKE and the top-k, so equal answers alone would not catch id order.
     * The range operators compare by string in both generations (#738):
     * z a m b repeat, 750 rows each. */
    AW_EXPECT("so", "(count (select {from: T where: (< s 'm)}))", "1500");
    AW_EXPECT("so", "(count (select {from: T where: (>= s 'b)}))", "2250");
    AW_EXPECT("so", "(min (at T 's))", "a");
    AW_EXPECT("so", "(max (at T 's))", "z");
    AW_EXPECT("so", "(at (select {s: s k: k from: T asc: s take: 3}) 's)", "[a a a]");
    AW_EXPECT("so", "(at (select {s: s k: k from: T desc: [s k] take: 3}) 's)", "[z z z]");
    AW_EXPECT("so", "(at (select {s: s k: k from: T desc: [s k] take: 3}) 'k)", "[2996 2992 2988]");
    AW_EXPECT("so", "(count (select {from: T where: (like s \"*a*\")}))", "750");
    AW_SAME_OR_ERR("so", "(count (select {from: T where: (< s 'm)}))");
    AW_SAME_OR_ERR("so", "(count (select {from: T where: (>= s 'b)}))");
    AW_SAME_OR_ERR("so", "(count (select {from: T where: (like s \"*a*\")}))");
    AW_SAME_OR_ERR("so", "(min (at T 's))");
    AW_SAME_OR_ERR("so", "(max (at T 's))");
    AW_SAME_OR_ERR("so", "(select {s: s k: k from: T asc: s take: 3})");
    AW_SAME_OR_ERR("so", "(select {s: s k: k from: T desc: [s k] take: 3})");
    AW_SAME_OR_ERR("so", "(count (select {from: T where: (== s 'm)}))");
    AW_SAME_OR_ERR("so", "(count (select {from: T where: (in s ['z 'b])}))");
    PASS();
}

/* aw_same_or_err on the parted tables adp0 / adp2, loaded as P. */
static test_result_t aw_parted_same_or_err(const char* q) {
    ray_t* out[2];
    for (int gen = 0; gen < 2; gen++) {
        aw_eval_ok(gen ? "(set P (.db.parted.get \"" AW_TMP "/adp2\" 't))" : "(set P (.db.parted.get \"" AW_TMP "/adp0\" 't))");
        out[gen] = aw_eval(q);
    }
    return aw_cmp_pair(q, out[0], out[1]);
}
#define AW_PARTED_SAME_OR_ERR(q) do { test_result_t r_ = aw_parted_same_or_err(q); if (r_.status != TEST_PASS) return r_; } while (0)

/* Aggregate admission: sum, avg, count, min, max over every type, flat,
 * under a selection and parted: same values, types and errors. */
static test_result_t test_agg_admission(void) {
    const char* tbl =
        "(table [k b u h i l f e d t ts s x] (list (til 3000) (== 0 (% (til 3000) 3)) (as 'U8 (% (til 3000) 250)) "
        "(as 'I16 (- (% (til 3000) 300) 150)) (as 'I32 (- (til 3000) 1500)) (* (til 3000) 1000003) "
        "(at (select {e: (as 'F32 x) from: (table [x] (list (* 0.5 (til 3000))))}) 'e) (* 0.25 (til 3000)) (as 'DATE (% (til 3000) 900)) (as 'TIME (* 1000 (til 3000))) "
        "(as 'TIMESTAMP (* 1000000 (til 3000))) (as 'SYMBOL (map (fn [j] (format \"s%\" (% j 7))) (til 3000))) "
        "(map (fn [j] (format \"string %\" (% j 11))) (til 3000))))";
    /* `as 'F32` is a query-projection cast only, so column f is cast inside a select. */
    aw_save("ad", tbl, -1);
    for (int p = 0; p < 2; p++) for (int gen = 0; gen <= 2; gen += 2) {
        char q[1600];
        int n = snprintf(q, sizeof q, "(.db.splayed.set \"" AW_TMP "/adp%d/2024.01.0%d/t/\" %s {compress: %s})",
                         gen, p + 1, tbl, gen ? "true" : "false");
        TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof q);
        aw_eval_ok(q);
    }
    static const char* const cols[] = { "b", "u", "h", "i", "l", "f", "e", "d", "t", "ts", "s", "x" };
    static const char* const aggs[] = { "sum", "avg", "count", "min", "max" };
    char q[512];
    for (int c = 0; c < 12; c++) for (int a = 0; a < 5; a++) {
        snprintf(q, sizeof q, "(select {r: (%s %s) from: T})", aggs[a], cols[c]);
        AW_SAME_OR_ERR("ad", q);
        snprintf(q, sizeof q, "(select {r: (%s %s) from: T where: (> k 1700)})", aggs[a], cols[c]);
        AW_SAME_OR_ERR("ad", q);
        snprintf(q, sizeof q, "(select {r: (%s %s) from: P})", aggs[a], cols[c]);
        AW_PARTED_SAME_OR_ERR(q);
    }
    PASS();
}

/* SYM ids at widths 8 and 16 across the sign bit (ids 127/128 and
 * 32767/32768) and U8 values across 127/128: min, max, count, the
 * not-admitted sum / avg errors, and equality. */
static test_result_t test_sign_bit_widths(void) {
    aw_save("w8", "(table [s u] (list (as 'SYMBOL (map (fn [j] (format \"v%\" (% j 200))) (til 5000))) (as 'U8 (+ 100 (% (til 5000) 60)))))", RAY_ENC_FOR_BP);
    aw_save("w16", "(table [s] (list (as 'SYMBOL (map (fn [j] (format \"v%\" (% j 40000))) (til 80000)))))", RAY_ENC_FOR_BP);
    TEST_ASSERT_EQ_I(aw_vec_enc("w8", "s", 0), RAY_ENC_FOR_BP);
    TEST_ASSERT_EQ_I(aw_vec_enc("w8", "s", 4), RAY_ENC_FOR_BP);
    TEST_ASSERT_EQ_I(aw_sym_width("w8", "s"), RAY_SYM_W8);
    TEST_ASSERT_EQ_I(aw_vec_enc("w16", "s", 0), RAY_ENC_FOR_BP);
    TEST_ASSERT_EQ_I(aw_vec_enc("w16", "s", 78), RAY_ENC_FOR_BP);
    TEST_ASSERT_EQ_I(aw_sym_width("w16", "s"), RAY_SYM_W16);
    static const char* const q8[] = {
        "(min (at T 's))", "(max (at T 's))", "(count (at T 's))", "(sum (at T 's))", "(avg (at T 's))",
        "(count (select {from: T where: (== s 'v127)}))", "(count (select {from: T where: (== s 'v128)}))",
        "(sum (at T 'u))", "(avg (at T 'u))", "(min (at T 'u))", "(max (at T 'u))",
        "(count (select {from: T where: (> u 127)}))", NULL,
    };
    for (int i = 0; q8[i]; i++) AW_SAME_OR_ERR("w8", q8[i]);
    static const char* const q16[] = {
        "(min (at T 's))", "(max (at T 's))",
        "(count (select {from: T where: (== s 'v32767)}))", "(count (select {from: T where: (== s 'v32768)}))",
        "(count (select {from: T where: (in s ['v32767 'v32768 'v1])}))", NULL,
    };
    for (int i = 0; q16[i]; i++) AW_SAME_OR_ERR("w16", q16[i]);
    PASS();
}

/* Top-k on F64, BOOL, U8 and SYM first keys: results equal, the fused top-k
 * takes each query on generation 2, and nothing is pruned. */
static test_result_t test_topk_never_pruned(void) {
    aw_save("tn", "(table [f b u v s] (list (map (fn [j] (if (== 0 (% j 97)) 0N (* 0.5 (- j 100000)))) (til 200000)) "
                  "(== 0 (% (til 200000) 5)) (as 'U8 (% (til 200000) 251)) (% (til 200000) 1000) "
                  "(as 'SYMBOL (map (fn [j] (format \"k%\" (% j 300))) (til 200000)))))", -1);
    static const char* const q[] = {
        "(select {f: f from: T where: (> v 10) asc: f take: 7})",
        "(select {f: f from: T where: (> v 10) desc: f take: 7})",
        "(select {b: b v: v from: T where: (> v 990) asc: [b v] take: 7})",
        "(select {u: u v: v from: T where: (> v 990) desc: [u v] take: 7})",
        "(select {s: s v: v from: T where: (> v 990) asc: [s v] take: 7})",     /* SYM first key: never pruned */
        NULL,
    };
    for (int i = 0; q[i]; i++) {
        AW_SAME_OR_ERR("tn", q[i]);                    /* leaves T on generation 2 */
        /* Run it once more on generation 2 alone: the fused top-k must take
         * it (else the pruned counters would be another call's). */
        ray_fused_topk_counters_reset();
        ray_t* r = aw_eval(q[i]);
        TEST_ASSERT_FMT(r && !RAY_IS_ERR(r), "%s: gen 2 rerun failed", q[i]);
        ray_release(r);
        uint64_t ran = 0, dec = 0;
        ray_fused_topk_counters(&ran, &dec);
        TEST_ASSERT_FMT(ran >= 1 && dec == 0, "%s: fused top-k ran %llu declined %llu", q[i],
                        (unsigned long long)ran, (unsigned long long)dec);
        uint64_t pc = 1, pv = 1;
        ray_fused_topk_test_pruned(&pc, &pv);
        TEST_ASSERT_FMT(pc == 0 && pv == 0, "%s pruned %llu chunks %llu vectors", q[i],
                        (unsigned long long)pc, (unsigned long long)pv);
    }
    PASS();
}

/* A splayed generation-2 `in` / `not in` filter later in the where:
 * chain (P3 Task 14b) reads its column by range over the morsels the
 * installed selection keeps, ANDed with it (exec_in_parted_selection's
 * single-file arm), as the first conjunct does: nullable integer, SYM and
 * F64 columns, middle and last conjuncts, grouped and counted.  Answers
 * equal generation 0's; nothing materializes (the generic OP_IN read the
 * column whole). */
static test_result_t test_splayed_in_later(void) {
    aw_save("il", "(table [k x v s f b t] (list (til 210000) (as 'I32 (- (% (til 210000) 40) (* 500 (div (til 210000) 70000)))) "
                  "(as 'I32 (map (fn [j] (if (== 0 (% j 9)) 0N (% j 1000))) (til 210000))) "
                  "(take ['a 'b 'c 'd 'e] 210000) (as 'F64 (% (til 210000) 50)) "
                  "(as 'I16 (% (til 210000) 2)) (as 'I16 (- (% (til 210000) 11) 1))))", -1);
    static const char* const qs[] = {
        "(select {c: (count k) s: (sum v) from: T where: (and (> v 10) (in x [3 -497 -980])) by: x asc: x})",
        "(select {c: (count k) from: T where: (and (> v 10) (in x [3 -497]) (< v 900)) by: s asc: s})",
        "(select {c: (count k) from: T where: (and (> v 10) (not-in x [3 -497 -998]) (< v 900))})",
        "(select {k: k v: v from: T where: (and (== k 70005) (in x [5 -495 -1000]))})",
        "(select {c: (count k) from: T where: (and (> f 10.0) (in v [1 2 3 9 500]))})",
        "(select {c: (count k) from: T where: (and (> f 10.0) (not-in v [1 2 3 9 500]))})",
        "(select {c: (count k) from: T where: (and (> v 10) (in s ['a 'c])) by: s asc: s})",
        "(select {c: (count k) from: T where: (and (> v 10) (not-in s ['a 'c]))})",
        "(select {c: (count k) from: T where: (and (> v 10) (in f [1.0 7.0 -0.0]))})",
        "(select {c: (count k) from: T where: (and (> v 10) (in x [12345]))})",
        "(select {c: (count k) from: T where: (and (> v 10) (not-in x [100000]))})",
        "(select {c: (count k) from: T where: (and (== b 0) (in t [-1 6]))})",
        "(select {c: (count k) from: T where: (and (== b 1) (not-in t [-1 6 9]))})",
        "(select {c: (count k) from: T where: (and (== b 0) (in f [1.0 7.0 -0.0]))})",
        "(select {c: (count k) from: T where: (and (== b 0) (in v [1 2 3 9 500]))})",
        "(select {c: (count k) from: T where: (and (< v 100) (in x [3 -497 -980]))})",
        "(select {c: (count k) from: T where: (and (< v 100) (in f [1.0 7.0 -0.0]))})",
        "(select {c: (count k) from: T where: (and (== b 0) (not-in x [3 -497]))})",
        "(select {c: (count k) from: T where: (and (== b 0) (not-in f [1.0 7.0]))})",
        NULL,
    };
    static const char* const in_col[] = { "x", "x", "x", "x", "v", "v", "s", "s", "f", "x", "x", "t", "t", "f", "v", "x", "f", "x", "f" };
    /* the rows that reach exec_node's later-conjunct `in` (the others take
     * another route, which the materialization check covers): SYM, I16,
     * I32 and F64; each is answered by in_sel_single_within (route
     * counter), so disabling that arm for any type fails here */
    static const bool routed[] = { 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1 };
    for (int i = 0; qs[i]; i++) {
        uint64_t w0 = ray_exec_test_in_single_within();
        AW_SAME("il", qs[i], 0);
        if (routed[i])
            TEST_ASSERT_FMT(ray_exec_test_in_single_within() == w0 + 1, "%s: not read by range over the selection", qs[i]);
        /* and the column stays unmaterialized (T: generation 2) */
        char b[128];
        snprintf(b, sizeof b, "(at (.idx.info T '%s) 'materialized)", in_col[i]);
        ray_t* mt = aw_eval(b);
        TEST_ASSERT_FMT(mt && !RAY_IS_ERR(mt) && mt->b8 == 0, "%s: %s materialized", qs[i], in_col[i]);
        ray_release(mt);
    }
    PASS();
}

/* A parted group's (strlen c) SUM / AVG input (ClickBench q28: avg
 * (strlen URL) by CounterID) is aggregated by each partition's group over
 * the borrowed segment, by range on generation 2, as the splayed group
 * does: the SYM segments (URL's type in the ClickBench schema) are not
 * materialized (the per-partition path evaluated the expression into a
 * column, reading each segment whole).  Three partitions of 70000 rows,
 * under a filter and without one, with a column without "" and one
 * holding it; answers equal generation 0's. */
static test_result_t test_parted_strlen_avg(void) {
    for (int gen = 0; gen <= 2; gen += 2)
        for (int p = 0; p < 3; p++) {
            char q[1200];
            snprintf(q, sizeof q,
                     "(.db.splayed.set \"" AW_TMP "/ps%d/2024.01.0%d/t/\" (table [g u w v] (list (%% (til 70000) 13) "
                     "(as 'SYM (as 'STR (* (%% (til 70000) 997) %d))) "
                     "(as 'SYMBOL (map (fn [j] (if (== 0 (%% j 7)) \"\" (format \"w%%\" (%% j 50)))) (til 70000))) (til 70000))) "
                     "\"" AW_TMP "/ps%d/.sym\" {compress: %s})",
                     gen, p + 1, 1000 + p * 7, gen, gen ? "true" : "false");
            aw_eval_ok(q);
        }
    static const char* const qs[] = {
        "(select {l: (avg (strlen u)) c: (count v) from: T where: (!= w \"\") by: g asc: g})",
        "(select {l: (avg (strlen w)) s: (sum (strlen u)) c: (count v) from: T by: g asc: g})",
        "(select {l: (avg (strlen w)) c: (count v) from: T where: (> v 1000) by: g asc: g})",
        NULL,
    };
    for (int i = 0; qs[i]; i++) {
        ray_t* out[2];
        for (int gen = 0; gen <= 2; gen += 2) {
            char b[256];
            snprintf(b, sizeof b, "(set T (.db.parted.get \"" AW_TMP "/ps%d\" 't))", gen);
            aw_eval_ok(b);
            ray_enc_counters_reset();
            int64_t pp0 = ray_group_perpart_done();
            out[gen / 2] = ray_eval_str(qs[i]);
            TEST_ASSERT_FMT(out[gen / 2] && !RAY_IS_ERR(out[gen / 2]), "%s gen %d: %s", qs[i], gen, ray_error_msg());
            /* the per-partition group returned the answer (not its concat fallback) */
            TEST_ASSERT_FMT(ray_group_perpart_done() == pp0 + 1, "%s gen %d: not the per-partition group", qs[i], gen);
            if (gen) {
                ray_enc_counters_t k; ray_enc_counters_get(&k);
                TEST_ASSERT_FMT(k.materialized == 0, "%s: %llu materializations", qs[i], (unsigned long long)k.materialized);
            }
        }
        ray_t* fa = ray_fmt(out[0], 0); ray_t* fb = ray_fmt(out[1], 0);
        TEST_ASSERT_FMT(ray_str_len(fa) == ray_str_len(fb) && memcmp(ray_str_ptr(fa), ray_str_ptr(fb), ray_str_len(fa)) == 0,
                        "%s differs", qs[i]);
        ray_release(fa); ray_release(fb); ray_release(out[0]); ray_release(out[1]);
    }
    aw_eval_ok("(set T 0)");
    PASS();
}

const test_entry_t aware_entries[] = {
    { "aware/vview_encodings", test_vview_encodings, aw_setup, aw_teardown },
    { "aware/vview_near_top",  test_vview_near_top,  aw_setup, aw_teardown },
    { "aware/vview_declines",  test_vview_declines,  aw_setup, aw_teardown },
    { "aware/vview_source",    test_vview_source,    aw_setup, aw_teardown },
    { "aware/eval_encodings",   test_eval_encodings,   aw_setup, aw_teardown },
    { "aware/eval_for_edges",   test_eval_for_edges,   aw_setup, aw_teardown },
    { "aware/eval_const_nulls", test_eval_const_nulls, aw_setup, aw_teardown },
    { "aware/eval_in",          test_eval_in,          aw_setup, aw_teardown },
    { "aware/eval_gen0",        test_eval_gen0,        aw_setup, aw_teardown },
    { "aware/eval_sym",         test_eval_sym,         aw_setup, aw_teardown },
    { "aware/eval_float_const", test_eval_float_const, aw_setup, aw_teardown },
    { "aware/filter_encoded_leaves",   test_filter_encoded_leaves,   aw_setup, aw_teardown },
    { "aware/filter_vector_decisions", test_filter_vector_decisions, aw_setup, aw_teardown },
    { "aware/filter_nulls_masked",     test_filter_nulls_masked,     aw_setup, aw_teardown },
    { "aware/filter_floats",           test_filter_floats,           aw_setup, aw_teardown },
    { "aware/filter_mixed_generations", test_filter_mixed_generations, aw_setup, aw_teardown },
    { "aware/filter_parted_leaves",    test_filter_parted_leaves,    aw_setup, aw_teardown },
    { "aware/fused_pred_leaves",       test_fused_pred_leaves,       aw_setup, aw_teardown },
    { "aware/filter_leaf_ops",         test_filter_leaf_ops,         aw_setup, aw_teardown },
    { "aware/sym_membership", test_sym_membership, aw_setup, aw_teardown },
    { "aware/sym_in_bounds",  test_sym_in_bounds,  aw_setup, aw_teardown },
    { "aware/sym_null_literal", test_sym_null_literal, aw_setup, aw_teardown },
    { "aware/sym_fused_gen0", test_sym_fused_gen0, aw_setup, aw_teardown },
    { "aware/float_in_bounds", test_float_in_bounds, aw_setup, aw_teardown },
    { "aware/gather_rows_contract",  test_gather_rows_contract,  aw_setup, aw_teardown },
    { "aware/gather_sel_skips_none", test_gather_sel_skips_none, aw_setup, aw_teardown },
    { "aware/gather_sel_parted",     test_gather_sel_parted,     aw_setup, aw_teardown },
    { "aware/gather_gen0_parity",    test_gather_gen0_parity,    aw_setup, aw_teardown },
    { "aware/gather_rows_sorted",    test_gather_rows_sorted,    aw_setup, aw_teardown },
    { "aware/gather_empty_and_nulls", test_gather_empty_and_nulls, aw_setup, aw_teardown },
    { "aware/gather_ragged",         test_gather_ragged,         aw_setup, aw_teardown },
    { "aware/gather_operators",  test_gather_operators,  aw_setup, aw_teardown },
    { "aware/gather_index_rows", test_gather_index_rows, aw_setup, aw_teardown },
    { "aware/gather_list_cells", test_gather_list_cells, aw_setup, aw_teardown },
    { "aware/gather_compact_group", test_gather_compact_group, aw_setup, aw_teardown },
    { "aware/gather_refine",     test_gather_refine,     aw_setup, aw_teardown },
    { "aware/gather_parted_sym", test_gather_parted_sym, aw_setup, aw_teardown },
    { "aware/gather_filter_ops", test_gather_filter_ops, aw_setup, aw_teardown },
    { "aware/gather_sel_parallel", test_gather_sel_parallel, aw_setup, aw_teardown },
    { "aware/refine_in",         test_refine_in,         aw_setup, aw_teardown },
    { "aware/gather_sel_late_null_cancel", test_gather_sel_late_null_cancel, aw_setup, aw_teardown },
    { "aware/firstn_by_range",    test_firstn_by_range,    aw_setup, aw_teardown },
    { "aware/firstn_parted",      test_firstn_parted,      aw_setup, aw_teardown },
    { "aware/sparse_key_points",  test_sparse_key_points,  aw_setup, aw_teardown },
    { "aware/gather_points",      test_gather_points,      aw_setup, aw_teardown },
    { "aware/gather_raw_points",  test_gather_raw_points,  aw_setup, aw_teardown },
    { "aware/gather_sym_points",  test_gather_sym_points,  aw_setup, aw_teardown },
    { "aware/sort_head_late",     test_sort_head_late,     aw_setup, aw_teardown },
    { "aware/firstn_runs",        test_firstn_runs,        aw_setup, aw_teardown },
    { "aware/firstn_sel",         test_firstn_sel,         aw_setup, aw_teardown },
    { "aware/sparse_walk_points", test_sparse_walk_points, aw_setup, aw_teardown },
    { "aware/sym_key_chunk_extremes", test_sym_key_chunk_extremes, aw_setup, aw_teardown },
    { "aware/sym_key_points", test_sym_key_points, aw_setup, aw_teardown },
    { "aware/leaf_emit_runs", test_leaf_emit_runs, aw_setup, aw_teardown },
    { "aware/da_prescan_abort_staged", test_da_prescan_abort_staged, aw_setup, aw_teardown },
    { "aware/refine_on_views",    test_refine_on_views,    aw_setup, aw_teardown },
    { "aware/eq_on_runs",         test_eq_runs_lack,       aw_setup, aw_teardown },
    { "aware/gather_rows_parallel", test_gather_rows_parallel, aw_setup, aw_teardown },
    { "aware/sum_extremes",      test_sum_extremes,      aw_setup, aw_teardown },
    { "aware/sum_parted_dir",    test_sum_parted_dir,    aw_setup, aw_teardown },
    { "aware/sum_parted_ragged", test_sum_parted_ragged, aw_setup, aw_teardown },
    { "aware/float_sum_decoded", test_float_sum_decoded, aw_setup, aw_teardown },
    { "aware/sum_encoded_vectors", test_sum_encoded_vectors, aw_setup, aw_teardown },
    { "aware/agg_under_selection", test_agg_under_selection, aw_setup, aw_teardown },
    { "aware/scalar_route_stats",  test_scalar_route_stats,  aw_setup, aw_teardown },
    { "aware/scalar_route_mixed",  test_scalar_route_mixed,  aw_setup, aw_teardown },
    { "aware/scalar_route_parted", test_scalar_route_parted, aw_setup, aw_teardown },
    { "aware/scalar_route_parted_stats", test_scalar_route_parted_stats, aw_setup, aw_teardown },
    { "aware/scalar_route_time",   test_scalar_route_time,   aw_setup, aw_teardown },
    { "aware/scalar_route_float_inf", test_scalar_route_float_inf, aw_setup, aw_teardown },
    { "aware/unaligned_grain",     test_unaligned_grain,     aw_setup, aw_teardown },
    { "aware/pool_clamp",          test_pool_clamp,          aw_setup, aw_teardown },
    { "aware/count_on_runs",       test_count_on_runs,       aw_setup, aw_teardown },
    { "aware/group_count_on_runs", test_group_count_on_runs, aw_setup, aw_teardown },
    { "aware/topk_prune_cases",    test_topk_prune_cases,    aw_setup, aw_teardown },
    { "aware/topk_range_keys",     test_topk_range_keys,     aw_setup, aw_teardown },
    { "aware/group_topn_ties",     test_group_topn_ties,     aw_setup, aw_teardown },
    { "aware/group_topn_nulls",    test_group_topn_nulls,    aw_setup, aw_teardown },
    { "aware/group_topn_affine",   test_group_topn_affine,   aw_setup, aw_teardown },
    { "aware/group_affine_nulls",  test_group_affine_nulls,  aw_setup, aw_teardown },
    { "aware/str_key_domain",      test_str_key_domain,      aw_setup, aw_teardown },
    { "aware/str_key_domain_raw",  test_str_key_domain_raw,  aw_setup, aw_teardown },
    { "aware/str_key_domain_dict", test_str_key_domain_dict, aw_setup, aw_teardown },
    { "aware/gate_materialization", test_gate_materialization, aw_setup, aw_teardown },
    { "aware/gate_decoded_bytes",   test_gate_decoded_bytes,   aw_setup, aw_teardown },
    { "aware/cross_segment_codes",  test_cross_segment_codes,  aw_setup, aw_teardown },
    { "aware/parted_in",            test_parted_in,            aw_setup, aw_teardown },
    { "aware/splayed_in_later",     test_splayed_in_later,     aw_setup, aw_teardown },
    { "aware/parted_strlen_avg",    test_parted_strlen_avg,    aw_setup, aw_teardown },
    { "aware/signed_zeros",      test_signed_zeros,      aw_setup, aw_teardown },
    { "aware/symbol_order",      test_symbol_order,      aw_setup, aw_teardown },
    { "aware/agg_admission",     test_agg_admission,     aw_setup, aw_teardown },
    { "aware/sign_bit_widths",   test_sign_bit_widths,   aw_setup, aw_teardown },
    { "aware/topk_never_pruned", test_topk_never_pruned, aw_setup, aw_teardown },
    { NULL, NULL, NULL, NULL },
};
