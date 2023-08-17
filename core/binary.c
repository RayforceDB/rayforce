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
#include "util.h"
#include "format.h"
#include "hash.h"
#include "fs.h"
#include "serde.h"
#include "ops.h"

obj_t call_binary(binary_f f, obj_t x, obj_t y)
{
    obj_t cst, res;
    bool_t dropx = false, dropy = false;

    if (x->type == TYPE_ENUM)
    {
        x = rf_value(x);
        if (is_error(x))
            return x;

        dropx = true;
    }

    if (y->type == TYPE_ENUM)
    {
        y = rf_value(y);
        if (is_error(y))
        {
            if (dropx)
                drop(x);
            return y;
        }

        dropy = true;
    }

    // no need to cast
    if (x->type == y->type || x->type == -y->type)
    {
        res = f(x, y);
        goto call;
    }

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_F64):
        cst = f64(x->i64);
        res = f(cst, y);
        drop(cst);
        goto call;
    case mtype2(-TYPE_F64, -TYPE_I64):
        cst = f64(y->i64);
        res = f(x, cst);
        drop(cst);
        goto call;
    case mtype2(-TYPE_I64, TYPE_F64):
        cst = f64(x->i64);
        res = f(cst, y);
        drop(cst);
        goto call;
    case mtype2(-TYPE_F64, TYPE_I64):
        cst = cast(TYPE_F64, y);
        res = f(x, cst);
        drop(cst);
        goto call;
    case mtype2(TYPE_I64, -TYPE_F64):
        cst = cast(TYPE_I64, x);
        res = f(y, cst);
        drop(cst);
        goto call;
    case mtype2(TYPE_F64, -TYPE_I64):
        cst = f64(y->i64);
        res = f(x, cst);
        drop(cst);
        goto call;
    case mtype2(TYPE_I64, TYPE_F64):
        cst = cast(TYPE_F64, x);
        res = f(cst, y);
        drop(cst);
        goto call;
    case mtype2(TYPE_F64, TYPE_I64):
        cst = cast(TYPE_F64, y);
        res = f(cst, x);
        drop(cst);
        goto call;
    default:
        res = f(x, y);
    }

call:
    if (dropx)
        drop(x);
    if (dropy)
        drop(y);

    return res;
}

obj_t rf_call_binary_left_atomic(binary_f f, obj_t x, obj_t y)
{
    u64_t i, l;
    obj_t res, item, a;

    if (x->type == TYPE_LIST)
    {
        l = x->len;
        a = at_idx(x, 0);
        item = rf_call_binary_left_atomic(f, a, y);
        drop(a);

        if (item->type == TYPE_ERROR)
            return item;

        res = vector(item->type, l);

        write_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            a = at_idx(x, i);
            item = rf_call_binary_left_atomic(f, a, y);
            drop(a);

            if (item->type == TYPE_ERROR)
            {
                res->len = i;
                drop(res);
                return item;
            }

            write_obj(&res, i, item);
        }

        return res;
    }

    return f(x, y);
}

obj_t rf_call_binary_right_atomic(binary_f f, obj_t x, obj_t y)
{
    u64_t i, l;
    obj_t res, item, b;

    if (y->type == TYPE_LIST)
    {
        l = y->len;
        b = at_idx(y, 0);
        item = rf_call_binary_right_atomic(f, x, b);
        drop(b);

        if (item->type == TYPE_ERROR)
            return item;

        res = vector(item->type, l);

        write_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            b = at_idx(y, i);
            item = rf_call_binary_right_atomic(f, x, b);
            drop(b);

            if (item->type == TYPE_ERROR)
            {
                res->len = i;
                drop(res);
                return item;
            }

            write_obj(&res, i, item);
        }

        return res;
    }

    return f(x, y);
}

// Atomic binary functions (iterates through list of arguments down to atoms)
obj_t rf_call_binary_atomic(binary_f f, obj_t x, obj_t y)
{
    u64_t i, l;
    obj_t res, item, a, b;

    if ((x->type == TYPE_LIST && is_vector(y)) ||
        (y->type == TYPE_LIST && is_vector(x)))
    {
        l = x->len;

        if (l != y->len)
            return error(ERR_LENGTH, "binary: vectors must be of the same length");

        a = at_idx(x, 0);
        b = at_idx(y, 0);
        item = rf_call_binary_atomic(f, a, b);
        drop(a);
        drop(b);

        if (is_error(item))
            return item;

        res = vector(item->type, l);

        write_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            a = at_idx(x, i);
            b = at_idx(y, i);
            item = rf_call_binary_atomic(f, a, b);
            drop(a);
            drop(b);

            if (item->type == TYPE_ERROR)
            {
                res->len = i;
                drop(res);
                return item;
            }

            write_obj(&res, i, item);
        }

        return res;
    }
    else if (x->type == TYPE_LIST)
    {
        l = x->len;
        a = at_idx(x, 0);
        item = rf_call_binary_atomic(f, a, y);
        drop(a);

        if (item->type == TYPE_ERROR)
            return item;

        res = vector(item->type, l);

        write_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            a = at_idx(x, i);
            item = rf_call_binary_atomic(f, a, y);
            drop(a);

            if (item->type == TYPE_ERROR)
            {
                res->len = i;
                drop(res);
                return item;
            }

            write_obj(&res, i, item);
        }

        return res;
    }
    else if (y->type == TYPE_LIST)
    {
        l = y->len;
        b = at_idx(y, 0);
        item = rf_call_binary_atomic(f, x, b);
        drop(b);

        if (item->type == TYPE_ERROR)
            return item;

        res = vector(item->type, l);

        write_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            b = at_idx(y, i);
            item = rf_call_binary_atomic(f, x, b);
            drop(b);

            if (item->type == TYPE_ERROR)
            {
                res->len = i;
                drop(res);
                return item;
            }

            write_obj(&res, i, item);
        }

        return res;
    }

    return call_binary(f, x, y);
}

obj_t rf_call_binary(u8_t attrs, binary_f f, obj_t x, obj_t y)
{
    switch (attrs)
    {
    case FLAG_ATOMIC:
        return rf_call_binary_atomic(f, x, y);
    case FLAG_LEFT_ATOMIC:
        return rf_call_binary_left_atomic(f, x, y);
    case FLAG_RIGHT_ATOMIC:
        return rf_call_binary_right_atomic(f, x, y);
    default:
        return f(x, y);
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

obj_t rf_set(obj_t x, obj_t y)
{
    obj_t res, col, s, p, k, v, e, cols, sym, buf;
    i64_t fd, c = 0;
    u64_t i, l, sz, size;
    byte_t *b;

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
                raise(ERR_IO, "write: failed to open file '%s': %s", as_string(x), get_os_error());

            buf = ser(y);

            if (is_error(buf))
            {
                fs_fclose(fd);
                return buf;
            }

            c = fs_fwrite(fd, as_byte(buf), buf->len);
            fs_fclose(fd);
            drop(buf);

            if (c == -1)
                raise(ERR_IO, "write: failed to write to file '%s': %s", as_string(x), get_os_error());

            return clone(x);
        case TYPE_TABLE:
            if (as_string(x)[x->len - 1] != '/')
                raise(ERR_TYPE, "set: table path must be a directory");

            // save columns schema
            s = string_from_str(".d", 2);
            col = rf_concat(x, s);
            res = rf_set(col, as_list(y)[0]);

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
                    join_obj(&cols, clone(as_list(as_list(y)[1])[i]));
            }

            sym = distinct_syms(as_list(cols), cols->len);

            if (sym->len > 0)
            {
                s = string_from_str("sym", 3);
                col = rf_concat(x, s);
                res = rf_set(col, sym);

                drop(s);
                drop(col);

                if (is_error(res))
                    return res;

                drop(res);

                s = symbol("sym");
                res = rf_set(s, sym);

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
                    e = rf_enum(s, v);

                    drop(s);
                    drop(v);

                    if (is_error(e))
                        return e;

                    v = e;
                }

                p = at_idx(as_list(y)[0], i);
                s = cast(TYPE_CHAR, p);
                col = rf_concat(x, s);
                res = rf_set(col, v);

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
                raise(ERR_IO, "set: failed to open file '%s': %s", as_string(x), get_os_error());

            if (is_external_compound(y))
            {
                size = PAGE_SIZE + sizeof(struct obj_t) + y->len * sizeof(i64_t);

                c = fs_fwrite(fd, (str_t)y - PAGE_SIZE, size);
                fs_fclose(fd);

                if (c == -1)
                    raise(ERR_IO, "set: failed to write to file '%s': %s", as_string(x), get_os_error());

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
                raise(ERR_IO, "set: failed to write to file '%s': %s", as_string(x), get_os_error());
            }

            p->type = TYPE_ENUM;
            p->len = as_list(y)[1]->len;

            c = fs_fwrite(fd, (str_t)p, sizeof(struct obj_t));
            if (c == -1)
            {
                heap_free(p);
                fs_fclose(fd);
                raise(ERR_IO, "set: failed to write to file '%s': %s", as_string(x), get_os_error());
            }

            size = as_list(y)[1]->len * sizeof(i64_t);

            c = fs_fwrite(fd, as_string(as_list(y)[1]), size);
            heap_free(p);
            fs_fclose(fd);

            if (c == -1)
                raise(ERR_IO, "set: failed to write to file '%s': %s", as_string(x), get_os_error());

            return clone(x);

        case TYPE_LIST:
            s = string_from_str("#", 1);
            col = rf_concat(x, s);
            drop(s);

            l = y->len;
            size = size_obj(y) - sizeof(type_t) - sizeof(u64_t);
            buf = vector_byte(size);
            b = as_byte(buf);
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

                    raise(ERR_NOT_SUPPORTED, "set: unsupported type: %d", y->type);
                }

                as_i64(k)[i] = size;

                size += sz;
                b += sz;
            }

            res = rf_set(col, buf);

            drop(col);
            drop(buf);

            if (is_error(res))
                return res;

            drop(res);

            // save anymap
            fd = fs_fopen(as_string(x), ATTR_WRONLY | ATTR_CREAT);

            if (fd == -1)
                raise(ERR_IO, "set: failed to open file '%s': %s", as_string(x), get_os_error());

            p = (obj_t)heap_alloc(PAGE_SIZE);

            memset((str_t)p, 0, PAGE_SIZE);

            p->mmod = MMOD_EXTERNAL_COMPOUND;

            c = fs_fwrite(fd, (str_t)p, PAGE_SIZE);
            if (c == -1)
            {
                heap_free(p);
                fs_fclose(fd);
                raise(ERR_IO, "set: failed to write to file '%s': %s", as_string(x), get_os_error());
            }

            p->type = TYPE_ANYMAP;
            p->len = y->len;

            c = fs_fwrite(fd, (str_t)p, sizeof(struct obj_t));
            if (c == -1)
            {
                heap_free(p);
                fs_fclose(fd);
                raise(ERR_IO, "set: failed to write to file '%s': %s", as_string(x), get_os_error());
            }

            size = k->len * sizeof(i64_t);

            c = fs_fwrite(fd, as_string(k), size);
            heap_free(p);
            heap_free(k);
            fs_fclose(fd);

            if (c == -1)
                raise(ERR_IO, "set: failed to write to file '%s': %s", as_string(x), get_os_error());

            // --

            return clone(x);

        default:
            if (is_vector(y))
            {
                fd = fs_fopen(as_string(x), ATTR_WRONLY | ATTR_CREAT);

                if (fd == -1)
                    raise(ERR_IO, "set: failed to open file '%s': %s", as_string(x), get_os_error());

                p = (obj_t)heap_alloc(sizeof(struct obj_t));

                memcpy(p, y, sizeof(struct obj_t));

                p->mmod = MMOD_EXTERNAL_SIMPLE;

                c = fs_fwrite(fd, (str_t)p, sizeof(struct obj_t));
                if (c == -1)
                {
                    heap_free(p);
                    fs_fclose(fd);
                    raise(ERR_IO, "set: failed to write to file '%s': %s", as_string(x), get_os_error());
                }

                size = size_of(y) - sizeof(struct obj_t);

                c = fs_fwrite(fd, as_string(y), size);
                heap_free(p);
                fs_fclose(fd);

                if (c == -1)
                    raise(ERR_IO, "set: failed to write to file '%s': %s", as_string(x), get_os_error());

                return clone(x);
            }

            raise(ERR_TYPE, "set: unsupported types: %d %d", x->type, y->type);
        }

    default:
        raise(ERR_TYPE, "set: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_write(obj_t x, obj_t y)
{
    unused(x);
    unused(y);
    raise(ERR_NOT_IMPLEMENTED, "write: not implemented");
}

obj_t rf_cast(obj_t x, obj_t y)
{
    type_t type;
    obj_t err;
    str_t fmt, msg;

    if (x->type != -TYPE_SYMBOL)
        raise(ERR_TYPE, "cast: first argument must be a symbol");

    type = env_get_type_by_typename(&runtime_get()->env, x->i64);

    if (type == TYPE_ERROR)
    {
        fmt = obj_fmt(x);
        msg = str_fmt(0, "cast: unknown type: '%s'", fmt);
        err = error(ERR_TYPE, msg);
        heap_free(fmt);
        heap_free(msg);
        return err;
    }

    return cast(type, y);
}

obj_t rf_dict(obj_t x, obj_t y)
{
    if (!is_vector(x) || !is_vector(y))
        return error(ERR_TYPE, "Keys and Values must be lists");

    if (x->len != y->len)
        return error(ERR_LENGTH, "Keys and Values must have the same length");

    return dict(clone(x), clone(y));
}

obj_t rf_table(obj_t x, obj_t y)
{
    bool_t s = false;
    u64_t i, j, len, cl = 0;
    obj_t lst, c, l = null(0);

    if (x->type != TYPE_SYMBOL)
        return error(ERR_TYPE, "Keys must be a symbol vector");

    if (y->type != TYPE_LIST)
    {
        if (x->len != 1)
            return error(ERR_LENGTH, "Keys and Values must have the same length");

        l = list(1);
        as_list(l)[0] = clone(y);
        y = l;
    }

    if (x->len != y->len)
    {
        drop(l);
        return error(ERR_LENGTH, "Keys and Values must have the same length");
    }

    len = y->len;

    for (i = 0; i < len; i++)
    {
        switch (as_list(y)[i]->type)
        {
        // case TYPE_NULL:
        case -TYPE_BOOL:
        case -TYPE_I64:
        case -TYPE_F64:
        case -TYPE_CHAR:
        case -TYPE_SYMBOL:
        case -TYPE_TIMESTAMP:
        case TYPE_LAMBDA:
        case TYPE_DICT:
        case TYPE_TABLE:
            s = true;
            break;
        case TYPE_BOOL:
        case TYPE_I64:
        case TYPE_F64:
        case TYPE_TIMESTAMP:
        case TYPE_CHAR:
        case TYPE_SYMBOL:
        case TYPE_LIST:
            j = as_list(y)[i]->len;
            if (cl != 0 && j != cl)
                return error(ERR_LENGTH, "Values must be of the same length");

            cl = j;
            break;
        case TYPE_ENUM:
            j = as_list(as_list(y)[i])[1]->len;
            if (cl != 0 && j != cl)
                return error(ERR_LENGTH, "Values must be of the same length");

            cl = j;
            break;

        default:
            return error(ERR_TYPE, "Unsupported type in a Values list");
        }
    }

    // there are no atoms and all columns are of the same length
    if (!s)
    {
        drop(l);
        return table(clone(x), clone(y));
    }

    // otherwise we need to expand atoms to vectors
    lst = list(len);

    if (cl == 0)
        cl = 1;

    for (i = 0; i < len; i++)
    {
        switch (as_list(y)[i]->type)
        {
        // case TYPE_NULL:
        case -TYPE_BOOL:
        case -TYPE_I64:
        case -TYPE_F64:
        case -TYPE_CHAR:
        case -TYPE_SYMBOL:
            c = i64(cl);
            as_list(lst)[i] = rf_take(c, as_list(y)[i]);
            break;
        default:
            as_list(lst)[i] = clone(as_list(y)[i]);
        }
    }

    drop(l);

    return table(clone(x), lst);
}

obj_t rf_rand(obj_t x, obj_t y)
{
    i64_t i, count;
    obj_t vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        count = x->i64;
        vec = vector_i64(count);

        for (i = 0; i < count; i++)
            as_i64(vec)[i] = rfi_rand_u64() % y->i64;

        return vec;

    default:
        raise(ERR_TYPE, "rand: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_add(obj_t x, obj_t y)
{
    u64_t i, l;
    obj_t vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return i64(addi64(x->i64, y->i64));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return f64(addf64(x->f64, y->f64));

    case mtype2(-TYPE_TIMESTAMP, -TYPE_I64):
        return timestamp(addi64(x->i64, y->i64));

    case mtype2(-TYPE_I64, TYPE_I64):
        l = y->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = addi64(x->i64, as_i64(y)[i]);

        return vec;

    case mtype2(-TYPE_F64, TYPE_F64):
        l = y->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = addf64(x->f64, as_f64(y)[i]);

        return vec;

    case mtype2(-TYPE_TIMESTAMP, TYPE_I64):
        l = y->len;
        vec = vector_timestamp(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = addi64(x->i64, as_i64(y)[i]);

        return vec;

    case mtype2(TYPE_TIMESTAMP, -TYPE_I64):
        l = x->len;
        vec = vector_timestamp(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = addi64(as_i64(x)[i], y->i64);

        return vec;

    case mtype2(TYPE_TIMESTAMP, TYPE_I64):
        l = x->len;
        vec = vector_timestamp(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = addi64(as_i64(x)[i], as_i64(y)[i]);

        return vec;

    case mtype2(TYPE_I64, -TYPE_I64):
        l = x->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = addi64(as_i64(x)[i], y->i64);

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
        l = x->len;
        if (l != y->len)
            return error(ERR_LENGTH, "add: vectors must be of the same length");
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = addi64(as_i64(x)[i], as_i64(y)[i]);

        return vec;

    case mtype2(TYPE_F64, -TYPE_F64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = addf64(as_f64(x)[i], y->f64);

        return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        l = x->len;
        if (l != y->len)
            return error(ERR_LENGTH, "add: vectors must be of the same length");
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = addf64(as_f64(x)[i], as_f64(y)[i]);

        return vec;

    default:
        raise(ERR_TYPE, "add: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_sub(obj_t x, obj_t y)
{
    i32_t i;
    i64_t l;
    obj_t vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return (i64(subi64(x->i64, y->i64)));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (f64(subf64(x->f64, y->f64)));

    case mtype2(TYPE_I64, -TYPE_I64):
        l = x->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = subi64(as_i64(x)[i], y->i64);

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
        l = x->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = subi64(as_i64(x)[i], as_i64(y)[i]);

        return vec;

    case mtype2(TYPE_F64, -TYPE_F64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = subf64(as_f64(x)[i], y->f64);

        return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = subf64(as_f64(x)[i], as_f64(y)[i]);

        return vec;

    default:
        raise(ERR_TYPE, "sub: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_mul(obj_t x, obj_t y)
{
    i32_t i;
    i64_t l;
    obj_t vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return (i64(muli64(x->i64, y->i64)));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (f64(mulf64(x->f64, y->f64)));

    case mtype2(TYPE_I64, -TYPE_I64):
        l = x->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = muli64(as_i64(x)[i], y->i64);

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
        l = x->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = muli64(as_i64(x)[i], as_i64(y)[i]);

        return vec;

    case mtype2(TYPE_F64, -TYPE_F64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = mulf64(as_f64(x)[i], y->f64);

        return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = mulf64(as_f64(x)[i], as_f64(y)[i]);

        return vec;

    default:
        raise(ERR_TYPE, "mul: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_div(obj_t x, obj_t y)
{
    i32_t i;
    i64_t l;
    obj_t vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return (i64(divi64(x->i64, y->i64)));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (f64(divf64(x->f64, y->f64)));

    case mtype2(TYPE_I64, -TYPE_I64):
        l = x->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = divi64(as_i64(x)[i], y->i64);

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
        l = x->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = divi64(as_i64(x)[i], as_i64(y)[i]);

        return vec;

    case mtype2(TYPE_F64, -TYPE_F64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = divf64(as_f64(x)[i], y->f64);

        return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = divf64(as_f64(x)[i], as_f64(y)[i]);

        return vec;

    default:
        raise(ERR_TYPE, "div: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_fdiv(obj_t x, obj_t y)
{
    i32_t i;
    i64_t l;
    obj_t vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return (f64(fdivi64(x->i64, y->i64)));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (f64(fdivf64(x->f64, y->f64)));

    case mtype2(TYPE_I64, -TYPE_I64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = fdivi64(as_i64(x)[i], y->i64);

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = fdivi64(as_i64(x)[i], as_i64(y)[i]);

        return vec;

    case mtype2(TYPE_F64, -TYPE_F64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = fdivf64(as_f64(x)[i], y->f64);

        return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = fdivf64(as_f64(x)[i], as_f64(y)[i]);

        return vec;

    default:
        raise(ERR_TYPE, "fdiv: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_mod(obj_t x, obj_t y)
{
    i32_t i;
    i64_t l;
    obj_t vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return (i64(modi64(x->i64, y->i64)));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (f64(modf64(x->f64, y->f64)));

    case mtype2(TYPE_I64, -TYPE_I64):
        l = x->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = modi64(as_i64(x)[i], y->i64);

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
        l = x->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = modi64(as_i64(x)[i], as_i64(y)[i]);

        return vec;

    case mtype2(TYPE_F64, -TYPE_F64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = modf64(as_f64(x)[i], y->f64);

        return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = modf64(as_f64(x)[i], as_f64(y)[i]);

        return vec;

    default:
        raise(ERR_TYPE, "mod: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_like(obj_t x, obj_t y)
{
    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_CHAR, TYPE_CHAR):
        return (bool(str_match(as_string(x), as_string(y))));

    default:
        raise(ERR_TYPE, "like: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_eq(obj_t x, obj_t y)
{
    i64_t i, l;
    obj_t vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_BOOL, TYPE_BOOL):
        return (bool(x->bool == y->bool));

    case mtype2(-TYPE_I64, -TYPE_I64):
    case mtype2(-TYPE_SYMBOL, -TYPE_SYMBOL):
    case mtype2(-TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        return (bool(x->i64 == y->i64));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (bool(x->f64 == y->f64));

    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_SYMBOL, -TYPE_SYMBOL):
    case mtype2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        l = x->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_bool(vec)[i] = as_i64(x)[i] == y->i64;

        return vec;

    case mtype2(TYPE_F64, -TYPE_F64):
        l = x->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_bool(vec)[i] = as_f64(x)[i] == y->f64;

        return vec;

    case mtype2(-TYPE_I64, TYPE_I64):
    case mtype2(-TYPE_SYMBOL, TYPE_SYMBOL):
    case mtype2(-TYPE_TIMESTAMP, TYPE_TIMESTAMP):
        l = y->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_bool(vec)[i] = x->i64 == as_i64(y)[i];

        return vec;

    case mtype2(-TYPE_F64, TYPE_F64):
        l = y->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_bool(vec)[i] = x->f64 == as_f64(y)[i];

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_SYMBOL, TYPE_SYMBOL):
    case mtype2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
        if (x->len != y->len)
            return error(ERR_LENGTH, "eq: vectors of different length");

        l = x->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_bool(vec)[i] = as_i64(x)[i] == as_i64(y)[i];

        return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        if (x->len != y->len)
            return error(ERR_LENGTH, "eq: vectors of different length");

        l = x->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_bool(vec)[i] = as_f64(x)[i] == as_f64(y)[i];

        return vec;

    case mtype2(TYPE_LIST, TYPE_LIST):
        if (x->len != y->len)
            return error(ERR_LENGTH, "eq: lists of different length");

        l = x->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_bool(vec)[i] = equal(as_list(x)[i], as_list(y)[i]);

        return vec;

    default:
        raise(ERR_TYPE, "eq: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_ne(obj_t x, obj_t y)
{
    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_BOOL, TYPE_BOOL):
        return (bool(x->bool != y->bool));

    case mtype2(-TYPE_I64, -TYPE_I64):
        return (bool(x->i64 != y->i64));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (bool(x->f64 != y->f64));

    default:
        raise(ERR_TYPE, "ne: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_lt(obj_t x, obj_t y)
{
    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return (bool(x->i64 < y->i64));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (bool(x->f64 < y->f64));

    default:
        raise(ERR_TYPE, "lt: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_le(obj_t x, obj_t y)
{
    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return (bool(x->i64 <= y->i64));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (bool(x->f64 <= y->f64));

    default:
        raise(ERR_TYPE, "le: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_gt(obj_t x, obj_t y)
{
    i64_t i, l;
    obj_t vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return (bool(x->i64 > y->i64));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (bool(x->f64 > y->f64));

    case mtype2(TYPE_I64, TYPE_I64):
        if (x->len != y->len)
            return error(ERR_LENGTH, "gt: vectors of different length");

        l = x->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_bool(vec)[i] = as_i64(x)[i] > as_i64(y)[i];

        return vec;

    default:
        raise(ERR_TYPE, "gt: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_ge(obj_t x, obj_t y)
{
    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return (bool(x->i64 >= y->i64));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (bool(x->f64 >= y->f64));

    default:
        raise(ERR_TYPE, "ge: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_and(obj_t x, obj_t y)
{
    i32_t i;
    i64_t l;
    obj_t res;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_BOOL, -TYPE_BOOL):
        return (bool(x->bool && y->bool));

    case mtype2(TYPE_BOOL, TYPE_BOOL):
        l = x->len;
        res = vector_bool(x->len);
        for (i = 0; i < l; i++)
            as_bool(res)[i] = as_bool(x)[i] & as_bool(y)[i];

        return res;

    default:
        raise(ERR_TYPE, "and: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_or(obj_t x, obj_t y)
{
    i32_t i;
    i64_t l;
    obj_t res;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_BOOL, -TYPE_BOOL):
        return (bool(x->bool || y->bool));

    case mtype2(TYPE_BOOL, TYPE_BOOL):
        l = x->len;
        res = vector_bool(x->len);
        for (i = 0; i < l; i++)
            as_bool(res)[i] = as_bool(x)[i] | as_bool(y)[i];

        return res;

    default:
        raise(ERR_TYPE, "or: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_at(obj_t x, obj_t y)
{
    u64_t i, j, yl, xl;
    obj_t res, k, s, v, c, cols;
    byte_t *buf;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_BOOL, -TYPE_I64):
    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_F64, -TYPE_I64):
    case mtype2(TYPE_TIMESTAMP, -TYPE_I64):
    case mtype2(TYPE_GUID, -TYPE_I64):
    case mtype2(TYPE_CHAR, -TYPE_I64):
    case mtype2(TYPE_LIST, -TYPE_I64):
        return at_idx(x, y->i64);

    case mtype2(TYPE_TABLE, -TYPE_SYMBOL):
        return at_obj(x, y);

    case mtype2(TYPE_BOOL, TYPE_I64):
        yl = y->len;
        xl = x->len;
        res = vector_bool(yl);
        for (i = 0; i < yl; i++)
        {
            if (as_bool(y)[i] >= (i64_t)xl)
                as_bool(res)[i] = false;
            else
                as_bool(res)[i] = as_bool(x)[(i32_t)as_bool(y)[i]];
        }

        return res;

    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_SYMBOL, TYPE_I64):
    case mtype2(TYPE_TIMESTAMP, TYPE_I64):
        yl = y->len;
        xl = x->len;
        res = vector(x->type, yl);
        for (i = 0; i < yl; i++)
        {
            if (as_i64(y)[i] >= (i64_t)xl)
                as_i64(res)[i] = NULL_I64;
            else
                as_i64(res)[i] = as_i64(x)[as_i64(y)[i]];
        }

        return res;

    case mtype2(TYPE_F64, TYPE_I64):
        yl = y->len;
        xl = x->len;
        res = vector_f64(yl);
        for (i = 0; i < yl; i++)
        {
            if (as_i64(y)[i] >= (i64_t)xl)
                as_f64(res)[i] = NULL_F64;
            else
                as_f64(res)[i] = as_f64(x)[(i32_t)as_i64(y)[i]];
        }

        return res;

        // case mtype2(TYPE_GUID, TYPE_I64):
        //     yl = y->len;
        //     xl = x->len;
        //     vec = vector_guid(yl);
        //     for (i = 0; i < yl; i++)
        //     {
        //         if (as_i64(y)[i] >= xl)
        //             as_guid(vec)[i] = guid(NULL_GUID);
        //         else
        //             as_guid(vec)[i] = as_guid(x)[(i32_t)as_i64(y)[i]];
        //     }

        //     return vec;

    case mtype2(TYPE_CHAR, TYPE_I64):
        yl = y->len;
        xl = x->len;
        res = string(yl);
        for (i = 0; i < yl; i++)
        {
            if (as_i64(y)[i] >= (i64_t)xl)
                as_string(res)[i] = ' ';
            else
                as_string(res)[i] = as_string(x)[(i32_t)as_i64(y)[i]];
        }

        return res;

    case mtype2(TYPE_LIST, TYPE_I64):
        yl = y->len;
        xl = x->len;
        res = list(yl);
        for (i = 0; i < yl; i++)
        {
            if (as_i64(y)[i] >= (i64_t)xl)
                as_list(res)[i] = null(0);
            else
                as_list(res)[i] = clone(as_list(x)[(i32_t)as_i64(y)[i]]);
        }

        return res;

    case mtype2(TYPE_TABLE, TYPE_I64):
        xl = as_list(x)[0]->len;
        cols = vector(TYPE_LIST, xl);
        for (i = 0; i < xl; i++)
        {
            c = rf_at(as_list(as_list(x)[1])[i], y);

            if (is_atom(c))
                c = rf_enlist(&c, 1);

            if (is_error(c))
            {
                cols->len = i;
                drop(cols);
                return c;
            }

            write_obj(&cols, i, c);
        }

        res = table(clone(as_list(x)[0]), cols);

        return res;

    case mtype2(TYPE_TABLE, TYPE_SYMBOL):
        xl = as_list(x)[1]->len;
        yl = y->len;
        cols = vector(TYPE_LIST, yl);
        for (i = 0; i < yl; i++)
        {
            for (j = 0; j < xl; j++)
            {
                if (as_symbol(as_list(x)[0])[j] == as_symbol(y)[i])
                {
                    as_list(cols)[i] = clone(as_list(as_list(x)[1])[i]);
                    break;
                }
            }

            if (j == xl)
            {
                cols->len = i;
                drop(cols);
                raise(ERR_INDEX, "at: column '%s' has not found in a table", symtostr(as_symbol(y)[i]));
            }
        }

        res = table(clone(y), cols);

        return res;

    case mtype2(TYPE_ENUM, -TYPE_I64):
        k = rf_key(x);
        s = rf_get(k);
        drop(k);

        v = enum_val(x);

        if (y->i64 >= (i64_t)v->len)
        {
            drop(s);
            raise(ERR_INDEX, "at: enum can not be resolved: index out of range");
        }

        if (!s || is_error(s) || s->type != TYPE_SYMBOL)
        {
            drop(s);
            return i64(as_i64(v)[y->i64]);
        }

        if (as_i64(v)[y->i64] >= (i64_t)s->len)
        {
            drop(s);
            raise(ERR_INDEX, "at: enum can not be resolved: index out of range");
        }

        res = at_idx(s, as_i64(v)[y->i64]);

        drop(s);

        return res;

    case mtype2(TYPE_ENUM, TYPE_I64):
        k = rf_key(x);
        s = rf_get(k);
        drop(k);

        if (is_error(s))
            return s;

        v = enum_val(x);

        yl = y->len;

        if (!s || s->type != TYPE_SYMBOL)
        {
            res = vector_i64(yl);

            for (i = 0; i < yl; i++)
                as_i64(res)[i] = as_i64(v)[y->i64];

            drop(s);

            return res;
        }

        res = vector_symbol(yl);

        for (i = 0; i < yl; i++)
        {
            if (as_i64(v)[y->i64] >= (i64_t)s->len)
            {
                drop(s);
                drop(res);
                raise(ERR_INDEX, "at: enum can not be resolved: index out of range");
            }

            as_symbol(res)[i] = as_i64(s)[as_i64(v)[y->i64]];
        }

        drop(s);

        return res;

    case mtype2(TYPE_ANYMAP, -TYPE_I64):
        k = anymap_key(x);
        v = anymap_val(x);

        xl = k->len;
        yl = v->len;

        if (y->i64 >= (i64_t)v->len)
            raise(ERR_INDEX, "at: anymap can not be resolved: index out of range");

        buf = as_byte(k) + as_i64(v)[y->i64];

        return load_obj(&buf, xl);

        // case mtype2(TYPE_ANYMAP, TYPE_I64):
        //     k = anymap_key(x);
        //     v = anymap_val(x);

        //     xl = k->len;
        //     yl = v->len;

        //     res = vector_i64(yl);

        //     for (i = 0; i < yl; i++)
        //     {
        //         if (as_i64(v)[y->i64] >= (i64_t)xl)
        //         {
        //             drop(res);
        //             raise(ERR_INDEX, "at: anymap can not be resolved: index out of range");
        //         }

        //         buf = as_byte(k) + as_i64(v)[y->i64];

        //         as_i64(res)[i] = load_obj(&buf, xl);
        //     }

        //     return res;

    default:
        raise(ERR_TYPE, "at: unsupported types: %d %d", x->type, y->type);
    }

    return null(0);
}

obj_t rf_find_vector_i64_vector_i64(obj_t x, obj_t y, bool_t allow_null)
{
    u64_t i, range, xl = x->len, yl = y->len;
    i64_t n;
    i64_t max = 0, min = 0, p;
    obj_t vec, found, ht;

    if (xl == 0)
        return vector_i64(0);

    max = min = as_i64(x)[0];

    for (i = 0; i < xl; i++)
    {
        if (as_i64(x)[i] > max)
            max = as_i64(x)[i];
        else if (as_i64(x)[i] < min)
            min = as_i64(x)[i];
    }

    vec = vector_i64(yl);

    range = max - min + 1;

    if (range <= yl)
    {
        found = vector_i64(range);

        for (i = 0; i < range; i++)
            as_i64(found)[i] = NULL_I64;

        for (i = 0; i < xl; i++)
        {
            n = as_i64(x)[i] - min;
            if (as_i64(found)[n] == NULL_I64)
                as_i64(found)[n] = i;
        }

        for (i = 0; i < yl; i++)
        {
            n = as_i64(y)[i] - min;
            if (as_i64(y)[i] < min || as_i64(y)[i] > max)
            {
                if (allow_null)
                    as_i64(vec)[i] = NULL_I64;
                else
                    raise(ERR_INDEX, "find: index out of range");
            }
            else
                as_i64(vec)[i] = as_i64(found)[n];
        }

        drop(found);

        return vec;
    }

    // otherwise, use a hash table
    ht = ht_tab(xl, TYPE_I64);

    for (i = 0; i < xl; i++)
    {
        p = ht_tab_next(&ht, as_i64(x)[i] - min);
        if (as_i64(as_list(ht)[0])[p] == NULL_I64)
        {
            as_i64(as_list(ht)[0])[p] = as_i64(x)[i] - min;
            as_i64(as_list(ht)[1])[p] = i;
        }
    }

    for (i = 0; i < yl; i++)
    {
        p = ht_tab_next(&ht, as_i64(y)[i] - min);
        if (as_i64(as_list(ht)[0])[p] == NULL_I64)
        {
            if (allow_null)
                as_i64(vec)[i] = NULL_I64;
            else
                raise(ERR_INDEX, "find: index out of range");
        }
        else
            as_i64(vec)[i] = as_i64(as_list(ht)[1])[p];
    }

    drop(ht);

    return vec;
}

obj_t rf_find(obj_t x, obj_t y)
{
    u64_t l, i;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_BOOL, -TYPE_BOOL):
    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_F64, -TYPE_F64):
    case mtype2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
    case mtype2(TYPE_GUID, -TYPE_GUID):
    case mtype2(TYPE_CHAR, -TYPE_CHAR):
        l = x->len;
        i = find_obj(x, y);

        if (i == l)
            return i64(NULL_I64);
        else
            return i64(i);

    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_SYMBOL, TYPE_SYMBOL):
        return rf_find_vector_i64_vector_i64(x, y, true);

    default:
        raise(ERR_TYPE, "find: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_concat(obj_t x, obj_t y)
{
    i64_t i, xl, yl;
    obj_t vec;

    if (!x || !y)
        return list(2, clone(x), clone(y));

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_BOOL, -TYPE_BOOL):
        vec = vector_bool(2);
        as_bool(vec)[0] = x->bool;
        as_bool(vec)[1] = y->bool;
        return vec;

    case mtype2(-TYPE_I64, -TYPE_I64):
        vec = vector_i64(2);
        as_i64(vec)[0] = x->i64;
        as_i64(vec)[1] = y->i64;
        return vec;

    case mtype2(-TYPE_F64, -TYPE_F64):
        vec = vector_f64(2);
        as_f64(vec)[0] = x->f64;
        as_f64(vec)[1] = y->f64;
        return vec;

    case mtype2(-TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        vec = vector_timestamp(2);
        as_timestamp(vec)[0] = x->i64;
        as_timestamp(vec)[1] = y->i64;
        return vec;

        // case mtype2(-TYPE_GUID, -TYPE_GUID):
        //     vec = vector_guid(2);
        //     memcpy(&as_guid(vec)[0], x->guid, sizeof(guid_t));
        //     memcpy(&as_guid(vec)[1], y->guid, sizeof(guid_t));
        //     return vec;

    case mtype2(-TYPE_CHAR, -TYPE_CHAR):
        vec = string(2);
        as_string(vec)[0] = x->vchar;
        as_string(vec)[1] = y->vchar;
        return vec;

    case mtype2(TYPE_BOOL, -TYPE_BOOL):
        xl = x->len;
        vec = vector_bool(xl + 1);
        for (i = 0; i < xl; i++)
            as_bool(vec)[i] = as_bool(x)[i];

        as_bool(vec)[xl] = y->bool;
        return vec;

    case mtype2(TYPE_I64, -TYPE_I64):
        xl = x->len;
        vec = vector_i64(xl + 1);
        for (i = 0; i < xl; i++)
            as_i64(vec)[i] = as_i64(x)[i];

        as_i64(vec)[xl] = y->i64;
        return vec;

    case mtype2(-TYPE_I64, TYPE_I64):
        yl = y->len;
        vec = vector_i64(yl + 1);
        as_i64(vec)[0] = x->i64;
        for (i = 1; i <= yl; i++)
            as_i64(vec)[i] = as_i64(y)[i - 1];

        return vec;

    case mtype2(TYPE_F64, -TYPE_F64):
        xl = x->len;
        vec = vector_f64(xl + 1);
        for (i = 0; i < xl; i++)
            as_f64(vec)[i] = as_f64(x)[i];

        as_f64(vec)[xl] = y->f64;
        return vec;

    case mtype2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        xl = x->len;
        vec = vector_timestamp(xl + 1);
        for (i = 0; i < xl; i++)
            as_timestamp(vec)[i] = as_timestamp(x)[i];

        as_timestamp(vec)[xl] = y->i64;
        return vec;

        // case mtype2(TYPE_GUID, -TYPE_GUID):
        //     xl = x->len;
        //     vec = vector_guid(xl + 1);
        //     for (i = 0; i < xl; i++)
        //         as_guid(vec)[i] = as_guid(x)[i];

        //     memcpy(&as_guid(vec)[xl], y->guid, sizeof(guid_t));
        //     return vec;

    case mtype2(TYPE_BOOL, TYPE_BOOL):
        xl = x->len;
        yl = y->len;
        vec = vector_bool(xl + yl);
        for (i = 0; i < xl; i++)
            as_bool(vec)[i] = as_bool(x)[i];
        for (i = 0; i < yl; i++)
            as_bool(vec)[i + xl] = as_bool(y)[i];
        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
        xl = x->len;
        yl = y->len;
        vec = vector_i64(xl + yl);
        for (i = 0; i < xl; i++)
            as_i64(vec)[i] = as_i64(x)[i];
        for (i = 0; i < yl; i++)
            as_i64(vec)[i + xl] = as_i64(y)[i];
        return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        xl = x->len;
        yl = y->len;
        vec = vector_f64(xl + yl);
        for (i = 0; i < xl; i++)
            as_f64(vec)[i] = as_f64(x)[i];
        for (i = 0; i < yl; i++)
            as_f64(vec)[i + xl] = as_f64(y)[i];
        return vec;

    case mtype2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
        xl = x->len;
        yl = y->len;
        vec = vector_timestamp(xl + yl);
        for (i = 0; i < xl; i++)
            as_timestamp(vec)[i] = as_timestamp(x)[i];
        for (i = 0; i < yl; i++)
            as_timestamp(vec)[i + xl] = as_timestamp(y)[i];
        return vec;

    case mtype2(TYPE_GUID, TYPE_GUID):
        xl = x->len;
        yl = y->len;
        vec = vector_guid(xl + yl);
        for (i = 0; i < xl; i++)
            as_guid(vec)[i] = as_guid(x)[i];
        for (i = 0; i < yl; i++)
            as_guid(vec)[i + xl] = as_guid(y)[i];
        return vec;

    case mtype2(TYPE_CHAR, TYPE_CHAR):
        xl = x->len;
        yl = y->len;
        vec = string(xl + yl);
        for (i = 0; i < xl; i++)
            as_string(vec)[i] = as_string(x)[i];
        for (i = 0; i < yl; i++)
            as_string(vec)[i + xl] = as_string(y)[i];
        return vec;

    case mtype2(TYPE_LIST, TYPE_LIST):
        xl = x->len;
        yl = y->len;
        vec = list(xl + yl);
        for (i = 0; i < xl; i++)
            as_list(vec)[i] = clone(as_list(x)[i]);
        for (i = 0; i < yl; i++)
            as_list(vec)[i + xl] = clone(as_list(y)[i]);
        return vec;

    default:
        if (x->type == TYPE_LIST)
        {
            xl = x->len;
            vec = list(xl + 1);
            for (i = 0; i < xl; i++)
                as_list(vec)[i] = clone(as_list(x)[i]);
            as_list(vec)[xl] = clone(y);

            return vec;
        }
        if (y->type == TYPE_LIST)
        {
            yl = y->len;
            vec = list(yl + 1);
            as_list(vec)[0] = clone(x);
            for (i = 0; i < yl; i++)
                as_list(vec)[i + 1] = clone(as_list(y)[i]);

            return vec;
        }

        raise(ERR_TYPE, "join: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_filter(obj_t x, obj_t y)
{
    u64_t i, j = 0, l;
    obj_t res, vals, col;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_BOOL, TYPE_BOOL):
        if (x->len != y->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = vector_bool(l);
        for (i = 0; i < l; i++)
            if (as_bool(y)[i])
                as_bool(res)[j++] = as_bool(x)[i];

        resize(&res, j);

        return res;

    case mtype2(TYPE_I64, TYPE_BOOL):
        if (x->len != y->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = vector_i64(l);
        for (i = 0; i < l; i++)
            if (as_bool(y)[i])
                as_i64(res)[j++] = as_i64(x)[i];

        resize(&res, j);

        return res;

    case mtype2(TYPE_SYMBOL, TYPE_BOOL):
        if (x->len != y->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = vector_symbol(l);
        for (i = 0; i < l; i++)
            if (as_bool(y)[i])
                as_symbol(res)[j++] = as_symbol(x)[i];

        resize(&res, j);

        return res;

    case mtype2(TYPE_F64, TYPE_BOOL):
        if (x->len != y->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = vector_f64(l);
        for (i = 0; i < l; i++)
            if (as_bool(y)[i])
                as_f64(res)[j++] = as_f64(x)[i];

        resize(&res, j);

        return res;

    case mtype2(TYPE_TIMESTAMP, TYPE_BOOL):
        if (x->len != y->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = vector_timestamp(l);
        for (i = 0; i < l; i++)
            if (as_bool(y)[i])
                as_timestamp(res)[j++] = as_timestamp(x)[i];

        resize(&res, j);

        return res;

    case mtype2(TYPE_GUID, TYPE_BOOL):
        if (x->len != y->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = vector_guid(l);
        for (i = 0; i < l; i++)
            if (as_bool(y)[i])
                as_guid(res)[j++] = as_guid(x)[i];

        resize(&res, j);

        return res;

    case mtype2(TYPE_CHAR, TYPE_BOOL):
        if (x->len != y->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = string(l);
        for (i = 0; i < l; i++)
            if (as_bool(y)[i])
                as_string(res)[j++] = as_string(x)[i];

        resize(&res, j);

        return res;

    case mtype2(TYPE_LIST, TYPE_BOOL):
        if (x->len != y->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = list(l);
        for (i = 0; i < l; i++)
            if (as_bool(y)[i])
                as_list(res)[j++] = clone(as_list(x)[i]);

        resize(&res, j);

        return res;

    case mtype2(TYPE_TABLE, TYPE_BOOL):
        vals = as_list(x)[1];
        l = vals->len;
        res = list(l);

        for (i = 0; i < l; i++)
        {
            col = rf_filter(as_list(vals)[i], y);
            as_list(res)[i] = col;
        }

        return table(clone(as_list(x)[0]), res);

    default:
        raise(ERR_TYPE, "filter: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_take(obj_t x, obj_t y)
{
    u64_t i, j, l, m, n;
    obj_t k, s, v, res, c, cols;
    byte_t *buf;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, TYPE_I64):
    case mtype2(-TYPE_I64, TYPE_SYMBOL):
    case mtype2(-TYPE_I64, TYPE_TIMESTAMP):
        l = y->len;
        m = x->i64;
        res = vector(y->type, m);

        for (i = 0; i < m; i++)
            as_i64(res)[i] = as_i64(y)[i % l];

        return res;

    case mtype2(-TYPE_I64, TYPE_F64):
        l = y->len;
        m = x->i64;
        res = vector_f64(m);

        for (i = 0; i < m; i++)
            as_f64(res)[i] = as_f64(y)[i % l];

        return res;

    case mtype2(-TYPE_I64, -TYPE_I64):
    case mtype2(-TYPE_I64, -TYPE_SYMBOL):
    case mtype2(-TYPE_I64, -TYPE_TIMESTAMP):
        l = x->i64;
        res = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(res)[i] = y->i64;

        return res;

    case mtype2(-TYPE_I64, -TYPE_F64):
        l = x->i64;
        res = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(res)[i] = y->f64;

        return res;

        l = x->i64;
        res = vector_timestamp(l);
        for (i = 0; i < l; i++)
            as_timestamp(res)[i] = y->i64;

        return res;

    case mtype2(-TYPE_I64, TYPE_ENUM):
        k = rf_key(y);
        s = rf_get(k);
        drop(k);

        if (is_error(s))
            return s;

        v = enum_val(y);

        l = x->i64;
        m = v->len;

        if (!s || s->type != TYPE_SYMBOL)
        {
            res = vector_i64(l);

            for (i = 0; i < l; i++)
                as_i64(res)[i] = as_i64(v)[i];

            drop(s);

            return res;
        }

        res = vector_symbol(l);

        for (i = 0; i < l; i++)
        {

            if (as_i64(v)[i % m] >= (i64_t)s->len)
            {
                drop(s);
                drop(res);
                raise(ERR_INDEX, "take: enum can not be resolved: index out of range");
            }

            as_symbol(res)[i] = as_i64(s)[as_i64(v)[i % m]];
        }

        drop(s);

        return res;

    case mtype2(-TYPE_I64, TYPE_ANYMAP):
        l = x->i64;
        res = list(l);

        k = anymap_key(y);
        s = anymap_val(y);

        m = k->len;
        n = s->len;

        res = vector(TYPE_LIST, l);

        for (i = 0; i < l; i++)
        {
            if (as_i64(s)[i % n] < (i64_t)m)
            {
                buf = as_byte(k) + as_i64(s)[i % n];
                v = load_obj(&buf, l);

                if (is_error(v))
                {
                    res->len = i;
                    drop(res);
                    return v;
                }

                as_list(res)[i] = v;
            }
            else
            {
                res->len = i;
                drop(res);
                raise(ERR_INDEX, "anymap value: index out of range: %d", as_i64(s)[i % n]);
            }
        }

        return res;

    default:
        raise(ERR_TYPE, "take: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_in(obj_t x, obj_t y)
{
    i64_t i, xl, yl, p;
    obj_t vec, set;

    switch
        mtype2(x->type, y->type)
        {
        case mtype2(TYPE_I64, TYPE_I64):
        case mtype2(TYPE_SYMBOL, TYPE_SYMBOL):
            xl = x->len;
            yl = y->len;
            set = ht_tab(yl, -1);

            for (i = 0; i < yl; i++)
            {
                // p = ht_tab_next_with(&set, as_i64(y)[i], &rfi_i64_hash, &i64_cmp);
                p = ht_tab_next(&set, as_i64(y)[i]);
                if (as_i64(as_list(set)[0])[p] == NULL_I64)
                    as_i64(as_list(set)[0])[p] = as_i64(y)[i];
            }

            vec = vector_bool(xl);

            for (i = 0; i < xl; i++)
            {
                // p = ht_tab_next_with(&set, as_i64(x)[i], &rfi_i64_hash, &i64_cmp);
                p = ht_tab_get(set, as_i64(x)[i]);
                as_bool(vec)[i] = (p != NULL_I64);
            }

            drop(set);

            return vec;

        default:
            raise(ERR_TYPE, "in: unsupported types: %d %d", x->type, y->type);
        }

    return null(0);
}

obj_t rf_sect(obj_t x, obj_t y)
{
    obj_t mask, res;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_SYMBOL, TYPE_SYMBOL):
        mask = rf_in(x, y);
        res = rf_filter(x, mask);
        drop(mask);
        return res;

    default:
        raise(ERR_TYPE, "sect: unsupported types: %d %d", x->type, y->type);
    }

    return null(0);
}

obj_t rf_except(obj_t x, obj_t y)
{
    i64_t i, j = 0, l;
    obj_t mask, nmask, res;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_SYMBOL, -TYPE_SYMBOL):
        l = x->len;
        res = vector(x->type, l);

        for (i = 0; i < l; i++)
        {
            if (as_i64(x)[i] != y->i64)
                as_i64(res)[j++] = as_i64(x)[i];
        }

        resize(&res, j);

        return res;
    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_SYMBOL, TYPE_SYMBOL):
        mask = rf_in(x, y);
        nmask = rf_not(mask);
        drop(mask);
        res = rf_filter(x, nmask);
        drop(nmask);
        return res;
    default:
        raise(ERR_TYPE, "except: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_group_Table(obj_t x, obj_t y)
{
    i64_t i, l;
    obj_t res;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_TABLE, TYPE_LIST):
        l = as_list(x)[1]->len;
        res = list(l);
        for (i = 0; i < l; i++)
            as_list(res)[i] = rf_call_binary_right_atomic(rf_take, as_list(as_list(x)[1])[i], y);

        return table(clone(as_list(x)[0]), res);

    default:
        raise(ERR_TYPE, "group: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_xasc(obj_t x, obj_t y)
{
    obj_t idx, col, res;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_TABLE, -TYPE_SYMBOL):
        col = at_obj(x, y);

        if (is_error(col))
            return col;

        idx = rf_iasc(col);
        drop(col);

        if (is_error(idx))
            return idx;

        res = rf_take(x, idx);

        drop(idx);

        return res;
    default:
        raise(ERR_TYPE, "xasc: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_xdesc(obj_t x, obj_t y)
{
    obj_t idx, col, res;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_TABLE, -TYPE_SYMBOL):
        col = at_obj(x, y);

        if (is_error(col))
            return col;

        idx = rf_idesc(col);
        drop(col);

        if (is_error(idx))
            return idx;

        res = rf_take(x, idx);

        drop(idx);

        return res;
    default:
        raise(ERR_TYPE, "xdesc: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_enum(obj_t x, obj_t y)
{
    obj_t s, v;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_SYMBOL, TYPE_SYMBOL):
        s = rf_get(x);

        if (is_error(s))
            return s;

        if (!s || s->type != TYPE_SYMBOL)
        {
            drop(s);
            raise(ERR_TYPE, "enum: expected vector symbol");
        }

        v = rf_find_vector_i64_vector_i64(s, y, false);
        drop(s);

        if (is_error(v))
        {
            drop(v);
            raise(ERR_TYPE, "enum: can not be fully indexed");
        }

        return venum(clone(x), v);
    default:
        raise(ERR_TYPE, "enum: unsupported types: %d %d", x->type, y->type);
    }
}