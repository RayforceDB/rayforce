/*
 *   Copyright (c) 2024 Anton Kundenko <singaraiona@gmail.com>
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

#include "binary.h"
#include "compose.h"
#include "def.h"
#include "error.h"
#include "fs.h"
#include "ops.h"
#include "runtime.h"
#include "serde.h"
#include "string.h"
#include "io.h"
#include "iter.h"

obj_p binary_call(obj_p f, obj_p x, obj_p y) {
    binary_f fn;

    switch (f->attrs & FN_ATOMIC_MASK) {
        case FN_ATOMIC:
            return map_binary(f, x, y);
        case FN_LEFT_ATOMIC:
            return map_binary_left(f, x, y);
        case FN_RIGHT_ATOMIC:
            return map_binary_right(f, x, y);
        default:
            fn = (binary_f)f->i64;
            return fn(x, y);
    }
}

obj_p binary_set(obj_p x, obj_p y) {
    i64_t fd, c = 0;
    i64_t i, l, sz, size;
    u8_t *b;
    obj_p res, col, s, p, k, v, e, path, buf;
    c8_t objbuf[RAY_PAGE_SIZE] = {0};

    switch (x->type) {
        case -TYPE_SYMBOL:
            res = set_obj(&runtime_get()->env.variables, x, clone_obj(y));

            if (y && y->type == TYPE_LAMBDA) {
                if (is_null(AS_LAMBDA(y)->name))
                    AS_LAMBDA(y)->name = clone_obj(x);
            }

            if (IS_ERR(res))
                return res;

            return clone_obj(y);

        case TYPE_C8:
            switch (y->type) {
                case TYPE_SYMBOL:
                    path = cstring_from_obj(x);
                    fd = fs_fopen(AS_C8(path), ATTR_WRONLY | ATTR_CREAT);

                    if (fd == -1) {
                        res = sys_error(ERROR_TYPE_SYS, AS_C8(path));
                        drop_obj(path);
                        return res;
                    }

                    buf = ser_obj(y);

                    if (IS_ERR(buf)) {
                        fs_fclose(fd);
                        drop_obj(path);
                        return buf;
                    }

                    c = fs_fwrite(fd, (str_p)AS_U8(buf), buf->len);
                    fs_fclose(fd);
                    drop_obj(buf);

                    if (c == -1) {
                        res = sys_error(ERROR_TYPE_SYS, AS_C8(path));
                        drop_obj(path);
                        return res;
                    }

                    drop_obj(path);

                    return clone_obj(x);
                case TYPE_TABLE:
                    return io_set_table(x, y);
                case TYPE_ENUM:
                    path = cstring_from_obj(x);
                    fd = fs_fopen(AS_C8(path), ATTR_WRONLY | ATTR_CREAT);

                    if (fd == -1) {
                        res = sys_error(ERROR_TYPE_SYS, AS_C8(path));
                        drop_obj(path);
                        return res;
                    }

                    if (IS_EXTERNAL_COMPOUND(y)) {
                        size = RAY_PAGE_SIZE + sizeof(struct obj_t) + y->len * sizeof(i64_t);

                        c = fs_fwrite(fd, (str_p)y - RAY_PAGE_SIZE, size);
                        fs_fclose(fd);

                        if (c == -1) {
                            res = sys_error(ERROR_TYPE_SYS, AS_C8(path));
                            drop_obj(path);
                            return res;
                        }

                        drop_obj(path);

                        return clone_obj(x);
                    }

                    memset(objbuf, 0, RAY_PAGE_SIZE);
                    p = (obj_p)objbuf;
                    strncpy(AS_C8(p), str_from_symbol(AS_LIST(y)[0]->i64), RAY_PAGE_SIZE - sizeof(struct obj_t));
                    p->mmod = MMOD_EXTERNAL_COMPOUND;

                    c = fs_fwrite(fd, objbuf, RAY_PAGE_SIZE);
                    if (c == -1) {
                        res = sys_error(ERROR_TYPE_SYS, AS_C8(path));
                        fs_fclose(fd);
                        drop_obj(path);
                        return res;
                    }

                    p->type = TYPE_ENUM;
                    p->len = AS_LIST(y)[1]->len;

                    c = fs_fwrite(fd, objbuf, sizeof(struct obj_t));
                    if (c == -1) {
                        res = sys_error(ERROR_TYPE_SYS, AS_C8(path));
                        fs_fclose(fd);
                        drop_obj(path);
                        return res;
                    }

                    size = AS_LIST(y)[1]->len * sizeof(i64_t);

                    c = fs_fwrite(fd, AS_C8(AS_LIST(y)[1]), size);
                    fs_fclose(fd);

                    if (c == -1) {
                        res = sys_error(ERROR_TYPE_SYS, AS_C8(path));
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
                    size = size_obj(y) - sizeof(i8_t) - sizeof(i64_t);
                    buf = U8(size);
                    b = AS_U8(buf);
                    k = I64(l);

                    for (i = 0, size = 0; i < l; i++) {
                        v = AS_LIST(y)[i];
                        sz = ser_raw(b, v);

                        if (sz == 0) {
                            drop_obj(col);
                            drop_obj(buf);
                            drop_obj(k);

                            THROW_TYPE1("set", y->type);
                        }

                        AS_I64(k)[i] = size;

                        size += sz;
                        b += sz;
                    }

                    res = binary_set(col, buf);

                    drop_obj(col);
                    drop_obj(buf);

                    if (IS_ERR(res))
                        return res;

                    drop_obj(res);

                    // save anymap
                    path = cstring_from_obj(x);
                    fd = fs_fopen(AS_C8(path), ATTR_WRONLY | ATTR_CREAT);

                    if (fd == -1) {
                        res = sys_error(ERROR_TYPE_SYS, AS_C8(path));
                        drop_obj(path);
                        return res;
                    }

                    memset(objbuf, 0, RAY_PAGE_SIZE);
                    p = (obj_p)objbuf;

                    p->mmod = MMOD_EXTERNAL_COMPOUND;

                    c = fs_fwrite(fd, objbuf, RAY_PAGE_SIZE);
                    if (c == -1) {
                        res = sys_error(ERROR_TYPE_SYS, AS_C8(path));
                        fs_fclose(fd);
                        drop_obj(path);
                        return res;
                    }

                    p->type = TYPE_MAPLIST;
                    p->len = y->len;

                    c = fs_fwrite(fd, objbuf, sizeof(struct obj_t));
                    if (c == -1) {
                        res = sys_error(ERROR_TYPE_SYS, AS_C8(path));
                        fs_fclose(fd);
                        drop_obj(path);
                        return res;
                    }

                    size = k->len * sizeof(i64_t);

                    c = fs_fwrite(fd, AS_C8(k), size);
                    drop_obj(k);
                    fs_fclose(fd);

                    if (c == -1) {
                        res = sys_error(ERROR_TYPE_SYS, AS_C8(path));
                        drop_obj(path);
                        return res;
                    }

                    drop_obj(path);

                    return clone_obj(x);

                default:
                    if (IS_VECTOR(y)) {
                        path = cstring_from_obj(x);
                        fd = fs_fopen(AS_C8(path), ATTR_WRONLY | ATTR_CREAT);

                        if (fd == -1) {
                            res = sys_error(ERROR_TYPE_SYS, AS_C8(path));
                            drop_obj(path);
                            return res;
                        }

                        // Create a clean struct obj_t header to avoid writing uninitialized bytes
                        struct obj_t clean_header;
                        memset(&clean_header, 0, sizeof(struct obj_t));
                        clean_header.mmod = MMOD_EXTERNAL_SIMPLE;
                        clean_header.order = 0;
                        clean_header.type = y->type;
                        clean_header.attrs = y->attrs;
                        clean_header.rc = 0;
                        clean_header.len = y->len;

                        // Write the clean header
                        c = fs_fwrite(fd, (str_p)&clean_header, sizeof(struct obj_t));
                        if (c == -1) {
                            e = sys_error(ERROR_TYPE_SYS, AS_C8(path));
                            drop_obj(path);
                            fs_fclose(fd);
                            return e;
                        }

                        // Write the data
                        size = y->len * size_of_type(y->type);
                        c = fs_fwrite(fd, AS_C8(y), size);
                        if (c == -1) {
                            e = sys_error(ERROR_TYPE_SYS, AS_C8(path));
                            drop_obj(path);
                            fs_fclose(fd);
                            return e;
                        }

                        drop_obj(path);
                        fs_fclose(fd);

                        return clone_obj(x);
                    }

                    THROW_TYPE2("set", x->type, y->type);
            }

        default:
            THROW_TYPE2("set", x->type, y->type);
    }
}

obj_p ray_set(obj_p x, obj_p y) {
    obj_p e, res;

    e = eval(y);
    if (IS_ERR(e))
        return e;

    res = binary_set(x, e);
    drop_obj(e);

    return res;
}

obj_p ray_let(obj_p x, obj_p y) {
    obj_p e;

    switch (x->type) {
        case -TYPE_SYMBOL:
            e = eval(y);

            if (IS_ERR(e))
                return e;

            return amend(x, e);

        default:
            THROW_TYPE2("let", x->type, y->type);
    }
}
