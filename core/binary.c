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

#include <time.h>
#include "runtime.h"
#include "binary.h"
#include "dict.h"
#include "util.h"
#include "ops.h"
#include "util.h"
#include "format.h"
#include "vector.h"
#include "hash.h"

rf_object_t error_type2(type_t type1, type_t type2, str_t msg)
{
    str_t fmsg = str_fmt(0, "%s: '%s', '%s'", msg, env_get_typename(type1), env_get_typename(type2));
    rf_object_t err = error(ERR_TYPE, fmsg);
    rf_free(fmsg);
    return err;
}

rf_object_t rf_set_variable(rf_object_t *key, rf_object_t *val)
{
    return dict_set(&runtime_get()->env.variables, key, rf_object_clone(val));
}

rf_object_t rf_dict(rf_object_t *x, rf_object_t *y)
{
    return dict(rf_object_clone(x), rf_object_clone(y));
}

rf_object_t rf_table(rf_object_t *x, rf_object_t *y)
{
    return table(rf_object_clone(x), rf_object_clone(y));
}

rf_object_t rf_add(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l;
    rf_object_t vec;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        return (i64(ADDI64(x->i64, y->i64)));

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        return (f64(ADDF64(x->f64, y->f64)));

    case MTYPE2(TYPE_I64, -TYPE_I64):
        l = x->adt->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = ADDI64(as_vector_i64(x)[i], y->i64);

        return vec;

    case MTYPE2(TYPE_I64, TYPE_I64):
        l = x->adt->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = ADDI64(as_vector_i64(x)[i], as_vector_i64(y)[i]);

        return vec;

    case MTYPE2(TYPE_F64, -TYPE_F64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = ADDF64(as_vector_f64(x)[i], y->f64);

        return vec;

    case MTYPE2(TYPE_F64, TYPE_F64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(x)[i] = ADDF64(as_vector_f64(&vec)[i], as_vector_f64(y)[i]);

        return vec;

    default:
        return error_type2(x->type, y->type, "add: unsupported types");
    }
}

rf_object_t rf_sub(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l;
    rf_object_t vec;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        return (i64(SUBI64(x->i64, y->i64)));

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        return (f64(SUBF64(x->f64, y->f64)));

    case MTYPE2(TYPE_I64, -TYPE_I64):
        l = x->adt->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = SUBI64(as_vector_i64(x)[i], y->i64);

        return vec;

    case MTYPE2(TYPE_I64, TYPE_I64):
        l = x->adt->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = SUBI64(as_vector_i64(x)[i], as_vector_i64(y)[i]);

        return vec;

    case MTYPE2(TYPE_F64, -TYPE_F64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = SUBF64(as_vector_f64(x)[i], y->f64);

        return vec;

    case MTYPE2(TYPE_F64, TYPE_F64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = SUBF64(as_vector_f64(x)[i], as_vector_f64(y)[i]);

        return vec;

    default:
        return error_type2(x->type, y->type, "sub: unsupported types");
    }
}

rf_object_t rf_mul(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l;
    rf_object_t vec;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        return (i64(MULI64(x->i64, y->i64)));

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        return (f64(MULF64(x->f64, y->f64)));

    case MTYPE2(TYPE_I64, -TYPE_I64):
        l = x->adt->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = MULI64(as_vector_i64(x)[i], y->i64);

        return vec;

    case MTYPE2(TYPE_I64, TYPE_I64):
        l = x->adt->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = MULI64(as_vector_i64(x)[i], as_vector_i64(y)[i]);

        return vec;

    case MTYPE2(TYPE_F64, -TYPE_F64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = MULF64(as_vector_f64(x)[i], y->f64);

        return vec;

    case MTYPE2(TYPE_F64, TYPE_F64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = MULF64(as_vector_f64(x)[i], as_vector_f64(y)[i]);

        return vec;

    default:
        return error_type2(x->type, y->type, "mul: unsupported types");
    }
}

rf_object_t rf_fdiv(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l;
    rf_object_t vec;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        return (f64(FDIVI64(x->i64, y->i64)));

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        return (f64(FDIVF64(x->f64, y->f64)));

    case MTYPE2(TYPE_I64, -TYPE_I64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = FDIVI64(as_vector_i64(x)[i], y->i64);

        return vec;

    case MTYPE2(TYPE_I64, TYPE_I64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = FDIVI64(as_vector_i64(x)[i], as_vector_i64(y)[i]);

        return vec;

    case MTYPE2(TYPE_F64, -TYPE_F64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = FDIVF64(as_vector_f64(x)[i], y->f64);

        return vec;

    case MTYPE2(TYPE_F64, TYPE_F64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = FDIVF64(as_vector_f64(x)[i], as_vector_f64(y)[i]);

        return vec;

    default:
        return error_type2(x->type, y->type, "fdiv: unsupported types");
    }
}

rf_object_t rf_mod(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l;
    rf_object_t vec;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        return (i64(MODI64(x->i64, y->i64)));

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        return (f64(MODF64(x->f64, y->f64)));

    case MTYPE2(TYPE_I64, -TYPE_I64):
        l = x->adt->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = MODI64(as_vector_i64(x)[i], y->i64);

        return vec;

    case MTYPE2(TYPE_I64, TYPE_I64):
        l = x->adt->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = MODI64(as_vector_i64(x)[i], as_vector_i64(y)[i]);

        return vec;

    case MTYPE2(TYPE_F64, -TYPE_F64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = MODF64(as_vector_f64(x)[i], y->f64);

        return vec;

    case MTYPE2(TYPE_F64, TYPE_F64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = MODF64(as_vector_f64(x)[i], as_vector_f64(y)[i]);

        return vec;

    default:
        return error_type2(x->type, y->type, "mod: unsupported types");
    }
}

rf_object_t rf_mod(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l;
    rf_object_t vec;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        return (i64(MODI64(x->i64, y->i64)));

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        return (f64(MODF64(x->f64, y->f64)));

    case MTYPE2(TYPE_I64, -TYPE_I64):
        l = x->adt->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = MODI64(as_vector_i64(x)[i], y->i64);

        return vec;

    case MTYPE2(TYPE_I64, TYPE_I64):
        l = x->adt->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = MODI64(as_vector_i64(x)[i], as_vector_i64(y)[i]);

        return vec;

    case MTYPE2(TYPE_F64, -TYPE_F64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = MODF64(as_vector_f64(x)[i], y->f64);

        return vec;

    case MTYPE2(TYPE_F64, TYPE_F64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = MODF64(as_vector_f64(x)[i], as_vector_f64(y)[i]);

        return vec;

    default:
        return error_type2(x->type, y->type, "mod: unsupported types");
    }
}

rf_object_t rf_like(rf_object_t *x, rf_object_t *y)
{
    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(TYPE_CHAR, TYPE_CHAR):
        return (bool(string_match(as_string(x), as_string(y))));

    default:
        return error_type2(x->type, y->type, "like: unsupported types");
    }
}

rf_object_t rf_eq(rf_object_t *x, rf_object_t *y)
{
    return bool(rfi_eq(x, y));
}

rf_object_t rf_ne_i64_i64(rf_object_t *x, rf_object_t *y)
{
    return (bool(!rfi_eq(x, y)));
}

rf_object_t rf_lt(rf_object_t *x, rf_object_t *y)
{
    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        return (bool(x->i64 < y->i64));

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        return (bool(x->f64 < y->f64));

    default:
        return error_type2(x->type, y->type, "lt: unsupported types");
    }
}

rf_object_t rf_le(rf_object_t *x, rf_object_t *y)
{
    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        return (bool(x->i64 <= y->i64));

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        return (bool(x->f64 <= y->f64));

    default:
        return error_type2(x->type, y->type, "le: unsupported types");
    }
}

rf_object_t rf_gt(rf_object_t *x, rf_object_t *y)
{
    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        return (bool(x->i64 > y->i64));

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        return (bool(x->f64 > y->f64));

    default:
        return error_type2(x->type, y->type, "gt: unsupported types");
    }
}

rf_object_t rf_ge(rf_object_t *x, rf_object_t *y)
{
    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        return (bool(x->i64 >= y->i64));

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        return (bool(x->f64 >= y->f64));

    default:
        return error_type2(x->type, y->type, "ge: unsupported types");
    }
}

rf_object_t rf_and(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l;
    rf_object_t res;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_BOOL, -TYPE_BOOL):
        return (bool(x->bool && y->bool));

    case MTYPE2(TYPE_BOOL, TYPE_BOOL):
        l = x->adt->len;
        res = vector_bool(x->adt->len);
        for (i = 0; i < l; i++)
            as_vector_bool(&res)[i] = as_vector_bool(x)[i] & as_vector_bool(y)[i];

        return res;

    default:
        return error_type2(x->type, y->type, "and: unsupported types");
    }
}

rf_object_t rf_get(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t yl, xl;
    rf_object_t vec;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(TYPE_BOOL, -TYPE_I64):
    case MTYPE2(TYPE_I64, -TYPE_I64):
    case MTYPE2(TYPE_F64, -TYPE_I64):
    case MTYPE2(TYPE_TIMESTAMP, -TYPE_I64):
    case MTYPE2(TYPE_GUID, -TYPE_I64):
    case MTYPE2(TYPE_CHAR, -TYPE_I64):
    case MTYPE2(TYPE_LIST, -TYPE_I64):
        return vector_get(x, y->i64);

    case MTYPE2(TYPE_TABLE, -TYPE_SYMBOL):
        return dict_get(x, y);

    case MTYPE2(TYPE_BOOL, TYPE_I64):
        yl = y->adt->len;
        xl = x->adt->len;
        vec = vector_bool(yl);
        for (i = 0; i < yl; i++)
        {
            if (as_vector_bool(y)[i] >= xl)
                as_vector_bool(&vec)[i] = false;
            else
                as_vector_bool(&vec)[i] = as_vector_bool(x)[(i32_t)as_vector_bool(y)[i]];
        }

        return vec;

    case MTYPE2(TYPE_I64, TYPE_I64):
        yl = y->adt->len;
        xl = x->adt->len;
        vec = vector_i64(yl);
        for (i = 0; i < yl; i++)
        {
            if (as_vector_i64(y)[i] >= xl)
                as_vector_i64(&vec)[i] = NULL_I64;
            else
                as_vector_i64(&vec)[i] = as_vector_i64(x)[(i32_t)as_vector_i64(y)[i]];
        }

        return vec;

    case MTYPE2(TYPE_F64, TYPE_I64):
        yl = y->adt->len;
        xl = x->adt->len;
        vec = vector_f64(yl);
        for (i = 0; i < yl; i++)
        {
            if (as_vector_i64(y)[i] >= xl)
                as_vector_f64(&vec)[i] = NULL_F64;
            else
                as_vector_f64(&vec)[i] = as_vector_f64(x)[(i32_t)as_vector_i64(y)[i]];
        }

        return vec;

    case MTYPE2(TYPE_TIMESTAMP, TYPE_I64):
        yl = y->adt->len;
        xl = x->adt->len;
        vec = vector_timestamp(yl);
        for (i = 0; i < yl; i++)
        {
            if (as_vector_i64(y)[i] >= xl)
                as_vector_timestamp(&vec)[i] = NULL_I64;
            else
                as_vector_timestamp(&vec)[i] = as_vector_timestamp(x)[(i32_t)as_vector_i64(y)[i]];
        }

        return vec;

        // case MTYPE2(TYPE_GUID, TYPE_I64):
        //     yl = y->adt->len;
        //     xl = x->adt->len;
        //     vec = vector_guid(yl);
        //     for (i = 0; i < yl; i++)
        //     {
        //         if (as_vector_i64(y)[i] >= xl)
        //             as_vector_guid(&vec)[i] = guid(NULL_GUID);
        //         else
        //             as_vector_guid(&vec)[i] = as_vector_guid(x)[(i32_t)as_vector_i64(y)[i]];
        //     }

        //     return vec;

    case MTYPE2(TYPE_CHAR, TYPE_I64):
        yl = y->adt->len;
        xl = x->adt->len;
        vec = string(yl);
        for (i = 0; i < yl; i++)
        {
            if (as_vector_i64(y)[i] >= xl)
                as_string(&vec)[i] = ' ';
            else
                as_string(&vec)[i] = as_string(x)[(i32_t)as_vector_i64(y)[i]];
        }

        return vec;

    case MTYPE2(TYPE_LIST, TYPE_I64):
        yl = y->adt->len;
        xl = x->adt->len;
        vec = list(yl);
        for (i = 0; i < yl; i++)
        {
            if (as_vector_i64(y)[i] >= xl)
                as_list(&vec)[i] = null();
            else
                as_list(&vec)[i] = rf_object_clone(&as_list(x)[(i32_t)as_vector_i64(y)[i]]);
        }

        return vec;

    default:
        return error_type2(x->type, y->type, "get: unsupported types");
    }
}

rf_object_t rf_find_I64_I64(rf_object_t *x, rf_object_t *y)
{
    u64_t i, n, range, xl = x->adt->len, yl = y->adt->len;
    i64_t max = 0, min = 0;
    rf_object_t vec = vector_i64(yl), found;
    i64_t *iv1 = as_vector_i64(x), *iv2 = as_vector_i64(y),
          *ov = as_vector_i64(&vec), *fv;
    ht_t *ht;

    for (i = 0; i < xl; i++)
    {
        if (iv1[i] > max)
            max = iv1[i];
        else if (iv1[i] < min)
            min = iv1[i];
    }

#define mask -1ll
#define normalize(k) ((u64_t)(k - min))
    // if range fits in 64 mb, use vector positions instead of hash table
    range = max - min + 1;
    if (range < 1024 * 1024 * 64)
    {
        found = vector_i64(range);
        fv = as_vector_i64(&found);
        memset(fv, 255, sizeof(i64_t) * range);

        for (i = 0; i < xl; i++)
        {
            n = normalize(iv1[i]);
            if (fv[n] == mask)
                fv[n] = i;
        }

        for (i = 0; i < yl; i++)
        {
            n = normalize(iv2[i]);
            if (iv2[i] < min || iv2[i] > max || fv[n] == mask)
                ov[i] = NULL_I64;
            else
                ov[i] = fv[n];
        }

        rf_object_free(&found);

        return vec;
    }

    // otherwise, use a hash table
    ht = ht_new(xl, &rfi_kmh_hash, &i64_cmp);

    for (i = 0; i < xl; i++)
        ht_insert(ht, iv1[i], i);

    for (i = 0; i < yl; i++)
        ov[i] = ht_get(ht, iv2[i]);

    ht_free(ht);

    return vec;
}

rf_object_t rf_find(rf_object_t *x, rf_object_t *y)
{
    i64_t l, i;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(TYPE_BOOL, -TYPE_BOOL):
    case MTYPE2(TYPE_I64, -TYPE_I64):
    case MTYPE2(TYPE_F64, -TYPE_F64):
    case MTYPE2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
    case MTYPE2(TYPE_GUID, -TYPE_GUID):
    case MTYPE2(TYPE_CHAR, -TYPE_CHAR):
    case MTYPE2(TYPE_LIST, -TYPE_LIST):
        l = x->adt->len;
        i = vector_find(x, y);

        if (i == l)
            return i64(NULL_I64);
        else
            return i64(i);

        return i64(vector_find(x, y));

    case MTYPE2(TYPE_I64, TYPE_I64):
        return rf_find_I64_I64(x, y);

    default:
        return error_type2(x->type, y->type, "find: unsupported types");
    }
}

rf_object_t rf_concat(rf_object_t *x, rf_object_t *y)
{
    i64_t i, xl, yl;
    rf_object_t vec;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_BOOL, -TYPE_BOOL):
        vec = vector_bool(2);
        as_vector_bool(&vec)[0] = x->bool;
        as_vector_bool(&vec)[1] = y->bool;
        return vec;

    case MTYPE2(-TYPE_I64, -TYPE_I64):
        vec = vector_i64(2);
        as_vector_i64(&vec)[0] = x->i64;
        as_vector_i64(&vec)[1] = y->i64;
        return vec;

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        vec = vector_f64(2);
        as_vector_f64(&vec)[0] = x->f64;
        as_vector_f64(&vec)[1] = y->f64;
        return vec;

    case MTYPE2(-TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        vec = vector_timestamp(2);
        as_vector_timestamp(&vec)[0] = x->i64;
        as_vector_timestamp(&vec)[1] = y->i64;
        return vec;

    case MTYPE2(-TYPE_GUID, -TYPE_GUID):
        vec = vector_guid(2);
        memcpy(&as_vector_guid(&vec)[0], x->guid, sizeof(guid_t));
        memcpy(&as_vector_guid(&vec)[1], y->guid, sizeof(guid_t));
        return vec;

    case MTYPE2(-TYPE_CHAR, -TYPE_CHAR):
        vec = string(2);
        as_string(&vec)[0] = x->schar;
        as_string(&vec)[1] = y->schar;
        return vec;

    case MTYPE2(TYPE_BOOL, -TYPE_BOOL):
        xl = x->adt->len;
        vec = vector_bool(xl + 1);
        for (i = 0; i < xl; i++)
            as_vector_bool(&vec)[i] = as_vector_bool(x)[i];

        as_vector_bool(&vec)[xl] = y->bool;
        return vec;

    case MTYPE2(TYPE_I64, -TYPE_I64):
        xl = x->adt->len;
        vec = vector_i64(xl + 1);
        for (i = 0; i < xl; i++)
            as_vector_i64(&vec)[i] = as_vector_i64(x)[i];

        as_vector_i64(&vec)[xl] = y->i64;
        return vec;

    case MTYPE2(TYPE_F64, -TYPE_F64):
        xl = x->adt->len;
        vec = vector_f64(xl + 1);
        for (i = 0; i < xl; i++)
            as_vector_f64(&vec)[i] = as_vector_f64(x)[i];

        as_vector_f64(&vec)[xl] = y->f64;
        return vec;

    case MTYPE2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        xl = x->adt->len;
        vec = vector_timestamp(xl + 1);
        for (i = 0; i < xl; i++)
            as_vector_timestamp(&vec)[i] = as_vector_timestamp(x)[i];

        as_vector_timestamp(&vec)[xl] = y->i64;
        return vec;

    case MTYPE2(TYPE_GUID, -TYPE_GUID):
        xl = x->adt->len;
        vec = vector_guid(xl + 1);
        for (i = 0; i < xl; i++)
            as_vector_guid(&vec)[i] = as_vector_guid(x)[i];

        memcpy(&as_vector_guid(&vec)[xl], y->guid, sizeof(guid_t));
        return vec;

    case MTYPE2(TYPE_BOOL, TYPE_BOOL):
        xl = x->adt->len;
        yl = y->adt->len;
        vec = vector_bool(xl + yl);
        for (i = 0; i < xl; i++)
            as_vector_bool(&vec)[i] = as_vector_bool(x)[i];
        for (i = 0; i < yl; i++)
            as_vector_bool(&vec)[i + xl] = as_vector_bool(y)[i];
        return vec;

    case MTYPE2(TYPE_I64, TYPE_I64):
        xl = x->adt->len;
        yl = y->adt->len;
        vec = vector_i64(xl + yl);
        for (i = 0; i < xl; i++)
            as_vector_i64(&vec)[i] = as_vector_i64(x)[i];
        for (i = 0; i < yl; i++)
            as_vector_i64(&vec)[i + xl] = as_vector_i64(y)[i];
        return vec;

    case MTYPE2(TYPE_F64, TYPE_F64):
        xl = x->adt->len;
        yl = y->adt->len;
        vec = vector_f64(xl + yl);
        for (i = 0; i < xl; i++)
            as_vector_f64(&vec)[i] = as_vector_f64(x)[i];
        for (i = 0; i < yl; i++)
            as_vector_f64(&vec)[i + xl] = as_vector_f64(y)[i];
        return vec;

    case MTYPE2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
        xl = x->adt->len;
        yl = y->adt->len;
        vec = vector_timestamp(xl + yl);
        for (i = 0; i < xl; i++)
            as_vector_timestamp(&vec)[i] = as_vector_timestamp(x)[i];
        for (i = 0; i < yl; i++)
            as_vector_timestamp(&vec)[i + xl] = as_vector_timestamp(y)[i];
        return vec;

    case MTYPE2(TYPE_GUID, TYPE_GUID):
        xl = x->adt->len;
        yl = y->adt->len;
        vec = vector_guid(xl + yl);
        for (i = 0; i < xl; i++)
            as_vector_guid(&vec)[i] = as_vector_guid(x)[i];
        for (i = 0; i < yl; i++)
            as_vector_guid(&vec)[i + xl] = as_vector_guid(y)[i];
        return vec;

    case MTYPE2(TYPE_CHAR, TYPE_CHAR):
        xl = x->adt->len;
        yl = y->adt->len;
        vec = string(xl + yl);
        for (i = 0; i < xl; i++)
            as_string(&vec)[i] = as_string(x)[i];
        for (i = 0; i < yl; i++)
            as_string(&vec)[i + xl] = as_string(y)[i];
        return vec;

    case MTYPE2(TYPE_LIST, TYPE_LIST):
        xl = x->adt->len;
        yl = y->adt->len;
        vec = list(xl + yl);
        for (i = 0; i < xl; i++)
            as_list(&vec)[i] = rf_object_clone(&as_list(x)[i]);
        for (i = 0; i < yl; i++)
            as_list(&vec)[i + xl] = rf_object_clone(&as_list(y)[i]);
        return vec;

    default:
        return error_type2(x->type, y->type, "concat: unsupported types");
    }
}

rf_object_t rf_filter_I64_Bool(rf_object_t *x, rf_object_t *y)
{
    if (x->adt->len != y->adt->len)
        return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

    i32_t i, j = 0;
    i64_t l = x->adt->len;
    i64_t *iv1 = as_vector_i64(x);
    bool_t *iv2 = as_vector_bool(y);
    rf_object_t res = vector_i64(l);
    i64_t *ov = as_vector_i64(&res);

    for (i = 0; i < l; i++)
        if (iv2[i])
            ov[j++] = iv1[i];

    vector_shrink(&res, j);

    return res;
}

rf_object_t rf_filter_Table_Bool(rf_object_t *x, rf_object_t *y)
{
    rf_object_t *vals = &as_list(x)[1], outvals, col;
    i64_t i, j, p = NULL_I64, len = vals->adt->len;
    bool_t *m = as_vector_bool(y);

    outvals = list(len);

    for (i = 0; i < len; i++)
    {
        col = vector_filter(&as_list(vals)[i], m, p);
        p = col.adt->len;
        as_list(&outvals)[i] = col;
    }

    return table(rf_object_clone(&as_list(x)[0]), outvals);
}

rf_object_t rf_take_i64_i64(rf_object_t *x, rf_object_t *y)
{
    i64_t i, l = x->i64, n = y->i64;
    rf_object_t vec = vector_i64(l);
    i64_t *ov = as_vector_i64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = n;

    return vec;
}
