/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#define _DEFAULT_SOURCE   /* mkdtemp */

#include "test.h"
#include <rayforce.h>
#include "lang/eval.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Most introspection helpers need a live heap/runtime so vectors and
 * atoms can be constructed via the public API. Match the test_link.c
 * pattern: bring up a runtime in setup, tear it down afterwards. */
struct ray_runtime_s;
typedef struct ray_runtime_s ray_runtime_t;
extern ray_runtime_t* ray_runtime_create(int argc, char** argv);
extern void           ray_runtime_destroy(ray_runtime_t* rt);
extern ray_runtime_t* __RUNTIME;

static void public_api_setup(void)    { ray_runtime_create(0, NULL); }
static void public_api_teardown(void) { ray_runtime_destroy(__RUNTIME); }

static test_result_t test_public_ipc_client_symbols(void) {
    int64_t   (*connect_fn)(const char*, uint16_t, const char*, const char*, int) = ray_ipc_connect;
    void      (*close_fn)(int64_t) = ray_ipc_close;
    ray_t*    (*send_fn)(int64_t, ray_t*) = ray_ipc_send;
    ray_err_t (*async_fn)(int64_t, ray_t*) = ray_ipc_send_async;
    ray_t*    (*verbose_fn)(int64_t, ray_t*) = ray_ipc_send_verbose;

    TEST_ASSERT_NOT_NULL((void*)connect_fn);
    TEST_ASSERT_NOT_NULL((void*)close_fn);
    TEST_ASSERT_NOT_NULL((void*)send_fn);
    TEST_ASSERT_NOT_NULL((void*)async_fn);
    TEST_ASSERT_NOT_NULL((void*)verbose_fn);
    PASS();
}

static test_result_t test_public_query_and_format_symbols(void) {
    ray_t* (*select_fn)(ray_t**, int64_t) = ray_select;
    ray_t* (*update_fn)(ray_t**, int64_t) = ray_update;
    ray_t* (*insert_fn)(ray_t**, int64_t) = ray_insert;
    ray_t* (*upsert_fn)(ray_t**, int64_t) = ray_upsert;
    ray_t* (*fmt_fn)(ray_t*, int) = ray_fmt;

    TEST_ASSERT_NOT_NULL((void*)select_fn);
    TEST_ASSERT_NOT_NULL((void*)update_fn);
    TEST_ASSERT_NOT_NULL((void*)insert_fn);
    TEST_ASSERT_NOT_NULL((void*)upsert_fn);
    TEST_ASSERT_NOT_NULL((void*)fmt_fn);

    ray_t* s = ray_fmt(ray_i64(42), 0);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    TEST_ASSERT_EQ_I(s->type, -RAY_STR);
    TEST_ASSERT_EQ_I(ray_str_len(s), 2);
    TEST_ASSERT_MEM_EQ(2, ray_str_ptr(s), "42");
    ray_release(s);
    PASS();
}

/* ─── ray_obj_type / ray_obj_attrs ──────────────────────────────────
 *
 * The FFI helpers are thin readers of v->type and v->attrs.  Atoms
 * carry the negative form of the type tag; vectors carry the positive
 * tag.  RAY_LIST is type 0, RAY_TABLE is 98, RAY_DICT is 99. */

static test_result_t test_public_obj_type_atom_i64(void) {
    ray_t* v = ray_i64(42);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQ_I(ray_obj_type(v), -RAY_I64);
    TEST_ASSERT_EQ_I(ray_obj_attrs(v), 0);
    ray_release(v);
    PASS();
}

static test_result_t test_public_obj_type_atom_f64(void) {
    ray_t* v = ray_f64(3.14);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQ_I(ray_obj_type(v), -RAY_F64);
    TEST_ASSERT_EQ_I(ray_obj_attrs(v), 0);
    ray_release(v);
    PASS();
}

static test_result_t test_public_obj_type_atom_sym(void) {
    int64_t sid = ray_sym_intern("alpha", 5);
    ray_t* v = ray_sym(sid);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQ_I(ray_obj_type(v), -RAY_SYM);
    TEST_ASSERT_EQ_I(ray_obj_attrs(v), 0);
    ray_release(v);
    PASS();
}

static test_result_t test_public_obj_type_vec_i64(void) {
    ray_t* v = ray_vec_new(RAY_I64, 4);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(ray_obj_type(v), RAY_I64);
    TEST_ASSERT_EQ_I(ray_obj_attrs(v), 0);
    ray_release(v);
    PASS();
}

static test_result_t test_public_obj_type_vec_f64(void) {
    ray_t* v = ray_vec_new(RAY_F64, 4);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(ray_obj_type(v), RAY_F64);
    TEST_ASSERT_EQ_I(ray_obj_attrs(v), 0);
    ray_release(v);
    PASS();
}

static test_result_t test_public_obj_type_vec_sym(void) {
    /* ray_sym_vec_new stores the width in the low 2 bits of attrs;
     * ray_obj_attrs should expose them verbatim. */
    ray_t* v = ray_sym_vec_new(RAY_SYM_W32, 4);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(ray_obj_type(v), RAY_SYM);
    TEST_ASSERT_EQ_I(ray_obj_attrs(v) & 0x3, RAY_SYM_W32);
    ray_release(v);
    PASS();
}

static test_result_t test_public_obj_type_list(void) {
    ray_t* v = ray_list_new(2);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(ray_obj_type(v), RAY_LIST);
    TEST_ASSERT_EQ_I(ray_obj_attrs(v), 0);
    ray_release(v);
    PASS();
}

static test_result_t test_public_obj_type_table(void) {
    ray_t* tbl = ray_table_new(2);
    TEST_ASSERT_NOT_NULL(tbl);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT_EQ_I(ray_obj_type(tbl), RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_obj_attrs(tbl), 0);
    ray_release(tbl);
    PASS();
}

static test_result_t test_public_obj_type_dict(void) {
    /* Two-element typed-vec keys + typed-vec vals → dict. */
    ray_t* keys = ray_vec_new(RAY_I64, 2);
    int64_t k0 = 10, k1 = 20;
    keys = ray_vec_append(keys, &k0);
    keys = ray_vec_append(keys, &k1);
    ray_t* vals = ray_vec_new(RAY_I64, 2);
    int64_t v0 = 100, v1 = 200;
    vals = ray_vec_append(vals, &v0);
    vals = ray_vec_append(vals, &v1);

    ray_t* d = ray_dict_new(keys, vals);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_FALSE(RAY_IS_ERR(d));
    TEST_ASSERT_EQ_I(ray_obj_type(d), RAY_DICT);
    TEST_ASSERT_EQ_I(ray_obj_attrs(d), 0);
    ray_release(d);
    PASS();
}

/* ─── ray_vec_get_i64 — every integer width branch ───────────────────
 *
 * Implementation (src/core/runtime.c) dispatches on vec->type:
 *   I64 / DATE / TIME / TIMESTAMP  → int64_t cast
 *   I32                            → int32_t cast
 *   I16                            → int16_t cast
 *   U8  / BOOL                     → uint8_t cast
 *
 * For each branch read at idx 0, mid, and last to exercise the indexing
 * arithmetic on top of the type-specific element size. */

static test_result_t test_public_vec_get_i64_i64(void) {
    ray_t* v = ray_vec_new(RAY_I64, 5);
    int64_t xs[] = { -1000, 1, 2, 3, 9223372036854775000LL };
    for (int i = 0; i < 5; i++) v = ray_vec_append(v, &xs[i]);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 0), xs[0]);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 2), xs[2]);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 4), xs[4]);
    ray_release(v);
    PASS();
}

static test_result_t test_public_vec_get_i64_i32(void) {
    ray_t* v = ray_vec_new(RAY_I32, 4);
    int32_t xs[] = { -7, 0, 12345, 2147483600 };
    for (int i = 0; i < 4; i++) v = ray_vec_append(v, &xs[i]);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 0), (int64_t)xs[0]);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 1), (int64_t)xs[1]);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 3), (int64_t)xs[3]);
    ray_release(v);
    PASS();
}

static test_result_t test_public_vec_get_i64_i16(void) {
    ray_t* v = ray_vec_new(RAY_I16, 3);
    int16_t xs[] = { -32000, 0, 32000 };
    for (int i = 0; i < 3; i++) v = ray_vec_append(v, &xs[i]);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 0), (int64_t)xs[0]);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 1), (int64_t)xs[1]);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 2), (int64_t)xs[2]);
    ray_release(v);
    PASS();
}

static test_result_t test_public_vec_get_i64_u8(void) {
    ray_t* v = ray_vec_new(RAY_U8, 4);
    uint8_t xs[] = { 0, 1, 200, 255 };
    for (int i = 0; i < 4; i++) v = ray_vec_append(v, &xs[i]);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 0), (int64_t)xs[0]);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 2), (int64_t)xs[2]);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 3), (int64_t)xs[3]);
    ray_release(v);
    PASS();
}

static test_result_t test_public_vec_get_i64_bool(void) {
    ray_t* v = ray_vec_new(RAY_BOOL, 3);
    uint8_t xs[] = { 0, 1, 1 };
    for (int i = 0; i < 3; i++) v = ray_vec_append(v, &xs[i]);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 0), 0);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 1), 1);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 2), 1);
    ray_release(v);
    PASS();
}

/* RAY_DATE / RAY_TIME branches — element width is 4 bytes (int32) per
 * ray_type_sizes in src/core/types.c.  ray_vec_get_i64 must read them as
 * int32, not int64. */

static test_result_t test_public_vec_get_i64_date(void) {
    ray_t* v = ray_vec_new(RAY_DATE, 3);
    /* Pick three distinct int32 day values that differ in both halves so
     * a wrong-width read would catch obviously-wrong adjacent bytes. */
    int32_t xs[] = { 0, 8766, 19724 };  /* 2000.01.01, 2024.01.01, 2054.01.01 */
    for (int i = 0; i < 3; i++) v = ray_vec_append(v, &xs[i]);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 0), xs[0]);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 1), xs[1]);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 2), xs[2]);

    ray_t* epoch = ray_date(xs[0]);
    ray_t* formatted = ray_fmt(epoch, 0);
    TEST_ASSERT_NOT_NULL(formatted);
    TEST_ASSERT_FALSE(RAY_IS_ERR(formatted));
    TEST_ASSERT_EQ_I(ray_str_len(formatted), 10);
    TEST_ASSERT_MEM_EQ(10, ray_str_ptr(formatted), "2000.01.01");
    ray_release(formatted);
    ray_release(epoch);
    ray_release(v);
    PASS();
}

static test_result_t test_public_vec_get_i64_time(void) {
    ray_t* v = ray_vec_new(RAY_TIME, 3);
    int32_t xs[] = { 0, 43200000, 86399000 };  /* 00:00:00.000, 12:00:00.000, 23:59:59.000 */
    for (int i = 0; i < 3; i++) v = ray_vec_append(v, &xs[i]);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 0), xs[0]);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 1), xs[1]);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 2), xs[2]);

    ray_t* noon = ray_time(xs[1]);
    ray_t* formatted = ray_fmt(noon, 0);
    TEST_ASSERT_NOT_NULL(formatted);
    TEST_ASSERT_FALSE(RAY_IS_ERR(formatted));
    TEST_ASSERT_EQ_I(ray_str_len(formatted), 12);
    TEST_ASSERT_MEM_EQ(12, ray_str_ptr(formatted), "12:00:00.000");
    ray_release(formatted);
    ray_release(noon);
    ray_release(v);
    PASS();
}

static test_result_t test_public_vec_get_i64_timestamp(void) {
    ray_t* v = ray_vec_new(RAY_TIMESTAMP, 3);
    int64_t xs[] = { 0, 1700000000000000000LL, 1800000000000000000LL };
    for (int i = 0; i < 3; i++) v = ray_vec_append(v, &xs[i]);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 0), xs[0]);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 1), xs[1]);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(v, 2), xs[2]);
    ray_release(v);
    PASS();
}

/* ─── ray_vec_get_f64 — F32/F64 branches ─────────────────────────────
 *
 * Implementation accepts only RAY_F64 and RAY_F32; any other type
 * returns 0.0.  Integer vectors do NOT coerce — verified by reading
 * the source.  Cover only the supported (happy) types here. */

static test_result_t test_public_vec_get_f64_f64(void) {
    ray_t* v = ray_vec_new(RAY_F64, 4);
    double xs[] = { -1.5, 0.0, 2.25, 1e10 };
    for (int i = 0; i < 4; i++) v = ray_vec_append(v, &xs[i]);
    TEST_ASSERT_EQ_F(ray_vec_get_f64(v, 0), xs[0], 0.0);
    TEST_ASSERT_EQ_F(ray_vec_get_f64(v, 2), xs[2], 0.0);
    TEST_ASSERT_EQ_F(ray_vec_get_f64(v, 3), xs[3], 0.0);
    ray_release(v);
    PASS();
}

static test_result_t test_public_vec_get_f64_f32(void) {
    ray_t* v = ray_vec_new(RAY_F32, 3);
    float xs[] = { -0.5f, 1.25f, 3.5f };
    for (int i = 0; i < 3; i++) v = ray_vec_append(v, &xs[i]);
    /* F32 values round-trip exactly to double when they are representable
     * in 24-bit mantissa form — these are powers-of-two-fraction sums. */
    TEST_ASSERT_EQ_F(ray_vec_get_f64(v, 0), (double)xs[0], 1e-6);
    TEST_ASSERT_EQ_F(ray_vec_get_f64(v, 1), (double)xs[1], 1e-6);
    TEST_ASSERT_EQ_F(ray_vec_get_f64(v, 2), (double)xs[2], 1e-6);
    ray_release(v);
    PASS();
}

/* ─── ray_vec_get_sym_id — every SYM width ───────────────────────────
 *
 * The implementation dispatches through ray_read_sym which respects the
 * width-encoded attrs.  Use ray_sym_intern to obtain real IDs, append
 * via the W64-shaped int64 elem (ray_vec_append normalizes width), then
 * verify the round-trip.  W8 sym vec only addresses ≤255 distinct IDs;
 * the first builtins claim low slots so our user-interned names land in
 * a range that still fits an 8-bit index. */

static test_result_t test_public_vec_get_sym_id_w64(void) {
    int64_t a = ray_sym_intern("pub_w64_a", 9);
    int64_t b = ray_sym_intern("pub_w64_b", 9);
    int64_t c = ray_sym_intern("pub_w64_c", 9);

    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, 3);
    v = ray_vec_append(v, &a);
    v = ray_vec_append(v, &b);
    v = ray_vec_append(v, &c);

    TEST_ASSERT_EQ_I(ray_vec_get_sym_id(v, 0), a);
    TEST_ASSERT_EQ_I(ray_vec_get_sym_id(v, 1), b);
    TEST_ASSERT_EQ_I(ray_vec_get_sym_id(v, 2), c);
    ray_release(v);
    PASS();
}

static test_result_t test_public_vec_get_sym_id_w32(void) {
    int64_t a = ray_sym_intern("pub_w32_a", 9);
    int64_t b = ray_sym_intern("pub_w32_b", 9);

    ray_t* v = ray_sym_vec_new(RAY_SYM_W32, 2);
    v = ray_vec_append(v, &a);
    v = ray_vec_append(v, &b);
    TEST_ASSERT_EQ_I(ray_vec_get_sym_id(v, 0), a);
    TEST_ASSERT_EQ_I(ray_vec_get_sym_id(v, 1), b);
    ray_release(v);
    PASS();
}

static test_result_t test_public_vec_get_sym_id_w16(void) {
    int64_t a = ray_sym_intern("pub_w16_a", 9);
    int64_t b = ray_sym_intern("pub_w16_b", 9);

    ray_t* v = ray_sym_vec_new(RAY_SYM_W16, 2);
    v = ray_vec_append(v, &a);
    v = ray_vec_append(v, &b);
    TEST_ASSERT_EQ_I(ray_vec_get_sym_id(v, 0), a);
    TEST_ASSERT_EQ_I(ray_vec_get_sym_id(v, 1), b);
    ray_release(v);
    PASS();
}

static test_result_t test_public_vec_get_sym_id_w8(void) {
    /* W8 sym vectors store IDs 0-255 in a single byte.  We can't source
     * those IDs from ray_sym_intern: once the runtime is up the builtin
     * symbol set already occupies more than 256 IDs, so fresh interns land
     * above the W8 ceiling and would TRUNCATE on append (a different,
     * narrowing case).  This test targets the W8 storage round-trip, so use
     * in-range IDs directly — ray_vec_get_sym_id is a width-decoded read of
     * the stored byte (ray_read_sym), independent of whether the ID names a
     * live symbol. */
    int64_t a = 8;
    int64_t b = 200;

    ray_t* v = ray_sym_vec_new(RAY_SYM_W8, 2);
    v = ray_vec_append(v, &a);
    v = ray_vec_append(v, &b);
    TEST_ASSERT_EQ_I(ray_vec_get_sym_id(v, 0), a);
    TEST_ASSERT_EQ_I(ray_vec_get_sym_id(v, 1), b);
    ray_release(v);
    PASS();
}

/* ─── ray_runtime_create_with_sym* (happy path, eval round-trip) ─────
 *
 * These tests own their runtime lifecycle (no setup/teardown entry).
 * Pass a path that doesn't exist: per the contract, ENOENT is the
 * normal first-run case — out_sym_err stays RAY_OK and the runtime
 * comes up.  Run a trivial eval to confirm the language stack is live,
 * then destroy. */

static test_result_t test_public_runtime_create_with_sym_eval(void) {
    char tmpl[] = "/tmp/rayforce-pub-rt-XXXXXX";
    char* dir = mkdtemp(tmpl);
    TEST_ASSERT_NOT_NULL(dir);
    char path[256];
    snprintf(path, sizeof(path), "%s/missing.sym", dir);

    ray_runtime_t* rt = ray_runtime_create_with_sym(path);
    TEST_ASSERT_NOT_NULL(rt);

    /* Trivial eval — confirms ray_lang_init ran and the env has the
     * arithmetic builtin wired up.  Rayfall uses Lisp-style prefix
     * notation (see test_lang.c). */
    ray_t* r = ray_eval_str("(+ 1 2)");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_obj_type(r), -RAY_I64);
    TEST_ASSERT_EQ_I(r->i64, 3);
    ray_release(r);

    ray_runtime_destroy(rt);
    rmdir(dir);
    PASS();
}

static test_result_t test_public_runtime_create_with_sym_err_eval(void) {
    char tmpl[] = "/tmp/rayforce-pub-rt-err-XXXXXX";
    char* dir = mkdtemp(tmpl);
    TEST_ASSERT_NOT_NULL(dir);
    char path[256];
    snprintf(path, sizeof(path), "%s/missing.sym", dir);

    ray_err_t err = RAY_ERR_OOM;  /* poison */
    ray_runtime_t* rt = ray_runtime_create_with_sym_err(path, &err);
    TEST_ASSERT_NOT_NULL(rt);
    /* ENOENT is the documented first-run case: out_sym_err must be
     * cleared to RAY_OK by runtime_create_impl. */
    TEST_ASSERT_EQ_I((int)err, (int)RAY_OK);

    ray_t* r = ray_eval_str("(* 5 6)");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->i64, 30);
    ray_release(r);

    ray_runtime_destroy(rt);
    rmdir(dir);
    PASS();
}

/* ray_runtime_destroy(NULL) is documented as a no-op via the early
 * `if (!rt) return;` guard.  Pin that behaviour so a future refactor
 * can't silently break it.  No setup/teardown — we don't want a real
 * runtime alive when we hand the destroyer NULL. */
static test_result_t test_public_runtime_destroy_null_is_noop(void) {
    ray_runtime_destroy(NULL);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 * Interrupt API — global flag (eval.c).  Happy-path set/get round-trip
 * for the public ray_*_interrupt names and their legacy ray_eval_*
 * wrappers.  The flag is thread-local sig_atomic_t storage; here we
 * only verify the set→get→clear contract.
 * ═══════════════════════════════════════════════════════════════════════ */

static test_result_t test_public_interrupt_roundtrip(void) {
    ray_clear_interrupt();
    TEST_ASSERT_FALSE(ray_interrupted());

    ray_request_interrupt();
    TEST_ASSERT_TRUE(ray_interrupted());

    ray_clear_interrupt();
    TEST_ASSERT_FALSE(ray_interrupted());
    PASS();
}

static test_result_t test_public_interrupt_idempotent_set(void) {
    ray_clear_interrupt();
    ray_request_interrupt();
    ray_request_interrupt();
    TEST_ASSERT_TRUE(ray_interrupted());
    ray_clear_interrupt();
    TEST_ASSERT_FALSE(ray_interrupted());
    PASS();
}

static test_result_t test_public_eval_interrupt_wrappers(void) {
    ray_eval_clear_interrupt();
    TEST_ASSERT_EQ_I(ray_eval_is_interrupted(), 0);
    TEST_ASSERT_FALSE(ray_interrupted());

    ray_eval_request_interrupt();
    TEST_ASSERT_TRUE(ray_eval_is_interrupted() != 0);
    TEST_ASSERT_TRUE(ray_interrupted());

    ray_eval_clear_interrupt();
    TEST_ASSERT_EQ_I(ray_eval_is_interrupted(), 0);
    TEST_ASSERT_FALSE(ray_interrupted());
    PASS();
}

static test_result_t test_public_interrupt_cross_path(void) {
    ray_clear_interrupt();

    ray_request_interrupt();
    TEST_ASSERT_TRUE(ray_eval_is_interrupted() != 0);
    ray_clear_interrupt();

    ray_eval_request_interrupt();
    TEST_ASSERT_TRUE(ray_interrupted());
    ray_eval_clear_interrupt();
    TEST_ASSERT_FALSE(ray_interrupted());
    PASS();
}

/* nfo API — get/set returns the same handle. */
static test_result_t test_public_eval_nfo_roundtrip(void) {
    ray_t* prev = ray_eval_get_nfo();

    ray_eval_set_nfo(NULL);
    TEST_ASSERT_NULL(ray_eval_get_nfo());

    const char* src = "(+ 1 2)";
    ray_t* nfo = ray_nfo_create("test", 4, src, 7);
    TEST_ASSERT_NOT_NULL(nfo);
    TEST_ASSERT_FALSE(RAY_IS_ERR(nfo));

    ray_eval_set_nfo(nfo);
    TEST_ASSERT_EQ_PTR(ray_eval_get_nfo(), nfo);

    ray_eval_set_nfo(prev);
    ray_release(nfo);
    PASS();
}

/* Restricted-mode API — round-trips through the per-thread VM
 * (__VM->restricted), so a runtime must be live; without a bound VM the
 * setter is a no-op and the getter reports false. */
static test_result_t test_public_eval_restricted_setget(void) {
    ray_eval_set_restricted(false);
    TEST_ASSERT_FALSE(ray_eval_get_restricted());

    ray_eval_set_restricted(true);
    TEST_ASSERT_TRUE(ray_eval_get_restricted());

    ray_eval_set_restricted(false);
    TEST_ASSERT_FALSE(ray_eval_get_restricted());
    PASS();
}

static test_result_t test_public_eval_restricted_allows_arith(void) {
    ray_eval_set_restricted(true);
    ray_t* r = ray_eval_str("(+ 1 2)");
    ray_eval_set_restricted(false);

    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, -RAY_I64);
    TEST_ASSERT_EQ_I(r->i64, 3);
    ray_release(r);
    PASS();
}

/* Error-trace API — ray_eval_str clears the trace at entry. */
static test_result_t test_public_get_error_trace_populated(void) {
    ray_t* def = ray_eval_str("(set boom (fn [x] (+ x 1)))");
    TEST_ASSERT_NOT_NULL(def);
    TEST_ASSERT_FALSE(RAY_IS_ERR(def));
    ray_release(def);

    ray_t* err = ray_eval_str("(boom \"not-a-number\")");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_TRUE(RAY_IS_ERR(err));

    ray_t* trace = ray_get_error_trace();
    TEST_ASSERT_NOT_NULL(trace);
    TEST_ASSERT_EQ_I(trace->type, RAY_LIST);
    TEST_ASSERT_TRUE(ray_len(trace) > 0);

    ray_t* frame0 = ((ray_t**)ray_data(trace))[0];
    TEST_ASSERT_NOT_NULL(frame0);
    TEST_ASSERT_EQ_I(frame0->type, RAY_LIST);
    TEST_ASSERT_EQ_I(ray_len(frame0), 4);

    ray_error_free(err);
    PASS();
}

static test_result_t test_public_get_error_trace_cleared_on_eval(void) {
    ray_t* def = ray_eval_str("(set boom2 (fn [x] (+ x 1)))");
    TEST_ASSERT_NOT_NULL(def);
    ray_release(def);

    ray_t* err = ray_eval_str("(boom2 \"x\")");
    TEST_ASSERT_TRUE(RAY_IS_ERR(err));
    TEST_ASSERT_NOT_NULL(ray_get_error_trace());
    ray_error_free(err);

    ray_t* ok = ray_eval_str("(+ 10 20)");
    TEST_ASSERT_NOT_NULL(ok);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ok));
    TEST_ASSERT_NULL(ray_get_error_trace());
    ray_release(ok);
    PASS();
}

static test_result_t test_public_nullability_matrix(void) {
    ray_t* ordinary[] = {
        ray_sym(0),
        ray_str("", 0),
        ray_bool(false),
        ray_u8(0),
        ray_f32(0.0f),
    };
    ray_t* requested_null[] = {
        ray_typed_null(-RAY_SYM),
        ray_typed_null(-RAY_STR),
        ray_typed_null(-RAY_BOOL),
        ray_typed_null(-RAY_U8),
        ray_typed_null(-RAY_F32),
    };
    const bool requested_is_null[] = { false, false, true, true, true };

    for (size_t i = 0; i < sizeof(ordinary) / sizeof(ordinary[0]); i++) {
        TEST_ASSERT_NOT_NULL(ordinary[i]);
        TEST_ASSERT_FALSE(RAY_IS_ERR(ordinary[i]));
        TEST_ASSERT_FALSE(RAY_ATOM_IS_NULL(ordinary[i]));
        TEST_ASSERT_NOT_NULL(requested_null[i]);
        TEST_ASSERT_FALSE(RAY_IS_ERR(requested_null[i]));
        TEST_ASSERT_EQ_I(RAY_ATOM_IS_NULL(requested_null[i]), requested_is_null[i]);
        ray_release(ordinary[i]);
        ray_release(requested_null[i]);
    }
    PASS();
}

const test_entry_t public_api_entries[] = {
    { "public/ipc_client_symbols",        test_public_ipc_client_symbols,        NULL, NULL },
    { "public/query_and_format_symbols",  test_public_query_and_format_symbols,  NULL, NULL },

    { "public/obj_type_atom_i64",   test_public_obj_type_atom_i64,   public_api_setup, public_api_teardown },
    { "public/obj_type_atom_f64",   test_public_obj_type_atom_f64,   public_api_setup, public_api_teardown },
    { "public/obj_type_atom_sym",   test_public_obj_type_atom_sym,   public_api_setup, public_api_teardown },
    { "public/obj_type_vec_i64",    test_public_obj_type_vec_i64,    public_api_setup, public_api_teardown },
    { "public/obj_type_vec_f64",    test_public_obj_type_vec_f64,    public_api_setup, public_api_teardown },
    { "public/obj_type_vec_sym",    test_public_obj_type_vec_sym,    public_api_setup, public_api_teardown },
    { "public/obj_type_list",       test_public_obj_type_list,       public_api_setup, public_api_teardown },
    { "public/obj_type_table",      test_public_obj_type_table,      public_api_setup, public_api_teardown },
    { "public/obj_type_dict",       test_public_obj_type_dict,       public_api_setup, public_api_teardown },

    { "public/vec_get_i64_i64",        test_public_vec_get_i64_i64,        public_api_setup, public_api_teardown },
    { "public/vec_get_i64_i32",        test_public_vec_get_i64_i32,        public_api_setup, public_api_teardown },
    { "public/vec_get_i64_i16",        test_public_vec_get_i64_i16,        public_api_setup, public_api_teardown },
    { "public/vec_get_i64_u8",         test_public_vec_get_i64_u8,         public_api_setup, public_api_teardown },
    { "public/vec_get_i64_bool",       test_public_vec_get_i64_bool,       public_api_setup, public_api_teardown },
    { "public/vec_get_i64_date",       test_public_vec_get_i64_date,       public_api_setup, public_api_teardown },
    { "public/vec_get_i64_time",       test_public_vec_get_i64_time,       public_api_setup, public_api_teardown },
    { "public/vec_get_i64_timestamp",  test_public_vec_get_i64_timestamp,  public_api_setup, public_api_teardown },

    { "public/vec_get_f64_f64",   test_public_vec_get_f64_f64,   public_api_setup, public_api_teardown },
    { "public/vec_get_f64_f32",   test_public_vec_get_f64_f32,   public_api_setup, public_api_teardown },

    { "public/vec_get_sym_id_w64", test_public_vec_get_sym_id_w64, public_api_setup, public_api_teardown },
    { "public/vec_get_sym_id_w32", test_public_vec_get_sym_id_w32, public_api_setup, public_api_teardown },
    { "public/vec_get_sym_id_w16", test_public_vec_get_sym_id_w16, public_api_setup, public_api_teardown },
    { "public/vec_get_sym_id_w8",  test_public_vec_get_sym_id_w8,  public_api_setup, public_api_teardown },

    /* These tests manage their own runtime lifecycle. */
    { "public/runtime_create_with_sym_eval",      test_public_runtime_create_with_sym_eval,     NULL, NULL },
    { "public/runtime_create_with_sym_err_eval",  test_public_runtime_create_with_sym_err_eval, NULL, NULL },
    { "public/runtime_destroy_null_is_noop",      test_public_runtime_destroy_null_is_noop,     NULL, NULL },

    /* eval interrupt / nfo / restricted / error-trace public API. */
    { "public/interrupt_roundtrip",            test_public_interrupt_roundtrip,            NULL, NULL },
    { "public/interrupt_idempotent_set",       test_public_interrupt_idempotent_set,       NULL, NULL },
    { "public/eval_interrupt_wrappers",        test_public_eval_interrupt_wrappers,        NULL, NULL },
    { "public/interrupt_cross_path",           test_public_interrupt_cross_path,           NULL, NULL },
    { "public/eval_nfo_roundtrip",             test_public_eval_nfo_roundtrip,             public_api_setup, public_api_teardown },
    { "public/eval_restricted_setget",         test_public_eval_restricted_setget,         public_api_setup, public_api_teardown },
    { "public/eval_restricted_allows_arith",   test_public_eval_restricted_allows_arith,   public_api_setup, public_api_teardown },
    { "public/get_error_trace_populated",      test_public_get_error_trace_populated,      public_api_setup, public_api_teardown },
    { "public/get_error_trace_cleared_on_eval",test_public_get_error_trace_cleared_on_eval,public_api_setup, public_api_teardown },
    { "public/nullability_matrix",             test_public_nullability_matrix,             public_api_setup, public_api_teardown },

    { NULL, NULL, NULL, NULL },
};
