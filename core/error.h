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
// Error Context - Typed union for additional error info
// ============================================================================
typedef struct {
    i8_t expected;
    i8_t actual;
    i16_t _pad;
    i32_t field;  // optional: symbol id of field name (0 = none)
} err_types_t;

typedef struct {
    i32_t need;
    i32_t have;
} err_counts_t;

typedef struct {
    i32_t idx;
    i32_t len;
} err_bounds_t;

typedef union {
    err_types_t types;    // EC_TYPE: expected/actual types + optional field
    err_counts_t counts;  // EC_LENGTH: need/have counts
    err_bounds_t bounds;  // EC_INDEX: idx/len bounds
    i64_t symbol;         // EC_VALUE: symbol id
    i32_t errnum;         // EC_OS: errno
} err_ctx_t;

RAY_ASSERT(sizeof(err_ctx_t) == sizeof(i64_t), "err_ctx_t must fit in obj->i64");

// ============================================================================
// Error Creation API
// ============================================================================

// With context
obj_p err_type(i8_t expected, i8_t actual, i64_t field);  // field=0 for none
obj_p err_arity(i32_t need, i32_t have);                  // function arguments
obj_p err_length(i32_t need, i32_t have);                 // list lengths
obj_p err_index(i32_t idx, i32_t len);
obj_p err_value(i64_t sym);  // sym=0 for no context
obj_p err_limit(i32_t limit);
obj_p err_os(nil_t);
obj_p err_user(lit_p msg);

// Without context (simple errors)
obj_p err_domain(nil_t);
obj_p err_nyi(i8_t type);  // Not yet implemented for given type
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

// Convenience accessors
static inline err_types_t err_get_types(obj_p err) { return err_ctx(err)->types; }

static inline err_counts_t err_get_counts(obj_p err) { return err_ctx(err)->counts; }

static inline err_bounds_t err_get_bounds(obj_p err) { return err_ctx(err)->bounds; }

static inline i64_t err_get_symbol(obj_p err) { return err_ctx(err)->symbol; }

static inline i32_t err_get_errno(obj_p err) { return err_ctx(err)->errnum; }

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
