/*
 *   Copyright (c) 2024 Anton Kundenko <singaraiona@gmail.com>
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

#include "cc.h"
#include "eval.h"
#include "ops.h"
#include "format.h"
#include "cond.h"
#include "vary.h"
#include "nfo.h"
#include "env.h"
#include "error.h"

// Add location info from compiler context when a compile error occurs
static nil_t cc_error_add_loc(cc_ctx_t *cc) {
    obj_p loc, nfo;
    span_t span;
    vm_p vm = VM;

    if (cc->nfo == NULL_OBJ || cc->cur_expr == NULL_OBJ)
        return;

    nfo = cc->nfo;

    // Check nfo has proper structure (list with at least 2 elements)
    if (nfo->type != TYPE_LIST || nfo->len < 2)
        return;

    span = nfo_get(nfo, (i64_t)cc->cur_expr);
    if (span.id == 0)
        return;

    // Create location: (span.id, filename, fn_name, source)
    loc = vn_list(4, i64(span.id), clone_obj(AS_LIST(nfo)[0]), NULL_OBJ, clone_obj(AS_LIST(nfo)[1]));

    if (vm->trace == NULL_OBJ)
        vm->trace = vn_list(1, loc);
    else
        push_raw(&vm->trace, &loc);
}

// Look up span for a bytecode offset
// dbg is an I64 vector of pairs: [offset0, span0, offset1, span1, ...]
// Returns the span for the largest offset <= ip
span_t bc_dbg_get(obj_p dbg, i64_t ip) {
    span_t span = {0};
    i64_t i, n, best_offset = -1;

    if (dbg == NULL_OBJ || dbg->len == 0)
        return span;

    n = dbg->len;
    for (i = 0; i < n; i += 2) {
        i64_t offset = AS_I64(dbg)[i];
        if (offset <= ip && offset > best_offset) {
            best_offset = offset;
            span.id = AS_I64(dbg)[i + 1];
        }
    }

    return span;
}

// clang-format off
#define OP(ctx, op) (AS_U8((ctx)->bc)[(ctx)->ip++] = (u8_t)(op))
#define CE(f)       { if ((f) == -1) return -1; }

// Load constant from consts pool by offset (no name, pure value)
#define LOADCONST(ctx, val) ({ OP(ctx, OP_LOADCONST); OP(ctx, (ctx)->consts->len); push_obj(&(ctx)->consts, (val)); })

// Load from env by offset (args + let-bound locals, resolvable by name)
#define LOADENV(ctx, offset) ({ OP(ctx, OP_LOADENV); OP(ctx, (offset)); })

// Store to env by offset (for let bindings)
#define STOREENV(ctx, offset) ({ OP(ctx, OP_STOREENV); OP(ctx, (offset)); })

// Record debug info: map bytecode offset to span from AST node
#define DBG(ctx, expr) do { \
    span_t _span = nfo_get((ctx)->nfo, (i64_t)(expr)); \
    if (_span.id != 0) { \
        i64_t _ip = (ctx)->ip; \
        push_raw(&(ctx)->dbg, &_ip); \
        push_raw(&(ctx)->dbg, &_span.id); \
    } \
} while(0)
// clang-format on

// Find symbol in env_names (args), return offset or -1 if not found
static i64_t cc_find_env(cc_ctx_t *cc, i64_t sym) {
    i64_t i, n;
    i64_t *names;

    if (cc->env_names == NULL_OBJ)
        return -1;

    n = cc->env_names->len;
    names = AS_SYMBOL(cc->env_names);

    for (i = 0; i < n; i++) {
        if (names[i] == sym)
            return i;
    }
    return -1;
}

static i64_t cc_expr(cc_ctx_t *cc, obj_p e);

// Check if the function is a special form that needs control flow handling
static b8_t is_cond_fn(obj_p car) {
    extern obj_p ray_cond(obj_p *, i64_t);
    return car->type == TYPE_VARY && (vary_f)car->i64 == ray_cond;
}

// Compile if/cond expression
static i64_t cc_cond(cc_ctx_t *cc, obj_p *lst, i64_t n) {
    i64_t j1, j2;

    if (n < 2 || n > 3) {
        LOADCONST(cc, NULL_OBJ);
        return 0;
    }

    // compile condition
    CE(cc_expr(cc, lst[0]));

    // jump if false to else branch
    OP(cc, OP_JMPF);
    j1 = cc->ip;
    OP(cc, 0);

    // compile then branch
    CE(cc_expr(cc, lst[1]));

    // jump over else branch
    OP(cc, OP_JMP);
    j2 = cc->ip;
    OP(cc, 0);

    // patch jmpf offset
    AS_U8(cc->bc)[j1] = (u8_t)(cc->ip - j1 - 1);

    // compile else branch (or null)
    if (n == 3) {
        CE(cc_expr(cc, lst[2]));
    } else {
        LOADCONST(cc, NULL_OBJ);
    }

    // patch jmp offset
    AS_U8(cc->bc)[j2] = (u8_t)(cc->ip - j2 - 1);

    return 0;
}

// Compile expression as a constant (no dereferencing of symbols)
// Used for special form arguments that shouldn't be evaluated
static i64_t cc_const(cc_ctx_t *cc, obj_p e) {
    LOADCONST(cc, clone_obj(e));
    return 0;
}

// Check if a binary function is a special form (like set/let)
static b8_t is_binary_special_form(obj_p fn) { return fn->type == TYPE_BINARY && (fn->attrs & FN_SPECIAL_FORM); }

// Check if a unary function is a special form (like quote)
static b8_t is_unary_special_form(obj_p fn) { return fn->type == TYPE_UNARY && (fn->attrs & FN_SPECIAL_FORM); }

// Compile function call
// expr is the original list expression (for debug info)
static i64_t cc_call(cc_ctx_t *cc, obj_p expr, obj_p *lst, i64_t n) {
    obj_p car = lst[0];
    i64_t i;

    // Track current expression for error reporting
    cc->cur_expr = expr;

    lst++;
    n--;

    // Handle special form 'cond'/'if'
    if (is_cond_fn(car)) {
        return cc_cond(cc, lst, n);
    }

    // Handle unary special forms (quote)
    // Argument is passed unevaluated
    if (is_unary_special_form(car) && n == 1) {
        // Push arg as constant (no dereference/evaluation)
        CE(cc_const(cc, lst[0]));
        // Record debug info and emit call
        DBG(cc, expr);
        LOADCONST(cc, clone_obj(car));
        OP(cc, OP_CALL1);
        return 0;
    }

    // Handle binary special forms (set, let, try)
    // Both arguments are passed unevaluated - let special form decide
    if (is_binary_special_form(car) && n == 2) {
        // First arg: push as constant (no dereference)
        CE(cc_const(cc, lst[0]));
        // Second arg: push as constant (let special form decide)
        CE(cc_const(cc, lst[1]));
        // Record debug info and emit call
        DBG(cc, expr);
        LOADCONST(cc, clone_obj(car));
        OP(cc, OP_CALL2);
        return 0;
    }

    // Compile arguments first (left to right)
    for (i = 0; i < n; ++i) {
        CE(cc_expr(cc, lst[i]));
    }

    // Record debug info before emitting call opcode
    DBG(cc, expr);

    // Now emit call instruction based on function type
    switch (car->type) {
        case TYPE_UNARY:
            if (n != 1)
                return -1;
            LOADCONST(cc, clone_obj(car));
            OP(cc, OP_CALL1);
            break;

        case TYPE_BINARY:
            if (n != 2)
                return -1;
            LOADCONST(cc, clone_obj(car));
            OP(cc, OP_CALL2);
            break;

        case TYPE_VARY:
            LOADCONST(cc, clone_obj(car));
            OP(cc, OP_CALLN);
            OP(cc, n);
            break;

        case TYPE_LAMBDA:
            LOADCONST(cc, clone_obj(car));
            OP(cc, OP_CALLF);
            break;

        case -TYPE_SYMBOL:
            if (car->i64 == SYMBOL_SELF) {
                // Self-recursive call
                OP(cc, OP_CALLS);
            } else {
                // Dynamic call - need to resolve symbol at runtime
                CE(cc_expr(cc, car));
                OP(cc, OP_CALLD);
                OP(cc, n);
            }
            break;

        default:
            // For other types, try to evaluate at runtime
            CE(cc_expr(cc, car));
            OP(cc, OP_CALLD);
            OP(cc, n);
            break;
    }

    return 0;
}

// Compile an expression
static i64_t cc_expr(cc_ctx_t *cc, obj_p e) {
    i64_t i;

    switch (e->type) {
        case -TYPE_SYMBOL:
            // Quoted symbols are treated as literal constants (no resolve)
            if (e->attrs & ATTR_QUOTED) {
                LOADCONST(cc, clone_obj(e));
                return 0;
            }
            // Check if it's in env (args + let-bound locals)
            i = cc_find_env(cc, e->i64);
            if (i >= 0) {
                // Found in env - load by offset (compile-time known, runtime resolvable)
                LOADENV(cc, i);
            } else {
                // Not in env - dynamic resolution via resolve()
                DBG(cc, e);  // Record debug info for symbol resolution errors
                LOADCONST(cc, clone_obj(e));
                OP(cc, OP_RESOLVE);
            }
            return 0;

        case TYPE_LIST:
            if (e->len == 0) {
                // Empty list - push null
                LOADCONST(cc, NULL_OBJ);
                return 0;
            }
            // Function call - pass expression for debug info
            return cc_call(cc, e, AS_LIST(e), e->len);

        default:
            // Constant - load from constants pool
            LOADCONST(cc, clone_obj(e));
            return 0;
    }
}

// Compile body (multiple expressions)
static i64_t cc_body(cc_ctx_t *cc, obj_p *lst, i64_t n) {
    i64_t i;

    for (i = 0; i < n; ++i) {
        if (cc_expr(cc, lst[i]) == -1)
            return -1;
        // Pop intermediate results (except last)
        if (i < n - 1)
            OP(cc, OP_POP);
    }

    return 0;
}

// Compile a lambda function
static obj_p cc_fn(cc_ctx_t *cc, obj_p *lst, i64_t n) {
    if (n == 0) {
        LOADCONST(cc, NULL_OBJ);
        goto end;
    }

    if (cc_body(cc, lst, n) == -1) {
        // Add location info from the failing expression before returning error
        cc_error_add_loc(cc);
        return err_new(EC_TYPE);
    }

end:
    OP(cc, OP_RET);

    // Resize bytecode to actual size
    resize_obj(&cc->bc, cc->ip);

    return NULL_OBJ;  // Success
}

// Main compilation entry point
obj_p cc_compile(obj_p lambda) {
    lambda_p fn = AS_LAMBDA(lambda);
    obj_p body = fn->body;
    obj_p result;
    i64_t body_len;
    obj_p *body_exprs;

    // Initialize compiler context
    cc_ctx_t cc = {
        .ip = 0,
        .bc = U8(BC_SIZE),
        .args = (fn->args != NULL_OBJ) ? clone_obj(fn->args) : SYMBOL(0),
        .locals = LIST(0),
        .consts = LIST(0),
        .dbg = I64(0),   // Debug info: pairs of (bytecode offset, span.id)
        .nfo = fn->nfo,  // Source nfo from parser
        .lp = 0,
        .cur_expr = NULL_OBJ,    // Current expression being compiled
        .env_names = SYMBOL(0),  // Env layout: symbol names by offset
    };

    // Initialize env_names with function arguments (slots 0..nargs-1)
    if (fn->args != NULL_OBJ) {
        i64_t i, n = fn->args->len;
        for (i = 0; i < n; i++) {
            i64_t sym = AS_SYMBOL(fn->args)[i];
            push_raw(&cc.env_names, &sym);
        }
    }

    // Prepare body expressions
    if (body == NULL_OBJ) {
        body_len = 0;
        body_exprs = NULL;
    } else if (body->type == TYPE_LIST && body->len > 0) {
        // Check if it's a 'do' block (first element is do function)
        obj_p first = AS_LIST(body)[0];
        extern obj_p ray_do(obj_p *, i64_t);
        if (first->type == TYPE_VARY && (vary_f)first->i64 == ray_do) {
            // It's (do expr1 expr2 ...) - compile the body expressions
            body_len = body->len - 1;
            body_exprs = AS_LIST(body) + 1;
        } else {
            // Single expression (could be a function call)
            body_len = 1;
            body_exprs = &body;
        }
    } else {
        // Single non-list expression
        body_len = 1;
        body_exprs = &body;
    }

    // Compile
    result = cc_fn(&cc, body_exprs, body_len);

    if (IS_ERR(result)) {
        drop_obj(cc.bc);
        drop_obj(cc.args);
        drop_obj(cc.locals);
        drop_obj(cc.consts);
        drop_obj(cc.dbg);
        drop_obj(cc.env_names);
        return result;
    }

    // Store compiled bytecode, constants, and debug info in lambda
    fn->bc = cc.bc;
    fn->consts = cc.consts;
    fn->dbg = cc.dbg;

    // Don't pre-create env dict - let runtime handle it via amend()
    // env_names was just for compile-time offset lookups (args)
    drop_obj(cc.env_names);

    // Cleanup
    drop_obj(cc.args);
    drop_obj(cc.locals);

    return NULL_OBJ;  // Success
}

// Dump bytecode for debugging
nil_t cc_dump(obj_p lambda) {
    lambda_p fn = AS_LAMBDA(lambda);
    u8_t *bc;
    obj_p *consts;
    i64_t n, i;
    obj_p fmt;

    if (fn->bc == NULL_OBJ) {
        printf("(not compiled)\n");
        return;
    }

    bc = AS_U8(fn->bc);
    consts = AS_LIST(fn->consts);
    n = fn->bc->len;

    printf("=== Bytecode dump ===\n");
    printf("Args: %lld, Consts: %lld, BC size: %lld\n", fn->args->len, fn->consts->len, n);

    for (i = 0; i < n; ++i) {
        printf("%3lld: ", i);
        switch (bc[i]) {
            // Control flow
            case OP_RET:
                printf("RET\n");
                break;
            case OP_JMP:
                printf("JMP +%d\n", bc[++i]);
                break;
            case OP_JMPF:
                printf("JMPF +%d\n", bc[++i]);
                break;
            // Data operations
            case OP_LOADCONST:
                fmt = obj_fmt(consts[bc[++i]], B8_FALSE);
                printf("LOADCONST %lld (%.*s)\n", (i64_t)bc[i], (i32_t)fmt->len, AS_C8(fmt));
                drop_obj(fmt);
                break;
            case OP_LOADENV:
                printf("LOADENV %d\n", bc[++i]);
                break;
            case OP_STOREENV:
                printf("STOREENV %d\n", bc[++i]);
                break;
            case OP_POP:
                printf("POP\n");
                break;
            // Symbol resolution
            case OP_RESOLVE:
                printf("RESOLVE\n");
                break;
            // Function calls
            case OP_CALL1:
                printf("CALL1\n");
                break;
            case OP_CALL2:
                printf("CALL2\n");
                break;
            case OP_CALLN:
                printf("CALLN %d\n", bc[++i]);
                break;
            case OP_CALLF:
                printf("CALLF\n");
                break;
            case OP_CALLS:
                printf("CALLS\n");
                break;
            case OP_CALLD:
                printf("CALLD %d\n", bc[++i]);
                break;
            default:
                printf("??? %d\n", bc[i]);
                break;
        }
    }
    printf("=====================\n");
}
