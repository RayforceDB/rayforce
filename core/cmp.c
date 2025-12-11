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
#include "ops.h"
#include "error.h"
#include "items.h"
#include "runtime.h"
#include "pool.h"

typedef obj_p (*ray_cmp_f)(obj_p, obj_p, i64_t, i64_t, obj_p);

#define __CMP_A_V(x, y, lt, rt, mt, op, ln, of, ov)                              \
    ({                                                                           \
        __BASE_##rt##_t *$rhs;                                                   \
        b8_t *$out;                                                              \
        $rhs = __AS_##rt(y) + of;                                                \
        $out = AS_B8(ov) + of;                                                   \
        for (i64_t $i = 0; $i < ln; $i++)                                        \
            $out[$i] = op(lt##_to_##mt(x->__BASE_##lt), rt##_to_##mt($rhs[$i])); \
        NULL_OBJ;                                                                \
    })

#define __CMP_V_A(x, y, lt, rt, mt, op, ln, of, ov)                              \
    ({                                                                           \
        __BASE_##lt##_t *$lhs;                                                   \
        b8_t *$out;                                                              \
        $lhs = __AS_##lt(x) + of;                                                \
        $out = AS_B8(ov) + of;                                                   \
        for (i64_t $i = 0; $i < ln; $i++)                                        \
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
        for (i64_t $i = 0; $i < ln; $i++)                                  \
            $out[$i] = op(lt##_to_##mt($lhs[$i]), rt##_to_##mt($rhs[$i])); \
        NULL_OBJ;                                                          \
    })

#define __DECLARE_CMP_FN(op)                                                                   \
    obj_p ray_##op##_partial(obj_p x, obj_p y, i64_t len, i64_t offset, obj_p res) {           \
        i64_t i;                                                                               \
        i64_t *xi, *ei, si;                                                                    \
        b8_t *out;                                                                             \
        obj_p k, sym, e;                                                                       \
                                                                                               \
        switch (MTYPE2(x->type, y->type)) {                                                    \
            case MTYPE2(-TYPE_B8, -TYPE_B8):                                                   \
                return b8(op##I8(x->b8, y->b8));                                               \
            case MTYPE2(-TYPE_U8, -TYPE_U8):                                                   \
                return b8(op##I8(x->u8, y->u8));                                               \
            case MTYPE2(-TYPE_B8, -TYPE_U8):                                                   \
                return b8(op##I8(x->b8, y->u8));                                               \
            case MTYPE2(-TYPE_U8, -TYPE_B8):                                                   \
                return b8(op##I8(x->u8, y->b8));                                               \
            case MTYPE2(-TYPE_C8, -TYPE_C8):                                                   \
                return b8(op##C8(x->c8, y->c8));                                               \
            case MTYPE2(-TYPE_C8, TYPE_C8):                                                    \
                return b8(op##STR((lit_p)(&x->c8), 1, AS_C8(y), y->len));                      \
            case MTYPE2(TYPE_C8, -TYPE_C8):                                                    \
                return b8(op##STR(AS_C8(x), x->len, (lit_p)(&y->c8), 1));                      \
            case MTYPE2(TYPE_C8, TYPE_C8):                                                     \
                return b8(op##STR(AS_C8(x), x->len, AS_C8(y), y->len));                        \
                                                                                               \
            case MTYPE2(-TYPE_I16, -TYPE_I16):                                                 \
                return b8(op##I16(x->i16, y->i16));                                            \
            case MTYPE2(-TYPE_I16, -TYPE_I32):                                                 \
                return b8(op##I32(i16_to_i32(x->i16), y->i32));                                \
            case MTYPE2(-TYPE_I16, -TYPE_I64):                                                 \
                return b8(op##I64(i16_to_i64(x->i16), y->i64));                                \
            case MTYPE2(-TYPE_I16, -TYPE_F64):                                                 \
                return b8(op##F64(i16_to_f64(x->i16), y->f64));                                \
            case MTYPE2(-TYPE_I16, TYPE_I16):                                                  \
                return __CMP_A_V(x, y, i16, i16, i16, op##I16, len, offset, res);              \
            case MTYPE2(-TYPE_I16, TYPE_I32):                                                  \
                return __CMP_A_V(x, y, i16, i32, i32, op##I32, len, offset, res);              \
            case MTYPE2(-TYPE_I16, TYPE_I64):                                                  \
                return __CMP_A_V(x, y, i16, i64, i64, op##I64, len, offset, res);              \
            case MTYPE2(-TYPE_I16, TYPE_F64):                                                  \
                return __CMP_A_V(x, y, i16, f64, f64, op##F64, len, offset, res);              \
            case MTYPE2(TYPE_I16, -TYPE_I16):                                                  \
                return __CMP_V_A(x, y, i16, i16, i16, op##I16, len, offset, res);              \
            case MTYPE2(TYPE_I16, -TYPE_I32):                                                  \
                return __CMP_V_A(x, y, i16, i32, i32, op##I32, len, offset, res);              \
            case MTYPE2(TYPE_I16, -TYPE_I64):                                                  \
                return __CMP_V_A(x, y, i16, i64, i64, op##I64, len, offset, res);              \
            case MTYPE2(TYPE_I16, -TYPE_F64):                                                  \
                return __CMP_V_A(x, y, i16, f64, f64, op##F64, len, offset, res);              \
            case MTYPE2(TYPE_I16, TYPE_I16):                                                   \
                return __CMP_V_V(x, y, i16, i16, i16, op##I16, len, offset, res);              \
            case MTYPE2(TYPE_I16, TYPE_I32):                                                   \
                return __CMP_V_V(x, y, i16, i32, i32, op##I32, len, offset, res);              \
            case MTYPE2(TYPE_I16, TYPE_I64):                                                   \
                return __CMP_V_V(x, y, i16, i64, i64, op##I64, len, offset, res);              \
            case MTYPE2(TYPE_I16, TYPE_F64):                                                   \
                return __CMP_V_V(x, y, i16, f64, f64, op##F64, len, offset, res);              \
                                                                                               \
            case MTYPE2(-TYPE_I32, -TYPE_I16):                                                 \
                return b8(op##I32(x->i32, i16_to_i32(y->i16)));                                \
            case MTYPE2(-TYPE_I32, -TYPE_I32):                                                 \
            case MTYPE2(-TYPE_DATE, -TYPE_DATE):                                               \
            case MTYPE2(-TYPE_TIME, -TYPE_TIME):                                               \
                return b8(op##I32(x->i32, y->i32));                                            \
            case MTYPE2(-TYPE_I32, -TYPE_I64):                                                 \
                return b8(op##I64(i32_to_i64(x->i32), y->i64));                                \
            case MTYPE2(-TYPE_I32, -TYPE_F64):                                                 \
                return b8(op##F64(i32_to_f64(x->i32), y->f64));                                \
            case MTYPE2(-TYPE_I32, TYPE_I16):                                                  \
                return __CMP_A_V(x, y, i32, i16, i32, op##I32, len, offset, res);              \
            case MTYPE2(-TYPE_I32, TYPE_I32):                                                  \
            case MTYPE2(-TYPE_DATE, TYPE_DATE):                                                \
            case MTYPE2(-TYPE_TIME, TYPE_TIME):                                                \
                return __CMP_A_V(x, y, i32, i32, i32, op##I32, len, offset, res);              \
            case MTYPE2(-TYPE_I32, TYPE_I64):                                                  \
                return __CMP_A_V(x, y, i32, i64, i64, op##I64, len, offset, res);              \
            case MTYPE2(-TYPE_I32, TYPE_F64):                                                  \
                return __CMP_A_V(x, y, i32, f64, f64, op##F64, len, offset, res);              \
            case MTYPE2(TYPE_I32, -TYPE_I16):                                                  \
                return __CMP_V_A(x, y, i32, i16, i32, op##I32, len, offset, res);              \
            case MTYPE2(TYPE_I32, -TYPE_I32):                                                  \
            case MTYPE2(TYPE_DATE, -TYPE_DATE):                                                \
            case MTYPE2(TYPE_TIME, -TYPE_TIME):                                                \
                return __CMP_V_A(x, y, i32, i32, i32, op##I32, len, offset, res);              \
            case MTYPE2(TYPE_I32, -TYPE_I64):                                                  \
                return __CMP_V_A(x, y, i32, i64, i64, op##I64, len, offset, res);              \
            case MTYPE2(TYPE_I32, -TYPE_F64):                                                  \
                return __CMP_V_A(x, y, i32, f64, f64, op##F64, len, offset, res);              \
            case MTYPE2(TYPE_I32, TYPE_I16):                                                   \
                return __CMP_V_V(x, y, i32, i16, i32, op##I32, len, offset, res);              \
            case MTYPE2(TYPE_I32, TYPE_I32):                                                   \
            case MTYPE2(TYPE_DATE, TYPE_DATE):                                                 \
            case MTYPE2(TYPE_TIME, TYPE_TIME):                                                 \
                return __CMP_V_V(x, y, i32, i32, i32, op##I32, len, offset, res);              \
            case MTYPE2(TYPE_I32, TYPE_I64):                                                   \
                return __CMP_V_V(x, y, i32, i64, i64, op##I64, len, offset, res);              \
            case MTYPE2(TYPE_I32, TYPE_F64):                                                   \
                return __CMP_V_V(x, y, i32, f64, f64, op##F64, len, offset, res);              \
                                                                                               \
            case MTYPE2(-TYPE_I64, -TYPE_I16):                                                 \
                return b8(op##I64(x->i64, i16_to_i64(y->i16)));                                \
            case MTYPE2(-TYPE_I64, -TYPE_I32):                                                 \
                return b8(op##I64(x->i64, i32_to_i64(y->i32)));                                \
            case MTYPE2(-TYPE_I64, -TYPE_I64):                                                 \
            case MTYPE2(-TYPE_SYMBOL, -TYPE_SYMBOL):                                           \
            case MTYPE2(-TYPE_TIMESTAMP, -TYPE_TIMESTAMP):                                     \
                return b8(op##I64(x->i64, y->i64));                                            \
            case MTYPE2(-TYPE_I64, -TYPE_F64):                                                 \
                return b8(op##F64(i64_to_f64(x->i64), y->f64));                                \
            case MTYPE2(-TYPE_I64, TYPE_I16):                                                  \
                return __CMP_A_V(x, y, i64, i16, i64, op##I64, len, offset, res);              \
            case MTYPE2(-TYPE_I64, TYPE_I32):                                                  \
                return __CMP_A_V(x, y, i64, i32, i64, op##I64, len, offset, res);              \
            case MTYPE2(-TYPE_I64, TYPE_I64):                                                  \
            case MTYPE2(-TYPE_SYMBOL, TYPE_SYMBOL):                                            \
            case MTYPE2(-TYPE_TIMESTAMP, TYPE_TIMESTAMP):                                      \
                return __CMP_A_V(x, y, i64, i64, i64, op##I64, len, offset, res);              \
            case MTYPE2(-TYPE_I64, TYPE_F64):                                                  \
                return __CMP_A_V(x, y, i64, f64, f64, op##F64, len, offset, res);              \
            case MTYPE2(TYPE_I64, -TYPE_I16):                                                  \
                return __CMP_V_A(x, y, i64, i16, i64, op##I64, len, offset, res);              \
            case MTYPE2(TYPE_I64, -TYPE_I32):                                                  \
                return __CMP_V_A(x, y, i64, i32, i64, op##I64, len, offset, res);              \
            case MTYPE2(TYPE_I64, -TYPE_I64):                                                  \
            case MTYPE2(TYPE_SYMBOL, -TYPE_SYMBOL):                                            \
            case MTYPE2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):                                      \
                return __CMP_V_A(x, y, i64, i64, i64, op##I64, len, offset, res);              \
            case MTYPE2(TYPE_I64, -TYPE_F64):                                                  \
                return __CMP_V_A(x, y, i64, f64, f64, op##F64, len, offset, res);              \
            case MTYPE2(TYPE_I64, TYPE_I16):                                                   \
                return __CMP_V_V(x, y, i64, i16, i64, op##I64, len, offset, res);              \
            case MTYPE2(TYPE_I64, TYPE_I32):                                                   \
                return __CMP_V_V(x, y, i64, i32, i64, op##I64, len, offset, res);              \
            case MTYPE2(TYPE_I64, TYPE_I64):                                                   \
            case MTYPE2(TYPE_SYMBOL, TYPE_SYMBOL):                                             \
            case MTYPE2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):                                       \
                return __CMP_V_V(x, y, i64, i64, i64, op##I64, len, offset, res);              \
            case MTYPE2(TYPE_I64, TYPE_F64):                                                   \
                return __CMP_V_V(x, y, i64, f64, f64, op##F64, len, offset, res);              \
                                                                                               \
            case MTYPE2(-TYPE_F64, -TYPE_I16):                                                 \
                return b8(op##F64(x->f64, i16_to_f64(y->i16)));                                \
            case MTYPE2(-TYPE_F64, -TYPE_I32):                                                 \
                return b8(op##F64(x->f64, i32_to_f64(y->i32)));                                \
            case MTYPE2(-TYPE_F64, -TYPE_I64):                                                 \
                return b8(op##F64(x->f64, i64_to_f64(y->i64)));                                \
            case MTYPE2(-TYPE_F64, -TYPE_F64):                                                 \
                return b8(op##F64(x->f64, y->f64));                                            \
            case MTYPE2(-TYPE_F64, TYPE_I16):                                                  \
                return __CMP_A_V(x, y, f64, i16, f64, op##F64, len, offset, res);              \
            case MTYPE2(-TYPE_F64, TYPE_I32):                                                  \
                return __CMP_A_V(x, y, f64, i32, f64, op##F64, len, offset, res);              \
            case MTYPE2(-TYPE_F64, TYPE_I64):                                                  \
                return __CMP_A_V(x, y, f64, i64, f64, op##F64, len, offset, res);              \
            case MTYPE2(-TYPE_F64, TYPE_F64):                                                  \
                return __CMP_A_V(x, y, f64, f64, f64, op##F64, len, offset, res);              \
            case MTYPE2(TYPE_F64, -TYPE_I16):                                                  \
                return __CMP_V_A(x, y, f64, i16, f64, op##F64, len, offset, res);              \
            case MTYPE2(TYPE_F64, -TYPE_I32):                                                  \
                return __CMP_V_A(x, y, f64, i32, f64, op##F64, len, offset, res);              \
            case MTYPE2(TYPE_F64, -TYPE_I64):                                                  \
                return __CMP_V_A(x, y, f64, i64, f64, op##F64, len, offset, res);              \
            case MTYPE2(TYPE_F64, -TYPE_F64):                                                  \
                return __CMP_V_A(x, y, f64, f64, f64, op##F64, len, offset, res);              \
            case MTYPE2(TYPE_F64, TYPE_I16):                                                   \
                return __CMP_V_V(x, y, f64, i16, f64, op##F64, len, offset, res);              \
            case MTYPE2(TYPE_F64, TYPE_I32):                                                   \
                return __CMP_V_V(x, y, f64, i32, f64, op##F64, len, offset, res);              \
            case MTYPE2(TYPE_F64, TYPE_I64):                                                   \
                return __CMP_V_V(x, y, f64, i64, f64, op##F64, len, offset, res);              \
            case MTYPE2(TYPE_F64, TYPE_F64):                                                   \
                return __CMP_V_V(x, y, f64, f64, f64, op##F64, len, offset, res);              \
                                                                                               \
            case MTYPE2(-TYPE_DATE, -TYPE_TIMESTAMP):                                          \
                return b8(op##F64(date_to_timestamp(x->i32), y->i64));                         \
            case MTYPE2(-TYPE_DATE, TYPE_TIMESTAMP):                                           \
                return __CMP_A_V(x, y, date, timestamp, timestamp, op##I64, len, offset, res); \
            case MTYPE2(TYPE_DATE, -TYPE_TIMESTAMP):                                           \
                return __CMP_V_A(x, y, date, timestamp, timestamp, op##I64, len, offset, res); \
            case MTYPE2(TYPE_DATE, TYPE_TIMESTAMP):                                            \
                return __CMP_V_V(x, y, date, timestamp, timestamp, op##I64, len, offset, res); \
            case MTYPE2(-TYPE_TIMESTAMP, -TYPE_DATE):                                          \
                return b8(op##F64(x->i64, date_to_timestamp(y->i32)));                         \
            case MTYPE2(-TYPE_TIMESTAMP, TYPE_DATE):                                           \
                return __CMP_A_V(x, y, timestamp, date, timestamp, op##I64, len, offset, res); \
            case MTYPE2(TYPE_TIMESTAMP, -TYPE_DATE):                                           \
                return __CMP_V_A(x, y, timestamp, date, timestamp, op##I64, len, offset, res); \
            case MTYPE2(TYPE_TIMESTAMP, TYPE_DATE):                                            \
                return __CMP_V_V(x, y, timestamp, date, timestamp, op##I64, len, offset, res); \
                                                                                               \
            case MTYPE2(TYPE_ENUM, -TYPE_SYMBOL):                                              \
                k = ray_key(x);                                                                \
                sym = ray_get(k);                                                              \
                drop_obj(k);                                                                   \
                                                                                               \
                e = ENUM_VAL(x);                                                               \
                                                                                               \
                if (is_null(sym) || sym->type != TYPE_SYMBOL) {                                \
                    drop_obj(sym);                                                             \
                    THROW_S(ERR_TYPE, "eq: invalid enum");                                       \
                }                                                                              \
                                                                                               \
                xi = AS_I64(sym);                                                              \
                ei = AS_I64(e) + offset;                                                       \
                si = y->i64;                                                                   \
                out = AS_B8(res) + offset;                                                     \
                                                                                               \
                for (i = 0; i < len; i++)                                                      \
                    out[i] = xi[ei[i]] == si;                                                  \
                                                                                               \
                drop_obj(sym);                                                                 \
                return res;                                                                    \
            case MTYPE2(-TYPE_SYMBOL, TYPE_ENUM):                                              \
                return ray_##op##_partial(y, x, len, offset, res);                             \
            case MTYPE2(TYPE_ENUM, TYPE_SYMBOL):                                               \
                k = ray_key(y);                                                                \
                sym = ray_get(k);                                                              \
                drop_obj(k);                                                                   \
                                                                                               \
                e = ENUM_VAL(y);                                                               \
                                                                                               \
                if (is_null(sym) || sym->type != TYPE_SYMBOL) {                                \
                    drop_obj(sym);                                                             \
                    THROW_S(ERR_TYPE, "eq: invalid enum");                                       \
                }                                                                              \
                                                                                               \
                xi = AS_I64(x);                                                                \
                ei = AS_I64(e) + offset;                                                       \
                si = sym->i64;                                                                 \
                out = AS_B8(res) + offset;                                                     \
                                                                                               \
                for (i = 0; i < len; i++)                                                      \
                    out[i] = xi[i] == ei[si];                                                  \
                                                                                               \
                drop_obj(sym);                                                                 \
                return res;                                                                    \
            case MTYPE2(TYPE_SYMBOL, TYPE_ENUM):                                               \
                return ray_##op##_partial(y, x, len, offset, res);                             \
                                                                                               \
            case MTYPE2(-TYPE_GUID, -TYPE_GUID):                                               \
                return b8(op##GUID(AS_GUID(x)[0], AS_GUID(y)[0]));                             \
            case MTYPE2(-TYPE_GUID, TYPE_GUID):                                                \
                out = AS_B8(res) + offset;                                                     \
                for (i = 0; i < len; i++)                                                      \
                    out[i] = op##GUID(AS_GUID(x)[0], AS_GUID(y)[i]);                           \
                return res;                                                                    \
            case MTYPE2(TYPE_GUID, -TYPE_GUID):                                                \
                out = AS_B8(res) + offset;                                                     \
                for (i = 0; i < len; i++)                                                      \
                    out[i] = op##GUID(AS_GUID(x)[i], AS_GUID(y)[0]);                           \
                return res;                                                                    \
            case MTYPE2(TYPE_GUID, TYPE_GUID):                                                 \
                out = AS_B8(res) + offset;                                                     \
                for (i = 0; i < len; i++)                                                      \
                    out[i] = op##GUID(AS_GUID(x)[i], AS_GUID(y)[i]);                           \
                return res;                                                                    \
            case MTYPE2(TYPE_ERR, TYPE_ERR):                                                   \
                return b8(cmp_obj(x, y) == 0);                                                 \
            case MTYPE2(TYPE_NULL, TYPE_NULL):                                                 \
                return b8(B8_TRUE);                                                            \
            default:                                                                           \
                THROW_TYPE2("eq", x->type, y->type);                                           \
        }                                                                                      \
    }

obj_p cmp_map(raw_p op, obj_p x, obj_p y) {
    pool_p pool = runtime_get()->pool;
    i64_t i, l, n;
    obj_p v, map, res;
    ray_cmp_f cmp_fn = (ray_cmp_f)op;

    if (x->type == TYPE_MAPCOMMON) {
        l = AS_LIST(x)[0]->len;
        res = B8(l);
        v = cmp_fn(AS_LIST(x)[0], y, l, 0, res);
        if (IS_ERR(v)) {
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
        if (IS_ERR(v)) {
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

    // Handle TYPE_MAPLIST (lazy list type)
    // Optimization: for MAPLIST comparison with atom, avoid full materialization
    if (x->type == TYPE_MAPLIST && IS_ATOM(y) && y->type < 0) {
        obj_p key = MAPLIST_KEY(x);
        obj_p offsets = MAPLIST_VAL(x);
        l = offsets->len;

        // Check the type of the first element
        if (l > 0) {
            u8_t *buf = AS_U8(key) + AS_I64(offsets)[0];
            i8_t elem_type = (i8_t)buf[0];

            // If MAPLIST contains i64 atoms, extract directly to typed vector
            if (elem_type == -TYPE_I64 && y->type == -TYPE_I64) {
                obj_p vec = I64(l);
                for (i = 0; i < l; i++) {
                    buf = AS_U8(key) + AS_I64(offsets)[i];
                    if (buf[0] == (u8_t)(-TYPE_I64)) {
                        memcpy(&AS_I64(vec)[i], buf + 1, sizeof(i64_t));
                    } else {
                        AS_I64(vec)[i] = NULL_I64;
                    }
                }
                res = cmp_map(op, vec, y);
                drop_obj(vec);
                return res;
            }
            // If MAPLIST contains f64 atoms, extract directly to typed vector
            if (elem_type == -TYPE_F64 && y->type == -TYPE_F64) {
                obj_p vec = F64(l);
                for (i = 0; i < l; i++) {
                    buf = AS_U8(key) + AS_I64(offsets)[i];
                    if (buf[0] == (u8_t)(-TYPE_F64)) {
                        memcpy(&AS_F64(vec)[i], buf + 1, sizeof(f64_t));
                    } else {
                        AS_F64(vec)[i] = NULL_F64;
                    }
                }
                res = cmp_map(op, vec, y);
                drop_obj(vec);
                return res;
            }
        }
        // Fall back to full materialization for other types
        v = ray_value(x);
        if (IS_ERR(v))
            return v;
        res = cmp_map(op, v, y);
        drop_obj(v);
        return res;
    }
    if (x->type == TYPE_MAPLIST) {
        v = ray_value(x);
        if (IS_ERR(v))
            return v;
        res = cmp_map(op, v, y);
        drop_obj(v);
        return res;
    } else if (y->type == TYPE_MAPLIST && IS_ATOM(x) && x->type < 0) {
        obj_p key = MAPLIST_KEY(y);
        obj_p offsets = MAPLIST_VAL(y);
        l = offsets->len;

        if (l > 0) {
            u8_t *buf = AS_U8(key) + AS_I64(offsets)[0];
            i8_t elem_type = (i8_t)buf[0];

            if (elem_type == -TYPE_I64 && x->type == -TYPE_I64) {
                obj_p vec = I64(l);
                for (i = 0; i < l; i++) {
                    buf = AS_U8(key) + AS_I64(offsets)[i];
                    if (buf[0] == (u8_t)(-TYPE_I64)) {
                        memcpy(&AS_I64(vec)[i], buf + 1, sizeof(i64_t));
                    } else {
                        AS_I64(vec)[i] = NULL_I64;
                    }
                }
                res = cmp_map(op, x, vec);
                drop_obj(vec);
                return res;
            }
            if (elem_type == -TYPE_F64 && x->type == -TYPE_F64) {
                obj_p vec = F64(l);
                for (i = 0; i < l; i++) {
                    buf = AS_U8(key) + AS_I64(offsets)[i];
                    if (buf[0] == (u8_t)(-TYPE_F64)) {
                        memcpy(&AS_F64(vec)[i], buf + 1, sizeof(f64_t));
                    } else {
                        AS_F64(vec)[i] = NULL_F64;
                    }
                }
                res = cmp_map(op, x, vec);
                drop_obj(vec);
                return res;
            }
        }
        v = ray_value(y);
        if (IS_ERR(v))
            return v;
        res = cmp_map(op, x, v);
        drop_obj(v);
        return res;
    } else if (y->type == TYPE_MAPLIST) {
        v = ray_value(y);
        if (IS_ERR(v))
            return v;
        res = cmp_map(op, x, v);
        drop_obj(v);
        return res;
    }

    // Handle TYPE_LIST (compare each element) - return TYPE_B8 vector
    // Optimization: if comparing with atom and list contains homogeneous atoms, convert to typed vector
    if (x->type == TYPE_LIST && x->len > 0 && IS_ATOM(y) && y->type < 0) {
        l = x->len;
        obj_p first = AS_LIST(x)[0];
        // Check if first element is an atom of same base type as y (or both numeric)
        if (first != NULL_OBJ && first->type < 0) {
            i8_t elem_type = -first->type;
            i8_t y_type = -y->type;

            // If element type matches y type, extract to typed vector and compare
            if (elem_type == y_type || (elem_type == TYPE_I64 && y_type == TYPE_I64) ||
                (elem_type == TYPE_F64 && y_type == TYPE_F64)) {
                // Create typed vector from list elements
                if (elem_type == TYPE_I64) {
                    obj_p vec = I64(l);
                    for (i = 0; i < l; i++) {
                        obj_p elem = AS_LIST(x)[i];
                        AS_I64(vec)[i] = (elem != NULL_OBJ && elem->type == -TYPE_I64) ? elem->i64 : NULL_I64;
                    }
                    res = cmp_map(op, vec, y);
                    drop_obj(vec);
                    return res;
                } else if (elem_type == TYPE_F64) {
                    obj_p vec = F64(l);
                    for (i = 0; i < l; i++) {
                        obj_p elem = AS_LIST(x)[i];
                        AS_F64(vec)[i] = (elem != NULL_OBJ && elem->type == -TYPE_F64) ? elem->f64 : NULL_F64;
                    }
                    res = cmp_map(op, vec, y);
                    drop_obj(vec);
                    return res;
                }
            }
        }
        // Fall through to element-by-element comparison
    }
    if (x->type == TYPE_LIST) {
        l = x->len;
        res = B8(l);
        for (i = 0; i < l; i++) {
            v = cmp_map(op, AS_LIST(x)[i], y);
            if (IS_ERR(v)) {
                drop_obj(res);
                return v;
            }
            // Extract boolean from result
            AS_B8(res)[i] = (v->type == -TYPE_B8 && v->b8) ? B8_TRUE : B8_FALSE;
            drop_obj(v);
        }
        return res;
    } else if (y->type == TYPE_LIST && y->len > 0 && IS_ATOM(x) && x->type < 0) {
        l = y->len;
        obj_p first = AS_LIST(y)[0];
        if (first != NULL_OBJ && first->type < 0) {
            i8_t elem_type = -first->type;
            i8_t x_type = -x->type;

            if (elem_type == x_type || (elem_type == TYPE_I64 && x_type == TYPE_I64) ||
                (elem_type == TYPE_F64 && x_type == TYPE_F64)) {
                if (elem_type == TYPE_I64) {
                    obj_p vec = I64(l);
                    for (i = 0; i < l; i++) {
                        obj_p elem = AS_LIST(y)[i];
                        AS_I64(vec)[i] = (elem != NULL_OBJ && elem->type == -TYPE_I64) ? elem->i64 : NULL_I64;
                    }
                    res = cmp_map(op, x, vec);
                    drop_obj(vec);
                    return res;
                } else if (elem_type == TYPE_F64) {
                    obj_p vec = F64(l);
                    for (i = 0; i < l; i++) {
                        obj_p elem = AS_LIST(y)[i];
                        AS_F64(vec)[i] = (elem != NULL_OBJ && elem->type == -TYPE_F64) ? elem->f64 : NULL_F64;
                    }
                    res = cmp_map(op, x, vec);
                    drop_obj(vec);
                    return res;
                }
            }
        }
    }
    if (y->type == TYPE_LIST) {
        l = y->len;
        res = B8(l);
        for (i = 0; i < l; i++) {
            v = cmp_map(op, x, AS_LIST(y)[i]);
            if (IS_ERR(v)) {
                drop_obj(res);
                return v;
            }
            // Extract boolean from result
            AS_B8(res)[i] = (v->type == -TYPE_B8 && v->b8) ? B8_TRUE : B8_FALSE;
            drop_obj(v);
        }
        return res;
    }

    // Handle parted types (TYPE_PARTEDLIST and derived)
    if (x->type >= TYPE_PARTEDLIST && x->type < TYPE_TABLE) {
        l = x->len;
        map = LIST(l);
        map->type = TYPE_PARTEDB8;

        for (i = 0; i < l; i++) {
            v = cmp_map(op, AS_LIST(x)[i], y);
            if (IS_ERR(v)) {
                map->len = i;
                drop_obj(map);
                return v;
            }
            AS_LIST(map)[i] = v;
        }

        return map;
    } else if (y->type >= TYPE_PARTEDLIST && y->type < TYPE_TABLE) {
        l = y->len;
        map = LIST(l);
        map->type = TYPE_PARTEDB8;

        for (i = 0; i < l; i++) {
            v = cmp_map(op, x, AS_LIST(y)[i]);
            if (IS_ERR(v)) {
                map->len = i;
                drop_obj(map);
                return v;
            }
            AS_LIST(map)[i] = v;
        }

        return map;
    }

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(TYPE_C8, TYPE_C8):
        case MTYPE2(TYPE_C8, -TYPE_C8):
        case MTYPE2(-TYPE_C8, TYPE_C8):
            return cmp_fn(x, y, 1, 0, NULL_OBJ);
        case MTYPE2(TYPE_DICT, TYPE_DICT):
        case MTYPE2(TYPE_TABLE, TYPE_TABLE):
            return b8(cmp_obj(x, y) == 0);
    }

    if (IS_VECTOR(x) && IS_VECTOR(y)) {
        if (x->len != y->len)
            THROW_S(ERR_LENGTH, ERR_MSG_VEC_SAME_LEN);

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
        if (IS_ERR(v)) {
            drop_obj(res);
            return v;
        }

        return res;
    }

    // --- PAGE-ALIGNED CHUNKING ---
    i64_t elem_size = sizeof(b8_t);  // result is always B8 vector
    i64_t page_size = RAY_PAGE_SIZE;
    i64_t elems_per_page = page_size / elem_size;
    if (elems_per_page == 0)
        elems_per_page = 1;
    i64_t base_chunk = (l + n - 1) / n;
    base_chunk = ((base_chunk + elems_per_page - 1) / elems_per_page) * elems_per_page;

    pool_prepare(pool);
    i64_t offset = 0;
    for (i = 0; i < n - 1; i++) {
        i64_t this_chunk = base_chunk;
        if (offset + this_chunk > l)
            this_chunk = l - offset;
        pool_add_task(pool, op, 5, x, y, this_chunk, offset, res);
        offset += this_chunk;
        if (offset >= l)
            break;
    }
    if (offset < l)
        pool_add_task(pool, op, 5, x, y, l - offset, offset, res);

    v = pool_run(pool);
    if (IS_ERR(v)) {
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
