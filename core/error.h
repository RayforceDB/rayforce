/*
 *   Copyright (c) 2023 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 */

#ifndef ERROR_H
#define ERROR_H

#include "rayforce.h"
#include "ops.h"

// ============================================================================
// Error Codes
// ============================================================================
typedef enum {
    EC_OK = 0,  // No error
    EC_TYPE,    // Type mismatch
    EC_ARITY,   // Wrong number of arguments
    EC_LENGTH,  // List length mismatch
    EC_DOMAIN,  // Value out of range
    EC_INDEX,   // Index out of bounds
    EC_VALUE,   // Undefined symbol
    EC_LIMIT,   // Resource limit
    EC_OS,      // System error (wraps errno)
    EC_PARSE,   // Parse error
    EC_NYI,     // Not yet implemented
    EC_USER,    // User raised
    EC_MAX
} err_code_t;

#define ERR_MSG_SIZE 24  // sizeof(err_t) == 32

typedef struct err_t {
    u8_t code;  // err_code_t
    union {
        struct {
            i8_t expected;
            i8_t actual;
            u8_t arg;
            u8_t field;
        } type;
        struct {
            i8_t need;
            i8_t have;
            u8_t arg;
        } arity;
        struct {
            i8_t need;
            i8_t have;
            u8_t arg;
            u8_t arg2;
            u8_t field;
            u8_t field2;
        } length;
        struct {
            i8_t idx;
            i8_t len;
            u8_t arg;
            u8_t field;
        } index;
        struct {
            u8_t arg;
            u8_t field;
        } domain;
        struct {
            i64_t sym;
        } value;
        struct {
            i32_t val;
        } limit;
        struct {
            i32_t no;
        } os;
        struct {
            i8_t type;
        } nyi;
        struct {
            char msg[ERR_MSG_SIZE];
        } user;
    };
} err_t;

// ============================================================================
// Error Creation - Set VM error context, return ERR_OBJ
// ============================================================================
obj_p err_type(i8_t expected, i8_t actual, u8_t arg, u8_t field);
obj_p err_arity(i8_t need, i8_t have, u8_t arg);
obj_p err_length(i8_t need, i8_t have, u8_t arg, u8_t arg2, u8_t field, u8_t field2);
obj_p err_index(i8_t idx, i8_t len, u8_t arg, u8_t field);
obj_p err_domain(u8_t arg, u8_t field);
obj_p err_value(i64_t sym);
obj_p err_limit(i32_t limit);
obj_p err_os(nil_t);
obj_p err_user(lit_p msg);
obj_p err_nyi(i8_t type);
obj_p err_parse(nil_t);
obj_p err_raw(err_code_t code);

// ============================================================================
// Error Query
// ============================================================================
err_code_t err_code(obj_p err);
lit_p err_name(err_code_t code);
lit_p err_msg(obj_p err);
obj_p err_info(obj_p err);

obj_p ray_err(lit_p msg);  // Create user error

// ============================================================================
// Helpers
// ============================================================================
#define PANIC(...)                                            \
    do {                                                      \
        fprintf(stderr, "panic %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__);                         \
        exit(1);                                              \
    } while (0)

#endif  // ERROR_H
