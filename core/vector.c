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
        return sizeof(obj_t);
    default:
        panic_type("size of val unknown type", type);
    }
}

/*
 * Creates new vector of type
 */
obj_t vector(type_t type, i64_t len)
{
    i64_t size = capacity(len * size_of_val(type));
    obj_t vec = heap_malloc(sizeof(struct obj_t));

    vec->type = type;
    vec->rc = 1;
    vec->len = len;
    vec->ptr = heap_malloc(size);

    return vec;
}

obj_t _push(obj_t vec, obj_t value)
{
    switch (vec->type)
    {
    case TYPE_BOOL:
        push(vec, bool_t, value->bool);
        return null();
    case TYPE_I64:
        push(vec, i64_t, value->i64);
        return null();
    case TYPE_F64:
        push(vec, f64_t, value->f64);
        return null();
    case TYPE_SYMBOL:
        push(vec, i64_t, value->i64);
        return null();
    case TYPE_TIMESTAMP:
        push(vec, i64_t, value->i64);
        return null();
    // case TYPE_GUID:
    //     push(vec, guid_t, *value->guid);
    //     return null();
    case TYPE_CHAR:
        push(vec, char_t, value->schar);
        return null();
    // case TYPE_LIST:
    //     push(vec, obj_t, clone(value));
    //     return null();
    default:
        panic("vector push: can not push to a unknown type");
    }
}

obj_t list_push(obj_t vec, obj_t value)
{
    debug_assert(is_vector(vec));

    if (vec->type != -value->type && vec->type != TYPE_LIST)
        panic("vector push: can not push to a non-list");

    return _push(vec, value);
}

obj_t vector_push(obj_t vec, obj_t value)
{
    u64_t i, l;
    obj_t lst = NULL;

    debug_assert(is_vector(vec));

    l = vec->len;

    if (l == 0)
    {
        if (is_scalar(value))
            vec->type = -value->type;
        else
            vec->type = TYPE_LIST;
    }
    else
    {
        // change vector type to a list
        if (vec->type != -value->type && vec->type != TYPE_LIST)
        {
            lst = list(l + 1);
            for (i = 0; i < l; i++)
                as_list(lst)[i] = vector_get(vec, i);

            as_list(lst)[l] = value;

            drop(vec);

            vec = lst;
            return null();
        }
    }

    return _push(vec, value);
}

obj_t vector_pop(obj_t vec)
{
    guid_t *g;

    if (!is_vector(vec) || vec->len == 0)
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
        return guid(g[--vec->len].data);
    case TYPE_CHAR:
        return schar(pop(vec, char_t));
    case TYPE_LIST:
        if (vec->ptr == NULL)
            return null();
        return pop(vec, obj_t);
    default:
        panic_type("vector pop: unknown type", vec->type);
    }
}

nil_t vector_reserve(obj_t vec, u32_t len)
{
    switch (vec->type)
    {
    case TYPE_BOOL:
        reserve(vec, bool_t, len);
        return;
    case TYPE_I64:
        reserve(vec, i64_t, len);
        return;
    case TYPE_F64:
        reserve(vec, f64_t, len);
        return;
    case TYPE_SYMBOL:
        reserve(vec, i64_t, len);
        return;
    case TYPE_TIMESTAMP:
        reserve(vec, i64_t, len);
        return;
    case TYPE_GUID:
        reserve(vec, guid_t, len);
        return;
    case TYPE_CHAR:
        reserve(vec, char_t, len);
        return;
    case TYPE_LIST:
        if (vec->ptr == NULL)
            panic_type("vector reserve: can not reserve a null", vec->type);
        reserve(vec, obj_t, len);
        return;
    default:
        panic_type("vector reserve: unknown type", vec->type);
    }
}

nil_t vector_grow(obj_t vec, u32_t len)
{
    debug_assert(is_vector(vec));

    // calculate size of vector with new length
    i64_t new_size = capacity(len * size_of_val(vec->type));

    heap_realloc(vec->ptr, new_size);
    vec->len = len;
}

nil_t vector_shrink(obj_t vec, u32_t len)
{
    debug_assert(is_vector(vec));

    if (vec->len == len)
        return;

    // calculate size of vector with new length
    i64_t new_size = capacity(len * size_of_val(vec->type));

    heap_realloc(vec->ptr, new_size);
    vec->len = len;
}

i64_t vector_find(obj_t vec, obj_t key)
{
    return 0;
    // char_t kc, *vc;
    // bool_t kb, *vb;
    // i64_t ki, *vi;
    // f64_t kf, *vf;
    // obj_t*kl, *vl;
    // i64_t i, l;
    // guid_t *kg, *vg;

    // debug_assert(is_vector(vec));

    // l = vec->len;

    // if (key->type != -vec->type && vec->type != TYPE_LIST)
    //     return l;

    // switch (vec->type)
    // {
    // case TYPE_BOOL:
    //     vb = as_vector_bool(vec);
    //     kb = key->bool;
    //     for (i = 0; i < l; i++)
    //         if (vb[i] == kb)
    //             return i;
    //     return l;
    // case TYPE_I64:
    //     vi = as_vector_i64(vec);
    //     ki = key->i64;
    //     for (i = 0; i < l; i++)
    //         if (vi[i] == ki)
    //             return i;
    //     return l;
    // case TYPE_F64:
    //     vf = as_vector_f64(vec);
    //     kf = key->f64;
    //     for (i = 0; i < l; i++)
    //         if (vf[i] == kf)
    //             return i;
    //     return l;
    // case TYPE_SYMBOL:
    //     vi = as_vector_symbol(vec);
    //     ki = key->i64;
    //     for (i = 0; i < l; i++)
    //         if (vi[i] == ki)
    //             return i;
    //     return l;
    // case TYPE_TIMESTAMP:
    //     vi = as_vector_timestamp(vec);
    //     ki = key->i64;
    //     for (i = 0; i < l; i++)
    //         if (vi[i] == ki)
    //             return i;
    //     return l;
    // case TYPE_GUID:
    //     vg = as_vector_guid(vec);
    //     kg = key->guid;
    //     for (i = 0; i < l; i++)
    //         if (memcmp(vg + i, kg, sizeof(guid_t)) == 0)
    //             return i;
    //     return l;
    // case TYPE_CHAR:
    //     vc = as_string(vec);
    //     kc = key->schar;
    //     for (i = 0; i < l; i++)
    //         if (vc[i] == kc)
    //             return i;
    //     return l;
    // case TYPE_LIST:
    //     vl = as_list(vec);
    //     kl = key;
    //     for (i = 0; i < l; i++)
    //         if (obj_t_eq(&vl[i], kl))
    //             return i;
    //     return l;
    // default:
    //     return l;
    // }
}

obj_t vector_get(obj_t vec, i64_t index)
{
    i64_t l;

    debug_assert(is_vector(vec));

    // l = vec->len;

    // switch (vec->type)
    // {
    // case TYPE_BOOL:
    //     if (index < l)
    //         return bool(as_vector_bool(vec)[index]);
    //     return bool(false);
    // case TYPE_I64:
    //     if (index < l)
    //         return i64(as_vector_i64(vec)[index]);
    //     return i64(NULL_vector_i64);
    // case TYPE_F64:
    //     if (index < l)
    //         return f64(as_vector_f64(vec)[index]);
    //     return f64(NULL_vector_f64);
    // case TYPE_SYMBOL:
    //     if (index < l)
    //         return symboli64(as_vector_i64(vec)[index]);
    //     return symboli64(NULL_vector_i64);
    // case TYPE_TIMESTAMP:
    //     if (index < l)
    //         return timestamp(as_vector_i64(vec)[index]);
    //     return timestamp(NULL_vector_i64);
    // case TYPE_GUID:
    //     if (index < l)
    //         return guid(as_vector_guid(vec)[index].data);
    //     return guid(NULL);
    // case TYPE_CHAR:
    //     if (index < l)
    //         return schar(as_string(vec)[index]);
    //     return schar(0);
    // case TYPE_LIST:
    //     if (index < l)
    //         return clone(&as_list(vec)[index]);
    //     return null();
    // default:
    //     return null();
    // }

    return null();
}

obj_t vector_set(obj_t vec, i64_t index, obj_t value)
{
    guid_t *g;
    i64_t i, l;
    obj_t lst;

    debug_assert(is_vector(vec));

    // l = vec->len;

    // if (index >= l)
    //     return error(ERR_LENGTH, "vector set: index out of bounds");

    // // change vector type to a list
    // if (vec->type != -value.type && vec->type != TYPE_LIST)
    // {
    //     lst = list(l);
    //     for (i = 0; i < l; i++)
    //         as_list(&lst)[i] = vector_get(vec, i);

    //     as_list(&lst)[index] = value;

    //     drop(vec);

    //     *vec = lst;

    //     return null();
    // }

    // switch (vec->type)
    // {
    // case TYPE_BOOL:
    //     as_vector_bool(vec)[index] = value.bool;
    //     break;
    // case TYPE_I64:
    //     as_vector_i64(vec)[index] = value.i64;
    //     break;
    // case TYPE_F64:
    //     as_vector_f64(vec)[index] = value.f64;
    //     break;
    // case TYPE_SYMBOL:
    //     as_vector_i64(vec)[index] = value.i64;
    //     break;
    // case TYPE_TIMESTAMP:
    //     as_vector_i64(vec)[index] = value.i64;
    //     break;
    // case TYPE_GUID:
    //     g = as_vector_guid(vec);
    //     memcpy(g + index, value.guid, sizeof(guid_t));
    //     break;
    // case TYPE_CHAR:
    //     as_string(vec)[index] = value.schar;
    //     break;
    // case TYPE_LIST:
    //     drop(&as_list(vec)[index]);
    //     as_list(vec)[index] = value;
    //     break;
    // default:
    //     panic_type("vector_set: unknown type", vec->type);
    // }

    return null();
}

/*
 * same as vector_set, but does not check bounds and does not free old value in case of list
 */
nil_t vector_write(obj_t vec, i64_t index, obj_t value)
{
    // guid_t *g;
    // i64_t i, l;
    // obj_tlst;

    // l = vec->len;

    // // change vector type to a list
    // if (vec->type != -value.type && vec->type != TYPE_LIST)
    // {
    //     lst = list(l);

    //     for (i = 0; i < index; i++)
    //         as_list(&lst)[i] = vector_get(vec, i);

    //     as_list(&lst)[index] = value;

    //     drop(vec);

    //     *vec = lst;

    //     return;
    // }

    // switch (vec->type)
    // {
    // case TYPE_BOOL:
    //     as_vector_bool(vec)[index] = value.bool;
    //     break;
    // case TYPE_I64:
    //     as_vector_i64(vec)[index] = value.i64;
    //     break;
    // case TYPE_F64:
    //     as_vector_f64(vec)[index] = value.f64;
    //     break;
    // case TYPE_SYMBOL:
    //     as_vector_i64(vec)[index] = value.i64;
    //     break;
    // case TYPE_TIMESTAMP:
    //     as_vector_i64(vec)[index] = value.i64;
    //     break;
    // case TYPE_GUID:
    //     g = as_vector_guid(vec);
    //     memcpy(g + index, value.guid, sizeof(guid_t));
    //     break;
    // case TYPE_CHAR:
    //     as_string(vec)[index] = value.schar;
    //     break;
    // case TYPE_LIST:
    //     as_list(vec)[index] = value;
    //     break;
    // default:
    //     panic_type("vector_set: unknown type", vec->type);
    // }
}

obj_t vector_filter(obj_t vec, bool_t mask[], i64_t len)
{
    i64_t i, j = 0, l, ol;
    obj_t res = NULL;

    debug_assert(is_vector(vec));

    return res;

    // l = vec->len;
    // ol = (len == NULL_vector_i64) ? (i64_t)vec->len : len;

    // switch (vec->type)
    // {
    // case TYPE_BOOL:
    //     res = vector_bool(ol);
    //     for (i = 0; (j < ol && i < l); i++)
    //         if (mask[i])
    //             as_vector_bool(&res)[j++] = as_vector_bool(vec)[i];
    //     if (len == NULL_vector_i64)
    //         vector_shrink(&res, j);
    //     return res;
    // case TYPE_I64:
    //     res = vector_i64(ol);
    //     for (i = 0; (j < ol && i < l); i++)
    //         if (mask[i])
    //             as_vector_i64(&res)[j++] = as_vector_i64(vec)[i];
    //     if (len == NULL_vector_i64)
    //         vector_shrink(&res, j);
    //     return res;
    // case TYPE_F64:
    //     res = vector_f64(ol);
    //     for (i = 0; (j < ol && i < l); i++)
    //         if (mask[i])
    //             as_vector_f64(&res)[j++] = as_vector_f64(vec)[i];
    //     if (len == NULL_vector_i64)
    //         vector_shrink(&res, j);
    //     return res;
    // case TYPE_SYMBOL:
    //     res = vector_symbol(ol);
    //     for (i = 0; (j < ol && i < l); i++)
    //         if (mask[i])
    //             as_vector_i64(&res)[j++] = as_vector_i64(vec)[i];
    //     if (len == NULL_vector_i64)
    //         vector_shrink(&res, j);
    //     return res;
    // case TYPE_TIMESTAMP:
    //     res = vector_timestamp(ol);
    //     for (i = 0; (j < ol && i < l); i++)
    //         if (mask[i])
    //             as_vector_i64(&res)[j++] = as_vector_i64(vec)[i];
    //     if (len == NULL_vector_i64)
    //         vector_shrink(&res, j);
    //     return res;
    // case TYPE_GUID:
    //     res = vector_guid(ol);
    //     for (i = 0; (j < ol && i < l); i++)
    //         if (mask[i])
    //             as_vector_guid(&res)[j++] = as_vector_guid(vec)[i];
    //     if (len == NULL_vector_i64)
    //         vector_shrink(&res, j);
    //     return res;
    // case TYPE_CHAR:
    //     res = string(ol);
    //     for (i = 0; (j < ol && i < l); i++)
    //         if (mask[i])
    //             as_string(&res)[j++] = as_string(vec)[i];
    //     if (len == NULL_vector_i64)
    //         vector_shrink(&res, j);
    //     return res;
    // case TYPE_LIST:
    //     res = list(ol);
    //     for (i = 0; (j < ol && i < l); i++)
    //         if (mask[i])
    //             as_list(&res)[j++] = clone(&as_list(vec)[i]);
    //     if (len == NULL_vector_i64)
    //         vector_shrink(&res, j);
    //     return res;
    // default:
    //     panic_type("vector_filter: unknown type", vec->type);
    // }
}

nil_t vector_clear(obj_t vec)
{
    // if (vec->type == TYPE_LIST)
    // {
    //     i64_t i, l = vec->len;
    //     obj_t*list = as_list(vec);

    //     for (i = 0; i < l; i++)
    //         drop(&list[i]);
    // }

    // vector_shrink(vec, 0);
}

obj_t rf_list(obj_t x, u32_t n)
{
    obj_t l = list(n);
    u32_t i;

    for (i = 0; i < n; i++)
        as_list(l)[i] = clone(x + i);

    return l;
}

obj_t rf_enlist(obj_t x, u32_t n)
{
    obj_t l = list(0);
    u32_t i;

    for (i = 0; i < n; i++)
    {
        obj_t item = x + i;
        vector_push(l, clone(item));
    }

    return l;
}

obj_t join_raw(obj_t *obj, nil_t *val)
{
    return *obj;
}

obj_t join_obj(obj_t *obj, obj_t val)
{
    return null();
}

obj_t join_sym(obj_t *obj, str_t str)
{
    return null();
}

obj_t join_lst(obj_t *obj, obj_t val)
{
    return null();
}