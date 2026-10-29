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
#include "lang/internal.h"   /* atom_eq */
#include "table/sym.h"
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

/* ---- atom_eq RAY_LIST structural compare -------------------------------
 *
 * atom_eq for RAY_LIST previously fell through to the default branch's
 * memcmp on the element pointer array, comparing pointer identity
 * instead of structural equality.  Two structurally-identical lists
 * with different element pointers (the common case after construction)
 * compared not-equal, breaking ray_group_indices_fn / ray_dict / distinct
 * fallback for any code that built composite-list keys (e.g. multi-key
 * group-by via the eval-level path).
 * --------------------------------------------------------------------- */

/* Helper: build a list of the given i64 atoms.  Caller releases. */
static ray_t* mk_i64_list(const int64_t* vals, int64_t n) {
    ray_t* l = ray_list_new(n);
    for (int64_t i = 0; i < n; i++) {
        ray_t* a = ray_i64(vals[i]);
        l = ray_list_append(l, a);
        ray_release(a);
    }
    return l;
}

static test_result_t test_atom_eq_list_basic(void) {
    int64_t va[] = {1, 2}, vb[] = {1, 2}, vc[] = {3, 4}, vd[] = {1, 2, 3};
    ray_t* a = mk_i64_list(va, 2);
    ray_t* b = mk_i64_list(vb, 2);
    ray_t* c = mk_i64_list(vc, 2);
    ray_t* d = mk_i64_list(vd, 3);

    /* Same shape, same values, different pointers — must compare equal. */
    TEST_ASSERT_TRUE(atom_eq(a, b));
    /* Same shape, different values — not equal. */
    TEST_ASSERT_FALSE(atom_eq(a, c));
    /* Same prefix, different lengths — not equal. */
    TEST_ASSERT_FALSE(atom_eq(a, d));
    /* Reflexive. */
    TEST_ASSERT_TRUE(atom_eq(a, a));

    ray_release(a); ray_release(b); ray_release(c); ray_release(d);
    PASS();
}

static test_result_t test_atom_eq_list_mixed_types(void) {
    /* Lists holding heterogeneous atom types — recursive compare must
     * dispatch on each element's own type. */
    ray_t* a = ray_list_new(3);
    a = ray_list_append(a, ray_i64(7));
    a = ray_list_append(a, ray_f64(3.14));
    a = ray_list_append(a, ray_str("hi", 2));

    ray_t* b = ray_list_new(3);
    b = ray_list_append(b, ray_i64(7));
    b = ray_list_append(b, ray_f64(3.14));
    b = ray_list_append(b, ray_str("hi", 2));

    ray_t* c = ray_list_new(3);
    c = ray_list_append(c, ray_i64(7));
    c = ray_list_append(c, ray_f64(3.14));
    c = ray_list_append(c, ray_str("HI", 2));

    TEST_ASSERT_TRUE(atom_eq(a, b));
    TEST_ASSERT_FALSE(atom_eq(a, c));   /* differs only in str case */

    /* Releasing each list also releases the appended atoms. */
    ray_release(a); ray_release(b); ray_release(c);
    PASS();
}

static test_result_t test_atom_eq_list_nested(void) {
    /* (list (list 1) (list 2 3)) vs (list (list 1) (list 2 3)) — must
     * recurse through the outer LIST into each inner LIST. */
    int64_t in1[] = {1};
    int64_t in23[] = {2, 3};
    int64_t in24[] = {2, 4};
    ray_t* inner_a1 = mk_i64_list(in1,  1);
    ray_t* inner_a2 = mk_i64_list(in23, 2);
    ray_t* inner_b1 = mk_i64_list(in1,  1);
    ray_t* inner_b2 = mk_i64_list(in23, 2);
    ray_t* inner_c2 = mk_i64_list(in24, 2);

    ray_t* a = ray_list_new(2);
    a = ray_list_append(a, inner_a1);
    a = ray_list_append(a, inner_a2);

    ray_t* b = ray_list_new(2);
    b = ray_list_append(b, inner_b1);
    b = ray_list_append(b, inner_b2);

    ray_t* c = ray_list_new(2);
    c = ray_list_append(c, inner_a1);
    c = ray_list_append(c, inner_c2);

    TEST_ASSERT_TRUE(atom_eq(a, b));
    TEST_ASSERT_FALSE(atom_eq(a, c));

    ray_release(inner_a1); ray_release(inner_a2);
    ray_release(inner_b1); ray_release(inner_b2);
    ray_release(inner_c2);
    ray_release(a); ray_release(b); ray_release(c);
    PASS();
}

static test_result_t test_atom_eq_list_with_nulls(void) {
    /* atom_eq's null short-circuit must apply per element when the
     * element is itself a null atom (typed null SYM, etc.). */
    ray_t* a = ray_list_new(2);
    a = ray_list_append(a, ray_i64(1));
    a = ray_list_append(a, ray_typed_null(-RAY_I64));

    ray_t* b = ray_list_new(2);
    b = ray_list_append(b, ray_i64(1));
    b = ray_list_append(b, ray_typed_null(-RAY_I64));

    ray_t* c = ray_list_new(2);
    c = ray_list_append(c, ray_i64(1));
    c = ray_list_append(c, ray_i64(0));      /* 0 is NOT null */

    TEST_ASSERT_TRUE(atom_eq(a, b));
    TEST_ASSERT_FALSE(atom_eq(a, c));

    ray_release(a); ray_release(b); ray_release(c);
    PASS();
}

static test_result_t test_atom_eq_list_empty(void) {
    /* Two empty lists are equal regardless of identity. */
    ray_t* a = ray_list_new(0);
    ray_t* b = ray_list_new(0);
    TEST_ASSERT_TRUE(atom_eq(a, b));
    ray_release(a); ray_release(b);
    PASS();
}

static test_result_t test_atom_eq_list_sym_atoms(void) {
    /* Composite group-by keys land here: each row's key is a fresh list
     * containing fresh sym atoms with the same interned id.  This was
     * exactly the q6 multi-key bug — different pointers, same id, must
     * compare equal. */
    ray_sym_init();
    int64_t s_a = ray_sym_intern("A", 1);
    int64_t s_b = ray_sym_intern("B", 1);

    ray_t* row1 = ray_list_new(2);
    row1 = ray_list_append(row1, ray_sym(s_a));
    row1 = ray_list_append(row1, ray_sym(s_b));

    ray_t* row2 = ray_list_new(2);
    row2 = ray_list_append(row2, ray_sym(s_a));
    row2 = ray_list_append(row2, ray_sym(s_b));

    ray_t* row3 = ray_list_new(2);
    row3 = ray_list_append(row3, ray_sym(s_b));
    row3 = ray_list_append(row3, ray_sym(s_a));   /* swapped */

    TEST_ASSERT_TRUE(atom_eq(row1, row2));
    TEST_ASSERT_FALSE(atom_eq(row1, row3));

    ray_release(row1); ray_release(row2); ray_release(row3);
    ray_sym_destroy();
    PASS();
}

static test_result_t test_atom_typed_null_f64(void) {
    /* ray_typed_null(-RAY_F64) stores NaN in the f64 payload AND sets
     * aux[0]&1.  Downstream kernels reading the slot raw see NaN. */
    ray_t* v = ray_typed_null(-RAY_F64);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_TRUE(ray_is_atom(v));
    TEST_ASSERT_EQ_I(v->type, -RAY_F64);
    TEST_ASSERT_TRUE(v->f64 != v->f64);          /* NaN by IEEE-754 */
    TEST_ASSERT_TRUE((v->aux[0] & 1) != 0);  /* typed-null bit also set */
    ray_release(v);
    PASS();
}

static test_result_t test_atom_typed_null_i64(void) {
    /* Integer typed nulls use the INT_MIN sentinel and set aux[0]&1. */
    ray_t* v = ray_typed_null(-RAY_I64);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(v->type, -RAY_I64);
    TEST_ASSERT_EQ_I(v->i64, NULL_I64);
    TEST_ASSERT_TRUE((v->aux[0] & 1) != 0);
    ray_release(v);
    PASS();
}

static test_result_t test_atom_typed_null_i16(void) {
    ray_t* v = ray_typed_null(-RAY_I16);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(v->type, -RAY_I16);
    TEST_ASSERT_EQ_I(v->i16, NULL_I16);
    TEST_ASSERT_TRUE((v->aux[0] & 1) != 0);
    ray_release(v);
    PASS();
}

static test_result_t test_atom_typed_null_i32(void) {
    ray_t* v = ray_typed_null(-RAY_I32);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(v->type, -RAY_I32);
    TEST_ASSERT_EQ_I(v->i32, NULL_I32);
    TEST_ASSERT_TRUE((v->aux[0] & 1) != 0);
    ray_release(v);
    PASS();
}

static test_result_t test_atom_typed_null_date(void) {
    ray_t* v = ray_typed_null(-RAY_DATE);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(v->type, -RAY_DATE);
    TEST_ASSERT_EQ_I(v->i32, NULL_I32);
    TEST_ASSERT_TRUE((v->aux[0] & 1) != 0);
    ray_release(v);
    PASS();
}

static test_result_t test_atom_typed_null_time(void) {
    ray_t* v = ray_typed_null(-RAY_TIME);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(v->type, -RAY_TIME);
    TEST_ASSERT_EQ_I(v->i32, NULL_I32);
    TEST_ASSERT_TRUE((v->aux[0] & 1) != 0);
    ray_release(v);
    PASS();
}

static test_result_t test_atom_typed_null_timestamp(void) {
    ray_t* v = ray_typed_null(-RAY_TIMESTAMP);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(v->type, -RAY_TIMESTAMP);
    TEST_ASSERT_EQ_I(v->i64, NULL_I64);
    TEST_ASSERT_TRUE((v->aux[0] & 1) != 0);
    ray_release(v);
    PASS();
}

/* ---- Suite definition -------------------------------------------------- */

/* ---- An exhausted heap: an oom error, nothing left allocated ----------- */

static ray_t* oom_str_empty(void) { return ray_str("", 0); }
static ray_t* oom_str_short(void) { return ray_str("abc", 3); }
static ray_t* oom_str_long(void)  { return ray_str("cannot add i64 and sym", 22); }
static ray_t* oom_sym(void)       { return ray_sym(ray_sym_intern("oom_atom", 8)); }

static size_t atom_heap_bytes(void) {
    ray_mem_stats_t s;
    ray_mem_stats(&s);
    return s.bytes_allocated;
}

/* Each allocation a constructor makes fails in turn: a string of up to 6
 * bytes and a symbol make one (the header), a longer string two (its
 * bytes, then the header).  Failed alone, the oom error the constructor
 * returns is a block of its own; failed with the error's allocation too,
 * it is the static RAY_OOM_OBJ.  Either way it is an error, not NULL, and
 * nothing is left allocated. */
static test_result_t test_atom_oom(void) {
    static const struct { ray_t* (*make)(void); int64_t allocs; } cases[] = {
        { oom_str_empty, 1 }, { oom_str_short, 1 }, { oom_str_long, 2 }, { oom_sym, 1 },
    };
    ray_sym_intern("oom_atom", 8);   /* interned before anything is failed */
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (int64_t nth = 1; nth <= cases[i].allocs; nth++) {
            for (int64_t count = 1; count <= 2; count++) {
                size_t before = atom_heap_bytes();
                ray_alloc_fail_set_for_test(nth, count);
                ray_t* v = cases[i].make();
                ray_alloc_fail_set_for_test(0, 0);
                TEST_ASSERT_TRUE(RAY_IS_ERR(v));
                TEST_ASSERT_STR_EQ(ray_err_code(v), "oom");
                TEST_ASSERT_TRUE(count == 1 ? v != RAY_OOM_OBJ : v == RAY_OOM_OBJ);
                ray_error_free(v);
                TEST_ASSERT_EQ_U(atom_heap_bytes(), before);
            }
        }
        /* Armed one past the last allocation: the atom is built. */
        ray_alloc_fail_set_for_test(cases[i].allocs + 1, 1);
        ray_t* v = cases[i].make();
        ray_alloc_fail_set_for_test(0, 0);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
        ray_release(v);
    }
    PASS();
}

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
    { "atom/oom", test_atom_oom, atom_setup, atom_teardown },
    { "atom/date", test_atom_date, atom_setup, atom_teardown },
    { "atom/time", test_atom_time, atom_setup, atom_teardown },
    { "atom/timestamp", test_atom_timestamp, atom_setup, atom_teardown },
    { "atom/guid", test_atom_guid, atom_setup, atom_teardown },
    { "atom/is_atom", test_is_atom, atom_setup, atom_teardown },
    { "atom/eq_list_basic",       test_atom_eq_list_basic,       atom_setup, atom_teardown },
    { "atom/eq_list_mixed_types", test_atom_eq_list_mixed_types, atom_setup, atom_teardown },
    { "atom/eq_list_nested",      test_atom_eq_list_nested,      atom_setup, atom_teardown },
    { "atom/eq_list_with_nulls",  test_atom_eq_list_with_nulls,  atom_setup, atom_teardown },
    { "atom/eq_list_empty",       test_atom_eq_list_empty,       atom_setup, atom_teardown },
    { "atom/eq_list_sym_atoms",   test_atom_eq_list_sym_atoms,   atom_setup, atom_teardown },
    { "atom/typed_null_f64",       test_atom_typed_null_f64,       atom_setup, atom_teardown },
    { "atom/typed_null_i64",       test_atom_typed_null_i64,       atom_setup, atom_teardown },
    { "atom/typed_null_i16",       test_atom_typed_null_i16,       atom_setup, atom_teardown },
    { "atom/typed_null_i32",       test_atom_typed_null_i32,       atom_setup, atom_teardown },
    { "atom/typed_null_date",      test_atom_typed_null_date,      atom_setup, atom_teardown },
    { "atom/typed_null_time",      test_atom_typed_null_time,      atom_setup, atom_teardown },
    { "atom/typed_null_timestamp", test_atom_typed_null_timestamp, atom_setup, atom_teardown },
    { NULL, NULL, NULL, NULL },
};

