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
#include "math.h"  // for forward declarations
#include "util.h"
#include "ops.h"
#include "error.h"
#include "aggr.h"
#include "pool.h"
#include "order.h"
#include "runtime.h"
#include "serde.h"   // for size_of_type
#include "index.h"   // for INDEX_TYPE_PARTEDCOMMON
#include "filter.h"  // for filter_collect

#define __UNOP_FOLD(x, lt, ot, op, ln, of, iv)                  \
    ({                                                          \
        __BASE_##lt##_t *__restrict__ $lhs = __AS_##lt(x) + of; \
        __BASE_##ot##_t $out = iv;                              \
        for (i64_t $i = 0; $i < ln; $i++)                       \
            $out = op($out, $lhs[$i]);                          \
        ot($out);                                               \
    })

#define __UNOP_MAP(x, lt, ot, op, ln, of, ov)           \
    ({                                                  \
        lt##_t *__restrict__ $lhs = __AS_##lt(x) + of;  \
        ot##_t *__restrict__ $out = __AS_##lt(ov) + of; \
        for (i64_t $i = 0; $i < ln; $i++)               \
            $out[$i] = op($lhs[$i]);                    \
        NULL_OBJ;                                       \
    })

#define __BINOP_A_V(x, y, lt, rt, ot, mt, op, ln, of, ov)                              \
    ({                                                                                 \
        __BASE_##rt##_t *__restrict__ $rhs;                                            \
        __BASE_##ot##_t *__restrict__ $out;                                            \
        __BASE_##lt##_t $x_val = x->__BASE_##lt;                                       \
        $rhs = __AS_##rt(y) + of;                                                      \
        $out = __AS_##ot(ov) + of;                                                     \
        for (i64_t $i = 0; $i < ln; $i++)                                              \
            $out[$i] = mt##_to_##ot(op(lt##_to_##mt($x_val), rt##_to_##mt($rhs[$i]))); \
        NULL_OBJ;                                                                      \
    })

#define __BINOP_V_A(x, y, lt, rt, ot, mt, op, ln, of, ov)                              \
    ({                                                                                 \
        __BASE_##lt##_t *__restrict__ $lhs;                                            \
        __BASE_##ot##_t *__restrict__ $out;                                            \
        __BASE_##rt##_t $y_val = y->__BASE_##rt;                                       \
        $lhs = __AS_##lt(x) + of;                                                      \
        $out = __AS_##ot(ov) + of;                                                     \
        for (i64_t $i = 0; $i < ln; $i++)                                              \
            $out[$i] = mt##_to_##ot(op(lt##_to_##mt($lhs[$i]), rt##_to_##mt($y_val))); \
        NULL_OBJ;                                                                      \
    })

#define __BINOP_V_V(x, y, lt, rt, ot, mt, op, ln, of, ov)                                \
    ({                                                                                   \
        __BASE_##lt##_t *__restrict__ $lhs;                                              \
        __BASE_##rt##_t *__restrict__ $rhs;                                              \
        __BASE_##ot##_t *__restrict__ $out;                                              \
        $lhs = __AS_##lt(x) + of;                                                        \
        $rhs = __AS_##rt(y) + of;                                                        \
        $out = __AS_##ot(ov) + of;                                                       \
        for (i64_t $i = 0; $i < ln; $i++)                                                \
            $out[$i] = mt##_to_##ot(op(lt##_to_##mt($lhs[$i]), rt##_to_##mt($rhs[$i]))); \
        NULL_OBJ;                                                                        \
    })

i8_t infer_math_type(obj_p x, obj_p y) {
    switch (MTYPE2(ABSI8(x->type), ABSI8(y->type))) {
        case MTYPE2(TYPE_B8, TYPE_I64):
        case MTYPE2(TYPE_I64, TYPE_B8):
            return TYPE_I64;
        case MTYPE2(TYPE_U8, TYPE_U8):
            return TYPE_U8;
        case MTYPE2(TYPE_U8, TYPE_I32):
        case MTYPE2(TYPE_I32, TYPE_U8):
            return TYPE_I32;
        case MTYPE2(TYPE_U8, TYPE_I64):
        case MTYPE2(TYPE_I64, TYPE_U8):
            return TYPE_I64;
        case MTYPE2(TYPE_U8, TYPE_F64):
        case MTYPE2(TYPE_F64, TYPE_U8):
            return TYPE_F64;
        case MTYPE2(TYPE_I16, TYPE_I16):
            return TYPE_I16;
        case MTYPE2(TYPE_I16, TYPE_I32):
        case MTYPE2(TYPE_I32, TYPE_I16):
            return TYPE_I32;
        case MTYPE2(TYPE_I16, TYPE_I64):
        case MTYPE2(TYPE_I64, TYPE_I16):
            return TYPE_I64;
        case MTYPE2(TYPE_I16, TYPE_F64):
        case MTYPE2(TYPE_F64, TYPE_I16):
            return TYPE_F64;
        case MTYPE2(TYPE_I32, TYPE_I32):
        case MTYPE2(TYPE_DATE, TYPE_DATE):
            return TYPE_I32;
        case MTYPE2(TYPE_I32, TYPE_I64):
        case MTYPE2(TYPE_I64, TYPE_I32):
        case MTYPE2(TYPE_I64, TYPE_I64):
        case MTYPE2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
            return TYPE_I64;
        case MTYPE2(TYPE_I32, TYPE_F64):
        case MTYPE2(TYPE_I64, TYPE_F64):
        case MTYPE2(TYPE_F64, TYPE_I32):
        case MTYPE2(TYPE_F64, TYPE_I64):
        case MTYPE2(TYPE_F64, TYPE_F64):
            return TYPE_F64;
        case MTYPE2(TYPE_I32, TYPE_DATE):
        case MTYPE2(TYPE_I64, TYPE_DATE):
        case MTYPE2(TYPE_DATE, TYPE_I32):
        case MTYPE2(TYPE_DATE, TYPE_I64):
            return TYPE_DATE;
        case MTYPE2(TYPE_I32, TYPE_TIME):
        case MTYPE2(TYPE_I64, TYPE_TIME):
        case MTYPE2(TYPE_TIME, TYPE_I32):
        case MTYPE2(TYPE_TIME, TYPE_I64):
        case MTYPE2(TYPE_TIME, TYPE_TIME):
            return TYPE_TIME;
        default:
            return TYPE_TIMESTAMP;
    }
}

i8_t infer_div_type(obj_p x, obj_p y) {
    switch (MTYPE2(ABSI8(x->type), ABSI8(y->type))) {
        case MTYPE2(TYPE_U8, TYPE_U8):
            return TYPE_U8;
        case MTYPE2(TYPE_U8, TYPE_I32):
        case MTYPE2(TYPE_I32, TYPE_U8):
            return TYPE_I32;
        case MTYPE2(TYPE_U8, TYPE_I64):
        case MTYPE2(TYPE_I64, TYPE_U8):
            return TYPE_I64;
        case MTYPE2(TYPE_U8, TYPE_F64):
        case MTYPE2(TYPE_F64, TYPE_U8):
            return TYPE_F64;
        case MTYPE2(TYPE_I16, TYPE_I16):
            return TYPE_I16;
        case MTYPE2(TYPE_I16, TYPE_I32):
        case MTYPE2(TYPE_I32, TYPE_I16):
            return TYPE_I32;
        case MTYPE2(TYPE_I16, TYPE_I64):
        case MTYPE2(TYPE_I64, TYPE_I16):
            return TYPE_I64;
        case MTYPE2(TYPE_I16, TYPE_F64):
        case MTYPE2(TYPE_F64, TYPE_I16):
            return TYPE_F64;
        case MTYPE2(TYPE_I32, TYPE_I32):
        case MTYPE2(TYPE_I32, TYPE_I64):
        case MTYPE2(TYPE_I32, TYPE_F64):
            return TYPE_I32;
        case MTYPE2(TYPE_F64, TYPE_I32):
        case MTYPE2(TYPE_F64, TYPE_I64):
        case MTYPE2(TYPE_F64, TYPE_F64):
            return TYPE_F64;
        case MTYPE2(TYPE_TIME, TYPE_I32):
        case MTYPE2(TYPE_TIME, TYPE_I64):
        case MTYPE2(TYPE_TIME, TYPE_F64):
            return TYPE_TIME;
        default:
            return TYPE_I64;
    }
}

i8_t infer_mod_type(obj_p x, obj_p y) {
    switch (MTYPE2(ABSI8(x->type), ABSI8(y->type))) {
        case MTYPE2(TYPE_U8, TYPE_U8):
            return TYPE_U8;
        case MTYPE2(TYPE_U8, TYPE_I32):
        case MTYPE2(TYPE_I32, TYPE_U8):
            return TYPE_I32;
        case MTYPE2(TYPE_U8, TYPE_I64):
        case MTYPE2(TYPE_I64, TYPE_U8):
            return TYPE_I64;
        case MTYPE2(TYPE_I16, TYPE_I16):
            return TYPE_I16;
        case MTYPE2(TYPE_I16, TYPE_I32):
        case MTYPE2(TYPE_I32, TYPE_I16):
            return TYPE_I32;
        case MTYPE2(TYPE_I16, TYPE_I64):
        case MTYPE2(TYPE_I64, TYPE_I16):
            return TYPE_I64;
        case MTYPE2(TYPE_I32, TYPE_I32):
        case MTYPE2(TYPE_I64, TYPE_I32):
            return TYPE_I32;
        case MTYPE2(TYPE_I32, TYPE_F64):
        case MTYPE2(TYPE_I64, TYPE_F64):
        case MTYPE2(TYPE_F64, TYPE_F64):
        case MTYPE2(TYPE_F64, TYPE_I32):
        case MTYPE2(TYPE_F64, TYPE_I64):
            return TYPE_F64;
        case MTYPE2(TYPE_TIME, TYPE_I32):
        case MTYPE2(TYPE_TIME, TYPE_I64):
            return TYPE_TIME;
        default:
            return TYPE_I64;
    }
}

i8_t infer_xbar_type(obj_p x, obj_p y) {
    switch (MTYPE2(ABSI8(x->type), ABSI8(y->type))) {
        case MTYPE2(TYPE_I32, TYPE_I32):
            return TYPE_I32;
        case MTYPE2(TYPE_I32, TYPE_F64):
        case MTYPE2(TYPE_I64, TYPE_F64):
        case MTYPE2(TYPE_F64, TYPE_F64):
        case MTYPE2(TYPE_F64, TYPE_I32):
        case MTYPE2(TYPE_F64, TYPE_I64):
            return TYPE_F64;
        case MTYPE2(TYPE_DATE, TYPE_I32):
        case MTYPE2(TYPE_DATE, TYPE_I64):
            return TYPE_DATE;
        case MTYPE2(TYPE_TIME, TYPE_I32):
        case MTYPE2(TYPE_TIME, TYPE_I64):
        case MTYPE2(TYPE_TIME, TYPE_TIME):
            return TYPE_TIME;
        case MTYPE2(TYPE_TIMESTAMP, TYPE_I32):
        case MTYPE2(TYPE_TIMESTAMP, TYPE_I64):
        case MTYPE2(TYPE_TIMESTAMP, TYPE_TIME):
            return TYPE_TIMESTAMP;
        default:
            return TYPE_I64;
    }
}

obj_p ray_add_partial(obj_p x, obj_p y, i64_t len, i64_t offset, obj_p out) {
    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(-TYPE_I32, -TYPE_I32):
            return i32(ADDI32(x->i32, y->i32));
        case MTYPE2(-TYPE_I32, -TYPE_I64):
            return i64(ADDI64(i32_to_i64(x->i32), y->i64));
        case MTYPE2(-TYPE_I32, -TYPE_F64):
            return f64(ADDF64(i32_to_f64(x->i32), y->f64));
        case MTYPE2(-TYPE_I32, -TYPE_DATE):
            return adate(ADDI32(x->i32, y->i32));
        case MTYPE2(-TYPE_I32, -TYPE_TIME):
            return atime(ADDI32(x->i32, y->i32));
        case MTYPE2(-TYPE_I32, -TYPE_TIMESTAMP):
            return timestamp(ADDI64(i32_to_i64(x->i32), y->i64));
        case MTYPE2(-TYPE_I32, TYPE_I32):
            return __BINOP_A_V(x, y, i32, i32, i32, i32, ADDI32, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_I64):
            return __BINOP_A_V(x, y, i32, i64, i64, i64, ADDI64, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_F64):
            return __BINOP_A_V(x, y, i32, f64, f64, f64, ADDF64, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_DATE):
            return __BINOP_A_V(x, y, i32, i32, date, date, ADDI32, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_TIME):
            return __BINOP_A_V(x, y, i32, i32, time, time, ADDI32, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_TIMESTAMP):
            return __BINOP_A_V(x, y, i32, i64, timestamp, timestamp, ADDI64, len, offset, out);

        case MTYPE2(-TYPE_I64, -TYPE_I32):
            return i64(ADDI64(x->i64, i32_to_i64(y->i32)));
        case MTYPE2(-TYPE_I64, -TYPE_I64):
            return i64(ADDI64(x->i64, y->i64));
        case MTYPE2(-TYPE_I64, -TYPE_F64):
            return f64(ADDF64(i64_to_f64(x->i64), y->f64));
        case MTYPE2(-TYPE_I64, -TYPE_DATE):
            return adate(ADDI32(i64_to_i32(x->i64), y->i32));
        case MTYPE2(-TYPE_I64, -TYPE_TIME):
            return atime(ADDI32(i64_to_i32(x->i64), y->i32));
        case MTYPE2(-TYPE_I64, -TYPE_TIMESTAMP):
            return timestamp(ADDI64(x->i64, y->i64));
        case MTYPE2(-TYPE_I64, TYPE_I32):
            return __BINOP_A_V(x, y, i64, i32, i64, i64, ADDI64, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_I64):
            return __BINOP_A_V(x, y, i64, i64, i64, i64, ADDI64, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_F64):
            return __BINOP_A_V(x, y, i64, f64, f64, f64, ADDF64, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_DATE):
            return __BINOP_A_V(x, y, i64, i32, date, date, ADDI32, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_TIME):
            return __BINOP_A_V(x, y, i64, i32, time, time, ADDI32, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_TIMESTAMP):
            return __BINOP_A_V(x, y, i64, i64, timestamp, timestamp, ADDI64, len, offset, out);

        case MTYPE2(-TYPE_F64, -TYPE_I32):
            return f64(ADDF64(x->f64, i32_to_f64(y->i32)));
        case MTYPE2(-TYPE_F64, -TYPE_I64):
            return f64(ADDF64(x->f64, i64_to_f64(y->i64)));
        case MTYPE2(-TYPE_F64, -TYPE_F64):
            return f64(ADDF64(x->f64, y->f64));
        case MTYPE2(-TYPE_F64, TYPE_I32):
            return __BINOP_A_V(x, y, f64, i32, f64, f64, ADDF64, len, offset, out);
        case MTYPE2(-TYPE_F64, TYPE_I64):
            return __BINOP_A_V(x, y, f64, i64, f64, f64, ADDF64, len, offset, out);
        case MTYPE2(-TYPE_F64, TYPE_F64):
            return __BINOP_A_V(x, y, f64, f64, f64, f64, ADDF64, len, offset, out);

        case MTYPE2(-TYPE_DATE, -TYPE_I32):
            return adate(ADDI32(x->i32, y->i32));
        case MTYPE2(-TYPE_DATE, -TYPE_I64):
            return adate(ADDI32(x->i32, i64_to_i32(y->i64)));
        case MTYPE2(-TYPE_DATE, -TYPE_TIME):
            return timestamp(ADDI64(date_to_timestamp(x->i32), time_to_timestamp(y->i32)));
        case MTYPE2(-TYPE_DATE, TYPE_I32):
            return __BINOP_A_V(x, y, i32, i32, date, date, ADDI32, len, offset, out);
        case MTYPE2(-TYPE_DATE, TYPE_I64):
            return __BINOP_A_V(x, y, i32, i64, date, date, ADDI32, len, offset, out);
        case MTYPE2(-TYPE_DATE, TYPE_TIME):
            return __BINOP_A_V(x, y, date, time, timestamp, timestamp, ADDI64, len, offset, out);
        case MTYPE2(-TYPE_TIME, -TYPE_I32):
        case MTYPE2(-TYPE_TIME, -TYPE_TIME):
            return atime(ADDI32(x->i32, y->i32));
        case MTYPE2(-TYPE_TIME, -TYPE_I64):
            return atime(ADDI32(x->i32, i64_to_i32(y->i64)));
        case MTYPE2(-TYPE_TIME, -TYPE_DATE):
            return timestamp(ADDI64(time_to_timestamp(x->i32), date_to_timestamp(y->i32)));
        case MTYPE2(-TYPE_TIME, -TYPE_TIMESTAMP):
            return timestamp(ADDI64(time_to_timestamp(x->i32), y->i64));
        case MTYPE2(-TYPE_TIME, TYPE_I32):
        case MTYPE2(-TYPE_TIME, TYPE_TIME):
            return __BINOP_A_V(x, y, i32, i32, time, time, ADDI32, len, offset, out);
        case MTYPE2(-TYPE_TIME, TYPE_I64):
            return __BINOP_A_V(x, y, i32, i64, time, time, ADDI32, len, offset, out);
        case MTYPE2(-TYPE_TIME, TYPE_DATE):
            return __BINOP_A_V(x, y, time, date, timestamp, timestamp, ADDI64, len, offset, out);
        case MTYPE2(-TYPE_TIME, TYPE_TIMESTAMP):
            return __BINOP_A_V(x, y, time, timestamp, timestamp, timestamp, ADDI64, len, offset, out);

        case MTYPE2(-TYPE_TIMESTAMP, -TYPE_I32):
            return timestamp(ADDI64(x->i64, i32_to_i64(y->i32)));
        case MTYPE2(-TYPE_TIMESTAMP, -TYPE_I64):
            return timestamp(ADDI64(x->i64, y->i64));
        case MTYPE2(-TYPE_TIMESTAMP, -TYPE_TIME):
            return timestamp(ADDI64(x->i64, time_to_timestamp(y->i32)));
        case MTYPE2(-TYPE_TIMESTAMP, TYPE_I32):
            return __BINOP_A_V(x, y, i64, i32, timestamp, timestamp, ADDI64, len, offset, out);
        case MTYPE2(-TYPE_TIMESTAMP, TYPE_I64):
            return __BINOP_A_V(x, y, i64, i64, timestamp, timestamp, ADDI64, len, offset, out);
        case MTYPE2(-TYPE_TIMESTAMP, TYPE_TIME):
            return __BINOP_A_V(x, y, i64, time, timestamp, timestamp, ADDI64, len, offset, out);

        case MTYPE2(TYPE_I32, -TYPE_I32):
            return __BINOP_V_A(x, y, i32, i32, i32, i32, ADDI32, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_I64):
            return __BINOP_V_A(x, y, i32, i64, i64, i64, ADDI64, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_F64):
            return __BINOP_V_A(x, y, i32, f64, f64, f64, ADDF64, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_DATE):
            return __BINOP_V_A(x, y, i32, i32, date, date, ADDI32, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_TIME):
            return __BINOP_V_A(x, y, i32, i32, time, time, ADDI32, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_TIMESTAMP):
            return __BINOP_V_A(x, y, i32, i64, timestamp, timestamp, ADDI64, len, offset, out);
        case MTYPE2(TYPE_B8, TYPE_I64):
            return __BINOP_V_V(x, y, b8, i64, i64, i64, ADDI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_B8):
            return __BINOP_V_V(x, y, i64, b8, i64, i64, ADDI64, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_I32):
            return __BINOP_V_V(x, y, i32, i32, i32, i32, ADDI32, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_I64):
            return __BINOP_V_V(x, y, i32, i64, i64, i64, ADDI64, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_F64):
            return __BINOP_V_V(x, y, i32, f64, f64, f64, ADDF64, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_DATE):
            return __BINOP_V_V(x, y, i32, i32, date, date, ADDI32, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_TIME):
            return __BINOP_V_V(x, y, i32, i32, time, time, ADDI32, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_TIMESTAMP):
            return __BINOP_V_V(x, y, i32, i64, timestamp, timestamp, ADDI64, len, offset, out);

        case MTYPE2(TYPE_I64, -TYPE_I32):
        case MTYPE2(TYPE_I64, -TYPE_I64):
            return __BINOP_V_A(x, y, i64, i64, i64, i64, ADDI64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_F64):
            return __BINOP_V_A(x, y, i64, f64, f64, f64, ADDF64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_DATE):
            return __BINOP_V_A(x, y, i64, i32, date, date, ADDI32, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_TIME):
            return __BINOP_V_A(x, y, i64, i32, time, time, ADDI32, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_TIMESTAMP):
            return __BINOP_V_A(x, y, i64, i64, timestamp, timestamp, ADDI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_I32):
            return __BINOP_V_V(x, y, i64, i32, i64, i64, ADDI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_I64):
            return __BINOP_V_V(x, y, i64, i64, i64, i64, ADDI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_F64):
            return __BINOP_V_V(x, y, i64, f64, f64, f64, ADDF64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_DATE):
            return __BINOP_V_V(x, y, i64, i32, date, date, ADDI32, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_TIME):
            return __BINOP_V_V(x, y, i64, i32, time, time, ADDI32, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_TIMESTAMP):
            return __BINOP_V_V(x, y, i64, i64, timestamp, timestamp, ADDI64, len, offset, out);

        case MTYPE2(TYPE_F64, -TYPE_I32):
            return __BINOP_V_A(x, y, f64, i32, f64, f64, ADDF64, len, offset, out);
        case MTYPE2(TYPE_F64, -TYPE_I64):
            return __BINOP_V_A(x, y, f64, i64, f64, f64, ADDF64, len, offset, out);
        case MTYPE2(TYPE_F64, -TYPE_F64):
            return __BINOP_V_A(x, y, f64, f64, f64, f64, ADDF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_I32):
            return __BINOP_V_V(x, y, f64, i32, f64, f64, ADDF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_I64):
            return __BINOP_V_V(x, y, f64, i64, f64, f64, ADDF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_F64):
            return __BINOP_V_V(x, y, f64, f64, f64, f64, ADDF64, len, offset, out);

        case MTYPE2(TYPE_DATE, -TYPE_I32):
            return __BINOP_V_A(x, y, i32, i32, date, date, ADDI32, len, offset, out);
        case MTYPE2(TYPE_DATE, -TYPE_I64):
            return __BINOP_V_A(x, y, i32, i64, date, date, ADDI32, len, offset, out);
        case MTYPE2(TYPE_DATE, -TYPE_TIME):
            return __BINOP_V_A(x, y, date, time, timestamp, timestamp, ADDI64, len, offset, out);
        case MTYPE2(TYPE_DATE, TYPE_I32):
            return __BINOP_V_V(x, y, i32, i32, date, date, ADDI32, len, offset, out);
        case MTYPE2(TYPE_DATE, TYPE_I64):
            return __BINOP_V_V(x, y, i32, i64, date, date, ADDI32, len, offset, out);
        case MTYPE2(TYPE_DATE, TYPE_TIME):
            return __BINOP_V_V(x, y, date, time, timestamp, timestamp, ADDI64, len, offset, out);

        case MTYPE2(TYPE_TIME, -TYPE_I32):
        case MTYPE2(TYPE_TIME, -TYPE_TIME):
            return __BINOP_V_A(x, y, i32, i32, time, time, ADDI32, len, offset, out);
        case MTYPE2(TYPE_TIME, -TYPE_I64):
            return __BINOP_V_A(x, y, i32, i64, time, time, ADDI32, len, offset, out);
        case MTYPE2(TYPE_TIME, -TYPE_DATE):
            return __BINOP_V_A(x, y, time, date, timestamp, timestamp, ADDI64, len, offset, out);
        case MTYPE2(TYPE_TIME, -TYPE_TIMESTAMP):
            return __BINOP_V_A(x, y, time, timestamp, timestamp, timestamp, ADDI64, len, offset, out);
        case MTYPE2(TYPE_TIME, TYPE_I32):
        case MTYPE2(TYPE_TIME, TYPE_TIME):
            return __BINOP_V_V(x, y, i32, i32, time, time, ADDI32, len, offset, out);
        case MTYPE2(TYPE_TIME, TYPE_I64):
            return __BINOP_V_V(x, y, i32, i64, time, time, ADDI32, len, offset, out);
        case MTYPE2(TYPE_TIME, TYPE_DATE):
            return __BINOP_V_V(x, y, time, date, timestamp, timestamp, ADDI64, len, offset, out);
        case MTYPE2(TYPE_TIME, TYPE_TIMESTAMP):
            return __BINOP_V_V(x, y, time, timestamp, timestamp, timestamp, ADDI64, len, offset, out);

        case MTYPE2(TYPE_TIMESTAMP, -TYPE_I32):
            return __BINOP_V_A(x, y, i64, i32, timestamp, timestamp, ADDI64, len, offset, out);
        case MTYPE2(TYPE_TIMESTAMP, -TYPE_I64):
            return __BINOP_V_A(x, y, i64, i64, timestamp, timestamp, ADDI64, len, offset, out);
        case MTYPE2(TYPE_TIMESTAMP, -TYPE_TIME):
            return __BINOP_V_A(x, y, timestamp, time, timestamp, timestamp, ADDI64, len, offset, out);
        case MTYPE2(TYPE_TIMESTAMP, TYPE_I32):
            return __BINOP_V_V(x, y, i64, i32, timestamp, timestamp, ADDI64, len, offset, out);
        case MTYPE2(TYPE_TIMESTAMP, TYPE_I64):
            return __BINOP_V_V(x, y, i64, i64, timestamp, timestamp, ADDI64, len, offset, out);
        case MTYPE2(TYPE_TIMESTAMP, TYPE_TIME):
            return __BINOP_V_V(x, y, i64, time, timestamp, timestamp, ADDI64, len, offset, out);

        case MTYPE2(-TYPE_U8, -TYPE_U8):
            return u8(ADDU8(x->u8, y->u8));
        case MTYPE2(-TYPE_U8, -TYPE_I32):
            return i32(ADDI32(u8_to_i32(x->u8), y->i32));
        case MTYPE2(-TYPE_U8, -TYPE_I64):
            return i64(ADDI64(u8_to_i64(x->u8), y->i64));
        case MTYPE2(-TYPE_U8, -TYPE_F64):
            return f64(ADDF64(u8_to_f64(x->u8), y->f64));
        case MTYPE2(-TYPE_U8, TYPE_U8):
            return __BINOP_A_V(x, y, u8, u8, u8, u8, ADDU8, len, offset, out);
        case MTYPE2(-TYPE_U8, TYPE_I32):
            return __BINOP_A_V(x, y, u8, i32, i32, i32, ADDI32, len, offset, out);
        case MTYPE2(-TYPE_U8, TYPE_I64):
            return __BINOP_A_V(x, y, u8, i64, i64, i64, ADDI64, len, offset, out);
        case MTYPE2(-TYPE_U8, TYPE_F64):
            return __BINOP_A_V(x, y, u8, f64, f64, f64, ADDF64, len, offset, out);
        case MTYPE2(TYPE_U8, -TYPE_U8):
            return __BINOP_V_A(x, y, u8, u8, u8, u8, ADDU8, len, offset, out);
        case MTYPE2(TYPE_U8, -TYPE_I32):
            return __BINOP_V_A(x, y, u8, i32, i32, i32, ADDI32, len, offset, out);
        case MTYPE2(TYPE_U8, -TYPE_I64):
            return __BINOP_V_A(x, y, u8, i64, i64, i64, ADDI64, len, offset, out);
        case MTYPE2(TYPE_U8, -TYPE_F64):
            return __BINOP_V_A(x, y, u8, f64, f64, f64, ADDF64, len, offset, out);
        case MTYPE2(TYPE_U8, TYPE_U8):
            return __BINOP_V_V(x, y, u8, u8, u8, u8, ADDU8, len, offset, out);
        case MTYPE2(TYPE_U8, TYPE_I32):
            return __BINOP_V_V(x, y, u8, i32, i32, i32, ADDI32, len, offset, out);
        case MTYPE2(TYPE_U8, TYPE_I64):
            return __BINOP_V_V(x, y, u8, i64, i64, i64, ADDI64, len, offset, out);
        case MTYPE2(TYPE_U8, TYPE_F64):
            return __BINOP_V_V(x, y, u8, f64, f64, f64, ADDF64, len, offset, out);

        case MTYPE2(-TYPE_I16, -TYPE_I16):
            return i16(ADDI16(x->i16, y->i16));
        case MTYPE2(-TYPE_I16, -TYPE_I32):
            return i32(ADDI32(i16_to_i32(x->i16), y->i32));
        case MTYPE2(-TYPE_I16, -TYPE_I64):
            return i64(ADDI64(i16_to_i64(x->i16), y->i64));
        case MTYPE2(-TYPE_I16, -TYPE_F64):
            return f64(ADDF64(i16_to_f64(x->i16), y->f64));
        case MTYPE2(-TYPE_I16, TYPE_I16):
            return __BINOP_A_V(x, y, i16, i16, i16, i16, ADDI16, len, offset, out);
        case MTYPE2(-TYPE_I16, TYPE_I32):
            return __BINOP_A_V(x, y, i16, i32, i32, i32, ADDI32, len, offset, out);
        case MTYPE2(-TYPE_I16, TYPE_I64):
            return __BINOP_A_V(x, y, i16, i64, i64, i64, ADDI64, len, offset, out);
        case MTYPE2(-TYPE_I16, TYPE_F64):
            return __BINOP_A_V(x, y, i16, f64, f64, f64, ADDF64, len, offset, out);
        case MTYPE2(TYPE_I16, -TYPE_I16):
            return __BINOP_V_A(x, y, i16, i16, i16, i16, ADDI16, len, offset, out);
        case MTYPE2(TYPE_I16, -TYPE_I32):
            return __BINOP_V_A(x, y, i16, i32, i32, i32, ADDI32, len, offset, out);
        case MTYPE2(TYPE_I16, -TYPE_I64):
            return __BINOP_V_A(x, y, i16, i64, i64, i64, ADDI64, len, offset, out);
        case MTYPE2(TYPE_I16, -TYPE_F64):
            return __BINOP_V_A(x, y, i16, f64, f64, f64, ADDF64, len, offset, out);
        case MTYPE2(TYPE_I16, TYPE_I16):
            return __BINOP_V_V(x, y, i16, i16, i16, i16, ADDI16, len, offset, out);
        case MTYPE2(TYPE_I16, TYPE_I32):
            return __BINOP_V_V(x, y, i16, i32, i32, i32, ADDI32, len, offset, out);
        case MTYPE2(TYPE_I16, TYPE_I64):
            return __BINOP_V_V(x, y, i16, i64, i64, i64, ADDI64, len, offset, out);
        case MTYPE2(TYPE_I16, TYPE_F64):
            return __BINOP_V_V(x, y, i16, f64, f64, f64, ADDF64, len, offset, out);

        case MTYPE2(-TYPE_I32, -TYPE_U8):
            return i32(ADDI32(x->i32, u8_to_i32(y->u8)));
        case MTYPE2(-TYPE_I32, -TYPE_I16):
            return i32(ADDI32(x->i32, i16_to_i32(y->i16)));
        case MTYPE2(-TYPE_I32, TYPE_U8):
            return __BINOP_A_V(x, y, i32, u8, i32, i32, ADDI32, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_I16):
            return __BINOP_A_V(x, y, i32, i16, i32, i32, ADDI32, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_U8):
            return __BINOP_V_A(x, y, i32, u8, i32, i32, ADDI32, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_I16):
            return __BINOP_V_A(x, y, i32, i16, i32, i32, ADDI32, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_U8):
            return __BINOP_V_V(x, y, i32, u8, i32, i32, ADDI32, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_I16):
            return __BINOP_V_V(x, y, i32, i16, i32, i32, ADDI32, len, offset, out);

        case MTYPE2(-TYPE_I64, -TYPE_U8):
            return i64(ADDI64(x->i64, u8_to_i64(y->u8)));
        case MTYPE2(-TYPE_I64, -TYPE_I16):
            return i64(ADDI64(x->i64, i16_to_i64(y->i16)));
        case MTYPE2(-TYPE_I64, TYPE_U8):
            return __BINOP_A_V(x, y, i64, u8, i64, i64, ADDI64, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_I16):
            return __BINOP_A_V(x, y, i64, i16, i64, i64, ADDI64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_U8):
            return __BINOP_V_A(x, y, i64, u8, i64, i64, ADDI64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_I16):
            return __BINOP_V_A(x, y, i64, i16, i64, i64, ADDI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_U8):
            return __BINOP_V_V(x, y, i64, u8, i64, i64, ADDI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_I16):
            return __BINOP_V_V(x, y, i64, i16, i64, i64, ADDI64, len, offset, out);

        case MTYPE2(-TYPE_F64, -TYPE_U8):
            return f64(ADDF64(x->f64, u8_to_f64(y->u8)));
        case MTYPE2(-TYPE_F64, -TYPE_I16):
            return f64(ADDF64(x->f64, i16_to_f64(y->i16)));
        case MTYPE2(-TYPE_F64, TYPE_U8):
            return __BINOP_A_V(x, y, f64, u8, f64, f64, ADDF64, len, offset, out);
        case MTYPE2(-TYPE_F64, TYPE_I16):
            return __BINOP_A_V(x, y, f64, i16, f64, f64, ADDF64, len, offset, out);
        case MTYPE2(TYPE_F64, -TYPE_U8):
            return __BINOP_V_A(x, y, f64, u8, f64, f64, ADDF64, len, offset, out);
        case MTYPE2(TYPE_F64, -TYPE_I16):
            return __BINOP_V_A(x, y, f64, i16, f64, f64, ADDF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_U8):
            return __BINOP_V_V(x, y, f64, u8, f64, f64, ADDF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_I16):
            return __BINOP_V_V(x, y, f64, i16, f64, f64, ADDF64, len, offset, out);

        default:
            return err_new(EC_TYPE);
    }
}

obj_p ray_sub_partial(obj_p x, obj_p y, i64_t len, i64_t offset, obj_p out) {
    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(-TYPE_I32, -TYPE_I32):
            return i32(SUBI32(x->i32, y->i32));
        case MTYPE2(-TYPE_I32, -TYPE_I64):
            return i64(SUBI64(i32_to_i64(x->i32), y->i64));
        case MTYPE2(-TYPE_I32, -TYPE_F64):
            return f64(SUBF64(i32_to_f64(x->i32), y->f64));
        case MTYPE2(-TYPE_I32, -TYPE_TIME):
            return atime(SUBI32(x->i32, y->i32));
        case MTYPE2(-TYPE_I32, TYPE_I32):
            return __BINOP_A_V(x, y, i32, i32, i32, i32, SUBI32, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_I64):
            return __BINOP_A_V(x, y, i32, i64, i64, i64, SUBI64, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_F64):
            return __BINOP_A_V(x, y, i32, f64, f64, f64, SUBF64, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_TIME):
            return __BINOP_A_V(x, y, i32, i32, time, time, SUBI32, len, offset, out);

        case MTYPE2(-TYPE_I64, -TYPE_I32):
            return i64(SUBI64(x->i64, i32_to_i64(y->i32)));
        case MTYPE2(-TYPE_I64, -TYPE_I64):
            return i64(SUBI64(x->i64, y->i64));
        case MTYPE2(-TYPE_I64, -TYPE_F64):
            return f64(SUBF64(i64_to_f64(x->i64), y->f64));
        case MTYPE2(-TYPE_I64, -TYPE_TIME):
            return atime(SUBI32(i64_to_i32(x->i64), y->i32));
        case MTYPE2(-TYPE_I64, TYPE_I32):
            return __BINOP_A_V(x, y, i64, i32, i64, i64, SUBI64, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_I64):
            return __BINOP_A_V(x, y, i64, i64, i64, i64, SUBI64, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_F64):
            return __BINOP_A_V(x, y, i64, f64, f64, f64, SUBF64, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_TIME):
            return __BINOP_A_V(x, y, i64, i32, time, time, SUBI32, len, offset, out);

        case MTYPE2(-TYPE_F64, -TYPE_I32):
            return f64(SUBF64(x->f64, i32_to_f64(y->i32)));
        case MTYPE2(-TYPE_F64, -TYPE_I64):
            return f64(SUBF64(x->f64, i64_to_f64(y->i64)));
        case MTYPE2(-TYPE_F64, -TYPE_F64):
            return f64(SUBF64(x->f64, y->f64));
        case MTYPE2(-TYPE_F64, TYPE_I32):
            return __BINOP_A_V(x, y, f64, i32, f64, f64, SUBF64, len, offset, out);
        case MTYPE2(-TYPE_F64, TYPE_I64):
            return __BINOP_A_V(x, y, f64, i64, f64, f64, SUBF64, len, offset, out);
        case MTYPE2(-TYPE_F64, TYPE_F64):
            return __BINOP_A_V(x, y, f64, f64, f64, f64, SUBF64, len, offset, out);

        case MTYPE2(-TYPE_DATE, -TYPE_I32):
            return adate(SUBI32(x->i32, y->i32));
        case MTYPE2(-TYPE_DATE, -TYPE_I64):
            return adate(SUBI32(x->i32, i64_to_i32(y->i64)));
        case MTYPE2(-TYPE_DATE, -TYPE_DATE):
            return i32(SUBI32(x->i32, y->i32));
        case MTYPE2(-TYPE_DATE, -TYPE_TIME):
            return timestamp(SUBI64(date_to_timestamp(x->i32), time_to_timestamp(y->i32)));
        case MTYPE2(-TYPE_DATE, TYPE_I32):
            return __BINOP_A_V(x, y, i32, i32, date, date, SUBI32, len, offset, out);
        case MTYPE2(-TYPE_DATE, TYPE_I64):
            return __BINOP_A_V(x, y, i32, i64, date, date, SUBI32, len, offset, out);
        case MTYPE2(-TYPE_DATE, TYPE_DATE):
            return __BINOP_A_V(x, y, i32, i32, i32, i32, SUBI32, len, offset, out);
        case MTYPE2(-TYPE_DATE, TYPE_TIME):
            return __BINOP_A_V(x, y, date, time, timestamp, timestamp, SUBI64, len, offset, out);

        case MTYPE2(-TYPE_TIME, -TYPE_I32):
        case MTYPE2(-TYPE_TIME, -TYPE_TIME):
            return atime(SUBI32(x->i32, y->i32));
        case MTYPE2(-TYPE_TIME, -TYPE_I64):
            return atime(SUBI32(x->i32, i64_to_i32(y->i64)));
        case MTYPE2(-TYPE_TIME, TYPE_I32):
        case MTYPE2(-TYPE_TIME, TYPE_TIME):
            return __BINOP_A_V(x, y, i32, i32, time, time, SUBI32, len, offset, out);
        case MTYPE2(-TYPE_TIME, TYPE_I64):
            return __BINOP_A_V(x, y, i32, i64, time, time, SUBI32, len, offset, out);

        case MTYPE2(-TYPE_TIMESTAMP, -TYPE_I32):
            return timestamp(SUBI64(x->i64, i32_to_i64(y->i32)));
        case MTYPE2(-TYPE_TIMESTAMP, -TYPE_I64):
            return timestamp(SUBI64(x->i64, y->i64));
        case MTYPE2(-TYPE_TIMESTAMP, -TYPE_TIME):
            return timestamp(SUBI64(x->i64, time_to_timestamp(y->i32)));
        case MTYPE2(-TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
            return i64(SUBI64(x->i64, y->i64));
        case MTYPE2(-TYPE_TIMESTAMP, TYPE_I32):
            return __BINOP_A_V(x, y, i64, i32, timestamp, timestamp, SUBI64, len, offset, out);
        case MTYPE2(-TYPE_TIMESTAMP, TYPE_I64):
            return __BINOP_A_V(x, y, i64, i64, timestamp, timestamp, SUBI64, len, offset, out);
        case MTYPE2(-TYPE_TIMESTAMP, TYPE_TIME):
            return __BINOP_A_V(x, y, i64, time, timestamp, timestamp, SUBI64, len, offset, out);
        case MTYPE2(-TYPE_TIMESTAMP, TYPE_TIMESTAMP):
            return __BINOP_A_V(x, y, i64, i64, i64, i64, SUBI64, len, offset, out);

        case MTYPE2(TYPE_I32, -TYPE_I32):
            return __BINOP_V_A(x, y, i32, i32, i32, i32, SUBI32, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_I64):
            return __BINOP_V_A(x, y, i32, i64, i64, i64, SUBI64, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_F64):
            return __BINOP_V_A(x, y, i32, f64, f64, f64, SUBF64, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_TIME):
            return __BINOP_V_A(x, y, i32, i32, time, time, SUBI32, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_I32):
            return __BINOP_V_V(x, y, i32, i32, i32, i32, SUBI32, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_I64):
            return __BINOP_V_V(x, y, i32, i64, i64, i64, SUBI64, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_F64):
            return __BINOP_V_V(x, y, i32, f64, f64, f64, SUBF64, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_TIME):
            return __BINOP_V_V(x, y, i32, i32, time, time, SUBI32, len, offset, out);

        case MTYPE2(TYPE_I64, -TYPE_I32):
        case MTYPE2(TYPE_I64, -TYPE_I64):
            return __BINOP_V_A(x, y, i64, i64, i64, i64, SUBI64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_F64):
            return __BINOP_V_A(x, y, i64, f64, f64, f64, SUBF64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_TIME):
            return __BINOP_V_A(x, y, i64, i32, time, time, SUBI32, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_I32):
            return __BINOP_V_V(x, y, i64, i32, i64, i64, SUBI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_I64):
            return __BINOP_V_V(x, y, i64, i64, i64, i64, SUBI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_F64):
            return __BINOP_V_V(x, y, i64, f64, f64, f64, SUBF64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_TIME):
            return __BINOP_V_V(x, y, i64, i32, time, time, SUBI32, len, offset, out);

        case MTYPE2(TYPE_F64, -TYPE_I32):
            return __BINOP_V_A(x, y, f64, i32, f64, f64, SUBF64, len, offset, out);
        case MTYPE2(TYPE_F64, -TYPE_I64):
            return __BINOP_V_A(x, y, f64, i64, f64, f64, SUBF64, len, offset, out);
        case MTYPE2(TYPE_F64, -TYPE_F64):
            return __BINOP_V_A(x, y, f64, f64, f64, f64, SUBF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_I32):
            return __BINOP_V_V(x, y, f64, i32, f64, f64, SUBF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_I64):
            return __BINOP_V_V(x, y, f64, i64, f64, f64, SUBF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_F64):
            return __BINOP_V_V(x, y, f64, f64, f64, f64, SUBF64, len, offset, out);

        case MTYPE2(TYPE_DATE, -TYPE_I32):
            return __BINOP_V_A(x, y, i32, i32, date, date, SUBI32, len, offset, out);
        case MTYPE2(TYPE_DATE, -TYPE_I64):
            return __BINOP_V_A(x, y, i32, i64, date, date, SUBI32, len, offset, out);
        case MTYPE2(TYPE_DATE, -TYPE_DATE):
            return __BINOP_V_A(x, y, i32, i32, i32, i32, SUBI32, len, offset, out);
        case MTYPE2(TYPE_DATE, -TYPE_TIME):
            return __BINOP_V_A(x, y, date, time, timestamp, timestamp, SUBI64, len, offset, out);
        case MTYPE2(TYPE_DATE, TYPE_I32):
            return __BINOP_V_V(x, y, i32, i32, date, date, SUBI32, len, offset, out);
        case MTYPE2(TYPE_DATE, TYPE_I64):
            return __BINOP_V_V(x, y, i32, i64, date, date, SUBI32, len, offset, out);
        case MTYPE2(TYPE_DATE, TYPE_DATE):
            return __BINOP_V_V(x, y, i32, i32, i32, i32, SUBI32, len, offset, out);
        case MTYPE2(TYPE_DATE, TYPE_TIME):
            return __BINOP_V_V(x, y, date, time, timestamp, timestamp, SUBI64, len, offset, out);

        case MTYPE2(TYPE_TIME, -TYPE_I32):
        case MTYPE2(TYPE_TIME, -TYPE_TIME):
            return __BINOP_V_A(x, y, i32, i32, time, time, SUBI32, len, offset, out);
        case MTYPE2(TYPE_TIME, -TYPE_I64):
            return __BINOP_V_A(x, y, i32, i64, time, time, SUBI32, len, offset, out);
        case MTYPE2(TYPE_TIME, TYPE_I32):
        case MTYPE2(TYPE_TIME, TYPE_TIME):
            return __BINOP_V_V(x, y, i32, i32, time, time, SUBI32, len, offset, out);
        case MTYPE2(TYPE_TIME, TYPE_I64):
            return __BINOP_V_V(x, y, i32, i64, time, time, SUBI32, len, offset, out);

        case MTYPE2(TYPE_TIMESTAMP, -TYPE_I32):
            return __BINOP_V_A(x, y, i64, i32, timestamp, timestamp, SUBI64, len, offset, out);
        case MTYPE2(TYPE_TIMESTAMP, -TYPE_I64):
            return __BINOP_V_A(x, y, i64, i64, timestamp, timestamp, SUBI64, len, offset, out);
        case MTYPE2(TYPE_TIMESTAMP, -TYPE_TIME):
            return __BINOP_V_A(x, y, timestamp, time, timestamp, timestamp, SUBI64, len, offset, out);
        case MTYPE2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
            return __BINOP_V_A(x, y, i64, i64, i64, i64, SUBI64, len, offset, out);
        case MTYPE2(TYPE_TIMESTAMP, TYPE_I32):
            return __BINOP_V_V(x, y, i64, i32, timestamp, timestamp, SUBI64, len, offset, out);
        case MTYPE2(TYPE_TIMESTAMP, TYPE_I64):
            return __BINOP_V_V(x, y, i64, i64, timestamp, timestamp, SUBI64, len, offset, out);
        case MTYPE2(TYPE_TIMESTAMP, TYPE_TIME):
            return __BINOP_V_V(x, y, i64, time, timestamp, timestamp, SUBI64, len, offset, out);
        case MTYPE2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
            return __BINOP_V_V(x, y, i64, i64, i64, i64, SUBI64, len, offset, out);

        case MTYPE2(-TYPE_U8, -TYPE_U8):
            return u8(SUBU8(x->u8, y->u8));
        case MTYPE2(-TYPE_U8, -TYPE_I32):
            return i32(SUBI32(u8_to_i32(x->u8), y->i32));
        case MTYPE2(-TYPE_U8, -TYPE_I64):
            return i64(SUBI64(u8_to_i64(x->u8), y->i64));
        case MTYPE2(-TYPE_U8, -TYPE_F64):
            return f64(SUBF64(u8_to_f64(x->u8), y->f64));
        case MTYPE2(-TYPE_U8, TYPE_U8):
            return __BINOP_A_V(x, y, u8, u8, u8, u8, SUBU8, len, offset, out);
        case MTYPE2(-TYPE_U8, TYPE_I32):
            return __BINOP_A_V(x, y, u8, i32, i32, i32, SUBI32, len, offset, out);
        case MTYPE2(-TYPE_U8, TYPE_I64):
            return __BINOP_A_V(x, y, u8, i64, i64, i64, SUBI64, len, offset, out);
        case MTYPE2(-TYPE_U8, TYPE_F64):
            return __BINOP_A_V(x, y, u8, f64, f64, f64, SUBF64, len, offset, out);
        case MTYPE2(TYPE_U8, -TYPE_U8):
            return __BINOP_V_A(x, y, u8, u8, u8, u8, SUBU8, len, offset, out);
        case MTYPE2(TYPE_U8, -TYPE_I32):
            return __BINOP_V_A(x, y, u8, i32, i32, i32, SUBI32, len, offset, out);
        case MTYPE2(TYPE_U8, -TYPE_I64):
            return __BINOP_V_A(x, y, u8, i64, i64, i64, SUBI64, len, offset, out);
        case MTYPE2(TYPE_U8, -TYPE_F64):
            return __BINOP_V_A(x, y, u8, f64, f64, f64, SUBF64, len, offset, out);
        case MTYPE2(TYPE_U8, TYPE_U8):
            return __BINOP_V_V(x, y, u8, u8, u8, u8, SUBU8, len, offset, out);
        case MTYPE2(TYPE_U8, TYPE_I32):
            return __BINOP_V_V(x, y, u8, i32, i32, i32, SUBI32, len, offset, out);
        case MTYPE2(TYPE_U8, TYPE_I64):
            return __BINOP_V_V(x, y, u8, i64, i64, i64, SUBI64, len, offset, out);
        case MTYPE2(TYPE_U8, TYPE_F64):
            return __BINOP_V_V(x, y, u8, f64, f64, f64, SUBF64, len, offset, out);

        case MTYPE2(-TYPE_I16, -TYPE_I16):
            return i16(SUBI16(x->i16, y->i16));
        case MTYPE2(-TYPE_I16, -TYPE_I32):
            return i32(SUBI32(i16_to_i32(x->i16), y->i32));
        case MTYPE2(-TYPE_I16, -TYPE_I64):
            return i64(SUBI64(i16_to_i64(x->i16), y->i64));
        case MTYPE2(-TYPE_I16, -TYPE_F64):
            return f64(SUBF64(i16_to_f64(x->i16), y->f64));
        case MTYPE2(-TYPE_I16, TYPE_I16):
            return __BINOP_A_V(x, y, i16, i16, i16, i16, SUBI16, len, offset, out);
        case MTYPE2(-TYPE_I16, TYPE_I32):
            return __BINOP_A_V(x, y, i16, i32, i32, i32, SUBI32, len, offset, out);
        case MTYPE2(-TYPE_I16, TYPE_I64):
            return __BINOP_A_V(x, y, i16, i64, i64, i64, SUBI64, len, offset, out);
        case MTYPE2(-TYPE_I16, TYPE_F64):
            return __BINOP_A_V(x, y, i16, f64, f64, f64, SUBF64, len, offset, out);
        case MTYPE2(TYPE_I16, -TYPE_I16):
            return __BINOP_V_A(x, y, i16, i16, i16, i16, SUBI16, len, offset, out);
        case MTYPE2(TYPE_I16, -TYPE_I32):
            return __BINOP_V_A(x, y, i16, i32, i32, i32, SUBI32, len, offset, out);
        case MTYPE2(TYPE_I16, -TYPE_I64):
            return __BINOP_V_A(x, y, i16, i64, i64, i64, SUBI64, len, offset, out);
        case MTYPE2(TYPE_I16, -TYPE_F64):
            return __BINOP_V_A(x, y, i16, f64, f64, f64, SUBF64, len, offset, out);
        case MTYPE2(TYPE_I16, TYPE_I16):
            return __BINOP_V_V(x, y, i16, i16, i16, i16, SUBI16, len, offset, out);
        case MTYPE2(TYPE_I16, TYPE_I32):
            return __BINOP_V_V(x, y, i16, i32, i32, i32, SUBI32, len, offset, out);
        case MTYPE2(TYPE_I16, TYPE_I64):
            return __BINOP_V_V(x, y, i16, i64, i64, i64, SUBI64, len, offset, out);
        case MTYPE2(TYPE_I16, TYPE_F64):
            return __BINOP_V_V(x, y, i16, f64, f64, f64, SUBF64, len, offset, out);

        case MTYPE2(-TYPE_I32, -TYPE_U8):
            return i32(SUBI32(x->i32, u8_to_i32(y->u8)));
        case MTYPE2(-TYPE_I32, -TYPE_I16):
            return i32(SUBI32(x->i32, i16_to_i32(y->i16)));
        case MTYPE2(-TYPE_I32, TYPE_U8):
            return __BINOP_A_V(x, y, i32, u8, i32, i32, SUBI32, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_I16):
            return __BINOP_A_V(x, y, i32, i16, i32, i32, SUBI32, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_U8):
            return __BINOP_V_A(x, y, i32, u8, i32, i32, SUBI32, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_I16):
            return __BINOP_V_A(x, y, i32, i16, i32, i32, SUBI32, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_U8):
            return __BINOP_V_V(x, y, i32, u8, i32, i32, SUBI32, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_I16):
            return __BINOP_V_V(x, y, i32, i16, i32, i32, SUBI32, len, offset, out);

        case MTYPE2(-TYPE_I64, -TYPE_U8):
            return i64(SUBI64(x->i64, u8_to_i64(y->u8)));
        case MTYPE2(-TYPE_I64, -TYPE_I16):
            return i64(SUBI64(x->i64, i16_to_i64(y->i16)));
        case MTYPE2(-TYPE_I64, TYPE_U8):
            return __BINOP_A_V(x, y, i64, u8, i64, i64, SUBI64, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_I16):
            return __BINOP_A_V(x, y, i64, i16, i64, i64, SUBI64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_U8):
            return __BINOP_V_A(x, y, i64, u8, i64, i64, SUBI64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_I16):
            return __BINOP_V_A(x, y, i64, i16, i64, i64, SUBI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_U8):
            return __BINOP_V_V(x, y, i64, u8, i64, i64, SUBI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_I16):
            return __BINOP_V_V(x, y, i64, i16, i64, i64, SUBI64, len, offset, out);

        case MTYPE2(-TYPE_F64, -TYPE_U8):
            return f64(SUBF64(x->f64, u8_to_f64(y->u8)));
        case MTYPE2(-TYPE_F64, -TYPE_I16):
            return f64(SUBF64(x->f64, i16_to_f64(y->i16)));
        case MTYPE2(-TYPE_F64, TYPE_U8):
            return __BINOP_A_V(x, y, f64, u8, f64, f64, SUBF64, len, offset, out);
        case MTYPE2(-TYPE_F64, TYPE_I16):
            return __BINOP_A_V(x, y, f64, i16, f64, f64, SUBF64, len, offset, out);
        case MTYPE2(TYPE_F64, -TYPE_U8):
            return __BINOP_V_A(x, y, f64, u8, f64, f64, SUBF64, len, offset, out);
        case MTYPE2(TYPE_F64, -TYPE_I16):
            return __BINOP_V_A(x, y, f64, i16, f64, f64, SUBF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_U8):
            return __BINOP_V_V(x, y, f64, u8, f64, f64, SUBF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_I16):
            return __BINOP_V_V(x, y, f64, i16, f64, f64, SUBF64, len, offset, out);

        default:
            return err_new(EC_TYPE);
    }
}

obj_p ray_mul_partial(obj_p x, obj_p y, i64_t len, i64_t offset, obj_p out) {
    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(-TYPE_I32, -TYPE_I32):
            return i32(MULI32(x->i32, y->i32));
        case MTYPE2(-TYPE_I32, -TYPE_I64):
            return i64(MULI64(i32_to_i64(x->i32), y->i64));
        case MTYPE2(-TYPE_I32, -TYPE_F64):
            return f64(MULF64(i32_to_f64(x->i32), y->f64));
        case MTYPE2(-TYPE_I32, -TYPE_TIME):
            return atime(MULI32(x->i32, y->i32));
        case MTYPE2(-TYPE_I32, TYPE_I32):
            return __BINOP_A_V(x, y, i32, i32, i32, i32, MULI32, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_I64):
            return __BINOP_A_V(x, y, i32, i64, i64, i64, MULI64, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_F64):
            return __BINOP_A_V(x, y, i32, f64, f64, f64, MULF64, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_TIME):
            return __BINOP_A_V(x, y, i32, time, time, time, MULI32, len, offset, out);

        case MTYPE2(-TYPE_I64, -TYPE_I32):
            return i64(MULI64(x->i64, i32_to_i64(y->i32)));
        case MTYPE2(-TYPE_I64, -TYPE_I64):
            return i64(MULI64(x->i64, y->i64));
        case MTYPE2(-TYPE_I64, -TYPE_F64):
            return f64(MULF64(i64_to_f64(x->i64), y->f64));
        case MTYPE2(-TYPE_I64, -TYPE_TIME):
            return atime(MULI32(i64_to_i32(x->i64), y->i32));
        case MTYPE2(-TYPE_I64, TYPE_I32):
            return __BINOP_A_V(x, y, i64, i32, i64, i64, MULI64, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_I64):
            return __BINOP_A_V(x, y, i64, i64, i64, i64, MULI64, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_F64):
            return __BINOP_A_V(x, y, i64, f64, f64, f64, MULF64, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_TIME):
            return __BINOP_A_V(x, y, i64, time, time, time, MULI32, len, offset, out);

        case MTYPE2(-TYPE_F64, -TYPE_I32):
            return f64(MULF64(x->f64, i32_to_f64(y->i32)));
        case MTYPE2(-TYPE_F64, -TYPE_I64):
            return f64(MULF64(x->f64, i64_to_f64(y->i64)));
        case MTYPE2(-TYPE_F64, -TYPE_F64):
            return f64(MULF64(x->f64, y->f64));
        case MTYPE2(-TYPE_F64, TYPE_I32):
            return __BINOP_A_V(x, y, f64, i32, f64, f64, MULF64, len, offset, out);
        case MTYPE2(-TYPE_F64, TYPE_I64):
            return __BINOP_A_V(x, y, f64, i64, f64, f64, MULF64, len, offset, out);
        case MTYPE2(-TYPE_F64, TYPE_F64):
            return __BINOP_A_V(x, y, f64, f64, f64, f64, MULF64, len, offset, out);

        case MTYPE2(-TYPE_TIME, -TYPE_I32):
            return atime(MULI32(x->i32, y->i32));
        case MTYPE2(-TYPE_TIME, -TYPE_I64):
            return atime(MULI32(x->i32, i64_to_i32(y->i64)));
        case MTYPE2(-TYPE_TIME, TYPE_I32):
            return __BINOP_A_V(x, y, time, i32, time, time, MULI32, len, offset, out);
        case MTYPE2(-TYPE_TIME, TYPE_I64):
            return __BINOP_A_V(x, y, time, i64, time, time, MULI32, len, offset, out);

        case MTYPE2(TYPE_I32, -TYPE_I32):
            return __BINOP_V_A(x, y, i32, i32, i32, i32, MULI32, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_I64):
            return __BINOP_V_A(x, y, i32, i64, i64, i64, MULI64, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_F64):
            return __BINOP_V_A(x, y, i32, f64, f64, f64, MULF64, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_TIME):
            return __BINOP_V_A(x, y, i32, time, time, time, MULI32, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_I32):
            return __BINOP_V_V(x, y, i32, i32, i32, i32, MULI32, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_I64):
            return __BINOP_V_V(x, y, i32, i64, i64, i64, MULI64, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_F64):
            return __BINOP_V_V(x, y, i32, f64, f64, f64, MULF64, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_TIME):
            return __BINOP_V_V(x, y, i32, time, time, time, MULI32, len, offset, out);

        case MTYPE2(TYPE_I64, -TYPE_I32):
        case MTYPE2(TYPE_I64, -TYPE_I64):
            return __BINOP_V_A(x, y, i64, i64, i64, i64, MULI64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_F64):
            return __BINOP_V_A(x, y, i64, f64, f64, f64, MULF64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_TIME):
            return __BINOP_V_A(x, y, i64, time, time, time, MULI32, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_I32):
            return __BINOP_V_V(x, y, i64, i32, i64, i64, MULI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_I64):
            return __BINOP_V_V(x, y, i64, i64, i64, i64, MULI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_F64):
            return __BINOP_V_V(x, y, i64, f64, f64, f64, MULF64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_TIME):
            return __BINOP_V_V(x, y, i64, i32, time, time, MULI32, len, offset, out);

        case MTYPE2(TYPE_F64, -TYPE_I32):
            return __BINOP_V_A(x, y, f64, i32, f64, f64, MULF64, len, offset, out);
        case MTYPE2(TYPE_F64, -TYPE_I64):
            return __BINOP_V_A(x, y, f64, i64, f64, f64, MULF64, len, offset, out);
        case MTYPE2(TYPE_F64, -TYPE_F64):
            return __BINOP_V_A(x, y, f64, f64, f64, f64, MULF64, len, offset, out);
        case MTYPE2(TYPE_B8, TYPE_I64):
            return __BINOP_V_V(x, y, b8, i64, b8, i64, MULI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_B8):
            return __BINOP_V_V(x, y, i64, b8, i64, i64, MULI64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_I32):
            return __BINOP_V_V(x, y, f64, i32, f64, f64, MULF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_I64):
            return __BINOP_V_V(x, y, f64, i64, f64, f64, MULF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_F64):
            return __BINOP_V_V(x, y, f64, f64, f64, f64, MULF64, len, offset, out);
        case MTYPE2(TYPE_TIME, -TYPE_I32):
            return __BINOP_V_A(x, y, time, i32, time, time, MULI32, len, offset, out);
        case MTYPE2(TYPE_TIME, -TYPE_I64):
            return __BINOP_V_A(x, y, time, i64, time, time, MULI32, len, offset, out);
        case MTYPE2(TYPE_TIME, TYPE_I32):
            return __BINOP_V_V(x, y, time, i32, time, time, MULI32, len, offset, out);
        case MTYPE2(TYPE_TIME, TYPE_I64):
            return __BINOP_V_V(x, y, time, i64, time, time, MULI32, len, offset, out);

        case MTYPE2(-TYPE_U8, -TYPE_U8):
            return u8(MULU8(x->u8, y->u8));
        case MTYPE2(-TYPE_U8, -TYPE_I32):
            return i32(MULI32(u8_to_i32(x->u8), y->i32));
        case MTYPE2(-TYPE_U8, -TYPE_I64):
            return i64(MULI64(u8_to_i64(x->u8), y->i64));
        case MTYPE2(-TYPE_U8, -TYPE_F64):
            return f64(MULF64(u8_to_f64(x->u8), y->f64));
        case MTYPE2(-TYPE_U8, TYPE_U8):
            return __BINOP_A_V(x, y, u8, u8, u8, u8, MULU8, len, offset, out);
        case MTYPE2(-TYPE_U8, TYPE_I32):
            return __BINOP_A_V(x, y, u8, i32, i32, i32, MULI32, len, offset, out);
        case MTYPE2(-TYPE_U8, TYPE_I64):
            return __BINOP_A_V(x, y, u8, i64, i64, i64, MULI64, len, offset, out);
        case MTYPE2(-TYPE_U8, TYPE_F64):
            return __BINOP_A_V(x, y, u8, f64, f64, f64, MULF64, len, offset, out);
        case MTYPE2(TYPE_U8, -TYPE_U8):
            return __BINOP_V_A(x, y, u8, u8, u8, u8, MULU8, len, offset, out);
        case MTYPE2(TYPE_U8, -TYPE_I32):
            return __BINOP_V_A(x, y, u8, i32, i32, i32, MULI32, len, offset, out);
        case MTYPE2(TYPE_U8, -TYPE_I64):
            return __BINOP_V_A(x, y, u8, i64, i64, i64, MULI64, len, offset, out);
        case MTYPE2(TYPE_U8, -TYPE_F64):
            return __BINOP_V_A(x, y, u8, f64, f64, f64, MULF64, len, offset, out);
        case MTYPE2(TYPE_U8, TYPE_U8):
            return __BINOP_V_V(x, y, u8, u8, u8, u8, MULU8, len, offset, out);
        case MTYPE2(TYPE_U8, TYPE_I32):
            return __BINOP_V_V(x, y, u8, i32, i32, i32, MULI32, len, offset, out);
        case MTYPE2(TYPE_U8, TYPE_I64):
            return __BINOP_V_V(x, y, u8, i64, i64, i64, MULI64, len, offset, out);
        case MTYPE2(TYPE_U8, TYPE_F64):
            return __BINOP_V_V(x, y, u8, f64, f64, f64, MULF64, len, offset, out);

        case MTYPE2(-TYPE_I16, -TYPE_I16):
            return i16(MULI16(x->i16, y->i16));
        case MTYPE2(-TYPE_I16, -TYPE_I32):
            return i32(MULI32(i16_to_i32(x->i16), y->i32));
        case MTYPE2(-TYPE_I16, -TYPE_I64):
            return i64(MULI64(i16_to_i64(x->i16), y->i64));
        case MTYPE2(-TYPE_I16, -TYPE_F64):
            return f64(MULF64(i16_to_f64(x->i16), y->f64));
        case MTYPE2(-TYPE_I16, TYPE_I16):
            return __BINOP_A_V(x, y, i16, i16, i16, i16, MULI16, len, offset, out);
        case MTYPE2(-TYPE_I16, TYPE_I32):
            return __BINOP_A_V(x, y, i16, i32, i32, i32, MULI32, len, offset, out);
        case MTYPE2(-TYPE_I16, TYPE_I64):
            return __BINOP_A_V(x, y, i16, i64, i64, i64, MULI64, len, offset, out);
        case MTYPE2(-TYPE_I16, TYPE_F64):
            return __BINOP_A_V(x, y, i16, f64, f64, f64, MULF64, len, offset, out);
        case MTYPE2(TYPE_I16, -TYPE_I16):
            return __BINOP_V_A(x, y, i16, i16, i16, i16, MULI16, len, offset, out);
        case MTYPE2(TYPE_I16, -TYPE_I32):
            return __BINOP_V_A(x, y, i16, i32, i32, i32, MULI32, len, offset, out);
        case MTYPE2(TYPE_I16, -TYPE_I64):
            return __BINOP_V_A(x, y, i16, i64, i64, i64, MULI64, len, offset, out);
        case MTYPE2(TYPE_I16, -TYPE_F64):
            return __BINOP_V_A(x, y, i16, f64, f64, f64, MULF64, len, offset, out);
        case MTYPE2(TYPE_I16, TYPE_I16):
            return __BINOP_V_V(x, y, i16, i16, i16, i16, MULI16, len, offset, out);
        case MTYPE2(TYPE_I16, TYPE_I32):
            return __BINOP_V_V(x, y, i16, i32, i32, i32, MULI32, len, offset, out);
        case MTYPE2(TYPE_I16, TYPE_I64):
            return __BINOP_V_V(x, y, i16, i64, i64, i64, MULI64, len, offset, out);
        case MTYPE2(TYPE_I16, TYPE_F64):
            return __BINOP_V_V(x, y, i16, f64, f64, f64, MULF64, len, offset, out);

        case MTYPE2(-TYPE_I32, -TYPE_U8):
            return i32(MULI32(x->i32, u8_to_i32(y->u8)));
        case MTYPE2(-TYPE_I32, -TYPE_I16):
            return i32(MULI32(x->i32, i16_to_i32(y->i16)));
        case MTYPE2(-TYPE_I32, TYPE_U8):
            return __BINOP_A_V(x, y, i32, u8, i32, i32, MULI32, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_I16):
            return __BINOP_A_V(x, y, i32, i16, i32, i32, MULI32, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_U8):
            return __BINOP_V_A(x, y, i32, u8, i32, i32, MULI32, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_I16):
            return __BINOP_V_A(x, y, i32, i16, i32, i32, MULI32, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_U8):
            return __BINOP_V_V(x, y, i32, u8, i32, i32, MULI32, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_I16):
            return __BINOP_V_V(x, y, i32, i16, i32, i32, MULI32, len, offset, out);

        case MTYPE2(-TYPE_I64, -TYPE_U8):
            return i64(MULI64(x->i64, u8_to_i64(y->u8)));
        case MTYPE2(-TYPE_I64, -TYPE_I16):
            return i64(MULI64(x->i64, i16_to_i64(y->i16)));
        case MTYPE2(-TYPE_I64, TYPE_U8):
            return __BINOP_A_V(x, y, i64, u8, i64, i64, MULI64, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_I16):
            return __BINOP_A_V(x, y, i64, i16, i64, i64, MULI64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_U8):
            return __BINOP_V_A(x, y, i64, u8, i64, i64, MULI64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_I16):
            return __BINOP_V_A(x, y, i64, i16, i64, i64, MULI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_U8):
            return __BINOP_V_V(x, y, i64, u8, i64, i64, MULI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_I16):
            return __BINOP_V_V(x, y, i64, i16, i64, i64, MULI64, len, offset, out);

        case MTYPE2(-TYPE_F64, -TYPE_U8):
            return f64(MULF64(x->f64, u8_to_f64(y->u8)));
        case MTYPE2(-TYPE_F64, -TYPE_I16):
            return f64(MULF64(x->f64, i16_to_f64(y->i16)));
        case MTYPE2(-TYPE_F64, TYPE_U8):
            return __BINOP_A_V(x, y, f64, u8, f64, f64, MULF64, len, offset, out);
        case MTYPE2(-TYPE_F64, TYPE_I16):
            return __BINOP_A_V(x, y, f64, i16, f64, f64, MULF64, len, offset, out);
        case MTYPE2(TYPE_F64, -TYPE_U8):
            return __BINOP_V_A(x, y, f64, u8, f64, f64, MULF64, len, offset, out);
        case MTYPE2(TYPE_F64, -TYPE_I16):
            return __BINOP_V_A(x, y, f64, i16, f64, f64, MULF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_U8):
            return __BINOP_V_V(x, y, f64, u8, f64, f64, MULF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_I16):
            return __BINOP_V_V(x, y, f64, i16, f64, f64, MULF64, len, offset, out);

        default:
            return err_new(EC_TYPE);
    }
}

obj_p ray_div_partial(obj_p x, obj_p y, i64_t len, i64_t offset, obj_p out) {
    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(-TYPE_I32, -TYPE_I32):
            return i32(DIVI32(x->i32, y->i32));
        case MTYPE2(-TYPE_I32, -TYPE_I64):
            return i32(i64_to_i32(DIVI64(i32_to_i64(x->i32), y->i64)));
        case MTYPE2(-TYPE_I32, -TYPE_F64):
            return i32(f64_to_i32(DIVF64(i32_to_f64(x->i32), y->f64)));
        case MTYPE2(-TYPE_I32, TYPE_I32):
            return __BINOP_A_V(x, y, i32, i32, i32, i32, DIVI32, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_I64):
            return __BINOP_A_V(x, y, i32, i64, i32, i64, DIVI64, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_F64):
            return __BINOP_A_V(x, y, i32, f64, i32, f64, DIVF64, len, offset, out);

        case MTYPE2(-TYPE_I64, -TYPE_I32):
            return i64(DIVI64(x->i64, i32_to_i64(y->i32)));
        case MTYPE2(-TYPE_I64, -TYPE_I64):
            return i64(DIVI64(x->i64, y->i64));
        case MTYPE2(-TYPE_I64, -TYPE_F64):
            return i64(f64_to_i64(DIVF64(i64_to_f64(x->i64), y->f64)));
        case MTYPE2(-TYPE_I64, TYPE_I32):
            return __BINOP_A_V(x, y, i64, i32, i64, i64, DIVI64, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_I64):
            return __BINOP_A_V(x, y, i64, i64, i64, i64, DIVI64, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_F64):
            return __BINOP_A_V(x, y, i64, f64, i64, f64, DIVF64, len, offset, out);

        case MTYPE2(-TYPE_F64, -TYPE_I32):
            return f64(DIVF64(x->f64, i32_to_f64(y->i32)));
        case MTYPE2(-TYPE_F64, -TYPE_I64):
            return f64(DIVF64(x->f64, i64_to_f64(y->i64)));
        case MTYPE2(-TYPE_F64, -TYPE_F64):
            return f64(DIVF64(x->f64, y->f64));
        case MTYPE2(-TYPE_F64, TYPE_I32):
            return __BINOP_A_V(x, y, f64, i32, f64, f64, DIVF64, len, offset, out);
        case MTYPE2(-TYPE_F64, TYPE_I64):
            return __BINOP_A_V(x, y, f64, i64, f64, f64, DIVF64, len, offset, out);
        case MTYPE2(-TYPE_F64, TYPE_F64):
            return __BINOP_A_V(x, y, f64, f64, f64, f64, DIVF64, len, offset, out);

        case MTYPE2(TYPE_I32, -TYPE_I32):
            return __BINOP_V_A(x, y, i32, i32, i32, i32, DIVI32, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_I64):
            return __BINOP_V_A(x, y, i32, i64, i32, i64, DIVI64, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_F64):
            return __BINOP_V_A(x, y, i32, f64, i32, f64, DIVF64, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_I32):
            return __BINOP_V_V(x, y, i32, i32, i32, i32, DIVI32, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_I64):
            return __BINOP_V_V(x, y, i32, i64, i32, i64, DIVI64, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_F64):
            return __BINOP_V_V(x, y, i32, f64, i32, f64, DIVF64, len, offset, out);

        case MTYPE2(TYPE_I64, -TYPE_I32):
            return __BINOP_V_A(x, y, i64, i32, i64, i64, DIVI64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_I64):
            return __BINOP_V_A(x, y, i64, i64, i64, i64, DIVI64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_F64):
            return __BINOP_V_A(x, y, i64, f64, i64, f64, DIVF64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_I32):
            return __BINOP_V_V(x, y, i64, i32, i64, i64, DIVI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_I64):
            return __BINOP_V_V(x, y, i64, i64, i64, i64, DIVI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_F64):
            return __BINOP_V_V(x, y, i64, f64, i64, f64, DIVF64, len, offset, out);

        case MTYPE2(TYPE_F64, -TYPE_I32):
            return __BINOP_V_A(x, y, f64, i32, f64, f64, DIVF64, len, offset, out);
        case MTYPE2(TYPE_F64, -TYPE_I64):
            return __BINOP_V_A(x, y, f64, i64, f64, f64, DIVF64, len, offset, out);
        case MTYPE2(TYPE_F64, -TYPE_F64):
            return __BINOP_V_A(x, y, f64, f64, f64, f64, DIVF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_I32):
            return __BINOP_V_V(x, y, f64, i32, f64, f64, DIVF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_I64):
            return __BINOP_V_V(x, y, f64, i64, f64, f64, DIVF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_F64):
            return __BINOP_V_V(x, y, f64, f64, f64, f64, DIVF64, len, offset, out);

        case MTYPE2(-TYPE_TIME, -TYPE_I32):
            return atime(DIVI32(x->i32, y->i32));
        case MTYPE2(-TYPE_TIME, -TYPE_I64):
            return atime(DIVI32(x->i32, i64_to_i32(y->i64)));
        case MTYPE2(-TYPE_TIME, -TYPE_F64):
            return atime(f64_to_i32(DIVF64(i32_to_f64(x->i32), y->f64)));
        case MTYPE2(-TYPE_TIME, TYPE_I32):
            return __BINOP_A_V(x, y, time, i32, time, time, DIVI32, len, offset, out);
        case MTYPE2(-TYPE_TIME, TYPE_I64):
            return __BINOP_A_V(x, y, time, i64, time, time, DIVI32, len, offset, out);
        case MTYPE2(-TYPE_TIME, TYPE_F64):
            return __BINOP_A_V(x, y, time, f64, time, f64, DIVF64, len, offset, out);
        case MTYPE2(TYPE_TIME, -TYPE_I32):
            return __BINOP_V_A(x, y, time, i32, time, time, DIVI32, len, offset, out);
        case MTYPE2(TYPE_TIME, -TYPE_I64):
            return __BINOP_V_A(x, y, time, i64, time, i64, DIVI64, len, offset, out);
        case MTYPE2(TYPE_TIME, -TYPE_F64):
            return __BINOP_V_A(x, y, time, f64, time, f64, DIVF64, len, offset, out);
        case MTYPE2(TYPE_TIME, TYPE_I32):
            return __BINOP_V_V(x, y, time, i32, time, time, DIVI32, len, offset, out);
        case MTYPE2(TYPE_TIME, TYPE_I64):
            return __BINOP_V_V(x, y, time, i64, time, i64, DIVI64, len, offset, out);
        case MTYPE2(TYPE_TIME, TYPE_F64):
            return __BINOP_V_V(x, y, time, f64, time, f64, DIVF64, len, offset, out);

        case MTYPE2(-TYPE_U8, -TYPE_U8):
            return u8(DIVU8(x->u8, y->u8));
        case MTYPE2(-TYPE_U8, -TYPE_I32):
            return i32(DIVI32(u8_to_i32(x->u8), y->i32));
        case MTYPE2(-TYPE_U8, -TYPE_I64):
            return i64(DIVI64(u8_to_i64(x->u8), y->i64));
        case MTYPE2(-TYPE_U8, -TYPE_F64):
            return f64(DIVF64(u8_to_f64(x->u8), y->f64));
        case MTYPE2(-TYPE_U8, TYPE_U8):
            return __BINOP_A_V(x, y, u8, u8, u8, u8, DIVU8, len, offset, out);
        case MTYPE2(-TYPE_U8, TYPE_I32):
            return __BINOP_A_V(x, y, u8, i32, i32, i32, DIVI32, len, offset, out);
        case MTYPE2(-TYPE_U8, TYPE_I64):
            return __BINOP_A_V(x, y, u8, i64, i64, i64, DIVI64, len, offset, out);
        case MTYPE2(-TYPE_U8, TYPE_F64):
            return __BINOP_A_V(x, y, u8, f64, f64, f64, DIVF64, len, offset, out);
        case MTYPE2(TYPE_U8, -TYPE_U8):
            return __BINOP_V_A(x, y, u8, u8, u8, u8, DIVU8, len, offset, out);
        case MTYPE2(TYPE_U8, -TYPE_I32):
            return __BINOP_V_A(x, y, u8, i32, i32, i32, DIVI32, len, offset, out);
        case MTYPE2(TYPE_U8, -TYPE_I64):
            return __BINOP_V_A(x, y, u8, i64, i64, i64, DIVI64, len, offset, out);
        case MTYPE2(TYPE_U8, -TYPE_F64):
            return __BINOP_V_A(x, y, u8, f64, f64, f64, DIVF64, len, offset, out);
        case MTYPE2(TYPE_U8, TYPE_U8):
            return __BINOP_V_V(x, y, u8, u8, u8, u8, DIVU8, len, offset, out);
        case MTYPE2(TYPE_U8, TYPE_I32):
            return __BINOP_V_V(x, y, u8, i32, i32, i32, DIVI32, len, offset, out);
        case MTYPE2(TYPE_U8, TYPE_I64):
            return __BINOP_V_V(x, y, u8, i64, i64, i64, DIVI64, len, offset, out);
        case MTYPE2(TYPE_U8, TYPE_F64):
            return __BINOP_V_V(x, y, u8, f64, f64, f64, DIVF64, len, offset, out);

        case MTYPE2(-TYPE_I16, -TYPE_I16):
            return i16(DIVI16(x->i16, y->i16));
        case MTYPE2(-TYPE_I16, -TYPE_I32):
            return i32(DIVI32(i16_to_i32(x->i16), y->i32));
        case MTYPE2(-TYPE_I16, -TYPE_I64):
            return i64(DIVI64(i16_to_i64(x->i16), y->i64));
        case MTYPE2(-TYPE_I16, -TYPE_F64):
            return f64(DIVF64(i16_to_f64(x->i16), y->f64));
        case MTYPE2(-TYPE_I16, TYPE_I16):
            return __BINOP_A_V(x, y, i16, i16, i16, i16, DIVI16, len, offset, out);
        case MTYPE2(-TYPE_I16, TYPE_I32):
            return __BINOP_A_V(x, y, i16, i32, i32, i32, DIVI32, len, offset, out);
        case MTYPE2(-TYPE_I16, TYPE_I64):
            return __BINOP_A_V(x, y, i16, i64, i64, i64, DIVI64, len, offset, out);
        case MTYPE2(-TYPE_I16, TYPE_F64):
            return __BINOP_A_V(x, y, i16, f64, f64, f64, DIVF64, len, offset, out);
        case MTYPE2(TYPE_I16, -TYPE_I16):
            return __BINOP_V_A(x, y, i16, i16, i16, i16, DIVI16, len, offset, out);
        case MTYPE2(TYPE_I16, -TYPE_I32):
            return __BINOP_V_A(x, y, i16, i32, i32, i32, DIVI32, len, offset, out);
        case MTYPE2(TYPE_I16, -TYPE_I64):
            return __BINOP_V_A(x, y, i16, i64, i64, i64, DIVI64, len, offset, out);
        case MTYPE2(TYPE_I16, -TYPE_F64):
            return __BINOP_V_A(x, y, i16, f64, f64, f64, DIVF64, len, offset, out);
        case MTYPE2(TYPE_I16, TYPE_I16):
            return __BINOP_V_V(x, y, i16, i16, i16, i16, DIVI16, len, offset, out);
        case MTYPE2(TYPE_I16, TYPE_I32):
            return __BINOP_V_V(x, y, i16, i32, i32, i32, DIVI32, len, offset, out);
        case MTYPE2(TYPE_I16, TYPE_I64):
            return __BINOP_V_V(x, y, i16, i64, i64, i64, DIVI64, len, offset, out);
        case MTYPE2(TYPE_I16, TYPE_F64):
            return __BINOP_V_V(x, y, i16, f64, f64, f64, DIVF64, len, offset, out);

        case MTYPE2(-TYPE_I32, -TYPE_U8):
            return i32(DIVI32(x->i32, u8_to_i32(y->u8)));
        case MTYPE2(-TYPE_I32, -TYPE_I16):
            return i32(DIVI32(x->i32, i16_to_i32(y->i16)));
        case MTYPE2(-TYPE_I32, TYPE_U8):
            return __BINOP_A_V(x, y, i32, u8, i32, i32, DIVI32, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_I16):
            return __BINOP_A_V(x, y, i32, i16, i32, i32, DIVI32, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_U8):
            return __BINOP_V_A(x, y, i32, u8, i32, i32, DIVI32, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_I16):
            return __BINOP_V_A(x, y, i32, i16, i32, i32, DIVI32, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_U8):
            return __BINOP_V_V(x, y, i32, u8, i32, i32, DIVI32, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_I16):
            return __BINOP_V_V(x, y, i32, i16, i32, i32, DIVI32, len, offset, out);

        case MTYPE2(-TYPE_I64, -TYPE_U8):
            return i64(DIVI64(x->i64, u8_to_i64(y->u8)));
        case MTYPE2(-TYPE_I64, -TYPE_I16):
            return i64(DIVI64(x->i64, i16_to_i64(y->i16)));
        case MTYPE2(-TYPE_I64, TYPE_U8):
            return __BINOP_A_V(x, y, i64, u8, i64, i64, DIVI64, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_I16):
            return __BINOP_A_V(x, y, i64, i16, i64, i64, DIVI64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_U8):
            return __BINOP_V_A(x, y, i64, u8, i64, i64, DIVI64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_I16):
            return __BINOP_V_A(x, y, i64, i16, i64, i64, DIVI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_U8):
            return __BINOP_V_V(x, y, i64, u8, i64, i64, DIVI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_I16):
            return __BINOP_V_V(x, y, i64, i16, i64, i64, DIVI64, len, offset, out);

        case MTYPE2(-TYPE_F64, -TYPE_U8):
            return f64(DIVF64(x->f64, u8_to_f64(y->u8)));
        case MTYPE2(-TYPE_F64, -TYPE_I16):
            return f64(DIVF64(x->f64, i16_to_f64(y->i16)));
        case MTYPE2(-TYPE_F64, TYPE_U8):
            return __BINOP_A_V(x, y, f64, u8, f64, f64, DIVF64, len, offset, out);
        case MTYPE2(-TYPE_F64, TYPE_I16):
            return __BINOP_A_V(x, y, f64, i16, f64, f64, DIVF64, len, offset, out);
        case MTYPE2(TYPE_F64, -TYPE_U8):
            return __BINOP_V_A(x, y, f64, u8, f64, f64, DIVF64, len, offset, out);
        case MTYPE2(TYPE_F64, -TYPE_I16):
            return __BINOP_V_A(x, y, f64, i16, f64, f64, DIVF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_U8):
            return __BINOP_V_V(x, y, f64, u8, f64, f64, DIVF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_I16):
            return __BINOP_V_V(x, y, f64, i16, f64, f64, DIVF64, len, offset, out);

        default:
            return err_new(EC_TYPE);
    }
}

obj_p ray_fdiv_partial(obj_p x, obj_p y, i64_t len, i64_t offset, obj_p out) {
    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(-TYPE_I32, -TYPE_I32):
            return f64(FDIVI32(x->i32, y->i32));
        case MTYPE2(-TYPE_I32, -TYPE_I64):
            return f64(FDIVI64(i32_to_i64(x->i32), y->i64));
        case MTYPE2(-TYPE_I32, -TYPE_F64):
            return f64(FDIVF64(i32_to_f64(x->i32), y->f64));
        case MTYPE2(-TYPE_I32, TYPE_I32):
            return __BINOP_A_V(x, y, i32, i32, f64, f64, FDIVI64, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_I64):
            return __BINOP_A_V(x, y, i32, i64, f64, f64, FDIVI64, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_F64):
            return __BINOP_A_V(x, y, i32, f64, f64, f64, FDIVF64, len, offset, out);

        case MTYPE2(-TYPE_I64, -TYPE_I32):
            return f64(FDIVI64(x->i64, i32_to_i64(y->i32)));
        case MTYPE2(-TYPE_I64, -TYPE_I64):
            return f64(FDIVI64(x->i64, y->i64));
        case MTYPE2(-TYPE_I64, -TYPE_F64):
            return f64(FDIVI64(x->i64, y->f64));
        case MTYPE2(-TYPE_I64, TYPE_I32):
            return __BINOP_A_V(x, y, i64, i32, f64, f64, FDIVI64, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_I64):
            return __BINOP_A_V(x, y, i64, i64, f64, f64, FDIVI64, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_F64):
            return __BINOP_A_V(x, y, i64, f64, f64, f64, FDIVI64, len, offset, out);

        case MTYPE2(-TYPE_F64, -TYPE_I32):
            return f64(FDIVF64(x->f64, i32_to_f64(y->i32)));
        case MTYPE2(-TYPE_F64, -TYPE_I64):
            return f64(FDIVF64(x->f64, i64_to_f64(y->i64)));
        case MTYPE2(-TYPE_F64, -TYPE_F64):
            return f64(FDIVF64(x->f64, y->f64));
        case MTYPE2(-TYPE_F64, TYPE_I32):
            return __BINOP_A_V(x, y, f64, i32, f64, f64, FDIVF64, len, offset, out);
        case MTYPE2(-TYPE_F64, TYPE_I64):
            return __BINOP_A_V(x, y, f64, i64, f64, f64, FDIVF64, len, offset, out);
        case MTYPE2(-TYPE_F64, TYPE_F64):
            return __BINOP_A_V(x, y, f64, f64, f64, f64, FDIVF64, len, offset, out);

        case MTYPE2(TYPE_I32, -TYPE_I32):
            return __BINOP_V_A(x, y, i32, i32, f64, f64, FDIVI64, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_I64):
            return __BINOP_V_A(x, y, i32, i64, f64, f64, FDIVI64, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_F64):
            return __BINOP_V_A(x, y, i32, f64, f64, f64, FDIVF64, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_I32):
            return __BINOP_V_V(x, y, i32, i32, f64, f64, FDIVI64, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_I64):
            return __BINOP_V_V(x, y, i32, i64, f64, f64, FDIVI64, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_F64):
            return __BINOP_V_V(x, y, i32, f64, f64, f64, FDIVF64, len, offset, out);

        case MTYPE2(TYPE_I64, -TYPE_I32):
            return __BINOP_V_A(x, y, i64, i32, f64, f64, FDIVI64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_I64):
            return __BINOP_V_A(x, y, i64, i64, f64, f64, FDIVI64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_F64):
            return __BINOP_V_A(x, y, i64, f64, f64, f64, FDIVI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_I32):
            return __BINOP_V_V(x, y, i64, i32, f64, f64, FDIVI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_I64):
            return __BINOP_V_V(x, y, i64, i64, f64, f64, FDIVI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_F64):
            return __BINOP_V_V(x, y, i64, f64, f64, f64, FDIVI64, len, offset, out);

        case MTYPE2(TYPE_F64, -TYPE_I32):
            return __BINOP_V_A(x, y, f64, i32, f64, f64, FDIVF64, len, offset, out);
        case MTYPE2(TYPE_F64, -TYPE_I64):
            return __BINOP_V_A(x, y, f64, i64, f64, f64, FDIVF64, len, offset, out);
        case MTYPE2(TYPE_F64, -TYPE_F64):
            return __BINOP_V_A(x, y, f64, f64, f64, f64, FDIVF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_I32):
            return __BINOP_V_V(x, y, f64, i32, f64, f64, FDIVF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_I64):
            return __BINOP_V_V(x, y, f64, i64, f64, f64, FDIVF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_F64):
            return __BINOP_V_V(x, y, f64, f64, f64, f64, FDIVF64, len, offset, out);
        default:
            return err_new(EC_TYPE);
    }
}

obj_p ray_mod_partial(obj_p x, obj_p y, i64_t len, i64_t offset, obj_p out) {
    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(-TYPE_I32, -TYPE_I32):
            return i32(MODI32(x->i32, y->i32));
        case MTYPE2(-TYPE_I32, -TYPE_I64):
            return i64(MODI64(i32_to_i64(x->i32), y->i64));
        case MTYPE2(-TYPE_I32, -TYPE_F64):
            return f64(MODF64(i32_to_f64(x->i32), y->f64));
        case MTYPE2(-TYPE_I32, TYPE_I32):
            return __BINOP_A_V(x, y, i32, i32, i32, i32, MODI32, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_I64):
            return __BINOP_A_V(x, y, i32, i64, i64, i64, MODI64, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_F64):
            return __BINOP_A_V(x, y, i32, f64, f64, f64, MODF64, len, offset, out);

        case MTYPE2(-TYPE_I64, -TYPE_I32):
            return i32(i64_to_i32(MODI64(x->i64, i32_to_i64(y->i32))));
        case MTYPE2(-TYPE_I64, -TYPE_I64):
            return i64(MODI64(x->i64, y->i64));
        case MTYPE2(-TYPE_I64, -TYPE_F64):
            return f64(MODF64(i64_to_f64(x->i64), y->f64));
        case MTYPE2(-TYPE_I64, TYPE_I32):
            return __BINOP_A_V(x, y, i64, i32, i32, i64, MODI64, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_I64):
            return __BINOP_A_V(x, y, i64, i64, i64, i64, MODI64, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_F64):
            return __BINOP_A_V(x, y, i64, f64, f64, f64, MODF64, len, offset, out);

        case MTYPE2(-TYPE_F64, -TYPE_I32):
            return f64(MODF64(x->f64, i32_to_f64(y->i32)));
        case MTYPE2(-TYPE_F64, -TYPE_I64):
            return f64(MODF64(x->f64, i64_to_f64(y->i64)));
        case MTYPE2(-TYPE_F64, -TYPE_F64):
            return f64(MODF64(x->f64, y->f64));
        case MTYPE2(-TYPE_F64, TYPE_I32):
            return __BINOP_A_V(x, y, f64, i32, f64, f64, MODF64, len, offset, out);
        case MTYPE2(-TYPE_F64, TYPE_I64):
            return __BINOP_A_V(x, y, f64, i64, f64, f64, MODF64, len, offset, out);
        case MTYPE2(-TYPE_F64, TYPE_F64):
            return __BINOP_A_V(x, y, f64, f64, f64, f64, MODF64, len, offset, out);

        case MTYPE2(TYPE_I32, -TYPE_I32):
            return __BINOP_V_A(x, y, i32, i32, i32, i32, MODI32, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_I64):
            return __BINOP_V_A(x, y, i32, i64, i64, i64, MODI64, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_F64):
            return __BINOP_V_A(x, y, i32, f64, f64, f64, MODF64, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_I32):
            return __BINOP_V_V(x, y, i32, i32, i32, i32, MODI32, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_I64):
            return __BINOP_V_V(x, y, i32, i64, i64, i64, MODI64, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_F64):
            return __BINOP_V_V(x, y, i32, f64, f64, f64, MODF64, len, offset, out);

        case MTYPE2(TYPE_I64, -TYPE_I32):
            return __BINOP_V_A(x, y, i64, i32, i32, i64, MODI64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_I64):
            return __BINOP_V_A(x, y, i64, i64, i64, i64, MODI64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_F64):
            return __BINOP_V_A(x, y, i64, f64, f64, f64, MODF64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_I32):
            return __BINOP_V_V(x, y, i64, i32, i32, i64, MODI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_I64):
            return __BINOP_V_V(x, y, i64, i64, i64, i64, MODI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_F64):
            return __BINOP_V_V(x, y, i64, f64, f64, f64, MODF64, len, offset, out);

        case MTYPE2(TYPE_F64, -TYPE_I32):
            return __BINOP_V_A(x, y, f64, i32, f64, f64, MODF64, len, offset, out);
        case MTYPE2(TYPE_F64, -TYPE_I64):
            return __BINOP_V_A(x, y, f64, i64, f64, f64, MODF64, len, offset, out);
        case MTYPE2(TYPE_F64, -TYPE_F64):
            return __BINOP_V_A(x, y, f64, f64, f64, f64, MODF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_I32):
            return __BINOP_V_V(x, y, f64, i32, f64, f64, MODF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_I64):
            return __BINOP_V_V(x, y, f64, i64, f64, f64, MODF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_F64):
            return __BINOP_V_V(x, y, f64, f64, f64, f64, MODF64, len, offset, out);

        case MTYPE2(-TYPE_TIME, -TYPE_I32):
            return atime(MODI32(x->i32, y->i32));
        case MTYPE2(-TYPE_TIME, -TYPE_I64):
            return atime(MODI32(x->i32, i64_to_i32(y->i64)));
        case MTYPE2(-TYPE_TIME, TYPE_I32):
            return __BINOP_A_V(x, y, time, i32, time, time, MODI32, len, offset, out);
        case MTYPE2(-TYPE_TIME, TYPE_I64):
            return __BINOP_A_V(x, y, time, i64, time, time, MODI32, len, offset, out);
        case MTYPE2(TYPE_TIME, -TYPE_I32):
            return __BINOP_V_A(x, y, time, i32, time, time, MODI32, len, offset, out);
        case MTYPE2(TYPE_TIME, -TYPE_I64):
            return __BINOP_V_A(x, y, time, i64, time, i64, MODI64, len, offset, out);
        case MTYPE2(TYPE_TIME, TYPE_I32):
            return __BINOP_V_V(x, y, time, i32, time, time, MODI32, len, offset, out);
        case MTYPE2(TYPE_TIME, TYPE_I64):
            return __BINOP_V_V(x, y, time, i64, time, i64, MODI64, len, offset, out);

        case MTYPE2(-TYPE_U8, -TYPE_U8):
            return u8(MODU8(x->u8, y->u8));
        case MTYPE2(-TYPE_U8, -TYPE_I32):
            return i32(MODI32(u8_to_i32(x->u8), y->i32));
        case MTYPE2(-TYPE_U8, -TYPE_I64):
            return i64(MODI64(u8_to_i64(x->u8), y->i64));
        case MTYPE2(-TYPE_U8, TYPE_U8):
            return __BINOP_A_V(x, y, u8, u8, u8, u8, MODU8, len, offset, out);
        case MTYPE2(-TYPE_U8, TYPE_I32):
            return __BINOP_A_V(x, y, u8, i32, i32, i32, MODI32, len, offset, out);
        case MTYPE2(-TYPE_U8, TYPE_I64):
            return __BINOP_A_V(x, y, u8, i64, i64, i64, MODI64, len, offset, out);
        case MTYPE2(TYPE_U8, -TYPE_U8):
            return __BINOP_V_A(x, y, u8, u8, u8, u8, MODU8, len, offset, out);
        case MTYPE2(TYPE_U8, -TYPE_I32):
            return __BINOP_V_A(x, y, u8, i32, i32, i32, MODI32, len, offset, out);
        case MTYPE2(TYPE_U8, -TYPE_I64):
            return __BINOP_V_A(x, y, u8, i64, i64, i64, MODI64, len, offset, out);
        case MTYPE2(TYPE_U8, TYPE_U8):
            return __BINOP_V_V(x, y, u8, u8, u8, u8, MODU8, len, offset, out);
        case MTYPE2(TYPE_U8, TYPE_I32):
            return __BINOP_V_V(x, y, u8, i32, i32, i32, MODI32, len, offset, out);
        case MTYPE2(TYPE_U8, TYPE_I64):
            return __BINOP_V_V(x, y, u8, i64, i64, i64, MODI64, len, offset, out);

        case MTYPE2(-TYPE_I16, -TYPE_I16):
            return i16(MODI16(x->i16, y->i16));
        case MTYPE2(-TYPE_I16, -TYPE_I32):
            return i32(MODI32(i16_to_i32(x->i16), y->i32));
        case MTYPE2(-TYPE_I16, -TYPE_I64):
            return i64(MODI64(i16_to_i64(x->i16), y->i64));
        case MTYPE2(-TYPE_I16, TYPE_I16):
            return __BINOP_A_V(x, y, i16, i16, i16, i16, MODI16, len, offset, out);
        case MTYPE2(-TYPE_I16, TYPE_I32):
            return __BINOP_A_V(x, y, i16, i32, i32, i32, MODI32, len, offset, out);
        case MTYPE2(-TYPE_I16, TYPE_I64):
            return __BINOP_A_V(x, y, i16, i64, i64, i64, MODI64, len, offset, out);
        case MTYPE2(TYPE_I16, -TYPE_I16):
            return __BINOP_V_A(x, y, i16, i16, i16, i16, MODI16, len, offset, out);
        case MTYPE2(TYPE_I16, -TYPE_I32):
            return __BINOP_V_A(x, y, i16, i32, i32, i32, MODI32, len, offset, out);
        case MTYPE2(TYPE_I16, -TYPE_I64):
            return __BINOP_V_A(x, y, i16, i64, i64, i64, MODI64, len, offset, out);
        case MTYPE2(TYPE_I16, TYPE_I16):
            return __BINOP_V_V(x, y, i16, i16, i16, i16, MODI16, len, offset, out);
        case MTYPE2(TYPE_I16, TYPE_I32):
            return __BINOP_V_V(x, y, i16, i32, i32, i32, MODI32, len, offset, out);
        case MTYPE2(TYPE_I16, TYPE_I64):
            return __BINOP_V_V(x, y, i16, i64, i64, i64, MODI64, len, offset, out);

        case MTYPE2(-TYPE_I32, -TYPE_U8):
            return i32(MODI32(x->i32, u8_to_i32(y->u8)));
        case MTYPE2(-TYPE_I32, -TYPE_I16):
            return i32(MODI32(x->i32, i16_to_i32(y->i16)));
        case MTYPE2(-TYPE_I32, TYPE_U8):
            return __BINOP_A_V(x, y, i32, u8, i32, i32, MODI32, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_I16):
            return __BINOP_A_V(x, y, i32, i16, i32, i32, MODI32, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_U8):
            return __BINOP_V_A(x, y, i32, u8, i32, i32, MODI32, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_I16):
            return __BINOP_V_A(x, y, i32, i16, i32, i32, MODI32, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_U8):
            return __BINOP_V_V(x, y, i32, u8, i32, i32, MODI32, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_I16):
            return __BINOP_V_V(x, y, i32, i16, i32, i32, MODI32, len, offset, out);

        case MTYPE2(-TYPE_I64, -TYPE_U8):
            return i64(MODI64(x->i64, u8_to_i64(y->u8)));
        case MTYPE2(-TYPE_I64, -TYPE_I16):
            return i64(MODI64(x->i64, i16_to_i64(y->i16)));
        case MTYPE2(-TYPE_I64, TYPE_U8):
            return __BINOP_A_V(x, y, i64, u8, i64, i64, MODI64, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_I16):
            return __BINOP_A_V(x, y, i64, i16, i64, i64, MODI64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_U8):
            return __BINOP_V_A(x, y, i64, u8, i64, i64, MODI64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_I16):
            return __BINOP_V_A(x, y, i64, i16, i64, i64, MODI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_U8):
            return __BINOP_V_V(x, y, i64, u8, i64, i64, MODI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_I16):
            return __BINOP_V_V(x, y, i64, i16, i64, i64, MODI64, len, offset, out);

        default:
            return err_new(EC_TYPE);
    }
}

obj_p ray_xbar_partial(obj_p x, obj_p y, i64_t len, i64_t offset, obj_p out) {
    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(-TYPE_I32, -TYPE_I32):
            return i32(XBARI32(x->i32, y->i32));
        case MTYPE2(-TYPE_I32, -TYPE_I64):
            return i64(XBARI64(i32_to_i64(x->i32), y->i64));
        case MTYPE2(-TYPE_I32, -TYPE_F64):
            return f64(XBARF64(i32_to_f64(x->i32), y->f64));
        case MTYPE2(-TYPE_I32, TYPE_I32):
            return __BINOP_A_V(x, y, i32, i32, i32, i32, XBARI32, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_I64):
            return __BINOP_A_V(x, y, i32, i64, i64, i64, XBARI64, len, offset, out);
        case MTYPE2(-TYPE_I32, TYPE_F64):
            return __BINOP_A_V(x, y, i32, f64, f64, f64, XBARF64, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_I32):
            return __BINOP_V_A(x, y, i32, i32, i32, i32, XBARI32, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_I64):
            return __BINOP_V_A(x, y, i32, i64, i64, i64, XBARI64, len, offset, out);
        case MTYPE2(TYPE_I32, -TYPE_F64):
            return __BINOP_V_A(x, y, i32, f64, f64, f64, XBARF64, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_I32):
            return __BINOP_V_V(x, y, i32, i32, i32, i32, XBARI32, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_I64):
            return __BINOP_V_V(x, y, i32, i64, i64, i64, XBARI64, len, offset, out);
        case MTYPE2(TYPE_I32, TYPE_F64):
            return __BINOP_V_V(x, y, i32, f64, f64, f64, XBARF64, len, offset, out);

        case MTYPE2(-TYPE_I64, -TYPE_I32):
            return i64(XBARI64(x->i64, i32_to_i64(y->i32)));
        case MTYPE2(-TYPE_I64, -TYPE_I64):
            return i64(XBARI64(x->i64, y->i64));
        case MTYPE2(-TYPE_I64, -TYPE_F64):
            return f64(XBARF64(i64_to_f64(x->i64), y->f64));
        case MTYPE2(-TYPE_I64, TYPE_I32):
            return __BINOP_A_V(x, y, i64, i32, i64, i64, XBARI64, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_I64):
            return __BINOP_A_V(x, y, i64, i64, i64, i64, XBARI64, len, offset, out);
        case MTYPE2(-TYPE_I64, TYPE_F64):
            return __BINOP_A_V(x, y, i64, f64, f64, f64, XBARF64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_I32):
            return __BINOP_V_A(x, y, i64, i32, i64, i64, XBARI64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_I64):
            return __BINOP_V_A(x, y, i64, i64, i64, i64, XBARI64, len, offset, out);
        case MTYPE2(TYPE_I64, -TYPE_F64):
            return __BINOP_V_A(x, y, i64, f64, f64, f64, XBARF64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_I32):
            return __BINOP_V_V(x, y, i64, i32, i64, i64, XBARI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_I64):
            return __BINOP_V_V(x, y, i64, i64, i64, i64, XBARI64, len, offset, out);
        case MTYPE2(TYPE_I64, TYPE_F64):
            return __BINOP_V_V(x, y, i64, f64, f64, f64, XBARF64, len, offset, out);

        case MTYPE2(-TYPE_F64, -TYPE_I32):
            return f64(XBARF64(x->f64, i32_to_f64(y->i32)));
        case MTYPE2(-TYPE_F64, -TYPE_I64):
            return f64(XBARF64(x->f64, i64_to_f64(y->i64)));
        case MTYPE2(-TYPE_F64, -TYPE_F64):
            return f64(XBARF64(x->f64, y->f64));
        case MTYPE2(-TYPE_F64, TYPE_I32):
            return __BINOP_A_V(x, y, f64, i32, f64, f64, XBARF64, len, offset, out);
        case MTYPE2(-TYPE_F64, TYPE_I64):
            return __BINOP_A_V(x, y, f64, i64, f64, f64, XBARF64, len, offset, out);
        case MTYPE2(-TYPE_F64, TYPE_F64):
            return __BINOP_A_V(x, y, f64, f64, f64, f64, XBARF64, len, offset, out);
        case MTYPE2(TYPE_F64, -TYPE_I32):
            return __BINOP_V_A(x, y, f64, i32, f64, f64, XBARF64, len, offset, out);
        case MTYPE2(TYPE_F64, -TYPE_I64):
            return __BINOP_V_A(x, y, f64, i64, f64, f64, XBARF64, len, offset, out);
        case MTYPE2(TYPE_F64, -TYPE_F64):
            return __BINOP_V_A(x, y, f64, f64, f64, f64, XBARF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_I32):
            return __BINOP_V_V(x, y, f64, i32, f64, f64, XBARF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_I64):
            return __BINOP_V_V(x, y, f64, i64, f64, f64, XBARF64, len, offset, out);
        case MTYPE2(TYPE_F64, TYPE_F64):
            return __BINOP_V_V(x, y, f64, f64, f64, f64, XBARF64, len, offset, out);

        case MTYPE2(-TYPE_DATE, -TYPE_I32):
            return adate(XBARI32(x->i32, y->i32));
        case MTYPE2(-TYPE_DATE, -TYPE_I64):
            return adate(i64_to_i32(XBARI64(i32_to_i64(x->i32), y->i64)));
        case MTYPE2(-TYPE_DATE, TYPE_I32):
            return __BINOP_A_V(x, y, i32, i32, date, i32, XBARI32, len, offset, out);
        case MTYPE2(-TYPE_DATE, TYPE_I64):
            return __BINOP_A_V(x, y, i32, i64, date, i64, XBARI64, len, offset, out);
        case MTYPE2(TYPE_DATE, -TYPE_I32):
            return __BINOP_V_A(x, y, i32, i32, date, i32, XBARI32, len, offset, out);
        case MTYPE2(TYPE_DATE, -TYPE_I64):
            return __BINOP_V_A(x, y, i32, i64, date, i64, XBARI64, len, offset, out);
        case MTYPE2(TYPE_DATE, TYPE_I32):
            return __BINOP_V_V(x, y, i32, i32, date, i32, XBARI32, len, offset, out);
        case MTYPE2(TYPE_DATE, TYPE_I64):
            return __BINOP_V_V(x, y, i32, i64, date, i64, XBARI64, len, offset, out);

        case MTYPE2(-TYPE_TIME, -TYPE_I32):
            return atime(XBARI32(x->i32, y->i32));
        case MTYPE2(-TYPE_TIME, -TYPE_I64):
            return atime(i64_to_i32(XBARI64(i32_to_i64(x->i32), y->i64)));
        case MTYPE2(-TYPE_TIME, -TYPE_TIME):
            return atime(XBARI32(x->i32, y->i32));
        case MTYPE2(-TYPE_TIME, TYPE_I32):
            return __BINOP_A_V(x, y, i32, i32, time, i32, XBARI32, len, offset, out);
        case MTYPE2(-TYPE_TIME, TYPE_I64):
            return __BINOP_A_V(x, y, i32, i64, time, i64, XBARI64, len, offset, out);
        case MTYPE2(-TYPE_TIME, TYPE_TIME):
            return __BINOP_A_V(x, y, i32, i32, time, i32, XBARI32, len, offset, out);
        case MTYPE2(TYPE_TIME, -TYPE_I32):
            return __BINOP_V_A(x, y, i32, i32, time, i32, XBARI32, len, offset, out);
        case MTYPE2(TYPE_TIME, -TYPE_I64):
            return __BINOP_V_A(x, y, i32, i64, time, i64, XBARI64, len, offset, out);
        case MTYPE2(TYPE_TIME, -TYPE_TIME):
            return __BINOP_V_A(x, y, i32, i32, time, i32, XBARI32, len, offset, out);
        case MTYPE2(TYPE_TIME, TYPE_I32):
            return __BINOP_V_V(x, y, i32, i32, time, i32, XBARI32, len, offset, out);
        case MTYPE2(TYPE_TIME, TYPE_I64):
            return __BINOP_V_V(x, y, i32, i64, time, i64, XBARI64, len, offset, out);
        case MTYPE2(TYPE_TIME, TYPE_TIME):
            return __BINOP_V_V(x, y, i32, i32, time, i32, XBARI32, len, offset, out);

        case MTYPE2(-TYPE_TIMESTAMP, -TYPE_I32):
            return timestamp(XBARI64(x->i64, i32_to_i64(y->i32)));
        case MTYPE2(-TYPE_TIMESTAMP, -TYPE_I64):
            return timestamp(XBARI64(x->i64, y->i64));
        case MTYPE2(-TYPE_TIMESTAMP, -TYPE_TIME):
            return timestamp(XBARI32(x->i64, timestamp_to_i64(time_to_timestamp(y->i32))));
        case MTYPE2(-TYPE_TIMESTAMP, TYPE_I32):
            return __BINOP_A_V(x, y, i64, i32, timestamp, timestamp, XBARI64, len, offset, out);
        case MTYPE2(-TYPE_TIMESTAMP, TYPE_I64):
            return __BINOP_A_V(x, y, i64, i64, timestamp, timestamp, XBARI64, len, offset, out);
        case MTYPE2(-TYPE_TIMESTAMP, TYPE_TIME):
            return __BINOP_A_V(x, y, i64, time, timestamp, timestamp, XBARI64, len, offset, out);
        case MTYPE2(TYPE_TIMESTAMP, -TYPE_I32):
            return __BINOP_V_A(x, y, i64, i32, timestamp, timestamp, XBARI64, len, offset, out);
        case MTYPE2(TYPE_TIMESTAMP, -TYPE_I64):
            return __BINOP_V_A(x, y, i64, i64, timestamp, timestamp, XBARI64, len, offset, out);
        case MTYPE2(TYPE_TIMESTAMP, -TYPE_TIME):
            return __BINOP_V_A(x, y, i64, time, timestamp, timestamp, XBARI64, len, offset, out);
        case MTYPE2(TYPE_TIMESTAMP, TYPE_I32):
            return __BINOP_V_V(x, y, i64, i32, timestamp, timestamp, XBARI64, len, offset, out);
        case MTYPE2(TYPE_TIMESTAMP, TYPE_I64):
            return __BINOP_V_V(x, y, i64, i64, timestamp, timestamp, XBARI64, len, offset, out);
        case MTYPE2(TYPE_TIMESTAMP, TYPE_TIME):
            return __BINOP_V_V(x, y, i64, time, timestamp, timestamp, XBARI64, len, offset, out);

        default:
            return err_new(EC_TYPE);
    }
}

// count non-null values
obj_p ray_cnt_partial(obj_p x, i64_t len, i64_t offset) {
    switch (x->type) {
        case -TYPE_U8:
            return i64(1);  // u8 has no NULL
        case -TYPE_I16:
            return i64(CNTI16(0, x->i16));
        case -TYPE_I32:
            return i64(CNTI32(0, x->i32));
        case -TYPE_I64:
            return i64(CNTI64(0, x->i64));
        case -TYPE_F64:
            return i64(CNTF64(0, x->f64));
        case -TYPE_DATE:
            return i64(CNTI32(0, x->i32));
        case -TYPE_TIME:
            return i64(CNTI32(0, x->i32));
        case -TYPE_TIMESTAMP:
            return i64(CNTI64(0, x->i64));
        case TYPE_U8:
            return i64(len);  // u8 has no NULL, all elements count
        case TYPE_I16:
            return __UNOP_FOLD(x, i16, i64, CNTI16, len, offset, 0);
        case TYPE_I32:
            return __UNOP_FOLD(x, i32, i64, CNTI32, len, offset, 0);
        case TYPE_I64:
            return __UNOP_FOLD(x, i64, i64, CNTI64, len, offset, 0);
        case TYPE_F64:
            return __UNOP_FOLD(x, f64, i64, CNTF64, len, offset, 0);
        case TYPE_DATE:
            return __UNOP_FOLD(x, i32, i64, CNTI32, len, offset, 0);
        case TYPE_TIME:
            return __UNOP_FOLD(x, i32, i64, CNTI32, len, offset, 0);
        case TYPE_TIMESTAMP:
            return __UNOP_FOLD(x, i64, i64, CNTI64, len, offset, 0);
        case TYPE_PARTEDI16:
        case TYPE_PARTEDI32:
        case TYPE_PARTEDI64:
        case TYPE_PARTEDF64:
        case TYPE_PARTEDDATE:
        case TYPE_PARTEDTIME:
        case TYPE_PARTEDTIMESTAMP: {
            obj_p index =
                vn_list(7, i64(INDEX_TYPE_PARTEDCOMMON), i64(1), NULL_OBJ, i64(NULL_I64), NULL_OBJ, NULL_OBJ, NULL_OBJ);
            obj_p res = aggr_count(x, index);
            drop_obj(index);
            return res;
        }
        default:
            return err_new(EC_TYPE);
    }
}

obj_p ray_sum_partial(obj_p x, i64_t len, i64_t offset) {
    switch (x->type) {
        case -TYPE_U8:
            return i64((i64_t)x->u8);
        case -TYPE_I16:
            return (x->i16 == NULL_I16) ? i64(NULL_I64) : i64((i64_t)x->i16);
        case -TYPE_I32:
        case -TYPE_I64:
        case -TYPE_F64:
        case -TYPE_DATE:
        case -TYPE_TIME:
        case -TYPE_TIMESTAMP:
            return clone_obj(x);
        case TYPE_U8: {
            u8_t *lhs = AS_U8(x) + offset;
            i64_t out = 0;
            for (i64_t i = 0; i < len; i++)
                out += lhs[i];
            return i64(out);
        }
        case TYPE_I16: {
            i16_t *lhs = AS_I16(x) + offset;
            i64_t out = 0;
            for (i64_t i = 0; i < len; i++)
                out = FOLD_ADDI64(out, i16_to_i64(lhs[i]));
            return i64(out);
        }
        case TYPE_I32:
            return __UNOP_FOLD(x, i32, i32, FOLD_ADDI32, len, offset, 0);
        case TYPE_I64:
            return __UNOP_FOLD(x, i64, i64, FOLD_ADDI64, len, offset, 0);
        case TYPE_F64:
            return __UNOP_FOLD(x, f64, f64, FOLD_ADDF64, len, offset, 0);
        case TYPE_TIME:
            return __UNOP_FOLD(x, i32, atime, FOLD_ADDI32, len, offset, 0);
        case TYPE_MAPGROUP:
            return aggr_sum(AS_LIST(x)[0], AS_LIST(x)[1]);
        case TYPE_MAPFILTER: {
            obj_p val = AS_LIST(x)[0];
            obj_p filter = AS_LIST(x)[1];
            // Smart aggregation for parted data with parted filter
            if (val->type >= TYPE_PARTEDLIST && val->type <= TYPE_PARTEDGUID && filter->type == TYPE_PARTEDI64) {
                obj_p index = vn_list(7, i64(INDEX_TYPE_PARTEDCOMMON), i64(1), NULL_OBJ, i64(NULL_I64), NULL_OBJ,
                                      clone_obj(filter), NULL_OBJ);
                obj_p res = aggr_sum(val, index);
                drop_obj(index);
                return res;
            }
            // Fallback: materialize and aggregate
            obj_p collected = filter_collect(val, filter);
            obj_p res = ray_sum(collected);
            drop_obj(collected);
            return res;
        }
        case TYPE_PARTEDI16:
        case TYPE_PARTEDI32:
        case TYPE_PARTEDI64:
        case TYPE_PARTEDF64:
        case TYPE_PARTEDTIMESTAMP:
        case TYPE_PARTEDDATE:
        case TYPE_PARTEDTIME: {
            obj_p index =
                vn_list(7, i64(INDEX_TYPE_PARTEDCOMMON), i64(1), NULL_OBJ, i64(NULL_I64), NULL_OBJ, NULL_OBJ, NULL_OBJ);
            obj_p res = aggr_sum(x, index);
            drop_obj(index);
            return res;
        }
        default:
            return err_new(EC_TYPE);
    }
}

obj_p ray_min_partial(obj_p x, i64_t len, i64_t offset) {
    switch (x->type) {
        case -TYPE_U8:
            return u8(x->u8);
        case -TYPE_I16:
            return i16(x->i16);
        case -TYPE_I32:
        case -TYPE_I64:
        case -TYPE_F64:
        case -TYPE_DATE:
        case -TYPE_TIME:
        case -TYPE_TIMESTAMP:
            return clone_obj(x);
        case TYPE_U8: {
            u8_t *lhs = AS_U8(x) + offset;
            u8_t out = (len > 0) ? lhs[0] : 0;
            for (i64_t i = 1; i < len; i++)
                out = MINU8(out, lhs[i]);
            return u8(out);
        }
        case TYPE_I16:
            return __UNOP_FOLD(x, i16, i16, MINI16, len, offset, NULL_I16);
        case TYPE_I32:
            return __UNOP_FOLD(x, i32, i32, MINI32, len, offset, NULL_I32);
        case TYPE_I64:
            return __UNOP_FOLD(x, i64, i64, MINI64, len, offset, NULL_I64);
        case TYPE_F64:
            return __UNOP_FOLD(x, f64, f64, MINF64, len, offset, NULL_F64);
        case TYPE_DATE:
            return __UNOP_FOLD(x, i32, adate, MINI32, len, offset, NULL_I32);
        case TYPE_TIME:
            return __UNOP_FOLD(x, i32, atime, MINI32, len, offset, NULL_I32);
        case TYPE_TIMESTAMP:
            return __UNOP_FOLD(x, i64, timestamp, MINI64, len, offset, NULL_I64);
        case TYPE_MAPGROUP:
            return aggr_min(AS_LIST(x)[0], AS_LIST(x)[1]);
        case TYPE_MAPFILTER: {
            obj_p val = AS_LIST(x)[0];
            obj_p filter = AS_LIST(x)[1];
            if (val->type >= TYPE_PARTEDLIST && val->type <= TYPE_PARTEDGUID && filter->type == TYPE_PARTEDI64) {
                obj_p index = vn_list(7, i64(INDEX_TYPE_PARTEDCOMMON), i64(1), NULL_OBJ, i64(NULL_I64), NULL_OBJ,
                                      clone_obj(filter), NULL_OBJ);
                obj_p res = aggr_min(val, index);
                drop_obj(index);
                return res;
            }
            obj_p collected = filter_collect(val, filter);
            obj_p res = ray_min(collected);
            drop_obj(collected);
            return res;
        }
        case TYPE_PARTEDI16:
        case TYPE_PARTEDI32:
        case TYPE_PARTEDI64:
        case TYPE_PARTEDF64:
        case TYPE_PARTEDTIMESTAMP:
        case TYPE_PARTEDDATE:
        case TYPE_PARTEDTIME: {
            obj_p index =
                vn_list(7, i64(INDEX_TYPE_PARTEDCOMMON), i64(1), NULL_OBJ, i64(NULL_I64), NULL_OBJ, NULL_OBJ, NULL_OBJ);
            obj_p res = aggr_min(x, index);
            drop_obj(index);
            return res;
        }
        default:
            return err_new(EC_TYPE);
    }
}

obj_p ray_max_partial(obj_p x, i64_t len, i64_t offset) {
    switch (x->type) {
        case -TYPE_U8:
            return u8(x->u8);
        case -TYPE_I16:
            return i16(x->i16);
        case -TYPE_I32:
        case -TYPE_I64:
        case -TYPE_F64:
        case -TYPE_DATE:
        case -TYPE_TIME:
        case -TYPE_TIMESTAMP:
            return clone_obj(x);
        case TYPE_U8: {
            u8_t *lhs = AS_U8(x) + offset;
            u8_t out = (len > 0) ? lhs[0] : 0;
            for (i64_t i = 1; i < len; i++)
                out = MAXU8(out, lhs[i]);
            return u8(out);
        }
        case TYPE_I16:
            return __UNOP_FOLD(x, i16, i16, MAXI16, len, offset, NULL_I16);
        case TYPE_I32:
            return __UNOP_FOLD(x, i32, i32, MAXI32, len, offset, NULL_I32);
        case TYPE_I64:
            return __UNOP_FOLD(x, i64, i64, MAXI64, len, offset, NULL_I64);
        case TYPE_F64:
            return __UNOP_FOLD(x, f64, f64, MAXF64, len, offset, NULL_F64);
        case TYPE_DATE:
            return __UNOP_FOLD(x, i32, adate, MAXI32, len, offset, NULL_I32);
        case TYPE_TIME:
            return __UNOP_FOLD(x, i32, atime, MAXI32, len, offset, NULL_I32);
        case TYPE_TIMESTAMP:
            return __UNOP_FOLD(x, i64, timestamp, MAXI64, len, offset, NULL_I64);
        case TYPE_MAPGROUP:
            return aggr_max(AS_LIST(x)[0], AS_LIST(x)[1]);
        case TYPE_MAPFILTER: {
            obj_p val = AS_LIST(x)[0];
            obj_p filter = AS_LIST(x)[1];
            if (val->type >= TYPE_PARTEDLIST && val->type <= TYPE_PARTEDGUID && filter->type == TYPE_PARTEDI64) {
                obj_p index = vn_list(7, i64(INDEX_TYPE_PARTEDCOMMON), i64(1), NULL_OBJ, i64(NULL_I64), NULL_OBJ,
                                      clone_obj(filter), NULL_OBJ);
                obj_p res = aggr_max(val, index);
                drop_obj(index);
                return res;
            }
            obj_p collected = filter_collect(val, filter);
            obj_p res = ray_max(collected);
            drop_obj(collected);
            return res;
        }
        case TYPE_PARTEDI16:
        case TYPE_PARTEDI32:
        case TYPE_PARTEDI64:
        case TYPE_PARTEDF64:
        case TYPE_PARTEDTIMESTAMP:
        case TYPE_PARTEDDATE:
        case TYPE_PARTEDTIME: {
            obj_p index =
                vn_list(7, i64(INDEX_TYPE_PARTEDCOMMON), i64(1), NULL_OBJ, i64(NULL_I64), NULL_OBJ, NULL_OBJ, NULL_OBJ);
            obj_p res = aggr_max(x, index);
            drop_obj(index);
            return res;
        }
        default:
            return err_new(EC_TYPE);
    }
}

obj_p ray_round_partial(obj_p x, i64_t len, i64_t offset, obj_p out) {
    switch (x->type) {
        case -TYPE_I32:
        case -TYPE_I64:
        case -TYPE_DATE:
        case -TYPE_TIME:
        case -TYPE_TIMESTAMP:
        case TYPE_I32:
        case TYPE_I64:
        case TYPE_DATE:
        case TYPE_TIME:
        case TYPE_TIMESTAMP:
            return clone_obj(x);
        case -TYPE_F64:
            return f64(ROUNDF64(x->f64));
        case TYPE_F64:
            return __UNOP_MAP(x, f64, f64, ROUNDF64, len, offset, out);
        // case TYPE_MAPGROUP:
        //     return aggr_round(AS_LIST(x)[0], AS_LIST(x)[1]);
        default:
            return err_new(EC_TYPE);
    }
}

obj_p ray_floor_partial(obj_p x, i64_t len, i64_t offset, obj_p out) {
    switch (x->type) {
        case -TYPE_I32:
        case -TYPE_I64:
        case -TYPE_DATE:
        case -TYPE_TIME:
        case -TYPE_TIMESTAMP:
        case TYPE_I32:
        case TYPE_I64:
        case TYPE_DATE:
        case TYPE_TIME:
        case TYPE_TIMESTAMP:
            return clone_obj(x);
        case -TYPE_F64:
            return f64(FLOORF64(x->f64));
        case TYPE_F64:
            return __UNOP_MAP(x, f64, f64, FLOORF64, len, offset, out);
        // case TYPE_MAPGROUP:
        //     return aggr_floor(AS_LIST(x)[0], AS_LIST(x)[1]);
        default:
            return err_new(EC_TYPE);
    }
}

obj_p ray_ceil_partial(obj_p x, i64_t len, i64_t offset, obj_p out) {
    switch (x->type) {
        case -TYPE_I32:
        case -TYPE_I64:
        case -TYPE_DATE:
        case -TYPE_TIME:
        case -TYPE_TIMESTAMP:
        case TYPE_I32:
        case TYPE_I64:
        case TYPE_DATE:
        case TYPE_TIME:
        case TYPE_TIMESTAMP:
            return clone_obj(x);
        case -TYPE_F64:
            return f64(CEILF64(x->f64));
        case TYPE_F64:
            return __UNOP_MAP(x, f64, f64, CEILF64, len, offset, out);
        // case TYPE_MAPGROUP:
        //     return aggr_ceil(AS_LIST(x)[0], AS_LIST(x)[1]);
        default:
            return err_new(EC_TYPE);
    }
}
// TODO: DRY
obj_p ray_sq_sub_partial(obj_p x, obj_p y, i64_t len, i64_t offset) {
    switch (x->type) {
        case TYPE_U8: {
            u8_t *lhs = __AS_u8(x) + offset;
            f64_t out = 0.0;
            for (i64_t i = 0; i < len; i++) {
                f64_t t = ((f64_t)lhs[i]) - y->f64;
                out += t * t;
            }
            return f64(out);
        }
        case TYPE_I16: {
            i16_t *lhs = __AS_i16(x) + offset;
            f64_t out = 0.0;
            for (i64_t i = 0; i < len; i++)
                if (lhs[i] != NULL_I16) {
                    f64_t t = ((f64_t)lhs[i]) - y->f64;
                    out += t * t;
                }
            return f64(out);
        }
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME: {
            i32_t *lhs = __AS_i32(x) + offset;
            f64_t out = 0.0;
            for (i64_t i = 0; i < len; i++)
                if (lhs[i] != NULL_I32) {
                    f64_t t = ((f64_t)lhs[i]) - y->f64;
                    out += t * t;
                }
            return f64(out);
        }
        case TYPE_I64:
        case TYPE_TIMESTAMP: {
            i64_t *lhs = __AS_i64(x) + offset;
            f64_t out = 0.0;
            for (i64_t i = 0; i < len; i++)
                if (lhs[i] != NULL_I64) {
                    f64_t t = ((f64_t)lhs[i]) - y->f64;
                    out += t * t;
                }
            return f64(out);
        }
        default: {
            f64_t *lhs = __AS_f64(x) + offset;
            f64_t out = 0.0;
            for (i64_t i = 0; i < len; i++)
                if (!ISNANF64(lhs[i])) {
                    f64_t t = ((f64_t)lhs[i]) - y->f64;
                    out += t * t;
                }
            return f64(out);
        }
    }
}

obj_p unop_fold(raw_p op, obj_p x) {
    pool_p pool;
    i64_t i, l, n;
    obj_p v, res;
    obj_p (*unop)(obj_p);
    raw_p argv[3];

    if (IS_ATOM(x)) {
        unop = (obj_p(*)(obj_p))op;
        return unop(x);
    }

    // Handle parted types directly - they have special handling in partial functions
    if (x->type >= TYPE_PARTEDLIST && x->type <= TYPE_PARTEDGUID) {
        return ((obj_p(*)(obj_p, i64_t, i64_t))op)(x, x->len, 0);
    }

    // Handle MAPFILTER and MAPGROUP directly - they have special handling in partial functions
    if (x->type == TYPE_MAPFILTER || x->type == TYPE_MAPGROUP) {
        return ((obj_p(*)(obj_p, i64_t, i64_t))op)(x, x->len, 0);
    }

    l = ops_count(x);

    pool = runtime_get()->pool;
    n = pool_split_by(pool, l, 0);

    if (n == 1)
        return ((obj_p(*)(obj_p, i64_t, i64_t))op)(x, l, 0);

    i64_t chunk = pool_chunk_aligned(l, n, size_of_type(x->type));

    pool_prepare(pool);
    i64_t offset = 0;
    for (i = 0; i < n - 1 && offset < l; i++) {
        i64_t this_chunk = (offset + chunk <= l) ? chunk : (l - offset);
        pool_add_task(pool, op, 3, x, this_chunk, offset);
        offset += chunk;
    }
    if (offset < l)
        pool_add_task(pool, op, 3, x, l - offset, offset);

    v = pool_run(pool);
    if (IS_ERR(v))
        return v;

    // Fold the results
    v = unify_list(&v);
    argv[0] = (raw_p)v;
    argv[1] = (raw_p)v->len;
    argv[2] = (raw_p)0;
    res = pool_call_task_fn(op, 3, argv);
    drop_obj(v);

    return res;
}

obj_p unop_map(raw_p op, obj_p x) {
    pool_p pool;
    i64_t i, l, n, chunk;
    obj_p v, out;
    obj_p (*unop)(obj_p);

    if (IS_ATOM(x)) {
        unop = (obj_p(*)(obj_p))op;
        return unop(x);
    }

    l = ops_count(x);

    pool = runtime_get()->pool;
    n = pool_split_by(pool, l, 0);
    out = (rc_obj(x) == 1) ? clone_obj(x) : vector(x->type, l);

    if (n == 1) {
        v = ((obj_p(*)(obj_p, i64_t, i64_t, obj_p))op)(x, l, 0, out);
        if (IS_ERR(v)) {
            drop_obj(out);
            return v;
        }
        return out;
    }

    chunk = pool_chunk_aligned(l, n, size_of_type(x->type));

    pool_prepare(pool);
    i64_t offset = 0;
    for (i = 0; i < n - 1 && offset < l; i++) {
        i64_t this_chunk = (offset + chunk <= l) ? chunk : (l - offset);
        pool_add_task(pool, op, 4, x, this_chunk, offset, out);
        offset += chunk;
    }
    if (offset < l)
        pool_add_task(pool, op, 4, x, l - offset, offset, out);

    v = pool_run(pool);
    if (IS_ERR(v))
        return v;

    drop_obj(v);

    return out;
}

obj_p binop_map(raw_p op, obj_p x, obj_p y) {
    pool_p pool;
    i64_t i, l, n;
    obj_p v, out;
    i8_t t;

    if (IS_VECTOR(x) && IS_VECTOR(y)) {
        if (x->len != y->len)
            return err_new(EC_LENGTH);

        l = x->len;
    } else if (IS_VECTOR(x))
        l = x->len;
    else if (IS_VECTOR(y))
        l = y->len;
    else {
        // Both x and y are scalars - partial function handles this directly
        return ((obj_p(*)(obj_p, obj_p, i64_t, i64_t, obj_p))op)(x, y, 0, 0, NULL);
    }

    t = (op == ray_fdiv_partial)                            ? TYPE_F64
        : (op == ray_div_partial)                           ? infer_div_type(x, y)
        : (op == ray_xbar_partial)                          ? infer_xbar_type(x, y)
        : (op == ray_mod_partial || op == ray_xbar_partial) ? infer_mod_type(x, y)
                                                            : infer_math_type(x, y);

    pool = runtime_get()->pool;
    n = pool_split_by(pool, l, 0);
    // Only reuse input buffer if its type matches the output type
    // to avoid type mismatch (e.g., writing f64 into i64 buffer)
    out = (rc_obj(x) == 1 && IS_VECTOR(x) && x->type == t)   ? clone_obj(x)
          : (rc_obj(y) == 1 && IS_VECTOR(y) && y->type == t) ? clone_obj(y)
                                                             : vector(t, l);

    if (n == 1) {
        v = ((obj_p(*)(obj_p, obj_p, i64_t, i64_t, obj_p))op)(x, y, l, 0, out);
        if (IS_ERR(v)) {
            drop_obj(out);
            return v;
        }
        return out;
    }

    i64_t chunk = pool_chunk_aligned(l, n, size_of_type(t));

    pool_prepare(pool);
    i64_t offset = 0;
    for (i = 0; i < n - 1 && offset < l; i++) {
        i64_t this_chunk = (offset + chunk <= l) ? chunk : (l - offset);
        pool_add_task(pool, op, 5, x, y, this_chunk, offset, out);
        offset += chunk;
    }
    if (offset < l)
        pool_add_task(pool, op, 5, x, y, l - offset, offset, out);

    v = pool_run(pool);
    if (IS_ERR(v)) {
        out->len = 0;
        drop_obj(out);
        return v;
    }

    drop_obj(v);

    return out;
}

obj_p binop_fold(raw_p op, obj_p x, obj_p y) {
    pool_p pool;
    i64_t i, n, l;
    obj_p v, res;
    raw_p argv[4];

    l = x->len;
    pool = runtime_get()->pool;
    n = pool_split_by(pool, l, 0);

    if (n == 1)
        return ((obj_p(*)(obj_p, obj_p, i64_t, i64_t))op)(x, y, l, 0);

    i64_t chunk = pool_chunk_aligned(l, n, size_of_type(x->type));

    pool_prepare(pool);
    i64_t offset = 0;
    for (i = 0; i < n - 1 && offset < l; i++) {
        i64_t this_chunk = (offset + chunk <= l) ? chunk : (l - offset);
        pool_add_task(pool, op, 4, x, y, this_chunk, offset);
        offset += chunk;
    }
    if (offset < l)
        pool_add_task(pool, op, 4, x, y, l - offset, offset);

    v = pool_run(pool);
    if (IS_ERR(v))
        return v;

    // Fold the results
    v = unify_list(&v);
    argv[0] = (raw_p)v;
    argv[1] = (raw_p)v->len;
    argv[2] = (raw_p)0;
    res = pool_call_task_fn(ray_sum_partial, 3, argv);
    drop_obj(v);

    return res;
}

// Unaries
obj_p ray_sum(obj_p x) { return unop_fold(ray_sum_partial, x); }
obj_p ray_cnt(obj_p x) {
    pool_p pool;
    i64_t i, l, n;
    obj_p v, res;

    if (IS_ATOM(x)) {
        return ray_cnt_partial(x, 1, 0);
    }

    l = ops_count(x);
    pool = runtime_get()->pool;
    n = pool_split_by(pool, l, 0);

    if (n == 1)
        return ray_cnt_partial(x, l, 0);

    i64_t chunk = pool_chunk_aligned(l, n, size_of_type(x->type));

    pool_prepare(pool);
    i64_t offset = 0;
    for (i = 0; i < n - 1 && offset < l; i++) {
        i64_t this_chunk = (offset + chunk <= l) ? chunk : (l - offset);
        pool_add_task(pool, ray_cnt_partial, 3, x, this_chunk, offset);
        offset += chunk;
    }
    if (offset < l)
        pool_add_task(pool, ray_cnt_partial, 3, x, l - offset, offset);

    v = pool_run(pool);
    if (IS_ERR(v))
        return v;

    // Fold by SUMMING partial counts (not counting them)
    v = unify_list(&v);
    res = ray_sum(v);  // Sum the partial counts
    drop_obj(v);

    return res;
}
obj_p ray_min(obj_p x) { return unop_fold(ray_min_partial, x); }
obj_p ray_max(obj_p x) { return unop_fold(ray_max_partial, x); }
obj_p ray_round(obj_p x) { return unop_map(ray_round_partial, x); }
obj_p ray_floor(obj_p x) { return unop_map(ray_floor_partial, x); }
obj_p ray_ceil(obj_p x) { return unop_map(ray_ceil_partial, x); }

// Binaries
obj_p ray_sq_sub(obj_p x, obj_p y) { return binop_fold(ray_sq_sub_partial, x, y); }
obj_p ray_add(obj_p x, obj_p y) { return binop_map(ray_add_partial, x, y); }
obj_p ray_sub(obj_p x, obj_p y) { return binop_map(ray_sub_partial, x, y); }
obj_p ray_mul(obj_p x, obj_p y) { return binop_map(ray_mul_partial, x, y); }
obj_p ray_div(obj_p x, obj_p y) { return binop_map(ray_div_partial, x, y); }
obj_p ray_fdiv(obj_p x, obj_p y) { return binop_map(ray_fdiv_partial, x, y); }
obj_p ray_mod(obj_p x, obj_p y) { return binop_map(ray_mod_partial, x, y); }
obj_p ray_xbar(obj_p x, obj_p y) { return binop_map(ray_xbar_partial, x, y); }

// Functions which used functions with parallel execution
obj_p ray_avg(obj_p x) {
    obj_p l, r, res;
    switch (x->type) {
        case -TYPE_U8:
            return f64(u8_to_f64(x->u8));
        case -TYPE_I16:
            return f64(i16_to_f64(x->i16));
        case -TYPE_I32:
            return f64(i32_to_f64(x->i32));
        case -TYPE_I64:
            return f64(i64_to_f64(x->i64));
        case -TYPE_F64:
            return clone_obj(x);
        case TYPE_U8:
            // u8 has no NULL, so count = length
            l = ray_sum(x);
            res = f64(FDIVI64(l->i64, (i64_t)ops_count(x)));
            drop_obj(l);
            return res;
        case TYPE_I16:
            l = ray_sum(x);
            r = ray_cnt(x);
            res = f64(FDIVI64(l->i64, r->i64));
            drop_obj(l);
            drop_obj(r);
            return res;
        case TYPE_I32:
            l = ray_sum(x);
            r = ray_cnt(x);
            res = f64(FDIVI64(i32_to_i64(l->i32), r->i64));
            drop_obj(l);
            drop_obj(r);
            return res;
        case TYPE_I64:
            l = ray_sum(x);
            r = ray_cnt(x);
            res = f64(FDIVI64(l->i64, r->i64));
            drop_obj(l);
            drop_obj(r);
            return res;
        case TYPE_F64:
            l = ray_sum(x);
            r = ray_cnt(x);
            res = f64(FDIVF64(l->f64, i64_to_f64(r->i64)));
            drop_obj(l);
            drop_obj(r);
            return res;
        case TYPE_MAPGROUP:
            return aggr_avg(AS_LIST(x)[0], AS_LIST(x)[1]);
        case TYPE_MAPFILTER: {
            obj_p val = AS_LIST(x)[0];
            obj_p filter = AS_LIST(x)[1];
            if (val->type >= TYPE_PARTEDLIST && val->type <= TYPE_PARTEDGUID && filter->type == TYPE_PARTEDI64) {
                obj_p index = vn_list(7, i64(INDEX_TYPE_PARTEDCOMMON), i64(1), NULL_OBJ, i64(NULL_I64), NULL_OBJ,
                                      clone_obj(filter), NULL_OBJ);
                res = aggr_avg(val, index);
                drop_obj(index);
                return res;
            }
            obj_p collected = filter_collect(val, filter);
            res = ray_avg(collected);
            drop_obj(collected);
            return res;
        }
        case TYPE_PARTEDI16:
        case TYPE_PARTEDI32:
        case TYPE_PARTEDI64:
        case TYPE_PARTEDF64:
        case TYPE_PARTEDDATE:
        case TYPE_PARTEDTIME:
        case TYPE_PARTEDTIMESTAMP: {
            // Create minimal index for parted aggregation: group_count=1, filter=NULL
            obj_p index =
                vn_list(7, i64(INDEX_TYPE_PARTEDCOMMON), i64(1), NULL_OBJ, i64(NULL_I64), NULL_OBJ, NULL_OBJ, NULL_OBJ);
            res = aggr_avg(x, index);
            drop_obj(index);
            return res;
        }
        default:
            return err_new(EC_TYPE);
    }
}

// TODO: Refactoring with out sort and with parallel execution
obj_p ray_med(obj_p x) {
    i64_t l = ray_cnt(x)->i64;
    if (l == 0)
        return f64(NULL_F64);

    // i32_t *xi32sort;
    i64_t *xisort;
    u8_t *xu8sort;
    i16_t *xi16sort;
    // f64_t *xfsort, med;
    f64_t med;
    obj_p sort;

    switch (x->type) {
        case -TYPE_U8:
            return f64(u8_to_f64(x->u8));
        case -TYPE_I16:
            return f64(i16_to_f64(x->i16));
        case -TYPE_I32:
            return f64(i32_to_f64(x->i32));
        case -TYPE_I64:
            return f64(i64_to_f64(x->i64));
        case -TYPE_F64:
            return clone_obj(x);

        case TYPE_U8:
            sort = ray_asc(x);
            xu8sort = AS_U8(sort);
            med = (f64_t)((l % 2 == 0) ? (xu8sort[l / 2 - 1] + xu8sort[l / 2]) / 2.0 : xu8sort[l / 2]);
            drop_obj(sort);
            return f64(med);

        case TYPE_I16:
            sort = ray_asc(x);
            xi16sort = AS_I16(sort);
            med = (f64_t)((l % 2 == 0) ? (xi16sort[l / 2 - 1] + xi16sort[l / 2]) / 2.0 : xi16sort[l / 2]);
            drop_obj(sort);
            return f64(med);

            // TODO
            // case TYPE_I32:
            //     sort = ray_asc(x);
            //     xi32sort = AS_I32(sort);
            //     med = (f64_t)((l % 2 == 0) ? (xi32sort[l / 2 - 1] + xi32sort[l / 2]) / 2.0 : xi32sort[l / 2]);
            //     drop_obj(sort);

            //     return f64(med);

        case TYPE_I64:
            sort = ray_asc(x);
            xisort = AS_I64(sort);
            med = (f64_t)((l % 2 == 0) ? (xisort[l / 2 - 1] + xisort[l / 2]) / 2.0 : xisort[l / 2]);
            drop_obj(sort);

            return f64(med);
            // TODO
            // case TYPE_F64:
            //     sort = ray_asc(x);
            //     xfsort = AS_F64(sort);
            //     med = (l % 2 == 0) ? (xfsort[l / 2 - 1] + xfsort[l / 2]) / 2.0 : xfsort[l / 2];
            //     drop_obj(sort);

            //     return f64(med);

        case TYPE_MAPGROUP:
            return aggr_med(AS_LIST(x)[0], AS_LIST(x)[1]);
        case TYPE_MAPFILTER: {
            obj_p val = AS_LIST(x)[0];
            obj_p filter = AS_LIST(x)[1];
            if (val->type >= TYPE_PARTEDLIST && val->type <= TYPE_PARTEDGUID && filter->type == TYPE_PARTEDI64) {
                obj_p index = vn_list(7, i64(INDEX_TYPE_PARTEDCOMMON), i64(1), NULL_OBJ, i64(NULL_I64), NULL_OBJ,
                                      clone_obj(filter), NULL_OBJ);
                obj_p res = aggr_med(val, index);
                drop_obj(index);
                return res;
            }
            obj_p collected = filter_collect(val, filter);
            obj_p res = ray_med(collected);
            drop_obj(collected);
            return res;
        }
        case TYPE_PARTEDI16:
        case TYPE_PARTEDI32:
        case TYPE_PARTEDI64:
        case TYPE_PARTEDF64:
        case TYPE_PARTEDDATE:
        case TYPE_PARTEDTIME:
        case TYPE_PARTEDTIMESTAMP: {
            obj_p index =
                vn_list(7, i64(INDEX_TYPE_PARTEDCOMMON), i64(1), NULL_OBJ, i64(NULL_I64), NULL_OBJ, NULL_OBJ, NULL_OBJ);
            obj_p res = aggr_med(x, index);
            drop_obj(index);
            return res;
        }
        default:
            return err_new(EC_TYPE);
    }
}

obj_p ray_dev(obj_p x) {
    obj_p cnt_obj = ray_cnt(x);
    i64_t l = cnt_obj->i64;
    drop_obj(cnt_obj);

    if (l == 0)
        return f64(NULL_F64);
    else if (l == 1)
        return f64(0.0);
    f64_t favg = 0.0;
    obj_p sum_obj;

    switch (x->type) {
        case TYPE_U8:
        case TYPE_I16:
            sum_obj = ray_sum(x);
            favg = ((f64_t)sum_obj->i64) / (f64_t)l;
            drop_obj(sum_obj);
            break;
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            sum_obj = ray_sum(x);
            favg = ((f64_t)sum_obj->i32) / (f64_t)l;
            drop_obj(sum_obj);
            break;
        case TYPE_I64:
        case TYPE_TIMESTAMP:
            sum_obj = ray_sum(x);
            favg = ((f64_t)sum_obj->i64) / (f64_t)l;
            drop_obj(sum_obj);
            break;
        case TYPE_F64:
            sum_obj = ray_sum(x);
            favg = (sum_obj->f64) / (f64_t)l;
            drop_obj(sum_obj);
            break;
        case TYPE_MAPGROUP:
            return aggr_dev(AS_LIST(x)[0], AS_LIST(x)[1]);
        case TYPE_MAPFILTER: {
            obj_p val = AS_LIST(x)[0];
            obj_p filter = AS_LIST(x)[1];
            if (val->type >= TYPE_PARTEDLIST && val->type <= TYPE_PARTEDGUID && filter->type == TYPE_PARTEDI64) {
                obj_p index = vn_list(7, i64(INDEX_TYPE_PARTEDCOMMON), i64(1), NULL_OBJ, i64(NULL_I64), NULL_OBJ,
                                      clone_obj(filter), NULL_OBJ);
                obj_p res = aggr_dev(val, index);
                drop_obj(index);
                return res;
            }
            obj_p collected = filter_collect(val, filter);
            obj_p res = ray_dev(collected);
            drop_obj(collected);
            return res;
        }
        case TYPE_PARTEDI16:
        case TYPE_PARTEDI32:
        case TYPE_PARTEDI64:
        case TYPE_PARTEDF64:
        case TYPE_PARTEDDATE:
        case TYPE_PARTEDTIME:
        case TYPE_PARTEDTIMESTAMP: {
            obj_p index =
                vn_list(7, i64(INDEX_TYPE_PARTEDCOMMON), i64(1), NULL_OBJ, i64(NULL_I64), NULL_OBJ, NULL_OBJ, NULL_OBJ);
            obj_p res = aggr_dev(x, index);
            drop_obj(index);
            return res;
        }
        default:
            return err_new(EC_TYPE);
    }

    return f64(sqrt(ray_sq_sub(x, f64(favg))->f64 / (f64_t)l));
}
