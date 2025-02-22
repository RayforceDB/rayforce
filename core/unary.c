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
#include "string.h"
#include "pool.h"
#include "io.h"
#include "fdmap.h"

// Atomic unary functions (iterates through list of argument items down to atoms)
obj_p unary_call_atomic(unary_f f, obj_p x) {
    u64_t i, l, n;
    obj_p res = NULL_OBJ, item = NULL_OBJ, a, *v, parts;
    pool_p pool;

    pool = pool_get();

    switch (x->type) {
        case TYPE_LIST:
            l = ops_count(x);

            if (l == 0)
                return NULL_OBJ;

            v = AS_LIST(x);
            n = pool_split_by(pool, l, 0);

            if (n > 1) {
                pool_prepare(pool);

                for (i = 0; i < l; i++)
                    pool_add_task(pool, (raw_p)unary_call_atomic, 2, f, v[i]);

                parts = pool_run(pool);

                return parts;
            }

            item = unary_call_atomic(f, v[0]);

            if (IS_ERROR(item))
                return item;

            res = item->type < 0 ? vector(item->type, l) : LIST(l);

            ins_obj(&res, 0, item);

            for (i = 1; i < l; i++) {
                item = unary_call_atomic(f, v[i]);

                if (IS_ERROR(item)) {
                    res->len = i;
                    drop_obj(res);
                    return item;
                }

                ins_obj(&res, i, item);
            }

            return res;

        case TYPE_MAPLIST:
            l = ops_count(x);
            if (l == 0)
                return NULL_OBJ;

            a = at_idx(x, 0);
            item = unary_call_atomic(f, a);
            drop_obj(a);

            if (IS_ERROR(item))
                return item;

            res = item->type < 0 ? vector(item->type, l) : vector(TYPE_LIST, l);

            ins_obj(&res, 0, item);

            for (i = 1; i < l; i++) {
                a = at_idx(x, i);
                item = unary_call_atomic(f, a);
                drop_obj(a);

                if (IS_ERROR(item)) {
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

obj_p unary_call(u8_t attrs, unary_f f, obj_p x) {
    if (attrs & FN_ATOMIC)
        return unary_call_atomic(f, x);

    return f(x);
}

obj_p ray_get(obj_p x) {
    i64_t fd;
    obj_p res, col, keys, s, v, *sym, path, fdmap;
    u64_t size;

    switch (x->type) {
        case -TYPE_SYMBOL:
            sym = resolve(x->i64);
            if (sym == NULL)
                return error(ERR_NOT_FOUND, "get: symbol '%s' not found", str_from_symbol(x->i64));

            return clone_obj(*sym);
        case TYPE_C8:
            if (x->len == 0)
                THROW(ERR_LENGTH, "get: empty string path");

            path = cstring_from_obj(x);
            fd = fs_fopen(AS_C8(path), ATTR_RDWR);

            if (fd == -1) {
                res = sys_error(ERROR_TYPE_SYS, AS_C8(path));
                drop_obj(path);
                return res;
            }

            size = fs_fsize(fd);

            if (size < sizeof(struct obj_t)) {
                res = error(ERR_LENGTH, "get: file '%s': invalid size: %d", AS_C8(path), size);
                drop_obj(path);
                fs_fclose(fd);
                return res;
            }

            drop_obj(path);

            res = (obj_p)mmap_file(fd, NULL, size, 0);

            if (IS_EXTERNAL_SERIALIZED(res)) {
                v = de_raw((u8_t *)res, size);
                mmap_free(res, size);
                fs_fclose(fd);
                return v;
            } else if (IS_EXTERNAL_COMPOUND(res)) {
                fdmap = fdmap_create();
                fdmap_add_fd(&fdmap, res, fd, size);
                res = (obj_p)((str_p)res + RAY_PAGE_SIZE);
                runtime_fdmap_push(runtime_get(), res, fdmap);
            } else {
                fdmap = fdmap_create();
                fdmap_add_fd(&fdmap, res, fd, size);
                runtime_fdmap_push(runtime_get(), res, fdmap);
            }

            // anymap needs additional nested mapping of dependencies
            if (res->type == TYPE_MAPLIST) {
                s = cstring_from_str("#", 1);
                col = ray_concat(x, s);
                keys = ray_get(col);
                drop_obj(s);
                drop_obj(col);

                if (keys->type != TYPE_U8) {
                    drop_obj(keys);
                    mmap_free(res, size);
                    THROW(ERR_TYPE, "get: expected anymap schema as a byte vector, got: '%s", type_name(keys->type));
                }

                ((obj_p)((str_p)res - RAY_PAGE_SIZE))->obj = keys;
            }
            return clone_obj(res);  // increment ref count

        default:
            THROW(ERR_TYPE, "get: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_resolve(obj_p x) {
    if (x->type != -TYPE_SYMBOL)
        return error(ERR_TYPE, "resolve: expected symbol, got: '%s'", type_name(x->type));

    obj_p *res = resolve(x->i64);

    return (res == NULL) ? NULL_OBJ : clone_obj(*res);
}

obj_p ray_unicode_format(obj_p x) {
    if (x->type != -TYPE_B8)
        return error(ERR_TYPE, "graphic_format: expected bool, got: '%s'", type_name(x->type));

    format_set_use_unicode(x->b8);

    return null(0);
}

obj_p ray_is_null(obj_p x) { return b8(is_null(x)); }