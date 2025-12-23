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
#include "runtime.h"
#include "eval.h"
#include "format.h"
#include "fs.h"
#include "ops.h"
#include "serde.h"
#include "compose.h"
#include "error.h"
#include "string.h"
#include "fdmap.h"
#include "iter.h"

obj_p unary_call(obj_p f, obj_p x) {
    unary_f fn;

    if (f->attrs & FN_ATOMIC)
        return map_unary(f, x);

    fn = (unary_f)f->i64;
    return fn(x);
}

obj_p ray_get(obj_p x) {
    i64_t fd;
    obj_p res, col, keys, s, v, *sym, path, fdmap;
    i64_t sz, size;

    switch (x->type) {
        case -TYPE_SYMBOL:
            sym = resolve(x->i64);
            if (sym == NULL)
                return ray_err(ERR_TYPE);

            return clone_obj(*sym);
        case TYPE_C8:
            if (x->len == 0)
                return ray_err("get: empty string path");

            path = cstring_from_obj(x);
            fd = fs_fopen(AS_C8(path), ATTR_RDWR);

            if (fd == -1) {
                res = sys_error(ERR_SYS, AS_C8(path));
                drop_obj(path);
                return res;
            }

            size = fs_fsize(fd);

            if (size < ISIZEOF(struct obj_t)) {
                res = ray_err(ERR_TYPE);
                drop_obj(path);
                fs_fclose(fd);
                return res;
            }

            res = (obj_p)mmap_file(fd, NULL, size, 0);

            if (res == NULL) {
                res = ray_err(ERR_TYPE);
                drop_obj(path);
                fs_fclose(fd);
                return res;
            }

            if (IS_EXTERNAL_SERIALIZED(res)) {
                sz = size - ISIZEOF(struct obj_t);
                v = de_raw((u8_t *)res + ISIZEOF(struct obj_t), &sz);
                mmap_free(res, size);
                fs_fclose(fd);
                drop_obj(path);
                return v;
            } else if (IS_EXTERNAL_COMPOUND(res)) {
                fdmap = fdmap_create();
                fdmap_add_fd(&fdmap, res, fd, size);
                res = (obj_p)((str_p)res + RAY_PAGE_SIZE);
                runtime_fdmap_push(runtime_get(), res, fdmap);
            } else if (IS_EXTERNAL_SIMPLE(res)) {
                fdmap = fdmap_create();
                fdmap_add_fd(&fdmap, res, fd, size);
                runtime_fdmap_push(runtime_get(), res, fdmap);
            } else {
                res = ray_err(ERR_TYPE);
                drop_obj(path);
                return res;
            }

            drop_obj(path);

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
                    return ray_err(ERR_TYPE);
                }

                ((obj_p)((str_p)res - RAY_PAGE_SIZE))->obj = keys;
            }
            return clone_obj(res);  // increment ref count

        default:
            return ray_err(ERR_TYPE);
    }
}

obj_p ray_resolve(obj_p x) {
    if (x->type != -TYPE_SYMBOL)
        return ray_err(ERR_TYPE);

    obj_p *res = resolve(x->i64);

    return (res == NULL) ? NULL_OBJ : clone_obj(*res);
}

obj_p ray_unicode_format(obj_p x) {
    if (x->type != -TYPE_B8)
        return ray_err(ERR_TYPE);

    format_set_use_unicode(x->b8);

    return null(0);
}

obj_p ray_is_null(obj_p x) { return b8(is_null(x)); }
