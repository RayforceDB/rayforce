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
#include <errno.h>
#include <time.h>
#include "compose.h"
#include "error.h"
#include "format.h"
#include "fs.h"
#include "hash.h"
#include "items.h"
#include "ops.h"
#include "runtime.h"
#include "serde.h"
#include "sock.h"
#include "string.h"
#include "unary.h"
#include "util.h"
#include "vary.h"
#include "io.h"

obj_p binary_call_left_atomic(binary_f f, obj_p x, obj_p y) {
    u64_t i, l;
    obj_p res, item, a;

    switch (x->type) {
        case TYPE_LIST:
            l = ops_count(x);
            a = AS_LIST(x)[0];
            item = binary_call_left_atomic(f, a, y);

            if (IS_ERROR(item))
                return item;

            res = item->type < 0 ? vector(item->type, l) : LIST(l);

            ins_obj(&res, 0, item);

            for (i = 1; i < l; i++) {
                a = AS_LIST(x)[i];
                item = binary_call_left_atomic(f, a, y);

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
            a = at_idx(x, 0);
            item = binary_call_left_atomic(f, a, y);
            drop_obj(a);

            if (item->type == TYPE_ERROR)
                return item;

            res = item->type < 0 ? vector(item->type, l) : LIST(l);

            ins_obj(&res, 0, item);

            for (i = 1; i < l; i++) {
                a = at_idx(x, i);
                item = binary_call_left_atomic(f, a, y);
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
            return f(x, y);
    }
}

obj_p binary_call_right_atomic(binary_f f, obj_p x, obj_p y) {
    u64_t i, l;
    obj_p res, item, b;

    switch (y->type) {
        case TYPE_LIST:
            l = ops_count(y);
            b = AS_LIST(y)[0];
            item = binary_call_right_atomic(f, x, b);

            if (IS_ERROR(item))
                return item;

            res = item->type < 0 ? vector(item->type, l) : LIST(l);

            ins_obj(&res, 0, item);

            for (i = 1; i < l; i++) {
                b = AS_LIST(y)[i];
                item = binary_call_right_atomic(f, x, b);

                if (IS_ERROR(item)) {
                    res->len = i;
                    drop_obj(res);
                    return item;
                }

                ins_obj(&res, i, item);
            }

            return res;

        case TYPE_MAPLIST:
            l = ops_count(y);
            b = at_idx(y, 0);
            item = binary_call_right_atomic(f, x, b);
            drop_obj(b);

            if (IS_ERROR(item))
                return item;

            res = item->type < 0 ? vector(item->type, l) : LIST(l);

            ins_obj(&res, 0, item);

            for (i = 1; i < l; i++) {
                b = at_idx(y, i);
                item = binary_call_right_atomic(f, x, b);
                drop_obj(b);

                if (item->type == TYPE_ERROR) {
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
obj_p binary_call_atomic(binary_f f, obj_p x, obj_p y) {
    u64_t i, l;
    obj_p res, item, a, b;
    i8_t xt, yt;

    if (!x || !y)
        THROW(ERR_TYPE, "binary: null argument");

    xt = x->type;
    yt = y->type;
    if (((xt == TYPE_LIST || xt == TYPE_MAPLIST) && IS_VECTOR(y)) ||
        ((yt == TYPE_LIST || yt == TYPE_MAPLIST) && IS_VECTOR(x))) {
        l = ops_count(x);

        if (l != ops_count(y))
            return error_str(ERR_LENGTH, "binary: vectors must be of the same length");

        if (l == 0)
            return f(x, y);

        a = xt == TYPE_LIST ? AS_LIST(x)[0] : at_idx(x, 0);
        b = yt == TYPE_LIST ? AS_LIST(y)[0] : at_idx(y, 0);
        item = binary_call_atomic(f, a, b);

        if (xt != TYPE_LIST)
            drop_obj(a);
        if (yt != TYPE_LIST)
            drop_obj(b);

        if (IS_ERROR(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : LIST(l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++) {
            a = xt == TYPE_LIST ? AS_LIST(x)[i] : at_idx(x, i);
            b = yt == TYPE_LIST ? AS_LIST(y)[i] : at_idx(y, i);
            item = binary_call_atomic(f, a, b);

            if (xt != TYPE_LIST)
                drop_obj(a);
            if (yt != TYPE_LIST)
                drop_obj(b);

            if (IS_ERROR(item)) {
                res->len = i;
                drop_obj(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;
    } else if (xt == TYPE_LIST || xt == TYPE_MAPLIST) {
        l = ops_count(x);
        if (l == 0)
            return f(x, y);

        a = xt == TYPE_LIST ? AS_LIST(x)[0] : at_idx(x, 0);
        item = binary_call_atomic(f, a, y);
        if (xt != TYPE_LIST)
            drop_obj(a);

        if (IS_ERROR(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : LIST(l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++) {
            a = xt == TYPE_LIST ? AS_LIST(x)[i] : at_idx(x, i);
            item = binary_call_atomic(f, a, y);
            if (xt != TYPE_LIST)
                drop_obj(a);

            if (IS_ERROR(item)) {
                res->len = i;
                drop_obj(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;
    } else if (yt == TYPE_LIST || yt == TYPE_MAPLIST) {
        l = ops_count(y);
        if (l == 0)
            return f(x, y);

        b = yt == TYPE_LIST ? AS_LIST(y)[0] : at_idx(y, 0);
        item = binary_call_atomic(f, x, b);
        if (yt != TYPE_LIST)
            drop_obj(b);

        if (IS_ERROR(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : LIST(l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++) {
            b = yt == TYPE_LIST ? AS_LIST(y)[i] : at_idx(y, i);
            item = binary_call_atomic(f, x, b);
            if (yt != TYPE_LIST)
                drop_obj(b);

            if (IS_ERROR(item)) {
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

obj_p binary_call(u8_t attrs, binary_f f, obj_p x, obj_p y) {
    switch (attrs & FN_ATOMIC_MASK) {
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

obj_p binary_set(obj_p x, obj_p y) {
    i64_t fd, c = 0;
    u64_t i, l, sz, size;
    u32_t rc;
    u8_t *b, mmod;
    obj_p res, col, s, p, k, v, e, path, buf;
    c8_t objbuf[RAY_PAGE_SIZE] = {0};

    switch (x->type) {
        case -TYPE_SYMBOL:
            res = set_obj(&runtime_get()->env.variables, x, clone_obj(y));

            if (y && y->type == TYPE_LAMBDA) {
                if (is_null(AS_LAMBDA(y)->name))
                    AS_LAMBDA(y)->name = clone_obj(x);
            }

            if (IS_ERROR(res))
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

                    if (IS_ERROR(buf)) {
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
                    return io_set_table_splayed(x, y, NULL_OBJ);

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
                    size = size_obj(y) - sizeof(i8_t) - sizeof(u64_t);
                    buf = U8(size);
                    b = AS_U8(buf);
                    k = I64(l);

                    for (i = 0, size = 0; i < l; i++) {
                        v = AS_LIST(y)[i];
                        sz = save_obj(b, l, v);

                        if (sz == 0) {
                            drop_obj(col);
                            drop_obj(buf);
                            drop_obj(k);

                            THROW(ERR_NOT_SUPPORTED, "set: unsupported type: %s", type_name(y->type));
                        }

                        AS_I64(k)[i] = size;

                        size += sz;
                        b += sz;
                    }

                    res = binary_set(col, buf);

                    drop_obj(col);
                    drop_obj(buf);

                    if (IS_ERROR(res))
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

                        size = size_of(y);

                        c = fs_fwrite(fd, (str_p)y, size);

                        if (c == -1) {
                            e = sys_error(ERROR_TYPE_SYS, AS_C8(path));
                            drop_obj(path);
                            fs_fclose(fd);
                            return e;
                        }

                        // write mmod
                        lseek(fd, 0, SEEK_SET);
                        mmod = MMOD_EXTERNAL_SIMPLE;
                        c = fs_fwrite(fd, (str_p)&mmod, sizeof(u8_t));
                        if (c == -1) {
                            e = sys_error(ERROR_TYPE_SYS, AS_C8(path));
                            drop_obj(path);
                            fs_fclose(fd);
                            return e;
                        }

                        // write rc = 0
                        lseek(fd, sizeof(u32_t), SEEK_SET);
                        rc = 0;
                        c = fs_fwrite(fd, (str_p)&rc, sizeof(u32_t));
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

                    THROW(ERR_TYPE, "set: unsupported types: %s %s", type_name(x->type), type_name(y->type));
            }

        default:
            THROW(ERR_TYPE, "set: unsupported types: %s %s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_set(obj_p x, obj_p y) {
    obj_p e, res;

    e = eval(y);
    if (IS_ERROR(e))
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

            if (IS_ERROR(e))
                return e;

            return amend(x, e);

        default:
            THROW(ERR_TYPE, "let: unsupported types: '%s '%s", type_name(x->type), type_name(y->type));
    }
}
