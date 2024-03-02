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
#define MAX_TYPE (TYPE_ERROR + TYPE_OFFSET + 2)

// reserved symbols (keywords)
#define KW_EMPTY_SYMBOL 0
#define KW_FN 1
#define KW_SELF 2
#define KW_DO 3
#define KW_SET 4
#define KW_LET 5

/*
 *  Environment
 */
typedef struct env_t
{
    obj_p functions; // dict, containing function primitives
    obj_p variables; // dict, containing mappings variables names to their values
    obj_p typenames; // dict, containing mappings type ids to their names
} env_t;

env_t create_env(nil_t);
nil_t free_env(env_t *env);

i64_t env_get_typename_by_type(env_t *env, i8_t type);
i8_t env_get_type_by_type_name(env_t *env, i64_t name);
str_p env_get_type_name(i8_t type);
str_p env_get_internal_name(obj_p obj);
obj_p env_get_internal_function(str_p name);
obj_p env_get_internal_function_by_id(i64_t id);
obj_p env_set(env_t *env, obj_p key, obj_p val);
obj_p env_get(env_t *env, obj_p key);
obj_p ray_env(obj_p *x, u64_t n);
obj_p ray_memstat(obj_p *x, u64_t n);

#endif // ENV_H
