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
#include <errno.h>
#include "runtime.h"
#include "unary.h"
#include "binary.h"
#include "vary.h"
#include "util.h"
#include "format.h"
#include "hash.h"
#include "fs.h"
#include "serde.h"
#include "ops.h"
#include "sock.h"
#include "items.h"
#include "compose.h"

obj_t call_binary(binary_f f, obj_t x, obj_t y)
{
    u64_t i, l;
    obj_t res, item, a, b;
    type_t xt, yt;

    xt = x->type;
    yt = y->type;

    if (xt == TYPE_LISTMAP && yt == TYPE_LISTMAP)
    {
        l = ops_count(x);

        if (l != ops_count(y))
            return error(ERR_LENGTH, "binary: vectors must be of the same length");

        a = ray_vecmap(as_list(x)[0], as_list(as_list(x)[1])[0]);
        b = ray_vecmap(as_list(y)[0], as_list(as_list(y)[1])[0]);

        item = call_binary(f, a, b);

        if (is_error(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : vector(TYPE_LIST, l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            a = ray_vecmap(as_list(x)[0], as_list(as_list(x)[1])[i]);
            b = ray_vecmap(as_list(y)[0], as_list(as_list(y)[1])[i]);
            item = call_binary(f, a, b);

            if (is_error(item))
            {
                res->len = i;
                drop(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;
    }
    else if (xt == TYPE_LISTMAP)
    {
        l = ops_count(x);
        a = ray_vecmap(as_list(x)[0], as_list(as_list(x)[1])[0]);

        item = call_binary(f, a, y);

        if (is_error(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : vector(TYPE_LIST, l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            a = ray_vecmap(as_list(x)[0], as_list(as_list(x)[1])[i]);
            item = call_binary(f, a, y);

            if (is_error(item))
            {
                res->len = i;
                drop(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;
    }
    else if (yt == TYPE_LISTMAP)
    {
        l = ops_count(y);
        b = ray_vecmap(as_list(y)[0], as_list(as_list(y)[1])[0]);

        item = call_binary(f, x, b);

        if (is_error(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : vector(TYPE_LIST, l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            b = ray_vecmap(as_list(y)[0], as_list(as_list(y)[1])[i]);
            item = call_binary(f, x, b);

            if (is_error(item))
            {
                res->len = i;
                drop(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;
    }

    return f(x, y);
}

obj_t ray_call_binary_left_atomic(binary_f f, obj_t x, obj_t y)
{
    u64_t i, l;
    obj_t res, item, a;

    switch (x->type)
    {
    case TYPE_LIST:
        l = ops_count(x);
        a = as_list(x)[0];
        item = ray_call_binary_left_atomic(f, a, y);

        if (is_error(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : vector(TYPE_LIST, l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            a = as_list(x)[i];
            item = ray_call_binary_left_atomic(f, a, y);

            if (is_error(item))
            {
                res->len = i;
                drop(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;

    case TYPE_ANYMAP:
        l = ops_count(x);
        a = at_idx(x, 0);
        item = ray_call_binary_left_atomic(f, a, y);
        drop(a);

        if (item->type == TYPE_ERROR)
            return item;

        res = item->type < 0 ? vector(item->type, l) : vector(TYPE_LIST, l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            a = at_idx(x, i);
            item = ray_call_binary_left_atomic(f, a, y);
            drop(a);

            if (is_error(item))
            {
                res->len = i;
                drop(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;

    case TYPE_LISTMAP:
        l = ops_count(x);
        a = ray_vecmap(as_list(x)[0], as_list(as_list(x)[1])[0]);
        item = ray_call_binary_left_atomic(f, a, y);

        if (is_error(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : vector(TYPE_LIST, l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            a = ray_vecmap(as_list(x)[0], as_list(as_list(x)[1])[i]);
            item = ray_call_binary_left_atomic(f, a, y);

            if (is_error(item))
            {
                res->len = i;
                drop(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;

    default:
        return f(x, y);
    }
}

obj_t ray_call_binary_right_atomic(binary_f f, obj_t x, obj_t y)
{
    u64_t i, l;
    obj_t res, item, b;

    switch (y->type)
    {
    case TYPE_LIST:
        l = ops_count(y);
        b = as_list(y)[0];
        item = ray_call_binary_right_atomic(f, x, b);

        if (is_error(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : vector(TYPE_LIST, l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            b = as_list(y)[i];
            item = ray_call_binary_right_atomic(f, x, b);

            if (is_error(item))
            {
                res->len = i;
                drop(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;

    case TYPE_ANYMAP:
        l = ops_count(y);
        b = at_idx(y, 0);
        item = ray_call_binary_right_atomic(f, x, b);
        drop(b);

        if (is_error(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : vector(TYPE_LIST, l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            b = at_idx(y, i);
            item = ray_call_binary_right_atomic(f, x, b);
            drop(b);

            if (item->type == TYPE_ERROR)
            {
                res->len = i;
                drop(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;

    case TYPE_LISTMAP:
        l = ops_count(y);
        b = ray_vecmap(as_list(y)[0], as_list(as_list(y)[1])[0]);
        item = ray_call_binary_right_atomic(f, x, b);

        if (is_error(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : vector(TYPE_LIST, l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            b = ray_vecmap(as_list(y)[0], as_list(as_list(y)[1])[i]);
            item = ray_call_binary_right_atomic(f, x, b);

            if (is_error(item))
            {
                res->len = i;
                drop(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;

    default:
        return call_binary(f, x, y);
    }
}

// Atomic binary functions (iterates through list of arguments down to atoms)
obj_t ray_call_binary_atomic(binary_f f, obj_t x, obj_t y)
{
    u64_t i, l;
    obj_t res, item, a, b;
    type_t xt, yt;

    if (!x || !y)
        throw(ERR_TYPE, "binary: null argument");

    xt = x->type;
    yt = y->type;
    if (((xt == TYPE_LIST || xt == TYPE_ANYMAP || xt == TYPE_LISTMAP) && is_vector(y)) ||
        ((yt == TYPE_LIST || yt == TYPE_ANYMAP || yt == TYPE_LISTMAP) && is_vector(x)))
    {
        l = ops_count(x);

        if (l != ops_count(y))
            return error(ERR_LENGTH, "binary: vectors must be of the same length");

        a = xt == TYPE_LIST ? as_list(x)[0] : at_idx(x, 0);
        b = yt == TYPE_LIST ? as_list(y)[0] : at_idx(y, 0);
        item = ray_call_binary_atomic(f, a, b);

        if (xt != TYPE_LIST)
            drop(a);
        if (yt != TYPE_LIST)
            drop(b);

        if (is_error(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : vector(TYPE_LIST, l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            a = xt == TYPE_LIST ? as_list(x)[i] : at_idx(x, i);
            b = yt == TYPE_LIST ? as_list(y)[i] : at_idx(y, i);
            item = ray_call_binary_atomic(f, a, b);

            if (xt != TYPE_LIST)
                drop(a);
            if (yt != TYPE_LIST)
                drop(b);

            if (is_error(item))
            {
                res->len = i;
                drop(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;
    }
    else if (xt == TYPE_LIST || xt == TYPE_ANYMAP || xt == TYPE_LISTMAP)
    {
        l = ops_count(x);
        a = xt == TYPE_LIST ? as_list(x)[0] : at_idx(x, 0);
        item = ray_call_binary_atomic(f, a, y);
        if (xt != TYPE_LIST)
            drop(a);

        if (is_error(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : vector(TYPE_LIST, l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            a = xt == TYPE_LIST ? as_list(x)[i] : at_idx(x, i);
            item = ray_call_binary_atomic(f, a, y);
            if (xt != TYPE_LIST)
                drop(a);

            if (is_error(item))
            {
                res->len = i;
                drop(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;
    }
    else if (yt == TYPE_LIST || yt == TYPE_ANYMAP || yt == TYPE_LISTMAP)
    {
        l = ops_count(y);
        b = yt == TYPE_LIST ? as_list(y)[0] : at_idx(y, 0);
        item = ray_call_binary_atomic(f, x, b);
        if (yt != TYPE_LIST)
            drop(b);

        if (is_error(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : vector(TYPE_LIST, l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            b = yt == TYPE_LIST ? as_list(y)[i] : at_idx(y, i);
            item = ray_call_binary_atomic(f, x, b);
            if (yt != TYPE_LIST)
                drop(b);

            if (is_error(item))
            {
                res->len = i;
                drop(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;
    }

    return call_binary(f, x, y);
}

obj_t ray_call_binary(u8_t attrs, binary_f f, obj_t x, obj_t y)
{
    switch (attrs)
    {
    case FN_ATOMIC:
        return ray_call_binary_atomic(f, x, y);
    case FN_LEFT_ATOMIC:
        return ray_call_binary_left_atomic(f, x, y);
    case FN_RIGHT_ATOMIC:
        return ray_call_binary_right_atomic(f, x, y);
    default:
        return call_binary(f, x, y);
    }
}

obj_t distinct_syms(obj_t *x, u64_t n)
{
    i64_t p;
    u64_t i, j, h, l;
    obj_t vec, set, a;

    if (n == 0 || (*x)->len == 0)
        return vector_symbol(0);

    l = (*x)->len;

    set = ht_tab(l, -1);

    for (i = 0, h = 0; i < n; i++)
    {
        a = *(x + i);
        for (j = 0; j < l; j++)
        {
            p = ht_tab_next(&set, as_symbol(a)[j]);
            if (as_symbol(as_list(set)[0])[p] == NULL_I64)
            {
                as_symbol(as_list(set)[0])[p] = as_symbol(a)[j];
                h++;
            }
        }
    }

    vec = vector_symbol(h);
    l = as_list(set)[0]->len;

    for (i = 0, j = 0; i < l; i++)
    {
        if (as_symbol(as_list(set)[0])[i] != NULL_I64)
            as_symbol(vec)[j++] = as_symbol(as_list(set)[0])[i];
    }

    vec->attrs |= ATTR_DISTINCT;

    drop(set);

    return vec;
}

obj_t ray_set(obj_t x, obj_t y)
{
    obj_t res, col, s, p, k, v, e, cols, sym, buf;
    i64_t fd, c = 0;
    u64_t i, l, sz, size;
    u8_t *b, mmod;

    switch (x->type)
    {
    case -TYPE_SYMBOL:
        res = set_obj(&runtime_get()->env.variables, x, clone(y));

        if (res->type == TYPE_ERROR)
            return res;

        return clone(y);

    case TYPE_CHAR:
        switch (y->type)
        {
        case TYPE_SYMBOL:
            fd = fs_fopen(as_string(x), ATTR_WRONLY | ATTR_CREAT);

            if (fd == -1)
                return sys_error(ERROR_TYPE_SYS, as_string(x));

            buf = ser(y);

            if (is_error(buf))
            {
                fs_fclose(fd);
                return buf;
            }

            c = fs_fwrite(fd, (str_t)as_u8(buf), buf->len);
            fs_fclose(fd);
            drop(buf);

            if (c == -1)
                return sys_error(ERROR_TYPE_SYS, as_string(x));

            return clone(x);
        case TYPE_TABLE:
            if (as_string(x)[x->len - 1] != '/')
                throw(ERR_TYPE, "set: table path must be a directory");

            // save columns schema
            s = string_from_str(".d", 2);
            col = ray_concat(x, s);
            res = ray_set(col, as_list(y)[0]);

            drop(s);
            drop(col);

            if (is_error(res))
                return res;

            drop(res);

            l = as_list(y)[0]->len;

            cols = list(0);

            // find symbol columns
            for (i = 0, c = 0; i < l; i++)
            {
                if (as_list(as_list(y)[1])[i]->type == TYPE_SYMBOL)
                    push_obj(&cols, clone(as_list(as_list(y)[1])[i]));
            }

            sym = distinct_syms(as_list(cols), cols->len);

            if (sym->len > 0)
            {
                s = string_from_str("sym", 3);
                col = ray_concat(x, s);
                res = ray_set(col, sym);

                drop(s);
                drop(col);

                if (is_error(res))
                    return res;

                drop(res);

                s = symbol("sym");
                res = ray_set(s, sym);

                drop(s);

                if (is_error(res))
                    return res;

                drop(res);
            }

            drop(cols);
            drop(sym);
            // --

            // save columns data
            for (i = 0; i < l; i++)
            {
                v = at_idx(as_list(y)[1], i);

                // symbol column need to be converted to enum
                if (v->type == TYPE_SYMBOL)
                {
                    s = symbol("sym");
                    e = ray_enum(s, v);

                    drop(s);
                    drop(v);

                    if (is_error(e))
                        return e;

                    v = e;
                }

                p = at_idx(as_list(y)[0], i);
                s = as(TYPE_CHAR, p);
                col = ray_concat(x, s);
                res = ray_set(col, v);

                drop(p);
                drop(v);
                drop(s);
                drop(col);

                if (is_error(res))
                    return res;

                drop(res);
            }

            return clone(x);

        case TYPE_ENUM:
            fd = fs_fopen(as_string(x), ATTR_WRONLY | ATTR_CREAT);

            if (fd == -1)
                return sys_error(ERROR_TYPE_SYS, as_string(x));

            if (is_external_compound(y))
            {
                size = PAGE_SIZE + sizeof(struct obj_t) + y->len * sizeof(i64_t);

                c = fs_fwrite(fd, (str_t)y - PAGE_SIZE, size);
                fs_fclose(fd);

                if (c == -1)
                    return sys_error(ERROR_TYPE_SYS, as_string(x));

                return clone(x);
            }

            p = (obj_t)heap_alloc(PAGE_SIZE);

            memset((str_t)p, 0, PAGE_SIZE);

            strcpy(as_string(p), symtostr(as_list(y)[0]->i64));

            p->mmod = MMOD_EXTERNAL_COMPOUND;

            c = fs_fwrite(fd, (str_t)p, PAGE_SIZE);
            if (c == -1)
            {
                heap_free(p);
                fs_fclose(fd);
                return sys_error(ERROR_TYPE_SYS, as_string(x));
            }

            p->type = TYPE_ENUM;
            p->len = as_list(y)[1]->len;

            c = fs_fwrite(fd, (str_t)p, sizeof(struct obj_t));
            if (c == -1)
            {
                heap_free(p);
                fs_fclose(fd);
                return sys_error(ERROR_TYPE_SYS, as_string(x));
            }

            size = as_list(y)[1]->len * sizeof(i64_t);

            c = fs_fwrite(fd, as_string(as_list(y)[1]), size);
            heap_free(p);
            fs_fclose(fd);

            if (c == -1)
                return sys_error(ERROR_TYPE_SYS, as_string(x));

            return clone(x);

        case TYPE_LIST:
            s = string_from_str("#", 1);
            col = ray_concat(x, s);
            drop(s);

            l = y->len;
            size = size_obj(y) - sizeof(type_t) - sizeof(u64_t);
            buf = vector_u8(size);
            b = as_u8(buf);
            k = vector_i64(l);

            for (i = 0, size = 0; i < l; i++)
            {
                v = as_list(y)[i];
                sz = save_obj(b, l, v);

                if (sz == 0)
                {
                    drop(col);
                    drop(buf);
                    drop(k);

                    throw(ERR_NOT_SUPPORTED, "set: unsupported type: %d", y->type);
                }

                as_i64(k)[i] = size;

                size += sz;
                b += sz;
            }

            res = ray_set(col, buf);

            drop(col);
            drop(buf);

            if (is_error(res))
                return res;

            drop(res);

            // save anymap
            fd = fs_fopen(as_string(x), ATTR_WRONLY | ATTR_CREAT);

            if (fd == -1)
                return sys_error(ERROR_TYPE_SYS, as_string(x));

            p = (obj_t)heap_alloc(PAGE_SIZE);

            memset((str_t)p, 0, PAGE_SIZE);

            p->mmod = MMOD_EXTERNAL_COMPOUND;

            c = fs_fwrite(fd, (str_t)p, PAGE_SIZE);
            if (c == -1)
            {
                heap_free(p);
                fs_fclose(fd);
                return sys_error(ERROR_TYPE_SYS, as_string(x));
            }

            p->type = TYPE_ANYMAP;
            p->len = y->len;

            c = fs_fwrite(fd, (str_t)p, sizeof(struct obj_t));
            if (c == -1)
            {
                heap_free(p);
                fs_fclose(fd);
                return sys_error(ERROR_TYPE_SYS, as_string(x));
            }

            size = k->len * sizeof(i64_t);

            c = fs_fwrite(fd, as_string(k), size);
            heap_free(p);
            heap_free(k);
            fs_fclose(fd);

            if (c == -1)
                return sys_error(ERROR_TYPE_SYS, as_string(x));

            // --

            return clone(x);

        default:
            if (is_vector(y))
            {
                fd = fs_fopen(as_string(x), ATTR_WRONLY | ATTR_CREAT);

                if (fd == -1)
                    return sys_error(ERROR_TYPE_SYS, as_string(x));

                size = size_of(y);

                c = fs_fwrite(fd, (str_t)y, size);

                if (c == -1)
                {
                    e = sys_error(ERROR_TYPE_SYS, as_string(x));
                    fs_fclose(fd);
                    return e;
                }

                // write mmod
                lseek(fd, 0, SEEK_SET);
                mmod = MMOD_EXTERNAL_SIMPLE;
                c = fs_fwrite(fd, (str_t)&mmod, sizeof(u8_t));
                if (c == -1)
                {
                    e = sys_error(ERROR_TYPE_SYS, as_string(x));
                    fs_fclose(fd);
                    return e;
                }
                fs_fclose(fd);

                return clone(x);
            }

            throw(ERR_TYPE, "set: unsupported types: %d %d", typename(x->type), typename(y->type));
        }

    default:
        throw(ERR_TYPE, "set: unsupported types: %d %d", typename(x->type), typename(y->type));
    }
}
