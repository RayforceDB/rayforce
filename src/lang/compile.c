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

#include "lang/eval.h"
#include "lang/env.h"
#include "lang/nfo.h"
#include "table/sym.h"   /* ray_read_sym — capture names */
#include <stdbool.h>
#include <string.h>

/* ── Compiler state ──
 * Internal buffers are ray_t objects whose data area holds the raw
 * bytes / pointers. This avoids calling malloc/free. */
typedef struct {
    ray_t    *code_obj;   /* RAY_U8 vector used as growable byte buffer */
    uint8_t *code;       /* == ray_data(code_obj) */
    int32_t  code_len;
    int32_t  code_cap;

    ray_t    *consts_obj; /* RAY_LIST used as growable pointer array */
    ray_t   **consts;     /* == ray_data(consts_obj) */
    int32_t  n_consts;
    int32_t  consts_cap;

    struct { int64_t sym_id; int32_t slot; } locals[256];
    int32_t  n_locals;
    int32_t  n_fixed;    /* params: the slots that are always bound */
    int32_t  max_locals;
    bool     error;
    ray_t   *lambda;     /* the lambda being compiled (for 'self' resolution) */

    ray_t    *dbg_obj;   /* I64 vector: pairs of [offset, span.id] */
    int32_t  trap_depth;  /* open OP_TRAP frames in current lambda */
} compiler_t;

static void compile_expr(compiler_t *c, ray_t *ast);
static bool compile_fn_template(compiler_t *c, ray_t *ast);
static bool compile_lambda(ray_t *lambda, ray_t *capnames);

static bool compiler_init(compiler_t *c) {
    memset(c, 0, sizeof(*c));
    c->code_cap = 256;
    c->code_obj = ray_alloc(c->code_cap);
    if (!c->code_obj) return false;
    c->code_obj->type = RAY_U8;
    c->code_obj->len = 0;
    c->code = (uint8_t *)ray_data(c->code_obj);

    c->consts_cap = 16;
    c->consts_obj = ray_alloc(c->consts_cap * sizeof(ray_t *));
    if (!c->consts_obj) { ray_release(c->code_obj); return false; }
    c->consts_obj->type = RAY_LIST;
    c->consts_obj->len = 0;
    c->consts = (ray_t **)ray_data(c->consts_obj);
    memset(c->consts, 0, c->consts_cap * sizeof(ray_t *));
    return true;
}

static void compiler_destroy(compiler_t *c) {
    for (int32_t i = 0; i < c->n_consts; i++)
        if (c->consts[i]) ray_release(c->consts[i]);
    ray_release(c->consts_obj);
    ray_release(c->code_obj);
}

/* ── Debug info helpers ── */
static void dbg_append(compiler_t* c, int32_t offset, int64_t span_id) {
    if (!c->dbg_obj) {
        c->dbg_obj = ray_vec_new(RAY_I64, 0);
        if (!c->dbg_obj) return;
    }
    int64_t off64 = (int64_t)offset;
    c->dbg_obj = ray_vec_append(c->dbg_obj, &off64);
    c->dbg_obj = ray_vec_append(c->dbg_obj, &span_id);
}

#define EMIT_DBG(c, ast) do { \
    if ((c)->lambda && LAMBDA_NFO((c)->lambda)) { \
        ray_span_t _sp = ray_nfo_get(LAMBDA_NFO((c)->lambda), (ast)); \
        if (_sp.id != 0) dbg_append(c, (c)->code_len, _sp.id); \
    } \
} while(0)

/* ── Emit helpers ── */
static void emit(compiler_t *c, uint8_t byte) {
    if (c->code_len >= c->code_cap) {
        int32_t new_cap = c->code_cap * 2;
        ray_t *new_obj = ray_alloc(new_cap);
        if (!new_obj) { c->error = true; return; }
        new_obj->type = RAY_U8;
        new_obj->len = 0;
        memcpy(ray_data(new_obj), c->code, c->code_len);
        ray_release(c->code_obj);
        c->code_obj = new_obj;
        c->code = (uint8_t *)ray_data(new_obj);
        c->code_cap = new_cap;
    }
    c->code[c->code_len++] = byte;
}

static void emit_const(compiler_t *c, int32_t idx) {
    if (idx < 256) {
        emit(c, OP_LOADCONST);
        emit(c, (uint8_t)idx);
    } else {
        emit(c, OP_LOADCONST_W);
        emit(c, (uint8_t)(idx >> 8));
        emit(c, (uint8_t)(idx & 0xFF));
    }
}

/* ── Constant pool ── */
static int32_t add_constant(compiler_t *c, ray_t *value) {
    for (int32_t i = 0; i < c->n_consts; i++) {
        ray_t *v = c->consts[i];
        if (v == value) return i;
        if (v->type == value->type && ray_is_atom(v)) {
            if (v->type == -RAY_I64 && v->i64 == value->i64) return i;
            if (v->type == -RAY_F64 && v->f64 == value->f64) return i;
            if (v->type == -RAY_BOOL && v->b8 == value->b8) return i;
            if (v->type == -RAY_SYM && v->i64 == value->i64 &&
                v->attrs == value->attrs) return i;
        }
    }
    if (c->n_consts >= c->consts_cap) {
        int32_t new_cap = c->consts_cap * 2;
        ray_t *new_obj = ray_alloc(new_cap * sizeof(ray_t *));
        if (!new_obj || RAY_IS_ERR(new_obj)) { c->error = true; return c->n_consts; }
        new_obj->type = RAY_LIST;
        new_obj->len = 0;
        ray_t **new_arr = (ray_t **)ray_data(new_obj);
        memcpy(new_arr, c->consts, c->n_consts * sizeof(ray_t *));
        memset(new_arr + c->n_consts, 0, (new_cap - c->n_consts) * sizeof(ray_t *));
        ray_release(c->consts_obj);
        c->consts_obj = new_obj;
        c->consts = new_arr;
        c->consts_cap = new_cap;
    }
    ray_retain(value);
    c->consts[c->n_consts] = value;
    return c->n_consts++;
}

/* ── Local variable tracking ── */
static int32_t find_local(compiler_t *c, int64_t sym_id) {
    for (int32_t i = c->n_locals - 1; i >= 0; i--)
        if (c->locals[i].sym_id == sym_id) return c->locals[i].slot;
    return -1;
}

static int32_t add_local(compiler_t *c, int64_t sym_id) {
    if (c->n_locals >= 256) return -1;
    int32_t slot = c->n_locals;
    c->locals[c->n_locals].sym_id = sym_id;
    c->locals[c->n_locals].slot = slot;
    c->n_locals++;
    if (c->n_locals > c->max_locals) c->max_locals = c->n_locals;
    return slot;
}

/* ── Jump helpers ── */
static int32_t emit_jump(compiler_t *c, uint8_t opcode) {
    emit(c, opcode);
    int32_t patch_pos = c->code_len;
    emit(c, 0);
    emit(c, 0);
    return patch_pos;
}

/* Write a jump displacement into the 2-byte operand at `pos`, which is
 * relative to the instruction's end (pos + 2).  Out of int16_t range sets
 * c->error, aborting bytecode emission so the lambda falls back to the
 * tree-walking interpreter — the same graceful degradation as the other
 * compile-time bailouts. */
static void write_jump_offset(compiler_t *c, int32_t pos, int32_t target) {
    int32_t raw = target - pos - 2;
    if (raw > 32767 || raw < -32768) { c->error = true; return; }
    int16_t offset = (int16_t)raw;
    c->code[pos]     = (uint8_t)((uint16_t)offset >> 8);
    c->code[pos + 1] = (uint8_t)(offset & 0xFF);
}

/* Resolve a forward jump emitted earlier, now that its target is here. */
static void patch_jump(compiler_t *c, int32_t pos) {
    write_jump_offset(c, pos, c->code_len);
}

/* Emit an unconditional jump BACKWARD to an already-emitted address — the
 * mirror of patch_jump, where the target is known up front and the
 * displacement comes out negative.  The VM's op_jmp checks for a pending
 * interrupt whenever the offset is negative, so a runaway loop built from
 * this stays Ctrl-C-able. */
static void emit_jump_back(compiler_t *c, int32_t target) {
    emit(c, OP_JMP);
    int32_t pos = c->code_len;
    emit(c, 0);
    emit(c, 0);
    write_jump_offset(c, pos, target);
}

/* Cached sym IDs for special forms */
static _Thread_local int64_t sf_set = -1, sf_let = -1, sf_if = -1, sf_do = -1, sf_while = -1, sf_times = -1, sf_fn = -1, sf_self = -1, sf_try = -1, sf_return = -1, sf_null = -1;
static _Thread_local int64_t sf_eval = -1, sf_resolve = -1;

static void init_sf_syms(void) {
    if (sf_set >= 0) return;
    sf_set  = ray_sym_intern("set", 3);
    sf_let  = ray_sym_intern("let", 3);
    sf_if   = ray_sym_intern("if",  2);
    sf_do   = ray_sym_intern("do",  2);
    sf_while= ray_sym_intern("while", 5);
    sf_times= ray_sym_intern("times", 5);
    sf_fn   = ray_sym_intern("fn",  2);
    sf_self = ray_sym_intern("self", 4);
    sf_try  = ray_sym_intern("try",  3);
    sf_return = ray_sym_intern("return", 6);
    sf_null   = ray_sym_intern("null", 4);
    sf_eval    = ray_sym_intern("eval", 4);
    sf_resolve = ray_sym_intern("resolve", 7);
}

/* Does `ast` mention any of the current locals — or `self`, or a
 * dynamic resolver (`eval` / `resolve`) that could reach them by a
 * constructed name?  Decides whether a special-form call needs its
 * locals materialized into a scope frame around the dynamic eval.
 * Quoted syms count: forms like (alter 'v ...) resolve them through
 * the env, where a tree-walk local would shadow a global.  Conservative
 * by construction — descends lists, dicts, and sym vectors. */
static bool ast_refs_locals(compiler_t *c, ray_t *ast);
static bool sym_is_local_ref(compiler_t *c, int64_t id) {
    if (id == sf_self || id == sf_eval || id == sf_resolve) return true;
    for (int32_t i = c->n_locals - 1; i >= 0; i--)
        if (c->locals[i].sym_id == id) return true;   /* params, lets, captures */
    return false;
}
static bool ast_refs_locals(compiler_t *c, ray_t *ast) {
    if (!ast) return false;
    if (ast->type == -RAY_SYM)
        return sym_is_local_ref(c, ast->i64);
    if (ast->type == RAY_SYM) {  /* sym vector */
        const int64_t *ids = (const int64_t*)ray_data(ast);
        for (int64_t i = 0; i < ast->len; i++)
            if (sym_is_local_ref(c, ids[i])) return true;
        return false;
    }
    if (ast->type == RAY_LIST) {
        ray_t **elems = (ray_t**)ray_data(ast);
        for (int64_t i = 0; i < ast->len; i++)
            if (ast_refs_locals(c, elems[i])) return true;
        return false;
    }
    if (ast->type == RAY_DICT) {  /* slot[0]=keys, slot[1]=vals */
        ray_t **slots = (ray_t**)ray_data(ast);
        return ast_refs_locals(c, slots[0]) || ast_refs_locals(c, slots[1]);
    }
    return false;
}


/* ── Compile a list (special form or function call) ── */
static void compile_list(compiler_t *c, ray_t *ast) {
    if (c->error) return;
    EMIT_DBG(c, ast);
    int64_t n = ray_len(ast);
    if (n == 0) { c->error = true; return; }
    ray_t **elems = (ray_t **)ray_data(ast);
    ray_t *head = elems[0];

    init_sf_syms();

    /* A local (parameter, let or capture) named like a special form or a
     * builtin is the callee, as it is on the tree walker, where the name
     * resolves through the scope frames first. */
    bool head_local = head->type == -RAY_SYM && !(head->attrs & ATTR_QUOTED) &&
                      find_local(c, head->i64) >= 0;

    /* Check for special forms by name (name ref = unflagged default) */
    if (head->type == -RAY_SYM && !(head->attrs & ATTR_QUOTED) && !head_local) {
        int64_t sym_id = head->i64;

        /* (set name value) — bind in the global env.  Compile the value
         * expression (so parameter/local slot references resolve in the
         * current frame), then emit OP_STOREGLOBAL to bind the result by
         * name.  The value is left on the stack as the form's result.
         *
         * This previously emitted OP_CALLD on the whole form, which
         * tree-walked the value against the global env and so could not
         * see a compiled lambda's slot-bound parameters: `(fn [a] (set b a))`
         * raised `name: 'a' undefined`.  A non-symbol name aborts bytecode
         * emission and falls back to the interpreter, which raises the
         * proper `type` error via ray_set_fn. */
        if (sym_id == sf_set && n == 3) {
            ray_t *name_obj = elems[1];
            if (name_obj->type != -RAY_SYM) { c->error = true; return; }
            compile_expr(c, elems[2]);
            /* Match ray_set_fn: globals must never retain a single-use lazy
             * DAG handle.  Materializing one alias consumes its graph and
             * would leave every other global read pointing at a spent handle. */
            emit(c, OP_FORCE);
            int32_t idx = add_constant(c, name_obj);
            if (idx < 256) {
                emit(c, OP_STOREGLOBAL);
                emit(c, (uint8_t)idx);
            } else {
                emit(c, OP_STOREGLOBAL_W);
                emit(c, (uint8_t)(idx >> 8));
                emit(c, (uint8_t)(idx & 0xFF));
            }
            return;
        }

        /* (let name value) — compile value, store in local slot.
         * Reserved names (`.sys.*`, `.os.*`, `.csv.*`, `.ipc.*`) are
         * refused here so a compiled lambda can't shadow a builtin
         * through its local-slot table — the same guard
         * ray_env_set_local enforces on the tree-walking path.
         * Setting c->error aborts bytecode emission; call_lambda
         * then falls back to the tree-walking interpreter which
         * raises the proper `reserve` error via ray_let_fn.
         * The five `.ipc.on.*` connection-hook names are exempt —
         * they're user-settable, so a `let .ipc.on.open ...` in a
         * compiled body must emit the same OP_STOREENV as any other
         * local binding.  Without this, the bytecode path would
         * abort and silently fall back to the interpreter on every
         * such write. */
        if (sym_id == sf_let && n == 3) {
            ray_t *name_obj = elems[1];
            if (name_obj->type != -RAY_SYM ||
                (ray_sym_is_reserved(name_obj->i64) &&
                 !ray_sym_is_ipc_hook(name_obj->i64))) {
                c->error = true;
                return;
            }
            compile_expr(c, elems[2]);
            /* Materialize a lazy value before binding — the interpreter's
             * ray_let_fn does the same.  A lazy handle is single-use
             * (materialization consumes its deferred graph), so a local
             * that aliased one would break on its SECOND read (e.g.
             * `(let v (first xs)) (if (> v 0) v 0)`: the compare consumes
             * the lazy, the branch then reloads a dead handle). */
            emit(c, OP_FORCE);
            emit(c, OP_DUP);
            int32_t slot = find_local(c, name_obj->i64);
            if (slot < 0) slot = add_local(c, name_obj->i64);
            if (slot < 0) { c->error = true; return; }
            emit(c, OP_STOREENV);
            emit(c, (uint8_t)slot);
            return;
        }

        /* (if cond then else?) */
        if (sym_id == sf_if && n >= 3) {
            compile_expr(c, elems[1]);
            /* Match ray_cond_fn: truthiness belongs to the materialized value,
             * not to the non-NULL lazy handle that happens to contain it. */
            emit(c, OP_FORCE);
            int32_t jmpf_pos = emit_jump(c, OP_JMPF);
            compile_expr(c, elems[2]);
            if (n >= 4) {
                int32_t jmp_pos = emit_jump(c, OP_JMP);
                patch_jump(c, jmpf_pos);
                compile_expr(c, elems[3]);
                patch_jump(c, jmp_pos);
            } else {
                int32_t jmp_pos = emit_jump(c, OP_JMP);
                patch_jump(c, jmpf_pos);
                ray_t *zero = ray_alloc(0);
                zero->type = -RAY_I64;
                zero->i64 = 0;
                int32_t idx = add_constant(c, zero);
                ray_release(zero);
                emit_const(c, idx);
                patch_jump(c, jmp_pos);
            }
            return;
        }

        /* (do expr1 expr2 ...) */
        if (sym_id == sf_do && n >= 2) {
            for (int64_t i = 1; i < n; i++) {
                if (i > 1) emit(c, OP_POP);
                compile_expr(c, elems[i]);
            }
            return;
        }

        /* (while cond body...) — the only backward branch the compiler
         * emits.  Body values are discarded (OP_POP each), and the form
         * yields null however many times it iterated, matching
         * ray_while_fn.  No scope is pushed: `let` in the body writes this
         * frame's local slot, so loop-carried state survives the pass
         * exactly as it does on the tree-walking path.
         *
         * Compiling the body inline — rather than letting this fall through
         * to the generic special-form path below — is what keeps `return`
         * working inside a loop: it reaches the sf_return case and unwinds
         * the lambda, instead of degrading to the tree walker's identity
         * `return` (issue 588). */
        if (sym_id == sf_while && n >= 2) {
            int32_t top = c->code_len;
            compile_expr(c, elems[1]);
            /* Truthiness belongs to the materialized value, not to the
             * non-NULL lazy handle containing it — same rule as sf_if. */
            emit(c, OP_FORCE);
            int32_t jmpf_pos = emit_jump(c, OP_JMPF);
            for (int64_t i = 2; i < n; i++) {
                compile_expr(c, elems[i]);
                emit(c, OP_POP);
            }
            emit_jump_back(c, top);
            patch_jump(c, jmpf_pos);
            int32_t idx = add_constant(c, RAY_NULL_OBJ);
            emit_const(c, idx);
            return;
        }

        /* (times n body...) — a counted loop, desugared onto the same
         * backward branch `while` uses.
         *
         * The count is evaluated ONCE into a hidden local slot, so the
         * bound is fixed on entry however the body mutates its source.
         * ray_times_norm_fn type-checks it and clamps a negative bound to
         * zero, which is what lets the per-pass test be a bare truthiness
         * check on the counter: 0 is falsy (is_truthy), everything else is
         * truthy, so the loop needs no comparison call and a negative bound
         * cannot run away.
         *
         * Both helpers are pushed as constant-pool objects rather than
         * resolved by name, so the loop's own arithmetic is invisible to
         * user code and immune to an override of `-` or `>`.  The counter's
         * slot is likewise addressed by index, never by name, and its
         * sym contains a space so no source token can collide with it —
         * which is what keeps nested `times` counters apart.
         *
         * As with `while`, compiling the body inline is what keeps
         * `return` unwinding the enclosing lambda from inside the loop. */
        if (sym_id == sf_times && n >= 2) {
            ray_t *norm_fn = ray_fn_unary("times norm", RAY_FN_NONE, ray_times_norm_fn);
            ray_t *dec_fn  = ray_fn_unary("times dec",  RAY_FN_NONE, ray_times_dec_fn);
            if (!norm_fn || RAY_IS_ERR(norm_fn) || !dec_fn || RAY_IS_ERR(dec_fn)) {
                if (norm_fn && !RAY_IS_ERR(norm_fn)) ray_release(norm_fn);
                if (dec_fn  && !RAY_IS_ERR(dec_fn))  ray_release(dec_fn);
                c->error = true;
                return;
            }
            int32_t norm_idx = add_constant(c, norm_fn);
            int32_t dec_idx  = add_constant(c, dec_fn);
            ray_release(norm_fn);
            ray_release(dec_fn);
            int32_t cslot = add_local(c, ray_sym_intern("times ctr", 9));
            if (cslot < 0 || c->error) { c->error = true; return; }

            /* counter = normalize(<count>) */
            emit_const(c, norm_idx);
            compile_expr(c, elems[1]);
            emit(c, OP_CALL1);
            emit(c, OP_STOREENV);
            emit(c, (uint8_t)cslot);

            int32_t top = c->code_len;
            emit(c, OP_LOADENV);
            emit(c, (uint8_t)cslot);
            int32_t jmpf_pos = emit_jump(c, OP_JMPF);
            for (int64_t i = 2; i < n; i++) {
                compile_expr(c, elems[i]);
                emit(c, OP_POP);
            }
            /* counter = counter - 1 */
            emit_const(c, dec_idx);
            emit(c, OP_LOADENV);
            emit(c, (uint8_t)cslot);
            emit(c, OP_CALL1);
            emit(c, OP_STOREENV);
            emit(c, (uint8_t)cslot);
            emit_jump_back(c, top);
            patch_jump(c, jmpf_pos);
            int32_t null_idx = add_constant(c, RAY_NULL_OBJ);
            emit_const(c, null_idx);
            return;
        }

        /* (fn [params] body...) — a closure template compiled once here
         * and instantiated per evaluation (OP_MKCLOSURE); the dynamic
         * eval below is the fallback when the literal cannot compile. */
        if (sym_id == sf_fn && n >= 3) {
            if (compile_fn_template(c, ast)) return;
            if (c->error) return;
            if (ast_refs_locals(c, ast)) {
                ray_t *syms = ray_alloc((size_t)c->n_locals * sizeof(int64_t));
                if (!syms || RAY_IS_ERR(syms)) { c->error = true; return; }
                syms->type = RAY_I64;
                syms->len = c->n_locals;
                int64_t *ids = (int64_t*)ray_data(syms);
                for (int32_t i = 0; i < c->n_locals; i++)
                    ids[i] = c->locals[i].sym_id;
                int32_t syms_idx = add_constant(c, syms);
                ray_release(syms);
                if (c->error) return;
                emit_const(c, syms_idx);
                emit(c, OP_SCOPE_BEGIN);
                int32_t idx = add_constant(c, ast);
                emit_const(c, idx);
                emit(c, OP_CALLD);
                emit(c, 0);
                emit_const(c, syms_idx);
                emit(c, OP_SCOPE_END);
                return;
            }
            int32_t idx = add_constant(c, ast);
            emit_const(c, idx);
            emit(c, OP_CALLD);
            emit(c, 0);
            return;
        }

        /* (try body handler) — compile to OP_TRAP/OP_TRAP_END */
        if (sym_id == sf_try && n == 3) {
            /* Reserve a hidden local for err_val */
            int32_t err_slot = add_local(c, -1);
            if (err_slot < 0) { c->error = true; return; }

            int32_t trap_pos = emit_jump(c, OP_TRAP);
            c->trap_depth++;
            compile_expr(c, elems[1]);       /* body */
            c->trap_depth--;
            emit(c, OP_TRAP_END);
            int32_t jmp_pos = emit_jump(c, OP_JMP);
            patch_jump(c, trap_pos);         /* handler starts here */
            /* err_val is on stack (pushed by vm_error_cleanup).
             * Stash it, compile handler fn, reload err_val, call. */
            emit(c, OP_STOREENV);
            emit(c, (uint8_t)err_slot);
            compile_expr(c, elems[2]);       /* handler (fn or fallback value) */
            emit(c, OP_LOADENV);
            emit(c, (uint8_t)err_slot);
            emit(c, OP_TRYH);                /* callable → call(err); else value */
            patch_jump(c, jmp_pos);          /* end */
            return;
        }

        /* (return) | (return x) — early exit from enclosing compiled
         * lambda. (return) pushes RAY_NULL_OBJ explicitly because
         * OP_RET takes top-of-stack as the result; if (return) sits
         * inside an outer expression that has already pushed values
         * (e.g. (+ 1 (return))), relying on OP_RET's stack-empty
         * fallback would return a stale value. */
        if (sym_id == sf_return && (n == 1 || n == 2)) {
            if (n == 2) {
                compile_expr(c, elems[1]);
            } else {
                int32_t idx = add_constant(c, RAY_NULL_OBJ);
                emit_const(c, idx);
            }
            for (int32_t i = 0; i < c->trap_depth; i++)
                emit(c, OP_TRAP_END);
            emit(c, OP_RET);
            return;
        }
    }

    /* Self-recursive call: emit OP_CALLS (lean frame reuse, no fn object) */
    if (head->type == -RAY_SYM && !(head->attrs & ATTR_QUOTED) && !head_local &&
        head->i64 == sf_self) {
        int64_t argc = n - 1;
        if (argc > 64) { c->error = true; return; }
        for (int64_t i = 1; i < n; i++)
            compile_expr(c, elems[i]);
        emit(c, OP_CALLS);
        emit(c, (uint8_t)argc);
        return;
    }

    /* Look up head at compile time to determine call type */
    ray_t *fn = NULL;
    if (head->type == -RAY_SYM && !(head->attrs & ATTR_QUOTED) && !head_local)
        fn = ray_env_get(head->i64);

    /* Unrecognized special form: dynamic eval on the entire form.  The
     * special form re-evaluates its argument ASTs via the tree walker,
     * which resolves names through scope frames + globals and cannot
     * see this frame's local slots — so materialize the locals known
     * at this point (plus self) into a scope frame around the eval,
     * and sync any let-updates back into the slots afterwards. */
    if (fn && (fn->attrs & RAY_FN_SPECIAL_FORM)) {
        /* Fast path: a form that mentions no local (the common
         * globals-only case) needs no materialization — emit the bare
         * dynamic eval exactly as before. */
        if (!ast_refs_locals(c, ast)) {
            int32_t idx = add_constant(c, ast);
            emit_const(c, idx);
            emit(c, OP_CALLD);
            emit(c, 0);
            return;
        }

        ray_t *syms = ray_alloc((size_t)c->n_locals * sizeof(int64_t));
        if (!syms || RAY_IS_ERR(syms)) { c->error = true; return; }
        syms->type = RAY_I64;
        syms->len  = c->n_locals;
        int64_t *ids = (int64_t*)ray_data(syms);
        for (int32_t i = 0; i < c->n_locals; i++)
            ids[i] = c->locals[i].sym_id;   /* slot i <-> ids[i] */
        int32_t syms_idx = add_constant(c, syms);
        ray_release(syms);
        if (c->error) return;

        emit_const(c, syms_idx);
        emit(c, OP_SCOPE_BEGIN);
        int32_t idx = add_constant(c, ast);
        emit_const(c, idx);
        emit(c, OP_CALLD);
        emit(c, 0);
        emit_const(c, syms_idx);
        emit(c, OP_SCOPE_END);
        return;
    }

    /* `eval` and `resolve` read names from text at run time: the tree
     * walker that serves them resolves through the scope frames, so the
     * call runs inside a window that binds this frame's locals, captures
     * included, the way a dynamic special form does. */
    int32_t dyn_syms_idx = -1;
    if (head->type == -RAY_SYM && !(head->attrs & ATTR_QUOTED) && !head_local &&
        (head->i64 == sf_eval || head->i64 == sf_resolve) && c->n_locals > 0) {
        ray_t *syms = ray_alloc((size_t)c->n_locals * sizeof(int64_t));
        if (!syms || RAY_IS_ERR(syms)) { c->error = true; return; }
        syms->type = RAY_I64;
        syms->len  = c->n_locals;
        int64_t *ids = (int64_t*)ray_data(syms);
        for (int32_t i = 0; i < c->n_locals; i++) ids[i] = c->locals[i].sym_id;
        dyn_syms_idx = add_constant(c, syms);
        ray_release(syms);
        if (c->error) return;
        emit_const(c, dyn_syms_idx);
        emit(c, OP_SCOPE_BEGIN);
    }

    /* General function call: compile head, args, then dispatch.
     * If head resolved to a builtin at compile time, emit LOADCONST
     * instead of RESOLVE to skip the runtime hash lookup. */
    if (fn && (fn->type == RAY_UNARY || fn->type == RAY_BINARY || fn->type == RAY_VARY)) {
        int32_t idx = add_constant(c, fn);
        emit_const(c, idx);
    } else {
        compile_expr(c, head);
    }
    int64_t argc = n - 1;
    if (argc > 64) { c->error = true; return; }
    for (int64_t i = 1; i < n; i++)
        compile_expr(c, elems[i]);

    /* Record call-site span so errors point to the call expression, not the last arg */
    EMIT_DBG(c, ast);

    bool emitted = false;
    if (fn) {
        switch (fn->type) {
        case RAY_UNARY:
            if (argc == 1) { emit(c, OP_CALL1); emitted = true; }
            break;
        case RAY_BINARY:
            if (argc == 2) { emit(c, OP_CALL2); emitted = true; }
            break;
        case RAY_VARY:
            emit(c, OP_CALLN);
            emit(c, (uint8_t)argc);
            emitted = true;
            break;
        case RAY_LAMBDA:
            emit(c, OP_CALLF);
            emit(c, (uint8_t)argc);
            emitted = true;
            break;
        default:
            break;
        }
    }
    if (!emitted) {
        emit(c, OP_CALLF);
        emit(c, (uint8_t)argc);
    }
    if (dyn_syms_idx >= 0) {
        emit_const(c, dyn_syms_idx);
        emit(c, OP_SCOPE_END);
    }
}

/* ── Compile expression ── */
static void compile_expr(compiler_t *c, ray_t *ast) {
    if (c->error) return;
    if (!ast || RAY_IS_ERR(ast)) return;
    EMIT_DBG(c, ast);

    if (ray_is_atom(ast)) {
        if (ast->type == -RAY_SYM && !(ast->attrs & ATTR_QUOTED)) {
            /* The tree walker treats bare `null` as the singleton value, not
             * an environment lookup.  Compiled lambdas must do the same;
             * otherwise any branch returning null fails with a name error. */
            init_sf_syms();
            if (ast->i64 == sf_null) {
                emit_const(c, add_constant(c, RAY_NULL_OBJ));
                return;
            }
            int32_t slot = find_local(c, ast->i64);
            if (slot >= 0 && slot < c->n_fixed) {
                emit(c, OP_LOADENV);
                emit(c, (uint8_t)slot);
            } else if (slot >= 0) {
                /* A `let` slot may still be empty when its `let` has not
                 * run; the tree walker then finds the global of that name
                 * (or raises a name error), so the load falls back to it. */
                int32_t idx = add_constant(c, ast);
                emit(c, OP_LOADENVG);
                emit(c, (uint8_t)slot);
                emit(c, (uint8_t)(idx >> 8));
                emit(c, (uint8_t)(idx & 0xFF));
            } else if (ast->i64 == sf_self) {
                /* `self` as a value is the running lambda, as the tree
                 * walker binds it in every call frame. */
                emit(c, OP_LOADSELF);
            } else {
                int32_t idx = add_constant(c, ast);
                if (idx < 256) {
                    emit(c, OP_RESOLVE);
                    emit(c, (uint8_t)idx);
                } else {
                    emit(c, OP_RESOLVE_W);
                    emit(c, (uint8_t)(idx >> 8));
                    emit(c, (uint8_t)(idx & 0xFF));
                }
            }
            return;
        }
        int32_t idx = add_constant(c, ast);
        emit_const(c, idx);
        return;
    }

    if (ast->type != RAY_LIST) {
        int32_t idx = add_constant(c, ast);
        emit_const(c, idx);
        return;
    }

    if (ray_len(ast) == 0) {
        int32_t idx = add_constant(c, ast);
        emit_const(c, idx);
        return;
    }

    compile_list(c, ast);
}

/* ── Public API ── */
/* A closure compiles only when its captures have the shape the compiled
 * loads assume: SYM keys and a LIST of values of the same length.  Anything
 * else (a deserialized closure with a typed value vector, say) stays on the
 * tree walker, whose capture binding reports the type error. */
static bool closure_shape_ok(ray_t *closure) {
    if (!closure) return true;
    if (closure->type != RAY_DICT) return false;
    ray_t *keys = ray_dict_keys(closure);
    ray_t *vals = ray_dict_vals(closure);
    return keys && vals && keys->type == RAY_SYM && vals->type == RAY_LIST && keys->len == vals->len;
}

/* Every symbol id mentioned anywhere in `ast` (atoms, sym vectors, lists,
 * dicts), appended to the I64 vector *out without duplicates.  *dynamic is
 * set when the text mentions `eval` or `resolve`, which can reach a name
 * that is not spelled out. */
static bool ast_collect_syms(ray_t *ast, ray_t **out, bool *dynamic) {
    if (!ast || !*out) return false;
    if (ast->type == -RAY_SYM) {
        int64_t id = ast->i64;
        if (id == sf_eval || id == sf_resolve) *dynamic = true;
        const int64_t *have = (const int64_t*)ray_data(*out);
        for (int64_t i = 0; i < (*out)->len; i++) if (have[i] == id) return true;
        *out = ray_vec_append(*out, &id);
        return *out && !RAY_IS_ERR(*out);
    }
    if (ast->type == RAY_SYM) {
        for (int64_t i = 0; i < ast->len; i++) {
            int64_t id = ray_read_sym(ray_data(ast), i, RAY_SYM, ast->attrs);
            const int64_t *have = (const int64_t*)ray_data(*out);
            bool seen = false;
            for (int64_t k = 0; k < (*out)->len; k++) if (have[k] == id) { seen = true; break; }
            if (seen) continue;
            *out = ray_vec_append(*out, &id);
            if (!*out || RAY_IS_ERR(*out)) return false;
        }
        return true;
    }
    if (ast->type == RAY_LIST) {
        ray_t **elems = (ray_t**)ray_data(ast);
        for (int64_t i = 0; i < ast->len; i++)
            if (!ast_collect_syms(elems[i], out, dynamic)) return false;
        return true;
    }
    if (ast->type == RAY_DICT) {
        ray_t **slots = (ray_t**)ray_data(ast);
        return ast_collect_syms(slots[0], out, dynamic) && ast_collect_syms(slots[1], out, dynamic);
    }
    return true;
}

static bool syms_contain(ray_t *syms, int64_t id) {
    if (!syms) return false;
    const int64_t *ids = (const int64_t*)ray_data(syms);
    for (int64_t i = 0; i < syms->len; i++) if (ids[i] == id) return true;
    return false;
}

/* The capture names a closure body can reach: the closure dict's keys that
 * the body mentions (every key when the body uses eval or resolve), never
 * `self`.  A SYM vector the caller releases, or NULL for none. */
static ray_t *closure_capture_names(ray_t *lambda) {
    ray_t *closure = LAMBDA_CLOSURE(lambda);
    if (!closure) return NULL;
    ray_t *keys = ray_dict_keys(closure);
    if (!keys || keys->len == 0) return NULL;
    init_sf_syms();
    ray_t *mentioned = ray_vec_new(RAY_I64, 16);
    if (!mentioned || RAY_IS_ERR(mentioned)) return NULL;
    bool dynamic = false;
    bool ok = ast_collect_syms(LAMBDA_BODY(lambda), &mentioned, &dynamic);
    ray_t *names = ok ? ray_sym_vec_new(RAY_SYM_W64, keys->len) : NULL;
    if (names && !RAY_IS_ERR(names)) {
        int64_t *out = (int64_t*)ray_data(names);
        names->len = 0;
        for (int64_t i = 0; i < keys->len; i++) {
            int64_t id = ray_read_sym(ray_data(keys), i, RAY_SYM, keys->attrs);
            if (id == sf_self) continue;
            if (dynamic || syms_contain(mentioned, id)) out[names->len++] = id;
        }
    }
    if (mentioned) ray_release(mentioned);
    if (names && RAY_IS_ERR(names)) names = NULL;
    return names;
}

/* A `fn` literal inside a compiled body becomes a template compiled once,
 * here, against the enclosing locals its body mentions (all of them when
 * the body uses eval or resolve); OP_MKCLOSURE then makes each closure
 * share the template's bytecode and capture those slots.  Returns false,
 * without emitting, when the literal cannot be compiled this way; the
 * caller then falls back to evaluating the literal through the tree
 * walker. */
static bool compile_fn_template_at(compiler_t *c, ray_t *ast);
/* Templates compile their nested literals at once, each level on the C
 * stack; past this depth the literal falls back to the dynamic eval path,
 * whose closures compile one level at a time when they are called. */
static _Thread_local int g_template_depth = 0;
static bool compile_fn_template(compiler_t *c, ray_t *ast) {
    if (g_template_depth >= 32) return false;
    g_template_depth++;
    bool ok = compile_fn_template_at(c, ast);
    g_template_depth--;
    return ok;
}
static bool compile_fn_template_at(compiler_t *c, ray_t *ast) {
    ray_t **elems = (ray_t**)ray_data(ast);
    int64_t n = ray_len(ast);
    /* Parameters: a SYM vector; `[]` parses as an empty vector of another
     * type and a list of symbol atoms can occur too, both normalized to a
     * SYM vector here. */
    ray_t *params = elems[1];
    if (!params) return false;
    bool own_params = false;
    if (params->len == 0 && params->type != RAY_SYM && params->type > 0) {
        ray_t *pv = ray_sym_vec_new(RAY_SYM_W64, 1);
        if (!pv || RAY_IS_ERR(pv)) return false;
        pv->len = 0;
        params = pv;
        own_params = true;
    } else if (params->type == RAY_LIST) {
        ray_t **pe = (ray_t**)ray_data(params);
        for (int64_t i = 0; i < params->len; i++)
            if (!pe[i] || pe[i]->type != -RAY_SYM) return false;
        ray_t *pv = ray_sym_vec_new(RAY_SYM_W64, params->len > 0 ? params->len : 1);
        if (!pv || RAY_IS_ERR(pv)) return false;
        for (int64_t i = 0; i < params->len; i++) ((int64_t*)ray_data(pv))[i] = pe[i]->i64;
        pv->len = params->len;
        params = pv;
        own_params = true;
    } else if (params->type != RAY_SYM) {
        return false;
    }
    for (int64_t i = 0; i < params->len; i++)
        if (ray_sym_is_reserved(ray_read_sym(ray_data(params), i, RAY_SYM, params->attrs))) {
            if (own_params) ray_release(params);
            return false;
        }

    ray_t *mentioned = ray_vec_new(RAY_I64, 16);
    if (!mentioned || RAY_IS_ERR(mentioned)) { if (own_params) ray_release(params); return false; }
    bool dynamic = false, ok = true;
    for (int64_t i = 2; i < n && ok; i++) ok = ast_collect_syms(elems[i], &mentioned, &dynamic);
    if (!ok) { ray_release(mentioned); if (own_params) ray_release(params); return false; }

    /* Capture names and the enclosing slots they come from, in slot order. */
    ray_t *names = ray_sym_vec_new(RAY_SYM_W64, c->n_locals > 0 ? c->n_locals : 1);
    ray_t *slots = ray_vec_new(RAY_I64, c->n_locals > 0 ? c->n_locals : 1);
    if (!names || RAY_IS_ERR(names) || !slots || RAY_IS_ERR(slots)) {
        ray_release(mentioned);
        if (names && !RAY_IS_ERR(names)) ray_release(names);
        if (slots && !RAY_IS_ERR(slots)) ray_release(slots);
        if (own_params) ray_release(params);
        return false;
    }
    names->len = 0; slots->len = 0;
    for (int32_t i = 0; i < c->n_locals; i++) {
        int64_t id = c->locals[i].sym_id;
        if (id < 0 || id == sf_self) continue;
        if (!dynamic && !syms_contain(mentioned, id)) continue;
        bool dup = false;   /* a name rebound to a later slot: keep the latest */
        for (int64_t k = 0; k < names->len; k++)
            if (((int64_t*)ray_data(names))[k] == id) { ((int64_t*)ray_data(slots))[k] = c->locals[i].slot; dup = true; break; }
        if (dup) continue;
        ((int64_t*)ray_data(names))[names->len++] = id;
        ((int64_t*)ray_data(slots))[slots->len++] = c->locals[i].slot;
    }
    ray_release(mentioned);

    ray_t *body = ray_alloc((size_t)(n - 2) * sizeof(ray_t*));
    ray_t *tmpl = ray_alloc(LAMBDA_SLOTS * sizeof(ray_t*));
    if (!body || RAY_IS_ERR(body) || !tmpl || RAY_IS_ERR(tmpl)) {
        ray_release(names); ray_release(slots);
        if (own_params) ray_release(params);
        if (body && !RAY_IS_ERR(body)) { body->type = RAY_LIST; body->len = 0; ray_release(body); }
        if (tmpl && !RAY_IS_ERR(tmpl)) { tmpl->type = RAY_LAMBDA; memset(ray_data(tmpl), 0, LAMBDA_SLOTS * sizeof(ray_t*)); ray_release(tmpl); }
        return false;
    }
    body->type = RAY_LIST;
    body->len = n - 2;
    for (int64_t i = 2; i < n; i++) { ray_retain(elems[i]); ((ray_t**)ray_data(body))[i - 2] = elems[i]; }
    tmpl->type = RAY_LAMBDA;
    tmpl->attrs = 0;
    tmpl->len = 0;
    memset(ray_data(tmpl), 0, LAMBDA_SLOTS * sizeof(ray_t*));
    if (!own_params) ray_retain(params);   /* a fresh vector is owned already */
    LAMBDA_PARAMS(tmpl) = params;
    LAMBDA_BODY(tmpl) = body;
    if (c->lambda && LAMBDA_NFO(c->lambda)) { ray_retain(LAMBDA_NFO(c->lambda)); LAMBDA_NFO(tmpl) = LAMBDA_NFO(c->lambda); }

    if (!compile_lambda(tmpl, names->len ? names : NULL)) {
        ray_release(tmpl); ray_release(names); ray_release(slots);
        return false;
    }
    int32_t tidx = add_constant(c, tmpl);
    int32_t nidx = add_constant(c, names);
    int32_t sidx = add_constant(c, slots);
    ray_release(tmpl); ray_release(names); ray_release(slots);
    if (c->error) return false;
    emit_const(c, tidx);
    emit_const(c, nidx);
    emit_const(c, sidx);
    emit(c, OP_MKCLOSURE);
    return true;
}

void ray_compile(ray_t *lambda) {
    if (LAMBDA_IS_COMPILED(lambda)) return;
    if (!closure_shape_ok(LAMBDA_CLOSURE(lambda))) return;
    ray_t *names = closure_capture_names(lambda);
    (void)compile_lambda(lambda, names);
    if (names) ray_release(names);
}

/* Compile `lambda` with `capnames` (a SYM vector, or NULL) bound as locals
 * ahead of the body by one OP_BINDCAPS.  A parameter shadows a capture of
 * its name.  False when the body cannot be compiled (the lambda stays on
 * the tree walker). */
static bool compile_lambda(ray_t *lambda, ray_t *capnames) {
    compiler_t c;
    if (!compiler_init(&c)) return false;
    c.lambda = lambda;

    /* Register params as locals */
    ray_t *params_list = LAMBDA_PARAMS(lambda);
    int64_t param_count = ray_len(params_list);
    int64_t *param_ids = (int64_t*)ray_data(params_list);
    for (int64_t i = 0; i < param_count; i++) {
        if (add_local(&c, param_ids[i]) < 0) { c.error = true; break; }
    }
    /* Only the parameters are always bound.  A capture slot can be empty
     * (its `let` had not run when the closure was made), so it reads like
     * a `let` slot: the global of that name, or a name error. */
    c.n_fixed = c.n_locals;

    /* A closure's captures are locals of its body, bound before it runs:
     * a `let` of a captured name writes the same slot, a capture named
     * like a builtin is the callee, and a window (OP_SCOPE_BEGIN) binds
     * them with the other locals.  Only the names the body mentions take a
     * slot (closure_capture_names / compile_fn_template), so a closure's
     * recursion depth does not shrink with the bindings in scope.  One
     * OP_BINDCAPS binds them all, by name, from the closure dict.  A frame
     * holds 256 slots; a closure needing more stays on the tree walker. */
    if (!c.error && capnames && capnames->len > 0) {
        ray_t *pairs = ray_alloc((size_t)capnames->len * 2 * sizeof(int64_t));
        if (!pairs || RAY_IS_ERR(pairs)) { c.error = true; }
        else {
            pairs->type = RAY_I64;
            pairs->len  = 0;
            int64_t *pv = (int64_t*)ray_data(pairs);
            for (int64_t i = 0; i < capnames->len && !c.error; i++) {
                int64_t id = ray_read_sym(ray_data(capnames), i, RAY_SYM, capnames->attrs);
                if (find_local(&c, id) >= 0) continue;   /* a parameter shadows its capture */
                int32_t slot = add_local(&c, id);
                if (slot < 0) { c.error = true; break; }
                pv[pairs->len++] = id;
                pv[pairs->len++] = slot;
            }
            if (!c.error && pairs->len > 0) {
                int32_t idx = add_constant(&c, pairs);
                if (idx < 0 || idx > 255) c.error = true;
                else { emit(&c, OP_BINDCAPS); emit(&c, (uint8_t)idx); }
            }
            ray_release(pairs);
        }
    }

    /* Compile body expressions */
    ray_t *body = LAMBDA_BODY(lambda);
    int64_t body_count = ray_len(body);
    ray_t **body_exprs = (ray_t **)ray_data(body);
    for (int64_t i = 0; i < body_count; i++) {
        if (i > 0) emit(&c, OP_POP);
        compile_expr(&c, body_exprs[i]);
    }
    emit(&c, OP_RET);

    if (c.error) {
        if (c.dbg_obj) ray_release(c.dbg_obj);
        compiler_destroy(&c);
        return false;
    }

    /* Build bytecode vector */
    ray_t *bc = ray_alloc(c.code_len);
    if (!bc) { compiler_destroy(&c); return false; }
    bc->type = RAY_U8;
    bc->len = c.code_len;
    memcpy(ray_data(bc), c.code, c.code_len);

    /* Build constants list */
    ray_t *consts = ray_alloc(c.n_consts * sizeof(ray_t *));
    if (!consts) { ray_release(bc); compiler_destroy(&c); return false; }
    consts->type = RAY_LIST;
    consts->len = c.n_consts;
    ray_t **cpool = (ray_t **)ray_data(consts);
    for (int32_t i = 0; i < c.n_consts; i++) {
        ray_retain(c.consts[i]);
        cpool[i] = c.consts[i];
    }

    LAMBDA_BC(lambda) = bc;
    LAMBDA_CONSTS(lambda) = consts;
    LAMBDA_NLOCALS(lambda) = c.max_locals;
    lambda->attrs |= RAY_FN_COMPILED;

    if (c.dbg_obj) {
        LAMBDA_DBG(lambda) = c.dbg_obj;
        /* dbg_obj is now owned by the lambda, don't release it */
    }

    compiler_destroy(&c);
    return true;
}

ray_span_t ray_bc_dbg_get(ray_t* dbg, int32_t ip) {
    ray_span_t span = {0};
    if (!dbg || dbg->len == 0) return span;
    int64_t* data = (int64_t*)ray_data(dbg);
    int64_t n = dbg->len;
    int64_t best_offset = -1;
    for (int64_t i = 0; i < n; i += 2) {
        int64_t offset = data[i];
        if (offset <= ip && offset > best_offset) {
            best_offset = offset;
            span.id = data[i + 1];
        }
    }
    return span;
}

void ray_compile_reset(void) {
    sf_set = sf_let = sf_if = sf_do = sf_fn = sf_self = sf_try = sf_return = sf_null = -1;
    sf_eval = sf_resolve = -1;
}
