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

#define _POSIX_C_SOURCE 200809L
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

// Atomic unary functions (iterates through list of argument items down to atoms)
obj_p unary_call_atomic(unary_f f, obj_p x)
{
    u64_t i, l;
    obj_p res = NULL_OBJ, item = NULL_OBJ, a, *v;

    switch (x->type)
    {
    case TYPE_LIST:
        l = ops_count(x);

        if (l == 0)
            return NULL_OBJ;

        v = as_list(x);
        item = unary_call_atomic(f, v[0]);

        if (is_error(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : list(l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            item = unary_call_atomic(f, v[i]);

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
        if (l == 0)
            return NULL_OBJ;

        a = at_idx(x, 0);
        item = unary_call_atomic(f, a);
        drop_obj(a);

        if (is_error(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : vector(TYPE_LIST, l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            a = at_idx(x, i);
            item = unary_call_atomic(f, a);
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
        return f(x);
    }
}

obj_p unary_call(u8_t attrs, unary_f f, obj_p x)
{
    if (attrs & FN_ATOMIC)
        return unary_call_atomic(f, x);

    return f(x);
}

obj_p ray_get(obj_p x)
{
    i64_t fd;
    obj_p res, col, keys, vals, val, s, v, id, *sym, path;
    u64_t i, l, size;

    switch (x->type)
    {
    case -TYPE_SYMBOL:
        sym = deref(x);
        if (sym == NULL)
            return error(ERR_TYPE, "get: symbol '%s' not found", strof_sym(x->i64));

        return clone_obj(*sym);
    case TYPE_C8:
        if (x->len == 0)
            throw(ERR_LENGTH, "get: empty string path");

        // get splayed table
        if (x->len > 1 && as_string(x)[x->len - 1] == '/')
        {
            // first try to read columns schema
            s = cstring_from_str(".d", 2);
            col = ray_concat(x, s);
            keys = ray_get(col);
            drop_obj(s);
            drop_obj(col);

            if (is_error(keys))
                return keys;

            if (keys->type != TYPE_SYMBOL)
            {
                drop_obj(keys);
                throw(ERR_TYPE, "get: expected table schema as a symbol vector, got: '%s", type_name(keys->type));
            }

            l = keys->len;
            vals = list(l);

            for (i = 0; i < l; i++)
            {
                v = at_idx(keys, i);
                s = cast_obj(TYPE_C8, v);
                col = ray_concat(x, s);
                val = ray_get(col);

                drop_obj(v);
                drop_obj(s);
                drop_obj(col);

                if (is_error(val))
                {
                    vals->len = i;
                    drop_obj(vals);
                    drop_obj(keys);

                    return val;
                }

                as_list(vals)[i] = val;
            }

            // read symbol data (if any)
            s = cstring_from_str("sym", 3);
            col = ray_concat(x, s);
            v = ray_get(col);

            drop_obj(s);
            drop_obj(col);

            if (!is_error(v))
            {
                s = symbol("sym");
                drop_obj(ray_set(s, v));
                drop_obj(s);
            }

            drop_obj(v);

            return table(keys, vals);
        }
        // get other obj
        else
        {
            path = cstring_from_obj(x);
            fd = fs_fopen(as_string(path), ATTR_RDWR);

            if (fd == -1)
            {
                res = sys_error(ERROR_TYPE_SYS, as_string(path));
                drop_obj(path);
                return res;
            }

            size = fs_fsize(fd);

            if (size < sizeof(struct obj_t))
            {
                res = error(ERR_LENGTH, "get: file '%s': invalid size: %d", as_string(path), size);
                drop_obj(path);
                fs_fclose(fd);
                return res;
            }

            drop_obj(path);

            res = (obj_p)mmap_file(fd, size);

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
                drop_obj(id);
            }

            if (is_external_compound(res))
                res = (obj_p)((str_p)res + PAGE_SIZE);

            // anymap needs additional nested mapping of dependencies
            if (res->type == TYPE_ANYMAP)
            {
                s = cstring_from_str("#", 1);
                col = ray_concat(x, s);
                keys = ray_get(col);
                drop_obj(s);
                drop_obj(col);

                if (keys->type != TYPE_U8)
                {
                    drop_obj(keys);
                    mmap_free(res, size);
                    throw(ERR_TYPE, "get: expected anymap schema as a byte vector, got: '%s", type_name(keys->type));
                }

                ((obj_p)((str_p)res - PAGE_SIZE))->obj = keys;
            }

            res->rc = 1;

            return res;
        }

    default:
        throw(ERR_TYPE, "get: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_time(obj_p x)
{
    struct timespec start, end;
    f64_t elapsed;

    clock_gettime(CLOCK_MONOTONIC, &start);

    x = eval(x);
    if (is_error(x))
        return x;
    drop_obj(x);

    clock_gettime(CLOCK_MONOTONIC, &end);

    elapsed = (end.tv_sec - start.tv_sec) * 1e3;    // Convert to milliseconds
    elapsed += (end.tv_nsec - start.tv_nsec) / 1e6; // Convert nanoseconds to milliseconds

    return f64(elapsed);
}

obj_p ray_bins(obj_p x)
{
    obj_p bins, res;

    switch (x->type)
    {
    case TYPE_B8:
    case TYPE_U8:
    case TYPE_C8:
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
        throw(ERR_TYPE, "bins: unsupported type: '%s", type_name(x->type));
    }

    res = clone_obj(as_list(bins)[1]);
    drop_obj(bins);

    return res;
}
