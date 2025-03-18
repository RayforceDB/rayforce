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

#include "cmp.h"
#include "util.h"
#include "unary.h"
#include "heap.h"
#include "ops.h"
#include "error.h"
#include "items.h"
#include "runtime.h"
#include "pool.h"
#include "string.h"

typedef obj_p (*ray_cmp_f)(obj_p, obj_p, u64_t, u64_t, obj_p);

#define __CMP_A_V(x, y, lt, rt, mt, op, ln, of, ov)                              \
    ({                                                                           \
        __BASE_##rt##_t *$rhs;                                                   \
        b8_t *$out;                                                              \
        $rhs = __AS_##rt(y) + of;                                                \
        $out = AS_B8(ov) + of;                                                   \
        for (u64_t $i = 0; $i < ln; $i++)                                        \
            $out[$i] = op(lt##_to_##mt(x->__BASE_##lt), rt##_to_##mt($rhs[$i])); \
        NULL_OBJ;                                                                \
    })

#define __CMP_V_A(x, y, lt, rt, mt, op, ln, of, ov)                              \
    ({                                                                           \
        __BASE_##lt##_t *$lhs;                                                   \
        b8_t *$out;                                                              \
        $lhs = __AS_##lt(x) + of;                                                \
        $out = AS_B8(ov) + of;                                                   \
        for (u64_t $i = 0; $i < ln; $i++)                                        \
            $out[$i] = op(lt##_to_##mt($lhs[$i]), rt##_to_##mt(y->__BASE_##rt)); \
        NULL_OBJ;                                                                \
    })

#define __CMP_V_V(x, y, lt, rt, mt, op, ln, of, ov)                        \
    ({                                                                     \
        __BASE_##lt##_t *$lhs;                                             \
        __BASE_##rt##_t *$rhs;                                             \
        b8_t *$out;                                                        \
        $lhs = __AS_##lt(x) + of;                                          \
        $rhs = __AS_##rt(y) + of;                                          \
        $out = AS_B8(ov) + of;                                             \
        for (u64_t $i = 0; $i < ln; $i++)                                  \
            $out[$i] = op(lt##_to_##mt($lhs[$i]), rt##_to_##mt($rhs[$i])); \
        NULL_OBJ;                                                          \
    })

#define __DECLARE_CMP_FN(op)                                                                                \
    obj_p ray_##op##_partial(obj_p x, obj_p y, u64_t len, u64_t offset, obj_p res) {                        \
        u64_t i;                                                                                            \
        i64_t *xi, *ei, si;                                                                                 \
        b8_t *out;                                                                                          \
        obj_p k, sym, e;                                                                                    \
                                                                                                            \
        switch (MTYPE2(x->type, y->type)) {                                                                 \
            case MTYPE2(-TYPE_B8, -TYPE_B8):                                                                \
                return b8(op##I8(x->b8, y->b8));                                                            \
            case MTYPE2(-TYPE_U8, -TYPE_U8):                                                                \
                return b8(op##I8(x->u8, y->u8));                                                            \
            case MTYPE2(-TYPE_B8, -TYPE_U8):                                                                \
                return b8(op##I8(x->b8, y->u8));                                                            \
            case MTYPE2(-TYPE_U8, -TYPE_B8):                                                                \
                return b8(op##I8(x->u8, y->b8));                                                            \
            case MTYPE2(-TYPE_C8, -TYPE_C8):                                                                \
                return b8(op##C8(x->c8, y->c8));                                                            \
            case MTYPE2(-TYPE_C8, TYPE_C8):                                                                 \
                return b8(op##STR((lit_p)(&x->c8), 1, AS_C8(y), y->len));                                   \
            case MTYPE2(TYPE_C8, -TYPE_C8):                                                                 \
                return b8(op##STR(AS_C8(x), x->len, (lit_p)(&y->c8), 1));                                   \
            case MTYPE2(TYPE_C8, TYPE_C8):                                                                  \
                return b8(op##STR(AS_C8(x), x->len, AS_C8(y), y->len));                                     \
                                                                                                            \
            case MTYPE2(-TYPE_I16, -TYPE_I16):                                                              \
                return b8(op##I16(x->i16, y->i16));                                                         \
            case MTYPE2(-TYPE_I16, -TYPE_I32):                                                              \
                return b8(op##I32(i16_to_i32(x->i16), y->i32));                                             \
            case MTYPE2(-TYPE_I16, -TYPE_I64):                                                              \
                return b8(op##I64(i16_to_i64(x->i16), y->i64));                                             \
            case MTYPE2(-TYPE_I16, -TYPE_F64):                                                              \
                return b8(op##F64(i16_to_f64(x->i16), y->f64));                                             \
            case MTYPE2(-TYPE_I16, TYPE_I16):                                                               \
                return __CMP_A_V(x, y, i16, i16, i16, op##I16, len, offset, res);                           \
            case MTYPE2(-TYPE_I16, TYPE_I32):                                                               \
                return __CMP_A_V(x, y, i16, i32, i32, op##I32, len, offset, res);                           \
            case MTYPE2(-TYPE_I16, TYPE_I64):                                                               \
                return __CMP_A_V(x, y, i16, i64, i64, op##I64, len, offset, res);                           \
            case MTYPE2(-TYPE_I16, TYPE_F64):                                                               \
                return __CMP_A_V(x, y, i16, f64, f64, op##F64, len, offset, res);                           \
            case MTYPE2(TYPE_I16, -TYPE_I16):                                                               \
                return __CMP_V_A(x, y, i16, i16, i16, op##I16, len, offset, res);                           \
            case MTYPE2(TYPE_I16, -TYPE_I32):                                                               \
                return __CMP_V_A(x, y, i16, i32, i32, op##I32, len, offset, res);                           \
            case MTYPE2(TYPE_I16, -TYPE_I64):                                                               \
                return __CMP_V_A(x, y, i16, i64, i64, op##I64, len, offset, res);                           \
            case MTYPE2(TYPE_I16, -TYPE_F64):                                                               \
                return __CMP_V_A(x, y, i16, f64, f64, op##F64, len, offset, res);                           \
            case MTYPE2(TYPE_I16, TYPE_I16):                                                                \
                return __CMP_V_V(x, y, i16, i16, i16, op##I16, len, offset, res);                           \
            case MTYPE2(TYPE_I16, TYPE_I32):                                                                \
                return __CMP_V_V(x, y, i16, i32, i32, op##I32, len, offset, res);                           \
            case MTYPE2(TYPE_I16, TYPE_I64):                                                                \
                return __CMP_V_V(x, y, i16, i64, i64, op##I64, len, offset, res);                           \
            case MTYPE2(TYPE_I16, TYPE_F64):                                                                \
                return __CMP_V_V(x, y, i16, f64, f64, op##F64, len, offset, res);                           \
                                                                                                            \
            case MTYPE2(-TYPE_I32, -TYPE_I16):                                                              \
                return b8(op##I32(x->i32, i16_to_i32(y->i16)));                                             \
            case MTYPE2(-TYPE_I32, -TYPE_I32):                                                              \
            case MTYPE2(-TYPE_DATE, -TYPE_DATE):                                                            \
            case MTYPE2(-TYPE_TIME, -TYPE_TIME):                                                            \
                return b8(op##I32(x->i32, y->i32));                                                         \
            case MTYPE2(-TYPE_I32, -TYPE_I64):                                                              \
                return b8(op##I64(i32_to_i64(x->i32), y->i64));                                             \
            case MTYPE2(-TYPE_I32, -TYPE_F64):                                                              \
                return b8(op##F64(i32_to_f64(x->i32), y->f64));                                             \
            case MTYPE2(-TYPE_I32, TYPE_I16):                                                               \
                return __CMP_A_V(x, y, i32, i16, i32, op##I32, len, offset, res);                           \
            case MTYPE2(-TYPE_I32, TYPE_I32):                                                               \
            case MTYPE2(-TYPE_DATE, TYPE_DATE):                                                             \
            case MTYPE2(-TYPE_TIME, TYPE_TIME):                                                             \
                return __CMP_A_V(x, y, i32, i32, i32, op##I32, len, offset, res);                           \
            case MTYPE2(-TYPE_I32, TYPE_I64):                                                               \
                return __CMP_A_V(x, y, i32, i64, i64, op##I64, len, offset, res);                           \
            case MTYPE2(-TYPE_I32, TYPE_F64):                                                               \
                return __CMP_A_V(x, y, i32, f64, f64, op##F64, len, offset, res);                           \
            case MTYPE2(TYPE_I32, -TYPE_I16):                                                               \
                return __CMP_V_A(x, y, i32, i16, i32, op##I32, len, offset, res);                           \
            case MTYPE2(TYPE_I32, -TYPE_I32):                                                               \
            case MTYPE2(TYPE_DATE, -TYPE_DATE):                                                             \
            case MTYPE2(TYPE_TIME, -TYPE_TIME):                                                             \
                return __CMP_V_A(x, y, i32, i32, i32, op##I32, len, offset, res);                           \
            case MTYPE2(TYPE_I32, -TYPE_I64):                                                               \
                return __CMP_V_A(x, y, i32, i64, i64, op##I64, len, offset, res);                           \
            case MTYPE2(TYPE_I32, -TYPE_F64):                                                               \
                return __CMP_V_A(x, y, i32, f64, f64, op##F64, len, offset, res);                           \
            case MTYPE2(TYPE_I32, TYPE_I16):                                                                \
                return __CMP_V_V(x, y, i32, i16, i32, op##I32, len, offset, res);                           \
            case MTYPE2(TYPE_I32, TYPE_I32):                                                                \
            case MTYPE2(TYPE_DATE, TYPE_DATE):                                                              \
            case MTYPE2(TYPE_TIME, TYPE_TIME):                                                              \
                return __CMP_V_V(x, y, i32, i32, i32, op##I32, len, offset, res);                           \
            case MTYPE2(TYPE_I32, TYPE_I64):                                                                \
                return __CMP_V_V(x, y, i32, i64, i64, op##I64, len, offset, res);                           \
            case MTYPE2(TYPE_I32, TYPE_F64):                                                                \
                return __CMP_V_V(x, y, i32, f64, f64, op##F64, len, offset, res);                           \
                                                                                                            \
            case MTYPE2(-TYPE_I64, -TYPE_I16):                                                              \
                return b8(op##I64(x->i64, i16_to_i64(y->i16)));                                             \
            case MTYPE2(-TYPE_I64, -TYPE_I32):                                                              \
                return b8(op##I64(x->i64, i32_to_i64(y->i32)));                                             \
            case MTYPE2(-TYPE_I64, -TYPE_I64):                                                              \
            case MTYPE2(-TYPE_SYMBOL, -TYPE_SYMBOL):                                                        \
            case MTYPE2(-TYPE_TIMESTAMP, -TYPE_TIMESTAMP):                                                  \
                return b8(op##I64(x->i64, y->i64));                                                         \
            case MTYPE2(-TYPE_I64, -TYPE_F64):                                                              \
                return b8(op##F64(i64_to_f64(x->i64), y->f64));                                             \
            case MTYPE2(-TYPE_I64, TYPE_I16):                                                               \
                return __CMP_A_V(x, y, i64, i16, i64, op##I64, len, offset, res);                           \
            case MTYPE2(-TYPE_I64, TYPE_I32):                                                               \
                return __CMP_A_V(x, y, i64, i32, i64, op##I64, len, offset, res);                           \
            case MTYPE2(-TYPE_I64, TYPE_I64):                                                               \
            case MTYPE2(-TYPE_SYMBOL, TYPE_SYMBOL):                                                         \
            case MTYPE2(-TYPE_TIMESTAMP, TYPE_TIMESTAMP):                                                   \
                return __CMP_A_V(x, y, i64, i64, i64, op##I64, len, offset, res);                           \
            case MTYPE2(-TYPE_I64, TYPE_F64):                                                               \
                return __CMP_A_V(x, y, i64, f64, f64, op##F64, len, offset, res);                           \
            case MTYPE2(TYPE_I64, -TYPE_I16):                                                               \
                return __CMP_V_A(x, y, i64, i16, i64, op##I64, len, offset, res);                           \
            case MTYPE2(TYPE_I64, -TYPE_I32):                                                               \
                return __CMP_V_A(x, y, i64, i32, i64, op##I64, len, offset, res);                           \
            case MTYPE2(TYPE_I64, -TYPE_I64):                                                               \
            case MTYPE2(TYPE_SYMBOL, -TYPE_SYMBOL):                                                         \
            case MTYPE2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):                                                   \
                return __CMP_V_A(x, y, i64, i64, i64, op##I64, len, offset, res);                           \
            case MTYPE2(TYPE_I64, -TYPE_F64):                                                               \
                return __CMP_V_A(x, y, i64, f64, f64, op##F64, len, offset, res);                           \
            case MTYPE2(TYPE_I64, TYPE_I16):                                                                \
                return __CMP_V_V(x, y, i64, i16, i64, op##I64, len, offset, res);                           \
            case MTYPE2(TYPE_I64, TYPE_I32):                                                                \
                return __CMP_V_V(x, y, i64, i32, i64, op##I64, len, offset, res);                           \
            case MTYPE2(TYPE_I64, TYPE_I64):                                                                \
            case MTYPE2(TYPE_SYMBOL, TYPE_SYMBOL):                                                          \
            case MTYPE2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):                                                    \
                return __CMP_V_V(x, y, i64, i64, i64, op##I64, len, offset, res);                           \
            case MTYPE2(TYPE_I64, TYPE_F64):                                                                \
                return __CMP_V_V(x, y, i64, f64, f64, op##F64, len, offset, res);                           \
                                                                                                            \
            case MTYPE2(-TYPE_F64, -TYPE_I16):                                                              \
                return b8(op##F64(x->f64, i16_to_f64(y->i16)));                                             \
            case MTYPE2(-TYPE_F64, -TYPE_I32):                                                              \
                return b8(op##F64(x->f64, i32_to_f64(y->i32)));                                             \
            case MTYPE2(-TYPE_F64, -TYPE_I64):                                                              \
                return b8(op##F64(x->f64, i64_to_f64(y->i64)));                                             \
            case MTYPE2(-TYPE_F64, -TYPE_F64):                                                              \
                return b8(op##F64(x->f64, y->f64));                                                         \
            case MTYPE2(-TYPE_F64, TYPE_I16):                                                               \
                return __CMP_A_V(x, y, f64, i16, f64, op##F64, len, offset, res);                           \
            case MTYPE2(-TYPE_F64, TYPE_I32):                                                               \
                return __CMP_A_V(x, y, f64, i32, f64, op##F64, len, offset, res);                           \
            case MTYPE2(-TYPE_F64, TYPE_I64):                                                               \
                return __CMP_A_V(x, y, f64, i64, f64, op##F64, len, offset, res);                           \
            case MTYPE2(-TYPE_F64, TYPE_F64):                                                               \
                return __CMP_A_V(x, y, f64, f64, f64, op##F64, len, offset, res);                           \
            case MTYPE2(TYPE_F64, -TYPE_I16):                                                               \
                return __CMP_V_A(x, y, f64, i16, f64, op##F64, len, offset, res);                           \
            case MTYPE2(TYPE_F64, -TYPE_I32):                                                               \
                return __CMP_V_A(x, y, f64, i32, f64, op##F64, len, offset, res);                           \
            case MTYPE2(TYPE_F64, -TYPE_I64):                                                               \
                return __CMP_V_A(x, y, f64, i64, f64, op##F64, len, offset, res);                           \
            case MTYPE2(TYPE_F64, -TYPE_F64):                                                               \
                return __CMP_V_A(x, y, f64, f64, f64, op##F64, len, offset, res);                           \
            case MTYPE2(TYPE_F64, TYPE_I16):                                                                \
                return __CMP_V_V(x, y, f64, i16, f64, op##F64, len, offset, res);                           \
            case MTYPE2(TYPE_F64, TYPE_I32):                                                                \
                return __CMP_V_V(x, y, f64, i32, f64, op##F64, len, offset, res);                           \
            case MTYPE2(TYPE_F64, TYPE_I64):                                                                \
                return __CMP_V_V(x, y, f64, i64, f64, op##F64, len, offset, res);                           \
            case MTYPE2(TYPE_F64, TYPE_F64):                                                                \
                return __CMP_V_V(x, y, f64, f64, f64, op##F64, len, offset, res);                           \
                                                                                                            \
            case MTYPE2(-TYPE_DATE, -TYPE_TIMESTAMP):                                                       \
                return b8(op##F64(date_to_timestamp(x->i32), y->i64));                                      \
            case MTYPE2(-TYPE_DATE, TYPE_TIMESTAMP):                                                        \
                return __CMP_A_V(x, y, date, timestamp, timestamp, op##I64, len, offset, res);              \
            case MTYPE2(TYPE_DATE, -TYPE_TIMESTAMP):                                                        \
                return __CMP_V_A(x, y, date, timestamp, timestamp, op##I64, len, offset, res);              \
            case MTYPE2(TYPE_DATE, TYPE_TIMESTAMP):                                                         \
                return __CMP_V_V(x, y, date, timestamp, timestamp, op##I64, len, offset, res);              \
            case MTYPE2(-TYPE_TIMESTAMP, -TYPE_DATE):                                                       \
                return b8(op##F64(x->i64, date_to_timestamp(y->i32)));                                      \
            case MTYPE2(-TYPE_TIMESTAMP, TYPE_DATE):                                                        \
                return __CMP_A_V(x, y, timestamp, date, timestamp, op##I64, len, offset, res);              \
            case MTYPE2(TYPE_TIMESTAMP, -TYPE_DATE):                                                        \
                return __CMP_V_A(x, y, timestamp, date, timestamp, op##I64, len, offset, res);              \
            case MTYPE2(TYPE_TIMESTAMP, TYPE_DATE):                                                         \
                return __CMP_V_V(x, y, timestamp, date, timestamp, op##I64, len, offset, res);              \
                                                                                                            \
            case MTYPE2(TYPE_ENUM, -TYPE_SYMBOL):                                                           \
                k = ray_key(x);                                                                             \
                sym = ray_get(k);                                                                           \
                drop_obj(k);                                                                                \
                                                                                                            \
                e = ENUM_VAL(x);                                                                            \
                                                                                                            \
                if (is_null(sym) || sym->type != TYPE_SYMBOL) {                                             \
                    drop_obj(sym);                                                                          \
                    THROW(ERR_TYPE, "eq: invalid enum");                                                    \
                }                                                                                           \
                                                                                                            \
                xi = AS_I64(sym);                                                                           \
                ei = AS_I64(e) + offset;                                                                    \
                si = y->i64;                                                                                \
                out = AS_B8(res) + offset;                                                                  \
                                                                                                            \
                for (i = 0; i < len; i++)                                                                   \
                    out[i] = xi[ei[i]] == si;                                                               \
                                                                                                            \
                drop_obj(sym);                                                                              \
                return res;                                                                                 \
            case MTYPE2(-TYPE_SYMBOL, TYPE_ENUM):                                                           \
                return ray_##op##_partial(y, x, len, offset, res);                                          \
            case MTYPE2(TYPE_ENUM, TYPE_SYMBOL):                                                            \
                k = ray_key(y);                                                                             \
                sym = ray_get(k);                                                                           \
                drop_obj(k);                                                                                \
                                                                                                            \
                e = ENUM_VAL(y);                                                                            \
                                                                                                            \
                if (is_null(sym) || sym->type != TYPE_SYMBOL) {                                             \
                    drop_obj(sym);                                                                          \
                    THROW(ERR_TYPE, "eq: invalid enum");                                                    \
                }                                                                                           \
                                                                                                            \
                xi = AS_I64(x);                                                                             \
                ei = AS_I64(e) + offset;                                                                    \
                si = sym->i64;                                                                              \
                out = AS_B8(res) + offset;                                                                  \
                                                                                                            \
                for (i = 0; i < len; i++)                                                                   \
                    out[i] = xi[i] == ei[si];                                                               \
                                                                                                            \
                drop_obj(sym);                                                                              \
                return res;                                                                                 \
            case MTYPE2(TYPE_SYMBOL, TYPE_ENUM):                                                            \
                return ray_##op##_partial(y, x, len, offset, res);                                          \
                                                                                                            \
            case MTYPE2(-TYPE_GUID, -TYPE_GUID):                                                            \
                return b8(op##GUID(AS_GUID(x)[0], AS_GUID(y)[0]));                                          \
            case MTYPE2(-TYPE_GUID, TYPE_GUID):                                                             \
                out = AS_B8(res) + offset;                                                                  \
                for (i = 0; i < len; i++)                                                                   \
                    out[i] = op##GUID(AS_GUID(x)[0], AS_GUID(y)[i]);                                        \
                return res;                                                                                 \
            case MTYPE2(TYPE_GUID, -TYPE_GUID):                                                             \
                out = AS_B8(res) + offset;                                                                  \
                for (i = 0; i < len; i++)                                                                   \
                    out[i] = op##GUID(AS_GUID(x)[i], AS_GUID(y)[0]);                                        \
                return res;                                                                                 \
            case MTYPE2(TYPE_GUID, TYPE_GUID):                                                              \
                out = AS_B8(res) + offset;                                                                  \
                for (i = 0; i < len; i++)                                                                   \
                    out[i] = op##GUID(AS_GUID(x)[i], AS_GUID(y)[i]);                                        \
                return res;                                                                                 \
            case MTYPE2(TYPE_ERROR, TYPE_ERROR):                                                            \
                return b8(cmp_obj(x, y) == 0);                                                              \
            case MTYPE2(TYPE_NULL, TYPE_NULL):                                                              \
                return b8(B8_TRUE);                                                                         \
            default:                                                                                        \
                THROW(ERR_TYPE, "eq: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type)); \
        }                                                                                                   \
    }

obj_p cmp_map(raw_p op, obj_p x, obj_p y) {
    pool_p pool = runtime_get()->pool;
    u64_t i, l, n, chunk;
    obj_p v, map, res;
    ray_cmp_f cmp_fn = (ray_cmp_f)op;

    if (x->type == TYPE_MAPCOMMON) {
        l = AS_LIST(x)[0]->len;
        res = B8(l);
        v = cmp_fn(AS_LIST(x)[0], y, l, 0, res);
        if (IS_ERROR(v)) {
            drop_obj(res);
            return v;
        }

        map = LIST(l);
        map->type = TYPE_PARTEDB8;

        for (i = 0; i < l; i++)
            AS_LIST(map)[i] = AS_B8(res)[i] ? b8(B8_TRUE) : NULL_OBJ;

        drop_obj(res);

        return map;
    } else if (y->type == TYPE_MAPCOMMON) {
        l = AS_LIST(y)[0]->len;
        res = B8(l);
        v = cmp_fn(x, AS_LIST(y)[0], l, 0, NULL_OBJ);
        if (IS_ERROR(v)) {
            drop_obj(res);
            return v;
        }

        map = LIST(l);
        map->type = TYPE_PARTEDB8;

        for (i = 0; i < l; i++)
            AS_LIST(map)[i] = AS_B8(res)[i] ? b8(B8_TRUE) : NULL_OBJ;

        drop_obj(res);

        return map;
    }

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(TYPE_C8, TYPE_C8):
        case MTYPE2(TYPE_C8, -TYPE_C8):
        case MTYPE2(-TYPE_C8, TYPE_C8):
            return cmp_fn(x, y, 1, 0, NULL_OBJ);
    }

    if (IS_VECTOR(x) && IS_VECTOR(y)) {
        if (x->len != y->len)
            THROW(ERR_LENGTH, "vectors must have the same length");

        l = x->len;
    } else if (IS_VECTOR(x))
        l = x->len;
    else if (IS_VECTOR(y))
        l = y->len;
    else {
        return cmp_fn(x, y, 1, 0, NULL_OBJ);
    }

    res = B8(l);
    if (l == 0)
        return res;

    n = pool_split_by(pool, l, 0);

    if (n == 1) {
        v = cmp_fn(x, y, l, 0, res);
        if (IS_ERROR(v)) {
            drop_obj(res);
            return v;
        }

        return res;
    }

    pool_prepare(pool);
    chunk = l / n;

    for (i = 0; i < n - 1; i++)
        pool_add_task(pool, op, 5, x, y, chunk, i * chunk, res);

    pool_add_task(pool, op, 5, x, y, l - i * chunk, i * chunk, res);

    v = pool_run(pool);
    if (IS_ERROR(v)) {
        drop_obj(res);
        return v;
    }

    drop_obj(v);

    return res;
}

__DECLARE_CMP_FN(EQ)
__DECLARE_CMP_FN(NE)
__DECLARE_CMP_FN(LT)
__DECLARE_CMP_FN(GT)
__DECLARE_CMP_FN(LE)
__DECLARE_CMP_FN(GE)

obj_p ray_eq(obj_p x, obj_p y) { return cmp_map(ray_EQ_partial, x, y); }
obj_p ray_ne(obj_p x, obj_p y) { return cmp_map(ray_NE_partial, x, y); }
obj_p ray_lt(obj_p x, obj_p y) { return cmp_map(ray_LT_partial, x, y); }
obj_p ray_gt(obj_p x, obj_p y) { return cmp_map(ray_GT_partial, x, y); }
obj_p ray_le(obj_p x, obj_p y) { return cmp_map(ray_LE_partial, x, y); }
obj_p ray_ge(obj_p x, obj_p y) { return cmp_map(ray_GE_partial, x, y); }
