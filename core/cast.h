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

#ifndef CAST_H
#define CAST_H

#include "symbols.h"
#include "format.h"
#include "runtime.h"
#include "env.h"
#include "util.h"

/*
 * Casts the object to the specified type.
 * If the cast is not possible, returns an error object.
 * If object is already of the specified type, returns the object itself (cloned).
 */
static inline __attribute__((always_inline)) rf_object_t rf_cast(i8_t type, rf_object_t *y)
{

#define m(a, b) ((i16_t)((u8_t)a << 8 | (u8_t)b))

    // Do nothing if the type is the same
    if (type == y->type)
        return rf_object_clone(y);

    if (type == TYPE_STRING)
    {
        str_t s = rf_object_fmt(y);
        if (s == NULL)
            panic("rf_object_fmt() returned NULL");
        return string_from_str(s, strlen(s));
    }

    rf_object_t x, err;
    i16_t mask = m(type, y->type);

    switch (mask)
    {
    case m(-TYPE_I64, -TYPE_I64):
        x = *y;
        x.type = type;
        break;
    case m(-TYPE_I64, -TYPE_F64):
        x = i64((i64_t)y->f64);
        x.type = type;
        break;
    case m(-TYPE_F64, -TYPE_I64):
        x = f64((f64_t)y->i64);
        x.type = type;
        break;
    case m(-TYPE_SYMBOL, TYPE_STRING):
        x = symbol(as_string(y));
        break;
    case m(-TYPE_I64, TYPE_STRING):
        x = i64(strtol(as_string(y), NULL, 10));
        break;
    case m(-TYPE_F64, TYPE_STRING):
        x = f64(strtod(as_string(y), NULL));
        break;
    case m(TYPE_TABLE, TYPE_DICT):
        x = rf_object_clone(y);
        x.type = type;
        break;
    default:
        err = error(ERR_TYPE, str_fmt(0, "invalid conversion from '%s' to '%s'",
                                      symbols_get(env_get_typename_by_type(&runtime_get()->env, y->type)),
                                      symbols_get(env_get_typename_by_type(&runtime_get()->env, type))));
        err.id = y->id;
        return err;
    }

    return x;
}

#endif
