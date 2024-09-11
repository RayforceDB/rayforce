/*
 *   Copyright (c) 2023 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, modlicense, and/or sell
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

#include <math.h>
#include <time.h>
#include "math.h"
#include "util.h"
#include "heap.h"
#include "ops.h"
#include "error.h"
#include "aggr.h"
#include "pool.h"
#include "sort.h"
#include "order.h"

obj_p ray_add(obj_p x, obj_p y) {
    u64_t i, l = 0;
    obj_p vec;
    i64_t *xivals = NULL, *yivals = NULL, *iout;
    f64_t *xfvals = NULL, *yfvals = NULL, *fout;

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(-TYPE_I64, -TYPE_I64):
            return i64(ADDI64(x->i64, y->i64));
        case MTYPE2(-TYPE_I64, -TYPE_F64):
            return f64(ADDF64(x->i64, y->f64));
        case MTYPE2(-TYPE_F64, -TYPE_F64):
            return f64(ADDF64(x->f64, y->f64));
        case MTYPE2(-TYPE_F64, -TYPE_I64):
            return f64(ADDF64(x->f64, (f64_t)y->i64));
        case MTYPE2(-TYPE_TIMESTAMP, -TYPE_I64):
            return timestamp(ADDI64(x->i64, y->i64));
        case MTYPE2(-TYPE_I64, TYPE_I64):
            l = y->len;
            yivals = AS_I64(y);
            vec = I64(l);
            iout = AS_I64(vec);
            for (i = 0; i < l; i++)
                iout[i] = ADDI64(x->i64, yivals[i]);

            return vec;
        case MTYPE2(-TYPE_I64, TYPE_F64):
            l = y->len;
            yfvals = AS_F64(y);
            vec = F64(l);
            fout = AS_F64(vec);
            for (i = 0; i < l; i++)
                fout[i] = ADDF64((f64_t)x->i64, yfvals[i]);

            return vec;
        case MTYPE2(-TYPE_F64, TYPE_F64):
            l = y->len;
            yfvals = AS_F64(y);
            vec = F64(l);
            fout = AS_F64(vec);
            for (i = 0; i < l; i++)
                fout[i] = ADDF64(x->f64, yfvals[i]);

            return vec;
        case MTYPE2(-TYPE_F64, TYPE_I64):
            l = y->len;
            yivals = AS_I64(y);
            vec = F64(l);
            fout = AS_F64(vec);
            for (i = 0; i < l; i++)
                fout[i] = ADDF64(x->f64, (f64_t)yivals[i]);

            return vec;
        case MTYPE2(TYPE_I64, -TYPE_I64):
            l = x->len;
            xivals = AS_I64(x);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = ADDI64(xivals[i], y->i64);

            return vec;
        case MTYPE2(TYPE_I64, -TYPE_F64):
            l = x->len;
            xivals = AS_I64(x);
            vec = F64(l);
            fout = AS_F64(vec);
            for (i = 0; i < l; i++)
                fout[i] = ADDF64((f64_t)xivals[i], y->f64);

            return vec;
        case MTYPE2(TYPE_I64, TYPE_I64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "add: vectors must be of the same length");

            xivals = AS_I64(x);
            yivals = AS_I64(y);
            vec = I64(l);
            iout = AS_I64(vec);
            for (i = 0; i < l; i++)
                iout[i] = ADDI64(xivals[i], yivals[i]);

            return vec;
        case MTYPE2(TYPE_I64, TYPE_F64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "add: vectors must be of the same length");

            xivals = AS_I64(x);
            yfvals = AS_F64(y);
            vec = F64(l);
            fout = AS_F64(vec);
            for (i = 0; i < l; i++)
                fout[i] = ADDF64((f64_t)xivals[i], yfvals[i]);

            return vec;
        case MTYPE2(TYPE_F64, -TYPE_F64):
            l = x->len;
            xfvals = AS_F64(x);
            vec = F64(l);
            fout = AS_F64(vec);
            for (i = 0; i < l; i++)
                fout[i] = ADDF64(xfvals[i], y->f64);

            return vec;
        case MTYPE2(TYPE_F64, -TYPE_I64):
            l = x->len;
            xfvals = AS_F64(x);
            vec = F64(l);
            fout = AS_F64(vec);
            for (i = 0; i < l; i++)
                fout[i] = ADDF64(xfvals[i], (f64_t)y->i64);

            return vec;
        case MTYPE2(TYPE_F64, TYPE_F64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "add: vectors must be of the same length");

            xfvals = AS_F64(x);
            yfvals = AS_F64(y);
            vec = F64(l);
            fout = AS_F64(vec);
            for (i = 0; i < l; i++)
                fout[i] = ADDF64(xfvals[i], yfvals[i]);

            return vec;
        case MTYPE2(TYPE_F64, TYPE_I64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "add: vectors must be of the same length");

            xfvals = AS_F64(x);
            yivals = AS_I64(y);
            vec = F64(l);
            fout = AS_F64(vec);
            for (i = 0; i < l; i++)
                fout[i] = ADDF64(xfvals[i], (f64_t)yivals[i]);

            return vec;
        default:
            THROW(ERR_TYPE, "add: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_sub(obj_p x, obj_p y) {
    u64_t i, l = 0;
    obj_p vec;
    i64_t *xivals = NULL, *yivals = NULL;
    f64_t *xfvals = NULL, *yfvals = NULL;

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(-TYPE_I64, -TYPE_I64):
            return i64(SUBI64(x->i64, y->i64));
        case MTYPE2(-TYPE_I64, -TYPE_F64):
            return i64(SUBI64(x->i64, (i64_t)y->f64));
        case MTYPE2(-TYPE_F64, -TYPE_F64):
            return f64(SUBF64(x->f64, y->f64));
        case MTYPE2(-TYPE_F64, -TYPE_I64):
            return f64(SUBF64(x->f64, (f64_t)y->i64));
        case MTYPE2(-TYPE_TIMESTAMP, -TYPE_I64):
            return timestamp(SUBI64(x->i64, y->i64));
        case MTYPE2(-TYPE_I64, TYPE_I64):
            l = y->len;
            yivals = AS_I64(y);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = SUBI64(x->i64, yivals[i]);

            return vec;
        case MTYPE2(-TYPE_I64, TYPE_F64):
            l = y->len;
            yfvals = AS_F64(y);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = SUBF64((f64_t)x->i64, yfvals[i]);

            return vec;
        case MTYPE2(-TYPE_F64, TYPE_F64):
            l = y->len;
            yfvals = AS_F64(y);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = SUBF64(x->f64, yfvals[i]);

            return vec;
        case MTYPE2(-TYPE_F64, TYPE_I64):
            l = y->len;
            yivals = AS_I64(y);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = SUBF64(x->f64, (f64_t)yivals[i]);

            return vec;
        case MTYPE2(TYPE_I64, -TYPE_I64):
            l = x->len;
            xivals = AS_I64(x);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = SUBI64(xivals[i], y->i64);

            return vec;
        case MTYPE2(TYPE_I64, -TYPE_F64):
            l = x->len;
            xivals = AS_I64(x);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = SUBF64((f64_t)xivals[i], y->f64);

            return vec;
        case MTYPE2(TYPE_I64, TYPE_I64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "sub: vectors must be of the same length");

            xivals = AS_I64(x);
            yivals = AS_I64(y);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = SUBI64(xivals[i], yivals[i]);

            return vec;
        case MTYPE2(TYPE_I64, TYPE_F64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "sub: vectors must be of the same length");

            xivals = AS_I64(x);
            yivals = AS_I64(y);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = SUBI64(xivals[i], yivals[i]);

            return vec;
        case MTYPE2(TYPE_F64, -TYPE_F64):
            l = x->len;
            xfvals = AS_F64(x);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = SUBF64(xfvals[i], y->f64);

            return vec;
        case MTYPE2(TYPE_F64, -TYPE_I64):
            l = x->len;
            xfvals = AS_F64(x);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = SUBF64(xfvals[i], (f64_t)y->i64);

            return vec;
        case MTYPE2(TYPE_F64, TYPE_F64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "sub: vectors must be of the same length");

            xfvals = AS_F64(x);
            yfvals = AS_F64(y);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = SUBF64(xfvals[i], yfvals[i]);

            return vec;
        case MTYPE2(TYPE_F64, TYPE_I64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "sub: vectors must be of the same length");

            xfvals = AS_F64(x);
            yivals = AS_I64(y);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = SUBF64(xfvals[i], (f64_t)yivals[i]);

            return vec;
        default:
            THROW(ERR_TYPE, "sub: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_mul(obj_p x, obj_p y) {
    u64_t i, l = 0;
    obj_p vec;
    i64_t *xivals = NULL, *yivals = NULL;
    f64_t *xfvals = NULL, *yfvals = NULL;

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(-TYPE_I64, -TYPE_I64):
            return i64(MULI64(x->i64, y->i64));
        case MTYPE2(-TYPE_I64, -TYPE_F64):
            return i64(MULI64(x->i64, (i64_t)y->f64));
        case MTYPE2(-TYPE_F64, -TYPE_F64):
            return f64(SUBF64(x->f64, y->f64));
        case MTYPE2(-TYPE_F64, -TYPE_I64):
            return f64(MULF64(x->f64, (f64_t)y->i64));
        case MTYPE2(-TYPE_I64, TYPE_I64):
            l = y->len;
            yivals = AS_I64(y);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = MULI64(x->i64, yivals[i]);

            return vec;
        case MTYPE2(-TYPE_I64, TYPE_F64):
            l = y->len;
            yfvals = AS_F64(y);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = MULF64((f64_t)x->i64, yfvals[i]);

            return vec;
        case MTYPE2(-TYPE_F64, TYPE_F64):
            l = y->len;
            yfvals = AS_F64(y);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = MULF64(x->f64, yfvals[i]);

            return vec;
        case MTYPE2(-TYPE_F64, TYPE_I64):
            l = y->len;
            yivals = AS_I64(y);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = MULF64(x->f64, (f64_t)yivals[i]);

            return vec;
        case MTYPE2(TYPE_I64, -TYPE_I64):
            l = x->len;
            xivals = AS_I64(x);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = MULI64(xivals[i], y->i64);

            return vec;
        case MTYPE2(TYPE_I64, -TYPE_F64):
            l = x->len;
            xivals = AS_I64(x);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = MULF64((f64_t)xivals[i], y->f64);

            return vec;
        case MTYPE2(TYPE_I64, TYPE_I64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "mul: vectors must be of the same length");

            xivals = AS_I64(x);
            yivals = AS_I64(y);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = MULI64(xivals[i], yivals[i]);

            return vec;
        case MTYPE2(TYPE_I64, TYPE_F64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "mul: vectors must be of the same length");

            xivals = AS_I64(x);
            yivals = AS_I64(y);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = MULI64(xivals[i], yivals[i]);

            return vec;
        case MTYPE2(TYPE_F64, -TYPE_F64):
            l = x->len;
            xfvals = AS_F64(x);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = MULF64(xfvals[i], y->f64);

            return vec;
        case MTYPE2(TYPE_F64, -TYPE_I64):
            l = x->len;
            xfvals = AS_F64(x);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = MULF64(xfvals[i], (f64_t)y->i64);

            return vec;
        case MTYPE2(TYPE_F64, TYPE_F64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "mul: vectors must be of the same length");

            xfvals = AS_F64(x);
            yfvals = AS_F64(y);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = MULF64(xfvals[i], yfvals[i]);

            return vec;
        case MTYPE2(TYPE_F64, TYPE_I64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "mul: vectors must be of the same length");

            xfvals = AS_F64(x);
            yivals = AS_I64(y);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = MULF64(xfvals[i], (f64_t)yivals[i]);

            return vec;
        default:
            THROW(ERR_TYPE, "mul: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_div(obj_p x, obj_p y) {
    u64_t i, l = 0;
    obj_p vec;
    i64_t *xivals = NULL, *yivals = NULL;
    f64_t *xfvals = NULL, *yfvals = NULL;

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(-TYPE_I64, -TYPE_I64):
            return i64(DIVI64(x->i64, y->i64));
        case MTYPE2(-TYPE_I64, -TYPE_F64):
            return i64(DIVI64(x->i64, (i64_t)y->f64));
        case MTYPE2(-TYPE_F64, -TYPE_F64):
            return f64(DIVF64(x->f64, y->f64));
        case MTYPE2(-TYPE_F64, -TYPE_I64):
            return f64(DIVF64(x->f64, (f64_t)y->i64));
        case MTYPE2(-TYPE_I64, TYPE_I64):
            l = y->len;
            yivals = AS_I64(y);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = DIVI64(x->i64, yivals[i]);

            return vec;
        case MTYPE2(-TYPE_I64, TYPE_F64):
            l = y->len;
            yfvals = AS_F64(y);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = DIVF64((f64_t)x->i64, yfvals[i]);

            return vec;
        case MTYPE2(-TYPE_F64, TYPE_F64):
            l = y->len;
            yfvals = AS_F64(y);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = DIVF64(x->f64, yfvals[i]);

            return vec;
        case MTYPE2(-TYPE_F64, TYPE_I64):
            l = y->len;
            yivals = AS_I64(y);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = DIVF64(x->f64, (f64_t)yivals[i]);

            return vec;
        case MTYPE2(TYPE_I64, -TYPE_I64):
            l = x->len;
            xivals = AS_I64(x);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = DIVI64(xivals[i], y->i64);

            return vec;
        case MTYPE2(TYPE_I64, -TYPE_F64):
            l = x->len;
            xivals = AS_I64(x);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = DIVF64((f64_t)xivals[i], y->f64);

            return vec;
        case MTYPE2(TYPE_I64, TYPE_I64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "div: vectors must be of the same length");

            xivals = AS_I64(x);
            yivals = AS_I64(y);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = DIVI64(xivals[i], yivals[i]);

            return vec;
        case MTYPE2(TYPE_I64, TYPE_F64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "div: vectors must be of the same length");

            xivals = AS_I64(x);
            yivals = AS_I64(y);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = DIVI64(xivals[i], yivals[i]);

            return vec;
        case MTYPE2(TYPE_F64, -TYPE_F64):
            l = x->len;
            xfvals = AS_F64(x);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = DIVF64(xfvals[i], y->f64);

            return vec;
        case MTYPE2(TYPE_F64, -TYPE_I64):
            l = x->len;
            xfvals = AS_F64(x);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = DIVF64(xfvals[i], (f64_t)y->i64);

            return vec;
        case MTYPE2(TYPE_F64, TYPE_F64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "div: vectors must be of the same length");

            xfvals = AS_F64(x);
            yfvals = AS_F64(y);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = DIVF64(xfvals[i], yfvals[i]);

            return vec;
        case MTYPE2(TYPE_F64, TYPE_I64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "div: vectors must be of the same length");

            xfvals = AS_F64(x);
            yivals = AS_I64(y);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = DIVF64(xfvals[i], (f64_t)yivals[i]);

            return vec;
        default:
            THROW(ERR_TYPE, "div: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_fdiv(obj_p x, obj_p y) {
    u64_t i, l = 0;
    obj_p vec;
    i64_t *xivals = NULL, *yivals = NULL;
    f64_t *xfvals = NULL, *yfvals = NULL;

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(-TYPE_I64, -TYPE_I64):
            return f64(FDIVI64(x->i64, (f64_t)y->i64));
        case MTYPE2(-TYPE_I64, -TYPE_F64):
            return f64(FDIVI64(x->i64, y->f64));
        case MTYPE2(-TYPE_F64, -TYPE_F64):
            return f64(FDIVF64(x->f64, y->f64));
        case MTYPE2(-TYPE_F64, -TYPE_I64):
            return f64(FDIVI64(x->f64, y->i64));
        case MTYPE2(-TYPE_I64, TYPE_I64):
            l = y->len;
            yivals = AS_I64(y);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = FDIVI64(x->i64, yivals[i]);

            return vec;
        case MTYPE2(-TYPE_I64, TYPE_F64):
            l = y->len;
            yfvals = AS_F64(y);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = FDIVI64(x->i64, yfvals[i]);

            return vec;
        case MTYPE2(-TYPE_F64, TYPE_F64):
            l = y->len;
            yfvals = AS_F64(y);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = FDIVF64(x->f64, yfvals[i]);

            return vec;
        case MTYPE2(-TYPE_F64, TYPE_I64):
            l = y->len;
            yivals = AS_I64(y);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = FDIVI64(x->f64, yivals[i]);

            return vec;
        case MTYPE2(TYPE_I64, -TYPE_I64):
            l = x->len;
            xivals = AS_I64(x);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = FDIVI64(xivals[i], y->i64);

            return vec;
        case MTYPE2(TYPE_I64, -TYPE_F64):
            l = x->len;
            xivals = AS_I64(x);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = FDIVI64(xivals[i], y->f64);

            return vec;
        case MTYPE2(TYPE_I64, TYPE_I64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "fdiv: vectors must be of the same length");

            xivals = AS_I64(x);
            yivals = AS_I64(y);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = FDIVI64(xivals[i], yivals[i]);

            return vec;
        case MTYPE2(TYPE_I64, TYPE_F64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "fdiv: vectors must be of the same length");

            xivals = AS_I64(x);
            yivals = AS_I64(y);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = FDIVI64(xivals[i], yivals[i]);

            return vec;
        case MTYPE2(TYPE_F64, -TYPE_F64):
            l = x->len;
            xfvals = AS_F64(x);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = FDIVF64(xfvals[i], y->f64);

            return vec;
        case MTYPE2(TYPE_F64, -TYPE_I64):
            l = x->len;
            xfvals = AS_F64(x);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = FDIVI64(xfvals[i], y->i64);

            return vec;
        case MTYPE2(TYPE_F64, TYPE_F64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "fdiv: vectors must be of the same length");

            xfvals = AS_F64(x);
            yfvals = AS_F64(y);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = FDIVF64(xfvals[i], yfvals[i]);

            return vec;
        case MTYPE2(TYPE_F64, TYPE_I64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "fdiv: vectors must be of the same length");

            xfvals = AS_F64(x);
            yivals = AS_I64(y);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = FDIVI64(xfvals[i], yivals[i]);

            return vec;
        default:
            THROW(ERR_TYPE, "add: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_mod(obj_p x, obj_p y) {
    u64_t i, l = 0;
    obj_p vec;
    i64_t *xivals = NULL, *yivals = NULL;
    f64_t *xfvals = NULL, *yfvals = NULL;

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(-TYPE_I64, -TYPE_I64):
            return i64(MODI64(x->i64, y->i64));
        case MTYPE2(-TYPE_I64, -TYPE_F64):
            return i64(MODI64(x->i64, (i64_t)y->f64));
        case MTYPE2(-TYPE_F64, -TYPE_F64):
            return i64(MODF64(x->f64, y->f64));
        case MTYPE2(-TYPE_F64, -TYPE_I64):
            return i64(MODF64(x->f64, (f64_t)y->i64));
        case MTYPE2(-TYPE_I64, TYPE_I64):
            l = y->len;
            yivals = AS_I64(y);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = MODI64(x->i64, yivals[i]);

            return vec;
        case MTYPE2(-TYPE_I64, TYPE_F64):
            l = y->len;
            yfvals = AS_F64(y);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = MODF64((f64_t)x->i64, yfvals[i]);

            return vec;
        case MTYPE2(-TYPE_F64, TYPE_F64):
            l = y->len;
            yfvals = AS_F64(y);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = MODF64(x->f64, yfvals[i]);

            return vec;
        case MTYPE2(-TYPE_F64, TYPE_I64):
            l = y->len;
            yivals = AS_I64(y);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = MODF64(x->f64, (f64_t)yivals[i]);

            return vec;
        case MTYPE2(TYPE_I64, -TYPE_I64):
            l = x->len;
            xivals = AS_I64(x);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = MODI64(xivals[i], y->i64);

            return vec;
        case MTYPE2(TYPE_I64, -TYPE_F64):
            l = x->len;
            xivals = AS_I64(x);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = MODF64((f64_t)xivals[i], y->f64);

            return vec;
        case MTYPE2(TYPE_I64, TYPE_I64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "mod: vectors must be of the same length");

            xivals = AS_I64(x);
            yivals = AS_I64(y);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = MODI64(xivals[i], yivals[i]);

            return vec;
        case MTYPE2(TYPE_I64, TYPE_F64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "mod: vectors must be of the same length");

            xivals = AS_I64(x);
            yivals = AS_I64(y);
            vec = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(vec)
            [i] = MODI64(xivals[i], yivals[i]);

            return vec;
        case MTYPE2(TYPE_F64, -TYPE_F64):
            l = x->len;
            xfvals = AS_F64(x);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = MODF64(xfvals[i], y->f64);

            return vec;
        case MTYPE2(TYPE_F64, -TYPE_I64):
            l = x->len;
            xfvals = AS_F64(x);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = MODI64(xfvals[i], y->i64);

            return vec;
        case MTYPE2(TYPE_F64, TYPE_F64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "mod: vectors must be of the same length");

            xfvals = AS_F64(x);
            yfvals = AS_F64(y);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = MODF64(xfvals[i], yfvals[i]);

            return vec;
        case MTYPE2(TYPE_F64, TYPE_I64):
            l = x->len;
            if (l != y->len)
                THROW(ERR_LENGTH, "mod: vectors must be of the same length");

            xfvals = AS_F64(x);
            yivals = AS_I64(y);
            vec = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(vec)
            [i] = MODI64(xfvals[i], yivals[i]);

            return vec;
        default:
            THROW(ERR_TYPE, "add: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_sum_partial(u64_t len, i64_t input[]) {
    u64_t i;
    i64_t isum;

    for (i = 0, isum = 0; i < len; i++)
        isum += (input[i] == NULL_I64) ? 0 : input[i];

    return i64(isum);
}

obj_p ray_sum(obj_p x) {
    u64_t i, l, chunks, chunk;
    i64_t isum, *xii;
    f64_t fsum, *xfi;
    pool_p pool;
    obj_p res;

    switch (x->type) {
        case -TYPE_I64:
            return clone_obj(x);
        case -TYPE_F64:
            return clone_obj(x);
        case TYPE_I64:
            l = x->len;
            xii = AS_I64(x);

            pool = pool_get();
            chunks = pool_split_by(pool, l, 0);

            if (chunks == 1) {
                for (i = 0, isum = 0; i < l; i++)
                    isum += (xii[i] == NULL_I64) ? 0 : xii[i];

                return i64(isum);
            }

            pool_prepare(pool);
            chunk = l / chunks;

            for (i = 0; i < chunks - 1; i++)
                pool_add_task(pool, (raw_p)ray_sum_partial, 2, chunk, xii + i * chunk);

            pool_add_task(pool, (raw_p)ray_sum_partial, 2, l - i * chunk, xii + i * chunk);

            res = pool_run(pool);

            for (i = 0, isum = 0; i < chunks; i++)
                isum += AS_LIST(res)[i]->i64;

            drop_obj(res);

            return i64(isum);

        case TYPE_F64:
            l = x->len;
            xfi = AS_F64(x);
            for (i = 0, fsum = 0; i < l; i++)
                fsum += xfi[i];

            return f64(fsum);
        case TYPE_GROUPMAP:
            return aggr_sum(AS_LIST(x)[0], AS_LIST(x)[1]);

        default:
            THROW(ERR_TYPE, "sum: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_avg(obj_p x) {
    u64_t i, l = 0, n;
    i64_t isum = 0, *xivals = NULL;
    f64_t fsum = 0.0, *xfvals = NULL;

    switch (x->type) {
        case -TYPE_I64:
        case -TYPE_F64:
            return clone_obj(x);
        case TYPE_I64:
            l = x->len;
            xivals = AS_I64(x);

            for (i = 0, n = 0; i < l; i++) {
                if (xivals[i] != NULL_I64)
                    isum += xivals[i];
                else
                    n++;
            }

            return f64((f64_t)isum / (l - n));

        case TYPE_F64:
            l = x->len;
            xfvals = AS_F64(x);

            for (i = 0, n = 0; i < l; i++)
                fsum += xfvals[i];

            return f64(fsum / l);

        case TYPE_GROUPMAP:
            return aggr_avg(AS_LIST(x)[0], AS_LIST(x)[1]);

        default:
            THROW(ERR_TYPE, "avg: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_min(obj_p x) {
    u64_t i, l = 0;
    i64_t imin, iv, *xivals = NULL;
    f64_t fmin = 0.0, *xfvals = NULL;
    obj_p res;

    switch (x->type) {
        case TYPE_I64:
        case TYPE_TIMESTAMP:
            l = x->len;

            if (!l)
                return i64(NULL_I64);

            xivals = AS_I64(x);
            imin = xivals[0];

            xivals = AS_I64(x);
            imin = xivals[0];

            for (i = 0; i < l; i++) {
                iv = (xivals[i] == NULL_I64) ? MAX_I64 : xivals[i];
                imin = iv < imin ? iv : imin;
            }

            res = i64(imin);
            res->type = -x->type;

            return res;

        case TYPE_F64:
            l = x->len;

            if (!l)
                return f64(NULL_F64);

            xfvals = AS_F64(x);
            fmin = xfvals[0];

            for (i = 0; i < l; i++)
                fmin = xfvals[i] < fmin ? xfvals[i] : fmin;

            return f64(fmin);

        case TYPE_GROUPMAP:
            return aggr_min(AS_LIST(x)[0], AS_LIST(x)[1]);

        default:
            THROW(ERR_TYPE, "min: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_max(obj_p x) {
    u64_t i, l = 0;
    i64_t imax = 0, *xivals = NULL;
    f64_t fmax = 0.0, *xfvals = NULL;
    obj_p res;

    switch (x->type) {
        case TYPE_I64:
        case TYPE_TIMESTAMP:
            l = x->len;

            if (!l)
                return i64(NULL_I64);

            xivals = AS_I64(x);
            imax = xivals[0];

            for (i = 0; i < l; i++)
                imax = xivals[i] > imax ? xivals[i] : imax;

            res = i64(imax);
            res->type = -x->type;

            return res;

        case TYPE_F64:
            l = x->len;

            if (!l)
                return f64(NULL_F64);

            xfvals = AS_F64(x);
            fmax = xfvals[0];

            for (i = 0; i < l; i++)
                fmax = xfvals[i] > fmax ? xfvals[i] : fmax;

            return f64(fmax);

        case TYPE_GROUPMAP:
            return aggr_max(AS_LIST(x)[0], AS_LIST(x)[1]);

        default:
            THROW(ERR_TYPE, "max: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_dev(obj_p x) {
    u64_t i, l = 0;
    f64_t fsum = 0.0, favg = 0.0, *xfvals = NULL;
    obj_p res;

    switch (x->type) {
        case TYPE_I64:
        case TYPE_TIMESTAMP:
            l = x->len;

            if (!l)
                return f64(NULL_F64);

            xfvals = AS_F64(x);
            fsum = xfvals[0];

            for (i = 0; i < l; i++)
                fsum += xfvals[i];

            favg = fsum / l;

            fsum = 0.0;
            for (i = 0; i < l; i++)
                fsum += pow(xfvals[i] - favg, 2);

            res = f64(sqrt(fsum / l));

            return res;

        case TYPE_F64:
            l = x->len;

            if (!l)
                return f64(NULL_F64);

            xfvals = AS_F64(x);
            fsum = xfvals[0];

            for (i = 0; i < l; i++)
                fsum += xfvals[i];

            favg = fsum / l;

            fsum = 0.0;
            for (i = 0; i < l; i++)
                fsum += pow(xfvals[i] - favg, 2);

            res = f64(sqrt(fsum / l));

            return res;

        case TYPE_GROUPMAP:
            return aggr_dev(AS_LIST(x)[0], AS_LIST(x)[1]);

        default:
            THROW(ERR_TYPE, "dev: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_med(obj_p x) {
    u64_t l;
    i64_t *xisort;
    f64_t *xfsort, med;
    obj_p sort;

    switch (x->type) {
        case TYPE_I64:
            l = x->len;

            if (l == 0)
                return null(TYPE_F64);

            sort = ray_asc(x);
            xisort = AS_I64(sort);
            med = (l % 2 == 0) ? (xisort[l / 2 - 1] + xisort[l / 2]) / 2.0 : xisort[l / 2];
            drop_obj(sort);

            return f64(med);

        case TYPE_F64:
            l = x->len;

            if (l == 0)
                return null(TYPE_F64);

            sort = ray_asc(x);
            xfsort = AS_F64(sort);
            med = (l % 2 == 0) ? (xfsort[l / 2 - 1] + xfsort[l / 2]) / 2.0 : xfsort[l / 2];
            drop_obj(sort);

            return f64(med);

        case TYPE_GROUPMAP:
            return aggr_med(AS_LIST(x)[0], AS_LIST(x)[1]);

        default:
            THROW(ERR_TYPE, "med: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_round(obj_p x) {
    u64_t i, l = 0;
    i64_t *rvals;
    f64_t *xfvals;
    obj_p res;

    switch (x->type) {
        case -TYPE_F64:
            return i64(ROUNDF64(x->f64));
        case TYPE_F64:
            l = x->len;

            res = F64(l);
            rvals = AS_I64(res);
            xfvals = AS_F64(x);

            for (i = 0; i < l; i++)
                rvals[i] = ROUNDF64(xfvals[i]);

            return res;
        default:
            THROW(ERR_TYPE, "round: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_floor(obj_p x) {
    u64_t i, l = 0;
    i64_t *rvals;
    f64_t *xfvals;
    obj_p res;

    switch (x->type) {
        case -TYPE_F64:
            return i64(FLOORF64(x->f64));
        case TYPE_F64:
            l = x->len;

            res = F64(l);
            rvals = AS_I64(res);
            xfvals = AS_F64(x);

            for (i = 0; i < l; i++)
                rvals[i] = FLOORF64(xfvals[i]);

            return res;
        default:
            THROW(ERR_TYPE, "floor: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_ceil(obj_p x) {
    u64_t i, l = 0;
    i64_t *rvals;
    f64_t *xfvals;
    obj_p res;

    switch (x->type) {
        case -TYPE_F64:
            return i64(CEILF64(x->f64));
        case TYPE_F64:
            l = x->len;

            res = F64(l);
            rvals = AS_I64(res);
            xfvals = AS_F64(x);

            for (i = 0; i < l; i++)
                rvals[i] = CEILF64(xfvals[i]);

            return res;
        default:
            THROW(ERR_TYPE, "ceil: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_xbar(obj_p x, obj_p y) {
    u64_t i, l = 0;
    i64_t *xivals = NULL;
    f64_t *xfvals = NULL;
    obj_p res;

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(-TYPE_I64, -TYPE_I64):
        case MTYPE2(-TYPE_TIMESTAMP, -TYPE_I64):
            return i64(XBARI64(x->i64, y->i64));
        case MTYPE2(-TYPE_F64, -TYPE_I64):
            return f64(XBARF64(x->i64, y->f64));
        case MTYPE2(TYPE_I64, -TYPE_I64):
        case MTYPE2(TYPE_TIMESTAMP, -TYPE_I64):
            l = x->len;
            xivals = AS_I64(x);
            res = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(res)[i] = XBARI64(xivals[i], y->i64);

            return res;

        case MTYPE2(TYPE_F64, -TYPE_I64):
            l = x->len;
            xfvals = AS_F64(x);
            res = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(res)[i] = XBARF64(xfvals[i], y->i64);

            return res;

        default:
            THROW(ERR_TYPE, "xbar: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}
