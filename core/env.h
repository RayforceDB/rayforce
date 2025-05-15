/*
 *   Copyright (c) 2023 Anton Kundenko <singaraiona@gmail.com>
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

#ifndef ENV_H
#define ENV_H

#include "rayforce.h"
#include "heap.h"

// offset in array of typenames for each type
#define TYPE_OFFSET TYPE_C8
#define MAX_TYPE (TYPE_ERR + TYPE_OFFSET + 2)

// hot symbols
extern i64_t SYMBOL_FN;
extern i64_t SYMBOL_SELF;
extern i64_t SYMBOL_DO;
extern i64_t SYMBOL_SET;
extern i64_t SYMBOL_LET;
extern i64_t SYMBOL_TAKE;
extern i64_t SYMBOL_BY;
extern i64_t SYMBOL_FROM;
extern i64_t SYMBOL_WHERE;
extern i64_t SYMBOL_SYM;

/*
 *  Environment
 */
typedef struct env_t {
    obj_p keywords;   // list of reserved keywords
    obj_p functions;  // dict, containing function primitives
    obj_p variables;  // dict, containing mappings variables names to their values
    obj_p typenames;  // dict, containing mappings type ids to their names
    obj_p internals;  // dict, containing internal functions, variables, descriptors etc.
} env_t;

env_t env_create(nil_t);
nil_t env_destroy(env_t *env);

i64_t env_get_typename_by_type(env_t *env, i8_t type);
i8_t env_get_type_by_type_name(env_t *env, i64_t name);
str_p env_get_type_name(i8_t type);
str_p env_get_internal_name(obj_p obj);
i64_t env_get_internal_id(obj_p obj);
obj_p env_get_internal_function(lit_p name);
obj_p env_get_internal_function_by_id(i64_t id);
str_p env_get_internal_function_name(lit_p name, i64_t len, i64_t *index, b8_t exact);
str_p env_get_internal_keyword_name(lit_p name, i64_t len, i64_t *index, b8_t exact);
str_p env_get_global_name(lit_p name, i64_t len, i64_t *index, i64_t *sbidx);
obj_p env_set(env_t *env, obj_p key, obj_p val);
obj_p ray_env(obj_p *x, i64_t n);
obj_p ray_memstat(obj_p *x, i64_t n);
obj_p ray_internals(obj_p *x, i64_t n);

#endif  // ENV_H
