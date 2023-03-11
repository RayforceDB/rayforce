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

#include <stdio.h>
#include "storm.h"
#include "format.h"
#include "alloc.h"
#include "string.h"

extern i8_t is_null(value_t *value)
{
    return value->type == TYPE_LIST && value->list.ptr == NULL;
}

extern i8_t is_error(value_t *value)
{
    return value->type == TYPE_ERROR;
}

extern value_t error(i8_t code, str_t message)
{
    value_t error = {
        .type = TYPE_ERROR,
        .error = {
            .code = code,
            .message = message,
        },
    };

    return error;
}

extern value_t i64(i64_t value)
{
    value_t scalar = {
        .type = -TYPE_I64,
        .i64 = value,
    };

    return scalar;
}

extern value_t f64(f64_t value)
{
    value_t scalar = {
        .type = -TYPE_F64,
        .f64 = value,
    };

    return scalar;
}

extern value_t xi64(i64_t *ptr, i64_t len)
{
    value_t vector = {
        .type = TYPE_I64,
        .list = {
            .ptr = ptr,
            .len = len,
        },
    };

    return vector;
}

extern value_t xf64(f64_t *ptr, i64_t len)
{
    value_t vector = {
        .type = TYPE_F64,
        .list = {
            .ptr = ptr,
            .len = len,
        },
    };

    return vector;
}

extern value_t symbol(str_t ptr, i64_t len)
{
    string_t string = string_create(ptr, len);
    i64_t id = symbols_intern(string);
    value_t list = {
        .type = -TYPE_SYMBOL,
        .i64 = id,
    };

    return list;
}

extern value_t string(str_t ptr, i64_t len)
{
    value_t list = {
        .type = TYPE_STRING,
        .list = {
            .ptr = ptr,
            .len = len,
        },
    };

    return list;
}

extern value_t xsymbol(i64_t *ptr, i64_t len)
{
    value_t vector = {
        .type = TYPE_SYMBOL,
        .list = {
            .ptr = ptr,
            .len = len,
        },
    };

    return vector;
}

extern value_t list(value_t *ptr, i64_t len)
{
    value_t list = {
        .type = TYPE_LIST,
        .list = {
            .ptr = ptr,
            .len = len,
        },
    };

    return list;
}

extern value_t null()
{
    value_t list = {
        .type = TYPE_LIST,
        .list = {
            .ptr = NULL,
            .len = 0,
        },
    };

    return list;
}

extern null_t value_free(value_t *value)
{
    switch (value->type)
    {
    case TYPE_I64:
    {
        storm_free(value->list.ptr);
        break;
    }
    case TYPE_F64:
    {
        storm_free(value->list.ptr);
        break;
    }
    default:
    {
        // printf("** Free: Invalid type\n");
        break;
    }
    }
}
