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
#include "store/enc.h"
#include "store/encfmt.h"
#include "store/encw.h"
#include "store/enccol.h"
#include "store/colrange.h"
#include "store/enceval.h"
#include "lang/env.h"
#include "store/splay.h"
#include "store/part.h"
#include "vec/str.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Written as "/tmp/..." in the sources; every use goes through
 * ray_test_tmp (a per-process root), so concurrent suites never share it. */
#define AW_TMP "/tmp/rayforce_aware"
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
        const ray_enc_desc_t* d;
        ray_t* err = ray_enccol_chunk_descs_ref(ray_enccol_of(encv), vec / RAY_ENC_VECS_PER_CHUNK, &d);
        if (!err) enc = d[vec % RAY_ENC_VECS_PER_CHUNK].encoding; else ray_error_free(err);
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
    if (want_decoded >= 0)
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
static void W8(size_t off, uint8_t v)   { memcpy(g_img + off, &v, 1); }
static void W16(size_t off, uint16_t v) { memcpy(g_img + off, &v, 2); }
static void W32(size_t off, uint32_t v) { memcpy(g_img + off, &v, 4); }
static void W64(size_t off, uint64_t v) { memcpy(g_img + off, &v, 8); }
#define DIR(c) (96 + 64 * (size_t)(c))
#define EH 32
static size_t desc_at(int j)    { return (size_t)rd64(DIR(0)) + 48 * (size_t)j; }
static size_t payload_at(int j) { return (size_t)rd64(DIR(0)) + (size_t)rd64(desc_at(j) + 8); }

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

/* RLE body at `at`: [u32 runs][u32 0], the values child, the ends child
 * (8-byte header each: encoding, width, reserved, body bytes).  Moves every
 * run end up by one (the last past n), whatever the ends child's codec. */
static bool dv_rle_ends_up(size_t at) {
    uint32_t nr; memcpy(&nr, g_img + at, 4);
    size_t vc = at + 8;
    uint32_t vbb; memcpy(&vbb, g_img + vc + 4, 4);
    size_t ec = vc + 8 + vbb;
    uint8_t enc = g_img[ec];
    if (enc == RAY_ENC_RAW) {
        for (uint32_t r = 0; r < nr; r++) { uint16_t x; memcpy(&x, g_img + ec + 8 + 2 * r, 2); W16(ec + 8 + 2 * r, (uint16_t)(x + 1)); }
    } else if (enc == RAY_ENC_FOR_BP) {
        W64(ec + 8, rd64(ec + 8) + 1);
    } else if (enc == RAY_ENC_DELTA_BP) {
        for (int l = 0; l < 64; l++) W64(ec + 16 + 8 * (size_t)l, rd64(ec + 16 + 8 * (size_t)l) + 1);
    } else {
        return false;
    }
    return true;
}
/* Puts the T-bit pattern x on run 0's value of the RLE body at `at` (a RAW
 * values child: the first value; FOR_BP: its base, so run 0's value is x
 * when its residual is 0, as the minimum's is). */
static bool dv_rle_run0(size_t at, int T, uint64_t x) {
    size_t vc = at + 8;
    if (g_img[vc] == RAY_ENC_RAW) { memcpy(g_img + vc + 8, &x, (size_t)T / 8); return true; }
    if (g_img[vc] == RAY_ENC_FOR_BP) { W64(vc + 8, x); return true; }
    return false;
}

/* RAW and DELTA_BP have no view; a view never vouches for a vector its
 * decode rejects.  One sub-case per rule of the view's validation: each
 * declines (0) or reports what the decode reports (-1), and the range read
 * of the same vector then reports it `corrupt` with its exact rule. */
static test_result_t test_vview_declines(void) {
    const char* p = ray_test_tmp(AW_TMP "/dv");
    for (int i = 0; i < 256; i++) { char b[8]; int n = snprintf(b, sizeof b, "s%d", i); (void)ray_sym_intern(b, (size_t)n); }
    size_t at;
    /* no view: RAW, DELTA_BP, a float other than CONST */
    DV_SAVE(dv_hash(RAY_I32, 5000, 0), RAY_ENC_RAW, p); NEED_ENC(0, RAY_ENC_RAW);   DV(p, 0, NULL);
    {
        ray_t* v = ray_vec_new(RAY_I32, 1024); v->len = 1024;
        for (int i = 0; i < 1024; i++) ((int32_t*)ray_data(v))[i] = i * 3 + (i * 7) % 5;
        DV_SAVE(v, RAY_ENC_DELTA_BP, p); NEED_ENC(0, RAY_ENC_DELTA_BP); DV(p, 0, NULL);
    }
    DV_SAVE(dv_f64(false), RAY_ENC_RAW, p); NEED_ENC(0, RAY_ENC_RAW); DV(p, 0, NULL);
    /* descriptors failing validation: -1, as the decode reports them */
    DV_SAVE(dv_hash(RAY_I32, 5000, 0), RAY_ENC_FOR_BP, p); NEED_ENC(0, RAY_ENC_FOR_BP); DV(p, 1, NULL);
    W8(desc_at(0) + 1, 33);                                       DV(p, -1, "width");

    /* payload split: the bitmap count (payload_parts' corrupt is -1) */
    DV_SAVE(dv_hash(RAY_I32, 5000, 10), RAY_ENC_FOR_BP, p); NEED_ENC(0, RAY_ENC_FOR_BP); DV(p, 1, NULL);
    at = payload_at(0); W8(at, g_img[at] ^ 0x02);                 DV(p, -1, "null bitmap count");
    {   /* 100 rows: bit 100 lies past them */
        ray_t* v = dv_hash(RAY_I32, 5000, 10); v->len = 100;
        DV_SAVE(v, RAY_ENC_FOR_BP, p); NEED_ENC(0, RAY_ENC_FOR_BP); DV(p, 1, NULL);
        at = payload_at(0); g_img[at + 12] |= 0x10;               DV(p, -1, "null bitmap tail bits set");
    }

    /* FOR_BP: base above the type width; a non-null residual on the
     * sentinel (bitmap and none); a body that could wrap (U8: the decode
     * gives the modular values, so the read succeeds) */
    DV_SAVE(dv_hash(RAY_I32, 5000, 0), RAY_ENC_FOR_BP, p);
    W64(payload_at(0), 1ull << 32);                               DV(p, 0, "FOR_BP base above the type width");
    DV_SAVE(dv_hash(RAY_I32, 5000, 0), RAY_ENC_FOR_BP, p);
    W64(payload_at(0), 1ull << 31);                               DV(p, 0, "sentinel outside the bitmap");
    /* the body size against the width: the descriptor's width one lower */
    DV_SAVE(dv_hash(RAY_I32, 5000, 0), RAY_ENC_FOR_BP, p);
    TEST_ASSERT_EQ_I(g_img[desc_at(0) + 1], 6);
    W8(desc_at(0) + 1, 5);                                        DV(p, 0, "bytes after the codec body");
    DV_SAVE(dv_hash(RAY_I16, 5000, 10), RAY_ENC_FOR_BP, p); NEED_ENC(0, RAY_ENC_FOR_BP); DV(p, 1, NULL);
    W64(payload_at(0) + 128, 1ull << 15);                         DV(p, 0, "sentinel outside the bitmap");
    DV_SAVE(dv_hash(RAY_U8, 0, 0), RAY_ENC_FOR_BP, p); NEED_ENC(0, RAY_ENC_FOR_BP); DV(p, 1, NULL);
    TEST_ASSERT_EQ_I(g_img[desc_at(0) + 1], 6);
    W64(payload_at(0), 250);                                      DV(p, 0, NULL);
    /* the edge itself is no wrap: 192 + 63 = 255 */
    DV_SAVE(dv_hash(RAY_U8, 0, 0), RAY_ENC_FOR_BP, p); W64(payload_at(0), 192); DV(p, 1, NULL);
    DV_SAVE(dv_hash(RAY_I16, 0, 0), RAY_ENC_FOR_BP, p); NEED_ENC(0, RAY_ENC_FOR_BP);
    /* 32705 + 63 > 32767: wraps, through the sentinel (residual 63) */
    W64(payload_at(0), 32767 - 62);                               DV(p, 0, "sentinel outside the bitmap");

    /* CONST / RLE: a run on the sentinel outside the bitmap */
    DV_SAVE(dv_runs(0, true), RAY_ENC_CONST, p); NEED_ENC(0, RAY_ENC_CONST); DV(p, 1, NULL);
    W32(payload_at(0), 0x80000000u);                              DV(p, 0, "sentinel outside the bitmap");
    DV_SAVE(dv_runs(10, true), RAY_ENC_CONST, p); NEED_ENC(0, RAY_ENC_CONST); DV(p, 1, NULL);
    W32(payload_at(0) + 128, 0x80000000u);                        DV(p, 0, "sentinel outside the bitmap");
    DV_SAVE(dv_runs(0, false), RAY_ENC_RLE, p); NEED_ENC(0, RAY_ENC_RLE); DV(p, 1, NULL);
    TEST_ASSERT_TRUE(dv_rle_run0(payload_at(0), 32, 0x80000000u)); DV(p, 0, "sentinel outside the bitmap");
    /* RLE runs: a run end past n, a run count of zero, CONST padding */
    DV_SAVE(dv_runs(0, false), RAY_ENC_RLE, p);
    TEST_ASSERT_TRUE(dv_rle_ends_up(payload_at(0)));              DV(p, 0, "codec body");
    DV_SAVE(dv_runs(0, false), RAY_ENC_RLE, p); W32(payload_at(0), 0); DV(p, 0, "codec body");
    DV_SAVE(dv_runs(0, true), RAY_ENC_CONST, p); W8(payload_at(0) + 5, 1); DV(p, 0, "codec body");

    /* SYM in every encoding: an id at the bound, the null count, an id
     * outside the vector's statistics */
    {
        uint8_t ids[1024];
        for (int i = 0; i < 1024; i++) ids[i] = 5;
        DV_SAVE_SYM(ids, 1024, RAY_ENC_CONST, p); NEED_ENC(0, RAY_ENC_CONST); DV(p, 1, NULL);
        W8(payload_at(0), 200);                                   DV(p, 0, "SYM id outside the symbol domain");
        for (int i = 0; i < 1024; i++) ids[i] = (uint8_t)(1 + i / 100);
        DV_SAVE_SYM(ids, 1024, RAY_ENC_RLE, p); NEED_ENC(0, RAY_ENC_RLE); DV(p, 1, NULL);
        TEST_ASSERT_TRUE(dv_rle_run0(payload_at(0), 8, 200));     DV(p, 0, "SYM id outside the symbol domain");
        DV_SAVE_SYM(ids, 1024, RAY_ENC_RLE, p);
        W16(desc_at(0) + 2, 1); W32(DIR(0) + 52, 1);              DV(p, 0, "SYM null count");
        DV_SAVE_SYM(ids, 1024, RAY_ENC_RLE, p);
        TEST_ASSERT_EQ_I((int64_t)rd64(desc_at(0) + 40), 11);
        W64(desc_at(0) + 40, 10);                                 DV(p, 0, "value outside its statistics");
        for (int i = 0; i < 1024; i++) ids[i] = (uint8_t)(100 + i % 16);
        DV_SAVE_SYM(ids, 1024, RAY_ENC_FOR_BP, p); NEED_ENC(0, RAY_ENC_FOR_BP); DV(p, 1, NULL);
        W64(payload_at(0), 199);                                  DV(p, 0, "SYM id outside the symbol domain");
        DV_SAVE_SYM(ids, 1024, RAY_ENC_FOR_BP, p); W64(payload_at(0), 0); DV(p, 0, "SYM null count");
        DV_SAVE_SYM(ids, 1024, RAY_ENC_FOR_BP, p);
        W64(desc_at(0) + 40, 114);                                DV(p, 0, "value outside its statistics");
        /* a wrapping SYM body every other rule accepts: ids 1 and 51 (base
         * 1, w 6) with the base forged to 230 decode to 230 and 24 (mod
         * 256), inside a domain of 250 and statistics widened to [24, 230];
         * the decode gives them, the view declines */
        for (int i = 0; i < 1024; i++) ids[i] = (uint8_t)(i % 2 ? 51 : 1);
        TEST_ASSERT_TRUE(dv_save_sym_n(ids, 1024, RAY_ENC_FOR_BP, 250, p)); NEED_ENC(0, RAY_ENC_FOR_BP); DV(p, 1, NULL);
        TEST_ASSERT_EQ_I(g_img[desc_at(0) + 1], 6);
        W64(payload_at(0), 230);
        W64(desc_at(0) + 32, 24); W64(desc_at(0) + 40, 230); W64(DIR(0) + 16, 24); W64(DIR(0) + 24, 230);
        DV(p, 0, NULL);
    }

    /* DICT: a code past the dictionary, a value outside the statistics, a
     * non-null row on the sentinel's code, the VALUE_DICT's own validation (-1) */
    DV_SAVE(dv_dict(), RAY_ENC_DICT, p); NEED_ENC(0, RAY_ENC_DICT); DV(p, 1, NULL);
    TEST_ASSERT_EQ_I(g_img[payload_at(0)], RAY_ENC_FOR_BP);
    W64(payload_at(0) + 8, 4);                                    DV(p, 0, "DICT code outside the dictionary");
    {   /* a null row's code past the dictionary: three I32 values, a null
         * every 10th row (filled with the minimum's code 0); row 0's 2-bit
         * field (lane 0, bits 0-1 of the first packed word) set to 3 */
        static const int32_t vals[3] = { 11, 900000001, 77 };
        ray_t* v = ray_vec_new(RAY_I32, 1024); v->len = 1024;
        for (int i = 0; i < 1024; i++)
            ((int32_t*)ray_data(v))[i] = i % 10 ? vals[((uint32_t)((uint32_t)i * 2654435761u) >> 30) % 3] : NULL_I32;
        v->attrs |= RAY_ATTR_HAS_NULLS;
        DV_SAVE(v, RAY_ENC_DICT, p); NEED_ENC(0, RAY_ENC_DICT); DV(p, 1, NULL);
        at = payload_at(0) + 128;
        TEST_ASSERT_EQ_I(g_img[at], RAY_ENC_FOR_BP); TEST_ASSERT_EQ_I(g_img[at + 1], 2); TEST_ASSERT_EQ_I(rd64(at + 8), 0);
        TEST_ASSERT_EQ_I(g_img[at + 16] & 3, 0);
        g_img[at + 16] |= 3;                                      DV(p, 0, "DICT code outside the dictionary");
    }
    /* the code child's header: reserved bits, width above 32, body size, base above 32 bits */
    DV_SAVE(dv_dict(), RAY_ENC_DICT, p); W16(payload_at(0) + 2, 1);           DV(p, 0, "child header reserved bytes not zero");
    DV_SAVE(dv_dict(), RAY_ENC_DICT, p); W8(payload_at(0) + 1, 33);           DV(p, 0, "FOR_BP width above the type width");
    DV_SAVE(dv_dict(), RAY_ENC_DICT, p); W32(payload_at(0) + 4, (uint32_t)rd64(payload_at(0) + 4) + 8); DV(p, 0, "child body overruns its parent");
    DV_SAVE(dv_dict(), RAY_ENC_DICT, p); W64(payload_at(0) + 8, 1ull << 32);  DV(p, 0, "FOR_BP base above the type width");
    DV_SAVE(dv_dict(), RAY_ENC_DICT, p);
    W64(desc_at(0) + 32, 12);                                     DV(p, 0, "value outside its statistics");
    DV_SAVE(dv_dict(), RAY_ENC_DICT, p);
    W64(desc_at(0) + 40, 123456789012344ll);                      DV(p, 0, "value outside its statistics");
    {
        DV_SAVE(dv_dict(), RAY_ENC_DICT, p);
        size_t entry = (size_t)rd64(EH + 16) + 32;                /* side table of one entry, then VALUE_DICT */
        TEST_ASSERT_EQ_I((int64_t)rd64(entry + 8), 11);
        W64(entry + 8, (uint64_t)INT64_MIN); W64(desc_at(0) + 32, (uint64_t)INT64_MIN); W64(DIR(0) + 16, (uint64_t)INT64_MIN);
        DV(p, 0, "sentinel outside the bitmap");
        DV_SAVE(dv_dict(), RAY_ENC_DICT, p);
        entry = (size_t)rd64(EH + 16) + 32;
        W64(entry + 8, rd64(entry + 16));                         DV(p, -1, "VALUE_DICT values not ascending");
    }

    /* floats: CONST only, NaN outside the bitmap, outside the statistics */
    DV_SAVE(dv_f64(true), RAY_ENC_CONST, p); NEED_ENC(0, RAY_ENC_CONST); DV(p, 1, NULL);
    W64(payload_at(0), 0x7FF8000000000001ull);                    DV(p, 0, "NaN outside the bitmap");
    DV_SAVE(dv_f64(true), RAY_ENC_CONST, p);
    { double x = 3.5; memcpy(g_img + payload_at(0), &x, 8); }     DV(p, 0, "value outside its statistics");
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
            cs[nc++] = 0; cs[nc++] = (int64_t)e->sym_bound + 5;
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
        test_result_t r = aw_same(nm[i], "(select {x: x from: T where: (== x 7) desc: x take: 3})", 1);
        if (r.status != TEST_PASS) return r;
        ray_fused_topk_counters(&ran, &dec);
        TEST_ASSERT_EQ_U(ran, 2);                                /* both generations take the fused top-k */
        TEST_ASSERT_EQ_U(aw_col_decoded("x"), 70000 * 4);        /* the sort key's materialization only */

        aw_save(nk[i], tbk, enc[i]);
        TEST_ASSERT_EQ_I(aw_vec_enc(nk[i], "x", 0), enc[i]);
        ray_fused_topk_counters_reset();
        r = aw_same(nk[i], "(select {k: k from: T where: (== x 7) desc: k take: 3})", 1);
        if (r.status != TEST_PASS) return r;
        ray_fused_topk_counters(&ran, &dec);
        TEST_ASSERT_EQ_U(ran, 2);
        TEST_ASSERT_EQ_U(aw_col_decoded("x"), 0);                /* the leaf reads x's views */
        ray_fused_topk_counters_reset();
        r = aw_same(nk[i], "(select {k: k from: T where: (> x 25) take: 5})", 1);   /* the fused take (k: the output gather, Task 6) */
        if (r.status != TEST_PASS) return r;
        ray_fused_topk_counters(&ran, &dec);
        TEST_ASSERT_EQ_U(ran, 2);
        TEST_ASSERT_EQ_U(aw_col_decoded("x"), 0);
        /* from the end: windows end at the table's last row, so they lie
         * inside a vector (the leaf's whole-vector answer is cut) */
        ray_fused_topk_counters_reset();
        r = aw_same(nk[i], "(select {k: k from: T where: (> x 25) take: -1500})", 1);
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
    int64_t n7 = 0, ne = 0, n2 = 0, n5 = 0;
    for (int64_t j = 0; j < 200000; j++) {
        int v = aw_sy_val(j);
        n7 += v == 7; ne += v < 0; n2 += v == 2; n5 += v == 5;
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
    (void)n5;
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
    /* ordered compares on SYM still decode and order strings (#738): the
     * fused compiler keeps EXPR_BAIL_SYM_DOMAIN */
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

/* `in` / `not in` by membership bounds: a set none of whose ids lies in a
 * chunk's or vector's [min, max] decides it without a read (`not in` only
 * without "" rows); the rest is answered on the views' ids.  Generation 0
 * (the unchanged in-place kernel) is the reference. */
static test_result_t test_sym_in_bounds(void) {
    aw_save("sy", "(table [s] (list (as 'SYMBOL (map (fn [j] (if (and (>= j 65536) (== 0 (% j 3))) \"\" "
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
        if ((r = aw_sel_same("sy", shapes[i], dec[i])).status != TEST_PASS) { fprintf(stderr, "shape %zu\n", i); return r; }
    int64_t nin = 0;
    for (int64_t j = 0; j < 200000; j++) { int v = aw_sy_val(j); nin += v == 7 || v == 1; }
    TEST_ASSERT_EQ_I(aw_sel_count("sy", 2, ap_s_in, NULL), nin);
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
    { "aware/sym_fused_gen0", test_sym_fused_gen0, aw_setup, aw_teardown },
    { NULL, NULL, NULL, NULL },
};
