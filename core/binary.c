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
#include "error.h"

obj_p binary_call_left_atomic(binary_f f, obj_p x, obj_p y)
{
    u64_t i, l;
    obj_p res, item, a;

    switch (x->type)
    {
    case TYPE_LIST:
        l = ops_count(x);
        a = as_list(x)[0];
        item = binary_call_left_atomic(f, a, y);

        if (is_error(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : list(l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            a = as_list(x)[i];
            item = binary_call_left_atomic(f, a, y);

            if (is_error(item))
            {
                res->len = i;
                drop_obj(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;

    case TYPE_ANYMAP:
        l = ops_count(x);
        a = at_idx(x, 0);
        item = binary_call_left_atomic(f, a, y);
        drop_obj(a);

        if (item->type == TYPE_ERROR)
            return item;

        res = item->type < 0 ? vector(item->type, l) : list(l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            a = at_idx(x, i);
            item = binary_call_left_atomic(f, a, y);
            drop_obj(a);

            if (is_error(item))
            {
                res->len = i;
                drop_obj(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;

    default:
        return f(x, y);
    }
}

obj_p binary_call_right_atomic(binary_f f, obj_p x, obj_p y)
{
    u64_t i, l;
    obj_p res, item, b;

    switch (y->type)
    {
    case TYPE_LIST:
        l = ops_count(y);
        b = as_list(y)[0];
        item = binary_call_right_atomic(f, x, b);

        if (is_error(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : list(l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            b = as_list(y)[i];
            item = binary_call_right_atomic(f, x, b);

            if (is_error(item))
            {
                res->len = i;
                drop_obj(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;

    case TYPE_ANYMAP:
        l = ops_count(y);
        b = at_idx(y, 0);
        item = binary_call_right_atomic(f, x, b);
        drop_obj(b);

        if (is_error(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : list(l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            b = at_idx(y, i);
            item = binary_call_right_atomic(f, x, b);
            drop_obj(b);

            if (item->type == TYPE_ERROR)
            {
                res->len = i;
                drop_obj(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;

    default:
        return f(x, y);
    }
}

// Atomic binary functions (iterates through list of arguments down to atoms)
obj_p binary_call_atomic(binary_f f, obj_p x, obj_p y)
{
    u64_t i, l;
    obj_p res, item, a, b;
    i8_t xt, yt;

    if (!x || !y)
        throw(ERR_TYPE, "binary: null argument");

    xt = x->type;
    yt = y->type;
    if (((xt == TYPE_LIST || xt == TYPE_ANYMAP) && is_vector(y)) ||
        ((yt == TYPE_LIST || yt == TYPE_ANYMAP) && is_vector(x)))
    {
        l = ops_count(x);

        if (l != ops_count(y))
            return error_str(ERR_LENGTH, "binary: vectors must be of the same length");

        a = xt == TYPE_LIST ? as_list(x)[0] : at_idx(x, 0);
        b = yt == TYPE_LIST ? as_list(y)[0] : at_idx(y, 0);
        item = binary_call_atomic(f, a, b);

        if (xt != TYPE_LIST)
            drop_obj(a);
        if (yt != TYPE_LIST)
            drop_obj(b);

        if (is_error(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : list(l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            a = xt == TYPE_LIST ? as_list(x)[i] : at_idx(x, i);
            b = yt == TYPE_LIST ? as_list(y)[i] : at_idx(y, i);
            item = binary_call_atomic(f, a, b);

            if (xt != TYPE_LIST)
                drop_obj(a);
            if (yt != TYPE_LIST)
                drop_obj(b);

            if (is_error(item))
            {
                res->len = i;
                drop_obj(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;
    }
    else if (xt == TYPE_LIST || xt == TYPE_ANYMAP)
    {
        l = ops_count(x);
        a = xt == TYPE_LIST ? as_list(x)[0] : at_idx(x, 0);
        item = binary_call_atomic(f, a, y);
        if (xt != TYPE_LIST)
            drop_obj(a);

        if (is_error(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : list(l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            a = xt == TYPE_LIST ? as_list(x)[i] : at_idx(x, i);
            item = binary_call_atomic(f, a, y);
            if (xt != TYPE_LIST)
                drop_obj(a);

            if (is_error(item))
            {
                res->len = i;
                drop_obj(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;
    }
    else if (yt == TYPE_LIST || yt == TYPE_ANYMAP)
    {
        l = ops_count(y);
        b = yt == TYPE_LIST ? as_list(y)[0] : at_idx(y, 0);
        item = binary_call_atomic(f, x, b);
        if (yt != TYPE_LIST)
            drop_obj(b);

        if (is_error(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : list(l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            b = yt == TYPE_LIST ? as_list(y)[i] : at_idx(y, i);
            item = binary_call_atomic(f, x, b);
            if (yt != TYPE_LIST)
                drop_obj(b);

            if (is_error(item))
            {
                res->len = i;
                drop_obj(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;
    }

    return f(x, y);
}

obj_p binary_call(u8_t attrs, binary_f f, obj_p x, obj_p y)
{
    switch (attrs & FN_ATOMIC_MASK)
    {
    case FN_ATOMIC:
        return binary_call_atomic(f, x, y);
    case FN_LEFT_ATOMIC:
        return binary_call_left_atomic(f, x, y);
    case FN_RIGHT_ATOMIC:
        return binary_call_right_atomic(f, x, y);
    default:
        return f(x, y);
    }
}

obj_p distinct_syms(obj_p *x, u64_t n)
{
    i64_t p;
    u64_t i, j, h, l;
    obj_p vec, set, a;

    if (n == 0 || (*x)->len == 0)
        return vector_symbol(0);

    l = (*x)->len;

    set = ht_oa_create(l, -1);

    for (i = 0, h = 0; i < n; i++)
    {
        a = *(x + i);
        for (j = 0; j < l; j++)
        {
            p = ht_oa_tab_next(&set, as_symbol(a)[j]);
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

    drop_obj(set);

    return vec;
}

obj_p __ray_set(obj_p x, obj_p y)
{
    i64_t fd, c = 0;
    u64_t i, l, sz, size;
    u8_t *b, mmod;
    obj_p res, col, s, p, k, v, e, cols, sym, path, buf;
    c8_t objbuf[PAGE_SIZE] = {0};

    switch (x->type)
    {
    case -TYPE_SYMBOL:
        res = set_obj(&runtime_get()->env.variables, x, clone_obj(y));

        if (y && y->type == TYPE_LAMBDA)
        {
            if (is_null(as_lambda(y)->name))
                as_lambda(y)->name = clone_obj(x);
        }

        if (is_error(res))
            return res;

        return clone_obj(y);

    case TYPE_C8:
        switch (y->type)
        {
        case TYPE_SYMBOL:
            path = cstring_from_obj(x);
            fd = fs_fopen(as_string(path), ATTR_WRONLY | ATTR_CREAT);

            if (fd == -1)
            {
                res = sys_error(ERROR_TYPE_SYS, as_string(path));
                drop_obj(path);
                return res;
            }

            buf = ser_obj(y);

            if (is_error(buf))
            {
                fs_fclose(fd);
                drop_obj(path);
                return buf;
            }

            c = fs_fwrite(fd, (str_p)as_u8(buf), buf->len);
            fs_fclose(fd);
            drop_obj(buf);

            if (c == -1)
            {
                res = sys_error(ERROR_TYPE_SYS, as_string(path));
                drop_obj(path);
                return res;
            }

            drop_obj(path);

            return clone_obj(x);
        case TYPE_TABLE:
            if (x->len < 2 || as_string(x)[x->len - 1] != '/')
                throw(ERR_TYPE, "set: table path must be a directory");

            // save columns schema
            s = cstring_from_str(".d", 2);
            col = ray_concat(x, s);
            res = __ray_set(col, as_list(y)[0]);

            drop_obj(s);
            drop_obj(col);

            if (is_error(res))
                return res;

            drop_obj(res);

            l = as_list(y)[0]->len;

            cols = list(0);

            // find symbol columns
            for (i = 0, c = 0; i < l; i++)
            {
                if (as_list(as_list(y)[1])[i]->type == TYPE_SYMBOL)
                    push_obj(&cols, clone_obj(as_list(as_list(y)[1])[i]));
            }

            sym = distinct_syms(as_list(cols), cols->len);

            if (sym->len > 0)
            {
                s = cstring_from_str("sym", 3);
                col = ray_concat(x, s);
                res = __ray_set(col, sym);

                drop_obj(s);
                drop_obj(col);

                if (is_error(res))
                    return res;

                drop_obj(res);

                s = symbol("sym");
                res = __ray_set(s, sym);

                drop_obj(s);

                if (is_error(res))
                    return res;

                drop_obj(res);
            }

            drop_obj(cols);
            drop_obj(sym);
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
                    drop_obj(s);
                    drop_obj(v);

                    if (is_error(e))
                        return e;

                    v = e;
                }

                p = at_idx(as_list(y)[0], i);
                s = cast_obj(TYPE_C8, p);
                col = ray_concat(x, s);
                res = __ray_set(col, v);

                drop_obj(p);
                drop_obj(v);
                drop_obj(s);
                drop_obj(col);

                if (is_error(res))
                    return res;

                drop_obj(res);
            }

            return clone_obj(x);

        case TYPE_ENUM:
            path = cstring_from_obj(x);
            fd = fs_fopen(as_string(path), ATTR_WRONLY | ATTR_CREAT);

            if (fd == -1)
            {
                res = sys_error(ERROR_TYPE_SYS, as_string(path));
                drop_obj(path);
                return res;
            }

            if (is_external_compound(y))
            {
                size = PAGE_SIZE + sizeof(struct obj_t) + y->len * sizeof(i64_t);

                c = fs_fwrite(fd, (str_p)y - PAGE_SIZE, size);
                fs_fclose(fd);

                if (c == -1)
                {
                    res = sys_error(ERROR_TYPE_SYS, as_string(path));
                    drop_obj(path);
                    return res;
                }

                drop_obj(path);

                return clone_obj(x);
            }

            memset(objbuf, 0, PAGE_SIZE);
            p = (obj_p)objbuf;
            strncpy(as_string(p), strof_sym(as_list(y)[0]->i64), PAGE_SIZE - sizeof(struct obj_t));
            p->mmod = MMOD_EXTERNAL_COMPOUND;

            c = fs_fwrite(fd, objbuf, PAGE_SIZE);
            if (c == -1)
            {
                res = sys_error(ERROR_TYPE_SYS, as_string(path));
                fs_fclose(fd);
                drop_obj(path);
                return res;
            }

            p->type = TYPE_ENUM;
            p->len = as_list(y)[1]->len;

            c = fs_fwrite(fd, objbuf, sizeof(struct obj_t));
            if (c == -1)
            {
                res = sys_error(ERROR_TYPE_SYS, as_string(path));
                fs_fclose(fd);
                drop_obj(path);
                return res;
            }

            size = as_list(y)[1]->len * sizeof(i64_t);

            c = fs_fwrite(fd, as_string(as_list(y)[1]), size);
            fs_fclose(fd);

            if (c == -1)
            {
                res = sys_error(ERROR_TYPE_SYS, as_string(path));
                drop_obj(path);
                return res;
            }

            drop_obj(path);

            return clone_obj(x);

        case TYPE_LIST:
            s = cstring_from_str("#", 1);
            col = ray_concat(x, s);
            drop_obj(s);

            l = y->len;
            size = size_obj(y) - sizeof(i8_t) - sizeof(u64_t);
            buf = vector_u8(size);
            b = as_u8(buf);
            k = vector_i64(l);

            for (i = 0, size = 0; i < l; i++)
            {
                v = as_list(y)[i];
                sz = save_obj(b, l, v);

                if (sz == 0)
                {
                    drop_obj(col);
                    drop_obj(buf);
                    drop_obj(k);

                    throw(ERR_NOT_SUPPORTED, "set: unsupported type: %s", type_name(y->type));
                }

                as_i64(k)[i] = size;

                size += sz;
                b += sz;
            }

            res = __ray_set(col, buf);

            drop_obj(col);
            drop_obj(buf);

            if (is_error(res))
                return res;

            drop_obj(res);

            // save anymap
            path = cstring_from_obj(x);
            fd = fs_fopen(as_string(path), ATTR_WRONLY | ATTR_CREAT);

            if (fd == -1)
            {
                res = sys_error(ERROR_TYPE_SYS, as_string(path));
                drop_obj(path);
                return res;
            }

            memset(objbuf, 0, PAGE_SIZE);
            p = (obj_p)objbuf;

            p->mmod = MMOD_EXTERNAL_COMPOUND;

            c = fs_fwrite(fd, objbuf, PAGE_SIZE);
            if (c == -1)
            {
                res = sys_error(ERROR_TYPE_SYS, as_string(path));
                fs_fclose(fd);
                drop_obj(path);
                return res;
            }

            p->type = TYPE_ANYMAP;
            p->len = y->len;

            c = fs_fwrite(fd, objbuf, sizeof(struct obj_t));
            if (c == -1)
            {
                res = sys_error(ERROR_TYPE_SYS, as_string(path));
                fs_fclose(fd);
                drop_obj(path);
                return res;
            }

            size = k->len * sizeof(i64_t);

            c = fs_fwrite(fd, as_string(k), size);
            drop_obj(k);
            fs_fclose(fd);

            if (c == -1)
            {
                res = sys_error(ERROR_TYPE_SYS, as_string(path));
                drop_obj(path);
                return res;
            }

            drop_obj(path);

            return clone_obj(x);

        default:
            if (is_vector(y))
            {
                path = cstring_from_obj(x);
                fd = fs_fopen(as_string(path), ATTR_WRONLY | ATTR_CREAT);

                if (fd == -1)
                {
                    res = sys_error(ERROR_TYPE_SYS, as_string(path));
                    drop_obj(path);
                    return res;
                }

                size = size_of(y);

                c = fs_fwrite(fd, (str_p)y, size);

                if (c == -1)
                {
                    e = sys_error(ERROR_TYPE_SYS, as_string(path));
                    drop_obj(path);
                    fs_fclose(fd);
                    return e;
                }

                // write mmod
                lseek(fd, 0, SEEK_SET);
                mmod = MMOD_EXTERNAL_SIMPLE;
                c = fs_fwrite(fd, (str_p)&mmod, sizeof(u8_t));
                if (c == -1)
                {
                    e = sys_error(ERROR_TYPE_SYS, as_string(path));
                    drop_obj(path);
                    fs_fclose(fd);
                    return e;
                }

                drop_obj(path);
                fs_fclose(fd);

                return clone_obj(x);
            }

            throw(ERR_TYPE, "set: unsupported types: %s %s", type_name(x->type), type_name(y->type));
        }

    default:
        throw(ERR_TYPE, "set: unsupported types: %s %s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_set(obj_p x, obj_p y)
{
    obj_p e, res;

    e = eval(y);
    if (is_error(e))
        return e;

    res = __ray_set(x, e);
    drop_obj(e);

    return res;
}

obj_p ray_let(obj_p x, obj_p y)
{
    obj_p e;

    switch (x->type)
    {
    case -TYPE_SYMBOL:
        e = eval(y);

        if (is_error(e))
            return e;

        return amend(x, e);

    default:
        throw(ERR_TYPE, "let: unsupported types: '%s '%s", type_name(x->type), type_name(y->type));
    }
}
