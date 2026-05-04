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

/*
 * test_compile.c — unit tests for src/lang/compile.c
 *
 * Strategy: exercises compiler paths by calling user-defined lambdas via
 * ray_eval_str().  The compiler is invoked lazily on first call.
 * Tests target zero-hit regions identified from llvm-cov output.
 */

#include "test.h"
#include <rayforce.h>
#include "lang/eval.h"
#include "lang/env.h"
#include "lang/parse.h"
#include <string.h>

/* Forward-declare runtime API */
struct ray_runtime_s;
typedef struct ray_runtime_s ray_runtime_t;
extern ray_runtime_t* ray_runtime_create(int argc, char** argv);
extern void           ray_runtime_destroy(ray_runtime_t* rt);
extern ray_runtime_t *__RUNTIME;

/* ---- Setup / Teardown ---- */

static void compile_setup(void) {
    ray_runtime_create(0, NULL);
}

static void compile_teardown(void) {
    ray_runtime_destroy(__RUNTIME);
}

/* ─── Helper macros ─── */

/* Evaluate expr string; assert no error; release result; PASS. */
#define EVAL_OK(expr) do { \
    ray_t *_r = ray_eval_str(expr); \
    if (!_r || RAY_IS_ERR(_r)) { \
        if (_r) ray_error_free(_r); \
        FAILF("eval error on: %s", expr); \
    } \
    ray_release(_r); \
} while (0)

/* Evaluate and assert integer result. */
#define EVAL_I64(expr, expected) do { \
    ray_t *_r = ray_eval_str(expr); \
    if (!_r || RAY_IS_ERR(_r)) { \
        if (_r) ray_error_free(_r); \
        FAILF("eval error on: %s", expr); \
    } \
    if (_r->type != -RAY_I64 || _r->i64 != (int64_t)(expected)) { \
        ray_release(_r); \
        FAILF("expected %lld from: %s", (long long)(expected), expr); \
    } \
    ray_release(_r); \
} while (0)

/* Evaluate; assert IS an error. */
#define EVAL_ERR(expr) do { \
    ray_t *_r = ray_eval_str(expr); \
    if (_r && !RAY_IS_ERR(_r)) { \
        ray_release(_r); \
        FAILF("expected error from: %s", expr); \
    } \
    if (_r) ray_error_free(_r); \
} while (0)

/* ════════════════════════════════════════════════════════════════════
 * 1. (set name val) inside a compiled lambda body (line 225-230)
 *    The compiler emits OP_CALLD for set because set modifies the
 *    global environment and the compiler defers to the interpreter.
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_set_inside_fn(void) {
    /* Define a fn that calls (set ...) inside its body using a constant
     * value (not a local variable) so the deferred AST can resolve.
     * The compile path for (set name val) delegates to OP_CALLD. */
    EVAL_I64(
        "(do "
          "(set f (fn [] (set compile_set_g 42) compile_set_g)) "
          "(f))",
        42);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 2. (if cond then) WITHOUT else branch (lines 268-277)
 *    Compiler emits a zero literal for the false branch.
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_if_no_else_true(void) {
    /* When condition is true, result is the then-expr. */
    EVAL_I64(
        "(do (set f (fn [x] (if (> x 0) 99))) (f 5))",
        99);
    PASS();
}

static test_result_t test_compile_if_no_else_false(void) {
    /* When condition is false, result is the implicit 0. */
    EVAL_I64(
        "(do (set f (fn [x] (if (> x 0) 99))) (f -1))",
        0);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 3. (do ...) inside a compiled lambda body (lines 282-288)
 *    Compiler emits OP_POP between each expression.
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_do_inside_fn(void) {
    /* fn body with explicit (do ...) triggers the do-special-form path. */
    EVAL_I64(
        "(do (set f (fn [x] (do (let y (* x 3)) (+ y 1)))) (f 4))",
        13);
    PASS();
}

static test_result_t test_compile_do_multi_exprs(void) {
    /* Three expressions in do — exercises the i > 1 OP_POP branch. */
    EVAL_I64(
        "(do (set f (fn [x] (do (* x 1) (* x 2) (+ x 10)))) (f 5))",
        15);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 4. (fn ...) nested lambda inside a compiled body (lines 292-297)
 *    The compiler emits OP_CALLD for inline fn forms.
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_nested_fn(void) {
    /* An inner (fn ...) expression appearing inside a compiled lambda body.
     * The compiler emits OP_CALLD for the nested fn form.
     * The inner fn only uses its own parameter, avoiding closure over locals. */
    EVAL_I64(
        "(do "
          "(set outer (fn [x] "
            "(let adder (fn [y] (* y 3))) "
            "(+ x (adder 2)))) "
          "(outer 1))",
        7);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 5. (try body handler) inside a compiled lambda body (lines 300-321)
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_try_inside_fn_ok(void) {
    /* try body succeeds — handler not called. */
    EVAL_I64(
        "(do (set f (fn [x] (try (* x 2) (fn [e] -1)))) (f 5))",
        10);
    PASS();
}

static test_result_t test_compile_try_inside_fn_err(void) {
    /* try body raises — handler is called with the error object.
     * Handler returns a constant so no closure over locals needed. */
    EVAL_I64(
        "(do (set f (fn [x] (try (raise \"oops\") (fn [e] 99)))) (f 42))",
        99);
    PASS();
}

static test_result_t test_compile_try_div_zero(void) {
    /* Division by zero caught inside compiled lambda. */
    EVAL_I64(
        "(do (set f (fn [x] (try (/ 10 x) (fn [e] 0)))) (f 0))",
        0);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 6. (self ...) recursive self-call (lines 325-334)
 *    'self' inside a lambda body triggers OP_CALLS.
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_self_recursive(void) {
    /* Factorial using self — exercises OP_CALLS emission. */
    EVAL_I64(
        "(do "
          "(set fact (fn [n] (if (<= n 1) 1 (* n (self (- n 1)))))) "
          "(fact 5))",
        120);
    PASS();
}

static test_result_t test_compile_self_tail_recursive(void) {
    /* Tail-recursive countdown using self. */
    EVAL_I64(
        "(do "
          "(set countdown (fn [n acc] (if (== n 0) acc (self (- n 1) (+ acc 1))))) "
          "(countdown 10 0))",
        10);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 7. Unrecognized special form inside compiled body (lines 342-348)
 *    'and'/'or' are RAY_FN_SPECIAL_FORM but not handled specially.
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_and_special_form(void) {
    /* 'and' is a RAY_FN_SPECIAL_FORM that compile_list dispatches as OP_CALLD.
     * The whole (and ...) AST is pushed and evaluated dynamically.
     * We use constant sub-expressions to avoid closure-over-local issues. */
    ray_t *r = ray_eval_str(
        "(do (set f (fn [] (and true true))) (f))");
    if (!r || RAY_IS_ERR(r)) {
        if (r) ray_error_free(r);
        FAILF("eval error in and_special_form");
    }
    ray_release(r);
    PASS();
}

static test_result_t test_compile_or_special_form(void) {
    /* 'or' is also RAY_FN_SPECIAL_FORM. */
    ray_t *r = ray_eval_str(
        "(do (set f (fn [] (or false true))) (f))");
    if (!r || RAY_IS_ERR(r)) {
        if (r) ray_error_free(r);
        FAILF("eval error in or_special_form");
    }
    ray_release(r);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 8. Non-list non-atom: vector/table literal inside lambda (lines 422-426)
 *    A RAY_I64 vector appearing as a subexpression.
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_vector_literal(void) {
    /* A vector literal [1 2 3] in the body — ast->type == RAY_I64 (not list, not atom). */
    EVAL_OK(
        "(do (set f (fn [x] (+ [1 2 3] x))) (f 10))");
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 9. let with invalid (non-symbol) name — compile error path (line 244)
 *    Triggers c->error = true in the let handler.
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_let_reserved_name(void) {
    /* Trying to let-bind a reserved name (.sys.*) should trigger
     * c->error in the compiler, which falls back to the tree-walker.
     * The tree-walker raises a 'reserve' error. */
    EVAL_ERR(
        "(do (set f (fn [x] (let .sys.gc x) x)) (f 1))");
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 10. RAY_UNARY called with wrong argc (line 371 break + line 388-390)
 *     compile_list falls through to OP_CALLF after break.
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_unary_wrong_arity(void) {
    /* Calling a known unary fn (neg) with 2 args causes the compiler
     * to emit OP_CALLF instead of OP_CALL1. Runtime will error. */
    EVAL_ERR(
        "(do (set f (fn [x y] (neg x y))) (f 1 2))");
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 11. RAY_BINARY called with wrong argc (line 374 break + line 388-390)
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_binary_wrong_arity(void) {
    /* Calling a known binary fn (+) with 3 args — falls through to OP_CALLF. */
    EVAL_ERR(
        "(do (set f (fn [a b c] (+ a b c))) (f 1 2 3))");
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 12. Lambda call path through OP_CALLF (lines 379-382 and 388-390)
 *     A user-defined lambda called from within another compiled lambda.
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_lambda_call(void) {
    /* f calls g — g is a compiled lambda, so its call site in f's body
     * goes through case RAY_LAMBDA: which emits OP_CALLF. */
    EVAL_I64(
        "(do "
          "(set g (fn [x] (* x x))) "
          "(set f (fn [n] (g n))) "
          "(f 7))",
        49);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 13. Large constant pool (> 16 constants) — pool grow path (lines 142-154)
 *     Forces add_constant to reallocate the pool beyond initial cap=16.
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_large_const_pool(void) {
    /* Body with many distinct constant integers to overflow the initial
     * const-pool cap of 16 and trigger the grow path (lines 142-154).
     * We use a do block with 20 unique integer constants being summed. */
    EVAL_I64(
        "(do (set f (fn [] (do "
          "(+ 1 2) (+ 3 4) (+ 5 6) (+ 7 8) (+ 9 10) "
          "(+ 11 12) (+ 13 14) (+ 15 16) (+ 17 18) (+ 19 20)"
          "))) "
          "(f))",
        39);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 14. Many symbols in body — forces OP_RESOLVE_W path (lines 409-413)
 *     Need > 256 distinct symbol entries in the const pool.
 *     We do this by having a big lambda with many unique variable refs.
 * ════════════════════════════════════════════════════════════════════ */

/* Helper: generate a big expression referencing many free-variable symbols
 * through a chain that forces 256+ entries in the constant pool.
 * We call a lambda that uses many different known builtins so each
 * builtin symbol is added to the constant pool once. */
static test_result_t test_compile_many_symbols(void) {
    /* Force OP_RESOLVE_W (lines 409-413) by building a const pool with
     * > 256 entries before a free-symbol resolution occurs.
     *
     * Strategy: a lambda body with 260 distinct integer constants (as
     * (do (+ 0 k0) (+ 0 k1) ...) where k0..k259 are all unique), then
     * references a global symbol (which would land at index >= 256).
     * We pre-define the global symbol externally. */

    /* First bind a global that the lambda can reference as a free var. */
    ray_t *pre = ray_eval_str("(set _sym_resolve_w_test 777)");
    if (!pre || RAY_IS_ERR(pre)) {
        if (pre) ray_error_free(pre);
        FAILF("pre-setup failed");
    }
    ray_release(pre);

    /* Build fn body: 260 distinct integer adds + a reference to the
     * global. The integers force the pool to grow past 256 entries.
     * The global sym resolves to a pool slot >= 256 => OP_RESOLVE_W. */
    char buf[65536];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "(do (set bigfn2 (fn [] (do");

    /* 260 distinct integer literals: we add (+ 1000 k) for k=0..259 */
    for (int i = 0; i < 260 && pos < (int)sizeof(buf) - 300; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, " (+ 1000 %d)", i + 2);
    }
    /* Now reference the pre-defined global — this sym goes to pool at index > 256 */
    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    " _sym_resolve_w_test))) (bigfn2))");

    ray_t *r = ray_eval_str(buf);
    if (!r || RAY_IS_ERR(r)) {
        if (r) ray_error_free(r);
        FAILF("eval error in test_compile_many_symbols");
    }
    int64_t val = r->i64;
    ray_release(r);
    TEST_ASSERT_EQ_I(val, 777);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 15. Code buffer grow path (lines 103-113)
 *     Emit > 256 bytes to force the buffer to double.
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_code_buffer_grow(void) {
    /* Build a deeply nested expression to emit many opcodes.
     * Each arithmetic op emits at least 2 bytes; 150 nested ops = 300+ bytes. */
    char buf[32768];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "(do (set f (fn [x] ");

    /* 130 nested additions: (+ (+ (+ ... x 1) 1) ... 1) */
    for (int i = 0; i < 130; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "(+ ");
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "x");
    for (int i = 0; i < 130; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, " 1)");
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, ")) (f 0))");

    ray_t *r = ray_eval_str(buf);
    if (!r || RAY_IS_ERR(r)) {
        if (r) ray_error_free(r);
        FAILF("eval error in test_compile_code_buffer_grow");
    }
    int64_t val = r->i64;
    ray_release(r);
    TEST_ASSERT_EQ_I(val, 130);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 16. (if ...) with 3+ branches ensures n >= 4 path and n < 4 path both covered
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_if_with_else(void) {
    EVAL_I64(
        "(do (set f (fn [x] (if (> x 0) 1 -1))) (f 5))",
        1);
    PASS();
}

static test_result_t test_compile_if_with_else_false(void) {
    EVAL_I64(
        "(do (set f (fn [x] (if (> x 0) 1 -1))) (f -5))",
        -1);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 17. Empty list as expression (lines 428-432)
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_empty_list_expr(void) {
    /* An empty list () appearing inside a lambda body — compile_expr
     * handles it via the ray_len(ast) == 0 path. */
    EVAL_OK(
        "(do (set f (fn [x] (if (> x 0) x ()))) (f 5))");
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 18. compile_list with zero-length list — c->error path (line 213)
 * ════════════════════════════════════════════════════════════════════ */

/* ════════════════════════════════════════════════════════════════════
 * 19. Multiple body expressions with OP_POP between them (line 458)
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_multi_body_exprs(void) {
    /* Lambda body with 3 expressions — first two are popped. */
    EVAL_I64(
        "(do "
          "(set f (fn [x] "
            "(* x 1) "
            "(* x 2) "
            "(+ x 100))) "
          "(f 5))",
        105);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 20. (try ...) error path when c.error is set (dbg_obj release)
 *     Covered by the reserved-name test above, but add a variant
 *     where try handler compilation also fails gracefully.
 * ════════════════════════════════════════════════════════════════════ */

/* ════════════════════════════════════════════════════════════════════
 * 21. Boolean and float literals inside compiled lambda (non-sym atoms)
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_bool_literal(void) {
    EVAL_I64(
        "(do (set f (fn [x] (if true x 0))) (f 42))",
        42);
    PASS();
}

static test_result_t test_compile_float_literal(void) {
    /* Float constant in pool — also exercises f64 dedup path. */
    ray_t *r = ray_eval_str(
        "(do (set f (fn [x] (+ x 1.5))) (f 0.5))");
    if (!r || RAY_IS_ERR(r)) {
        if (r) ray_error_free(r);
        FAILF("eval error on float literal test");
    }
    TEST_ASSERT(r->type == -RAY_F64, "expected f64");
    ray_release(r);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 22. find_local hits existing local (returns slot >= 0) — let re-bind
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_let_rebind(void) {
    /* Re-binding the same name triggers find_local to return slot >= 0
     * and skip add_local — covers line 250 slot = find_local path. */
    EVAL_I64(
        "(do "
          "(set f (fn [x] "
            "(let r (* x 2)) "
            "(let r (+ r 1)) "
            "r)) "
          "(f 5))",
        11);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 23. Constant deduplication — same literal used twice
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_const_dedup(void) {
    /* Using 42 twice should reuse the same const pool slot. */
    EVAL_I64(
        "(do (set f (fn [x] (+ x (+ 42 42)))) (f 0))",
        84);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 24. ray_bc_dbg_get: called with NULL dbg → returns zero span.
 *     Covered by the existing 2 hits, but add explicit dbg test.
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_ray_compile_reset(void) {
    /* ray_compile_reset resets the thread-local sym IDs — subsequent
     * compilation should still work correctly after a reset. */
    ray_compile_reset();
    EVAL_I64(
        "(do (set fr (fn [x] (+ x 1))) (fr 10))",
        11);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 25. LAMBDA_IS_COMPILED guard: calling same fn twice should not recompile
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_already_compiled(void) {
    /* First call compiles; second call should hit LAMBDA_IS_COMPILED guard. */
    EVAL_I64("(do (set fc (fn [x] (* x 3))) (fc 4))", 12);
    EVAL_I64("(fc 5)", 15);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 26. head not a symbol — compile_expr for head (else branch, line 357)
 *     When head is a literal (not a named sym), fn = NULL.
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_head_is_lambda_literal(void) {
    /* The head of the call is itself a fn expression, not a symbol.
     * fn = NULL => falls into compile_expr(c, head) path. */
    EVAL_I64(
        "(do (set f (fn [x] ((fn [y] (+ y 1)) x))) (f 9))",
        10);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 27. self with zero args — argc == 0 (edge case, still exercises OP_CALLS)
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_self_zero_args(void) {
    /* self with zero args and a counter to stop recursion. */
    EVAL_I64(
        "(do "
          "(set g_cnt 0) "
          "(set noarg (fn [] "
            "(set g_cnt (+ g_cnt 1)) "
            "(if (< g_cnt 3) (self) g_cnt))) "
          "(noarg))",
        3);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 28. default case in switch(fn->type) (lines 383-384)
 *     Triggered when a global variable that is NOT a function type is
 *     used as the head of a call expression.  The compiler resolves
 *     `fn` from the env, finds it has a non-function type, hits the
 *     default: break branch and emits OP_CALLF anyway.  The VM then
 *     errors at runtime, which is expected.
 * ════════════════════════════════════════════════════════════════════ */
static test_result_t test_compile_default_switch_case(void) {
    /* Bind a non-function global, then call it from a compiled lambda.
     * Compile-time: fn != NULL, fn->type is -RAY_I64 (a negative value)
     * => hits default: break => emits OP_CALLF => runtime error. */
    ray_t *pre = ray_eval_str("(set not_a_fn_val 99)");
    if (!pre || RAY_IS_ERR(pre)) {
        if (pre) ray_error_free(pre);
        FAILF("pre-setup failed");
    }
    ray_release(pre);

    EVAL_ERR("(do (set f (fn [x] (not_a_fn_val x))) (f 1))");
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * Entry table
 * ════════════════════════════════════════════════════════════════════ */
const test_entry_t compile_entries[] = {
    { "compile/set_inside_fn",       test_compile_set_inside_fn,       compile_setup, compile_teardown },
    { "compile/if_no_else_true",     test_compile_if_no_else_true,     compile_setup, compile_teardown },
    { "compile/if_no_else_false",    test_compile_if_no_else_false,    compile_setup, compile_teardown },
    { "compile/do_inside_fn",        test_compile_do_inside_fn,        compile_setup, compile_teardown },
    { "compile/do_multi_exprs",      test_compile_do_multi_exprs,      compile_setup, compile_teardown },
    { "compile/nested_fn",           test_compile_nested_fn,           compile_setup, compile_teardown },
    { "compile/try_inside_fn_ok",    test_compile_try_inside_fn_ok,    compile_setup, compile_teardown },
    { "compile/try_inside_fn_err",   test_compile_try_inside_fn_err,   compile_setup, compile_teardown },
    { "compile/try_div_zero",        test_compile_try_div_zero,        compile_setup, compile_teardown },
    { "compile/self_recursive",      test_compile_self_recursive,      compile_setup, compile_teardown },
    { "compile/self_tail_recursive", test_compile_self_tail_recursive, compile_setup, compile_teardown },
    { "compile/and_special_form",    test_compile_and_special_form,    compile_setup, compile_teardown },
    { "compile/or_special_form",     test_compile_or_special_form,     compile_setup, compile_teardown },
    { "compile/vector_literal",      test_compile_vector_literal,      compile_setup, compile_teardown },
    { "compile/let_reserved_name",   test_compile_let_reserved_name,   compile_setup, compile_teardown },
    { "compile/unary_wrong_arity",   test_compile_unary_wrong_arity,   compile_setup, compile_teardown },
    { "compile/binary_wrong_arity",  test_compile_binary_wrong_arity,  compile_setup, compile_teardown },
    { "compile/lambda_call",         test_compile_lambda_call,         compile_setup, compile_teardown },
    { "compile/large_const_pool",    test_compile_large_const_pool,    compile_setup, compile_teardown },
    { "compile/many_symbols",        test_compile_many_symbols,        compile_setup, compile_teardown },
    { "compile/code_buffer_grow",    test_compile_code_buffer_grow,    compile_setup, compile_teardown },
    { "compile/if_with_else",        test_compile_if_with_else,        compile_setup, compile_teardown },
    { "compile/if_with_else_false",  test_compile_if_with_else_false,  compile_setup, compile_teardown },
    { "compile/empty_list_expr",     test_compile_empty_list_expr,     compile_setup, compile_teardown },
    { "compile/multi_body_exprs",    test_compile_multi_body_exprs,    compile_setup, compile_teardown },
    { "compile/bool_literal",        test_compile_bool_literal,        compile_setup, compile_teardown },
    { "compile/float_literal",       test_compile_float_literal,       compile_setup, compile_teardown },
    { "compile/let_rebind",          test_compile_let_rebind,          compile_setup, compile_teardown },
    { "compile/const_dedup",         test_compile_const_dedup,         compile_setup, compile_teardown },
    { "compile/compile_reset",       test_compile_ray_compile_reset,   compile_setup, compile_teardown },
    { "compile/already_compiled",    test_compile_already_compiled,    compile_setup, compile_teardown },
    { "compile/head_is_lambda",      test_compile_head_is_lambda_literal, compile_setup, compile_teardown },
    { "compile/self_zero_args",      test_compile_self_zero_args,      compile_setup, compile_teardown },
    { "compile/default_switch_case", test_compile_default_switch_case, compile_setup, compile_teardown },
    { NULL, NULL, NULL, NULL },
};
