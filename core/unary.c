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
#include "unary.h"
#include "binary.h"
#include "runtime.h"
#include "heap.h"
#include "eval.h"
#include "util.h"
#include "format.h"
#include "sort.h"
#include "guid.h"
#include "env.h"
#include "parse.h"
#include "fs.h"
#include "util.h"
#include "ops.h"
#include "serde.h"
#include "sock.h"
#include "items.h"
#include "compose.h"
#include "error.h"
#include "query.h"
#include "index.h"
#include "group.h"

obj_t call_unary(u8_t attrs, unary_f f, obj_t x)
{
    obj_t v, res;

    if (x->type == TYPE_GROUPMAP)
    {
        if (attrs & FN_AGGR)
            return f(x);
        else
        {
            v = group_collect(x);
            if (attrs & FN_ATOMIC)
            {
                res = call_unary_atomic(attrs, f, v);
                drop(v);
                return res;
            }
            else
            {
                res = f(v);
                drop(v);
                return res;
            }
        }
    }

    return f(x);
}

// Atomic unary functions (iterates through list of argument items down to atoms)
obj_t call_unary_atomic(u8_t attrs, unary_f f, obj_t x)
{
    u64_t i, l;
    obj_t res = NULL, item = NULL, a, *v;

    switch (x->type)
    {
    case TYPE_LIST:
        l = ops_count(x);

        if (l == 0)
            return null(0);

        v = as_list(x);
        item = call_unary_atomic(attrs, f, v[0]);

        if (is_error(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : vector(TYPE_LIST, l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            item = call_unary_atomic(attrs, f, v[i]);

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
        if (l == 0)
            return null(0);

        a = at_idx(x, 0);
        item = call_unary_atomic(attrs, f, a);
        drop(a);

        if (is_error(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : vector(TYPE_LIST, l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            a = at_idx(x, i);
            item = call_unary_atomic(attrs, f, a);
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

    default:
        return call_unary(attrs, f, x);
    }
}

obj_t ray_call_unary(u8_t attrs, unary_f f, obj_t x)
{
    if (!x)
        return null(0);

    if (attrs & FN_ATOMIC)
        return call_unary_atomic(attrs, f, x);

    return call_unary(attrs, f, x);
}

obj_t ray_get(obj_t x)
{
    i64_t fd;
    obj_t res, col, keys, vals, val, s, v, id;
    u64_t i, l, size;

    switch (x->type)
    {
    case -TYPE_SYMBOL:
        v = get_symbol(x);
        if (!is_error(v))
            return clone(v);
        else
            return v;
    case TYPE_CHAR:
        if (x->len == 0)
            throw(ERR_LENGTH, "get: empty string path");

        // get splayed table
        if (x->len > 1 && as_string(x)[x->len - 2] == '/')
        {
            // first try to read columns schema
            s = string_from_str(".d", 2);
            col = ray_concat(x, s);
            keys = ray_get(col);
            drop(s);
            drop(col);

            if (is_error(keys))
                return keys;

            if (keys->type != TYPE_SYMBOL)
            {
                drop(keys);
                throw(ERR_TYPE, "get: expected table schema as a symbol vector, got: '%s", typename(keys->type));
            }

            l = keys->len;
            vals = list(l);

            for (i = 0; i < l; i++)
            {
                v = at_idx(keys, i);
                s = as(TYPE_CHAR, v);
                col = ray_concat(x, s);
                val = ray_get(col);

                drop(v);
                drop(s);
                drop(col);

                if (is_error(val))
                {
                    vals->len = i;
                    drop(vals);
                    drop(keys);

                    return val;
                }

                as_list(vals)[i] = val;
            }

            // read symbol data (if any)
            s = string_from_str("sym", 3);
            col = ray_concat(x, s);
            v = ray_get(col);

            drop(s);
            drop(col);

            if (!is_error(v))
            {
                s = symbol("sym");
                drop(ray_set(s, v));
                drop(s);
            }

            drop(v);

            return table(keys, vals);
        }
        // get other obj
        else
        {
            fd = fs_fopen(as_string(x), ATTR_RDWR);

            if (fd == -1)
                return sys_error(ERROR_TYPE_SYS, as_string(x));

            size = fs_fsize(fd);

            if (size < sizeof(struct obj_t))
            {
                fs_fclose(fd);
                throw(ERR_LENGTH, "get: file '%s': invalid size: %d", as_string(x), size);
            }

            res = (obj_t)mmap_file(fd, size);

            if (is_external_serialized(res))
            {
                v = de_raw((u8_t *)res, size);
                mmap_free(res, size);
                fs_fclose(fd);
                return v;
            }
            else
            {
                // insert fd into runtime fds
                id = i64((i64_t)res);
                set_obj(&runtime_get()->fds, id, i64(fd));
                drop(id);
            }

            if (is_external_compound(res))
                res = (obj_t)((str_t)res + PAGE_SIZE);

            // anymap needs additional nested mapping of dependencies
            if (res->type == TYPE_ANYMAP)
            {
                s = string_from_str("#", 1);
                col = ray_concat(x, s);
                keys = ray_get(col);
                drop(s);
                drop(col);

                if (keys->type != TYPE_BYTE)
                {
                    drop(keys);
                    mmap_free(res, size);
                    throw(ERR_TYPE, "get: expected anymap schema as a byte vector, got: '%s", typename(keys->type));
                }

                ((obj_t)((str_t)res - PAGE_SIZE))->obj = keys;
            }

            res->rc = 1;

            return res;
        }

    default:
        throw(ERR_TYPE, "get: unsupported type: '%s", typename(x->type));
    }
}

obj_t ray_time(obj_t x)
{
    u64_t timer;

    timer = clock();
    x = eval(x);
    if (is_error(x))
        return x;
    drop(x);

    return f64((((f64_t)(clock() - timer)) / CLOCKS_PER_SEC) * 1000);
}

obj_t ray_bins(obj_t x)
{
    obj_t bins, res;

    switch (x->type)
    {
    case TYPE_BOOL:
    case TYPE_BYTE:
    case TYPE_CHAR:
        bins = index_group_i8((i8_t *)as_u8(x), NULL, x->len);
        break;
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        bins = index_group_i64(as_i64(x), NULL, x->len);
        break;
    case TYPE_F64:
        bins = index_group_i64((i64_t *)as_f64(x), NULL, x->len);
        break;
    case TYPE_LIST:
        bins = index_group_obj(as_list(x), NULL, x->len);
        break;
    case TYPE_GUID:
        bins = index_group_guid(as_guid(x), NULL, x->len);
        break;
    default:
        throw(ERR_TYPE, "bins: unsupported type: '%s", typename(x->type));
    }

    res = clone(as_list(bins)[1]);
    drop(bins);

    return res;
}