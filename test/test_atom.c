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
#include <rayforce.h>
#include "mem/heap.h"
#include <stdatomic.h>
#include <string.h>

/* ---- Setup / Teardown -------------------------------------------------- */

static void atom_setup(void) {
    ray_heap_init();
}

static void atom_teardown(void) {
    ray_heap_destroy();
}

/* ---- Bool atom --------------------------------------------------------- */

static test_result_t test_atom_bool(void) {
    ray_t* t = ray_bool(true);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_FALSE(RAY_IS_ERR(t));
    TEST_ASSERT_TRUE(ray_is_atom(t));
    TEST_ASSERT_EQ_I(t->type, -RAY_BOOL);
    TEST_ASSERT_EQ_U(t->b8, 1);
    ray_release(t);

    ray_t* f = ray_bool(false);
    TEST_ASSERT_EQ_I(f->type, -RAY_BOOL);
    TEST_ASSERT_EQ_U(f->b8, 0);
    ray_release(f);

    PASS();
}

/* ---- U8 atom ----------------------------------------------------------- */

static test_result_t test_atom_u8(void) {
    ray_t* v = ray_u8(255);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_TRUE(ray_is_atom(v));
    TEST_ASSERT_EQ_I(v->type, -RAY_U8);
    TEST_ASSERT_EQ_U(v->u8, 255);
    ray_release(v);

    PASS();
}

/* ---- Single-char string atom ------------------------------------------ */

static test_result_t test_atom_single_char_str(void) {
    ray_t* v = ray_str("Z", 1);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_TRUE(ray_is_atom(v));
    TEST_ASSERT_EQ_I(v->type, -RAY_STR);
    TEST_ASSERT_EQ_U(v->slen, 1);
    TEST_ASSERT_EQ_I(v->sdata[0], 'Z');
    ray_release(v);

    PASS();
}

/* ---- I16 atom ---------------------------------------------------------- */

static test_result_t test_atom_i16(void) {
    ray_t* v = ray_i16(-1234);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_TRUE(ray_is_atom(v));
    TEST_ASSERT_EQ_I(v->type, -RAY_I16);
    TEST_ASSERT_EQ_I(v->i16, -1234);
    ray_release(v);

    PASS();
}

/* ---- I32 atom ---------------------------------------------------------- */

static test_result_t test_atom_i32(void) {
    ray_t* v = ray_i32(1000000);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_TRUE(ray_is_atom(v));
    TEST_ASSERT_EQ_I(v->type, -RAY_I32);
    TEST_ASSERT_EQ_I(v->i32, 1000000);
    ray_release(v);

    PASS();
}

/* ---- I64 atom ---------------------------------------------------------- */

static test_result_t test_atom_i64(void) {
    ray_t* v = ray_i64(9876543210LL);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_TRUE(ray_is_atom(v));
    TEST_ASSERT_EQ_I(v->type, -RAY_I64);
    TEST_ASSERT_EQ_I(v->i64, 9876543210LL);
    ray_release(v);

    PASS();
}

/* ---- F64 atom ---------------------------------------------------------- */

static test_result_t test_atom_f64(void) {
    ray_t* v = ray_f64(3.14159265358979);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_TRUE(ray_is_atom(v));
    TEST_ASSERT_EQ_I(v->type, -RAY_F64);
    TEST_ASSERT((v->f64) == (3.14159265358979), "double == failed");
    ray_release(v);

    PASS();
}

/* ---- String SSO (short) ------------------------------------------------ */

static test_result_t test_atom_str_sso(void) {
    const char* s = "hello";
    ray_t* v = ray_str(s, 5);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_TRUE(ray_is_atom(v));
    TEST_ASSERT_EQ_I(v->type, -RAY_STR);
    TEST_ASSERT_EQ_U(v->slen, 5);
    TEST_ASSERT_MEM_EQ(5, v->sdata, "hello");
    ray_release(v);

    /* Empty string */
    ray_t* e = ray_str("", 0);
    TEST_ASSERT_EQ_I(e->type, -RAY_STR);
    TEST_ASSERT_EQ_U(e->slen, 0);
    ray_release(e);

    /* Exactly 7 bytes — uses long-string path (no room for NUL in sdata[7]) */
    ray_t* m = ray_str("1234567", 7);
    TEST_ASSERT_EQ_I(m->type, -RAY_STR);
    TEST_ASSERT_EQ_U(ray_str_len(m), 7);
    TEST_ASSERT_MEM_EQ(7, ray_str_ptr(m), "1234567");
    ray_release(m);

    PASS();
}

/* ---- String long (> 7 bytes) ------------------------------------------- */

static test_result_t test_atom_str_long(void) {
    const char* s = "hello world!";
    size_t len = strlen(s);
    ray_t* v = ray_str(s, len);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_TRUE(ray_is_atom(v));
    TEST_ASSERT_EQ_I(v->type, -RAY_STR);

    /* For long strings, obj points to a CHAR vector */
    ray_t* chars = v->obj;
    TEST_ASSERT_NOT_NULL(chars);
    TEST_ASSERT_EQ_I(chars->type, RAY_U8);
    TEST_ASSERT_EQ_I(chars->len, (int64_t)len);
    TEST_ASSERT_MEM_EQ(len, ray_data(chars), s);

    /* Keep one guard ref so we can observe atom-owned release. */
    ray_retain(chars);
    TEST_ASSERT_EQ_U(chars->rc, 2);

    ray_release(v);
    TEST_ASSERT_EQ_U(chars->rc, 1);
    ray_release(chars);

    PASS();
}

/* ---- Symbol atom ------------------------------------------------------- */

static test_result_t test_atom_sym(void) {
    ray_t* v = ray_sym(42);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_TRUE(ray_is_atom(v));
    TEST_ASSERT_EQ_I(v->type, -RAY_SYM);
    TEST_ASSERT_EQ_I(v->i64, 42);
    ray_release(v);

    PASS();
}

/* ---- Date atom --------------------------------------------------------- */

static test_result_t test_atom_date(void) {
    ray_t* v = ray_date(19700);  /* days since 2000-01-01 */
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_TRUE(ray_is_atom(v));
    TEST_ASSERT_EQ_I(v->type, -RAY_DATE);
    TEST_ASSERT_EQ_I(v->i64, 19700);
    ray_release(v);

    PASS();
}

/* ---- Time atom --------------------------------------------------------- */

static test_result_t test_atom_time(void) {
    ray_t* v = ray_time(43200000);  /* milliseconds since midnight (12:00:00.000) */
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_TRUE(ray_is_atom(v));
    TEST_ASSERT_EQ_I(v->type, -RAY_TIME);
    TEST_ASSERT_EQ_I(v->i64, 43200000);
    ray_release(v);

    PASS();
}

/* ---- Timestamp atom ---------------------------------------------------- */

static test_result_t test_atom_timestamp(void) {
    ray_t* v = ray_timestamp(1700000000000000000LL);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_TRUE(ray_is_atom(v));
    TEST_ASSERT_EQ_I(v->type, -RAY_TIMESTAMP);
    TEST_ASSERT_EQ_I(v->i64, 1700000000000000000LL);
    ray_release(v);

    PASS();
}

/* ---- GUID atom --------------------------------------------------------- */

static test_result_t test_atom_guid(void) {
    uint8_t bytes[16] = {
        0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
        0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10
    };
    ray_t* v = ray_guid(bytes);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_TRUE(ray_is_atom(v));
    TEST_ASSERT_EQ_I(v->type, -RAY_GUID);

    /* obj points to a U8 vector of length 16 */
    ray_t* vec = v->obj;
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_EQ_I(vec->type, RAY_U8);
    TEST_ASSERT_EQ_I(vec->len, 16);
    TEST_ASSERT_MEM_EQ(16, ray_data(vec), bytes);

    ray_retain(vec);
    TEST_ASSERT_EQ_U(vec->rc, 2);

    ray_release(v);
    TEST_ASSERT_EQ_U(vec->rc, 1);
    ray_release(vec);

    PASS();
}

/* ---- is_atom correctness ----------------------------------------------- */

static test_result_t test_is_atom(void) {
    ray_t* a = ray_i64(0);
    TEST_ASSERT_TRUE(ray_is_atom(a));
    TEST_ASSERT_FALSE(ray_is_vec(a));
    ray_release(a);

    /* A raw alloc with type 0 is not an atom (LIST) */
    ray_t* b = ray_alloc(0);
    TEST_ASSERT_FALSE(ray_is_atom(b));
    ray_free(b);

    PASS();
}

/* ---- Suite definition -------------------------------------------------- */

const test_entry_t atom_entries[] = {
    { "atom/bool", test_atom_bool, atom_setup, atom_teardown },
    { "atom/u8", test_atom_u8, atom_setup, atom_teardown },
    { "atom/single_char_str", test_atom_single_char_str, atom_setup, atom_teardown },
    { "atom/i16", test_atom_i16, atom_setup, atom_teardown },
    { "atom/i32", test_atom_i32, atom_setup, atom_teardown },
    { "atom/i64", test_atom_i64, atom_setup, atom_teardown },
    { "atom/f64", test_atom_f64, atom_setup, atom_teardown },
    { "atom/str_sso", test_atom_str_sso, atom_setup, atom_teardown },
    { "atom/str_long", test_atom_str_long, atom_setup, atom_teardown },
    { "atom/sym", test_atom_sym, atom_setup, atom_teardown },
    { "atom/date", test_atom_date, atom_setup, atom_teardown },
    { "atom/time", test_atom_time, atom_setup, atom_teardown },
    { "atom/timestamp", test_atom_timestamp, atom_setup, atom_teardown },
    { "atom/guid", test_atom_guid, atom_setup, atom_teardown },
    { "atom/is_atom", test_is_atom, atom_setup, atom_teardown },
    { NULL, NULL, NULL, NULL },
};


