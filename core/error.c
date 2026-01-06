/*
 *   Copyright (c) 2023 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 */

#include "error.h"
#include "heap.h"
#include "string.h"
#include "util.h"
#include "eval.h"

// ============================================================================
// Error Code Names
// ============================================================================
static lit_p err_names[] = {
    "ok",      // EC_OK
    "type",    // EC_TYPE
    "arity",   // EC_ARITY
    "length",  // EC_LENGTH
    "domain",  // EC_DOMAIN
    "index",   // EC_INDEX
    "value",   // EC_VALUE
    "limit",   // EC_LIMIT
    "os",      // EC_OS
    "parse",   // EC_PARSE
    "nyi",     // EC_NYI
    "",        // EC_USER
};

RAY_ASSERT(sizeof(err_names) / sizeof(err_names[0]) == EC_MAX, "err_names must match err_code_t");

lit_p err_name(err_code_t code) {
    if (code >= EC_MAX)
        return "error";
    return err_names[code];
}

// ============================================================================
// Platform errno
// ============================================================================
static i32_t get_errno(nil_t) {
#ifdef OS_WINDOWS
    return (i32_t)GetLastError();
#else
    return errno;
#endif
}

// ============================================================================
// Error Creation
// ============================================================================

static inline obj_p err_alloc(err_code_t code) {
    obj_p obj = (obj_p)heap_alloc(sizeof(struct obj_t));
    obj->mmod = MMOD_INTERNAL;
    obj->type = TYPE_ERR;
    obj->rc = 1;
    obj->attrs = (u8_t)code;
    obj->i64 = 0;  // Clear context
    return obj;
}

obj_p err_raw(err_code_t code) { return err_alloc(code); }

// Helper to pack i32 into v1-v4
static inline void ctx_set_i32(err_ctx_t* ctx, i32_t val) {
    ctx->v1 = (i8_t)(val & 0xFF);
    ctx->v2 = (i8_t)((val >> 8) & 0xFF);
    ctx->v3 = (i8_t)((val >> 16) & 0xFF);
    ctx->v4 = (i8_t)((val >> 24) & 0xFF);
}

obj_p err_type(i8_t expected, i8_t actual, u8_t arg, u8_t field) {
    obj_p err = err_alloc(EC_TYPE);
    err_ctx_t* ctx = (err_ctx_t*)&err->i64;
    ctx->arg = arg;
    ctx->field = field;
    ctx->v1 = expected;
    ctx->v2 = actual;
    return err;
}

obj_p err_arity(i8_t need, i8_t have, u8_t arg) {
    obj_p err = err_alloc(EC_ARITY);
    err_ctx_t* ctx = (err_ctx_t*)&err->i64;
    ctx->arg = arg;
    ctx->v1 = need;
    ctx->v2 = have;
    return err;
}

obj_p err_length(i8_t need, i8_t have, u8_t arg, u8_t arg2, u8_t field, u8_t field2) {
    obj_p err = err_alloc(EC_LENGTH);
    err_ctx_t* ctx = (err_ctx_t*)&err->i64;
    ctx->arg = arg;
    ctx->arg2 = arg2;
    ctx->field = field;
    ctx->field2 = field2;
    ctx->v1 = need;
    ctx->v2 = have;
    return err;
}

obj_p err_index(i8_t idx, i8_t len, u8_t arg, u8_t field) {
    obj_p err = err_alloc(EC_INDEX);
    err_ctx_t* ctx = (err_ctx_t*)&err->i64;
    ctx->arg = arg;
    ctx->field = field;
    ctx->v1 = idx;
    ctx->v2 = len;
    return err;
}

obj_p err_domain(u8_t arg, u8_t field) {
    obj_p err = err_alloc(EC_DOMAIN);
    err_ctx_t* ctx = (err_ctx_t*)&err->i64;
    ctx->arg = arg;
    ctx->field = field;
    return err;
}

obj_p err_value(i64_t sym) {
    obj_p err = err_alloc(EC_VALUE);
    err->i64 = sym;  // Store full 64-bit symbol pointer directly
    return err;
}

obj_p err_limit(i32_t limit) {
    obj_p err = err_alloc(EC_LIMIT);
    err_ctx_t* ctx = (err_ctx_t*)&err->i64;
    ctx->v1 = (i8_t)(limit & 0xFF);
    ctx->v2 = (i8_t)((limit >> 8) & 0xFF);
    return err;
}

obj_p err_os(nil_t) {
    obj_p err = err_alloc(EC_OS);
    err_ctx_t* ctx = (err_ctx_t*)&err->i64;
    ctx_set_i32(ctx, get_errno());
    return err;
}

obj_p err_user(lit_p msg) {
    i64_t len = msg ? strlen(msg) : 0;
    obj_p obj = (obj_p)heap_alloc(sizeof(struct obj_t) + len + 1);
    obj->mmod = MMOD_INTERNAL;
    obj->type = TYPE_ERR;
    obj->rc = 1;
    obj->attrs = (u8_t)EC_USER;
    obj->len = len;
    obj->i64 = 0;
    if (len > 0)
        memcpy((str_p)(obj + 1), msg, len + 1);
    else
        ((str_p)(obj + 1))[0] = '\0';
    return obj;
}

obj_p err_nyi(i8_t type) {
    obj_p err = err_alloc(EC_NYI);
    err_ctx_t* ctx = (err_ctx_t*)&err->i64;
    ctx->v1 = type;
    return err;
}

obj_p err_parse(nil_t) { return err_alloc(EC_PARSE); }

// ============================================================================
// Error Decoding
// ============================================================================

err_code_t err_code(obj_p err) {
    if (err == NULL_OBJ || err->type != TYPE_ERR)
        return EC_OK;
    return (err_code_t)err->attrs;
}

err_ctx_t* err_ctx(obj_p err) {
    static err_ctx_t empty = {0};
    if (err == NULL_OBJ || err->type != TYPE_ERR)
        return &empty;
    return (err_ctx_t*)&err->i64;
}

lit_p err_get_message(obj_p err) {
    if (err == NULL_OBJ || err->type != TYPE_ERR)
        return "";
    if (err_code(err) != EC_USER)
        return "";
    if (err->len == 0)
        return "";
    return (lit_p)(err + 1);
}

// ============================================================================
// String-based API (for deserialization)
// ============================================================================

obj_p ray_err(lit_p msg) { return err_user(msg); }

lit_p ray_err_msg(obj_p err) {
    if (err == NULL_OBJ || err->type != TYPE_ERR)
        return "";

    err_code_t code = err_code(err);
    if (code == EC_USER && err->len > 0)
        return (lit_p)(err + 1);

    return err_name(code);
}

// ============================================================================
// Error Info (for IPC serialization)
// ============================================================================

// Helper to count non-zero positional fields
static inline i64_t ctx_pos_count(err_ctx_t* ctx) {
    i64_t n = 0;
    if (ctx->arg) n++;
    if (ctx->arg2) n++;
    if (ctx->field) n++;
    if (ctx->field2) n++;
    return n;
}

// Helper to add positional context to keys/vals
static inline void ctx_add_pos(err_ctx_t* ctx, obj_p* keys, obj_p* vals, i64_t* idx) {
    if (ctx->arg) {
        ins_sym(keys, *idx, "arg");
        AS_LIST(*vals)[(*idx)++] = i32(ctx->arg);
    }
    if (ctx->arg2) {
        ins_sym(keys, *idx, "arg2");
        AS_LIST(*vals)[(*idx)++] = i32(ctx->arg2);
    }
    if (ctx->field) {
        ins_sym(keys, *idx, "field");
        AS_LIST(*vals)[(*idx)++] = i32(ctx->field);
    }
    if (ctx->field2) {
        ins_sym(keys, *idx, "field2");
        AS_LIST(*vals)[(*idx)++] = i32(ctx->field2);
    }
}

obj_p err_info(obj_p err) {
    if (err == NULL_OBJ || err->type != TYPE_ERR)
        return NULL_OBJ;

    err_code_t code = err_code(err);
    err_ctx_t* ctx = err_ctx(err);
    vm_p vm = VM;
    obj_p trace = (vm && vm->trace != NULL_OBJ) ? vm->trace : NULL_OBJ;
    lit_p s;

    obj_p keys, vals;
    i64_t n, idx;
    i64_t pos_n = ctx_pos_count(ctx);

    switch (code) {
        case EC_TYPE: {
            n = 3 + pos_n;  // code, expected, got, [positions...]
            keys = SYMBOL(n);
            vals = LIST(n);
            ins_sym(&keys, 0, "code");
            ins_sym(&keys, 1, "expected");
            ins_sym(&keys, 2, "got");
            s = err_name(code);
            AS_LIST(vals)[0] = symbol(s, strlen(s));
            // Handle type classes
            switch ((u8_t)ctx->v1) {
                case TCLASS_NUMERIC:    s = "numeric"; break;
                case TCLASS_INTEGER:    s = "integer"; break;
                case TCLASS_FLOAT:      s = "float"; break;
                case TCLASS_TEMPORAL:   s = "temporal"; break;
                case TCLASS_COLLECTION: s = "collection"; break;
                case TCLASS_CALLABLE:   s = "callable"; break;
                case TCLASS_ANY:        s = "any"; break;
                default:                s = type_name(ctx->v1); break;
            }
            AS_LIST(vals)[1] = symbol(s, strlen(s));
            s = type_name(ctx->v2);
            AS_LIST(vals)[2] = symbol(s, strlen(s));
            idx = 3;
            ctx_add_pos(ctx, &keys, &vals, &idx);
            break;
        }
        case EC_ARITY: {
            n = 3 + pos_n;
            keys = SYMBOL(n);
            vals = LIST(n);
            ins_sym(&keys, 0, "code");
            ins_sym(&keys, 1, "expected");
            ins_sym(&keys, 2, "got");
            s = err_name(code);
            AS_LIST(vals)[0] = symbol(s, strlen(s));
            AS_LIST(vals)[1] = i32(ctx->v1);
            AS_LIST(vals)[2] = i32(ctx->v2);
            idx = 3;
            ctx_add_pos(ctx, &keys, &vals, &idx);
            break;
        }
        case EC_LENGTH: {
            n = 3 + pos_n;
            keys = SYMBOL(n);
            vals = LIST(n);
            ins_sym(&keys, 0, "code");
            ins_sym(&keys, 1, "need");
            ins_sym(&keys, 2, "have");
            s = err_name(code);
            AS_LIST(vals)[0] = symbol(s, strlen(s));
            AS_LIST(vals)[1] = i32(ctx->v1);
            AS_LIST(vals)[2] = i32(ctx->v2);
            idx = 3;
            ctx_add_pos(ctx, &keys, &vals, &idx);
            break;
        }
        case EC_INDEX: {
            n = 3 + pos_n;
            keys = SYMBOL(n);
            vals = LIST(n);
            ins_sym(&keys, 0, "code");
            ins_sym(&keys, 1, "index");
            ins_sym(&keys, 2, "bound");
            s = err_name(code);
            AS_LIST(vals)[0] = symbol(s, strlen(s));
            AS_LIST(vals)[1] = i32(ctx->v1);
            AS_LIST(vals)[2] = i32(ctx->v2);
            idx = 3;
            ctx_add_pos(ctx, &keys, &vals, &idx);
            break;
        }
        case EC_DOMAIN: {
            n = 1 + pos_n;
            keys = SYMBOL(n);
            vals = LIST(n);
            ins_sym(&keys, 0, "code");
            s = err_name(code);
            AS_LIST(vals)[0] = symbol(s, strlen(s));
            idx = 1;
            ctx_add_pos(ctx, &keys, &vals, &idx);
            break;
        }
        case EC_VALUE: {
            i64_t sym_id = err_get_symbol(err);
            n = sym_id ? 2 : 1;
            keys = SYMBOL(n);
            vals = LIST(n);
            ins_sym(&keys, 0, "code");
            s = err_name(code);
            AS_LIST(vals)[0] = symbol(s, strlen(s));
            if (n == 2) {
                ins_sym(&keys, 1, "name");
                AS_LIST(vals)[1] = symboli64(sym_id);
            }
            break;
        }
        case EC_OS: {
            i32_t e = err_get_errno(err);
            n = e ? 2 : 1;
            keys = SYMBOL(n);
            vals = LIST(n);
            ins_sym(&keys, 0, "code");
            s = err_name(code);
            AS_LIST(vals)[0] = symbol(s, strlen(s));
            if (n == 2) {
                ins_sym(&keys, 1, "message");
                AS_LIST(vals)[1] = vn_c8("%s", strerror(e));
            }
            break;
        }
        case EC_USER: {
            lit_p msg = err_get_message(err);
            n = (msg && msg[0]) ? 2 : 1;
            keys = SYMBOL(n);
            vals = LIST(n);
            ins_sym(&keys, 0, "code");
            s = err_name(code);
            AS_LIST(vals)[0] = symbol(s, strlen(s));
            if (n == 2) {
                ins_sym(&keys, 1, "message");
                AS_LIST(vals)[1] = vn_c8("%s", msg);
            }
            break;
        }
        case EC_LIMIT: {
            i16_t limit = (i16_t)(((u8_t)ctx->v1) | ((u8_t)ctx->v2 << 8));
            n = limit ? 2 : 1;
            keys = SYMBOL(n);
            vals = LIST(n);
            ins_sym(&keys, 0, "code");
            s = err_name(code);
            AS_LIST(vals)[0] = symbol(s, strlen(s));
            if (n == 2) {
                ins_sym(&keys, 1, "limit");
                AS_LIST(vals)[1] = i32(limit);
            }
            break;
        }
        case EC_NYI: {
            n = 2;
            keys = SYMBOL(n);
            vals = LIST(n);
            ins_sym(&keys, 0, "code");
            ins_sym(&keys, 1, "type");
            s = err_name(code);
            AS_LIST(vals)[0] = symbol(s, strlen(s));
            s = type_name(ctx->v1);
            AS_LIST(vals)[1] = symbol(s, strlen(s));
            break;
        }
        default: {
            keys = SYMBOL(1);
            vals = LIST(1);
            ins_sym(&keys, 0, "code");
            s = err_name(code);
            AS_LIST(vals)[0] = symbol(s, strlen(s));
            break;
        }
    }

    // TODO: Add trace handling later
    UNUSED(trace);

    return dict(keys, vals);
}
