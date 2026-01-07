/*
 *   Copyright (c) 2023 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 */

#include "error.h"
#include "eval.h"
#include "string.h"
#include "util.h"

// ============================================================================
// Error Names (like sys_errlist)
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

RAY_ASSERT(sizeof(err_names) / sizeof(err_names[0]) == EC_MAX, "err_names size mismatch");

lit_p err_name(err_code_t code) { return (code < EC_MAX) ? err_names[code] : "error"; }

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
// Error Creation - Set VM->err, return ERR_OBJ
// ============================================================================

obj_p err_raw(err_code_t code) {
    VM->err.code = (u8_t)code;
    return ERR_OBJ;
}

obj_p err_type(i8_t expected, i8_t actual, u8_t arg, u8_t field) {
    err_t *e = &VM->err;
    e->code = EC_TYPE;
    e->type.expected = expected;
    e->type.actual = actual;
    e->type.arg = arg;
    e->type.field = field;
    return ERR_OBJ;
}

obj_p err_arity(i8_t need, i8_t have, u8_t arg) {
    err_t *e = &VM->err;
    e->code = EC_ARITY;
    e->arity.need = need;
    e->arity.have = have;
    e->arity.arg = arg;
    return ERR_OBJ;
}

obj_p err_length(i8_t need, i8_t have, u8_t arg, u8_t arg2, u8_t field, u8_t field2) {
    err_t *e = &VM->err;
    e->code = EC_LENGTH;
    e->length.need = need;
    e->length.have = have;
    e->length.arg = arg;
    e->length.arg2 = arg2;
    e->length.field = field;
    e->length.field2 = field2;
    return ERR_OBJ;
}

obj_p err_index(i8_t idx, i8_t len, u8_t arg, u8_t field) {
    err_t *e = &VM->err;
    e->code = EC_INDEX;
    e->index.idx = idx;
    e->index.len = len;
    e->index.arg = arg;
    e->index.field = field;
    return ERR_OBJ;
}

obj_p err_domain(u8_t arg, u8_t field) {
    err_t *e = &VM->err;
    e->code = EC_DOMAIN;
    e->domain.arg = arg;
    e->domain.field = field;
    return ERR_OBJ;
}

obj_p err_value(i64_t sym) {
    err_t *e = &VM->err;
    e->code = EC_VALUE;
    e->value.sym = sym;
    return ERR_OBJ;
}

obj_p err_limit(i32_t limit) {
    err_t *e = &VM->err;
    e->code = EC_LIMIT;
    e->limit.val = limit;
    return ERR_OBJ;
}

obj_p err_os(nil_t) {
    err_t *e = &VM->err;
    e->code = EC_OS;
    e->os.no = get_errno();
    return ERR_OBJ;
}

obj_p err_user(lit_p msg) {
    err_t *e = &VM->err;
    e->code = EC_USER;
    if (msg) {
        strncpy(e->user.msg, msg, ERR_MSG_SIZE - 1);
        e->user.msg[ERR_MSG_SIZE - 1] = '\0';
    } else {
        e->user.msg[0] = '\0';
    }
    return ERR_OBJ;
}

obj_p err_nyi(i8_t type) {
    err_t *e = &VM->err;
    e->code = EC_NYI;
    e->nyi.type = type;
    return ERR_OBJ;
}

obj_p err_parse(nil_t) {
    VM->err.code = EC_PARSE;
    return ERR_OBJ;
}

err_code_t err_code(obj_p err) {
    if (err == NULL_OBJ || err->type != TYPE_ERR)
        return EC_OK;
    return (err_code_t)VM->err.code;
}

lit_p err_msg(obj_p err) {
    if (err == NULL_OBJ || err->type != TYPE_ERR)
        return "";

    err_t *e = &VM->err;
    switch (e->code) {
        case EC_USER:
            return e->user.msg[0] ? e->user.msg : "";
        case EC_OS:
            return strerror(e->os.no);
        default:
            return err_name((err_code_t)e->code);
    }
}

obj_p err_info(obj_p err) {
    if (err == NULL_OBJ || err->type != TYPE_ERR)
        return NULL_OBJ;

    err_t *e = &VM->err;
    obj_p keys, vals;
    lit_p s;

    switch (e->code) {
        case EC_TYPE:
            keys = SYMBOL(3);
            vals = LIST(3);
            ins_sym(&keys, 0, "code");
            ins_sym(&keys, 1, "expected");
            ins_sym(&keys, 2, "got");
            s = err_name(EC_TYPE);
            AS_LIST(vals)[0] = symbol(s, strlen(s));
            s = type_name(e->type.expected);
            AS_LIST(vals)[1] = symbol(s, strlen(s));
            s = type_name(e->type.actual);
            AS_LIST(vals)[2] = symbol(s, strlen(s));
            break;

        case EC_ARITY:
            keys = SYMBOL(3);
            vals = LIST(3);
            ins_sym(&keys, 0, "code");
            ins_sym(&keys, 1, "expected");
            ins_sym(&keys, 2, "got");
            s = err_name(EC_ARITY);
            AS_LIST(vals)[0] = symbol(s, strlen(s));
            AS_LIST(vals)[1] = i32(e->arity.need);
            AS_LIST(vals)[2] = i32(e->arity.have);
            break;

        case EC_LENGTH:
            keys = SYMBOL(3);
            vals = LIST(3);
            ins_sym(&keys, 0, "code");
            ins_sym(&keys, 1, "need");
            ins_sym(&keys, 2, "have");
            s = err_name(EC_LENGTH);
            AS_LIST(vals)[0] = symbol(s, strlen(s));
            AS_LIST(vals)[1] = i32(e->length.need);
            AS_LIST(vals)[2] = i32(e->length.have);
            break;

        case EC_INDEX:
            keys = SYMBOL(3);
            vals = LIST(3);
            ins_sym(&keys, 0, "code");
            ins_sym(&keys, 1, "index");
            ins_sym(&keys, 2, "bound");
            s = err_name(EC_INDEX);
            AS_LIST(vals)[0] = symbol(s, strlen(s));
            AS_LIST(vals)[1] = i32(e->index.idx);
            AS_LIST(vals)[2] = i32(e->index.len);
            break;

        case EC_VALUE:
            keys = SYMBOL(e->value.sym ? 2 : 1);
            vals = LIST(e->value.sym ? 2 : 1);
            ins_sym(&keys, 0, "code");
            s = err_name(EC_VALUE);
            AS_LIST(vals)[0] = symbol(s, strlen(s));
            if (e->value.sym) {
                ins_sym(&keys, 1, "name");
                AS_LIST(vals)[1] = symboli64(e->value.sym);
            }
            break;

        case EC_OS:
            keys = SYMBOL(2);
            vals = LIST(2);
            ins_sym(&keys, 0, "code");
            ins_sym(&keys, 1, "message");
            s = err_name(EC_OS);
            AS_LIST(vals)[0] = symbol(s, strlen(s));
            AS_LIST(vals)[1] = vn_c8("%s", strerror(e->os.no));
            break;

        case EC_USER:
            keys = SYMBOL(e->user.msg[0] ? 2 : 1);
            vals = LIST(e->user.msg[0] ? 2 : 1);
            ins_sym(&keys, 0, "code");
            s = err_name(EC_USER);
            AS_LIST(vals)[0] = symbol(s, strlen(s));
            if (e->user.msg[0]) {
                ins_sym(&keys, 1, "message");
                AS_LIST(vals)[1] = vn_c8("%s", e->user.msg);
            }
            break;

        case EC_LIMIT:
            keys = SYMBOL(2);
            vals = LIST(2);
            ins_sym(&keys, 0, "code");
            ins_sym(&keys, 1, "limit");
            s = err_name(EC_LIMIT);
            AS_LIST(vals)[0] = symbol(s, strlen(s));
            AS_LIST(vals)[1] = i32(e->limit.val);
            break;

        case EC_NYI:
            keys = SYMBOL(2);
            vals = LIST(2);
            ins_sym(&keys, 0, "code");
            ins_sym(&keys, 1, "type");
            s = err_name(EC_NYI);
            AS_LIST(vals)[0] = symbol(s, strlen(s));
            s = type_name(e->nyi.type);
            AS_LIST(vals)[1] = symbol(s, strlen(s));
            break;

        default:
            keys = SYMBOL(1);
            vals = LIST(1);
            ins_sym(&keys, 0, "code");
            s = err_name((err_code_t)e->code);
            AS_LIST(vals)[0] = symbol(s, strlen(s));
            break;
    }

    return dict(keys, vals);
}

obj_p ray_err(lit_p msg) { return err_user(msg); }
