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
#include <rayforce.h>
#include "mem/heap.h"
#include "table/sym.h"
#include "lang/internal.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

/* Forward declarations for lang modules */
#include "lang/env.h"
#include "lang/parse.h"
#include "lang/eval.h"
#include "lang/format.h"
#include "ops/temporal.h"

/* Forward-declare runtime API to avoid ray_vm_t redefinition from runtime.h */
struct ray_runtime_s;
typedef struct ray_runtime_s ray_runtime_t;
extern ray_runtime_t* ray_runtime_create(int argc, char** argv);
extern void           ray_runtime_destroy(ray_runtime_t* rt);
extern ray_runtime_t *__RUNTIME;

/* ═══════════════════════════════════════════════════════════════
 * String-roundtrip assertion macros (mirrors rayforce test style)
 * ═══════════════════════════════════════════════════════════════ */

/* ASSERT_EQ: evaluate both sides, format, compare strings.
 * This mirrors the rayforce TEST_ASSERT_EQ semantics exactly:
 * both LHS and RHS are evaluated as expressions, formatted, then compared. */
#define ASSERT_EQ(expr, expected) do { \
    ray_t* _le = ray_eval_str(expr); \
    if (_le && RAY_IS_ERR(_le)) { \
        ray_t* _es = ray_fmt(_le, 0); \
        const char* _ep = _es ? ray_str_ptr(_es) : "?"; \
        int _en = _es ? (int)ray_str_len(_es) : 1; \
        fprintf(stderr, "  %s:%d: eval error: %.*s\n -- expr: %s\n", \
                __FILE__, __LINE__, _en, _ep, expr); \
        if (_es) ray_release(_es); \
        ray_error_free(_le); \
        FAIL("explicit MUNIT_FAIL"); \
    } \
    ray_t* _re = ray_eval_str(expected); \
    if (_re && RAY_IS_ERR(_re)) { \
        ray_t* _es = ray_fmt(_re, 0); \
        const char* _ep = _es ? ray_str_ptr(_es) : "?"; \
        int _en = _es ? (int)ray_str_len(_es) : 1; \
        fprintf(stderr, "  %s:%d: RHS eval error: %.*s\n -- expected: %s\n", \
                __FILE__, __LINE__, _en, _ep, expected); \
        if (_es) ray_release(_es); \
        ray_error_free(_re); \
        if (_le) ray_release(_le); \
        FAIL("explicit MUNIT_FAIL"); \
    } \
    ray_t* _ls = _le ? ray_fmt(_le, 0) : NULL; \
    ray_t* _rs = _re ? ray_fmt(_re, 0) : NULL; \
    const char* _lp = _ls ? ray_str_ptr(_ls) : "null"; \
    const char* _rp = _rs ? ray_str_ptr(_rs) : "null"; \
    int _ll = _ls ? (int)ray_str_len(_ls) : 4; \
    int _rl = _rs ? (int)ray_str_len(_rs) : 4; \
    int _same = (_ll == _rl) && (memcmp(_lp, _rp, (size_t)_rl) == 0); \
    if (_ls) ray_release(_ls); \
    if (_rs) ray_release(_rs); \
    if (_le) ray_release(_le); \
    if (_re) ray_release(_re); \
    if (!_same) { \
        fprintf(stderr, "  %s:%d: mismatch\n -- expr: %s\n", \
                __FILE__, __LINE__, expr); \
        FAIL("explicit MUNIT_FAIL"); \
    } \
} while(0)

/* ASSERT_ER: evaluate expr, assert it produces an error. */
#define ASSERT_ER(expr, err_substr) do { \
    ray_t* _le = ray_eval_str(expr); \
    if (!RAY_IS_ERR(_le)) { \
        ray_t* _s = _le ? ray_fmt(_le, 0) : NULL; \
        fprintf(stderr, "  %s:%d: expected error, got: %.*s\n -- expr: %s\n", \
                __FILE__, __LINE__, \
                (int)(_s ? ray_str_len(_s) : 0), \
                _s ? ray_str_ptr(_s) : "", expr); \
        if (_s) ray_release(_s); \
        if (_le) ray_release(_le); \
        FAIL("explicit MUNIT_FAIL"); \
    } \
    /* _le IS an error — reclaim via ray_error_free (ray_release is a no-op). */ \
    ray_error_free(_le); \
} while(0)

/* ---- Setup / Teardown ---- */

static void lang_setup(void) {
    ray_runtime_create(0, NULL);
}

static void lang_teardown(void) {
    ray_runtime_destroy(__RUNTIME);
}

/* ---- Dummy function for testing ---- */
static ray_t* dummy_unary(ray_t* x) { return ray_retain(x), x; }
static ray_t* dummy_binary(ray_t* x, ray_t* y) { (void)y; return ray_retain(x), x; }
static ray_t* dummy_vary(ray_t** args, int64_t n) { (void)n; return ray_retain(args[0]), args[0]; }

/* ---- Test: create unary function object ---- */
static test_result_t test_fn_unary(void) {
    ray_t* fn = ray_fn_unary("neg", RAY_FN_ATOMIC, dummy_unary);
    TEST_ASSERT_NOT_NULL(fn);
    TEST_ASSERT_FALSE(RAY_IS_ERR(fn));
    TEST_ASSERT_EQ_I(fn->type, RAY_UNARY);
    TEST_ASSERT((fn->attrs & RAY_FN_ATOMIC) != (0), "fn->attrs & RAY_FN_ATOMIC != 0");
    ray_release(fn);

    PASS();
}

/* ---- Test: create binary function object ---- */
static test_result_t test_fn_binary(void) {
    ray_t* fn = ray_fn_binary("+", RAY_FN_ATOMIC, dummy_binary);
    TEST_ASSERT_NOT_NULL(fn);
    TEST_ASSERT_EQ_I(fn->type, RAY_BINARY);
    ray_release(fn);

    PASS();
}

/* ---- Test: create vary function object ---- */
static test_result_t test_fn_vary(void) {
    ray_t* fn = ray_fn_vary("list", RAY_FN_NONE, dummy_vary);
    TEST_ASSERT_NOT_NULL(fn);
    TEST_ASSERT_EQ_I(fn->type, RAY_VARY);
    ray_release(fn);

    PASS();
}

/* ---- Test: lex integer ---- */
static test_result_t test_lex_i64(void) {
    ray_t* result = ray_parse("42");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 42);
    ray_release(result);
    PASS();
}

/* ---- Test: lex negative integer ---- */
static test_result_t test_lex_neg_i64(void) {
    ray_t* result = ray_parse("-7");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, -7);
    ray_release(result);
    PASS();
}

/* ---- Test: lex float ---- */
static test_result_t test_lex_f64(void) {
    ray_t* result = ray_parse("3.14");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQ_I(result->type, -RAY_F64);
    TEST_ASSERT((result->f64) == (3.14), "double == failed");
    ray_release(result);
    PASS();
}

/* ---- Test: lex string ---- */
static test_result_t test_lex_string(void) {
    ray_t* result = ray_parse("\"hello\"");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQ_I(result->type, -RAY_STR);
    ray_release(result);
    PASS();
}

/* ---- Test: lex symbol ---- */
static test_result_t test_lex_symbol(void) {
    ray_t* result = ray_parse("'AAPL");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQ_I(result->type, -RAY_SYM);
    ray_release(result);
    PASS();
}

/* ---- Test: lex true/false ---- */
static test_result_t test_lex_bool(void) {
    ray_t* t = ray_parse("true");
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQ_I(t->type, -RAY_BOOL);
    TEST_ASSERT_EQ_U(t->b8, 1);
    ray_release(t);

    ray_t* f = ray_parse("false");
    TEST_ASSERT_EQ_I(f->type, -RAY_BOOL);
    TEST_ASSERT_EQ_U(f->b8, 0);
    ray_release(f);
    PASS();
}

/* ---- Test: parse s-expression ---- */
static test_result_t test_parse_sexpr(void) {
    ray_t* result = ray_parse("(+ 1 2)");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* Should be a list of 3 elements: [name:"+", 1, 2] */
    TEST_ASSERT_EQ_I(result->type, RAY_LIST);
    TEST_ASSERT_EQ_I(ray_len(result), 3);
    ray_release(result);
    PASS();
}

/* ---- Test: parse nested s-expressions ---- */
static test_result_t test_parse_nested(void) {
    ray_t* result = ray_parse("(+ (* 2 3) 4)");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQ_I(result->type, RAY_LIST);
    TEST_ASSERT_EQ_I(ray_len(result), 3);
    /* Second element should be a list (the nested (* 2 3)) */
    ray_t** elems = (ray_t**)ray_data(result);
    TEST_ASSERT_EQ_I(elems[1]->type, RAY_LIST);
    TEST_ASSERT_EQ_I(ray_len(elems[1]), 3);
    ray_release(result);
    PASS();
}

/* ---- Test: parse vector literal ---- */
static test_result_t test_parse_vector(void) {
    ray_t* result = ray_parse("[1 2 3]");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* Should be a list of 3 i64 elements */
    TEST_ASSERT_EQ_I(ray_len(result), 3);
    ray_release(result);
    PASS();
}

/* ---- Test: parse empty list ---- */
static test_result_t test_parse_empty_list(void) {
    ray_t* result = ray_parse("()");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQ_I(result->type, RAY_LIST);
    TEST_ASSERT_EQ_I(ray_len(result), 0);
    ray_release(result);
    PASS();
}

/* ---- Test: eval literal passthrough ---- */
static test_result_t test_eval_literal(void) {
    ray_t* result = ray_eval_str("42");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 42);
    ray_release(result);
    PASS();
}

/* ---- Test: eval addition ---- */
static test_result_t test_eval_add(void) {
    ray_t* result = ray_eval_str("(+ 1 2)");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 3);
    ray_release(result);
    PASS();
}

/* ---- Test: eval nested arithmetic ---- */
static test_result_t test_eval_nested_arith(void) {
    ray_t* result = ray_eval_str("(+ (* 2 3) 4)");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 10);
    ray_release(result);
    PASS();
}

/* ---- Test: eval subtraction ---- */
static test_result_t test_eval_sub(void) {
    ray_t* result = ray_eval_str("(- 10 3)");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQ_I(result->i64, 7);
    ray_release(result);
    PASS();
}

/* ---- Test: eval division ---- */
static test_result_t test_eval_div(void) {
    ray_t* result = ray_eval_str("(/ 10 3)");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQ_I(result->i64, 3);
    ray_release(result);
    PASS();
}

/* ---- Test: eval comparison ---- */
static test_result_t test_eval_cmp(void) {
    ray_t* result = ray_eval_str("(> 5 3)");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQ_I(result->type, -RAY_BOOL);
    TEST_ASSERT_EQ_U(result->b8, 1);
    ray_release(result);
    PASS();
}

/* ---- Test: eval set ---- */
static test_result_t test_eval_set(void) {
    ray_t* result = ray_eval_str("(do (set x 10) x)");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 10);
    ray_release(result);
    PASS();
}

/* ---- Test: eval if true ---- */
static test_result_t test_eval_if_true(void) {
    ray_t* result = ray_eval_str("(if true 1 2)");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 1);
    ray_release(result);
    PASS();
}

/* ---- Test: eval if false ---- */
static test_result_t test_eval_if_false(void) {
    ray_t* result = ray_eval_str("(if false 1 2)");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 2);
    ray_release(result);
    PASS();
}

/* ---- Test: eval let ---- */
static test_result_t test_eval_let(void) {
    ray_t* result = ray_eval_str("(do (let x 5) (+ x 3))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 8);
    ray_release(result);
    PASS();
}

/* ---- Test: eval lambda ---- */
static test_result_t test_eval_lambda(void) {
    ray_t* result = ray_eval_str("(do (set double (fn [x] (* x 2))) (double 5))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 10);
    ray_release(result);
    PASS();
}

/* ---- Test: eval lambda with multiple params ---- */
static test_result_t test_eval_lambda_multi(void) {
    ray_t* result = ray_eval_str("(do (set add3 (fn [a b c] (+ a (+ b c)))) (add3 1 2 3))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 6);
    ray_release(result);
    PASS();
}

/* ---- Test: eval lambda with let in body ---- */
static test_result_t test_eval_lambda_let(void) {
    ray_t* result = ray_eval_str("(do (set f (fn [a b] (let c (+ a b)) (+ c 1))) (f 3 4))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 8);
    ray_release(result);
    PASS();
}

/* ---- Test: compile basic lambda ---- */
static test_result_t test_compile_basic(void) {
    ray_t* result = ray_eval_str("(do (set f (fn [x] (+ x 1))) (f 10))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 11);
    ray_release(result);
    PASS();
}

/* ---- Test: compile closure ---- */
static test_result_t test_compile_closure(void) {
    /* Verify compiled lambda with multiple body exprs and let binding */
    ray_t* result = ray_eval_str("(do (set f (fn [a b] (let c (+ a b)) (* c 2))) (f 3 4))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 14);
    ray_release(result);
    PASS();
}

/* ---- Test: VM recursive fibonacci ---- */
static test_result_t test_vm_fib(void) {
    ray_t* result = ray_eval_str(
        "(do (set fib (fn [n] (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2)))))) (fib 10))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 55);
    ray_release(result);
    PASS();
}

/* ---- Test: VM tail-recursive loop ---- */
static test_result_t test_vm_loop(void) {
    ray_t* result = ray_eval_str(
        "(do (set sum-to (fn [n acc] (if (== n 0) acc (sum-to (- n 1) (+ acc n))))) (sum-to 100 0))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 5050);
    ray_release(result);
    PASS();
}

/* ---- Test: try catches division by zero ---- */
static test_result_t test_eval_try(void) {
    ray_t* result = ray_eval_str("(try (/ 10 0) (fn [e] 0))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_TRUE(result->i64 == 0 || result->i64 == INT64_MIN);
    ray_release(result);
    PASS();
}

/* ---- Test: try catches explicit raise ---- */
static test_result_t test_eval_raise(void) {
    ray_t* result = ray_eval_str("(try (raise \"boom\") (fn [e] 42))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 42);
    ray_release(result);
    PASS();
}

/* ---- Test: vector + scalar auto-mapping ---- */
static test_result_t test_eval_vector_add(void) {
    ray_t* result = ray_eval_str("(+ [1 2 3] 10)");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_I64);
    TEST_ASSERT_EQ_I(ray_len(result), 3);
    int64_t* elems = (int64_t*)ray_data(result);
    TEST_ASSERT_EQ_I(elems[0], 11);
    TEST_ASSERT_EQ_I(elems[1], 12);
    TEST_ASSERT_EQ_I(elems[2], 13);
    ray_release(result);
    PASS();
}

/* ---- Test: vector + vector auto-mapping ---- */
static test_result_t test_eval_vector_add_vec(void) {
    ray_t* result = ray_eval_str("(+ [1 2 3] [4 5 6])");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_I64);
    TEST_ASSERT_EQ_I(ray_len(result), 3);
    int64_t* elems = (int64_t*)ray_data(result);
    TEST_ASSERT_EQ_I(elems[0], 5);
    TEST_ASSERT_EQ_I(elems[1], 7);
    TEST_ASSERT_EQ_I(elems[2], 9);
    ray_release(result);
    PASS();
}

/* ---- Test: sum aggregation ---- */
static test_result_t test_eval_sum(void) {
    ray_t* result = ray_eval_str("(sum [1 2 3 4 5])");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 15);
    ray_release(result);
    PASS();
}

/* ---- Test: count ---- */
static test_result_t test_eval_count(void) {
    ray_t* result = ray_eval_str("(count [1 2 3])");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 3);
    ray_release(result);
    PASS();
}

/* ---- Test: avg ---- */
static test_result_t test_eval_avg(void) {
    ray_t* result = ray_eval_str("(avg [2 4 6])");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_F64);
    TEST_ASSERT((result->f64) == (4.0), "double == failed");
    ray_release(result);
    PASS();
}

/* ---- Test: min/max ---- */
static test_result_t test_eval_min_max(void) {
    ray_t* mn = ray_eval_str("(min [5 2 8])");
    TEST_ASSERT_NOT_NULL(mn);
    TEST_ASSERT_FALSE(RAY_IS_ERR(mn));
    TEST_ASSERT_EQ_I(mn->type, -RAY_I64);
    TEST_ASSERT_EQ_I(mn->i64, 2);
    ray_release(mn);

    ray_t* mx = ray_eval_str("(max [5 2 8])");
    TEST_ASSERT_NOT_NULL(mx);
    TEST_ASSERT_FALSE(RAY_IS_ERR(mx));
    TEST_ASSERT_EQ_I(mx->type, -RAY_I64);
    TEST_ASSERT_EQ_I(mx->i64, 8);
    ray_release(mx);
    PASS();
}

/* ---- Test: first/last ---- */
static test_result_t test_eval_first_last(void) {
    ray_t* f = ray_eval_str("(first [1 2 3])");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_FALSE(RAY_IS_ERR(f));
    TEST_ASSERT_EQ_I(f->type, -RAY_I64);
    TEST_ASSERT_EQ_I(f->i64, 1);
    ray_release(f);

    ray_t* l = ray_eval_str("(last [1 2 3])");
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_FALSE(RAY_IS_ERR(l));
    TEST_ASSERT_EQ_I(l->type, -RAY_I64);
    TEST_ASSERT_EQ_I(l->i64, 3);
    ray_release(l);
    PASS();
}

/* ---- Test: map with binary fn and value ---- */
static test_result_t test_eval_map(void) {
    ray_t* result = ray_eval_str("(map + 1 [1 2 3])");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_LIST);
    TEST_ASSERT_EQ_I(ray_len(result), 3);
    ray_t** elems = (ray_t**)ray_data(result);
    TEST_ASSERT_EQ_I(elems[0]->i64, 2);
    TEST_ASSERT_EQ_I(elems[1]->i64, 3);
    TEST_ASSERT_EQ_I(elems[2]->i64, 4);
    ray_release(result);
    PASS();
}

/* ---- Test: pmap with binary fn and value ---- */
static test_result_t test_eval_pmap(void) {
    ray_t* result = ray_eval_str("(pmap * 2 [1 2 3])");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_LIST);
    TEST_ASSERT_EQ_I(ray_len(result), 3);
    ray_t** elems = (ray_t**)ray_data(result);
    TEST_ASSERT_EQ_I(elems[0]->i64, 2);
    TEST_ASSERT_EQ_I(elems[1]->i64, 4);
    TEST_ASSERT_EQ_I(elems[2]->i64, 6);
    ray_release(result);
    PASS();
}

/* ---- Test: fold (reduce) ---- */
static test_result_t test_eval_fold(void) {
    ray_t* result = ray_eval_str("(fold + [1 2 3 4 5])");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 15);
    ray_release(result);
    PASS();
}

/* ---- Test: scan (running fold) ---- */
static test_result_t test_eval_scan(void) {
    ray_t* result = ray_eval_str("(scan + [1 2 3 4 5])");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_LIST);
    TEST_ASSERT_EQ_I(ray_len(result), 5);
    ray_t** elems = (ray_t**)ray_data(result);
    TEST_ASSERT_EQ_I(elems[0]->i64, 1);
    TEST_ASSERT_EQ_I(elems[1]->i64, 3);
    TEST_ASSERT_EQ_I(elems[2]->i64, 6);
    TEST_ASSERT_EQ_I(elems[3]->i64, 10);
    TEST_ASSERT_EQ_I(elems[4]->i64, 15);
    ray_release(result);
    PASS();
}

/* ---- Test: filter by boolean mask ---- */
static test_result_t test_eval_filter(void) {
    ray_t* result = ray_eval_str("(filter [1 2 3 4 5] [true false true false true])");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_len(result), 3);
    if (result->type == RAY_I64) {
        int64_t* d = (int64_t*)ray_data(result);
        TEST_ASSERT_EQ_I(d[0], 1);
        TEST_ASSERT_EQ_I(d[1], 3);
        TEST_ASSERT_EQ_I(d[2], 5);
    } else {
        ray_t** elems = (ray_t**)ray_data(result);
        TEST_ASSERT_EQ_I(elems[0]->i64, 1);
        TEST_ASSERT_EQ_I(elems[1]->i64, 3);
        TEST_ASSERT_EQ_I(elems[2]->i64, 5);
    }
    ray_release(result);
    PASS();
}

/* ---- Test: apply (zip-apply) ---- */
static test_result_t test_eval_apply(void) {
    ray_t* result = ray_eval_str("(apply + [1 2] [3 4])");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_LIST);
    TEST_ASSERT_EQ_I(ray_len(result), 2);
    ray_t** elems = (ray_t**)ray_data(result);
    TEST_ASSERT_EQ_I(elems[0]->i64, 4);
    TEST_ASSERT_EQ_I(elems[1]->i64, 6);
    ray_release(result);
    PASS();
}

/* ---- Test: distinct ---- */
static test_result_t test_eval_distinct(void) {
    ray_t* result = ray_eval_str("(distinct [1 1 2 2 3])");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_TRUE(result->type == RAY_LIST || ray_is_vec(result));
    TEST_ASSERT_EQ_I(ray_len(result), 3);
    if (result->type == RAY_LIST) {
        ray_t** elems = (ray_t**)ray_data(result);
        TEST_ASSERT_EQ_I(elems[0]->i64, 1);
        TEST_ASSERT_EQ_I(elems[1]->i64, 2);
        TEST_ASSERT_EQ_I(elems[2]->i64, 3);
    } else {
        int64_t* vals = (int64_t*)ray_data(result);
        TEST_ASSERT_EQ_I(vals[0], 1);
        TEST_ASSERT_EQ_I(vals[1], 2);
        TEST_ASSERT_EQ_I(vals[2], 3);
    }
    ray_release(result);
    PASS();
}

/* ---- Test: in ---- */
static test_result_t test_eval_in(void) {
    ray_t* result = ray_eval_str("(in 2 [1 2 3])");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_BOOL);
    TEST_ASSERT_EQ_U(result->b8, 1);
    ray_release(result);

    ray_t* result2 = ray_eval_str("(in 9 [1 2 3])");
    TEST_ASSERT_NOT_NULL(result2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result2));
    TEST_ASSERT_EQ_I(result2->type, -RAY_BOOL);
    TEST_ASSERT_EQ_U(result2->b8, 0);
    ray_release(result2);
    PASS();
}

/* ---- Test: except ---- */
static test_result_t test_eval_except(void) {
    ray_t* result = ray_eval_str("(except [1 2 3] [2])");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_TRUE(result->type == RAY_LIST || ray_is_vec(result));
    TEST_ASSERT_EQ_I(ray_len(result), 2);
    if (result->type == RAY_LIST) {
        ray_t** elems = (ray_t**)ray_data(result);
        TEST_ASSERT_EQ_I(elems[0]->i64, 1);
        TEST_ASSERT_EQ_I(elems[1]->i64, 3);
    } else {
        int64_t* vals = (int64_t*)ray_data(result);
        TEST_ASSERT_EQ_I(vals[0], 1);
        TEST_ASSERT_EQ_I(vals[1], 3);
    }
    ray_release(result);
    PASS();
}

/* ---- Test: union ---- */
static test_result_t test_eval_union(void) {
    ray_t* result = ray_eval_str("(union [1 2] [2 3])");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_TRUE(result->type == RAY_LIST || ray_is_vec(result));
    TEST_ASSERT_EQ_I(ray_len(result), 3);
    if (result->type == RAY_LIST) {
        ray_t** elems = (ray_t**)ray_data(result);
        TEST_ASSERT_EQ_I(elems[0]->i64, 1);
        TEST_ASSERT_EQ_I(elems[1]->i64, 2);
        TEST_ASSERT_EQ_I(elems[2]->i64, 3);
    } else {
        int64_t* vals = (int64_t*)ray_data(result);
        TEST_ASSERT_EQ_I(vals[0], 1);
        TEST_ASSERT_EQ_I(vals[1], 2);
        TEST_ASSERT_EQ_I(vals[2], 3);
    }
    ray_release(result);
    PASS();
}

/* ---- Test: sect (intersection) ---- */
static test_result_t test_eval_sect(void) {
    ray_t* result = ray_eval_str("(sect [1 2 3] [2 3 4])");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_TRUE(result->type == RAY_LIST || ray_is_vec(result));
    TEST_ASSERT_EQ_I(ray_len(result), 2);
    if (result->type == RAY_LIST) {
        ray_t** elems = (ray_t**)ray_data(result);
        TEST_ASSERT_EQ_I(elems[0]->i64, 2);
        TEST_ASSERT_EQ_I(elems[1]->i64, 3);
    } else {
        int64_t* vals = (int64_t*)ray_data(result);
        TEST_ASSERT_EQ_I(vals[0], 2);
        TEST_ASSERT_EQ_I(vals[1], 3);
    }
    ray_release(result);
    PASS();
}

/* ---- Test: take positive ---- */
static test_result_t test_eval_take(void) {
    ray_t* result = ray_eval_str("(take [1 2 3 4 5] 3)");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_TRUE(result->type == RAY_LIST || ray_is_vec(result));
    TEST_ASSERT_EQ_I(ray_len(result), 3);
    if (result->type == RAY_LIST) {
        ray_t** elems = (ray_t**)ray_data(result);
        TEST_ASSERT_EQ_I(elems[0]->i64, 1);
        TEST_ASSERT_EQ_I(elems[1]->i64, 2);
        TEST_ASSERT_EQ_I(elems[2]->i64, 3);
    } else {
        int64_t* vals = (int64_t*)ray_data(result);
        TEST_ASSERT_EQ_I(vals[0], 1);
        TEST_ASSERT_EQ_I(vals[1], 2);
        TEST_ASSERT_EQ_I(vals[2], 3);
    }
    ray_release(result);
    PASS();
}

/* ---- Test: take negative (from end) ---- */
static test_result_t test_eval_take_neg(void) {
    ray_t* result = ray_eval_str("(take [1 2 3 4 5] -3)");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_TRUE(result->type == RAY_LIST || ray_is_vec(result));
    TEST_ASSERT_EQ_I(ray_len(result), 3);
    if (result->type == RAY_LIST) {
        ray_t** elems = (ray_t**)ray_data(result);
        TEST_ASSERT_EQ_I(elems[0]->i64, 3);
        TEST_ASSERT_EQ_I(elems[1]->i64, 4);
        TEST_ASSERT_EQ_I(elems[2]->i64, 5);
    } else {
        int64_t* vals = (int64_t*)ray_data(result);
        TEST_ASSERT_EQ_I(vals[0], 3);
        TEST_ASSERT_EQ_I(vals[1], 4);
        TEST_ASSERT_EQ_I(vals[2], 5);
    }
    ray_release(result);
    PASS();
}

/* ---- Test: at (index into vector) ---- */
static test_result_t test_eval_at(void) {
    ray_t* result = ray_eval_str("(at [10 20 30] 1)");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 20);
    ray_release(result);
    PASS();
}

/* ---- Test: find ---- */
static test_result_t test_eval_find(void) {
    ray_t* result = ray_eval_str("(find [1 2 3] 2)");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 1);
    ray_release(result);
    PASS();
}

/* ---- Test: reverse ---- */
static test_result_t test_eval_reverse(void) {
    ray_t* result = ray_eval_str("(reverse [1 2 3])");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_I64);
    TEST_ASSERT_EQ_I(ray_len(result), 3);
    int64_t* data = (int64_t*)ray_data(result);
    TEST_ASSERT_EQ_I(data[0], 3);
    TEST_ASSERT_EQ_I(data[1], 2);
    TEST_ASSERT_EQ_I(data[2], 1);
    ray_release(result);
    PASS();
}

/* ---- Test: table construction ---- */
static test_result_t test_eval_table(void) {
    ray_t* result = ray_eval_str("(table ['a 'b] (list [1 2 3] [10 20 30]))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_ncols(result), 2);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    ray_release(result);
    PASS();
}

/* ---- Test: at table (column access) ---- */
static test_result_t test_eval_at_table(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['a 'b] (list [1 2 3] [10 20 30]))) (at t 'a))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_I64);
    TEST_ASSERT_EQ_I(ray_len(result), 3);
    int64_t* data = (int64_t*)ray_data(result);
    TEST_ASSERT_EQ_I(data[0], 1);
    TEST_ASSERT_EQ_I(data[1], 2);
    TEST_ASSERT_EQ_I(data[2], 3);
    ray_release(result);
    PASS();
}

/* ---- Test: key table (column names) ---- */
static test_result_t test_eval_key_table(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['a 'b] (list [1 2 3] [10 20 30]))) (key t))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_SYM);
    TEST_ASSERT_EQ_I(ray_len(result), 2);
    ray_release(result);
    PASS();
}

/* ---- Test: count table (row count) ---- */
static test_result_t test_eval_count_table(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['a 'b] (list [1 2 3] [10 20 30]))) (count t))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 3);
    ray_release(result);
    PASS();
}

/* ---- Test: select all ---- */
static test_result_t test_eval_select_all(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['name 'salary] (list [1 2 3] [50000 60000 70000]))) "
        "(select {from: t}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    TEST_ASSERT_EQ_I(ray_table_ncols(result), 2);
    ray_release(result);
    PASS();
}

/* ---- Test: select where ---- */
static test_result_t test_eval_select_where(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['name 'salary] (list [1 2 3] [50000 60000 70000]))) "
        "(select {from: t where: (> salary 55000)}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);
    ray_release(result);
    PASS();
}

/* ---- Test: WHERE with `in` and a literal sym vector ----
 * New in compile_expr_dag completeness pass. */
static test_result_t test_eval_select_where_in_sym(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['s 'p] "
        "(list [A B C A B C] [10 20 30 40 50 60]))) "
        "(select {from: t where: (in s [A C])}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* Should filter to A, C, A, C — 4 rows */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 4);
    ray_release(result);
    PASS();
}

/* ---- Test: OP_IN type-mismatch must not leak via atom probe ----
 * Deterministic regression: interns a sym so we know its numeric
 * ID N, builds an i64 column containing exactly N, then runs
 * `(in col 'that_sym)`.  With the buggy code (atom branch ignoring
 * the SYM-vs-non-SYM set_len=0 suppression), sv[0] would be the
 * literal's sym ID N, col[0] is also N, they collide, and `in`
 * falsely returns 1 row.  With the fix set_len stays 0, the probe
 * is empty, no false match. */

/* ---- Test: lambda inlining in non-agg column expression ---- */
static test_result_t test_eval_select_lambda_nonagg(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['s 'p 'q] "
        "(list [A B A B] [10.0 20.0 30.0 40.0] [1 2 3 4]))) "
        "(select {from: t by: s m: ((fn [x y] (+ x y)) p q)}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);
    int64_t m_id = ray_sym_intern("m", 1);
    ray_t* m_col = ray_table_get_col(result, m_id);
    TEST_ASSERT_NOT_NULL(m_col);
    TEST_ASSERT_EQ_I(m_col->type, RAY_LIST);
    ray_release(result);
    PASS();
}

/* ---- Test: lambda in WHERE predicate compiles (was: error domain) ---- */
static test_result_t test_eval_select_lambda_where(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['p] (list [10 20 30 40 50]))) "
        "(select {from: t where: ((fn [x] (> x 25)) p)}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* rows where p > 25: 30, 40, 50 — 3 rows */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    ray_release(result);
    PASS();
}

/* ---- Test: lambda as an aggregation argument ---- */
static test_result_t test_eval_select_lambda_agg(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['s 'p] "
        "(list [A B A B] [10.0 20.0 30.0 40.0]))) "
        "(select {from: t by: s tot: (sum ((fn [x] (* x 2.0)) p))}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);
    int64_t tot_id = ray_sym_intern("tot", 3);
    ray_t* tot_col = ray_table_get_col(result, tot_id);
    TEST_ASSERT_NOT_NULL(tot_col);
    /* A: 2*(10+30) = 80; B: 2*(20+40) = 120 */
    double* td = (double*)ray_data(tot_col);
    TEST_ASSERT((td[0]) == (80.0), "double == failed");
    TEST_ASSERT((td[1]) == (120.0), "double == failed");
    ray_release(result);
    PASS();
}

/* ---- Test: `(let var val body)` binding in WHERE ---- */
static test_result_t test_eval_select_let_binding(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['p] (list [10 20 30 40 50]))) "
        "(select {from: t where: (let threshold 25 (> p threshold))}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    ray_release(result);
    PASS();
}

/* ---- Test: nested lambda shadowing — inner `x` shadows outer ----
 * Expression applied per row: ((fn [x] ((fn [x] (* x 10)) (+ x 1))) p)
 * Expands to: (* (+ p 1) 10) — so each row becomes (p+1)*10.
 * The outer `x` is `p`; the inner `x` is `(+ x 1)` = `(+ p 1)`.
 * Verify p=3 gives 40, p=5 gives 60. */
static test_result_t test_eval_select_lambda_nested_shadow(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['p] (list [3 5]))) "
        "(select {from: t m: ((fn [x] ((fn [x] (* x 10)) (+ x 1))) p)}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);
    int64_t m_id = ray_sym_intern("m", 1);
    ray_t* m_col = ray_table_get_col(result, m_id);
    TEST_ASSERT_NOT_NULL(m_col);
    int64_t* md = (int64_t*)ray_data(m_col);
    TEST_ASSERT_EQ_I(md[0], 40);
    TEST_ASSERT_EQ_I(md[1], 60);
    ray_release(result);
    PASS();
}

/* ---- Test: lambda arg reuse — actual compiled once, referenced twice ----
 * `((fn [x] (+ x x)) (* p 2))` per row.  Naive AST substitution
 * would recompute `(* p 2)` twice, but DAG-node sharing computes it
 * once and both inputs of `+` point at the same node.  Functionally:
 * each row becomes 4*p. */
static test_result_t test_eval_select_lambda_arg_reuse(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['p] (list [10 20 30]))) "
        "(select {from: t m: ((fn [x] (+ x x)) (* p 2))}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    int64_t m_id = ray_sym_intern("m", 1);
    ray_t* m_col = ray_table_get_col(result, m_id);
    TEST_ASSERT_NOT_NULL(m_col);
    int64_t* md = (int64_t*)ray_data(m_col);
    TEST_ASSERT_EQ_I(md[0], 40);
    TEST_ASSERT_EQ_I(md[1], 80);
    TEST_ASSERT_EQ_I(md[2], 120);
    ray_release(result);
    PASS();
}

/* ---- Test: arity mismatch returns an error ---- */
static test_result_t test_eval_select_lambda_arity_err(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['p] (list [1 2 3]))) "
        "(select {from: t where: ((fn [x y] (> x y)) p)}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));
    ray_release(result);
    PASS();
}

/* ---- Test: `== 0N` / `!= 0N` null-check idiom ----
 * Regression: compile_expr_dag routed typed null literals
 * (`-RAY_I64` with RAY_ATOM_IS_NULL set) through the fast ctors
 * `ray_const_i64` etc., which carry only the raw value — the
 * null flag stored in atom->nullmap[0] was dropped.  Downstream
 * fix_null_comparisons saw a non-null scalar rhs and didn't
 * apply null semantics, so `(== k 0N)` missed null rows and
 * `(!= k 0N)` leaked them through.  Now typed null atoms fall
 * through to `ray_const_atom` which preserves the null flag. */
static test_result_t test_eval_select_where_eq_null_literal(void) {
    /* (== k 0Nl) should match the two null rows. */
    ray_t* r1 = ray_eval_str(
        "(do (set t (table ['k] (list [1 0Nl 3 0Nl 5]))) "
        "(select {from: t where: (== k 0Nl)}))");
    TEST_ASSERT_NOT_NULL(r1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    TEST_ASSERT_EQ_I(ray_table_nrows(r1), 2);
    ray_release(r1);
    /* (!= k 0Nl) should return the 3 non-null rows. */
    ray_t* r2 = ray_eval_str(
        "(do (set t (table ['k] (list [1 0Nl 3 0Nl 5]))) "
        "(select {from: t where: (!= k 0Nl)}))");
    TEST_ASSERT_NOT_NULL(r2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    TEST_ASSERT_EQ_I(ray_table_nrows(r2), 3);
    ray_release(r2);
    /* Bare `0N` (no suffix) must parse as i64 null — same match as 0Nl. */
    ray_t* r3 = ray_eval_str(
        "(do (set t (table ['k] (list [1 0N 3 0N 5]))) "
        "(select {from: t where: (== k 0N)}))");
    TEST_ASSERT_NOT_NULL(r3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r3));
    TEST_ASSERT_EQ_I(ray_table_nrows(r3), 2);
    ray_release(r3);
    PASS();
}

/* Named-lambda call inside a SELECT — resolves through the global
 * env at compile time and inlines the body into the DAG, same as
 * the literal `((fn ...) ...)` case. */
static test_result_t test_eval_select_named_lambda(void) {
    ray_t* r = ray_eval_str(
        "(do (set t (table [p] (list [10 20 30]))) "
        "    (set add1 (fn [x] (+ x 1))) "
        "    (select {from: t m: (add1 p)}))");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 3);
    ray_t* m = ray_table_get_col(r, ray_sym_intern("m", 1));
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQ_I(m->type, RAY_I64);
    int64_t* md = (int64_t*)ray_data(m);
    TEST_ASSERT_EQ_I(md[0], 11);
    TEST_ASSERT_EQ_I(md[1], 21);
    TEST_ASSERT_EQ_I(md[2], 31);
    ray_release(r);
    PASS();
}

/* Global scalar binding used inside a WHERE clause — the
 * compile-time name resolver folds `threshold` to a const node. */
static test_result_t test_eval_select_global_scalar(void) {
    ray_t* r = ray_eval_str(
        "(do (set t (table [p] (list [10 20 30 40 50]))) "
        "    (set threshold 25) "
        "    (select {from: t where: (> p threshold)}))");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 3);
    ray_release(r);
    PASS();
}

/* Multi-index pivot with mixed-type composite key: (sym, i64) index,
 * sym pivot col, i64 value col.  Regression against an older bug where
 * composite-key grouping dropped or duplicated rows.  Verify each
 * cell is the correct sum for its (a, b, c) combination. */
static test_result_t test_eval_pivot_multi_index(void) {
    ray_t* r = ray_eval_str(
        "(do (set t (table [a b c v] (list "
        "  ['A 'A 'B 'B 'A 'B] "
        "  [10 20 10 20 20 10] "
        "  ['x 'y 'x 'y 'x 'y] "
        "  [100 200 300 400 500 600]))) "
        "(pivot t ['a 'b] 'c 'v sum))");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    /* Expect 4 distinct (a, b) rows: (A,10), (A,20), (B,10), (B,20). */
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 4);
    ray_t* a_col = ray_table_get_col(r, ray_sym_intern("a", 1));
    ray_t* b_col = ray_table_get_col(r, ray_sym_intern("b", 1));
    ray_t* x_col = ray_table_get_col(r, ray_sym_intern("x", 1));
    ray_t* y_col = ray_table_get_col(r, ray_sym_intern("y", 1));
    TEST_ASSERT_NOT_NULL(a_col);
    TEST_ASSERT_NOT_NULL(b_col);
    TEST_ASSERT_NOT_NULL(x_col);
    TEST_ASSERT_NOT_NULL(y_col);
    TEST_ASSERT_EQ_I(a_col->type, RAY_SYM);
    TEST_ASSERT_EQ_I(b_col->type, RAY_I64);
    TEST_ASSERT_EQ_I(x_col->type, RAY_I64);
    TEST_ASSERT_EQ_I(y_col->type, RAY_I64);
    /* Expected per-row: (A,10)→x=100,y=0; (A,20)→x=500,y=200;
     *                   (B,10)→x=300,y=600; (B,20)→x=0,y=400.
     * Build a lookup (a_sym, b) → (x, y) so we're independent of
     * group-output row order. */
    int64_t sym_A = ray_sym_intern("A", 1);
    int64_t sym_B = ray_sym_intern("B", 1);
    int64_t* bd = (int64_t*)ray_data(b_col);
    int64_t* xd = (int64_t*)ray_data(x_col);
    int64_t* yd = (int64_t*)ray_data(y_col);
    int seen_A10 = 0, seen_A20 = 0, seen_B10 = 0, seen_B20 = 0;
    for (int64_t i = 0; i < 4; i++) {
        int64_t a = (int64_t)ray_read_sym(ray_data(a_col), i, a_col->type, a_col->attrs);
        int64_t b = bd[i];
        if (a == sym_A && b == 10) { seen_A10 = 1; TEST_ASSERT_EQ_I(xd[i], 100); TEST_ASSERT_EQ_I(yd[i], 0);   }
        else if (a == sym_A && b == 20) { seen_A20 = 1; TEST_ASSERT_EQ_I(xd[i], 500); TEST_ASSERT_EQ_I(yd[i], 200); }
        else if (a == sym_B && b == 10) { seen_B10 = 1; TEST_ASSERT_EQ_I(xd[i], 300); TEST_ASSERT_EQ_I(yd[i], 600); }
        else if (a == sym_B && b == 20) { seen_B20 = 1; TEST_ASSERT_EQ_I(xd[i], 0);   TEST_ASSERT_EQ_I(yd[i], 400); }
        else TEST_ASSERT_TRUE(false);
    }
    TEST_ASSERT_TRUE(seen_A10 && seen_A20 && seen_B10 && seen_B20);
    ray_release(r);
    PASS();
}

static test_result_t test_eval_select_where_in_sym_vs_atom_mismatch(void) {
    /* Intern the probe sym so we know its numeric ID, then embed
     * that ID as a decimal literal in the source string so the i64
     * col value equals the literal sym's interned ID.  Under the
     * buggy code the atom branch wrote that sym ID into sv[0], it
     * collided with col[0], and `in` falsely returned 1 row.
     * Verified by temporarily reverting the fix: this test fails
     * with `1 == 0`. */
    int64_t target_id = ray_sym_intern("op_in_collision_probe", 21);
    char src[512];
    snprintf(src, sizeof(src),
        "(do (set tbug (table [k] (list [%lld]))) "
        "(select {from: tbug where: (in k 'op_in_collision_probe)}))",
        (long long)target_id);

    ray_t* r1 = ray_eval_str(src);
    TEST_ASSERT_NOT_NULL(r1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    TEST_ASSERT_EQ_I(r1->type, RAY_TABLE);
    /* With fix: 0 rows.  Without fix: 1 row (false collision). */
    TEST_ASSERT_EQ_I(ray_table_nrows(r1), 0);
    ray_release(r1);

    /* Same collision via not-in: should return ALL rows (1), not 0. */
    snprintf(src, sizeof(src),
        "(do (set tbug (table [k] (list [%lld]))) "
        "(select {from: tbug where: (not-in k 'op_in_collision_probe)}))",
        (long long)target_id);
    ray_t* r2 = ray_eval_str(src);
    TEST_ASSERT_NOT_NULL(r2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    TEST_ASSERT_EQ_I(ray_table_nrows(r2), 1);
    ray_release(r2);

    PASS();
}

/* ---- Test: OP_IN with an empty set + null column rows ----
 * Regression: exec_in had an early return for set_len == 0 that did
 * `memset(negate ? 1 : 0, col_len)` — flipping all rows, including
 * nulls, to true for `not-in`.  Null rows must still be excluded. */
static test_result_t test_eval_select_where_in_empty_set_nulls(void) {
    /* not-in with empty set: source has [1 0Nl 3 0Nl 5], filter
     * `not-in []` should return 1, 3, 5 — not the two nulls. */
    ray_t* r1 = ray_eval_str(
        "(do (set t (table ['k] (list [1 0Nl 3 0Nl 5]))) "
        "(select {from: t where: (not-in k [])}))");
    TEST_ASSERT_NOT_NULL(r1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    TEST_ASSERT_EQ_I(ray_table_nrows(r1), 3);
    ray_release(r1);

    /* in with empty set: no row passes. */
    ray_t* r2 = ray_eval_str(
        "(do (set t (table ['k] (list [1 0Nl 3 0Nl 5]))) "
        "(select {from: t where: (in k [])}))");
    TEST_ASSERT_NOT_NULL(r2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    TEST_ASSERT_EQ_I(ray_table_nrows(r2), 0);
    ray_release(r2);

    PASS();
}

/* ---- Test: OP_IN must not leak null rows through ----
 * Regression: exec_in originally treated the raw null-sentinel
 * value as a normal data value.  That meant `(not-in k [1])`
 * on a column containing 0Nl nulls returned the null rows as
 * "true" (because null-sentinel != 1), leaking them through.
 * Null rows must never pass either `in` or `not-in`. */
static test_result_t test_eval_select_where_in_nulls(void) {
    /* not-in with null rows: source has [1 0Nl 3 0Nl 5], filter
     * `not-in [1]` should return only 3 and 5 — not the two nulls. */
    ray_t* r1 = ray_eval_str(
        "(do (set t (table ['k] (list [1 0Nl 3 0Nl 5]))) "
        "(select {from: t where: (not-in k [1])}))");
    TEST_ASSERT_NOT_NULL(r1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    TEST_ASSERT_EQ_I(ray_table_nrows(r1), 2);
    ray_release(r1);

    /* in with null rows: source has [1 0Nl 3 0Nl 5], filter
     * `in [1 3]` should return 1 and 3 — not the nulls. */
    ray_t* r2 = ray_eval_str(
        "(do (set t (table ['k] (list [1 0Nl 3 0Nl 5]))) "
        "(select {from: t where: (in k [1 3])}))");
    TEST_ASSERT_NOT_NULL(r2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    TEST_ASSERT_EQ_I(ray_table_nrows(r2), 2);
    ray_release(r2);

    PASS();
}

/* ---- Test: `in` with mixed numeric types (F64 col vs I64 set) ----
 * Regression: exec_in originally compared bit patterns which made
 * `(in price_f64 [1 2 3])` match nothing.  Now we promote to double
 * when either side is a float type and compare numerically. */
static test_result_t test_eval_select_where_in_mixed_numeric(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['p] (list [1.0 2.0 3.0 4.0]))) "
        "(select {from: t where: (in p [1 2])}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);
    ray_release(result);
    PASS();
}

/* ---- Test: WHERE with `in` and a literal i64 vector ---- */
static test_result_t test_eval_select_where_in_i64(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['k 'p] "
        "(list [1 2 3 4 5] [10 20 30 40 50]))) "
        "(select {from: t where: (in k [1 3 5])}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    ray_release(result);
    PASS();
}

/* ---- Test: pre-existing WHERE+by bug — WHERE was silently dropped
 * when `by:` was present.  Now fixed by pre-materializing the
 * filter before the GROUP op's inputs are built. */
static test_result_t test_eval_select_by_where_filters(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['s 'p] "
        "(list [A B A B A B] [10.0 20.0 30.0 40.0 50.0 60.0]))) "
        "(select {from: t by: s where: (> p 25.0) tot: (sum p)}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);
    /* sum(A where p > 25) = 30+50 = 80; sum(B where p > 25) = 40+60 = 100 */
    int64_t tot_id = ray_sym_intern("tot", 3);
    ray_t* tot_col = ray_table_get_col(result, tot_id);
    TEST_ASSERT_NOT_NULL(tot_col);
    double* td = (double*)ray_data(tot_col);
    TEST_ASSERT((td[0]) == (80.0), "double == failed");
    TEST_ASSERT((td[1]) == (100.0), "double == failed");
    ray_release(result);
    PASS();
}

/* ---- Test: WHERE with `in` + group-by end-to-end ---- */
static test_result_t test_eval_select_by_where_in(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['s 'p] "
        "(list [A B C A B C A B C] [1 2 3 4 5 6 7 8 9]))) "
        "(select {from: t by: s where: (in s [A C]) tot: (sum p)}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* B should be filtered out, leaving only A and C. */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);
    ray_release(result);
    PASS();
}

/* ---- Test: `if` conditional projection ---- */
static test_result_t test_eval_select_if(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['p] (list [10 20 30 40]))) "
        "(select {from: t m: (if (> p 25) 1 0)}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 4);
    int64_t m_id = ray_sym_intern("m", 1);
    ray_t* m_col = ray_table_get_col(result, m_id);
    TEST_ASSERT_NOT_NULL(m_col);
    int64_t* md = (int64_t*)ray_data(m_col);
    TEST_ASSERT_EQ_I(md[0], 0);
    TEST_ASSERT_EQ_I(md[1], 0);
    TEST_ASSERT_EQ_I(md[2], 1);
    TEST_ASSERT_EQ_I(md[3], 1);
    ray_release(result);
    PASS();
}

/* ---- Test: equality against sym literal atom ---- */
static test_result_t test_eval_select_where_sym_atom(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['s 'p] "
        "(list [A B A B] [10 20 30 40]))) "
        "(select {from: t where: (== s 'A)}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);
    ray_release(result);
    PASS();
}

/* ---- Test: `as` type cast ---- */
static test_result_t test_eval_select_cast(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['p] (list [10.5 20.5 30.5]))) "
        "(select {from: t m: (as 'I64 p)}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    int64_t m_id = ray_sym_intern("m", 1);
    ray_t* m_col = ray_table_get_col(result, m_id);
    TEST_ASSERT_NOT_NULL(m_col);
    TEST_ASSERT_EQ_I(m_col->type, RAY_I64);
    ray_release(result);
    PASS();
}

/* ---- Test: `round` on f64 column ---- */
static test_result_t test_eval_select_round(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['p] (list [1.4 2.6 3.5]))) "
        "(select {from: t m: (round p)}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    int64_t m_id = ray_sym_intern("m", 1);
    ray_t* m_col = ray_table_get_col(result, m_id);
    TEST_ASSERT_NOT_NULL(m_col);
    double* md = (double*)ray_data(m_col);
    TEST_ASSERT((md[0]) == (1.0), "double == failed");
    TEST_ASSERT((md[1]) == (3.0), "double == failed");
    TEST_ASSERT((md[2]) >= (3.0), "double >= failed");  /* round half: either 3 or 4 depending on mode */
    ray_release(result);
    PASS();
}

/* ---- Test: select cols (projection) ---- */
static test_result_t test_eval_select_cols(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['name 'salary 'dept] "
        "(list [1 2 3] [50000 60000 70000] [10 20 10]))) "
        "(select {name: name salary: salary from: t}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_ncols(result), 2);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    ray_release(result);
    PASS();
}

/* ---- Test: select groupby ---- */
static test_result_t test_eval_select_groupby(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['dept 'salary] "
        "(list [1 2 1 2] [50000 60000 70000 80000]))) "
        "(select {avg_sal: (avg salary) from: t by: dept}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);
    ray_release(result);
    PASS();
}

/* ---- Test: select xbar (time bucket) ---- */
static test_result_t test_eval_select_xbar(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['ts 'val] "
        "(list [100 250 300 450 500] [1 2 3 4 5]))) "
        "(select {total: (sum val) from: t by: (xbar ts 200)}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* Buckets: 0(100), 200(250,300), 400(450,500) → 3 groups */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    ray_release(result);
    PASS();
}

/* ---- Test: select asc ---- */
static test_result_t test_eval_select_asc(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['a 'b] (list [3 1 2] [30 10 20]))) "
        "(select {from: t asc: 'a}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    int64_t a_id = ray_sym_intern("a", 1);
    ray_t* a_col = ray_table_get_col(result, a_id);
    int64_t* a_data = (int64_t*)ray_data(a_col);
    TEST_ASSERT_EQ_I(a_data[0], 1);
    TEST_ASSERT_EQ_I(a_data[1], 2);
    TEST_ASSERT_EQ_I(a_data[2], 3);
    ray_release(result);
    PASS();
}

/* ---- Test: select desc ---- */
static test_result_t test_eval_select_desc(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['a 'b] (list [3 1 2] [30 10 20]))) "
        "(select {from: t desc: 'a}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    int64_t a_id = ray_sym_intern("a", 1);
    ray_t* a_col = ray_table_get_col(result, a_id);
    int64_t* a_data = (int64_t*)ray_data(a_col);
    TEST_ASSERT_EQ_I(a_data[0], 3);
    TEST_ASSERT_EQ_I(a_data[1], 2);
    TEST_ASSERT_EQ_I(a_data[2], 1);
    ray_release(result);
    PASS();
}

/* ---- Test: select asc + desc mixed ---- */
static test_result_t test_eval_select_asc_desc(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['grp 'val] (list [1 1 2 2] [30 10 20 40]))) "
        "(select {from: t asc: 'grp desc: 'val}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    int64_t g_id = ray_sym_intern("grp", 3);
    int64_t v_id = ray_sym_intern("val", 3);
    ray_t* g_col = ray_table_get_col(result, g_id);
    ray_t* v_col = ray_table_get_col(result, v_id);
    int64_t* gd = (int64_t*)ray_data(g_col);
    int64_t* vd = (int64_t*)ray_data(v_col);
    /* grp=1: val desc → 30,10; grp=2: val desc → 40,20 */
    TEST_ASSERT_EQ_I(gd[0], 1); TEST_ASSERT_EQ_I(vd[0], 30);
    TEST_ASSERT_EQ_I(gd[1], 1); TEST_ASSERT_EQ_I(vd[1], 10);
    TEST_ASSERT_EQ_I(gd[2], 2); TEST_ASSERT_EQ_I(vd[2], 40);
    TEST_ASSERT_EQ_I(gd[3], 2); TEST_ASSERT_EQ_I(vd[3], 20);
    ray_release(result);
    PASS();
}

/* ---- Test: select take positive ---- */
static test_result_t test_eval_select_take(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['a] (list [10 20 30 40 50]))) "
        "(select {from: t take: 3}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    int64_t a_id = ray_sym_intern("a", 1);
    ray_t* a_col = ray_table_get_col(result, a_id);
    int64_t* a_data = (int64_t*)ray_data(a_col);
    TEST_ASSERT_EQ_I(a_data[0], 10);
    TEST_ASSERT_EQ_I(a_data[1], 20);
    TEST_ASSERT_EQ_I(a_data[2], 30);
    ray_release(result);
    PASS();
}

/* ---- Test: select take negative ---- */
static test_result_t test_eval_select_take_neg(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['a] (list [10 20 30 40 50]))) "
        "(select {from: t take: -2}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);
    int64_t a_id = ray_sym_intern("a", 1);
    ray_t* a_col = ray_table_get_col(result, a_id);
    int64_t* a_data = (int64_t*)ray_data(a_col);
    TEST_ASSERT_EQ_I(a_data[0], 40);
    TEST_ASSERT_EQ_I(a_data[1], 50);
    ray_release(result);
    PASS();
}

/* ---- Test: select take range [start count] ---- */
static test_result_t test_eval_select_take_range(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['a] (list [10 20 30 40 50]))) "
        "(select {from: t take: [1 2]}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);
    int64_t a_id = ray_sym_intern("a", 1);
    ray_t* a_col = ray_table_get_col(result, a_id);
    int64_t* a_data = (int64_t*)ray_data(a_col);
    TEST_ASSERT_EQ_I(a_data[0], 20);
    TEST_ASSERT_EQ_I(a_data[1], 30);
    ray_release(result);
    PASS();
}

/* ---- Test: select where + desc + take ---- */
static test_result_t test_eval_select_combined(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['a 'b] (list [3 1 2 5 4] [10 20 30 40 50]))) "
        "(select {from: t where: (> a 2) desc: 'a take: 2}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);
    int64_t a_id = ray_sym_intern("a", 1);
    ray_t* a_col = ray_table_get_col(result, a_id);
    int64_t* a_data = (int64_t*)ray_data(a_col);
    TEST_ASSERT_EQ_I(a_data[0], 5);
    TEST_ASSERT_EQ_I(a_data[1], 4);
    ray_release(result);
    PASS();
}

/* ---- Test: select asc multi-column vector ---- */
static test_result_t test_eval_select_asc_multi(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['a 'b] (list [2 1 1 2] [20 10 30 10]))) "
        "(select {from: t asc: ['a 'b]}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    int64_t a_id = ray_sym_intern("a", 1);
    int64_t b_id = ray_sym_intern("b", 1);
    ray_t* a_col = ray_table_get_col(result, a_id);
    ray_t* b_col = ray_table_get_col(result, b_id);
    int64_t* ad = (int64_t*)ray_data(a_col);
    int64_t* bd = (int64_t*)ray_data(b_col);
    /* a asc, b asc: (1,10), (1,30), (2,10), (2,20) */
    TEST_ASSERT_EQ_I(ad[0], 1); TEST_ASSERT_EQ_I(bd[0], 10);
    TEST_ASSERT_EQ_I(ad[1], 1); TEST_ASSERT_EQ_I(bd[1], 30);
    TEST_ASSERT_EQ_I(ad[2], 2); TEST_ASSERT_EQ_I(bd[2], 10);
    TEST_ASSERT_EQ_I(ad[3], 2); TEST_ASSERT_EQ_I(bd[3], 20);
    ray_release(result);
    PASS();
}

/* ---- Test: select groupby + desc + take ---- */
static test_result_t test_eval_select_groupby_sort(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['dept 'salary] "
        "(list [1 2 1 2 1] [50000 60000 70000 80000 30000]))) "
        "(select {from: t by: dept total: (sum salary) desc: 'total take: 1}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 1);
    /* dept=1: sum=150000, dept=2: sum=140000. desc by total → dept 1 first */
    int64_t dept_id = ray_sym_intern("dept", 4);
    ray_t* dept_col = ray_table_get_col(result, dept_id);
    TEST_ASSERT_NOT_NULL(dept_col);
    int64_t* dd = (int64_t*)ray_data(dept_col);
    TEST_ASSERT_EQ_I(dd[0], 1);
    ray_release(result);
    PASS();
}

/* ---- Test: select groupby + non-aggregation expression (I64 key) ----
 * Regression: the post-DAG scatter path used ray_read_sym (SYM-only)
 * to read plain i64 key elements, which read 1 byte instead of 8
 * and produced wrong per-group lists. */
static test_result_t test_eval_select_by_nonagg_i64_key(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['k 'p] "
        "(list [100 200 100 200 100 200] [10.0 20.0 30.0 40.0 50.0 60.0]))) "
        "(select {from: t by: k m: (+ p p)}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);

    int64_t k_id = ray_sym_intern("k", 1);
    int64_t m_id = ray_sym_intern("m", 1);
    ray_t* k_col = ray_table_get_col(result, k_id);
    ray_t* m_col = ray_table_get_col(result, m_id);
    TEST_ASSERT_NOT_NULL(k_col);
    TEST_ASSERT_NOT_NULL(m_col);
    TEST_ASSERT_EQ_I(m_col->type, RAY_LIST);
    TEST_ASSERT_EQ_I(k_col->type, RAY_I64);
    int64_t* kd = (int64_t*)ray_data(k_col);
    TEST_ASSERT_EQ_I(kd[0], 100);
    TEST_ASSERT_EQ_I(kd[1], 200);

    ray_t** mi = (ray_t**)ray_data(m_col);
    /* group 100 → rows [0 2 4] → p=[10 30 50] → (+ p p) = [20 60 100] */
    TEST_ASSERT_EQ_I(mi[0]->len, 3);
    double* d0 = (double*)ray_data(mi[0]);
    TEST_ASSERT((d0[0]) == (20.0), "double == failed");
    TEST_ASSERT((d0[1]) == (60.0), "double == failed");
    TEST_ASSERT((d0[2]) == (100.0), "double == failed");
    /* group 200 → rows [1 3 5] → p=[20 40 60] → (+ p p) = [40 80 120] */
    TEST_ASSERT_EQ_I(mi[1]->len, 3);
    double* d1 = (double*)ray_data(mi[1]);
    TEST_ASSERT((d1[0]) == (40.0), "double == failed");
    TEST_ASSERT((d1[1]) == (80.0), "double == failed");
    TEST_ASSERT((d1[2]) == (120.0), "double == failed");
    ray_release(result);
    PASS();
}

/* ---- Test: select groupby + non-agg with U8 key ----
 * Regression: KEY_READ macro's default case returned 0, so U8 keys
 * collapsed every row into a single (wrong) group. */
static test_result_t test_eval_select_by_nonagg_u8_key(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['k 'p] "
        "(list (as 'U8 [100 200 100 200 100 200]) "
        "      [10.0 20.0 30.0 40.0 50.0 60.0]))) "
        "(select {from: t by: k m: (+ p p)}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);

    int64_t m_id = ray_sym_intern("m", 1);
    ray_t* m_col = ray_table_get_col(result, m_id);
    TEST_ASSERT_NOT_NULL(m_col);
    TEST_ASSERT_EQ_I(m_col->type, RAY_LIST);

    ray_t** mi = (ray_t**)ray_data(m_col);
    TEST_ASSERT_EQ_I(mi[0]->len, 3);
    TEST_ASSERT_EQ_I(mi[1]->len, 3);
    double* d0 = (double*)ray_data(mi[0]);
    TEST_ASSERT((d0[0]) == (20.0), "double == failed");
    TEST_ASSERT((d0[1]) == (60.0), "double == failed");
    TEST_ASSERT((d0[2]) == (100.0), "double == failed");
    double* d1 = (double*)ray_data(mi[1]);
    TEST_ASSERT((d1[0]) == (40.0), "double == failed");
    TEST_ASSERT((d1[1]) == (80.0), "double == failed");
    TEST_ASSERT((d1[2]) == (120.0), "double == failed");
    ray_release(result);
    PASS();
}

/* ---- Test: empty groupby non-agg result keeps full schema ----
 * Regression: when WHERE filters all rows out, the scatter block
 * skipped adding the non-agg LIST column, producing a table with
 * only the key column. */
static test_result_t test_eval_select_by_nonagg_empty(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['k 'p] (list [100 200] [10.0 20.0]))) "
        "(select {from: t where: (> p 1000.0) by: k m: (+ p p)}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 0);
    /* Must still have both key and non-agg columns */
    TEST_ASSERT_EQ_I(ray_table_ncols(result), 2);
    int64_t m_id = ray_sym_intern("m", 1);
    ray_t* m_col = ray_table_get_col(result, m_id);
    TEST_ASSERT_NOT_NULL(m_col);
    TEST_ASSERT_EQ_I(m_col->type, RAY_LIST);
    ray_release(result);
    PASS();
}

/* ---- Test: mixed agg + non-agg column naming ----
 * Regression: when a non-agg column was declared before an agg in
 * the dict, the rename step swapped names because the DAG result
 * layout is [keys, aggs..., nonaggs...] regardless of dict order. */
static test_result_t test_eval_select_by_mixed_naming(void) {
    /* Non-agg listed FIRST in the dict */
    ray_t* result = ray_eval_str(
        "(do (set t (table ['s 'p] "
        "(list [A B A B A B] [10.0 20.0 30.0 40.0 50.0 60.0]))) "
        "(select {from: t by: s m: (+ p p) tot: (sum p)}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);

    int64_t m_id   = ray_sym_intern("m", 1);
    int64_t tot_id = ray_sym_intern("tot", 3);
    ray_t* m_col   = ray_table_get_col(result, m_id);
    ray_t* tot_col = ray_table_get_col(result, tot_id);
    TEST_ASSERT_NOT_NULL(m_col);
    TEST_ASSERT_NOT_NULL(tot_col);
    /* m must be the LIST (non-agg), tot must be the F64 sum */
    TEST_ASSERT_EQ_I(m_col->type, RAY_LIST);
    TEST_ASSERT_EQ_I(tot_col->type, RAY_F64);
    double* td = (double*)ray_data(tot_col);
    TEST_ASSERT((td[0]) == (90.0), "double == failed");
    TEST_ASSERT((td[1]) == (120.0), "double == failed");
    ray_release(result);
    PASS();
}

/* ---- Test: sort + take on grouped non-agg output ----
 * Regression: apply_sort_take used the DAG ray_head op for atom
 * take, which errors on tables containing LIST columns (the scatter
 * output).  And scatter was ordered after apply_sort_take, so sort
 * by non-agg output columns fell through with "nyi".  Both fixed by
 * (a) running scatter before apply_sort_take, (b) using ray_take_fn
 * instead of the DAG head/tail op. */
static test_result_t test_eval_select_by_nonagg_sort_take(void) {
    /* take: 1 with non-agg LIST column */
    ray_t* r1 = ray_eval_str(
        "(do (set t (table ['s 'p] "
        "(list [A B A B A B] [10.0 20.0 30.0 40.0 50.0 60.0]))) "
        "(select {from: t by: s m: (+ p p) take: 1}))");
    TEST_ASSERT_NOT_NULL(r1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    TEST_ASSERT_EQ_I(r1->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(r1), 1);
    int64_t m_id = ray_sym_intern("m", 1);
    ray_t* m1 = ray_table_get_col(r1, m_id);
    TEST_ASSERT_NOT_NULL(m1);
    TEST_ASSERT_EQ_I(m1->type, RAY_LIST);
    ray_release(r1);

    /* desc by agg column still reorders groups correctly with a
     * non-agg LIST column present. */
    ray_t* r2 = ray_eval_str(
        "(do (set t (table ['s 'p] "
        "(list [A B A B A B] [10.0 20.0 30.0 40.0 50.0 60.0]))) "
        "(select {from: t by: s tot: (sum p) m: (+ p p) desc: 'tot}))");
    TEST_ASSERT_NOT_NULL(r2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    TEST_ASSERT_EQ_I(r2->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(r2), 2);
    int64_t s_id = ray_sym_intern("s", 1);
    ray_t* s_col = ray_table_get_col(r2, s_id);
    int64_t* sd = (int64_t*)ray_data(s_col);
    /* sum(A)=90, sum(B)=120 → desc means B first, A second */
    int64_t sym_A = ray_sym_intern("A", 1);
    int64_t sym_B = ray_sym_intern("B", 1);
    TEST_ASSERT_EQ_I(sd[0], sym_B);
    TEST_ASSERT_EQ_I(sd[1], sym_A);
    ray_release(r2);

    /* desc by key column works and drags the non-agg LIST column
     * along consistently. */
    ray_t* r3 = ray_eval_str(
        "(do (set t (table ['s 'p] "
        "(list [A B C A B C] [10.0 20.0 30.0 40.0 50.0 60.0]))) "
        "(select {from: t by: s m: (+ p p) desc: 's}))");
    TEST_ASSERT_NOT_NULL(r3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r3));
    TEST_ASSERT_EQ_I(ray_table_nrows(r3), 3);
    ray_release(r3);

    PASS();
}

/* ---- Test: grouped take clamps, not wraps ----
 * Regression: switching to ray_take_fn for group-by take briefly
 * brought wrap/pad semantics — `take: 5` with 2 groups
 * produced 5 rows (A,B,A,B,A).  Group-by must clamp to min(n, nrows). */
static test_result_t test_eval_select_by_take_clamps(void) {
    /* agg-only: 2 groups, take: 5 → should clamp to 2 */
    ray_t* r1 = ray_eval_str(
        "(do (set t (table ['s 'p] "
        "(list [A B A B] [10.0 20.0 30.0 40.0]))) "
        "(select {from: t by: s tot: (sum p) take: 5}))");
    TEST_ASSERT_NOT_NULL(r1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    TEST_ASSERT_EQ_I(r1->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(r1), 2);
    ray_release(r1);

    /* with non-agg LIST column: same clamp behavior */
    ray_t* r2 = ray_eval_str(
        "(do (set t (table ['s 'p] "
        "(list [A B A B] [10.0 20.0 30.0 40.0]))) "
        "(select {from: t by: s m: (+ p p) take: 5}))");
    TEST_ASSERT_NOT_NULL(r2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    TEST_ASSERT_EQ_I(ray_table_nrows(r2), 2);
    ray_release(r2);

    /* take: -3 (tail) with 2 groups → clamp to 2 */
    ray_t* r3 = ray_eval_str(
        "(do (set t (table ['s 'p] "
        "(list [A B A B] [10.0 20.0 30.0 40.0]))) "
        "(select {from: t by: s tot: (sum p) take: -3}))");
    TEST_ASSERT_NOT_NULL(r3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r3));
    TEST_ASSERT_EQ_I(ray_table_nrows(r3), 2);
    ray_release(r3);

    PASS();
}

/* ---- Test: agg sub-calls inside non-agg expressions broadcast ----
 * Regression: the classifier that decides "row-aligned required vs
 * broadcast OK" looked at column refs but didn't account for
 * aggregation subexpressions that collapse column refs into scalars.
 * `(+ 1 (sum p))` references p but (sum p) reduces it to a scalar,
 * so the overall result is 1-wide and must broadcast. */
static test_result_t test_eval_select_by_nonagg_with_agg_subexpr(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['s 'p] "
        "(list [A B A B A B] [10.0 20.0 30.0 40.0 50.0 60.0]))) "
        "(select {from: t by: s m: (+ 1 (sum p))}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);
    int64_t m_id = ray_sym_intern("m", 1);
    ray_t* m_col = ray_table_get_col(result, m_id);
    TEST_ASSERT_NOT_NULL(m_col);
    TEST_ASSERT_EQ_I(m_col->type, RAY_LIST);
    ray_t** mi = (ray_t**)ray_data(m_col);
    /* Full-table sum of p is 210; (+ 1 210) = 211.  Broadcast into
     * every group cell — NOT gathered or errored. */
    TEST_ASSERT((mi[0]->f64) == (211.0), "double == failed");
    TEST_ASSERT((mi[1]->f64) == (211.0), "double == failed");
    ray_release(result);
    PASS();
}

/* ---- Test: non-agg classification — column refs vs constants ----
 * Regression: the scatter needs to distinguish between expressions
 * that reference table columns (row-aligned, gather per group) and
 * pure constants (broadcast as-is).  Broadcasting a row-derived
 * result whose length doesn't match nrows would hide bugs. */
static test_result_t test_eval_select_by_nonagg_colref_vs_const(void) {
    /* Case 1: column reference + row-aligned result → gather */
    ray_t* r1 = ray_eval_str(
        "(do (set t (table ['s 'p] "
        "(list [A B A B A B] [10.0 20.0 30.0 40.0 50.0 60.0]))) "
        "(select {from: t by: s m: (+ p p)}))");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    int64_t m_id = ray_sym_intern("m", 1);
    ray_t* m1 = ray_table_get_col(r1, m_id);
    TEST_ASSERT_EQ_I(m1->type, RAY_LIST);
    ray_t** m1i = (ray_t**)ray_data(m1);
    TEST_ASSERT_EQ_I(m1i[0]->len, 3);  /* A: 3 rows */
    TEST_ASSERT_EQ_I(m1i[1]->len, 3);  /* B: 3 rows */
    double* ga = (double*)ray_data(m1i[0]);
    TEST_ASSERT((ga[0]) == (20.0), "double == failed");
    ray_release(r1);

    /* Case 2: constant (list 99 88) → broadcast */
    ray_t* r2 = ray_eval_str(
        "(do (set t (table ['s 'p] "
        "(list [A B A B A B] [10.0 20.0 30.0 40.0 50.0 60.0]))) "
        "(select {from: t by: s m: (list 99 88)}))");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    ray_t* m2 = ray_table_get_col(r2, m_id);
    TEST_ASSERT_EQ_I(m2->type, RAY_LIST);
    ray_t** m2i = (ray_t**)ray_data(m2);
    /* Each cell holds the full 2-element broadcast */
    TEST_ASSERT_EQ_I(m2i[0]->len, 2);
    TEST_ASSERT_EQ_I(m2i[1]->len, 2);
    ray_release(r2);

    /* Case 3: chained column-ref (passthrough LIST col m) → gather */
    ray_t* r3 = ray_eval_str(
        "(do (set t (table ['s 'p] "
        "(list [A B A B A B] [10.0 20.0 30.0 40.0 50.0 60.0]))) "
        "(set r (select {from: t by: s m: (+ p p)})) "
        "(select {from: r by: s m2: m}))");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r3));
    int64_t m2_id = ray_sym_intern("m2", 2);
    ray_t* col3 = ray_table_get_col(r3, m2_id);
    TEST_ASSERT_EQ_I(col3->type, RAY_LIST);
    ray_t** c3i = (ray_t**)ray_data(col3);
    /* Each cell is a 1-element LIST containing that group's own
     * inner vec — NOT the full 2-element LIST duplicated. */
    TEST_ASSERT_EQ_I(c3i[0]->len, 1);
    TEST_ASSERT_EQ_I(c3i[1]->len, 1);
    ray_t* ia = ((ray_t**)ray_data(c3i[0]))[0];
    ray_t* ib = ((ray_t**)ray_data(c3i[1]))[0];
    TEST_ASSERT_EQ_I(ia->len, 3);  /* group A's inner vec */
    TEST_ASSERT_EQ_I(ib->len, 3);  /* group B's inner vec */
    /* And the two inner vecs must differ */
    double* a = (double*)ray_data(ia);
    double* b = (double*)ray_data(ib);
    TEST_ASSERT((a[0]) == (20.0), "double == failed");
    TEST_ASSERT((b[0]) == (40.0), "double == failed");
    ray_release(r3);
    PASS();
}

/* ---- Test: non-agg literal/short vectors broadcast ----
 * Regression: the over-eager fix that routed all RAY_LIST results
 * through gather_by_idx also swept up literal lists like `[1 2]`
 * whose length doesn't match nrows, reading out of bounds.  Correct
 * semantics: anything whose length doesn't match the input row
 * count broadcasts as-is into every group cell. */
static test_result_t test_eval_select_by_nonagg_broadcast(void) {
    /* DAG path: sym key, literal vector shorter than nrows */
    ray_t* r1 = ray_eval_str(
        "(do (set t (table ['s 'p] "
        "(list [A B A B A B] [10.0 20.0 30.0 40.0 50.0 60.0]))) "
        "(select {from: t by: s m: [1 2]}))");
    TEST_ASSERT_NOT_NULL(r1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    TEST_ASSERT_EQ_I(r1->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(r1), 2);
    int64_t m_id = ray_sym_intern("m", 1);
    ray_t* m1 = ray_table_get_col(r1, m_id);
    TEST_ASSERT_EQ_I(m1->type, RAY_LIST);
    ray_t** m1i = (ray_t**)ray_data(m1);
    /* Each cell holds the whole 2-element literal */
    TEST_ASSERT_EQ_I(m1i[0]->len, 2);
    TEST_ASSERT_EQ_I(m1i[1]->len, 2);
    int64_t* a = (int64_t*)ray_data(m1i[0]);
    int64_t* b = (int64_t*)ray_data(m1i[1]);
    TEST_ASSERT_EQ_I(a[0], 1); TEST_ASSERT_EQ_I(a[1], 2);
    TEST_ASSERT_EQ_I(b[0], 1); TEST_ASSERT_EQ_I(b[1], 2);
    ray_release(r1);

    /* eval_group path: STR key forces eval-level, literal list */
    ray_t* r2 = ray_eval_str(
        "(do (set t (table ['s 'p] "
        "(list (as 'STR [\"A\" \"B\" \"A\" \"B\"]) [10.0 20.0 30.0 40.0]))) "
        "(select {from: t by: s m: [7 8 9]}))");
    TEST_ASSERT_NOT_NULL(r2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    TEST_ASSERT_EQ_I(ray_table_nrows(r2), 2);
    ray_t* m2 = ray_table_get_col(r2, m_id);
    TEST_ASSERT_EQ_I(m2->type, RAY_LIST);
    ray_t** m2i = (ray_t**)ray_data(m2);
    TEST_ASSERT_EQ_I(m2i[0]->len, 3);
    TEST_ASSERT_EQ_I(m2i[1]->len, 3);
    int64_t* c = (int64_t*)ray_data(m2i[0]);
    TEST_ASSERT_EQ_I(c[0], 7);
    TEST_ASSERT_EQ_I(c[1], 8);
    TEST_ASSERT_EQ_I(c[2], 9);
    ray_release(r2);

    PASS();
}

/* ---- Test: eval_group path (STR key) gathers LIST non-agg ----
 * Regression: the eval_group non-agg branch had its own
 * `if (ray_is_vec(full_val))` check that excluded RAY_LIST and
 * duplicated the whole list into every group.  This only surfaced
 * when the group key column forced use_eval_group (STR/LIST/GUID),
 * so the earlier fix to the DAG scatter missed it. */
static test_result_t test_eval_select_by_str_nonagg_list_col(void) {
    ray_t* result = ray_eval_str(
        "(do "
        " (set t (table ['s 'p] "
        "   (list (as 'STR [\"A\" \"B\" \"C\" \"A\" \"B\" \"C\"]) "
        "         [10.0 20.0 30.0 40.0 50.0 60.0]))) "
        " (set r (select {from: t by: s m: (+ p p)})) "
        " (select {from: r by: [s] m2: m}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    int64_t m2_id = ray_sym_intern("m2", 2);
    ray_t* m2 = ray_table_get_col(result, m2_id);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQ_I(m2->type, RAY_LIST);
    ray_t** m2i = (ray_t**)ray_data(m2);
    /* Each cell is a 1-element LIST holding that group's own inner
     * vec — the three cells must have different inner values. */
    TEST_ASSERT_EQ_I(m2i[0]->len, 1);
    TEST_ASSERT_EQ_I(m2i[1]->len, 1);
    TEST_ASSERT_EQ_I(m2i[2]->len, 1);
    ray_t* ia = ((ray_t**)ray_data(m2i[0]))[0];
    ray_t* ib = ((ray_t**)ray_data(m2i[1]))[0];
    ray_t* ic = ((ray_t**)ray_data(m2i[2]))[0];
    double* a = (double*)ray_data(ia);
    double* b = (double*)ray_data(ib);
    double* c = (double*)ray_data(ic);
    /* A: (+ p p) on rows 0,3 → [20, 80] */
    TEST_ASSERT_EQ_I(ia->len, 2);
    TEST_ASSERT((a[0]) == (20.0), "double == failed");
    TEST_ASSERT((a[1]) == (80.0), "double == failed");
    /* B: rows 1,4 → [40, 100] */
    TEST_ASSERT_EQ_I(ib->len, 2);
    TEST_ASSERT((b[0]) == (40.0), "double == failed");
    TEST_ASSERT((b[1]) == (100.0), "double == failed");
    /* C: rows 2,5 → [60, 120] */
    TEST_ASSERT_EQ_I(ic->len, 2);
    TEST_ASSERT((c[0]) == (60.0), "double == failed");
    TEST_ASSERT((c[1]) == (120.0), "double == failed");
    ray_release(result);
    PASS();
}

/* ---- Test: non-agg scatter gathers LIST columns per group ----
 * Regression: the scatter's `ray_is_vec` check excluded RAY_LIST
 * (type 0), so LIST-valued non-agg results were retained and
 * duplicated into every group instead of gathered. */
static test_result_t test_eval_select_by_nonagg_list_col(void) {
    ray_t* result = ray_eval_str(
        "(do "
        " (set t (table ['s 'p] "
        "   (list [A B A B A B] [10.0 20.0 30.0 40.0 50.0 60.0]))) "
        " (set r (select {from: t by: s m: (+ p p)})) "
        " (select {from: r by: s m2: m}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);
    int64_t m2_id = ray_sym_intern("m2", 2);
    ray_t* m2 = ray_table_get_col(result, m2_id);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQ_I(m2->type, RAY_LIST);
    ray_t** m2i = (ray_t**)ray_data(m2);
    /* Each cell should be a 1-element LIST holding that group's
     * original list, NOT the full LIST column duplicated. */
    TEST_ASSERT_EQ_I(m2i[0]->type, RAY_LIST);
    TEST_ASSERT_EQ_I(m2i[0]->len, 1);
    TEST_ASSERT_EQ_I(m2i[1]->type, RAY_LIST);
    TEST_ASSERT_EQ_I(m2i[1]->len, 1);
    /* The inner vectors must differ between groups */
    ray_t* inner_a = ((ray_t**)ray_data(m2i[0]))[0];
    ray_t* inner_b = ((ray_t**)ray_data(m2i[1]))[0];
    TEST_ASSERT_EQ_I(inner_a->len, 3);
    TEST_ASSERT_EQ_I(inner_b->len, 3);
    double* a = (double*)ray_data(inner_a);
    double* b = (double*)ray_data(inner_b);
    TEST_ASSERT((a[0]) == (20.0), "double == failed");  /* group A: (+ p p) = [20,60,100] */
    TEST_ASSERT((a[2]) == (100.0), "double == failed");
    TEST_ASSERT((b[0]) == (40.0), "double == failed");  /* group B: (+ p p) = [40,80,120] */
    TEST_ASSERT((b[2]) == (120.0), "double == failed");
    ray_release(result);
    PASS();
}

/* ---- Test: `by: [b]` single-element vector with BOOL key ----
 * Regression: the BOOL first-occurrence reorder only recognized
 * scalar `by_expr->type == -RAY_SYM`.  For `by: [b]` the reorder
 * was skipped and the result came out in radix order (false,true)
 * instead of first-occurrence order. */
static test_result_t test_eval_select_by_vec_bool_order(void) {
    /* First value is true → expect true row first in result */
    ray_t* result = ray_eval_str(
        "(do (set t (table ['b 'p] "
        "(list [true false true false true] [10.0 20.0 30.0 40.0 50.0]))) "
        "(select {from: t by: [b] tot: (sum p)}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);
    int64_t b_id = ray_sym_intern("b", 1);
    ray_t* b_col = ray_table_get_col(result, b_id);
    TEST_ASSERT_NOT_NULL(b_col);
    TEST_ASSERT_EQ_I(b_col->type, RAY_BOOL);
    bool* bd = (bool*)ray_data(b_col);
    TEST_ASSERT_TRUE(bd[0]);   /* true first (first-occurrence) */
    TEST_ASSERT_FALSE(bd[1]);
    ray_release(result);
    PASS();
}

/* ---- Test: `by: [s]` single-element vector with STR key ----
 * Regression: the use_eval_group check only looked at scalar
 * -RAY_SYM by_expr, so `by: [s]` slipped through to the DAG path;
 * the eval_group path then used by_expr->i64 (garbage for vector
 * form) and crashed inside ray_group_fn. */
static test_result_t test_eval_select_by_vec_str_key(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['s 'p] "
        "(list (as 'STR [\"A\" \"B\" \"A\" \"B\"]) [10.0 20.0 30.0 40.0]))) "
        "(select {from: t by: [s] m: (+ p p)}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);
    /* Result must have both key and non-agg columns */
    TEST_ASSERT_EQ_I(ray_table_ncols(result), 2);
    int64_t m_id = ray_sym_intern("m", 1);
    ray_t* m_col = ray_table_get_col(result, m_id);
    TEST_ASSERT_NOT_NULL(m_col);
    TEST_ASSERT_EQ_I(m_col->type, RAY_LIST);
    ray_t** mi = (ray_t**)ray_data(m_col);
    /* group "A" → rows [0,2] → p=[10,30] → (+ p p)=[20,60] */
    TEST_ASSERT_EQ_I(mi[0]->len, 2);
    double* d0 = (double*)ray_data(mi[0]);
    TEST_ASSERT((d0[0]) == (20.0), "double == failed");
    TEST_ASSERT((d0[1]) == (60.0), "double == failed");
    ray_release(result);
    PASS();
}

/* ---- Test: multi-key by + non-agg returns nyi error ---- */
static test_result_t test_eval_select_by_multi_nonagg_nyi(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['a 'b 'p] "
        "(list [X X Y] [1 2 1] [10.0 20.0 30.0]))) "
        "(select {from: t by: [a b] m: (+ p p)}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));
    ray_release(result);
    PASS();
}

/* ---- Test: update ---- */
static test_result_t test_eval_update(void) {
    /* Update salary to (* salary 2) where name == 2 (second row) */
    ray_t* result = ray_eval_str(
        "(do (set t (table ['name 'dept 'salary] "
        "(list [1 2 3] [10 20 10] [50000 60000 70000]))) "
        "(update {salary: (* salary 2) from: t where: (== name 2)}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    /* Check salary column: [50000, 120000, 70000] */
    int64_t sal_id = ray_sym_intern("salary", 6);
    ray_t* sal_col = ray_table_get_col(result, sal_id);
    TEST_ASSERT_NOT_NULL(sal_col);
    int64_t* sal_data = (int64_t*)ray_data(sal_col);
    TEST_ASSERT_EQ_I(sal_data[0], 50000);
    TEST_ASSERT_EQ_I(sal_data[1], 120000);
    TEST_ASSERT_EQ_I(sal_data[2], 70000);
    ray_release(result);
    PASS();
}

/* ---- Test: update without where broadcasts scalar ---- */
static test_result_t test_eval_update_no_where(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['x 'y] (list [1 2 3] [10 20 30]))) "
        "(update {x: 99 from: t}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    int64_t x_id = ray_sym_intern("x", 1);
    ray_t* x_col = ray_table_get_col(result, x_id);
    TEST_ASSERT_NOT_NULL(x_col);
    TEST_ASSERT_EQ_I(x_col->type, RAY_I64);
    TEST_ASSERT_EQ_I(x_col->len, 3);
    int64_t* xd = (int64_t*)ray_data(x_col);
    TEST_ASSERT_EQ_I(xd[0], 99);
    TEST_ASSERT_EQ_I(xd[1], 99);
    TEST_ASSERT_EQ_I(xd[2], 99);
    /* y column should be unchanged */
    int64_t y_id = ray_sym_intern("y", 1);
    ray_t* y_col = ray_table_get_col(result, y_id);
    int64_t* yd = (int64_t*)ray_data(y_col);
    TEST_ASSERT_EQ_I(yd[0], 10);
    TEST_ASSERT_EQ_I(yd[1], 20);
    TEST_ASSERT_EQ_I(yd[2], 30);
    ray_release(result);
    PASS();
}

/* ---- Test: masked update on string column ---- */
static test_result_t test_eval_update_str_masked(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['id 'name] (list [1 2 3] [\"alice\" \"bob\" \"carol\"]))) "
        "(update {name: \"REPLACED\" from: t where: (== id 2)}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    int64_t name_id = ray_sym_intern("name", 4);
    ray_t* name_col = ray_table_get_col(result, name_id);
    TEST_ASSERT_NOT_NULL(name_col);
    TEST_ASSERT_EQ_I(name_col->type, RAY_STR);
    size_t slen;
    const char* s0 = ray_str_vec_get(name_col, 0, &slen);
    TEST_ASSERT_EQ_I(slen, 5);
    TEST_ASSERT_MEM_EQ(5, s0, "alice");
    const char* s1 = ray_str_vec_get(name_col, 1, &slen);
    TEST_ASSERT_EQ_I(slen, 8);
    TEST_ASSERT_MEM_EQ(8, s1, "REPLACED");
    const char* s2 = ray_str_vec_get(name_col, 2, &slen);
    TEST_ASSERT_EQ_I(slen, 5);
    TEST_ASSERT_MEM_EQ(5, s2, "carol");
    ray_release(result);
    PASS();
}

/* ---- Test: table with mixed types rejects non-string in string column ---- */
static test_result_t test_eval_table_mixed_type_error(void) {
    ray_t* result = ray_eval_str("(table ['s] (list [\"ok\" 42]))");
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));
    PASS();
}

/* ---- Test: update string column with non-string expr returns error ---- */
static test_result_t test_eval_update_str_type_mismatch(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['id 'name] (list [1 2 3] [\"alice\" \"bob\" \"carol\"]))) "
        "(update {name: id from: t where: (== id 2)}))");
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));
    PASS();
}

/* ---- Test: select constant over empty table ---- */
static test_result_t test_eval_select_empty_const(void) {
    /* Create table then filter all rows to get empty table */
    ray_t* result = ray_eval_str(
        "(do (set t (select {x: x from: (table ['x] (list [1])) where: (== x 0)})) "
        "(select {y: 1 from: t}))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 0);
    ray_release(result);
    PASS();
}

/* ---- Test: insert ---- */
static test_result_t test_eval_insert(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['name 'salary] (list [1 2 3] [50000 60000 70000]))) "
        "(insert t (list 4 80000)))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 4);
    /* Verify last row */
    int64_t name_id = ray_sym_intern("name", 4);
    ray_t* name_col = ray_table_get_col(result, name_id);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(name_col))[3], 4);
    int64_t sal_id = ray_sym_intern("salary", 6);
    ray_t* sal_col = ray_table_get_col(result, sal_id);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sal_col))[3], 80000);
    ray_release(result);
    PASS();
}

/* ---- Test: insert vec/list append (arity 2) ---- */
static test_result_t test_eval_insert_vec_append(void) {
    ASSERT_EQ("(do (set v (til 5)) (insert 'v 99) v)", "[0 1 2 3 4 99]");
    ASSERT_EQ("(do (set v (til 3)) (insert 'v [10 20 30]) v)", "[0 1 2 10 20 30]");
    /* Return value mirrors the rebound v */
    ASSERT_EQ("(do (set v (til 3)) (insert 'v 9))", "[0 1 2 9]");
    PASS();
}

static test_result_t test_eval_insert_list_append(void) {
    ASSERT_EQ("(do (set l (list 1 2 3)) (insert 'l 42) l)", "(list 1 2 3 42)");
    /* Append a list as a single nested slot — never splice on append */
    ASSERT_EQ("(do (set l (list 1 2)) (insert 'l (list 9 8)) (count l))", "3");
    PASS();
}

/* ---- Test: insert positional (arity 3, scalar idx) ---- */
static test_result_t test_eval_insert_vec_positional(void) {
    /* Head, middle, tail (== append) */
    ASSERT_EQ("(do (set v (til 5)) (insert 'v 0 99) v)", "[99 0 1 2 3 4]");
    ASSERT_EQ("(do (set v (til 5)) (insert 'v 2 99) v)", "[0 1 99 2 3 4]");
    ASSERT_EQ("(do (set v (til 5)) (insert 'v 5 99) v)", "[0 1 2 3 4 99]");
    /* Splice a same-typed vec at position */
    ASSERT_EQ("(do (set v (til 5)) (insert 'v 2 [10 20]) v)", "[0 1 10 20 2 3 4]");
    /* Empty target */
    ASSERT_EQ("(do (set v (til 0)) (insert 'v 0 7) v)", "[7]");
    /* Type mismatch: insert string atom into I64 vec */
    ASSERT_ER("(do (set v (til 3)) (insert 'v 1 \"x\"))", "type");
    /* Out of range */
    ASSERT_ER("(do (set v (til 3)) (insert 'v -1 9))", "range");
    ASSERT_ER("(do (set v (til 3)) (insert 'v 4 9))", "range");
    PASS();
}

static test_result_t test_eval_insert_list_positional(void) {
    ASSERT_EQ("(do (set l (list 1 2 3)) (insert 'l 0 99) l)", "(list 99 1 2 3)");
    ASSERT_EQ("(do (set l (list 1 2 3)) (insert 'l 1 99) l)", "(list 1 99 2 3)");
    ASSERT_EQ("(do (set l (list 1 2 3)) (insert 'l 3 99) l)", "(list 1 2 3 99)");
    /* Insert a list as a single slot — no splice, count grows by one */
    ASSERT_EQ("(do (set l (list 1 2)) (insert 'l 1 (list 9 8)) (count l))", "3");
    /* Out of range */
    ASSERT_ER("(do (set l (list 1 2)) (insert 'l -1 9))", "range");
    ASSERT_ER("(do (set l (list 1 2)) (insert 'l 3 9))", "range");
    PASS();
}

/* ---- Test: insert positional multi (arity 3, vec idx) ---- */
static test_result_t test_eval_insert_positional_multi(void) {
    /* Broadcast a single value across N pre-positions */
    ASSERT_EQ("(do (set v (til 5)) (insert 'v [0 2 4] 99) v)",
              "[99 0 1 99 2 3 99 4]");
    /* Parallel insertion */
    ASSERT_EQ("(do (set v (til 5)) (insert 'v [1 3] [10 30]) v)",
              "[0 10 1 2 30 3 4]");
    /* Length mismatch */
    ASSERT_ER("(do (set v (til 5)) (insert 'v [0 2] [10 20 30]))", "range");
    /* Stable order on duplicate indices: both go to same pre-position
     * in original input order */
    ASSERT_EQ("(do (set v (til 3)) (insert 'v [1 1] [10 20]) v)",
              "[0 10 20 1 2]");
    /* List multi-insert */
    ASSERT_EQ("(do (set l (list 1 2 3)) (insert 'l [0 2] (list 10 30)) (count l))",
              "5");
    PASS();
}

/* ---- Test: insert preserves typed-null semantics ---- */
static test_result_t test_eval_insert_typed_null(void) {
    /* Arity-2 atom append: null bit must carry through */
    ASSERT_EQ("(do (set v [1 2 3]) (insert 'v 0Nl) v)", "[1 2 3 0Nl]");
    /* Arity-3 scalar insert: null bit must carry through */
    ASSERT_EQ("(do (set v [1 2 3]) (insert 'v 1 0Nl) v)", "[1 0Nl 2 3]");
    /* Float null */
    ASSERT_EQ("(do (set v [1.0 2.0]) (insert 'v 0 0Nf) v)", "[0Nf 1.0 2.0]");
    /* Multi-insert broadcast of typed null */
    ASSERT_EQ("(do (set v [1 2 3 4 5]) (insert 'v [0 3] 0Nl) v)",
              "[0Nl 1 2 3 0Nl 4 5]");
    PASS();
}

/* ---- Test: insert preserves GUID atom payload ---- */
static test_result_t test_eval_insert_guid(void) {
    /* Arity-2 atom append: inserted atom equals source atom */
    ASSERT_EQ("(do (set g (guid 3)) (set x (first (guid 1))) "
              "    (insert 'g x) (== x (get g 3)))", "true");
    /* Arity-3 scalar insert at head */
    ASSERT_EQ("(do (set g (guid 3)) (set x (first (guid 1))) "
              "    (insert 'g 0 x) (== x (get g 0)))", "true");
    /* Multi-insert broadcast: same atom at two pre-positions */
    ASSERT_EQ("(do (set g (guid 3)) (set x (first (guid 1))) "
              "    (insert 'g [0 2] x) "
              "    (and (== x (get g 0)) (== x (get g 3))))", "true");
    /* Splice a same-typed vec */
    ASSERT_EQ("(do (set g (guid 3)) (set v (guid 2)) "
              "    (insert 'g 1 v) (count g))", "5");

    /* Typed-null GUID atom (val->obj == NULL) — must be accepted and
     * stored with the null bit set, not rejected with "type". */
    ray_t* g = ray_vec_new(RAY_GUID, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(g));
    g->len = 2;
    memset(ray_data(g), 0xAA, 2 * 16);

    ray_t* null_atom = ray_typed_null(-RAY_GUID);
    TEST_ASSERT_FALSE(RAY_IS_ERR(null_atom));
    TEST_ASSERT_EQ_PTR(null_atom->obj, NULL);

    g = ray_vec_insert_at(g, 1, null_atom->obj ? ray_data(null_atom->obj) : (const void*)"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0");
    TEST_ASSERT_FALSE(RAY_IS_ERR(g));
    TEST_ASSERT_EQ_I(g->len, 3);
    ray_release(null_atom);
    ray_release(g);

    /* Now exercise the dispatcher path end-to-end — construct a GUID vec,
     * bind it, call ray_vec_insert_many directly with a typed-null broadcast. */
    ray_t* h = ray_vec_new(RAY_GUID, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));
    h->len = 3;
    memset(ray_data(h), 0xBB, 3 * 16);
    ray_t* idxs = ray_vec_new(RAY_I64, 2);
    idxs->len = 2;
    ((int64_t*)ray_data(idxs))[0] = 0;
    ((int64_t*)ray_data(idxs))[1] = 2;
    ray_t* nval = ray_typed_null(-RAY_GUID);
    ray_t* out = ray_vec_insert_many(h, idxs, nval);
    TEST_ASSERT_FALSE(RAY_IS_ERR(out));
    TEST_ASSERT_EQ_I(out->len, 5);
    TEST_ASSERT_TRUE(ray_vec_is_null(out, 0));
    TEST_ASSERT_TRUE(ray_vec_is_null(out, 3));
    TEST_ASSERT_FALSE(ray_vec_is_null(out, 1));
    ray_release(nval);
    ray_release(idxs);
    ray_release(h);
    ray_release(out);
    PASS();
}

/* ---- Test: insert error paths ---- */
static test_result_t test_eval_insert_positional_errors(void) {
    /* Unbound symbol */
    ASSERT_ER("(insert 'nope 0 9)", "domain");
    /* Vec target with non-I64 idx */
    ASSERT_ER("(do (set v (til 3)) (insert 'v 1.0 9))", "type");
    /* List multi-insert with non-list val */
    ASSERT_ER("(do (set l (list 1 2 3)) (insert 'l [0 1] 99))", "type");
    /* RAY_STR multi-insert is rejected */
    ASSERT_ER("(do (set s [\"a\" \"b\"]) (insert 's [0 1] \"c\"))", "type");
    /* Slice target materializes and succeeds */
    ASSERT_EQ("(do (set v (til 5)) (set s (take v 3)) (insert 's 0 99) s)",
              "[99 0 1 2]");
    PASS();
}

/* ---- Test: upsert (update existing row) ---- */
static test_result_t test_eval_upsert(void) {
    /* Upsert by 'name key — row with name=2 exists, update it */
    ray_t* result = ray_eval_str(
        "(do (set t (table ['name 'salary] (list [1 2 3] [50000 60000 70000]))) "
        "(upsert t 'name (list 2 99000)))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    /* Verify row 2's salary was updated */
    int64_t sal_id = ray_sym_intern("salary", 6);
    ray_t* sal_col = ray_table_get_col(result, sal_id);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sal_col))[1], 99000);
    ray_release(result);
    PASS();
}

/* ---- Test: upsert with F64 key and I64 promotion ---- */
static test_result_t test_eval_upsert_f64_key(void) {
    /* Table has F64 key column; upsert with integer literal should promote */
    ray_t* result = ray_eval_str(
        "(do (set t (table ['k 'v] (list [1.0 2.0] [10 20]))) "
        "(upsert t 'k (list 2 99)))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* Should update, not insert — still 2 rows */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);
    int64_t v_id = ray_sym_intern("v", 1);
    ray_t* v_col = ray_table_get_col(result, v_id);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(v_col))[1], 99);
    ray_release(result);
    PASS();
}

/* ---- Test: upsert with string key ---- */
static test_result_t test_eval_upsert_str_key(void) {
    ray_t* result = ray_eval_str(
        "(do (set t (table ['k 'v] (list [\"a\" \"b\"] [1 2]))) "
        "(upsert t 'k (list \"b\" 99)))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* Should update row with key "b", not insert */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);
    int64_t v_id = ray_sym_intern("v", 1);
    ray_t* v_col = ray_table_get_col(result, v_id);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(v_col))[1], 99);
    ray_release(result);
    PASS();
}

/* ---- Test: upsert type mismatch returns error ---- */
static test_result_t test_eval_upsert_type_mismatch(void) {
    /* Passing integer key to string key column should return error, not crash */
    ray_t* result = ray_eval_str(
        "(do (set t (table ['k 'v] (list [\"a\" \"b\"] [1 2]))) "
        "(upsert t 'k (list 42 99)))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));
    PASS();
}

/* ---- Test: left join ---- */
static test_result_t test_eval_left_join(void) {
    ray_t* result = ray_eval_str(
        "(do (set t1 (table ['id 'name] (list [1 2 3] [10 20 30]))) "
        "(set t2 (table ['id 'val] (list [1 3 4] [100 300 400]))) "
        "(left-join t1 t2 ['id]))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* All 3 left rows kept */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    /* Should have columns: id, name, val */
    int64_t val_id = ray_sym_intern("val", 3);
    ray_t* val_col = ray_table_get_col(result, val_id);
    TEST_ASSERT_NOT_NULL(val_col);
    int64_t* val_data = (int64_t*)ray_data(val_col);
    TEST_ASSERT_EQ_I(val_data[0], 100);  /* id=1 matched */
    TEST_ASSERT_EQ_I(val_data[2], 300);  /* id=3 matched */
    ray_release(result);
    PASS();
}

/* ---- Test: inner join ---- */
static test_result_t test_eval_inner_join(void) {
    ray_t* result = ray_eval_str(
        "(do (set t1 (table ['id 'name] (list [1 2 3] [10 20 30]))) "
        "(set t2 (table ['id 'val] (list [1 3 4] [100 300 400]))) "
        "(inner-join t1 t2 ['id]))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* Only matching rows: id=1 and id=3 */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);
    int64_t val_id = ray_sym_intern("val", 3);
    ray_t* val_col = ray_table_get_col(result, val_id);
    TEST_ASSERT_NOT_NULL(val_col);
    int64_t* val_data = (int64_t*)ray_data(val_col);
    TEST_ASSERT_EQ_I(val_data[0], 100);
    TEST_ASSERT_EQ_I(val_data[1], 300);
    ray_release(result);
    PASS();
}

/* ---- Test: window join (ASOF) ---- */
static test_result_t test_eval_window_join(void) {
    /* ASOF join: for each left row, find closest right row with ts <= left.ts
     * within same sym partition */
    ray_t* result = ray_eval_str(
        "(do (set trades (table ['sym 'ts 'price] "
        "(list [1 1] [100 200] [10 20]))) "
        "(set quotes (table ['sym 'ts 'bid] "
        "(list [1 1 1] [50 150 250] [5 15 25]))) "
        "(window-join trades quotes ['sym] 'ts))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* 2 left rows, each matched to closest quote */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);
    /* bid column from right: ts=100→bid=5 (closest ts=50), ts=200→bid=15 (closest ts=150) */
    int64_t bid_id = ray_sym_intern("bid", 3);
    ray_t* bid_col = ray_table_get_col(result, bid_id);
    TEST_ASSERT_NOT_NULL(bid_col);
    int64_t* bid_data = (int64_t*)ray_data(bid_col);
    TEST_ASSERT_EQ_I(bid_data[0], 5);
    TEST_ASSERT_EQ_I(bid_data[1], 15);
    ray_release(result);
    PASS();
}

/* ---- Test: println ---- */
static test_result_t test_eval_println(void) {
    ray_t* result = ray_eval_str("(println \"hello\")");
    /* println returns RAY_NULL_OBJ (no value — side-effect only) */
    TEST_ASSERT_TRUE(RAY_IS_NULL(result));
    PASS();
}

/* ---- Sort decode-gather regression tests (2000+ rows → radix path) ---- */

static test_result_t test_sort_decode_i64(void) {
    /* Random unsorted I64, large range (>2^24) → non-packed MSD radix → decode.
     * Verify: (1) every pair ordered, (2) sum preserved, (3) count preserved. */
    ray_t* tmp = ray_eval_str("(set _sv (rand 2000 100000000))");
    if (tmp) { if (RAY_IS_ERR(tmp)) ray_error_free(tmp); else ray_release(tmp); }
    ray_t* s = ray_eval_str("(asc _sv)");
    TEST_ASSERT_NOT_NULL(s); TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    ray_t* v = ray_eval_str("_sv");
    TEST_ASSERT_EQ_I(ray_len(s), 2000);
    int64_t* sd = (int64_t*)ray_data(s);
    for (int64_t i = 0; i < 1999; i++) TEST_ASSERT_TRUE(sd[i] <= sd[i + 1]);
    int64_t sum_orig = 0, sum_sorted = 0;
    int64_t* vd = (int64_t*)ray_data(v);
    for (int64_t i = 0; i < 2000; i++) { sum_orig += vd[i]; sum_sorted += sd[i]; }
    TEST_ASSERT_TRUE(sum_orig == sum_sorted);
    ray_release(s); ray_release(v);
    PASS();
}

static test_result_t test_sort_decode_f64(void) {
    /* Random unsorted F64 with negatives — always 8-byte keys → non-packed → decode. */
    ray_t* tmp = ray_eval_str("(set _sv (* 1.0 (- (rand 2000 2000000) 1000000)))");
    if (tmp) { if (RAY_IS_ERR(tmp)) ray_error_free(tmp); else ray_release(tmp); }
    ray_t* s = ray_eval_str("(asc _sv)");
    TEST_ASSERT_NOT_NULL(s); TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    ray_t* v = ray_eval_str("_sv");
    double* sd = (double*)ray_data(s);
    for (int64_t i = 0; i < 1999; i++) TEST_ASSERT_TRUE(sd[i] <= sd[i + 1]);
    double sum_orig = 0, sum_sorted = 0;
    double* vd = (double*)ray_data(v);
    for (int64_t i = 0; i < 2000; i++) { sum_orig += vd[i]; sum_sorted += sd[i]; }
    TEST_ASSERT((sum_orig) == (sum_sorted), "double == failed");
    ray_release(s); ray_release(v);
    PASS();
}

static test_result_t test_sort_decode_desc(void) {
    /* Random unsorted I64 desc — large range → non-packed decode. */
    ray_t* tmp = ray_eval_str("(set _sv (rand 2000 100000000))");
    if (tmp) { if (RAY_IS_ERR(tmp)) ray_error_free(tmp); else ray_release(tmp); }
    ray_t* s = ray_eval_str("(desc _sv)");
    TEST_ASSERT_NOT_NULL(s); TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    ray_t* v = ray_eval_str("_sv");
    int64_t* sd = (int64_t*)ray_data(s);
    for (int64_t i = 0; i < 1999; i++) TEST_ASSERT_TRUE(sd[i] >= sd[i + 1]);
    int64_t sum_orig = 0, sum_sorted = 0;
    int64_t* vd = (int64_t*)ray_data(v);
    for (int64_t i = 0; i < 2000; i++) { sum_orig += vd[i]; sum_sorted += sd[i]; }
    TEST_ASSERT_TRUE(sum_orig == sum_sorted);
    ray_release(s); ray_release(v);
    PASS();
}

static test_result_t test_sort_decode_f64_neg(void) {
    /* Random unsorted F64 desc with negatives — verify descending + sum. */
    ray_t* tmp = ray_eval_str("(set _sv (* 1.0 (- (rand 2000 2000000) 1000000)))");
    if (tmp) { if (RAY_IS_ERR(tmp)) ray_error_free(tmp); else ray_release(tmp); }
    ray_t* s = ray_eval_str("(desc _sv)");
    TEST_ASSERT_NOT_NULL(s); TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    ray_t* v = ray_eval_str("_sv");
    double* sd = (double*)ray_data(s);
    for (int64_t i = 0; i < 1999; i++) TEST_ASSERT_TRUE(sd[i] >= sd[i + 1]);
    double sum_orig = 0, sum_sorted = 0;
    double* vd = (double*)ray_data(v);
    for (int64_t i = 0; i < 2000; i++) { sum_orig += vd[i]; sum_sorted += sd[i]; }
    TEST_ASSERT((sum_orig) == (sum_sorted), "double == failed");
    ray_release(s); ray_release(v);
    PASS();
}

/* ---- Tests: radix sort decode for n > RADIX_SORT_THRESHOLD (4096) ----
 * Below 4096, key_introsort is used (no radix decode).  These tests
 * ensure the radix sort double-buffer correctly hands back the sorted
 * keys for decode — the use-after-free that produced -nan on F64. */

static test_result_t test_sort_decode_radix_f64(void) {
    /* Tile 10 F64 values to 4097 (just past RADIX_SORT_THRESHOLD),
     * sort ascending, verify ordering and no NaN. */
    ray_t* s = ray_eval_str("(asc (take [9.9 1.1 5.5 3.3 7.7 2.2 8.8 4.4 6.6 0.0] 4097))");
    TEST_ASSERT_NOT_NULL(s); TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    TEST_ASSERT_EQ_I(ray_len(s), 4097);
    double* d = (double*)ray_data(s);
    for (int64_t i = 0; i < 4097; i++) TEST_ASSERT_FALSE(d[i] != d[i]); /* no NaN */
    for (int64_t i = 0; i < 4096; i++) TEST_ASSERT_TRUE(d[i] <= d[i + 1]);
    TEST_ASSERT((d[0]) == (0.0), "double == failed");
    TEST_ASSERT((d[4096]) == (9.9), "double == failed");
    ray_release(s);
    PASS();
}

static test_result_t test_sort_decode_radix_f64_desc(void) {
    ray_t* s = ray_eval_str("(desc (take [9.9 1.1 5.5 -3.3 7.7 -2.2 8.8 -4.4 6.6 0.0] 5000))");
    TEST_ASSERT_NOT_NULL(s); TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    TEST_ASSERT_EQ_I(ray_len(s), 5000);
    double* d = (double*)ray_data(s);
    for (int64_t i = 0; i < 5000; i++) TEST_ASSERT_FALSE(d[i] != d[i]);
    for (int64_t i = 0; i < 4999; i++) TEST_ASSERT_TRUE(d[i] >= d[i + 1]);
    TEST_ASSERT((d[0]) == (9.9), "double == failed");
    TEST_ASSERT((d[4999]) == (-4.4), "double == failed");
    ray_release(s);
    PASS();
}

static test_result_t test_sort_decode_radix_i64(void) {
    /* I64 with large range forces 8-byte radix keys → non-packed radix decode. */
    ray_t* tmp = ray_eval_str("(set _rv (rand 5000 100000000))");
    if (tmp) { if (RAY_IS_ERR(tmp)) ray_error_free(tmp); else ray_release(tmp); }
    ray_t* s = ray_eval_str("(asc _rv)");
    TEST_ASSERT_NOT_NULL(s); TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    ray_t* v = ray_eval_str("_rv");
    TEST_ASSERT_EQ_I(ray_len(s), 5000);
    int64_t* sd = (int64_t*)ray_data(s);
    for (int64_t i = 0; i < 4999; i++) TEST_ASSERT_TRUE(sd[i] <= sd[i + 1]);
    int64_t sum_orig = 0, sum_sorted = 0;
    int64_t* vd = (int64_t*)ray_data(v);
    for (int64_t i = 0; i < 5000; i++) { sum_orig += vd[i]; sum_sorted += sd[i]; }
    TEST_ASSERT_TRUE(sum_orig == sum_sorted);
    ray_release(s); ray_release(v);
    PASS();
}

/* ---- Sort regression: long common prefix must not be quadratic ----
 * Every string shares the first 40 bytes ("AAAAAAAA..."), then a distinct
 * 6-byte suffix.  The packed-key window is 32 bytes, so the radix walks
 * the entire first window uniform, repacks the next window, and only
 * finds ordering in the suffix bytes.  A quadratic base-case fallback
 * at window exhaustion would time out on this input — the repack path
 * keeps it linear in total string bytes. */
static test_result_t test_sort_long_common_prefix(void) {
    /* Build a column of 4000 strings with identical 40-byte prefix
     * followed by a 6-byte distinct suffix, inserted in reverse order
     * so the input is maximally unsorted. */
    const int64_t n = 4000;
    ray_t* col = ray_vec_new(RAY_STR, n);
    TEST_ASSERT_NOT_NULL(col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(col));
    char buf[64];
    for (int64_t i = 0; i < n; i++) {
        int64_t v = n - 1 - i;
        snprintf(buf, sizeof buf,
                 "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA%06ld", (long)v);
        col = ray_str_vec_append(col, buf, strlen(buf));
        TEST_ASSERT_FALSE(RAY_IS_ERR(col));
    }
    /* Sanity check the input: every row should be 46 bytes. */
    TEST_ASSERT_EQ_I(ray_len(col), n);
    for (int64_t i = 0; i < n; i++) {
        size_t l; const char* p = ray_str_vec_get(col, i, &l);
        TEST_ASSERT_EQ_I((int)l, 46);
        (void)p;
    }
    /* Bind the column into the Rayfall env and get sort indices. */
    ray_env_set(ray_sym_intern("_lcp", 4), col);
    ray_t* idx = ray_eval_str("(iasc _lcp)");
    TEST_ASSERT_NOT_NULL(idx);
    TEST_ASSERT_FALSE(RAY_IS_ERR(idx));
    TEST_ASSERT_EQ_I(ray_len(idx), n);
    /* Walk the sort indices and verify each points to a string greater
     * than its predecessor. */
    int64_t* p = (int64_t*)ray_data(idx);
    size_t la, lb;
    const char* pa = ray_str_vec_get(col, p[0], &la);
    for (int64_t i = 1; i < n; i++) {
        const char* pb = ray_str_vec_get(col, p[i], &lb);
        size_t m = la < lb ? la : lb;
        int cmp = memcmp(pa, pb, m);
        if (!(cmp < 0 || (cmp == 0 && la < lb))) {
            fprintf(stderr, "FAIL at i=%ld p[i-1]=%ld p[i]=%ld\n"
                            "  prev=%.46s\n  cur =%.46s\n",
                    (long)i, (long)p[i-1], (long)p[i], pa, pb);
            TEST_ASSERT_TRUE(0);
        }
        pa = pb;
        la = lb;
    }
    ray_release(idx);
    ray_release(col);
    PASS();
}

/* ---- Sort regression: embedded NUL + prefix-of-each-other ---------
 * Two distinct strings can produce bit-identical zero-padded packed
 * prefixes in the RAY_STR radix when the longer one has embedded
 * NULs at exactly the positions where the shorter one was
 * zero-padded.  ray_str_t_cmp puts the strict-prefix string first,
 * but the packed radix saw them as tied.  Previously the repack
 * short-circuit (any_tail=false) returned without ordering by len,
 * leaving a bucket > the insertion-sort base case in arbitrary
 * order.  This test builds such a bucket and verifies the sort
 * groups all shorter strings before the longer ones.
 *
 * Construction: 50 copies of "abc" (len 3) and 50 copies of
 * "abc\0\0" (len 5).  Both pad to 0x6162630000000000 in the packed
 * window (parts_bytes=8 at the top level), so they all land in the
 * same top-level bucket, flow through uniform radix iterations, and
 * hit the any_tail=false short-circuit at the first repack. */
static test_result_t test_sort_embedded_nul(void) {
    const int64_t per_group = 50;
    const int64_t n = 2 * per_group;  /* 100 > BASE_CASE */
    ray_t* col = ray_vec_new(RAY_STR, n);
    TEST_ASSERT_NOT_NULL(col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(col));
    /* Interleave so the run-detection shortcut cannot fire — adjacent
     * pairs alternate (5,3) and (3,5), killing both monotone flags. */
    const char long_str[5] = { 'a', 'b', 'c', '\0', '\0' };
    for (int64_t k = 0; k < per_group; k++) {
        col = ray_str_vec_append(col, long_str, 5);
        TEST_ASSERT_FALSE(RAY_IS_ERR(col));
        col = ray_str_vec_append(col, "abc", 3);
        TEST_ASSERT_FALSE(RAY_IS_ERR(col));
    }
    ray_env_set(ray_sym_intern("_enul", 5), col);
    ray_t* idx = ray_eval_str("(iasc _enul)");
    TEST_ASSERT_NOT_NULL(idx);
    TEST_ASSERT_FALSE(RAY_IS_ERR(idx));
    TEST_ASSERT_EQ_I(ray_len(idx), n);
    int64_t* p = (int64_t*)ray_data(idx);
    /* First per_group entries must point at the len-3 rows, next
     * per_group at the len-5 rows. */
    for (int64_t i = 0; i < per_group; i++) {
        size_t l;
        (void)ray_str_vec_get(col, p[i], &l);
        TEST_ASSERT_EQ_I((int)l, 3);
    }
    for (int64_t i = per_group; i < n; i++) {
        size_t l;
        (void)ray_str_vec_get(col, p[i], &l);
        TEST_ASSERT_EQ_I((int)l, 5);
    }
    ray_release(idx);
    ray_release(col);
    PASS();
}

/* ---- Test: read/write CSV roundtrip ---- */
static test_result_t test_eval_read_write_csv(void) {
    /* Create a table, write it to CSV, read it back */
    ray_t* result = ray_eval_str(
        "(do (set t (table ['a 'b] (list [1 2 3] [10 20 30]))) "
        "(.csv.write t \"/tmp/test_rayfall.csv\") "
        "(set t2 (.csv.read \"/tmp/test_rayfall.csv\")) "
        "(count t2))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 3);
    ray_release(result);
    PASS();
}

/* ---- Test: as (type cast) ---- */
static test_result_t test_eval_as_cast(void) {
    ray_t* result = ray_eval_str("(as 'I64 \"42\")");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 42);
    ray_release(result);
    PASS();
}

/* ---- Test: type introspection ---- */
static test_result_t test_eval_type(void) {
    /* type returns a symbol name like 'i64, 'f64, 'b8 */
    ray_t* result = ray_eval_str("(type 42)");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_SYM);
    ray_release(result);
    result = ray_eval_str("(type 3.14)");
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_SYM);
    ray_release(result);
    result = ray_eval_str("(type true)");
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_SYM);
    ray_release(result);
    PASS();
}

/* ---- Suite definition ---- */
/* ---- Test: env prefix lookup ---- */
static test_result_t test_env_lookup_prefix(void) {
    /* After ray_lang_init(), global env has builtins like "select", "sum", etc. */
    const char* results[64];

    /* Exact prefix match for "sum" — should find it */
    int64_t n = ray_env_lookup_prefix("sum", 3, results, 64);
    TEST_ASSERT(((int)n) >= (1), "(int)n >= 1");
    int found_sum = 0;
    for (int64_t i = 0; i < n; i++) {
        if (strcmp(results[i], "sum") == 0) { found_sum = 1; break; }
    }
    TEST_ASSERT_TRUE(found_sum);

    /* Prefix "sel" should match "select" */
    n = ray_env_lookup_prefix("sel", 3, results, 64);
    TEST_ASSERT(((int)n) >= (1), "(int)n >= 1");
    int found_select = 0;
    for (int64_t i = 0; i < n; i++) {
        if (strcmp(results[i], "select") == 0) { found_select = 1; break; }
    }
    TEST_ASSERT_TRUE(found_select);

    /* Keywords: prefix "fn" should match keyword "fn" */
    n = ray_env_lookup_prefix("fn", 2, results, 64);
    TEST_ASSERT(((int)n) >= (1), "(int)n >= 1");
    int found_fn = 0;
    for (int64_t i = 0; i < n; i++) {
        if (strcmp(results[i], "fn") == 0) { found_fn = 1; break; }
    }
    TEST_ASSERT_TRUE(found_fn);

    /* Nonsense prefix should return 0 */
    n = ray_env_lookup_prefix("zzzzz", 5, results, 64);
    TEST_ASSERT_EQ_I((int)n, 0);

    /* Results should be sorted */
    n = ray_env_lookup_prefix("a", 1, results, 64);
    for (int64_t i = 1; i < n; i++) {
        TEST_ASSERT((strcmp(results[i - 1], results[i])) <= (0), "strcmp(results[i - 1], results[i]) <= 0");
    }

    PASS();
}

/* ---- Verb engine integration tests ---- */

static test_result_t test_verb_sum_til(void) {
    ray_t* result = ray_eval_str("(sum (til 100))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 4950);
    ray_release(result);
    PASS();
}

static test_result_t test_verb_avg_til(void) {
    ray_t* result = ray_eval_str("(avg (til 10))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_F64);
    TEST_ASSERT_EQ_F(result->f64, 4.5, 1e-4);
    ray_release(result);
    PASS();
}

static test_result_t test_verb_min_til(void) {
    ray_t* result = ray_eval_str("(min (til 10))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 0);
    ray_release(result);
    PASS();
}

static test_result_t test_verb_max_til(void) {
    ray_t* result = ray_eval_str("(max (til 10))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 9);
    ray_release(result);
    PASS();
}

static test_result_t test_verb_count_til(void) {
    ray_t* result = ray_eval_str("(count (til 100))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 100);
    ray_release(result);
    PASS();
}

static test_result_t test_verb_first_til(void) {
    ray_t* result = ray_eval_str("(first (til 10))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 0);
    ray_release(result);
    PASS();
}

static test_result_t test_verb_last_til(void) {
    ray_t* result = ray_eval_str("(last (til 10))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 9);
    ray_release(result);
    PASS();
}

static test_result_t test_verb_dev_til(void) {
    ray_t* result = ray_eval_str("(dev (til 10))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_F64);
    TEST_ASSERT_EQ_F(result->f64, 2.8722813232690143, 1e-4);
    ray_release(result);
    PASS();
}

static test_result_t test_verb_if_sum(void) {
    ray_t* result = ray_eval_str("(if (> (sum (til 10)) 0) 1 0)");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 1);
    ray_release(result);
    PASS();
}

static test_result_t test_verb_sum_var(void) {
    ray_t* result = ray_eval_str("(set x (til 10)) (sum x)");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 45);
    ray_release(result);
    PASS();
}

/* Regression: binary op on boxed list with nested vector used to segfault
 * in release mode because the raw atom fn received a vector argument. */
static test_result_t test_atomic_map_nested_vec(void) {
    /* scalar + list containing a vector → recursive auto-map */
    ASSERT_EQ("(+ 1 (list 1 2 (til 5)))", "(list 2 3 [1 2 3 4 5])");
    /* scalar + list containing only atoms (homogeneous) */
    ASSERT_EQ("(+ 1 (list 1 2 3))", "[2 3 4]");
    /* scalar + list with nested list */
    ASSERT_EQ("(+ 10 (list 1 (list 2 3)))", "(list 11 (list 12 13))");
    /* type error still propagated for incompatible element */
    ASSERT_ER("(+ 1 (list 1 2 \"s\"))", "type");
    PASS();
}

/* Verify that errors in compiled lambdas produce a trace with source info */
static test_result_t test_error_trace_exists(void) {
    /* Eval a lambda that will error — the trace should be built */
    ray_clear_error_trace();
    ray_t* r = ray_eval_str("((fn [x] (+ x \"s\")) 1)");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_t* trace = ray_get_error_trace();
    TEST_ASSERT_NOT_NULL(trace);
    TEST_ASSERT_TRUE(ray_len(trace) > 0);
    /* First frame should have a span with non-zero id */
    ray_t* frame = ((ray_t**)ray_data(trace))[0];
    TEST_ASSERT_NOT_NULL(frame);
    TEST_ASSERT_EQ_I(ray_len(frame), 4);
    ray_t** fe = (ray_t**)ray_data(frame);
    TEST_ASSERT_NOT_NULL(fe[0]); /* span atom */
    TEST_ASSERT_TRUE(fe[0]->i64 != 0); /* non-zero span */
    ray_clear_error_trace();
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
 * Datalog: recursive rule (semi-naive fixpoint)
 * ═══════════════════════════════════════════════════════════════ */

static test_result_t test_datalog_fixpoint(void) {
    /* Build an EAV database: 1->2, 2->3, 3->4 */
    ray_t* db = ray_eval_str(
        "(do"
        "  (set db (datoms))"
        "  (set db (assert-fact db 1 'edge 2))"
        "  (set db (assert-fact db 2 'edge 3))"
        "  (set db (assert-fact db 3 'edge 4))"
        "  db)"
    );
    TEST_ASSERT_TRUE(db != NULL);
    TEST_ASSERT_TRUE(!RAY_IS_ERR(db));

    /* Define base rule: (rule (path ?x ?y) (?x :edge ?y)) */
    ray_t* r1 = ray_eval_str("(rule (path ?x ?y) (?x :edge ?y))");
    TEST_ASSERT_TRUE(r1 != NULL);
    TEST_ASSERT_TRUE(!RAY_IS_ERR(r1));
    ray_release(r1);

    /* Define recursive rule: (rule (path ?x ?z) (?x :edge ?y) (path ?y ?z)) */
    ray_t* r2 = ray_eval_str("(rule (path ?x ?z) (?x :edge ?y) (path ?y ?z))");
    TEST_ASSERT_TRUE(r2 != NULL);
    TEST_ASSERT_TRUE(!RAY_IS_ERR(r2));
    ray_release(r2);

    /* Query: find all reachable pairs */
    ray_t* result = ray_eval_str(
        "(do"
        "  (set db (datoms))"
        "  (set db (assert-fact db 1 'edge 2))"
        "  (set db (assert-fact db 2 'edge 3))"
        "  (set db (assert-fact db 3 'edge 4))"
        "  (query db (find ?x ?y) (where (path ?x ?y))))"
    );
    TEST_ASSERT_TRUE(result != NULL);
    if (RAY_IS_ERR(result)) {
        ray_t* es = ray_fmt(result, 0);
        fprintf(stderr, "  query error: %.*s\n",
                (int)(es ? ray_str_len(es) : 0), es ? ray_str_ptr(es) : "?");
        if (es) ray_release(es);
        FAIL("explicit MUNIT_FAIL");
    }
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    /* Expect 6 rows: 1->2, 2->3, 3->4, 1->3, 2->4, 1->4 */
    int64_t nrows = ray_table_nrows(result);
    TEST_ASSERT_EQ_I((int)nrows, 6);

    ray_release(result);
    ray_release(db);
    PASS();
}

static test_result_t test_datalog_query_inline_rules(void) {
    ray_t* r_inline = ray_eval_str(
        "(do"
        "  (set db (datoms))"
        "  (set db (assert-fact db 1 'edge 2))"
        "  (set db (assert-fact db 2 'edge 3))"
        "  (set db (assert-fact db 3 'edge 4))"
        "  (query db (find ?x ?y) (where (path ?x ?y))"
        "    (rules"
        "      ((path ?x ?y) (?x :edge ?y))"
        "      ((path ?x ?z) (?x :edge ?y) (path ?y ?z)))))"
    );
    TEST_ASSERT_TRUE(r_inline != NULL);
    TEST_ASSERT_TRUE(!RAY_IS_ERR(r_inline));
    TEST_ASSERT_EQ_I(r_inline->type, RAY_TABLE);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(r_inline), 6);
    ray_release(r_inline);

    /* Global foo rule — inline rules omit it; foo yields no rows */
    ray_t* r_foo = ray_eval_str(
        "(do"
        "  (set db (datoms))"
        "  (set db (assert-fact db 1 'edge 2))"
        "  (rule (foo ?x) (?x :edge 2))"
        "  (query db (find ?x) (where (foo ?x))"
        "    (rules ((path ?x ?y) (?x :edge ?y)))))"
    );
    TEST_ASSERT_TRUE(r_foo != NULL);
    TEST_ASSERT_TRUE(!RAY_IS_ERR(r_foo));
    TEST_ASSERT_EQ_I((int)ray_table_nrows(r_foo), 0);
    ray_release(r_foo);

    ray_t* r_global = ray_eval_str(
        "(do"
        "  (set db (datoms))"
        "  (set db (assert-fact db 1 'edge 2))"
        "  (rule (foo ?x) (?x :edge 2))"
        "  (query db (find ?x) (where (foo ?x))))"
    );
    TEST_ASSERT_TRUE(r_global != NULL);
    TEST_ASSERT_TRUE(!RAY_IS_ERR(r_global));
    TEST_ASSERT_EQ_I((int)ray_table_nrows(r_global), 1);
    ray_release(r_global);

    PASS();
}

/* ═══════════════════════════════════════════════════════════════
 * Ported rayforce lang tests (41 functions, ~3800 assertions)
 * ═══════════════════════════════════════════════════════════════ */

/* Null bitmap propagation regression tests.
 * Verify that nulls survive through insert, left-join, select (head/tail/filter),
 * and group-by operations. */
static test_result_t test_rf_null_propagate(void) {
    /* INSERT preserves nulls in existing columns */
    ray_eval_str("(set __np_t (table [x] (list [1 0Nl 3])))");
    ray_eval_str("(set __np_t2 (insert __np_t (list 4)))");
    ASSERT_EQ("(at (at __np_t2 'x) 0)", "1");
    ASSERT_EQ("(at (at __np_t2 'x) 1)", "0Nl");
    ASSERT_EQ("(at (at __np_t2 'x) 2)", "3");
    ASSERT_EQ("(at (at __np_t2 'x) 3)", "4");

    /* LEFT-JOIN produces nulls for unmatched right rows */
    ray_eval_str("(set __np_l (table [k v] (list [1 2 3] [10 20 30])))");
    ray_eval_str("(set __np_r (table [k w] (list [2] [200])))");
    ray_eval_str("(set __np_j (left-join [k] __np_l __np_r))");
    ASSERT_EQ("(at (at __np_j 'w) 0)", "0Nl");
    ASSERT_EQ("(at (at __np_j 'w) 1)", "200");
    ASSERT_EQ("(at (at __np_j 'w) 2)", "0Nl");

    /* LEFT-JOIN preserves source nulls from left table */
    ray_eval_str("(set __np_l2 (table [k v] (list [1 2] [10 0Nl])))");
    ray_eval_str("(set __np_r2 (table [k w] (list [1 2] [100 200])))");
    ray_eval_str("(set __np_j2 (left-join [k] __np_l2 __np_r2))");
    ASSERT_EQ("(at (at __np_j2 'v) 1)", "0Nl");
    ASSERT_EQ("(at (at __np_j2 'w) 0)", "100");

    /* SELECT with filter preserves nulls (WHERE) */
    ray_eval_str("(set __np_ft (table [a b] (list [1 2 3] [10 0Nl 30])))");
    ASSERT_EQ(
        "(at (at (select {from: __np_ft where: (> a 1)}) 'b) 0)",
        "0Nl");
    ASSERT_EQ(
        "(at (at (select {from: __np_ft where: (> a 1)}) 'b) 1)",
        "30");

    PASS();
}

/* ---- Dotted-name (namespace) resolution -------------------------------- */

static test_result_t test_dotted_write_read(void) {
    ray_eval_str("(set math.pi 3.14)");
    ASSERT_EQ("math.pi", "3.14");
    /* The auto-created parent is a plain dict */
    ASSERT_EQ("math", "{pi: 3.14}");
    PASS();
}

static test_result_t test_dotted_multi_key(void) {
    ray_eval_str("(set math2.pi 3.14)");
    ray_eval_str("(set math2.e  2.71)");
    ASSERT_EQ("math2.pi", "3.14");
    ASSERT_EQ("math2.e",  "2.71");
    PASS();
}

static test_result_t test_dotted_nested(void) {
    ray_eval_str("(set cfg.db.host 1)");
    ray_eval_str("(set cfg.db.port 2)");
    ASSERT_EQ("cfg.db.host", "1");
    ASSERT_EQ("cfg.db.port", "2");
    /* Deep create */
    ray_eval_str("(set a.b.c.d 42)");
    ASSERT_EQ("a.b.c.d", "42");
    PASS();
}

static test_result_t test_dotted_update_in_place(void) {
    ray_eval_str("(set ns.pi 3.14)");
    ray_eval_str("(set ns.e  2.71)");
    ray_eval_str("(set ns.pi 3.14159)");   /* overwrite existing key */
    ASSERT_EQ("ns.pi", "3.14159");
    ASSERT_EQ("ns.e",  "2.71");           /* sibling preserved */
    PASS();
}

static test_result_t test_dotted_wrong_type_parent(void) {
    ray_eval_str("(set xleaf 5)");
    /* Writing through a non-dict parent is a type error, not a silent override */
    ray_t* r = ray_eval_str("(set xleaf.y 1)");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    PASS();
}

static test_result_t test_dotted_missing_key(void) {
    ray_eval_str("(set only.pi 3.14)");
    /* Reading a missing key under a real dict reports 'undefined' */
    ray_t* r = ray_eval_str("only.missing");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    PASS();
}

static test_result_t test_dotted_del_removes_key(void) {
    ray_eval_str("(set __dns.x 5)");
    ray_eval_str("(set __dns.y 10)");
    ASSERT_EQ("__dns.x", "5");
    ASSERT_EQ("__dns.y", "10");

    /* del must REMOVE the key, not leave a zombie entry. */
    ray_eval_str("(del __dns.x)");
    /* Sibling survives; removed key reports undefined on read. */
    ASSERT_EQ("__dns.y", "10");
    ray_t* r = ray_eval_str("__dns.x");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    /* The dict's cardinality reflects the removal (was 2 keys, now 1). */
    ASSERT_EQ("(count __dns)", "1");

    /* del on a missing leaf is a no-op (not an error). */
    ray_t* r2 = ray_eval_str("(del __dns.never_existed)");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    ASSERT_EQ("(count __dns)", "1");

    /* del on a missing head is a no-op too. */
    ray_t* r3 = ray_eval_str("(del __missing.leaf)");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r3));
    PASS();
}

static test_result_t test_dotted_del_nested(void) {
    ray_eval_str("(set __abc.b.c 1)");
    ray_eval_str("(set __abc.b.d 2)");
    ray_eval_str("(del __abc.b.c)");
    /* Sibling preserved, deleted key gone, intermediate dict still reachable. */
    ASSERT_EQ("__abc.b.d", "2");
    ASSERT_EQ("(count __abc.b)", "1");
    ray_t* r = ray_eval_str("__abc.b.c");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    PASS();
}

static test_result_t test_dotted_del_cascade(void) {
    /* Deleting the only key in a namespace removes the namespace itself. */
    ray_eval_str("(set __cascns.only 5)");
    ASSERT_EQ("__cascns.only", "5");
    ray_eval_str("(del __cascns.only)");
    ray_t* r = ray_eval_str("__cascns");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));   /* namespace gone, not left as {} */

    /* Deep cascade: a.b.c.d is the only key along the whole chain; deleting
     * it should remove `a` entirely (no stale empty `a` / `a.b` / `a.b.c`
     * bindings left behind). */
    ray_eval_str("(set __deep.b.c.d 1)");
    ray_eval_str("(del __deep.b.c.d)");
    r = ray_eval_str("__deep");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));

    /* Cascade stops at a level that still has siblings. */
    ray_eval_str("(set __mix.b.c.d 1)");
    ray_eval_str("(set __mix.other 2)");
    ray_eval_str("(del __mix.b.c.d)");
    ASSERT_EQ("__mix.other", "2");
    ASSERT_EQ("(count __mix)", "1");    /* `b` chain cascaded out */
    r = ray_eval_str("__mix.b");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));   /* empty `b` should not remain */

    PASS();
}

static test_result_t test_select_by_nullable_f64_key(void) {
    /* Nullable F64 key column: without null-awareness the new hash-based
     * first-idx path collided the null group with the 0.0 group — F64
     * null's bit pattern is -0.0, and ray_hash_f64 normalises -0.0 to
     * +0.0, so hash(null) == hash(0.0) and the null group got a stale
     * first_idx = -1.  The indices must point to the actual first row
     * with that Price value (or first null row). */
    ray_eval_str("(set __nv (table [OrderId Price] (list (til 5) [0.0 0Nf 0.0 0Nf 1.0])))");
    ray_eval_str("(set __ng (select {from: __nv by: Price}))");
    ASSERT_EQ("(count __ng)", "3");
    /* Each group's OrderId must be the first row index where that Price
     * (or null) appears: 0.0→row0, null→row1, 1.0→row4. */
    ASSERT_EQ("(at (at __ng 'OrderId) 0)", "0");
    ASSERT_EQ("(at (at __ng 'OrderId) 1)", "1");
    ASSERT_EQ("(at (at __ng 'OrderId) 2)", "4");
    /* The grouped Price column must preserve the null bit for the null
     * group — not silently collapse to 0.0.  store_typed_elem routes
     * nulls through ray_vec_set_null which range-checks against vec->len,
     * so the destination vec's len must be set before the store loop
     * populates it.  Previously len was assigned after the loop and the
     * null bit was dropped. */
    ASSERT_EQ("(nil? (at (at __ng 'Price) 0))", "false");
    ASSERT_EQ("(nil? (at (at __ng 'Price) 1))", "true");
    ASSERT_EQ("(nil? (at (at __ng 'Price) 2))", "false");
    PASS();
}

static test_result_t test_select_by_computed_key_nullable_nonkey(void) {
    /* Computed key (by: (expr)) takes a third result-build path (lines
     * around query.c:2120 — ray_group_fn on the computed vector + scatter
     * non-key columns).  Same hazard as the other sites: dc->len wasn't
     * set before store_typed_elem, so nullable non-key columns silently
     * dropped their null bit.
     *
     * Use an I64 computed key (`(mod Qty 2)`) so ray_group_fn's scalar
     * hash path actually merges duplicate buckets — F64 output would fall
     * back to "every row is its own group" which masks every grouping
     * bug and lets a broken test pass trivially.  Setup forces:
     *   - 5 source rows collapse into 2 groups (bucket 1: rows 0,2,4;
     *     bucket 0: rows 1,3);
     *   - first-of-group Price for bucket 0 is row 1 which is NULL, so
     *     the null bit must propagate into the scattered Price column;
     *   - first-of-group Price for bucket 1 is row 0 = 10.0, checked so
     *     the fix isn't hiding a cross-slot data corruption. */
    ray_eval_str("(set __tc (table [Qty Price] (list [1 2 3 4 5] [10.0 0Nf 15.0 20.0 25.0])))");
    ray_eval_str("(set __cg (select {from: __tc by: (% Qty 2)}))");
    ASSERT_EQ("(count __cg)", "2");                         /* duplicates merged */
    ASSERT_EQ("(nil? (at (at __cg 'Price) 0))", "false");   /* bucket 1 — row 0 = 10.0 */
    ASSERT_EQ("(nil? (at (at __cg 'Price) 1))", "true");    /* bucket 0 — row 1 = null */
    ASSERT_EQ("(+ 0.0 (at (at __cg 'Price) 0))", "10.0");   /* value preserved correctly */
    PASS();
}

static test_result_t test_select_by_str_nullable_nonkey(void) {
    /* STR-keyed by-grouping routes through the eval_group fallback
     * (ray_group_fn → gather first-of-group), which is a separate result-
     * build site from the scalar-key DAG path.  Same hazard though: the
     * non-key column's destination vec was filled via store_typed_elem
     * before its len was set, so nullable non-key columns lost their
     * null bits on output. */
    ray_eval_str("(set __ts (table [Tag Price] (list (as 'STR [\"a\" \"b\" \"a\"]) [1.0 0Nf 3.0])))");
    ray_eval_str("(set __sg (select {from: __ts by: Tag}))");
    ASSERT_EQ("(count __sg)", "2");
    /* Group \"a\" covers rows 0,2 (first-of-group → row 0, Price=1.0).
     * Group \"b\" covers row 1 (Price=null).  The null bit on the
     * resulting Price column must be preserved. */
    ASSERT_EQ("(nil? (at (at __sg 'Price) 0))", "false");
    ASSERT_EQ("(nil? (at (at __sg 'Price) 1))", "true");
    PASS();
}

static test_result_t test_select_by_nullable_i64_key(void) {
    /* Nullable I64 key where a real 0 coexists with nulls — classic
     * collision scenario for raw-bit hashing of null sentinels. */
    ray_eval_str("(set __ni (table [Ord Key] (list (til 5) [0 0Nl 0 0Nl 7])))");
    ray_eval_str("(set __ng (select {from: __ni by: Key}))");
    ASSERT_EQ("(count __ng)", "3");
    ASSERT_EQ("(at (at __ng 'Ord) 0)", "0");
    ASSERT_EQ("(at (at __ng 'Ord) 1)", "1");
    ASSERT_EQ("(at (at __ng 'Ord) 2)", "4");
    PASS();
}

static test_result_t test_select_by_narrow_int_key(void) {
    /* Non-F64 scalar integer columns (I32, I16, I8, BOOL, narrow SYM) hit
     * the same no-agg first-idx path.  The previous code read the key via
     * ray_read_sym, which interprets attrs as SYM adaptive width and
     * silently truncates to 1 byte for plain int columns — so I32 keys
     * above 255 were wrapped mod 256 before hashing/comparison and the
     * result table's key column held bogus values.  Group keys > 255 here
     * forces any regression to truncate and misgroup. */
    ray_eval_str("(set __niX (table [Age] (list (as 'I32 [300 700 300 700 1200]))))");
    ray_t* g = ray_eval_str("(select {from: __niX by: Age})");
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_FALSE(RAY_IS_ERR(g));
    ray_release(g);

    ASSERT_EQ("(count (select {from: __niX by: Age}))", "3");
    /* Force the I32 atoms to print as integers so we don't depend on
     * I32-atom format string rendering. */
    ASSERT_EQ("(+ 0 (at (at (select {from: __niX by: Age}) 'Age) 0))", "300");
    ASSERT_EQ("(+ 0 (at (at (select {from: __niX by: Age}) 'Age) 1))", "700");
    ASSERT_EQ("(+ 0 (at (at (select {from: __niX by: Age}) 'Age) 2))", "1200");

    /* Same idea with RAY_I16. */
    ray_eval_str("(set __niY (table [Year] (list (as 'I16 [2020 2024 2020 2024]))))");
    ASSERT_EQ("(count (select {from: __niY by: Year}))", "2");
    ASSERT_EQ("(+ 0 (at (at (select {from: __niY by: Year}) 'Year) 0))", "2020");
    ASSERT_EQ("(+ 0 (at (at (select {from: __niY by: Year}) 'Year) 1))", "2024");

    PASS();
}

static test_result_t test_select_by_f64_perf(void) {
    /* Build a table with N rows where the F64 key column has N unique values
     * (0.0, 1.0, 2.0, ...), so every row ends up in its own group.  The old
     * first-of-group scan was O(N * n_groups) for non-GUID keys — quadratic
     * on this shape — and hung for tens of seconds at N=200k.  With the
     * hash-based first-idx path it should finish near-instantly.  The
     * functional check is that each group has its matching OrderId. */
    ray_eval_str("(set __sbn 2000)");
    ray_eval_str("(set __sbf (table [OrderId Price] (list (til __sbn) (as 'F64 (til __sbn)))))");
    ray_t* g = ray_eval_str("(select {from: __sbf by: Price})");
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_FALSE(RAY_IS_ERR(g));
    ray_release(g);
    /* Every row is its own group. */
    ASSERT_EQ("(count (select {from: __sbf by: Price}))", "2000");
    /* Spot-check first-of-group matches OrderId at that row. */
    ASSERT_EQ("(at (at (select {from: __sbf by: Price}) 'OrderId) 0)",    "0");
    ASSERT_EQ("(at (at (select {from: __sbf by: Price}) 'OrderId) 1999)", "1999");
    PASS();
}

static test_result_t test_dotted_temporal_atom(void) {
    /* DATE atom: dotted field access maps to temporal component extract.
     * The value is days since 2000-01-01, so 10000 lands in 2027-05-19
     * via Hinnant's civil_from_days decomposition. */
    ray_eval_str("(set __dt (as 'DATE 10000))");
    ASSERT_EQ("__dt.yyyy", "2027");
    ASSERT_EQ("__dt.mm",   "5");
    ASSERT_EQ("__dt.dd",   "19");

    /* TIMESTAMP atom: 1 hour + 1 minute + 1 second in nanoseconds
     * (RAY_TIMESTAMP's native unit — matches io/csv.c). */
    ray_eval_str("(set __ts (as 'TIMESTAMP 3661000000000))");
    ASSERT_EQ("__ts.hh",     "1");
    ASSERT_EQ("__ts.minute", "1");
    ASSERT_EQ("__ts.ss",     "1");
    PASS();
}

static test_result_t test_dotted_temporal_truncate_atom(void) {
    /* `.date` / `.time` truncate a temporal value to day / second
     * boundary, returning RAY_TIMESTAMP.  Atom path: 90,000,000,000,000
     * ns = 1 day + 1 hour; truncating to day gives exactly 1 day =
     * 86,400 s * 1e9 ns = 86,400,000,000,000; truncating to second
     * leaves the value unchanged (no sub-second remainder here). */
    ray_eval_str("(set __tsa (as 'TIMESTAMP 90000000000000))");
    ASSERT_EQ("(as 'I64 __tsa.date)", "86400000000000");
    ASSERT_EQ("(as 'I64 __tsa.time)", "90000000000000");
    PASS();
}

static test_result_t test_select_nonagg_dotted_temporal(void) {
    /* Non-agg output expression using a dotted-temporal ref — e.g.
     * `s: Timestamp.ss` — must flow through the scatter path the same
     * way a plain column ref would.  Previously expr_bind_table_names
     * checked the whole dotted sym against the table schema, found no
     * column named "Timestamp.ss", and silently skipped binding — so
     * the subsequent ray_eval fell off the env and reported undefined. */
    ray_eval_str(
        "(set __sy (table [Sym Ts] "
        "(list ['A 'B 'A 'B 'A] "
        "      (as 'TIMESTAMP [1000000000 2000000000 3000000000 4000000000 5000000000]))))");
    ray_t* r = ray_eval_str("(select {from: __sy by: Sym s: Ts.ss})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    /* 2 groups (A, B).  Each group's `s` list contains the extracted
     * seconds for its rows: both A rows land in seconds 1/3/5, B in
     * 2/4 — all within second 0..5 of 2000-01-01 so ss = whole-second
     * index.  Spot-check lengths match source-row counts (3 and 2). */
    ASSERT_EQ("(count (select {from: __sy by: Sym s: Ts.ss}))", "2");
    PASS();
}

static test_result_t test_select_by_computed_key_many_groups(void) {
    /* Regression: the computed-key fallback used a `fi2[256]` stack
     * array and silently capped the first-index sweep at 256 groups;
     * the downstream result-column loops still iterated up to ng2, so
     * any group beyond 256 read uninitialised stack memory — UB under
     * ASan, silent corruption otherwise.  500 distinct days is enough
     * to trigger both the heap fallback and the past-256 range. */
    ray_eval_str("(set __mn 500)");
    ray_eval_str(
        "(set __mt (table [Ts Price] "
        "(list (as 'TIMESTAMP (* (til __mn) 86400000000000)) "
        "      (as 'F64 (til __mn)))))");
    ray_t* g = ray_eval_str("(select {from: __mt by: Ts.date})");
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_FALSE(RAY_IS_ERR(g));
    ray_release(g);
    ASSERT_EQ("(count (select {from: __mt by: Ts.date}))", "500");
    /* Each group has exactly one source row, so the non-key column's
     * first-of-group value at position gi must equal gi. */
    ASSERT_EQ("(+ 0.0 (at (at (select {from: __mt by: Ts.date}) 'Price) 499))", "499.0");
    PASS();
}

static test_result_t test_select_by_dotted_key_surfaces_key_col(void) {
    /* Regression: (select {from: t by: <computed key>}) with no aggs
     * used to produce a result missing the grouping-key column — the
     * computed-key fallback defaulted ckey_name to "+", looked that up
     * in the source schema, found nothing, and fell through to only
     * first-of-group source columns.  Users saw their grouped rows
     * but with no column reporting the *key* that defined each group.
     *
     * Fix: derive a column name from the by-expression (tail segment
     * of a dotted sym; last name arg of a call) and populate it from
     * the computed_key values at first-of-group indices. */
    ray_eval_str(
        "(set __sb (table [OrderId Timestamp] "
        "(list [1 2 3 4 5 6] "
        "      (as 'TIMESTAMP [100000000 200000000 1100000000 1200000000 2100000000 2200000000]))))");
    ray_t* r = ray_eval_str("(select {from: __sb by: Timestamp.ss})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    /* 3 groups (ss = 0, 1, 2), 3 columns (ss key + 2 source). */
    ASSERT_EQ("(count (select {from: __sb by: Timestamp.ss}))", "3");
    ASSERT_EQ("(at (at (select {from: __sb by: Timestamp.ss}) 'ss) 0)", "0");
    ASSERT_EQ("(at (at (select {from: __sb by: Timestamp.ss}) 'ss) 1)", "1");
    ASSERT_EQ("(at (at (select {from: __sb by: Timestamp.ss}) 'ss) 2)", "2");
    /* OrderId column carries first-of-group source values. */
    ASSERT_EQ("(at (at (select {from: __sb by: Timestamp.ss}) 'OrderId) 0)", "1");
    ASSERT_EQ("(at (at (select {from: __sb by: Timestamp.ss}) 'OrderId) 1)", "3");
    ASSERT_EQ("(at (at (select {from: __sb by: Timestamp.ss}) 'OrderId) 2)", "5");
    PASS();
}

static test_result_t test_select_by_dotted_key_name_collision(void) {
    /* Regression: when the dotted tail segment name matched a source
     * column that wasn't the head of the dotted expression, the old
     * code silently dropped that column from the result and the
     * method-dispatch lookup during scattered eval found the
     * (non-callable) local binding instead of the global accessor —
     * so `Timestamp.ss` first became "undefined" and, once that was
     * fixed, it still replaced the real `ss` column's data.
     *
     * Fix:
     *   - env.c: method dispatch inside a dotted walk only considers
     *     RAY_UNARY bindings when it re-scans the scope/global envs,
     *     so a local column named `ss` no longer shadows the global
     *     accessor function.
     *   - query.c: when the dotted tail collides with an unrelated
     *     source column, promote the key column name to the full
     *     dotted sym so the source column stays intact. */
    ray_eval_str(
        "(set __sc (table [Timestamp ss] "
        "(list (as 'TIMESTAMP [100000000 1100000000 2100000000]) "
        "      ['a 'b 'c])))");
    ray_t* r = ray_eval_str("(select {from: __sc by: Timestamp.ss})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    /* 3 groups — key column promoted to full dotted name, source ss
     * preserved as a separate column with first-of-group values. */
    ASSERT_EQ("(count (select {from: __sc by: Timestamp.ss}))", "3");
    ASSERT_EQ("(at (at (select {from: __sc by: Timestamp.ss}) 'Timestamp.ss) 0)", "0");
    ASSERT_EQ("(at (at (select {from: __sc by: Timestamp.ss}) 'Timestamp.ss) 2)", "2");
    /* Source ss column survives, carrying its first-of-group SYM values.
     * Compare via sym-literal RHS ('a / 'b / 'c) so the assertion's
     * own eval_str finds a real sym atom to format. */
    ASSERT_EQ("(at (at (select {from: __sc by: Timestamp.ss}) 'ss) 0)", "'a");
    ASSERT_EQ("(at (at (select {from: __sc by: Timestamp.ss}) 'ss) 1)", "'b");
    ASSERT_EQ("(at (at (select {from: __sc by: Timestamp.ss}) 'ss) 2)", "'c");
    PASS();
}

static test_result_t test_atomic_map_empty_sym_str_compare(void) {
    /* Regression: zero_atom_for_elem_type fell back to ray_i64(0) for
     * RAY_SYM / RAY_STR / RAY_GUID element types, so `(== empty_sym
     * 'foo)` probed an integer comparison and surfaced an empty I64
     * vector — a non-empty input would have produced BOOL.  Callers
     * branching on the output type (e.g. filter builders, DAG type
     * inference) saw disagreeing schemas for the same expression. */
    ASSERT_EQ("(type (== (as 'SYM []) 'foo))",   "'B8");
    ASSERT_EQ("(type (== (as 'STR []) \"a\"))",  "'B8");
    ASSERT_EQ("(type (== (as 'SYM [foo]) 'foo))","'B8");
    ASSERT_EQ("(type (== (as 'STR [\"a\"]) \"a\"))", "'B8");
    /* GUID comparison path — same principle. */
    ASSERT_EQ("(type (!= (as 'GUID []) (as 'GUID \"00000000-0000-0000-0000-000000000000\")))",
              "'B8");
    PASS();
}

static test_result_t test_select_by_xbar_empty_type(void) {
    /* Regression: atomic_map_{unary,binary}_op returned a hardcoded I64
     * empty vector when the input collection had zero elements — which
     * then propagated to the empty-result key-column type.  For
     * `(xbar TIME_col N)` on an empty table, the key column surfaced
     * as I64 instead of TIME, so the empty schema disagreed with the
     * non-empty one (and the source Ts column's type). */
    ray_eval_str("(set __xe (table [Sym Ts] (list (as 'SYM []) (as 'TIME []))))");
    ray_t* r = ray_eval_str("(select {from: __xe by: (xbar Ts 10000)})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    ASSERT_EQ("(count (select {from: __xe by: (xbar Ts 10000)}))", "0");
    ASSERT_EQ("(type (at (select {from: __xe by: (xbar Ts 10000)}) 'Ts))", "'TIME");

    /* Unary path: dotted `.date` truncate should produce TIMESTAMP on
     * an empty TIMESTAMP column, not I64 (the tail accessor `.date`
     * returns TIMESTAMP per ray_temporal_truncate). */
    ray_eval_str("(set __xe2 (table [Timestamp] (list (as 'TIMESTAMP []))))");
    ASSERT_EQ("(type (at (select {from: __xe2 by: Timestamp.date}) 'date))", "'TIMESTAMP");
    PASS();
}

static test_result_t test_select_by_dotted_key_empty_schema(void) {
    /* Regression: when `select by <dotted>` produced zero groups
     * (empty source, or all rows filtered out), the empty-result
     * schema only copied source columns — the key column was dropped
     * because the old empty-path pulled the key from key_sym, which
     * is -1 for computed keys.  The non-empty path surfaces an `ss`
     * column, so the empty path must too, or client code branching on
     * the result schema misreports the column set. */
    ray_eval_str("(set __se (table [Timestamp Price] (list (as 'TIMESTAMP []) [])))");
    ray_t* r = ray_eval_str("(select {from: __se by: Timestamp.ss})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    ASSERT_EQ("(count (select {from: __se by: Timestamp.ss}))", "0");
    /* Probe each expected column: `at` on a table returns the column
     * vector when the column exists (length 0 here), and errors
     * otherwise.  The empty schema must include the `ss` key column
     * produced by the dotted expression plus both source columns. */
    ASSERT_EQ("(count (at (select {from: __se by: Timestamp.ss}) 'ss))", "0");
    ASSERT_EQ("(type (at (select {from: __se by: Timestamp.ss}) 'ss))", "'I64");
    ASSERT_EQ("(count (at (select {from: __se by: Timestamp.ss}) 'Timestamp))", "0");
    ASSERT_EQ("(count (at (select {from: __se by: Timestamp.ss}) 'Price))", "0");
    PASS();
}

static test_result_t test_select_by_dotted_temporal_key_nocrash(void) {
    /* Regression: (select {from: t by: Timestamp.date}) used to crash
     * in group_rows_range because the dotted sym was emitted as a scan
     * of the non-existent column "Timestamp.date".  After the fix,
     * either compile-time desugars to scan+trunc, or (runtime path)
     * the computed-key fallback runs eval + truncate.  Here we assert
     * the query completes cleanly and collapses 5 rows spanning 3 days
     * into 3 groups. */
    ray_eval_str(
        "(set __tb (table [Timestamp Price] "
        "(list (as 'TIMESTAMP [0 3600000000000 86400000000000 90000000000000 172800000000000]) "
        "      [1.0 2.0 3.0 4.0 5.0])))");
    ray_t* r = ray_eval_str("(select {from: __tb by: Timestamp.date})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    ASSERT_EQ("(count (select {from: __tb by: Timestamp.date}))", "3");
    PASS();
}

static test_result_t test_dotted_temporal_vector(void) {
    /* DATE vector extraction returns an I64 vector of the same length. */
    ray_eval_str("(set __dv (as 'DATE [0 366 731]))");
    /* 2000-01-01, 2001-01-01, 2002-01-01 */
    ASSERT_EQ("(at __dv.yyyy 0)", "2000");
    ASSERT_EQ("(at __dv.yyyy 1)", "2001");
    ASSERT_EQ("(at __dv.yyyy 2)", "2002");
    ASSERT_EQ("(at __dv.mm 0)",   "1");
    ASSERT_EQ("(at __dv.dd 0)",   "1");
    PASS();
}

static test_result_t test_dag_temporal_extract_nulls(void) {
    /* Regression: DAG-path OP_EXTRACT / OP_DATE_TRUNC (emitted by
     * compile_expr_dag when a dotted-temporal sym is referenced by a
     * query) used to decode the raw null-sentinel bytes and emit bogus
     * I64 / timestamp values *without* setting the output null bit.
     * The null sentinel for RAY_TIMESTAMP is 0 (distinguished by the
     * nullmap bit, not the value), so the extract kernel silently
     * turned every null into `.ss == 0` and lost null-awareness for
     * all downstream consumers.
     *
     * `select {from: t s: Ts.ss}` with no `by:` goes through
     * compile_expr_dag → exec_extract and surfaces the I64 column
     * directly, so we can assert that the null bit travels all the
     * way to the final output column. */
    ray_eval_str("(set __dvn (as 'TIMESTAMP (list 1000000000 0Np 2000000000)))");
    ray_eval_str("(set __dvt (table [Ts] (list __dvn)))");

    /* Non-null rows pass through unchanged. */
    ASSERT_EQ("(at (at (select {from: __dvt s: Ts.ss}) 's) 0)", "1");
    ASSERT_EQ("(at (at (select {from: __dvt s: Ts.ss}) 's) 2)", "2");
    /* Null row must surface as 0Nl, not 0 — this is the regression. */
    ASSERT_EQ("(at (at (select {from: __dvt s: Ts.ss}) 's) 1)", "0Nl");

    /* OP_DATE_TRUNC path: `.date` truncates to day boundary and emits
     * a TIMESTAMP column.  Null row must stay 0Np. */
    ASSERT_EQ("(at (at (select {from: __dvt s: Ts.date}) 's) 1)", "0Np");
    PASS();
}

static test_result_t test_temporal_extract_slice_nulls(void) {
    /* Regression: HAS_NULLS lives on the storage owner, not on slice
     * views — `(input->attrs & RAY_ATTR_HAS_NULLS)` alone misses nulls
     * when `input` is a slice pointing at a nullable parent.  Mirrors
     * the slice-aware check used in sort.c / rerank.c / eval.c. */
    /* RAY_TIMESTAMP is ns since 2000-01-01; 1e9 ns = 1 second, so
     * raw[i] * 1e9 gives seconds i..5.  The extract kernel converts
     * ns → µs internally and returns whole-second indices. */
    int64_t raw[5] = {1000000000, 2000000000, 0, 4000000000, 5000000000};
    ray_t* v = ray_vec_from_raw(RAY_TIMESTAMP, raw, 5);
    TEST_ASSERT_NOT_NULL(v);
    ray_vec_set_null(v, 2, true);
    TEST_ASSERT_TRUE((v->attrs & RAY_ATTR_HAS_NULLS) != 0);
    TEST_ASSERT_TRUE(ray_vec_is_null(v, 2));

    /* Slice [1..4): {2s, null, 4s}.  Slice itself does not carry
     * HAS_NULLS; only the parent does. */
    ray_t* s = ray_vec_slice(v, 1, 3);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    TEST_ASSERT_TRUE((s->attrs & RAY_ATTR_SLICE) != 0);
    TEST_ASSERT_FALSE((s->attrs & RAY_ATTR_HAS_NULLS) != 0);
    TEST_ASSERT_TRUE(ray_vec_is_null(s, 1));

    /* Extract seconds.  Without slice-aware null detection this path
     * decoded raw=0 at index 1 and emitted `ss=0` with no null bit. */
    ray_t* ss = ray_temporal_extract(s, RAY_EXTRACT_SECOND);
    TEST_ASSERT_NOT_NULL(ss);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ss));
    TEST_ASSERT_EQ_I(ss->len, 3);
    TEST_ASSERT_TRUE(ray_vec_is_null(ss, 1));
    TEST_ASSERT_FALSE(ray_vec_is_null(ss, 0));
    TEST_ASSERT_FALSE(ray_vec_is_null(ss, 2));
    int64_t* out = (int64_t*)ray_data(ss);
    TEST_ASSERT_EQ_I(out[0], 2);
    TEST_ASSERT_EQ_I(out[2], 4);

    /* Same shape for truncate (.date / .time path). */
    ray_t* tr = ray_temporal_truncate(s, RAY_EXTRACT_DAY);
    TEST_ASSERT_NOT_NULL(tr);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tr));
    TEST_ASSERT_TRUE(ray_vec_is_null(tr, 1));

    ray_release(tr);
    ray_release(ss);
    ray_release(s);
    ray_release(v);
    PASS();
}

static test_result_t test_dotted_temporal_table_column(void) {
    /* t.col.field — chains through table → column → temporal extraction.
     * Exercises the three probe steps in ray_env_resolve's dotted walk
     * (scope/global for t, table column for Date, temporal extract for
     * yyyy / mm / dd). */
    ray_eval_str("(set __tt (table [Date Price] (list (as 'DATE [0 366 731]) [100.0 200.0 300.0])))");
    ASSERT_EQ("(at __tt.Date.yyyy 0)", "2000");
    ASSERT_EQ("(at __tt.Date.yyyy 2)", "2002");
    ASSERT_EQ("(at __tt.Date.mm 0)",   "1");

    /* Missing temporal field still reports undefined (matches every
     * other "nothing matched" dotted failure). */
    ray_t* r = ray_eval_str("__tt.Date.nope");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    PASS();
}

static test_result_t test_dotted_table_column(void) {
    ray_eval_str("(set __tbl (table [OrderId Price] (list [10 20 30] [1.0 2.0 3.0])))");
    /* t.col returns the column; it behaves as a vector downstream. */
    ASSERT_EQ("(at __tbl.OrderId 0)", "10");
    ASSERT_EQ("(at __tbl.OrderId 2)", "30");
    ASSERT_EQ("(at __tbl.Price 1)",   "2.0");
    ASSERT_EQ("(sum __tbl.Price)",    "6.0");
    /* Missing column surfaces as 'undefined' (same as a missing dict key). */
    ray_t* r = ray_eval_str("__tbl.NotAColumn");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    PASS();
}

/* ─── ops/builtins.c entry-point coverage ─────────────────────────── */

/* Mute stdout so the print/show output doesn't pollute test runner output. */
static int builtins_mute_stdout(void) {
    fflush(stdout);
    int saved = dup(fileno(stdout));
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) { dup2(devnull, fileno(stdout)); close(devnull); }
    return saved;
}
static void builtins_restore_stdout(int saved) {
    fflush(stdout);
    if (saved >= 0) { dup2(saved, fileno(stdout)); close(saved); }
}

/* (print v1 v2 ...) — variadic stdout writer; returns RAY_NULL_OBJ. */
static test_result_t test_builtin_print_fn(void) {
    ray_t* a = ray_i64(7);
    ray_t* b = ray_str("hello", 5);
    ray_t* args[2] = { a, b };

    int saved = builtins_mute_stdout();
    ray_t* r = ray_print_fn(args, 2);
    builtins_restore_stdout(saved);

    TEST_ASSERT_EQ_PTR(r, RAY_NULL_OBJ);

    /* Format-string mode (first arg is a -RAY_STR with % placeholders). */
    ray_t* fmt = ray_str("v=%", 3);
    ray_t* x   = ray_i64(42);
    ray_t* fa[2] = { fmt, x };
    saved = builtins_mute_stdout();
    r = ray_print_fn(fa, 2);
    builtins_restore_stdout(saved);
    TEST_ASSERT_EQ_PTR(r, RAY_NULL_OBJ);

    ray_release(a);
    ray_release(b);
    ray_release(fmt);
    ray_release(x);
    PASS();
}

/* (show v1 v2 ...) — formats via ray_fmt, newline at end. */
static test_result_t test_builtin_show_fn(void) {
    ray_t* a = ray_i64(99);
    ray_t* b = ray_str("k", 1);
    ray_t* args[2] = { a, b };

    int saved = builtins_mute_stdout();
    ray_t* r = ray_show_fn(args, 2);
    builtins_restore_stdout(saved);

    TEST_ASSERT_EQ_PTR(r, RAY_NULL_OBJ);

    ray_release(a);
    ray_release(b);
    PASS();
}

/* (timeit expr) — returns elapsed ms as F64. */
static test_result_t test_builtin_timeit_fn(void) {
    /* timeit calls ray_eval(args[0]) — pass a parsed expression. */
    ray_t* expr = ray_parse("(+ 1 2)");
    TEST_ASSERT_NOT_NULL(expr);
    TEST_ASSERT_FALSE(RAY_IS_ERR(expr));

    ray_t* args[1] = { expr };
    int saved = builtins_mute_stdout();
    ray_t* r = ray_timeit_fn(args, 1);
    builtins_restore_stdout(saved);

    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, -RAY_F64);

    /* Domain error path: n < 1. */
    ray_t* re = ray_timeit_fn(NULL, 0);
    TEST_ASSERT_TRUE(RAY_IS_ERR(re));

    ray_release(re);
    ray_release(r);
    ray_release(expr);
    PASS();
}

/* (load path) — read+eval a Rayforce script file. */
static test_result_t test_builtin_load_file_fn(void) {
    char path[64];
    snprintf(path, sizeof(path), "/tmp/ray_test_load_%d.rfl", (int)getpid());
    FILE* fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp);
    fputs("(+ 5 7)\n", fp);
    fclose(fp);

    ray_t* p = ray_str(path, strlen(path));
    int saved = builtins_mute_stdout();
    ray_t* r = ray_load_file_fn(p);
    builtins_restore_stdout(saved);

    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, -RAY_I64);
    TEST_ASSERT_EQ_I(r->i64, 12);

    /* Wrong-type path. */
    ray_t* bad = ray_i64(5);
    ray_t* re  = ray_load_file_fn(bad);
    TEST_ASSERT_TRUE(RAY_IS_ERR(re));

    /* I/O error path (nonexistent file). */
    ray_t* nope = ray_str("/tmp/ray_test_load_nope_xyz.rfl", 31);
    ray_t* re2  = ray_load_file_fn(nope);
    TEST_ASSERT_TRUE(RAY_IS_ERR(re2));

    unlink(path);
    ray_release(re2);
    ray_release(nope);
    ray_release(re);
    ray_release(bad);
    ray_release(r);
    ray_release(p);
    PASS();
}

/* (write path content) — write a string to a file. */
static test_result_t test_builtin_write_file_fn(void) {
    char path[64];
    snprintf(path, sizeof(path), "/tmp/ray_test_write_%d.txt", (int)getpid());
    ray_t* p = ray_str(path, strlen(path));
    ray_t* c = ray_str("hello world", 11);

    ray_t* r = ray_write_file_fn(p, c);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, -RAY_I64);

    /* Verify file contents. */
    FILE* fp = fopen(path, "r");
    TEST_ASSERT_NOT_NULL(fp);
    char buf[32] = {0};
    size_t rd = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    TEST_ASSERT_EQ_U(rd, 11);
    TEST_ASSERT_TRUE(memcmp(buf, "hello world", 11) == 0);

    /* Wrong-type paths. */
    ray_t* bad_path = ray_i64(0);
    ray_t* re1 = ray_write_file_fn(bad_path, c);
    TEST_ASSERT_TRUE(RAY_IS_ERR(re1));
    ray_t* bad_content = ray_i64(0);
    ray_t* re2 = ray_write_file_fn(p, bad_content);
    TEST_ASSERT_TRUE(RAY_IS_ERR(re2));

    unlink(path);
    ray_release(re2);
    ray_release(bad_content);
    ray_release(re1);
    ray_release(bad_path);
    ray_release(r);
    ray_release(c);
    ray_release(p);
    PASS();
}

const test_entry_t lang_entries[] = {
    { "lang/fn_unary", test_fn_unary, lang_setup, lang_teardown },
    { "lang/fn_binary", test_fn_binary, lang_setup, lang_teardown },
    { "lang/fn_vary", test_fn_vary, lang_setup, lang_teardown },
    { "lang/lex/i64", test_lex_i64, lang_setup, lang_teardown },
    { "lang/lex/neg_i64", test_lex_neg_i64, lang_setup, lang_teardown },
    { "lang/lex/f64", test_lex_f64, lang_setup, lang_teardown },
    { "lang/lex/string", test_lex_string, lang_setup, lang_teardown },
    { "lang/lex/symbol", test_lex_symbol, lang_setup, lang_teardown },
    { "lang/lex/bool", test_lex_bool, lang_setup, lang_teardown },
    { "lang/parse/sexpr", test_parse_sexpr, lang_setup, lang_teardown },
    { "lang/parse/nested", test_parse_nested, lang_setup, lang_teardown },
    { "lang/parse/vector", test_parse_vector, lang_setup, lang_teardown },
    { "lang/parse/empty_list", test_parse_empty_list, lang_setup, lang_teardown },
    { "lang/eval/literal", test_eval_literal, lang_setup, lang_teardown },
    { "lang/eval/add", test_eval_add, lang_setup, lang_teardown },
    { "lang/eval/nested_arith", test_eval_nested_arith, lang_setup, lang_teardown },
    { "lang/eval/sub", test_eval_sub, lang_setup, lang_teardown },
    { "lang/eval/div", test_eval_div, lang_setup, lang_teardown },
    { "lang/eval/cmp", test_eval_cmp, lang_setup, lang_teardown },
    { "lang/eval/set", test_eval_set, lang_setup, lang_teardown },
    { "lang/eval/if_true", test_eval_if_true, lang_setup, lang_teardown },
    { "lang/eval/if_false", test_eval_if_false, lang_setup, lang_teardown },
    { "lang/eval/let", test_eval_let, lang_setup, lang_teardown },
    { "lang/eval/lambda", test_eval_lambda, lang_setup, lang_teardown },
    { "lang/eval/lambda_multi", test_eval_lambda_multi, lang_setup, lang_teardown },
    { "lang/eval/lambda_let", test_eval_lambda_let, lang_setup, lang_teardown },
    { "lang/compile/basic", test_compile_basic, lang_setup, lang_teardown },
    { "lang/compile/closure", test_compile_closure, lang_setup, lang_teardown },
    { "lang/vm/fib", test_vm_fib, lang_setup, lang_teardown },
    { "lang/vm/loop", test_vm_loop, lang_setup, lang_teardown },
    { "lang/eval/try", test_eval_try, lang_setup, lang_teardown },
    { "lang/eval/raise", test_eval_raise, lang_setup, lang_teardown },
    { "lang/eval/vector_add", test_eval_vector_add, lang_setup, lang_teardown },
    { "lang/eval/vector_add_vec", test_eval_vector_add_vec, lang_setup, lang_teardown },
    { "lang/eval/sum", test_eval_sum, lang_setup, lang_teardown },
    { "lang/eval/count", test_eval_count, lang_setup, lang_teardown },
    { "lang/eval/avg", test_eval_avg, lang_setup, lang_teardown },
    { "lang/eval/min_max", test_eval_min_max, lang_setup, lang_teardown },
    { "lang/eval/first_last", test_eval_first_last, lang_setup, lang_teardown },
    { "lang/eval/map", test_eval_map, lang_setup, lang_teardown },
    { "lang/eval/pmap", test_eval_pmap, lang_setup, lang_teardown },
    { "lang/eval/fold", test_eval_fold, lang_setup, lang_teardown },
    { "lang/eval/scan", test_eval_scan, lang_setup, lang_teardown },
    { "lang/eval/filter", test_eval_filter, lang_setup, lang_teardown },
    { "lang/eval/apply", test_eval_apply, lang_setup, lang_teardown },
    { "lang/eval/distinct", test_eval_distinct, lang_setup, lang_teardown },
    { "lang/eval/in", test_eval_in, lang_setup, lang_teardown },
    { "lang/eval/except", test_eval_except, lang_setup, lang_teardown },
    { "lang/eval/union", test_eval_union, lang_setup, lang_teardown },
    { "lang/eval/sect", test_eval_sect, lang_setup, lang_teardown },
    { "lang/eval/take", test_eval_take, lang_setup, lang_teardown },
    { "lang/eval/take_neg", test_eval_take_neg, lang_setup, lang_teardown },
    { "lang/eval/at", test_eval_at, lang_setup, lang_teardown },
    { "lang/eval/find", test_eval_find, lang_setup, lang_teardown },
    { "lang/eval/reverse", test_eval_reverse, lang_setup, lang_teardown },
    { "lang/eval/table", test_eval_table, lang_setup, lang_teardown },
    { "lang/eval/at_table", test_eval_at_table, lang_setup, lang_teardown },
    { "lang/eval/key_table", test_eval_key_table, lang_setup, lang_teardown },
    { "lang/eval/count_table", test_eval_count_table, lang_setup, lang_teardown },
    { "lang/eval/select_all", test_eval_select_all, lang_setup, lang_teardown },
    { "lang/eval/select_where", test_eval_select_where, lang_setup, lang_teardown },
    { "lang/eval/select_where_in_sym", test_eval_select_where_in_sym, lang_setup, lang_teardown },
    { "lang/eval/select_where_in_i64", test_eval_select_where_in_i64, lang_setup, lang_teardown },
    { "lang/eval/select_where_in_mixed_numeric", test_eval_select_where_in_mixed_numeric, lang_setup, lang_teardown },
    { "lang/eval/select_where_in_nulls", test_eval_select_where_in_nulls, lang_setup, lang_teardown },
    { "lang/eval/select_where_in_empty_set_nulls", test_eval_select_where_in_empty_set_nulls, lang_setup, lang_teardown },
    { "lang/eval/select_where_in_sym_vs_atom_mismatch", test_eval_select_where_in_sym_vs_atom_mismatch, lang_setup, lang_teardown },
    { "lang/eval/select_where_eq_null_literal", test_eval_select_where_eq_null_literal, lang_setup, lang_teardown },
    { "lang/eval/pivot_multi_index", test_eval_pivot_multi_index, lang_setup, lang_teardown },
    { "lang/eval/select_named_lambda", test_eval_select_named_lambda, lang_setup, lang_teardown },
    { "lang/eval/select_global_scalar", test_eval_select_global_scalar, lang_setup, lang_teardown },
    { "lang/eval/select_lambda_nonagg", test_eval_select_lambda_nonagg, lang_setup, lang_teardown },
    { "lang/eval/select_lambda_where", test_eval_select_lambda_where, lang_setup, lang_teardown },
    { "lang/eval/select_lambda_agg", test_eval_select_lambda_agg, lang_setup, lang_teardown },
    { "lang/eval/select_let_binding", test_eval_select_let_binding, lang_setup, lang_teardown },
    { "lang/eval/select_lambda_nested_shadow", test_eval_select_lambda_nested_shadow, lang_setup, lang_teardown },
    { "lang/eval/select_lambda_arg_reuse", test_eval_select_lambda_arg_reuse, lang_setup, lang_teardown },
    { "lang/eval/select_lambda_arity_err", test_eval_select_lambda_arity_err, lang_setup, lang_teardown },
    { "lang/eval/select_by_where_filters", test_eval_select_by_where_filters, lang_setup, lang_teardown },
    { "lang/eval/select_by_where_in", test_eval_select_by_where_in, lang_setup, lang_teardown },
    { "lang/eval/select_if", test_eval_select_if, lang_setup, lang_teardown },
    { "lang/eval/select_where_sym_atom", test_eval_select_where_sym_atom, lang_setup, lang_teardown },
    { "lang/eval/select_cast", test_eval_select_cast, lang_setup, lang_teardown },
    { "lang/eval/select_round", test_eval_select_round, lang_setup, lang_teardown },
    { "lang/eval/select_cols", test_eval_select_cols, lang_setup, lang_teardown },
    { "lang/eval/select_groupby", test_eval_select_groupby, lang_setup, lang_teardown },
    { "lang/eval/select_xbar", test_eval_select_xbar, lang_setup, lang_teardown },
    { "lang/eval/select_asc", test_eval_select_asc, lang_setup, lang_teardown },
    { "lang/eval/select_desc", test_eval_select_desc, lang_setup, lang_teardown },
    { "lang/eval/select_asc_desc", test_eval_select_asc_desc, lang_setup, lang_teardown },
    { "lang/eval/select_take", test_eval_select_take, lang_setup, lang_teardown },
    { "lang/eval/select_take_neg", test_eval_select_take_neg, lang_setup, lang_teardown },
    { "lang/eval/select_take_range", test_eval_select_take_range, lang_setup, lang_teardown },
    { "lang/eval/select_combined", test_eval_select_combined, lang_setup, lang_teardown },
    { "lang/eval/select_asc_multi", test_eval_select_asc_multi, lang_setup, lang_teardown },
    { "lang/eval/select_groupby_sort", test_eval_select_groupby_sort, lang_setup, lang_teardown },
    { "lang/eval/select_by_nonagg_i64_key", test_eval_select_by_nonagg_i64_key, lang_setup, lang_teardown },
    { "lang/eval/select_by_nonagg_u8_key", test_eval_select_by_nonagg_u8_key, lang_setup, lang_teardown },
    { "lang/eval/select_by_nonagg_empty", test_eval_select_by_nonagg_empty, lang_setup, lang_teardown },
    { "lang/eval/select_by_mixed_naming", test_eval_select_by_mixed_naming, lang_setup, lang_teardown },
    { "lang/eval/select_by_nonagg_list_col", test_eval_select_by_nonagg_list_col, lang_setup, lang_teardown },
    { "lang/eval/select_by_str_nonagg_list_col", test_eval_select_by_str_nonagg_list_col, lang_setup, lang_teardown },
    { "lang/eval/select_by_nonagg_broadcast", test_eval_select_by_nonagg_broadcast, lang_setup, lang_teardown },
    { "lang/eval/select_by_nonagg_colref_vs_const", test_eval_select_by_nonagg_colref_vs_const, lang_setup, lang_teardown },
    { "lang/eval/select_by_nonagg_with_agg_subexpr", test_eval_select_by_nonagg_with_agg_subexpr, lang_setup, lang_teardown },
    { "lang/eval/select_by_nonagg_sort_take", test_eval_select_by_nonagg_sort_take, lang_setup, lang_teardown },
    { "lang/eval/select_by_take_clamps", test_eval_select_by_take_clamps, lang_setup, lang_teardown },
    { "lang/eval/select_by_vec_bool_order", test_eval_select_by_vec_bool_order, lang_setup, lang_teardown },
    { "lang/eval/select_by_vec_str_key", test_eval_select_by_vec_str_key, lang_setup, lang_teardown },
    { "lang/eval/select_by_multi_nonagg_nyi", test_eval_select_by_multi_nonagg_nyi, lang_setup, lang_teardown },
    { "lang/eval/update", test_eval_update, lang_setup, lang_teardown },
    { "lang/eval/update_no_where", test_eval_update_no_where, lang_setup, lang_teardown },
    { "lang/eval/update_str_masked", test_eval_update_str_masked, lang_setup, lang_teardown },
    { "lang/eval/table_mixed_type_error", test_eval_table_mixed_type_error, lang_setup, lang_teardown },
    { "lang/eval/update_str_type_mismatch", test_eval_update_str_type_mismatch, lang_setup, lang_teardown },
    { "lang/eval/select_empty_const", test_eval_select_empty_const, lang_setup, lang_teardown },
    { "lang/eval/insert", test_eval_insert, lang_setup, lang_teardown },
    { "lang/eval/insert_vec_append", test_eval_insert_vec_append, lang_setup, lang_teardown },
    { "lang/eval/insert_list_append", test_eval_insert_list_append, lang_setup, lang_teardown },
    { "lang/eval/insert_vec_positional", test_eval_insert_vec_positional, lang_setup, lang_teardown },
    { "lang/eval/insert_list_positional", test_eval_insert_list_positional, lang_setup, lang_teardown },
    { "lang/eval/insert_positional_multi", test_eval_insert_positional_multi, lang_setup, lang_teardown },
    { "lang/eval/insert_typed_null", test_eval_insert_typed_null, lang_setup, lang_teardown },
    { "lang/eval/insert_guid", test_eval_insert_guid, lang_setup, lang_teardown },
    { "lang/eval/insert_positional_errors", test_eval_insert_positional_errors, lang_setup, lang_teardown },
    { "lang/eval/upsert", test_eval_upsert, lang_setup, lang_teardown },
    { "lang/eval/upsert_f64_key", test_eval_upsert_f64_key, lang_setup, lang_teardown },
    { "lang/eval/upsert_str_key", test_eval_upsert_str_key, lang_setup, lang_teardown },
    { "lang/eval/upsert_type_mismatch", test_eval_upsert_type_mismatch, lang_setup, lang_teardown },
    { "lang/eval/left_join", test_eval_left_join, lang_setup, lang_teardown },
    { "lang/eval/inner_join", test_eval_inner_join, lang_setup, lang_teardown },
    { "lang/eval/window_join", test_eval_window_join, lang_setup, lang_teardown },
    { "lang/eval/println", test_eval_println, lang_setup, lang_teardown },
    { "lang/sort/decode_i64", test_sort_decode_i64, lang_setup, lang_teardown },
    { "lang/sort/decode_f64", test_sort_decode_f64, lang_setup, lang_teardown },
    { "lang/sort/decode_desc", test_sort_decode_desc, lang_setup, lang_teardown },
    { "lang/sort/decode_f64_neg", test_sort_decode_f64_neg, lang_setup, lang_teardown },
    { "lang/sort/decode_radix_f64", test_sort_decode_radix_f64, lang_setup, lang_teardown },
    { "lang/sort/decode_radix_f64_desc", test_sort_decode_radix_f64_desc, lang_setup, lang_teardown },
    { "lang/sort/decode_radix_i64", test_sort_decode_radix_i64, lang_setup, lang_teardown },
    { "lang/sort/long_common_prefix", test_sort_long_common_prefix, lang_setup, lang_teardown },
    { "lang/sort/embedded_nul", test_sort_embedded_nul, lang_setup, lang_teardown },
    { "lang/eval/read_write_csv", test_eval_read_write_csv, lang_setup, lang_teardown },
    { "lang/eval/as_cast", test_eval_as_cast, lang_setup, lang_teardown },
    { "lang/eval/type", test_eval_type, lang_setup, lang_teardown },
    { "lang/env/lookup_prefix", test_env_lookup_prefix, lang_setup, lang_teardown },
    { "lang/verb/sum_til", test_verb_sum_til, lang_setup, lang_teardown },
    { "lang/verb/avg_til", test_verb_avg_til, lang_setup, lang_teardown },
    { "lang/verb/min_til", test_verb_min_til, lang_setup, lang_teardown },
    { "lang/verb/max_til", test_verb_max_til, lang_setup, lang_teardown },
    { "lang/verb/count_til", test_verb_count_til, lang_setup, lang_teardown },
    { "lang/verb/first_til", test_verb_first_til, lang_setup, lang_teardown },
    { "lang/verb/last_til", test_verb_last_til, lang_setup, lang_teardown },
    { "lang/verb/dev_til", test_verb_dev_til, lang_setup, lang_teardown },
    { "lang/verb/if_sum", test_verb_if_sum, lang_setup, lang_teardown },
    { "lang/verb/sum_var", test_verb_sum_var, lang_setup, lang_teardown },
    { "lang/atomic_map_nested_vec", test_atomic_map_nested_vec, lang_setup, lang_teardown },
    { "lang/error_trace_exists", test_error_trace_exists, lang_setup, lang_teardown },
    { "lang/rf/null_propagate", test_rf_null_propagate, lang_setup, lang_teardown },
    { "lang/dotted/write_read", test_dotted_write_read, lang_setup, lang_teardown },
    { "lang/dotted/multi_key", test_dotted_multi_key, lang_setup, lang_teardown },
    { "lang/dotted/nested", test_dotted_nested, lang_setup, lang_teardown },
    { "lang/dotted/update_in_place", test_dotted_update_in_place, lang_setup, lang_teardown },
    { "lang/dotted/wrong_type_parent", test_dotted_wrong_type_parent, lang_setup, lang_teardown },
    { "lang/dotted/missing_key", test_dotted_missing_key, lang_setup, lang_teardown },
    { "lang/dotted/del_removes_key", test_dotted_del_removes_key, lang_setup, lang_teardown },
    { "lang/dotted/del_nested", test_dotted_del_nested, lang_setup, lang_teardown },
    { "lang/dotted/del_cascade", test_dotted_del_cascade, lang_setup, lang_teardown },
    { "lang/dotted/table_column", test_dotted_table_column, lang_setup, lang_teardown },
    { "lang/dotted/temporal_atom", test_dotted_temporal_atom, lang_setup, lang_teardown },
    { "lang/dotted/temporal_vector", test_dotted_temporal_vector, lang_setup, lang_teardown },
    { "lang/dotted/dag_temporal_nulls", test_dag_temporal_extract_nulls, lang_setup, lang_teardown },
    { "lang/dotted/temporal_slice_nulls", test_temporal_extract_slice_nulls, lang_setup, lang_teardown },
    { "lang/dotted/temporal_truncate_atom", test_dotted_temporal_truncate_atom, lang_setup, lang_teardown },
    { "lang/select_by_dotted_temporal_key_nocrash", test_select_by_dotted_temporal_key_nocrash, lang_setup, lang_teardown },
    { "lang/select_by_dotted_key_surfaces_key_col", test_select_by_dotted_key_surfaces_key_col, lang_setup, lang_teardown },
    { "lang/select_by_dotted_key_name_collision", test_select_by_dotted_key_name_collision, lang_setup, lang_teardown },
    { "lang/select_by_dotted_key_empty_schema", test_select_by_dotted_key_empty_schema, lang_setup, lang_teardown },
    { "lang/select_by_xbar_empty_type", test_select_by_xbar_empty_type, lang_setup, lang_teardown },
    { "lang/atomic_map_empty_sym_str_compare", test_atomic_map_empty_sym_str_compare, lang_setup, lang_teardown },
    { "lang/select_by_computed_key_many_groups", test_select_by_computed_key_many_groups, lang_setup, lang_teardown },
    { "lang/select_nonagg_dotted_temporal", test_select_nonagg_dotted_temporal, lang_setup, lang_teardown },
    { "lang/dotted/temporal_table_column", test_dotted_temporal_table_column, lang_setup, lang_teardown },
    { "lang/select_by_f64_perf", test_select_by_f64_perf, lang_setup, lang_teardown },
    { "lang/select_by_narrow_int_key", test_select_by_narrow_int_key, lang_setup, lang_teardown },
    { "lang/select_by_nullable_f64_key", test_select_by_nullable_f64_key, lang_setup, lang_teardown },
    { "lang/select_by_nullable_i64_key", test_select_by_nullable_i64_key, lang_setup, lang_teardown },
    { "lang/select_by_str_nullable_nonkey", test_select_by_str_nullable_nonkey, lang_setup, lang_teardown },
    { "lang/select_by_computed_key_nullable_nonkey", test_select_by_computed_key_nullable_nonkey, lang_setup, lang_teardown },
    { "lang/datalog/fixpoint", test_datalog_fixpoint, lang_setup, lang_teardown },
    { "lang/datalog/query_inline_rules", test_datalog_query_inline_rules, lang_setup, lang_teardown },

    /* ops/builtins.c entry-point coverage */
    { "lang/builtin/print",       test_builtin_print_fn,       lang_setup, lang_teardown },
    { "lang/builtin/show",        test_builtin_show_fn,        lang_setup, lang_teardown },
    { "lang/builtin/timeit",      test_builtin_timeit_fn,      lang_setup, lang_teardown },
    { "lang/builtin/load_file",   test_builtin_load_file_fn,   lang_setup, lang_teardown },
    { "lang/builtin/write_file",  test_builtin_write_file_fn,  lang_setup, lang_teardown },

    { NULL, NULL, NULL, NULL },
};


