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

i64_t size_of_val(type_t type)
{
    type_t typet = type - TYPE_BOOL;

    static null_t *types_table[] = {&&type_bool, &&type_i64, &&type_f64, &&type_symbol,
                                    &&type_timestamp, &&type_guid, &&type_char, &&type_list};

    goto *types_table[(i32_t)typet];

type_bool:
    return sizeof(bool_t);
type_i64:
    return sizeof(i64_t);
type_f64:
    return sizeof(f64_t);
type_symbol:
    return sizeof(i64_t);
type_timestamp:
    return sizeof(i64_t);
type_guid:
    return sizeof(guid_t);
type_char:
    return sizeof(char_t);
type_list:
    return sizeof(rf_object_t);
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

i64_t vector_push(rf_object_t *vector, rf_object_t value)
{
    type_t type = vector->type - TYPE_BOOL;

    static null_t *types_table[] = {&&type_bool, &&type_i64, &&type_f64, &&type_symbol,
                                    &&type_timestamp, &&type_guid, &&type_char, &&type_list};

    goto *types_table[(i32_t)type];

type_bool:
    push(vector, bool_t, value.bool);
    return vector->adt->len;
type_i64:
    push(vector, i64_t, value.i64);
    return vector->adt->len;
type_f64:
    push(vector, f64_t, value.f64);
    return vector->adt->len;
type_symbol:
    push(vector, i64_t, value.i64);
    return vector->adt->len;
type_timestamp:
    push(vector, i64_t, value.i64);
    return vector->adt->len;
type_guid:
    push(vector, guid_t, *value.guid);
    return vector->adt->len;
type_char:
    push(vector, char_t, value.schar);
    return vector->adt->len;
type_list:
    push(vector, rf_object_t, value);
    return vector->adt->len;
}

rf_object_t vector_pop(rf_object_t *vector)
{
    if (vector->adt->len == 0)
        return null();

    type_t type = vector->type - TYPE_BOOL;
    rf_object_t o;
    guid_t *g;

    static null_t *types_table[] = {&&type_bool, &&type_i64, &&type_f64, &&type_symbol,
                                    &&type_timestamp, &&type_guid, &&type_char, &&type_list};

    goto *types_table[(i32_t)type];

type_bool:
    return bool(pop(vector, bool_t));
type_i64:
    return i64(pop(vector, i64_t));
type_f64:
    return f64(pop(vector, f64_t));
type_symbol:
    return symboli64(pop(vector, i64_t));
type_timestamp:
    return i64(pop(vector, i64_t));
type_guid:
    g = as_vector_guid(vector);
    return guid(g[--vector->adt->len].data);
type_char:
    return schar(pop(vector, char_t));
type_list:
    o = pop(vector, rf_object_t);
    return rf_object_clone(&o);
}

null_t vector_reserve(rf_object_t *vector, u32_t len)
{
    type_t type = vector->type - TYPE_BOOL;

    static null_t *types_table[] = {&&type_bool, &&type_i64, &&type_f64, &&type_symbol,
                                    &&type_timestamp, &&type_guid, &&type_char, &&type_list};

    goto *types_table[(i32_t)type];

type_bool:
    reserve(vector, bool_t, len);
    return;
type_i64:
    reserve(vector, i64_t, len);
    return;
type_f64:
    reserve(vector, f64_t, len);
    return;
type_symbol:
    reserve(vector, i64_t, len);
    return;
type_timestamp:
    reserve(vector, i64_t, len);
    return;
type_guid:
    reserve(vector, guid_t, len);
    return;
type_char:
    reserve(vector, i8_t, len);
    return;
type_list:
    reserve(vector, rf_object_t, len);
    return;
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
    type_t type = vector->type;

    static null_t *types_table[] = {&&type_null, &&type_bool, &&type_i64, &&type_f64, &&type_symbol,
                                    &&type_timestamp, &&type_guid, &&type_char, &&type_list};

    l = vector->adt->len;

    goto *types_table[(i32_t)type];

type_null:
    return 0;
type_bool:
    vb = as_vector_bool(vector);
    kb = key->bool;
    for (i = 0; i < l; i++)
        if (vb[i] == kb)
            return i;
    return l;
type_i64:
    vi = as_vector_i64(vector);
    ki = key->i64;
    for (i = 0; i < l; i++)
        if (vi[i] == ki)
            return i;
    return l;
type_f64:
    vf = as_vector_f64(vector);
    kf = key->f64;
    for (i = 0; i < l; i++)
        if (vf[i] == kf)
            return i;
    return l;
type_symbol:
    vi = as_vector_symbol(vector);
    ki = key->i64;
    for (i = 0; i < l; i++)
        if (vi[i] == ki)
            return i;
    return l;
type_timestamp:
    vi = as_vector_timestamp(vector);
    ki = key->i64;
    for (i = 0; i < l; i++)
        if (vi[i] == ki)
            return i;
    return l;
type_guid:
    vg = as_vector_guid(vector);
    kg = key->guid;
    for (i = 0; i < l; i++)
        if (memcmp(vg + i, kg, sizeof(guid_t)) == 0)
            return i;
    return l;
type_char:
    vc = as_string(vector);
    kc = key->schar;
    for (i = 0; i < l; i++)
        if (vc[i] == kc)
            return i;
    return l;
type_list:
    vl = as_list(vector);
    kl = key;
    for (i = 0; i < l; i++)
        if (rf_object_eq(&vl[i], kl))
            return i;
    return l;
}

rf_object_t vector_get(rf_object_t *vector, i64_t index)
{
    i64_t l;
    type_t type = vector->type;

    static null_t *types_table[] = {&&type_null, &&type_bool, &&type_i64, &&type_f64, &&type_symbol,
                                    &&type_timestamp, &&type_guid, &&type_char, &&type_list};

    l = vector->adt->len;

    goto *types_table[(i32_t)type];

type_null:
    return null();
type_bool:
    if (index < l)
        return bool(as_vector_bool(vector)[index]);
    return bool(false);
type_i64:
    if (index < l)
        return i64(as_vector_i64(vector)[index]);
    return i64(NULL_I64);
type_f64:
    if (index < l)
        return f64(as_vector_f64(vector)[index]);
    return f64(NULL_F64);
type_symbol:
    if (index < l)
        return symboli64(as_vector_i64(vector)[index]);
    return symboli64(NULL_I64);
type_timestamp:
    if (index < l)
        return timestamp(as_vector_i64(vector)[index]);
    return timestamp(NULL_I64);
type_guid:
    if (index < l)
        return guid(as_vector_guid(vector)[index].data);
    return guid(NULL);
type_char:
    if (index < l)
        return schar(as_string(vector)[index]);
    return schar(0);
type_list:
    if (index < l)
        return rf_object_clone(&as_list(vector)[index]);
    return null();
}

null_t vector_set(rf_object_t *vector, i64_t index, rf_object_t value)
{
    guid_t *g;
    type_t type = vector->type;

    static null_t *types_table[] = {&&type_null, &&type_bool, &&type_i64, &&type_f64, &&type_symbol,
                                    &&type_timestamp, &&type_guid, &&type_char, &&type_list};

    // if (type > TYPE_LIST)
    // panic("vector_set: non-settable type");

    if (index < 0 || index >= vector->adt->len)
        return;

    goto *types_table[(i32_t)type];

type_null:
    return;
type_bool:
    as_vector_bool(vector)[index] = value.bool;
    return;
type_i64:
    as_vector_i64(vector)[index] = value.i64;
    return;
type_f64:
    as_vector_f64(vector)[index] = value.f64;
    return;
type_symbol:
    as_vector_i64(vector)[index] = value.i64;
    return;
type_timestamp:
    as_vector_i64(vector)[index] = value.i64;
    return;
type_guid:
    g = as_vector_guid(vector);
    memcpy(g + index, value.guid, sizeof(guid_t));
    return;
type_char:
    as_string(vector)[index] = value.schar;
    return;
type_list:
    as_list(vector)[index] = value;
    return;
}

/*
 * Try to flatten list in a vector if all elements are of the same type
 */
rf_object_t list_flatten(rf_object_t *list)
{
    i64_t len = list->adt->len;
    type_t type;
    rf_object_t vec;

    if (len == 0)
        return rf_object_clone(list);

    type = as_list(list)[0].type;

    // Only scalar types inside a list can be flattened
    if (type > 0)
        return rf_object_clone(list);

    switch (type)
    {
    case -TYPE_I64:
        flatten(list, vec, i64);
        break;
    case -TYPE_F64:
        flatten(list, vec, f64);
        break;
    case -TYPE_SYMBOL:
        flatten(list, vec, i64);
        if (vec.type == TYPE_I64)
            vec.type = TYPE_SYMBOL;
        break;
    default:
        return rf_object_clone(list);
    }

    return vec;
}

rf_object_t vector_filter(rf_object_t *x, bool_t mask[], i64_t len)
{
    i64_t i, j = 0, l, ol;
    type_t type = x->type;
    rf_object_t vec;

    static null_t *types_table[] = {&&type_null, &&type_bool, &&type_i64, &&type_f64, &&type_symbol,
                                    &&type_timestamp, &&type_guid, &&type_char, &&type_list};

    l = x->adt->len;
    ol = (len == NULL_I64) ? x->adt->len : len;

    goto *types_table[(i32_t)type];

type_null:
    return null();
type_bool:
    vec = vector_bool(ol);
    for (i = 0; (j < ol && i < l); i++)
        if (mask[i])
            as_vector_bool(&vec)[j++] = as_vector_bool(x)[i];
    if (len == NULL_I64)
        vector_shrink(&vec, j);
    return vec;
type_i64:
    vec = vector_i64(ol);
    for (i = 0; (j < ol && i < l); i++)
        if (mask[i])
            as_vector_i64(&vec)[j++] = as_vector_i64(x)[i];
    if (len == NULL_I64)
        vector_shrink(&vec, j);
    return vec;
type_f64:
    vec = vector_f64(ol);
    for (i = 0; (j < ol && i < l); i++)
        if (mask[i])
            as_vector_f64(&vec)[j++] = as_vector_f64(x)[i];
    if (len == NULL_I64)
        vector_shrink(&vec, j);
    return vec;
type_symbol:
    vec = vector_symbol(ol);
    for (i = 0; (j < ol && i < l); i++)
        if (mask[i])
            as_vector_i64(&vec)[j++] = as_vector_i64(x)[i];
    if (len == NULL_I64)
        vector_shrink(&vec, j);
    return vec;
type_timestamp:
    vec = vector_timestamp(ol);
    for (i = 0; (j < ol && i < l); i++)
        if (mask[i])
            as_vector_i64(&vec)[j++] = as_vector_i64(x)[i];
    if (len == NULL_I64)
        vector_shrink(&vec, j);
    return vec;
type_guid:
    vec = vector_guid(ol);
    for (i = 0; (j < ol && i < l); i++)
        if (mask[i])
            as_vector_guid(&vec)[j++] = as_vector_guid(x)[i];
    if (len == NULL_I64)
        return vec;
type_char:
    vec = string(ol);
    for (i = 0; (j < ol && i < l); i++)
        if (mask[i])
            as_string(&vec)[j++] = as_string(x)[i];
    if (len == NULL_I64)
        vector_shrink(&vec, j);
    return vec;
type_list:
    vec = list(ol);
    for (i = 0; (j < ol && i < l); i++)
        if (mask[i])
            as_list(&vec)[j++] = rf_object_clone(&as_list(x)[i]);
    if (len == NULL_I64)
        vector_shrink(&vec, j);
    return vec;
}

null_t vector_free(rf_object_t *vector)
{
    rf_free(vector->adt);
}