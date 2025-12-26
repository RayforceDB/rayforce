/*
 *   Copyright (c) 2023 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 */

#include "error.h"
#include "heap.h"
#include "string.h"
#include "def.h"
#include "util.h"

// ============================================================================
// Error Code Names
// ============================================================================
static lit_p err_names[] = {
    "ok",      // EC_OK
    "type",    // EC_TYPE
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

_Static_assert(sizeof(err_names) / sizeof(err_names[0]) == EC_MAX, "err_names must match err_code_t");

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

obj_p err_new(err_code_t code) { return err_alloc(code); }

obj_p err_type(i8_t expected, i8_t actual, i64_t field) {
    obj_p err = err_alloc(EC_TYPE);
    err_ctx_t* ctx = (err_ctx_t*)&err->i64;
    ctx->types.expected = expected;
    ctx->types.actual = actual;
    ctx->types.field = (i32_t)field;
    return err;
}

obj_p err_length(i32_t need, i32_t have) {
    obj_p err = err_alloc(EC_LENGTH);
    err_ctx_t* ctx = (err_ctx_t*)&err->i64;
    ctx->counts.need = need;
    ctx->counts.have = have;
    return err;
}

obj_p err_index(i32_t idx, i32_t len) {
    obj_p err = err_alloc(EC_INDEX);
    err_ctx_t* ctx = (err_ctx_t*)&err->i64;
    ctx->bounds.idx = idx;
    ctx->bounds.len = len;
    return err;
}

obj_p err_value(i64_t sym) {
    obj_p err = err_alloc(EC_VALUE);
    err_ctx_t* ctx = (err_ctx_t*)&err->i64;
    ctx->symbol = sym;
    return err;
}

obj_p err_limit(i32_t limit) {
    obj_p err = err_alloc(EC_LIMIT);
    err_ctx_t* ctx = (err_ctx_t*)&err->i64;
    ctx->counts.have = limit;
    return err;
}

obj_p err_os(nil_t) {
    obj_p err = err_alloc(EC_OS);
    err_ctx_t* ctx = (err_ctx_t*)&err->i64;
    ctx->errnum = get_errno();
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

obj_p ray_err(lit_p msg) {
    if (strcmp(msg, "type") == 0)
        return err_new(EC_TYPE);
    if (strcmp(msg, "length") == 0)
        return err_new(EC_LENGTH);
    if (strcmp(msg, "arity") == 0)
        return err_new(EC_LENGTH);
    if (strcmp(msg, "domain") == 0)
        return err_new(EC_DOMAIN);
    if (strcmp(msg, "range") == 0)
        return err_new(EC_DOMAIN);
    if (strcmp(msg, "index") == 0)
        return err_new(EC_INDEX);
    if (strcmp(msg, "value") == 0)
        return err_new(EC_VALUE);
    if (strcmp(msg, "nfound") == 0)
        return err_new(EC_VALUE);
    if (strcmp(msg, "eval") == 0)
        return err_new(EC_VALUE);
    if (strcmp(msg, "key") == 0)
        return err_new(EC_VALUE);
    if (strcmp(msg, "limit") == 0)
        return err_new(EC_LIMIT);
    if (strcmp(msg, "stack") == 0)
        return err_new(EC_LIMIT);
    if (strcmp(msg, "oom") == 0)
        return err_new(EC_LIMIT);
    if (strcmp(msg, "heap") == 0)
        return err_new(EC_LIMIT);
    if (strcmp(msg, "os") == 0)
        return err_os();
    if (strcmp(msg, "sys") == 0)
        return err_os();
    if (strcmp(msg, "io") == 0)
        return err_os();
    if (strcmp(msg, "parse") == 0)
        return err_new(EC_PARSE);
    if (strcmp(msg, "nyi") == 0)
        return err_new(EC_NYI);
    if (strcmp(msg, "raise") == 0)
        return err_user(NULL);
    if (strcmp(msg, "bad") == 0)
        return err_new(EC_DOMAIN);
    if (strcmp(msg, "join") == 0)
        return err_new(EC_TYPE);
    if (strcmp(msg, "init") == 0)
        return err_os();
    if (strcmp(msg, "empty") == 0)
        return err_new(EC_DOMAIN);

    return err_user(msg);
}

lit_p ray_err_msg(obj_p err) {
    if (err == NULL_OBJ || err->type != TYPE_ERR)
        return "";

    err_code_t code = err_code(err);
    if (code == EC_USER && err->len > 0)
        return (lit_p)(err + 1);

    return err_name(code);
}
