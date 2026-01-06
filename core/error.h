/*
 *   Copyright (c) 2023 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 */

#ifndef ERROR_H
#define ERROR_H

#include "rayforce.h"
#include "nfo.h"
#include "util.h"

// ============================================================================
// Error Codes - Minimal set inspired by kdb+
// ============================================================================
typedef enum {
    EC_OK = 0,  // No error
    EC_TYPE,    // 'type   - type mismatch
    EC_ARITY,   // 'arity  - wrong number of arguments
    EC_LENGTH,  // 'length - list length mismatch
    EC_DOMAIN,  // 'domain - value out of range
    EC_INDEX,   // 'index  - index out of bounds
    EC_VALUE,   // 'value  - undefined symbol
    EC_LIMIT,   // 'limit  - resource limit
    EC_OS,      // 'os     - system error
    EC_PARSE,   // 'parse  - parse error
    EC_NYI,     // 'nyi    - not yet implemented
    EC_USER,    // ''      - user raised
    EC_MAX
} err_code_t;

// ============================================================================
// Type Classes for err_type() - use in 'expected' field (104-124 range unused)
// ============================================================================
#define TCLASS_NUMERIC 104     // I8,I16,I32,I64,F32,F64
#define TCLASS_INTEGER 105     // I8,I16,I32,I64
#define TCLASS_FLOAT 106       // F32,F64
#define TCLASS_TEMPORAL 107    // DATE,TIME,TIMESTAMP,TIMESPAN
#define TCLASS_COLLECTION 108  // LIST,DICT,TABLE
#define TCLASS_CALLABLE 109    // LAMBDA,UNARY,BINARY,VARY
#define TCLASS_ANY 110         // any type

#define IS_TCLASS(t) ((t) >= 104 && (t) <= 110)

// ============================================================================
// Error Context - Unified 8-byte structure for all error types
// ============================================================================
// Encoding by error type:
//   EC_TYPE:   arg, field, v1=expected, v2=actual
//   EC_ARITY:  arg, v1=need, v2=have
//   EC_LENGTH: arg, arg2, field, field2, v1=need, v2=have
//   EC_INDEX:  arg, field, v1=idx, v2=len
//   EC_DOMAIN: arg, field
//   EC_VALUE:  i64 = symbol id (full 64-bit pointer)
//   EC_LIMIT:  v1-v2 = limit (as i16)
//   EC_OS:     v1-v4 = errno (as i32)
//   EC_NYI:    v1 = type
// ============================================================================
typedef struct {
    u8_t arg;     // argument index (1-based, 0 = none)
    u8_t arg2;    // second argument (for mismatches between args, 1-based)
    u8_t field;   // field index in arg (1-based, 0 = none)
    u8_t field2;  // field in arg2 / subfield (1-based, 0 = none)
    i8_t v1;      // expected type / need / idx / type
    i8_t v2;      // actual type / have / len
    i8_t v3;      // extra value (high byte for larger values)
    i8_t v4;      // extra value / flags
} err_ctx_t;

RAY_ASSERT(sizeof(err_ctx_t) == sizeof(i64_t), "err_ctx_t must fit in obj->i64");

// ============================================================================
// Error Creation API
// ============================================================================
// All functions use positional context: arg/field indices (1-based, 0 = none)
// The error formatter extracts sub-expressions from trace using these indices.

// Type mismatch: expected vs actual type at arg.field
obj_p err_type(i8_t expected, i8_t actual, u8_t arg, u8_t field);

// Arity: wrong number of arguments
obj_p err_arity(i8_t need, i8_t have, u8_t arg);

// Length mismatch: between arg.field and arg2.field2, or within arg.field
obj_p err_length(i8_t need, i8_t have, u8_t arg, u8_t arg2, u8_t field, u8_t field2);

// Index out of bounds at arg.field
obj_p err_index(i8_t idx, i8_t len, u8_t arg, u8_t field);

// Domain error at arg.field
obj_p err_domain(u8_t arg, u8_t field);

// Undefined symbol
obj_p err_value(i64_t sym);

// Resource limit exceeded
obj_p err_limit(i32_t limit);

// OS/system error (captures errno)
obj_p err_os(nil_t);

// User-raised error with message
obj_p err_user(lit_p msg);

// Not yet implemented for type
obj_p err_nyi(i8_t type);

// Parse error
obj_p err_parse(nil_t);

// Internal: create error with code only (for deserialization/parsing)
obj_p err_raw(err_code_t code);

// ============================================================================
// Error Decoding API
// ============================================================================

err_code_t err_code(obj_p err);
lit_p err_name(err_code_t code);

// Get context (returns pointer to ctx stored in obj->i64)
err_ctx_t* err_ctx(obj_p err);

// Convenience accessors for unified context
static inline u8_t err_get_arg(obj_p err) { return err_ctx(err)->arg; }
static inline u8_t err_get_arg2(obj_p err) { return err_ctx(err)->arg2; }
static inline u8_t err_get_field(obj_p err) { return err_ctx(err)->field; }
static inline u8_t err_get_field2(obj_p err) { return err_ctx(err)->field2; }
static inline i8_t err_get_v1(obj_p err) { return err_ctx(err)->v1; }
static inline i8_t err_get_v2(obj_p err) { return err_ctx(err)->v2; }
static inline i8_t err_get_v3(obj_p err) { return err_ctx(err)->v3; }
static inline i8_t err_get_v4(obj_p err) { return err_ctx(err)->v4; }

// For EC_VALUE: symbol stored directly in i64
static inline i64_t err_get_symbol(obj_p err) { return err->i64; }

// For EC_OS: errno stored across v1-v4 as i32
static inline i32_t err_get_errno(obj_p err) {
    err_ctx_t* ctx = err_ctx(err);
    return (i32_t)(((u8_t)ctx->v1) | ((u8_t)ctx->v2 << 8) | ((u8_t)ctx->v3 << 16) | ((u8_t)ctx->v4 << 24));
}

// User error message (stored after obj header)
lit_p err_get_message(obj_p err);

// ============================================================================
// String-based API (for deserialization)
// ============================================================================

obj_p ray_err(lit_p msg);      // parse string → error
lit_p ray_err_msg(obj_p err);  // error → string name

// ============================================================================
// Error Info (for IPC serialization)
// ============================================================================

// Returns a dict with all error information:
// {code: `type; expected: `i64; actual: `c8; field: `from; trace: [...]}
obj_p err_info(obj_p err);

// ============================================================================
// Helpers
// ============================================================================

#define PANIC(...)                                            \
    do {                                                      \
        fprintf(stderr, "panic %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__);                         \
        exit(1);                                              \
    } while (0)

typedef struct loc_t {
    span_t span;
    obj_p file;
    obj_p source;
} loc_t;

#endif  // ERROR_H
