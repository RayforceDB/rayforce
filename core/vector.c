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

#include "rayforce.h"
#include "vector.h"
#include "util.h"
#include "format.h"
#include "env.h"

#define panic_type(m, t1) panic(str_fmt(0, "%s: '%s'", m, env_get_typename(t1)))
#define panic_type2(m, t1, t2) panic(str_fmt(0, "%s: '%s', '%s'", m, env_get_typename(t1), env_get_typename(t2)))

i64_t size_of_val(type_t type)
{
    switch (type)
    {
    case TYPE_BOOL:
        return sizeof(bool_t);
    case TYPE_I64:
        return sizeof(i64_t);
    case TYPE_F64:
        return sizeof(f64_t);
    case TYPE_SYMBOL:
        return sizeof(i64_t);
    case TYPE_TIMESTAMP:
        return sizeof(i64_t);
    case TYPE_GUID:
        return sizeof(guid_t);
    case TYPE_CHAR:
        return sizeof(char_t);
    case TYPE_LIST:
        return sizeof(rf_object_t);
    default:
        panic_type("size of val unknown type", type);
    }
}

/*
 * Creates new vector of type type
 */
rf_object_t vector(type_t type, i64_t len)
{
    i64_t size = capacity(len * size_of_val(type) + sizeof(header_t));
    header_t *adt = rf_malloc(size);

    if (adt == NULL)
        panic("OOM");

    adt->len = len;
    adt->rc = 1;
    adt->attrs = 0;

    rf_object_t v = {
        .type = type,
        .adt = adt,
    };

    return v;
}

rf_object_t vector_push(rf_object_t *vec, rf_object_t value)
{
    i64_t i, l;
    rf_object_t lst;

    if (!is_vector(vec))
        return error(ERR_TYPE, "vector push: can not push to scalar");

    l = vec->adt->len;

    // if (l == 0)
    // {
    //     vec.type = -value.type;
    // }

    // change vector type to a list
    // if (vec->type != -value.type && vec->type != TYPE_LIST)
    // {
    //     lst = list(l + 1);
    //     for (i = 0; i < l; i++)
    //         as_list(&lst)[i] = vector_get(&vec[i], i);

    //     as_list(&lst)[l] = value;

    //     rf_object_free(vec);

    //     *vec = lst;
    //     return null();
    // }

    switch (vec->type)
    {
    case TYPE_BOOL:
        push(vec, bool_t, value.bool);
        return null();
    case TYPE_I64:
        push(vec, i64_t, value.i64);
        return null();
    case TYPE_F64:
        push(vec, f64_t, value.f64);
        return null();
    case TYPE_SYMBOL:
        push(vec, i64_t, value.i64);
        return null();
    case TYPE_TIMESTAMP:
        push(vec, i64_t, value.i64);
        return null();
    case TYPE_GUID:
        push(vec, guid_t, *value.guid);
        return null();
    case TYPE_CHAR:
        push(vec, char_t, value.schar);
        return null();
    case TYPE_LIST:
        push(vec, rf_object_t, value);
        return null();
    default:
        return error(ERR_TYPE, "vector push: unknown type");
    }
}

rf_object_t vector_pop(rf_object_t *vec)
{
    guid_t *g;

    if (!is_vector(vec) || vec->adt->len == 0)
        return null();

    switch (vec->type)
    {
    case TYPE_BOOL:
        return bool(pop(vec, bool_t));
    case TYPE_I64:
        return i64(pop(vec, i64_t));
    case TYPE_F64:
        return f64(pop(vec, f64_t));
    case TYPE_SYMBOL:
        return symboli64(pop(vec, i64_t));
    case TYPE_TIMESTAMP:
        return i64(pop(vec, i64_t));
    case TYPE_GUID:
        g = as_vector_guid(vec);
        return guid(g[--vec->adt->len].data);
    case TYPE_CHAR:
        return schar(pop(vec, char_t));
    case TYPE_LIST:
        if (vec->adt == NULL)
            return null();
        return pop(vec, rf_object_t);
    default:
        panic_type("vector pop: unknown type", vec->type);
    }
}

null_t vector_reserve(rf_object_t *vector, u32_t len)
{
    switch (vector->type)
    {
    case TYPE_BOOL:
        reserve(vector, bool_t, len);
        return;
    case TYPE_I64:
        reserve(vector, i64_t, len);
        return;
    case TYPE_F64:
        reserve(vector, f64_t, len);
        return;
    case TYPE_SYMBOL:
        reserve(vector, i64_t, len);
        return;
    case TYPE_TIMESTAMP:
        reserve(vector, i64_t, len);
        return;
    case TYPE_GUID:
        reserve(vector, guid_t, len);
        return;
    case TYPE_CHAR:
        reserve(vector, char_t, len);
        return;
    case TYPE_LIST:
        reserve(vector, rf_object_t, len);
        return;
    default:
        panic_type("vector reserve: unknown type", vector->type);
    }
}

null_t vector_grow(rf_object_t *vector, u32_t len)
{
    // calculate size of vector with new length
    i64_t new_size = capacity(len * size_of_val(vector->type) + sizeof(header_t));

    rf_realloc(vector->adt, new_size);
    vector->adt->len = len;
}

null_t vector_shrink(rf_object_t *vector, u32_t len)
{
    if (vector->adt->len == len)
        return;

    // calculate size of vector with new length
    i64_t new_size = capacity(len * size_of_val(vector->type) + sizeof(header_t));

    rf_realloc(vector->adt, new_size);
    vector->adt->len = len;
}

i64_t vector_find(rf_object_t *vector, rf_object_t *key)
{
    char_t kc, *vc;
    bool_t kb, *vb;
    i64_t ki, *vi;
    f64_t kf, *vf;
    rf_object_t *kl, *vl;
    i64_t i, l;
    guid_t *kg, *vg;

    if (key->type != -vector->type && vector->type != TYPE_LIST)
        return vector->adt->len;

    switch (vector->type)
    {
    case TYPE_BOOL:
        l = vector->adt->len;
        vb = as_vector_bool(vector);
        kb = key->bool;
        for (i = 0; i < l; i++)
            if (vb[i] == kb)
                return i;
        return l;
    case TYPE_I64:
        l = vector->adt->len;
        vi = as_vector_i64(vector);
        ki = key->i64;
        for (i = 0; i < l; i++)
            if (vi[i] == ki)
                return i;
        return l;
    case TYPE_F64:
        l = vector->adt->len;
        vf = as_vector_f64(vector);
        kf = key->f64;
        for (i = 0; i < l; i++)
            if (vf[i] == kf)
                return i;
        return l;
    case TYPE_SYMBOL:
        l = vector->adt->len;
        vi = as_vector_symbol(vector);
        ki = key->i64;
        for (i = 0; i < l; i++)
            if (vi[i] == ki)
                return i;
        return l;
    case TYPE_TIMESTAMP:
        l = vector->adt->len;
        vi = as_vector_timestamp(vector);
        ki = key->i64;
        for (i = 0; i < l; i++)
            if (vi[i] == ki)
                return i;
        return l;
    case TYPE_GUID:
        l = vector->adt->len;
        vg = as_vector_guid(vector);
        kg = key->guid;
        for (i = 0; i < l; i++)
            if (memcmp(vg + i, kg, sizeof(guid_t)) == 0)
                return i;
        return l;
    case TYPE_CHAR:
        l = vector->adt->len;
        vc = as_string(vector);
        kc = key->schar;
        for (i = 0; i < l; i++)
            if (vc[i] == kc)
                return i;
        return l;
    case TYPE_LIST:
        if (is_null(vector))
            return 0;

        l = vector->adt->len;
        vl = as_list(vector);
        kl = key;
        for (i = 0; i < l; i++)
            if (rf_object_eq(&vl[i], kl))
                return i;
        return l;
    default:
        return vector->adt->len;
    }
}

rf_object_t vector_get(rf_object_t *vec, i64_t index)
{
    i64_t l;

    if (!is_vector(vec))
        return error(ERR_TYPE, "vector get: can not get from scalar");

    l = vec->adt->len;

    switch (vec->type)
    {
    case TYPE_BOOL:
        if (index < l)
            return bool(as_vector_bool(vec)[index]);
        return bool(false);
    case TYPE_I64:
        if (index < l)
            return i64(as_vector_i64(vec)[index]);
        return i64(NULL_I64);
    case TYPE_F64:
        if (index < l)
            return f64(as_vector_f64(vec)[index]);
        return f64(NULL_F64);
    case TYPE_SYMBOL:
        if (index < l)
            return symboli64(as_vector_i64(vec)[index]);
        return symboli64(NULL_I64);
    case TYPE_TIMESTAMP:
        if (index < l)
            return timestamp(as_vector_i64(vec)[index]);
        return timestamp(NULL_I64);
    case TYPE_GUID:
        if (index < l)
            return guid(as_vector_guid(vec)[index].data);
        return guid(NULL);
    case TYPE_CHAR:
        if (index < l)
            return schar(as_string(vec)[index]);
        return schar(0);
    case TYPE_LIST:
        if (vec->adt == NULL)
            return *vec;
        if (index < l)
            return rf_object_clone(&as_list(vec)[index]);
        return null();
    default:
        return null();
    }
}

rf_object_t vector_set(rf_object_t *vec, i64_t index, rf_object_t value)
{
    guid_t *g;
    i64_t i, l;
    rf_object_t lst;

    // change vector type to a list
    if (vec->type != -value.type && vec->type != TYPE_LIST)
    {
        l = vec->adt->len;
        if (index >= l)
            return error(ERR_LENGTH, "vector set: index out of bounds");

        lst = list(l);
        for (i = 0; i < index; i++)
            as_list(&lst)[i] = vector_get(&vec[i], i);

        as_list(&lst)[index] = value;

        rf_object_free(vec);

        *vec = lst;
        return null();
    }

    l = vec->adt->len;
    if (index >= l)
        return error(ERR_LENGTH, "vector set: index out of bounds");

    switch (vec->type)
    {
    case TYPE_BOOL:
        as_vector_bool(vec)[index] = value.bool;
        break;
    case TYPE_I64:
        as_vector_i64(vec)[index] = value.i64;
        break;
    case TYPE_F64:
        as_vector_f64(vec)[index] = value.f64;
        break;
    case TYPE_SYMBOL:
        as_vector_i64(vec)[index] = value.i64;
        break;
    case TYPE_TIMESTAMP:
        as_vector_i64(vec)[index] = value.i64;
        break;
    case TYPE_GUID:
        g = as_vector_guid(vec);
        memcpy(g + index, value.guid, sizeof(guid_t));
        break;
    case TYPE_CHAR:
        as_string(vec)[index] = value.schar;
        break;
    case TYPE_LIST:
        as_list(vec)[index] = rf_object_clone(&value);
        break;
    default:
        panic_type("vector_set: unknown type", vec->type);
    }

    return null();
}

/*
 * Try to flatten list in a vector if all elements are of the same type
 */
rf_object_t vector_flatten(rf_object_t *x)
{
    i64_t i, l = x->adt->len;
    type_t type;
    rf_object_t vec;

    if (l == 0)
        return rf_object_clone(x);

    type = as_list(x)[0].type;

    // Only scalar types inside a list can be flattened
    if (type > 0)
        return rf_object_clone(x);

    for (i = 1; i < l; i++)
        if (as_list(x)[i].type != type)
            return rf_object_clone(x);

    switch (type)
    {
    case -TYPE_BOOL:
        vec = vector_bool(l);
        for (i = 0; i < l; i++)
            as_vector_bool(&vec)[i] = as_list(x)[i].bool;
        break;
    case -TYPE_I64:
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = as_list(x)[i].i64;
        break;
    case -TYPE_F64:
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = as_list(x)[i].f64;
        break;
    case -TYPE_SYMBOL:
        vec = vector_symbol(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = as_list(x)[i].i64;
        break;
    case -TYPE_TIMESTAMP:
        vec = vector_timestamp(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = as_list(x)[i].i64;
        break;
    case -TYPE_GUID:
        vec = vector_guid(l);
        for (i = 0; i < l; i++)
            as_vector_guid(&vec)[i] = *as_list(x)[i].guid;
        break;
    default:
        return rf_object_clone(x);
    }

    return vec;
}

rf_object_t vector_filter(rf_object_t *x, bool_t mask[], i64_t len)
{
    i64_t i, j = 0, l, ol;
    rf_object_t vec;

    if (!is_vector(x))
        return error(ERR_TYPE, "vector filter: can not filter scalar");

    switch (x->type)
    {
    case TYPE_BOOL:
        l = x->adt->len;
        ol = (len == NULL_I64) ? (i64_t)x->adt->len : len;
        vec = vector_bool(ol);
        for (i = 0; (j < ol && i < l); i++)
            if (mask[i])
                as_vector_bool(&vec)[j++] = as_vector_bool(x)[i];
        if (len == NULL_I64)
            vector_shrink(&vec, j);
        return vec;
    case TYPE_I64:
        l = x->adt->len;
        ol = (len == NULL_I64) ? (i64_t)x->adt->len : len;
        vec = vector_i64(ol);
        for (i = 0; (j < ol && i < l); i++)
            if (mask[i])
                as_vector_i64(&vec)[j++] = as_vector_i64(x)[i];
        if (len == NULL_I64)
            vector_shrink(&vec, j);
        return vec;
    case TYPE_F64:
        l = x->adt->len;
        ol = (len == NULL_I64) ? (i64_t)x->adt->len : len;
        vec = vector_f64(ol);
        for (i = 0; (j < ol && i < l); i++)
            if (mask[i])
                as_vector_f64(&vec)[j++] = as_vector_f64(x)[i];
        if (len == NULL_I64)
            vector_shrink(&vec, j);
        return vec;
    case TYPE_SYMBOL:
        l = x->adt->len;
        ol = (len == NULL_I64) ? (i64_t)x->adt->len : len;
        vec = vector_symbol(ol);
        for (i = 0; (j < ol && i < l); i++)
            if (mask[i])
                as_vector_i64(&vec)[j++] = as_vector_i64(x)[i];
        if (len == NULL_I64)
            vector_shrink(&vec, j);
        return vec;
    case TYPE_TIMESTAMP:
        l = x->adt->len;
        ol = (len == NULL_I64) ? (i64_t)x->adt->len : len;
        vec = vector_timestamp(ol);
        for (i = 0; (j < ol && i < l); i++)
            if (mask[i])
                as_vector_i64(&vec)[j++] = as_vector_i64(x)[i];
        if (len == NULL_I64)
            vector_shrink(&vec, j);
        return vec;
    case TYPE_GUID:
        l = x->adt->len;
        ol = (len == NULL_I64) ? (i64_t)x->adt->len : len;
        vec = vector_guid(ol);
        for (i = 0; (j < ol && i < l); i++)
            if (mask[i])
                as_vector_guid(&vec)[j++] = as_vector_guid(x)[i];
        if (len == NULL_I64)
            vector_shrink(&vec, j);
        return vec;
    case TYPE_CHAR:
        l = x->adt->len;
        ol = (len == NULL_I64) ? (i64_t)x->adt->len : len;
        vec = string(ol);
        for (i = 0; (j < ol && i < l); i++)
            if (mask[i])
                as_string(&vec)[j++] = as_string(x)[i];
        if (len == NULL_I64)
            vector_shrink(&vec, j);
        return vec;
    case TYPE_LIST:
        if (is_null(x))
            return null();
        l = x->adt->len;
        ol = (len == NULL_I64) ? (i64_t)x->adt->len : len;
        vec = list(ol);
        for (i = 0; (j < ol && i < l); i++)
            if (mask[i])
                as_list(&vec)[j++] = rf_object_clone(&as_list(x)[i]);
        if (len == NULL_I64)
            vector_shrink(&vec, j);
        return vec;
    default:
        panic_type("vector_filter: unknown type", x->type);
    }
}

null_t vector_clear(rf_object_t *vec)
{
    if (vec->type == TYPE_LIST)
    {
        i64_t i, l = vec->adt->len;
        rf_object_t *list = as_list(vec);

        for (i = 0; i < l; i++)
            rf_object_free(&list[i]);
    }

    vector_shrink(vec, 0);
}

null_t vector_free(rf_object_t *vec)
{
    rf_free(vec->adt);
}

rf_object_t rf_list(rf_object_t *x, u32_t n)
{
    rf_object_t l = list(n);
    u32_t i;

    for (i = 0; i < n; i++)
    {
        rf_object_t *item = x + i;
        as_list(&l)[i] = rf_object_clone(item);
    }

    return l;
}
