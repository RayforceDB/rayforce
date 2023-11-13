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

#include "math.h"
#include "util.h"
#include "heap.h"
#include "ops.h"

obj_t ray_add(obj_t x, obj_t y)
{
    u64_t i, l = 0;
    obj_t vec;
    i64_t *xivals = NULL, *yivals = NULL, *iout;
    f64_t *xfvals = NULL, *yfvals = NULL, *fout;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return i64(addi64(x->i64, y->i64));
    case mtype2(-TYPE_I64, -TYPE_F64):
        return f64(addf64(x->i64, y->f64));
    case mtype2(-TYPE_F64, -TYPE_F64):
        return f64(addf64(x->f64, y->f64));
    case mtype2(-TYPE_F64, -TYPE_I64):
        return f64(addf64(x->f64, (f64_t)y->i64));
    case mtype2(-TYPE_TIMESTAMP, -TYPE_I64):
        return timestamp(addi64(x->i64, y->i64));
    case mtype2(-TYPE_I64, TYPE_I64):
        l = y->len;
        yivals = as_i64(y);
        vec = vector_i64(l);
        iout = as_i64(vec);
        for (i = 0; i < l; i++)
            iout[i] = addi64(x->i64, yivals[i]);

        return vec;
    case mtype2(-TYPE_I64, TYPE_F64):
        l = y->len;
        yfvals = as_f64(y);
        vec = vector_i64(l);
        fout = as_f64(vec);
        for (i = 0; i < l; i++)
            fout[i] = addf64((f64_t)x->i64, yfvals[i]);

        return vec;
    case mtype2(-TYPE_F64, TYPE_F64):
        l = y->len;
        yfvals = as_f64(y);
        vec = vector_f64(l);
        fout = as_f64(vec);
        for (i = 0; i < l; i++)
            fout[i] = addf64(x->f64, yfvals[i]);

        return vec;
    case mtype2(-TYPE_F64, TYPE_I64):
        l = y->len;
        yivals = as_i64(y);
        vec = vector_f64(l);
        fout = as_f64(vec);
        for (i = 0; i < l; i++)
            fout[i] = addf64(x->f64, (f64_t)yivals[i]);

        return vec;
    case mtype2(TYPE_I64, -TYPE_I64):
        l = x->len;
        xivals = as_i64(x);
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = addi64(xivals[i], y->i64);

        return vec;
    case mtype2(TYPE_I64, -TYPE_F64):
        l = x->len;
        xivals = as_i64(x);
        vec = vector_i64(l);
        iout = as_i64(vec);
        for (i = 0; i < l; i++)
            iout[i] = addf64((f64_t)xivals[i], y->f64);

        return vec;
    case mtype2(TYPE_I64, TYPE_I64):
        l = x->len;
        if (l != y->len)
            throw(ERR_LENGTH, "add: vectors must be of the same length");

        xivals = as_i64(x);
        yivals = as_i64(y);
        vec = vector_i64(l);
        iout = as_i64(vec);
        for (i = 0; i < l; i++)
            iout[i] = addi64(xivals[i], yivals[i]);

        return vec;
    case mtype2(TYPE_I64, TYPE_F64):
        l = x->len;
        if (l != y->len)
            throw(ERR_LENGTH, "add: vectors must be of the same length");

        xivals = as_i64(x);
        yivals = as_i64(y);
        vec = vector_i64(l);
        iout = as_i64(vec);
        for (i = 0; i < l; i++)
            iout[i] = addi64(xivals[i], yivals[i]);

        return vec;
    case mtype2(TYPE_F64, -TYPE_F64):
        l = x->len;
        xfvals = as_f64(x);
        vec = vector_f64(l);
        fout = as_f64(vec);
        for (i = 0; i < l; i++)
            fout[i] = addf64(xfvals[i], y->f64);

        return vec;
    case mtype2(TYPE_F64, -TYPE_I64):
        l = x->len;
        xfvals = as_f64(x);
        vec = vector_f64(l);
        fout = as_f64(vec);
        for (i = 0; i < l; i++)
            fout[i] = addf64(xfvals[i], (f64_t)y->i64);

        return vec;
    case mtype2(TYPE_F64, TYPE_F64):
        l = x->len;
        if (l != y->len)
            throw(ERR_LENGTH, "add: vectors must be of the same length");

        xfvals = as_f64(x);
        yfvals = as_f64(y);
        vec = vector_f64(l);
        fout = as_f64(vec);
        for (i = 0; i < l; i++)
            fout[i] = addf64(xfvals[i], yfvals[i]);

        return vec;
    case mtype2(TYPE_F64, TYPE_I64):
        l = x->len;
        if (l != y->len)
            throw(ERR_LENGTH, "add: vectors must be of the same length");

        xfvals = as_f64(x);
        yivals = as_i64(y);
        vec = vector_f64(l);
        fout = as_f64(vec);
        for (i = 0; i < l; i++)
            fout[i] = addf64(xfvals[i], (f64_t)yivals[i]);

        return vec;
    default:
        throw(ERR_TYPE, "add: unsupported types: '%s, '%s", typename(x->type), typename(y->type));
    }
}

obj_t ray_sub(obj_t x, obj_t y)
{
    u64_t i, l = 0;
    obj_t vec;
    i64_t *xids = NULL, *yids = NULL, *xivals = NULL, *yivals = NULL;
    f64_t *xfvals = NULL, *yfvals = NULL;

dispatch:
    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return i64(subi64(x->i64, y->i64));
    case mtype2(-TYPE_I64, -TYPE_F64):
        return i64(subi64(x->i64, (i64_t)y->f64));
    case mtype2(-TYPE_F64, -TYPE_F64):
        return f64(subf64(x->f64, y->f64));
    case mtype2(-TYPE_F64, -TYPE_I64):
        return f64(subf64(x->f64, (f64_t)y->i64));
    case mtype2(-TYPE_TIMESTAMP, -TYPE_I64):
        return timestamp(subi64(x->i64, y->i64));
    case mtype2(-TYPE_I64, TYPE_I64):
        if (yids)
        {
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = subi64(x->i64, yivals[yids[i]]);

            return vec;
        }

        l = y->len;
        yivals = as_i64(y);
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = subi64(x->i64, yivals[i]);

        return vec;
    case mtype2(-TYPE_I64, TYPE_F64):
        if (yids)
        {
            yfvals = as_f64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = subf64((f64_t)x->i64, yfvals[yids[i]]);

            return vec;
        }

        l = y->len;
        yfvals = as_f64(y);
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = subf64((f64_t)x->i64, yfvals[i]);

        return vec;
    case mtype2(-TYPE_F64, TYPE_F64):
        if (yids)
        {
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = subf64(x->f64, yfvals[yids[i]]);

            return vec;
        }

        l = y->len;
        yfvals = as_f64(y);
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = subf64(x->f64, yfvals[i]);

        return vec;
    case mtype2(-TYPE_F64, TYPE_I64):
        if (yids)
        {
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = subf64(x->f64, (f64_t)yivals[yids[i]]);

            return vec;
        }

        l = y->len;
        yivals = as_i64(y);
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = subf64(x->f64, (f64_t)yivals[i]);

        return vec;
    case mtype2(TYPE_I64, -TYPE_I64):
        if (xids)
        {
            xivals = as_i64(x);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = subi64(xivals[xids[i]], y->i64);

            return vec;
        }

        l = x->len;
        xivals = as_i64(x);
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = subi64(xivals[i], y->i64);

        return vec;
    case mtype2(TYPE_I64, -TYPE_F64):
        if (xids)
        {
            xivals = as_i64(x);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = subf64((f64_t)xivals[xids[i]], y->f64);

            return vec;
        }

        l = x->len;
        xivals = as_i64(x);
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = subf64((f64_t)xivals[i], y->f64);

        return vec;
    case mtype2(TYPE_I64, TYPE_I64):
        if (xids && yids)
        {
            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = subi64(xivals[xids[i]], yivals[yids[i]]);
        }
        else if (xids)
        {
            if (l != y->len)
                throw(ERR_LENGTH, "sub: vectors must be of the same length");

            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = subi64(xivals[xids[i]], yivals[i]);
        }
        else if (yids)
        {
            if (l != x->len)
                throw(ERR_LENGTH, "sub: vectors must be of the same length");

            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = subi64(xivals[i], yivals[yids[i]]);
        }
        else
        {
            l = x->len;
            if (l != y->len)
                throw(ERR_LENGTH, "sub: vectors must be of the same length");

            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = subi64(xivals[i], yivals[i]);
        }

        return vec;
    case mtype2(TYPE_I64, TYPE_F64):
        if (xids && yids)
        {
            xivals = as_i64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = subf64((f64_t)xivals[xids[i]], yfvals[yids[i]]);
        }
        else if (xids)
        {
            if (l != y->len)
                throw(ERR_LENGTH, "sub: vectors must be of the same length");

            xivals = as_i64(x);
            yfvals = as_f64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = subf64((f64_t)xivals[xids[i]], yfvals[i]);
        }
        else if (yids)
        {
            if (l != x->len)
                throw(ERR_LENGTH, "sub: vectors must be of the same length");

            xivals = as_i64(x);
            yfvals = as_f64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = subf64((f64_t)xivals[i], yfvals[yids[i]]);
        }
        else
        {
            l = x->len;
            if (l != y->len)
                throw(ERR_LENGTH, "sub: vectors must be of the same length");

            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = subi64(xivals[i], yivals[i]);
        }

        return vec;
    case mtype2(TYPE_F64, -TYPE_F64):
        if (xids)
        {
            xfvals = as_f64(x);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = subf64(xfvals[xids[i]], y->f64);

            return vec;
        }

        l = x->len;
        xfvals = as_f64(x);
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = subf64(xfvals[i], y->f64);

        return vec;
    case mtype2(TYPE_F64, -TYPE_I64):
        if (xids)
        {
            xfvals = as_f64(x);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = subf64(xfvals[xids[i]], (f64_t)y->i64);

            return vec;
        }

        l = x->len;
        xfvals = as_f64(x);
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = subf64(xfvals[i], (f64_t)y->i64);

        return vec;
    case mtype2(TYPE_F64, TYPE_F64):
        if (xids && yids)
        {
            xfvals = as_f64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = subf64(xfvals[xids[i]], yfvals[yids[i]]);
        }
        else if (xids)
        {
            if (l != y->len)
                throw(ERR_LENGTH, "sub: vectors must be of the same length");

            xfvals = as_f64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = subf64(xfvals[xids[i]], yfvals[i]);
        }
        else if (yids)
        {
            if (l != x->len)
                throw(ERR_LENGTH, "sub: vectors must be of the same length");

            xfvals = as_f64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = subf64(xfvals[i], yfvals[yids[i]]);
        }
        else
        {
            l = x->len;
            if (l != y->len)
                throw(ERR_LENGTH, "sub: vectors must be of the same length");

            xfvals = as_f64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = subf64(xfvals[i], yfvals[i]);
        }

        return vec;
    case mtype2(TYPE_F64, TYPE_I64):
        if (xids && yids)
        {
            xfvals = as_f64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = subf64(xfvals[xids[i]], (f64_t)yivals[yids[i]]);
        }
        else if (xids)
        {
            if (l != y->len)
                throw(ERR_LENGTH, "sub: vectors must be of the same length");

            xfvals = as_f64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = subf64(xfvals[xids[i]], (f64_t)yivals[i]);
        }
        else if (yids)
        {
            if (l != x->len)
                throw(ERR_LENGTH, "sub: vectors must be of the same length");

            xfvals = as_f64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = subf64(xfvals[i], (f64_t)yivals[yids[i]]);
        }
        else
        {
            l = x->len;
            if (l != y->len)
                throw(ERR_LENGTH, "sub: vectors must be of the same length");

            xfvals = as_f64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = subf64(xfvals[i], (f64_t)yivals[i]);
        }

        return vec;
    default:
        if ((x->type == TYPE_FILTERMAP) && (y->type == TYPE_FILTERMAP))
        {
            xids = as_i64(as_list(x)[1]);
            l = as_list(x)[1]->len;
            x = as_list(x)[0];

            if (l != as_list(y)[1]->len)
                throw(ERR_LENGTH, "sub: vectors must be of the same length");

            yids = as_i64(as_list(y)[1]);
            y = as_list(y)[0];

            goto dispatch;
        }
        else if (x->type == TYPE_FILTERMAP)
        {
            xids = as_i64(as_list(x)[1]);
            l = as_list(x)[1]->len;
            x = as_list(x)[0];
            goto dispatch;
        }
        else if (y->type == TYPE_FILTERMAP)
        {
            yids = as_i64(as_list(y)[1]);
            l = as_list(y)[1]->len;
            y = as_list(y)[0];
            goto dispatch;
        }

        throw(ERR_TYPE, "sub: unsupported types: '%s, '%s", typename(x->type), typename(y->type));
    }
}

obj_t ray_mul(obj_t x, obj_t y)
{
    u64_t i, l = 0;
    obj_t vec;
    i64_t *xids = NULL, *yids = NULL, *xivals = NULL, *yivals = NULL;
    f64_t *xfvals = NULL, *yfvals = NULL;

dispatch:
    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return i64(muli64(x->i64, y->i64));
    case mtype2(-TYPE_I64, -TYPE_F64):
        return i64(muli64(x->i64, (i64_t)y->f64));
    case mtype2(-TYPE_F64, -TYPE_F64):
        return f64(subf64(x->f64, y->f64));
    case mtype2(-TYPE_F64, -TYPE_I64):
        return f64(mulf64(x->f64, (f64_t)y->i64));
    case mtype2(-TYPE_I64, TYPE_I64):
        if (yids)
        {
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = muli64(x->i64, yivals[yids[i]]);

            return vec;
        }

        l = y->len;
        yivals = as_i64(y);
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = muli64(x->i64, yivals[i]);

        return vec;
    case mtype2(-TYPE_I64, TYPE_F64):
        if (yids)
        {
            yfvals = as_f64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = mulf64((f64_t)x->i64, yfvals[yids[i]]);

            return vec;
        }

        l = y->len;
        yfvals = as_f64(y);
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = mulf64((f64_t)x->i64, yfvals[i]);

        return vec;
    case mtype2(-TYPE_F64, TYPE_F64):
        if (yids)
        {
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = mulf64(x->f64, yfvals[yids[i]]);

            return vec;
        }

        l = y->len;
        yfvals = as_f64(y);
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = mulf64(x->f64, yfvals[i]);

        return vec;
    case mtype2(-TYPE_F64, TYPE_I64):
        if (yids)
        {
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = mulf64(x->f64, (f64_t)yivals[yids[i]]);

            return vec;
        }

        l = y->len;
        yivals = as_i64(y);
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = mulf64(x->f64, (f64_t)yivals[i]);

        return vec;
    case mtype2(TYPE_I64, -TYPE_I64):
        if (xids)
        {
            xivals = as_i64(x);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = muli64(xivals[xids[i]], y->i64);

            return vec;
        }

        l = x->len;
        xivals = as_i64(x);
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = muli64(xivals[i], y->i64);

        return vec;
    case mtype2(TYPE_I64, -TYPE_F64):
        if (xids)
        {
            xivals = as_i64(x);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = mulf64((f64_t)xivals[xids[i]], y->f64);

            return vec;
        }

        l = x->len;
        xivals = as_i64(x);
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = mulf64((f64_t)xivals[i], y->f64);

        return vec;
    case mtype2(TYPE_I64, TYPE_I64):
        if (xids && yids)
        {
            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = muli64(xivals[xids[i]], yivals[yids[i]]);
        }
        else if (xids)
        {
            if (l != y->len)
                throw(ERR_LENGTH, "mul: vectors must be of the same length");

            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = muli64(xivals[xids[i]], yivals[i]);
        }
        else if (yids)
        {
            if (l != x->len)
                throw(ERR_LENGTH, "mul: vectors must be of the same length");

            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = muli64(xivals[i], yivals[yids[i]]);
        }
        else
        {
            l = x->len;
            if (l != y->len)
                throw(ERR_LENGTH, "mul: vectors must be of the same length");

            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = muli64(xivals[i], yivals[i]);
        }

        return vec;
    case mtype2(TYPE_I64, TYPE_F64):
        if (xids && yids)
        {
            xivals = as_i64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = mulf64((f64_t)xivals[xids[i]], yfvals[yids[i]]);
        }
        else if (xids)
        {
            if (l != y->len)
                throw(ERR_LENGTH, "mul: vectors must be of the same length");

            xivals = as_i64(x);
            yfvals = as_f64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = mulf64((f64_t)xivals[xids[i]], yfvals[i]);
        }
        else if (yids)
        {
            if (l != x->len)
                throw(ERR_LENGTH, "mul: vectors must be of the same length");

            xivals = as_i64(x);
            yfvals = as_f64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = mulf64((f64_t)xivals[i], yfvals[yids[i]]);
        }
        else
        {
            l = x->len;
            if (l != y->len)
                throw(ERR_LENGTH, "mul: vectors must be of the same length");

            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = muli64(xivals[i], yivals[i]);
        }

        return vec;
    case mtype2(TYPE_F64, -TYPE_F64):
        if (xids)
        {
            xfvals = as_f64(x);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = mulf64(xfvals[xids[i]], y->f64);

            return vec;
        }

        l = x->len;
        xfvals = as_f64(x);
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = mulf64(xfvals[i], y->f64);

        return vec;
    case mtype2(TYPE_F64, -TYPE_I64):
        if (xids)
        {
            xfvals = as_f64(x);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = mulf64(xfvals[xids[i]], (f64_t)y->i64);

            return vec;
        }

        l = x->len;
        xfvals = as_f64(x);
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = mulf64(xfvals[i], (f64_t)y->i64);

        return vec;
    case mtype2(TYPE_F64, TYPE_F64):
        if (xids && yids)
        {
            xfvals = as_f64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = mulf64(xfvals[xids[i]], yfvals[yids[i]]);
        }
        else if (xids)
        {
            if (l != y->len)
                throw(ERR_LENGTH, "mul: vectors must be of the same length");

            xfvals = as_f64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = mulf64(xfvals[xids[i]], yfvals[i]);
        }
        else if (yids)
        {
            if (l != x->len)
                throw(ERR_LENGTH, "mul: vectors must be of the same length");

            xfvals = as_f64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = mulf64(xfvals[i], yfvals[yids[i]]);
        }
        else
        {
            l = x->len;
            if (l != y->len)
                throw(ERR_LENGTH, "mul: vectors must be of the same length");

            xfvals = as_f64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = mulf64(xfvals[i], yfvals[i]);
        }

        return vec;
    case mtype2(TYPE_F64, TYPE_I64):
        if (xids && yids)
        {
            xfvals = as_f64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = mulf64(xfvals[xids[i]], (f64_t)yivals[yids[i]]);
        }
        else if (xids)
        {
            if (l != y->len)
                throw(ERR_LENGTH, "mul: vectors must be of the same length");

            xfvals = as_f64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = mulf64(xfvals[xids[i]], (f64_t)yivals[i]);
        }
        else if (yids)
        {
            if (l != x->len)
                throw(ERR_LENGTH, "mul: vectors must be of the same length");

            xfvals = as_f64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = mulf64(xfvals[i], (f64_t)yivals[yids[i]]);
        }
        else
        {
            l = x->len;
            if (l != y->len)
                throw(ERR_LENGTH, "mul: vectors must be of the same length");

            xfvals = as_f64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = mulf64(xfvals[i], (f64_t)yivals[i]);
        }

        return vec;
    default:
        if ((x->type == TYPE_FILTERMAP) && (y->type == TYPE_FILTERMAP))
        {
            xids = as_i64(as_list(x)[1]);
            l = as_list(x)[1]->len;
            x = as_list(x)[0];

            if (l != as_list(y)[1]->len)
                throw(ERR_LENGTH, "mul: vectors must be of the same length");

            yids = as_i64(as_list(y)[1]);
            y = as_list(y)[0];

            goto dispatch;
        }
        else if (x->type == TYPE_FILTERMAP)
        {
            xids = as_i64(as_list(x)[1]);
            l = as_list(x)[1]->len;
            x = as_list(x)[0];
            goto dispatch;
        }
        else if (y->type == TYPE_FILTERMAP)
        {
            yids = as_i64(as_list(y)[1]);
            l = as_list(y)[1]->len;
            y = as_list(y)[0];
            goto dispatch;
        }

        throw(ERR_TYPE, "mul: unsupported types: '%s, '%s", typename(x->type), typename(y->type));
    }
}

obj_t ray_div(obj_t x, obj_t y)
{
    u64_t i, l = 0;
    obj_t vec;
    i64_t *xids = NULL, *yids = NULL, *xivals = NULL, *yivals = NULL;
    f64_t *xfvals = NULL, *yfvals = NULL;

dispatch:
    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return i64(divi64(x->i64, y->i64));
    case mtype2(-TYPE_I64, -TYPE_F64):
        return i64(divi64(x->i64, (i64_t)y->f64));
    case mtype2(-TYPE_F64, -TYPE_F64):
        return f64(divf64(x->f64, y->f64));
    case mtype2(-TYPE_F64, -TYPE_I64):
        return f64(divf64(x->f64, (f64_t)y->i64));
    case mtype2(-TYPE_I64, TYPE_I64):
        if (yids)
        {
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = divi64(x->i64, yivals[yids[i]]);

            return vec;
        }

        l = y->len;
        yivals = as_i64(y);
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = divi64(x->i64, yivals[i]);

        return vec;
    case mtype2(-TYPE_I64, TYPE_F64):
        if (yids)
        {
            yfvals = as_f64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = divf64((f64_t)x->i64, yfvals[yids[i]]);

            return vec;
        }

        l = y->len;
        yfvals = as_f64(y);
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = divf64((f64_t)x->i64, yfvals[i]);

        return vec;
    case mtype2(-TYPE_F64, TYPE_F64):
        if (yids)
        {
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = divf64(x->f64, yfvals[yids[i]]);

            return vec;
        }

        l = y->len;
        yfvals = as_f64(y);
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = divf64(x->f64, yfvals[i]);

        return vec;
    case mtype2(-TYPE_F64, TYPE_I64):
        if (yids)
        {
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = divf64(x->f64, (f64_t)yivals[yids[i]]);

            return vec;
        }

        l = y->len;
        yivals = as_i64(y);
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = divf64(x->f64, (f64_t)yivals[i]);

        return vec;
    case mtype2(TYPE_I64, -TYPE_I64):
        if (xids)
        {
            xivals = as_i64(x);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = divi64(xivals[xids[i]], y->i64);

            return vec;
        }

        l = x->len;
        xivals = as_i64(x);
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = divi64(xivals[i], y->i64);

        return vec;
    case mtype2(TYPE_I64, -TYPE_F64):
        if (xids)
        {
            xivals = as_i64(x);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = divf64((f64_t)xivals[xids[i]], y->f64);

            return vec;
        }

        l = x->len;
        xivals = as_i64(x);
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = divf64((f64_t)xivals[i], y->f64);

        return vec;
    case mtype2(TYPE_I64, TYPE_I64):
        if (xids && yids)
        {
            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = divi64(xivals[xids[i]], yivals[yids[i]]);
        }
        else if (xids)
        {
            if (l != y->len)
                throw(ERR_LENGTH, "div: vectors must be of the same length");

            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = divi64(xivals[xids[i]], yivals[i]);
        }
        else if (yids)
        {
            if (l != x->len)
                throw(ERR_LENGTH, "div: vectors must be of the same length");

            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = divi64(xivals[i], yivals[yids[i]]);
        }
        else
        {
            l = x->len;
            if (l != y->len)
                throw(ERR_LENGTH, "div: vectors must be of the same length");

            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = divi64(xivals[i], yivals[i]);
        }

        return vec;
    case mtype2(TYPE_I64, TYPE_F64):
        if (xids && yids)
        {
            xivals = as_i64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = divf64((f64_t)xivals[xids[i]], yfvals[yids[i]]);
        }
        else if (xids)
        {
            if (l != y->len)
                throw(ERR_LENGTH, "div: vectors must be of the same length");

            xivals = as_i64(x);
            yfvals = as_f64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = divf64((f64_t)xivals[xids[i]], yfvals[i]);
        }
        else if (yids)
        {
            if (l != x->len)
                throw(ERR_LENGTH, "div: vectors must be of the same length");

            xivals = as_i64(x);
            yfvals = as_f64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = divf64((f64_t)xivals[i], yfvals[yids[i]]);
        }
        else
        {
            l = x->len;
            if (l != y->len)
                throw(ERR_LENGTH, "div: vectors must be of the same length");

            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = divi64(xivals[i], yivals[i]);
        }

        return vec;
    case mtype2(TYPE_F64, -TYPE_F64):
        if (xids)
        {
            xfvals = as_f64(x);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = divf64(xfvals[xids[i]], y->f64);

            return vec;
        }

        l = x->len;
        xfvals = as_f64(x);
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = divf64(xfvals[i], y->f64);

        return vec;
    case mtype2(TYPE_F64, -TYPE_I64):
        if (xids)
        {
            xfvals = as_f64(x);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = divf64(xfvals[xids[i]], (f64_t)y->i64);

            return vec;
        }

        l = x->len;
        xfvals = as_f64(x);
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = divf64(xfvals[i], (f64_t)y->i64);

        return vec;
    case mtype2(TYPE_F64, TYPE_F64):
        if (xids && yids)
        {
            xfvals = as_f64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = divf64(xfvals[xids[i]], yfvals[yids[i]]);
        }
        else if (xids)
        {
            if (l != y->len)
                throw(ERR_LENGTH, "div: vectors must be of the same length");

            xfvals = as_f64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = divf64(xfvals[xids[i]], yfvals[i]);
        }
        else if (yids)
        {
            if (l != x->len)
                throw(ERR_LENGTH, "div: vectors must be of the same length");

            xfvals = as_f64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = divf64(xfvals[i], yfvals[yids[i]]);
        }
        else
        {
            l = x->len;
            if (l != y->len)
                throw(ERR_LENGTH, "div: vectors must be of the same length");

            xfvals = as_f64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = divf64(xfvals[i], yfvals[i]);
        }

        return vec;
    case mtype2(TYPE_F64, TYPE_I64):
        if (xids && yids)
        {
            xfvals = as_f64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = divf64(xfvals[xids[i]], (f64_t)yivals[yids[i]]);
        }
        else if (xids)
        {
            if (l != y->len)
                throw(ERR_LENGTH, "div: vectors must be of the same length");

            xfvals = as_f64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = divf64(xfvals[xids[i]], (f64_t)yivals[i]);
        }
        else if (yids)
        {
            if (l != x->len)
                throw(ERR_LENGTH, "div: vectors must be of the same length");

            xfvals = as_f64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = divf64(xfvals[i], (f64_t)yivals[yids[i]]);
        }
        else
        {
            l = x->len;
            if (l != y->len)
                throw(ERR_LENGTH, "div: vectors must be of the same length");

            xfvals = as_f64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = divf64(xfvals[i], (f64_t)yivals[i]);
        }

        return vec;
    default:
        if ((x->type == TYPE_FILTERMAP) && (y->type == TYPE_FILTERMAP))
        {
            xids = as_i64(as_list(x)[1]);
            l = as_list(x)[1]->len;
            x = as_list(x)[0];

            if (l != as_list(y)[1]->len)
                throw(ERR_LENGTH, "div: vectors must be of the same length");

            yids = as_i64(as_list(y)[1]);
            y = as_list(y)[0];

            goto dispatch;
        }
        else if (x->type == TYPE_FILTERMAP)
        {
            xids = as_i64(as_list(x)[1]);
            l = as_list(x)[1]->len;
            x = as_list(x)[0];
            goto dispatch;
        }
        else if (y->type == TYPE_FILTERMAP)
        {
            yids = as_i64(as_list(y)[1]);
            l = as_list(y)[1]->len;
            y = as_list(y)[0];
            goto dispatch;
        }

        throw(ERR_TYPE, "div: unsupported types: '%s, '%s", typename(x->type), typename(y->type));
    }
}

obj_t ray_fdiv(obj_t x, obj_t y)
{
    u64_t i, l = 0;
    obj_t vec;
    i64_t *xids = NULL, *yids = NULL, *xivals = NULL, *yivals = NULL;
    f64_t *xfvals = NULL, *yfvals = NULL;

dispatch:
    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return f64(fdivi64(x->i64, (f64_t)y->i64));
    case mtype2(-TYPE_I64, -TYPE_F64):
        return f64(fdivi64(x->i64, y->f64));
    case mtype2(-TYPE_F64, -TYPE_F64):
        return f64(fdivf64(x->f64, y->f64));
    case mtype2(-TYPE_F64, -TYPE_I64):
        return f64(fdivi64(x->f64, y->i64));
    case mtype2(-TYPE_I64, TYPE_I64):
        if (yids)
        {
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = fdivi64(x->i64, yivals[yids[i]]);

            return vec;
        }

        l = y->len;
        yivals = as_i64(y);
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = fdivi64(x->i64, yivals[i]);

        return vec;
    case mtype2(-TYPE_I64, TYPE_F64):
        if (yids)
        {
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = fdivi64(x->i64, yfvals[yids[i]]);

            return vec;
        }

        l = y->len;
        yfvals = as_f64(y);
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = fdivi64(x->i64, yfvals[i]);

        return vec;
    case mtype2(-TYPE_F64, TYPE_F64):
        if (yids)
        {
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = fdivf64(x->f64, yfvals[yids[i]]);

            return vec;
        }

        l = y->len;
        yfvals = as_f64(y);
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = fdivf64(x->f64, yfvals[i]);

        return vec;
    case mtype2(-TYPE_F64, TYPE_I64):
        if (yids)
        {
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = fdivi64(x->f64, yivals[yids[i]]);

            return vec;
        }

        l = y->len;
        yivals = as_i64(y);
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = fdivi64(x->f64, yivals[i]);

        return vec;
    case mtype2(TYPE_I64, -TYPE_I64):
        if (xids)
        {
            xivals = as_i64(x);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = fdivi64(xivals[xids[i]], y->i64);

            return vec;
        }

        l = x->len;
        xivals = as_i64(x);
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = fdivi64(xivals[i], y->i64);

        return vec;
    case mtype2(TYPE_I64, -TYPE_F64):
        if (xids)
        {
            xivals = as_i64(x);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = fdivf64((f64_t)xivals[xids[i]], y->f64);

            return vec;
        }

        l = x->len;
        xivals = as_i64(x);
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = fdivi64(xivals[i], y->f64);

        return vec;
    case mtype2(TYPE_I64, TYPE_I64):
        if (xids && yids)
        {
            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = fdivi64(xivals[xids[i]], yivals[yids[i]]);
        }
        else if (xids)
        {
            if (l != y->len)
                throw(ERR_LENGTH, "fdiv: vectors must be of the same length");

            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = fdivi64(xivals[xids[i]], yivals[i]);
        }
        else if (yids)
        {
            if (l != x->len)
                throw(ERR_LENGTH, "fdiv: vectors must be of the same length");

            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = fdivi64(xivals[i], yivals[yids[i]]);
        }
        else
        {
            l = x->len;
            if (l != y->len)
                throw(ERR_LENGTH, "fdiv: vectors must be of the same length");

            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = fdivi64(xivals[i], yivals[i]);
        }

        return vec;
    case mtype2(TYPE_I64, TYPE_F64):
        if (xids && yids)
        {
            xivals = as_i64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = fdivi64(xivals[xids[i]], yfvals[yids[i]]);
        }
        else if (xids)
        {
            if (l != y->len)
                throw(ERR_LENGTH, "fdiv: vectors must be of the same length");

            xivals = as_i64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = fdivi64(xivals[xids[i]], yfvals[i]);
        }
        else if (yids)
        {
            if (l != x->len)
                throw(ERR_LENGTH, "fdiv: vectors must be of the same length");

            xivals = as_i64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = fdivi64(xivals[i], yfvals[yids[i]]);
        }
        else
        {
            l = x->len;
            if (l != y->len)
                throw(ERR_LENGTH, "fdiv: vectors must be of the same length");

            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = fdivi64(xivals[i], yivals[i]);
        }

        return vec;
    case mtype2(TYPE_F64, -TYPE_F64):
        if (xids)
        {
            xfvals = as_f64(x);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = fdivf64(xfvals[xids[i]], y->f64);

            return vec;
        }

        l = x->len;
        xfvals = as_f64(x);
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = fdivf64(xfvals[i], y->f64);

        return vec;
    case mtype2(TYPE_F64, -TYPE_I64):
        if (xids)
        {
            xfvals = as_f64(x);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = fdivi64(xfvals[xids[i]], y->i64);

            return vec;
        }

        l = x->len;
        xfvals = as_f64(x);
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = fdivi64(xfvals[i], y->i64);

        return vec;
    case mtype2(TYPE_F64, TYPE_F64):
        if (xids && yids)
        {
            xfvals = as_f64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = fdivf64(xfvals[xids[i]], yfvals[yids[i]]);
        }
        else if (xids)
        {
            if (l != y->len)
                throw(ERR_LENGTH, "fdiv: vectors must be of the same length");

            xfvals = as_f64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = fdivf64(xfvals[xids[i]], yfvals[i]);
        }
        else if (yids)
        {
            if (l != x->len)
                throw(ERR_LENGTH, "fdiv: vectors must be of the same length");

            xfvals = as_f64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = fdivf64(xfvals[i], yfvals[yids[i]]);
        }
        else
        {
            l = x->len;
            if (l != y->len)
                throw(ERR_LENGTH, "fdiv: vectors must be of the same length");

            xfvals = as_f64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = fdivf64(xfvals[i], yfvals[i]);
        }

        return vec;
    case mtype2(TYPE_F64, TYPE_I64):
        if (xids && yids)
        {
            xfvals = as_f64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = fdivi64(xfvals[xids[i]], yivals[yids[i]]);
        }
        else if (xids)
        {
            if (l != y->len)
                throw(ERR_LENGTH, "fdiv: vectors must be of the same length");

            xfvals = as_f64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = fdivi64(xfvals[xids[i]], yivals[i]);
        }
        else if (yids)
        {
            if (l != x->len)
                throw(ERR_LENGTH, "fdiv: vectors must be of the same length");

            xfvals = as_f64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = fdivi64(xfvals[i], yivals[yids[i]]);
        }
        else
        {
            l = x->len;
            if (l != y->len)
                throw(ERR_LENGTH, "fdiv: vectors must be of the same length");

            xfvals = as_f64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = fdivi64(xfvals[i], yivals[i]);
        }

        return vec;
    default:
        if ((x->type == TYPE_FILTERMAP) && (y->type == TYPE_FILTERMAP))
        {
            xids = as_i64(as_list(x)[1]);
            l = as_list(x)[1]->len;
            x = as_list(x)[0];

            if (l != as_list(y)[1]->len)
                throw(ERR_LENGTH, "fdiv: vectors must be of the same length");

            yids = as_i64(as_list(y)[1]);
            y = as_list(y)[0];

            goto dispatch;
        }
        else if (x->type == TYPE_FILTERMAP)
        {
            xids = as_i64(as_list(x)[1]);
            l = as_list(x)[1]->len;
            x = as_list(x)[0];
            goto dispatch;
        }
        else if (y->type == TYPE_FILTERMAP)
        {
            yids = as_i64(as_list(y)[1]);
            l = as_list(y)[1]->len;
            y = as_list(y)[0];
            goto dispatch;
        }

        throw(ERR_TYPE, "add: unsupported types: '%s, '%s", typename(x->type), typename(y->type));
    }
}

obj_t ray_mod(obj_t x, obj_t y)
{
    u64_t i, l = 0;
    obj_t vec;
    i64_t *xids = NULL, *yids = NULL, *xivals = NULL, *yivals = NULL;
    f64_t *xfvals = NULL, *yfvals = NULL;

dispatch:
    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return i64(modi64(x->i64, y->i64));
    case mtype2(-TYPE_I64, -TYPE_F64):
        return i64(modi64(x->i64, (i64_t)y->f64));
    case mtype2(-TYPE_F64, -TYPE_F64):
        return i64(modf64(x->f64, y->f64));
    case mtype2(-TYPE_F64, -TYPE_I64):
        return i64(modf64(x->f64, (f64_t)y->i64));
    case mtype2(-TYPE_I64, TYPE_I64):
        if (yids)
        {
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = modi64(x->i64, yivals[yids[i]]);

            return vec;
        }

        l = y->len;
        yivals = as_i64(y);
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = modi64(x->i64, yivals[i]);

        return vec;
    case mtype2(-TYPE_I64, TYPE_F64):
        if (yids)
        {
            yfvals = as_f64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = modf64((f64_t)x->i64, yfvals[yids[i]]);

            return vec;
        }

        l = y->len;
        yfvals = as_f64(y);
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = modf64((f64_t)x->i64, yfvals[i]);

        return vec;
    case mtype2(-TYPE_F64, TYPE_F64):
        if (yids)
        {
            yfvals = as_f64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = modf64(x->f64, yfvals[yids[i]]);

            return vec;
        }

        l = y->len;
        yfvals = as_f64(y);
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = modf64(x->f64, yfvals[i]);

        return vec;
    case mtype2(-TYPE_F64, TYPE_I64):
        if (yids)
        {
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = modf64(x->f64, (f64_t)yivals[yids[i]]);

            return vec;
        }

        l = y->len;
        yivals = as_i64(y);
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = modf64(x->f64, (f64_t)yivals[i]);

        return vec;
    case mtype2(TYPE_I64, -TYPE_I64):
        if (xids)
        {
            xivals = as_i64(x);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = modi64(xivals[xids[i]], y->i64);

            return vec;
        }
        l = x->len;
        xivals = as_i64(x);
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = modi64(xivals[i], y->i64);

        return vec;
    case mtype2(TYPE_I64, -TYPE_F64):
        if (xids)
        {
            xivals = as_i64(x);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = modf64((f64_t)xivals[xids[i]], y->f64);

            return vec;
        }

        l = x->len;
        xivals = as_i64(x);
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = modf64((f64_t)xivals[i], y->f64);

        return vec;
    case mtype2(TYPE_I64, TYPE_I64):
        if (xids && yids)
        {
            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = modi64(xivals[xids[i]], yivals[yids[i]]);
        }
        else if (xids)
        {
            if (l != y->len)
                throw(ERR_LENGTH, "mod: vectors must be of the same length");

            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = modi64(xivals[xids[i]], yivals[i]);
        }
        else if (yids)
        {
            if (l != x->len)
                throw(ERR_LENGTH, "mod: vectors must be of the same length");

            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = modi64(xivals[i], yivals[yids[i]]);
        }
        else
        {
            l = x->len;
            if (l != y->len)
                throw(ERR_LENGTH, "mod: vectors must be of the same length");

            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = modi64(xivals[i], yivals[i]);
        }

        return vec;
    case mtype2(TYPE_I64, TYPE_F64):
        if (xids && yids)
        {
            xivals = as_i64(x);
            yfvals = as_f64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = modi64(xivals[xids[i]], yfvals[yids[i]]);
        }
        else if (xids)
        {
            if (l != y->len)
                throw(ERR_LENGTH, "mod: vectors must be of the same length");

            xivals = as_i64(x);
            yfvals = as_f64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = modf64((f64_t)xivals[xids[i]], yfvals[i]);
        }
        else if (yids)
        {
            if (l != x->len)
                throw(ERR_LENGTH, "mod: vectors must be of the same length");

            xivals = as_i64(x);
            yfvals = as_f64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = modf64((f64_t)xivals[i], yfvals[yids[i]]);
        }
        else
        {
            l = x->len;
            if (l != y->len)
                throw(ERR_LENGTH, "mod: vectors must be of the same length");

            xivals = as_i64(x);
            yivals = as_i64(y);
            vec = vector_i64(l);
            for (i = 0; i < l; i++)
                as_i64(vec)[i] = modi64(xivals[i], yivals[i]);
        }

        return vec;
    case mtype2(TYPE_F64, -TYPE_F64):
        if (xids)
        {
            xfvals = as_f64(x);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = modf64(y->f64, xfvals[xids[i]]);

            return vec;
        }

        l = x->len;
        xfvals = as_f64(x);
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = modf64(xfvals[i], y->f64);

        return vec;
    case mtype2(TYPE_F64, -TYPE_I64):
        if (xids)
        {
            xfvals = as_f64(x);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = modi64(xfvals[xids[i]], y->i64);

            return vec;
        }

        l = x->len;
        xfvals = as_f64(x);
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = modi64(xfvals[i], y->i64);

        return vec;
    case mtype2(TYPE_F64, TYPE_F64):
        if (xids && yids)
        {
            xfvals = as_f64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = modf64(xfvals[xids[i]], yfvals[yids[i]]);
        }
        else if (xids)
        {
            if (l != y->len)
                throw(ERR_LENGTH, "mod: vectors must be of the same length");

            xfvals = as_f64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = modf64(xfvals[xids[i]], yfvals[i]);
        }
        else if (yids)
        {
            if (l != x->len)
                throw(ERR_LENGTH, "mod: vectors must be of the same length");

            xfvals = as_f64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = modf64(xfvals[i], yfvals[yids[i]]);
        }
        else
        {
            l = x->len;
            if (l != y->len)
                throw(ERR_LENGTH, "mod: vectors must be of the same length");

            xfvals = as_f64(x);
            yfvals = as_f64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = modf64(xfvals[i], yfvals[i]);
        }

        return vec;
    case mtype2(TYPE_F64, TYPE_I64):
        if (xids && yids)
        {
            xfvals = as_f64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = modi64(xfvals[xids[i]], yivals[yids[i]]);
        }
        else if (xids)
        {
            if (l != y->len)
                throw(ERR_LENGTH, "mod: vectors must be of the same length");

            xfvals = as_f64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = modi64(xfvals[xids[i]], yivals[i]);
        }
        else if (yids)
        {
            if (l != x->len)
                throw(ERR_LENGTH, "mod: vectors must be of the same length");

            xfvals = as_f64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = modi64(xfvals[i], yivals[yids[i]]);
        }
        else
        {
            l = x->len;
            if (l != y->len)
                throw(ERR_LENGTH, "mod: vectors must be of the same length");

            xfvals = as_f64(x);
            yivals = as_i64(y);
            vec = vector_f64(l);
            for (i = 0; i < l; i++)
                as_f64(vec)[i] = modi64(xfvals[i], yivals[i]);
        }

        return vec;
    default:
        if ((x->type == TYPE_FILTERMAP) && (y->type == TYPE_FILTERMAP))
        {
            xids = as_i64(as_list(x)[1]);
            l = as_list(x)[1]->len;
            x = as_list(x)[0];

            if (l != as_list(y)[1]->len)
                throw(ERR_LENGTH, "mod: vectors must be of the same length");

            yids = as_i64(as_list(y)[1]);
            y = as_list(y)[0];

            goto dispatch;
        }
        else if (x->type == TYPE_FILTERMAP)
        {
            xids = as_i64(as_list(x)[1]);
            l = as_list(x)[1]->len;
            x = as_list(x)[0];
            goto dispatch;
        }
        else if (y->type == TYPE_FILTERMAP)
        {
            yids = as_i64(as_list(y)[1]);
            l = as_list(y)[1]->len;
            y = as_list(y)[0];
            goto dispatch;
        }

        throw(ERR_TYPE, "add: unsupported types: '%s, '%s", typename(x->type), typename(y->type));
    }
}

obj_t ray_sum(obj_t x)
{
    i32_t i;
    i64_t l = 0, isum = 0, *xivals = NULL;
    f64_t fsum = 0.0, *xfvals = NULL;

    switch (x->type)
    {
    case -TYPE_I64:
        return clone(x);
    case -TYPE_F64:
        return clone(x);
    case TYPE_I64:
        l = x->len;
        xivals = as_i64(x);
        for (i = 0; i < l; i++)
            isum += (xivals[i] == NULL_I64) ? 0 : xivals[i];

        return i64(isum);
    case TYPE_F64:
        l = x->len;
        xfvals = as_f64(x);
        for (i = 0; i < l; i++)
            fsum += xfvals[i];

        return f64(fsum);

    default:
        throw(ERR_TYPE, "sum: unsupported type: '%s", typename(x->type));
    }
}

obj_t ray_avg(obj_t x)
{
    u64_t i, l = 0, n;
    i64_t isum = 0, *xids = NULL, *xivals = NULL;
    f64_t fsum = 0.0, *xfvals = NULL;

dispatch:
    switch (x->type)
    {
    case -TYPE_I64:
    case -TYPE_F64:
        return clone(x);
    case TYPE_I64:
        if (xids)
        {
            xivals = as_i64(x);
            for (i = 0, n = 0; i < l; i++)
            {
                if (xivals[xids[i]] != NULL_I64)
                    isum += xivals[xids[i]];
                else
                    n++;
            }

            return f64((f64_t)isum / (l - n));
        }

        l = x->len;
        xivals = as_i64(x);

        for (i = 0, n = 0; i < l; i++)
        {
            if (xivals[i] != NULL_I64)
                isum += xivals[i];
            else
                n++;
        }

        return f64((f64_t)isum / (l - n));

    case TYPE_F64:
        if (xids)
        {
            xfvals = as_f64(x);
            for (i = 0, n = 0; i < l; i++)
                fsum += xfvals[xids[i]];

            return f64(fsum / l);
        }

        l = x->len;
        xfvals = as_f64(x);

        for (i = 0, n = 0; i < l; i++)
            fsum += xfvals[i];

        return f64(fsum / l);

    default:
        if (x->type == TYPE_FILTERMAP)
        {
            xids = as_i64(as_list(x)[1]);
            l = as_list(x)[1]->len;
            x = as_list(x)[0];
            goto dispatch;
        }

        throw(ERR_TYPE, "avg: unsupported type: '%s", typename(x->type));
    }
}

obj_t ray_min(obj_t x)
{
    u64_t i, l = 0;
    i64_t imin, iv, *xids = NULL, *xivals = NULL;
    f64_t fmin = 0.0, *xfvals = NULL;
    obj_t res;

dispatch:
    switch (x->type)
    {
    case TYPE_I64:
    case TYPE_TIMESTAMP:
        if (xids)
        {
            if (!l)
                return i64(NULL_I64);

            xivals = as_i64(x);
            imin = xivals[xids[0]];

            for (i = 0; i < l; i++)
            {
                iv = (xivals[xids[i]] == NULL_I64) ? MAX_I64 : xivals[xids[i]];
                imin = iv < imin ? iv : imin;
            }

            res = i64(imin);
            res->type = -x->type;

            return res;
        }

        l = x->len;

        if (!l)
            return i64(NULL_I64);

        xivals = as_i64(x);
        imin = xivals[0];

        xivals = as_i64(x);
        imin = xivals[0];

        for (i = 0; i < l; i++)
        {
            iv = (xivals[i] == NULL_I64) ? MAX_I64 : xivals[i];
            imin = iv < imin ? iv : imin;
        }

        res = i64(imin);
        res->type = -x->type;

        return res;

    case TYPE_F64:
        if (xids)
        {
            if (!l)
                return f64(NULL_F64);

            xfvals = as_f64(x);
            fmin = xfvals[xids[0]];

            for (i = 0; i < l; i++)
                fmin = xfvals[xids[i]] < fmin ? xfvals[xids[i]] : fmin;

            return f64(fmin);
        }

        l = x->len;

        if (!l)
            return f64(NULL_F64);

        xfvals = as_f64(x);
        fmin = xfvals[0];

        for (i = 0; i < l; i++)
            fmin = xfvals[i] < fmin ? xfvals[i] : fmin;

        return f64(fmin);

    default:
        if (x->type == TYPE_FILTERMAP)
        {
            xids = as_i64(as_list(x)[1]);
            l = as_list(x)[1]->len;
            x = as_list(x)[0];
            goto dispatch;
        }

        throw(ERR_TYPE, "min: unsupported type: '%s", typename(x->type));
    }
}

obj_t ray_max(obj_t x)
{
    u64_t i, l = 0;
    i64_t imax = 0, *xids = NULL, *xivals = NULL;
    f64_t fmax = 0.0, *xfvals = NULL;
    obj_t res;

dispatch:
    switch (x->type)
    {
    case TYPE_I64:
    case TYPE_TIMESTAMP:
        if (xids)
        {
            if (!l)
                return i64(NULL_I64);

            xivals = as_i64(x);
            imax = xivals[xids[0]];

            for (i = 0; i < l; i++)
                imax = xivals[xids[i]] > imax ? xivals[xids[i]] : imax;

            res = i64(imax);
            res->type = -x->type;

            return res;
        }

        l = x->len;

        if (!l)
            return i64(NULL_I64);

        xivals = as_i64(x);
        imax = xivals[0];

        for (i = 0; i < l; i++)
            imax = xivals[i] > imax ? xivals[i] : imax;

        res = i64(imax);
        res->type = -x->type;

        return res;

    case TYPE_F64:
        if (xids)
        {
            if (!l)
                return f64(NULL_F64);

            xfvals = as_f64(x);
            fmax = xfvals[xids[0]];

            for (i = 0; i < l; i++)
                fmax = xfvals[xids[i]] > fmax ? xfvals[xids[i]] : fmax;

            return f64(fmax);
        }

        l = x->len;

        if (!l)
            return f64(NULL_F64);

        xfvals = as_f64(x);
        fmax = xfvals[0];

        for (i = 0; i < l; i++)
            fmax = xfvals[i] > fmax ? xfvals[i] : fmax;

        return f64(fmax);

    default:
        if (x->type == TYPE_FILTERMAP)
        {
            xids = as_i64(as_list(x)[1]);
            l = as_list(x)[1]->len;
            x = as_list(x)[0];
            goto dispatch;
        }

        throw(ERR_TYPE, "max: unsupported type: '%s", typename(x->type));
    }
}

obj_t ray_round(obj_t x)
{
    u64_t i, l = 0;
    i64_t *xids = NULL, *rvals;
    f64_t *xfvals;
    obj_t res;

dispatch:
    switch (x->type)
    {
    case -TYPE_F64:
        return i64(roundf64(x->f64));
    case TYPE_F64:
        if (xids)
        {
            res = vector_i64(l);
            rvals = as_i64(res);
            xfvals = as_f64(x);

            for (i = 0; i < l; i++)
                rvals[i] = roundf64(xfvals[xids[i]]);

            return res;
        }

        l = x->len;

        res = vector_f64(l);
        rvals = as_i64(res);
        xfvals = as_f64(x);

        for (i = 0; i < l; i++)
            rvals[i] = roundf64(xfvals[i]);

        return res;
    default:
        if (x->type == TYPE_FILTERMAP)
        {
            xids = as_i64(as_list(x)[1]);
            l = as_list(x)[1]->len;
            x = as_list(x)[0];
            goto dispatch;
        }

        throw(ERR_TYPE, "round: unsupported type: '%s", typename(x->type));
    }
}

obj_t ray_floor(obj_t x)
{
    u64_t i, l = 0;
    i64_t *xids = NULL, *rvals;
    f64_t *xfvals;
    obj_t res;

dispatch:
    switch (x->type)
    {
    case -TYPE_F64:
        return i64(floorf64(x->f64));
    case TYPE_F64:
        if (xids)
        {
            res = vector_i64(l);
            rvals = as_i64(res);
            xfvals = as_f64(x);

            for (i = 0; i < l; i++)
                rvals[i] = floorf64(xfvals[xids[i]]);

            return res;
        }

        l = x->len;

        res = vector_f64(l);
        rvals = as_i64(res);
        xfvals = as_f64(x);

        for (i = 0; i < l; i++)
            rvals[i] = floorf64(xfvals[i]);

        return res;
    default:
        if (x->type == TYPE_FILTERMAP)
        {
            xids = as_i64(as_list(x)[1]);
            l = as_list(x)[1]->len;
            x = as_list(x)[0];
            goto dispatch;
        }

        throw(ERR_TYPE, "floor: unsupported type: '%s", typename(x->type));
    }
}

obj_t ray_ceil(obj_t x)
{
    u64_t i, l = 0;
    i64_t *xids = NULL, *rvals;
    f64_t *xfvals;
    obj_t res;

dispatch:
    switch (x->type)
    {
    case -TYPE_F64:
        return i64(ceilf64(x->f64));
    case TYPE_F64:
        if (xids)
        {
            res = vector_i64(l);
            rvals = as_i64(res);
            xfvals = as_f64(x);

            for (i = 0; i < l; i++)
                rvals[i] = ceilf64(xfvals[xids[i]]);

            return res;
        }

        l = x->len;

        res = vector_f64(l);
        rvals = as_i64(res);
        xfvals = as_f64(x);

        for (i = 0; i < l; i++)
            rvals[i] = ceilf64(xfvals[i]);

        return res;
    default:
        if (x->type == TYPE_FILTERMAP)
        {
            xids = as_i64(as_list(x)[1]);
            l = as_list(x)[1]->len;
            x = as_list(x)[0];
            goto dispatch;
        }

        throw(ERR_TYPE, "ceil: unsupported type: '%s", typename(x->type));
    }
}