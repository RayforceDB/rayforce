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
#include "heap.h"
#include "error.h"
#include "symbols.h"
#include "runtime.h"
#include "format.h"
#include "cond.h"
#include "vary.h"
#include "nfo.h"

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
#define OP(ctx, op)   (AS_U8((ctx)->bc)[(ctx)->ip++] = (u8_t)(op))
#define CC(ctx, ct)   ({ OP(ctx, OP_PUSHC); OP(ctx, (ctx)->consts->len); push_obj(&(ctx)->consts, (ct)); })
#define CA(ctx, nn)   ({ OP(ctx, OP_DUP); OP(ctx, (nn)); })
#define CE(f)        { if ((f) == -1) return -1; }
#define SELF_SYM     (symbols_intern("self", 4))
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
        CC(cc, NULL_OBJ);
        return 0;
    }

    // compile condition
    CE(cc_expr(cc, lst[0]));

    // jmpz to else branch
    OP(cc, OP_JMPZ);
    j1 = cc->ip;
    OP(cc, 0);

    // compile then branch
    CE(cc_expr(cc, lst[1]));

    // jmp over else branch
    OP(cc, OP_JMP);
    j2 = cc->ip;
    OP(cc, 0);

    // patch jmpz offset
    AS_U8(cc->bc)[j1] = (u8_t)(cc->ip - j1 - 1);

    // compile else branch (or null)
    if (n == 3) {
        CE(cc_expr(cc, lst[2]));
    } else {
        CC(cc, NULL_OBJ);
    }

    // patch jmp offset
    AS_U8(cc->bc)[j2] = (u8_t)(cc->ip - j2 - 1);

    return 0;
}

// Compile function call
// expr is the original list expression (for debug info)
static i64_t cc_call(cc_ctx_t *cc, obj_p expr, obj_p *lst, i64_t n) {
    obj_p car = lst[0];
    i64_t i;

    lst++;
    n--;

    // Handle special form 'cond'/'if'
    if (is_cond_fn(car)) {
        return cc_cond(cc, lst, n);
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
            CC(cc, clone_obj(car));
            OP(cc, OP_CALF1);
            break;

        case TYPE_BINARY:
            if (n != 2)
                return -1;
            CC(cc, clone_obj(car));
            OP(cc, OP_CALF2);
            break;

        case TYPE_VARY:
            CC(cc, clone_obj(car));
            OP(cc, OP_CALF0);
            OP(cc, n);
            break;

        case TYPE_LAMBDA:
            CC(cc, clone_obj(car));
            OP(cc, OP_CALFN);
            break;

        case -TYPE_SYMBOL:
            if (car->i64 == SELF_SYM) {
                // Self-recursive call
                OP(cc, OP_CALFS);
            } else {
                // Dynamic call - need to resolve symbol at runtime
                CE(cc_expr(cc, car));
                OP(cc, OP_CALFD);
                OP(cc, n);
            }
            break;

        default:
            // For other types, try to evaluate at runtime
            CE(cc_expr(cc, car));
            OP(cc, OP_CALFD);
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
            // Check if it's a function argument
            i = find_raw(cc->args, &e->i64);
            if (i == NULL_I64 || i >= cc->args->len) {
                // Not a local - push symbol and dereference
                CC(cc, clone_obj(e));
                OP(cc, OP_DEREF);
            } else {
                // Local argument - duplicate from stack
                CA(cc, i);
            }
            return 0;

        case TYPE_LIST:
            if (e->len == 0) {
                // Empty list - push null
                CC(cc, NULL_OBJ);
                return 0;
            }
            // Function call - pass expression for debug info
            return cc_call(cc, e, AS_LIST(e), e->len);

        default:
            // Constant - push to constants pool
            CC(cc, clone_obj(e));
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
        CC(cc, NULL_OBJ);
        goto end;
    }

    if (cc_body(cc, lst, n) == -1) {
        return ray_err(ERR_TYPE);
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
    };

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
        return result;
    }

    // Store compiled bytecode, constants, and debug info in lambda
    fn->bc = cc.bc;
    fn->consts = cc.consts;
    fn->dbg = cc.dbg;

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
            case OP_RET:
                printf("RET\n");
                break;
            case OP_PUSHC:
                fmt = obj_fmt(consts[bc[++i]], B8_FALSE);
                printf("PUSHC %lld (%.*s)\n", (i64_t)bc[i], (i32_t)fmt->len, AS_C8(fmt));
                drop_obj(fmt);
                break;
            case OP_DUP:
                printf("DUP %d\n", bc[++i]);
                break;
            case OP_POP:
                printf("POP\n");
                break;
            case OP_JMPZ:
                printf("JMPZ +%d\n", bc[++i]);
                break;
            case OP_JMP:
                printf("JMP +%d\n", bc[++i]);
                break;
            case OP_DEREF:
                printf("DEREF\n");
                break;
            case OP_CALF1:
                printf("CALF1\n");
                break;
            case OP_CALF2:
                printf("CALF2\n");
                break;
            case OP_CALF0:
                printf("CALF0 %d\n", bc[++i]);
                break;
            case OP_CALFN:
                printf("CALFN\n");
                break;
            case OP_CALFS:
                printf("CALFS\n");
                break;
            case OP_CALFD:
                printf("CALFD %d\n", bc[++i]);
                break;
            default:
                printf("??? %d\n", bc[i]);
                break;
        }
    }
    printf("=====================\n");
}
