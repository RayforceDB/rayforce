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

#include <stdio.h>
#include "eval.h"
#include "unary.h"
#include "binary.h"
#include "vary.h"
#include "heap.h"
#include "string.h"
#include "format.h"
#include "util.h"
#include "runtime.h"
#include "error.h"
#include "ops.h"
#include "io.h"
#include "fs.h"
#include "compose.h"
#include "items.h"
#include "order.h"
#include "cmp.h"
#include "iter.h"

obj_p vary_call(obj_p f, obj_p *x, i64_t n) {
    vary_f fn;

    if ((f->attrs & FN_ATOMIC) || (n && x[0]->type == TYPE_MAPGROUP))
        return map_vary(f, x, n);
    else {
        fn = (vary_f)f->i64;
        return fn(x, n);
    }
}

obj_p ray_apply(obj_p *x, i64_t n) {
    i64_t i;
    obj_p f, res;

    if (n < 2)
        return null(0);

    f = x[0];
    x++;
    n--;

    switch (f->type) {
        case TYPE_UNARY:
            if (n != 1)
                THROW_S(ERR_LENGTH, ERR_MSG_APPLY_ARITY);
            return unary_call(f, x[0]);
        case TYPE_BINARY:
            if (n != 2)
                THROW_S(ERR_LENGTH, ERR_MSG_APPLY_ARITY);
            return binary_call(f, x[0], x[1]);
        case TYPE_VARY:
            return vary_call(f, x, n);
        case TYPE_LAMBDA:
            if (n != AS_LAMBDA(f)->args->len)
                THROW_S(ERR_LENGTH, ERR_MSG_APPLY_ARITY);

            for (i = 0; i < n; i++)
                stack_push(clone_obj(x[i]));

            res = call(f, n);
            for (i = 0; i < n; i++)
                drop_obj(stack_pop());
            return res;
        default:
            THROW_TYPE1("map", f->type);
    }
}

obj_p ray_do(obj_p *x, i64_t n) {
    i64_t i;
    obj_p res = NULL_OBJ;

    for (i = 0; i < n; i++) {
        drop_obj(res);
        res = eval(x[i]);
        if (IS_ERR(res))
            return res;
    }

    return res;
}

obj_p ray_gc(obj_p *x, i64_t n) {
    UNUSED(x);
    UNUSED(n);
    return i64(heap_gc());
}

obj_p ray_format(obj_p *x, i64_t n) { return obj_fmt_n(x, n); }

obj_p ray_print(obj_p *x, i64_t n) {
    obj_p s = obj_fmt_n(x, n);

    if (s == NULL_OBJ)
        return error_str(ERR_TYPE, ERR_MSG_MALFORMED_FMT);

    printf("%.*s", (i32_t)s->len, AS_C8(s));
    drop_obj(s);

    return NULL_OBJ;
}

obj_p ray_println(obj_p *x, i64_t n) {
    obj_p s = obj_fmt_n(x, n);

    if (s == NULL_OBJ)
        return error_str(ERR_TYPE, ERR_MSG_MALFORMED_FMT);

    printf("%.*s\n", (i32_t)s->len, AS_C8(s));
    drop_obj(s);

    return NULL_OBJ;
}

obj_p ray_args(obj_p *x, i64_t n) {
    UNUSED(x);
    UNUSED(n);
    return clone_obj(runtime_get()->args);
}

obj_p ray_set_splayed(obj_p *x, i64_t n) {
    switch (n) {
        case 2:
            return ray_set(x[0], x[1]);
        case 3:
            if (x[0]->type != TYPE_C8)
                THROW_S(ERR_TYPE, "set: table path must be a string");

            if (x[1]->type != TYPE_TABLE)
                THROW_S(ERR_TYPE, "set: table must be a table");

            if (x[0]->len < 2 || AS_C8(x[0])[x[0]->len - 1] != '/')
                THROW_S(ERR_TYPE, "set: table path must be a directory");

            return io_set_table_splayed(x[0], x[1], x[2]);
        default:
            THROW(ERR_LENGTH, "set splayed: expected 2, 3 arguments, got %lld", n);
    }
}

obj_p ray_get_splayed(obj_p *x, i64_t n) {
    switch (n) {
        case 1:
            return io_get_table_splayed(x[0], NULL_OBJ);
        case 2:
            return io_get_table_splayed(x[0], x[1]);
        default:
            THROW(ERR_LENGTH, "get splayed: expected 1 or 2 arguments, got %lld", n);
    }
}

obj_p ray_set_parted(obj_p *x, i64_t n) {
    switch (n) {
        case 2:
            return ray_set(x[0], x[1]);
        default:
            THROW(ERR_LENGTH, "set parted: expected 2, 3 arguments, got %lld", n);
    }
}

obj_p ray_get_parted(obj_p *x, i64_t n) {
    i8_t type;
    i64_t i, j, l, wide;
    obj_p path, dir, sym, dirs, gcol, ord, t1, t2, eq, fmaps, virtcol, v, keys, vals, res;

    switch (n) {
        case 2:
            if (x[0]->type != TYPE_C8)
                THROW(ERR_TYPE, "get parted: expected string as 1st argument, got %s", type_name(x[0]->type));

            if (x[1]->type != -TYPE_SYMBOL)
                THROW(ERR_TYPE, "get parted: expected symbol as 2nd argument, got %s", type_name(x[1]->type));

            // Read directories structure
            path = cstring_from_obj(x[0]);
            dir = fs_read_dir(AS_C8(path));
            drop_obj(path);

            if (IS_ERR(dir))
                return dir;

            // Get grouping column (parted by)
            sym = string_from_str("sym", 3);
            dirs = ray_except(dir, sym);
            drop_obj(sym);
            drop_obj(dir);

            if (IS_ERR(dirs))
                return dirs;

            // Try to convert dirs to a parted column (one of numeric datatypes)
            res = cast_obj(TYPE_DATE, dirs);

            if (IS_ERR(res)) {
                drop_obj(dirs);
                return res;
            }

            // TODO: Sort parted dirs in an ascending order
            v = cast_obj(TYPE_I64, res);
            ord = ray_iasc(v);
            drop_obj(v);

            if (IS_ERR(ord)) {
                drop_obj(res);
                drop_obj(dirs);
                return ord;
            }

            gcol = ray_at(res, ord);
            drop_obj(res);

            res = ray_at(dirs, ord);
            drop_obj(ord);
            drop_obj(dirs);

            // Trverse dirs for requested table
            l = res->len;

            if (l == 0) {
                drop_obj(gcol);
                drop_obj(res);
                THROW_S(ERR_LENGTH, "get parted: empty directory");
            }

            // Load schema of the first partition
            path = str_fmt(-1, "%.*s%.*s/%s/", (i32_t)x[0]->len, AS_C8(x[0]), (i32_t)AS_LIST(res)[0]->len,
                           AS_C8(AS_LIST(res)[0]), str_from_symbol(x[1]->i64));

            t1 = io_get_table_splayed(path, NULL_OBJ);

            if (IS_ERR(t1)) {
                drop_obj(gcol);
                drop_obj(res);
                drop_obj(path);
                return t1;
            }

            wide = AS_LIST(t1)[1]->len;

            // Load symfile only if there are enum columns and sym is not already resolved
            if (resolve(SYMBOL_SYM) == NULL) {
                for (i = 0; i < wide; i++) {
                    if (AS_LIST(AS_LIST(t1)[1])[i]->type == TYPE_ENUM) {
                        v = io_get_symfile(x[0]);
                        if (IS_ERR(v)) {
                            drop_obj(gcol);
                            drop_obj(res);
                            drop_obj(t1);
                            drop_obj(path);
                            return v;
                        }
                        break;
                    }
                }
            }

            if (wide == 0) {
                drop_obj(gcol);
                drop_obj(res);
                drop_obj(t1);
                drop_obj(path);
                THROW_S(ERR_LENGTH, "get parted: partition may not have zero columns");
            }

            // Create maps over columns
            fmaps = LIST(wide);
            for (i = 0; i < wide; i++)
                AS_LIST(fmaps)[i] = LIST(0);

            // Create filemaps over columns of the 1st partition
            for (i = 0; i < wide; i++)
                push_obj(AS_LIST(fmaps) + i, clone_obj(AS_LIST(AS_LIST(t1)[1])[i]));

            drop_obj(path);

            // Check all the remaining partitions
            for (i = 1; i < l; i++) {
                path = str_fmt(-1, "%.*s%.*s/%s/", (i32_t)x[0]->len, AS_C8(x[0]), (i32_t)AS_LIST(res)[i]->len,
                               AS_C8(AS_LIST(res)[i]), str_from_symbol(x[1]->i64));

                t2 = io_get_table_splayed(path, NULL_OBJ);

                if (IS_ERR(t2)) {
                    drop_obj(gcol);
                    drop_obj(res);
                    drop_obj(t1);
                    drop_obj(path);
                    drop_obj(fmaps);
                    return t2;
                }

                // Partitions must have the same length
                if (wide != AS_LIST(t2)[1]->len) {
                    drop_obj(gcol);
                    drop_obj(res);
                    drop_obj(t1);
                    drop_obj(t2);
                    drop_obj(path);
                    drop_obj(fmaps);
                    THROW_S(ERR_LENGTH, "get parted: partitions have different wides");
                }

                // Partitions must have the same column names
                eq = ray_eq(AS_LIST(t1)[0], AS_LIST(t2)[0]);
                if (!eq->b8) {
                    drop_obj(eq);
                    drop_obj(gcol);
                    drop_obj(res);
                    drop_obj(t1);
                    drop_obj(t2);
                    drop_obj(path);
                    drop_obj(fmaps);
                    THROW_S(ERR_LENGTH, "get parted: partitions have different column names");
                }

                drop_obj(eq);

                // Partitions must have the same column types
                for (j = 0; j < wide; j++) {
                    if (AS_LIST(AS_LIST(t1)[1])[j]->type != AS_LIST(AS_LIST(t2)[1])[j]->type) {
                        drop_obj(gcol);
                        drop_obj(res);
                        drop_obj(t1);
                        drop_obj(t2);
                        drop_obj(path);
                        drop_obj(fmaps);
                        THROW_S(ERR_LENGTH, "get parted: partitions have different column types");
                    }
                }

                // Append maps over columns of the partition
                for (j = 0; j < wide; j++)
                    push_obj(AS_LIST(fmaps) + j, clone_obj(AS_LIST(AS_LIST(t2)[1])[j]));

                drop_obj(t2);
                drop_obj(path);
            }

            sym = (gcol->type == TYPE_DATE) ? symbol("Date", 4) : symbol("Id", 2);
            keys = ray_concat(sym, AS_LIST(t1)[0]);

            l = wide + 1;
            vals = LIST(l);

            // Create a virtual column for the grouping column
            l = gcol->len;
            virtcol = vn_list(2, vector(gcol->type, l), I64(l));
            virtcol->type = TYPE_MAPCOMMON;
            for (i = 0; i < l; i++) {
                n = ops_count(AS_LIST(AS_LIST(fmaps)[0])[i]);
                AS_DATE(AS_LIST(virtcol)[0])[i] = AS_DATE(gcol)[i];
                AS_I64(AS_LIST(virtcol)[1])[i] = n;
            }

            AS_LIST(vals)[0] = virtcol;
            for (i = 0; i < wide; i++) {
                AS_LIST(vals)[i + 1] = clone_obj(AS_LIST(fmaps)[i]);

                type = AS_LIST(AS_LIST(t1)[1])[i]->type;
                if ((type >= TYPE_B8 && type <= TYPE_GUID) || type == TYPE_ENUM)
                    type += TYPE_PARTEDLIST;
                else
                    type = TYPE_PARTEDLIST;

                AS_LIST(vals)[i + 1]->type = type;
            }

            drop_obj(sym);
            drop_obj(res);
            drop_obj(t1);
            drop_obj(gcol);
            drop_obj(fmaps);

            return table(keys, vals);

        default:
            THROW(ERR_LENGTH, "get parted: expected 2 arguments, got %lld", n);
    }
}