/*
 *   Copyright (c) 2025 Anton Kundenko <singaraiona@gmail.com>
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

#include "serde.h"
#include "raykx.h"
#include "k.h"
#include <stdio.h>
#include <string.h>
#include "../../core/util.h"
#include "../../core/ops.h"
#include "../../core/log.h"
#include "../../core/error.h"
#include "../../core/symbols.h"

/*
 * Returns size (in bytes) that an obj occupy in memory via serialization into KDB+ IPC format
 */
i64_t raykx_size_obj(obj_p obj) {
    i64_t size, i, l;

    switch (obj->type) {
        case -TYPE_B8:
            return ISIZEOF(i8_t) + ISIZEOF(b8_t);
        case -TYPE_U8:
            return ISIZEOF(i8_t) + ISIZEOF(u8_t);
        case -TYPE_I16:
            return ISIZEOF(i8_t) + ISIZEOF(i16_t);
        case -TYPE_I32:
        case -TYPE_DATE:
        case -TYPE_TIME:
            return ISIZEOF(i8_t) + ISIZEOF(i32_t);
        case -TYPE_I64:
        case -TYPE_TIMESTAMP:
            return ISIZEOF(i8_t) + ISIZEOF(i64_t);
        case -TYPE_F64:
            return ISIZEOF(i8_t) + ISIZEOF(f64_t);
        case -TYPE_SYMBOL:
            return ISIZEOF(i8_t) + SYMBOL_STRLEN(obj->i64) + 1;
        case -TYPE_C8:
            return ISIZEOF(i8_t) + ISIZEOF(c8_t);
        case -TYPE_GUID:
            return ISIZEOF(i8_t) + ISIZEOF(guid_t);
        case TYPE_GUID:
            return ISIZEOF(i8_t) + 1 + ISIZEOF(u32_t) + obj->len * ISIZEOF(guid_t);
        case TYPE_B8:
            return ISIZEOF(i8_t) + 1 + ISIZEOF(u32_t) + obj->len * ISIZEOF(i8_t);
        case TYPE_U8:
            return ISIZEOF(i8_t) + 1 + ISIZEOF(u32_t) + obj->len * ISIZEOF(u8_t);
        case TYPE_I16:
            return ISIZEOF(i8_t) + 1 + ISIZEOF(u32_t) + obj->len * ISIZEOF(i16_t);
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            return ISIZEOF(i8_t) + 1 + ISIZEOF(u32_t) + obj->len * ISIZEOF(i32_t);
        case TYPE_I64:
        case TYPE_TIMESTAMP:
            return ISIZEOF(i8_t) + 1 + ISIZEOF(u32_t) + obj->len * ISIZEOF(i64_t);
        case TYPE_F64:
            return ISIZEOF(i8_t) + 1 + ISIZEOF(u32_t) + obj->len * ISIZEOF(f64_t);
        case TYPE_C8:
            return ISIZEOF(i8_t) + 1 + ISIZEOF(u32_t) + obj->len * ISIZEOF(c8_t);
        case TYPE_SYMBOL:
            l = obj->len;
            size = ISIZEOF(i8_t) + 1 + ISIZEOF(u32_t);
            for (i = 0; i < l; i++)
                size += SYMBOL_STRLEN(AS_SYMBOL(obj)[i]) + 1;
            return size;
        case TYPE_LIST:
            l = obj->len;
            size = ISIZEOF(i8_t) + 1 + ISIZEOF(u32_t);
            for (i = 0; i < l; i++)
                size += raykx_size_obj(AS_LIST(obj)[i]);
            return size;
        case TYPE_TABLE:
        case TYPE_DICT:
            return ISIZEOF(i8_t) + raykx_size_obj(AS_LIST(obj)[0]) + raykx_size_obj(AS_LIST(obj)[1]);
        case TYPE_ERR:
            l = AS_ERROR(obj)->msg->len;
            //If there isn't a null terminator on the original message add an extra byte to add one
            if (l == 0 || AS_C8(AS_ERROR(obj)->msg)[l - 1] != '\0')
                l++;
            return ISIZEOF(i8_t) + l;
        case TYPE_NULL:
            return ISIZEOF(i8_t) + 1 + sizeof(i32_t);
        // // case TYPE_LAMBDA:
        // //     return ISIZEOF(i8_t) + size_obj(AS_LAMBDA(obj)->args) + size_obj(AS_LAMBDA(obj)->body);
        // case TYPE_UNARY:
        // case TYPE_BINARY:
        // case TYPE_VARY:
        //     return ISIZEOF(i8_t) + strlen(env_get_internal_name(obj)) + 1;
        // case TYPE_NULL:
        //     return ISIZEOF(i8_t);
        // case TYPE_ERR:
        //     return ISIZEOF(i8_t) + ISIZEOF(i8_t) + size_obj(AS_ERROR(obj)->msg);
        default:
            return 0;
    }
}

// Table mapping RayKX types to KDB+ types
static const i8_t raykx_type_to_k_table[128] = {
    [TYPE_TIMESTAMP] = KP,  // Timestamp
    [TYPE_I64] = KJ,        // Long
    [TYPE_F64] = KF,        // Float
    [TYPE_I32] = KI,        // Int
    [TYPE_I16] = KH,        // Short
    [TYPE_U8] = KG,         // Byte
    [TYPE_B8] = KB,         // Boolean
    [TYPE_C8] = KC,         // Char
    [TYPE_SYMBOL] = KS,     // Symbol
    [TYPE_GUID] = UU,       // GUID
    [TYPE_DATE] = KD,       // Date
    [TYPE_TIME] = KT,       // Time
    [TYPE_LIST] = 0,        // List
    [TYPE_TABLE] = XT,      // Table
    [TYPE_DICT] = XD,       // Dictionary
    [TYPE_NULL] = 0,        // Null
    [TYPE_ERR] = -128,      // Error
};

#define RAYKX_TYPE_TO_K(t) (raykx_type_to_k_table[(t < 0 ? -t : t)] * (t < 0 ? -1 : 1))

#define RAYKX_SER_ATOM(b, o, t)          \
    ({                                   \
        memcpy(b, &o->t, sizeof(t##_t)); \
        ISIZEOF(i8_t) + ISIZEOF(t##_t);  \
    })

#define RAYKX_SER_VEC(b, o, t)                 \
    ({                                         \
        b[0] = 0;                              \
        b++;                                   \
        l = (i32_t)obj->len;                   \
        memcpy(b, &l, sizeof(i32_t));          \
        b += sizeof(i32_t);                    \
        n = obj->len * ISIZEOF(t##_t);         \
        memcpy(b, o->raw, n);                  \
        ISIZEOF(i8_t) + 1 + sizeof(i32_t) + n; \
    })

#define RAYKX_DES_ATOM(b, l, t, r)        \
    ({                                    \
        obj_p $o = t(0);                  \
        memcpy(&$o->t, b, sizeof(t##_t)); \
        b += sizeof(t##_t);               \
        (*l) -= sizeof(t##_t);            \
        $o->type = -TYPE_##r;             \
        $o;                               \
    })

#define RAYKX_DES_VEC(b, l, t, r)         \
    ({                                    \
        i32_t $n;                         \
        i64_t $m;                         \
        obj_p $o;                         \
        b++;                              \
        memcpy(&$n, b, sizeof(i32_t));    \
        b += sizeof(i32_t);               \
        $o = r($n);                       \
        $m = $n * ISIZEOF(t##_t);         \
        memcpy($o->raw, b, $m);           \
        b += $m;                          \
        (*l) -= ($m + sizeof(i32_t) + 1); \
        $o->type = TYPE_##r;              \
        $o;                               \
    })

i64_t raykx_ser_obj(u8_t *buf, obj_p obj) {
    i32_t i, n, l;
    u8_t *b;
    str_p str;

    buf[0] = RAYKX_TYPE_TO_K(obj->type);
    buf++;
    b = buf;

    switch (obj->type) {
        case -TYPE_B8:
            return RAYKX_SER_ATOM(b, obj, u8);
        case -TYPE_U8:
            return RAYKX_SER_ATOM(b, obj, u8);
        case -TYPE_C8:
            return RAYKX_SER_ATOM(b, obj, c8);
        case -TYPE_I16:
            return RAYKX_SER_ATOM(b, obj, i16);
        case -TYPE_I32:
        case -TYPE_DATE:
        case -TYPE_TIME:
            return RAYKX_SER_ATOM(b, obj, i32);
        case -TYPE_I64:
        case -TYPE_TIMESTAMP:
            return RAYKX_SER_ATOM(b, obj, i64);
        case -TYPE_F64:
            return RAYKX_SER_ATOM(b, obj, f64);
        case -TYPE_SYMBOL:
            str = str_from_symbol(obj->i64);
            l = SYMBOL_STRLEN(obj->i64) + 1;
            memcpy(buf, str, l);
            return ISIZEOF(i8_t) + l;
        case -TYPE_GUID:
            memcpy(buf, obj->raw, sizeof(guid_t));
            return ISIZEOF(i8_t) + ISIZEOF(guid_t);
        case TYPE_C8:
        case TYPE_B8:
        case TYPE_U8:
            return RAYKX_SER_VEC(buf, obj, c8);
        case TYPE_I16:
            return RAYKX_SER_VEC(buf, obj, i16);
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            return RAYKX_SER_VEC(buf, obj, i32);
        case TYPE_I64:
        case TYPE_TIMESTAMP:
            return RAYKX_SER_VEC(buf, obj, i64);
        case TYPE_F64:
            return RAYKX_SER_VEC(buf, obj, f64);
        case TYPE_SYMBOL:
            buf[0] = 0;  // attrs
            buf++;
            l = (i32_t)obj->len;
            memcpy(buf, &l, sizeof(i32_t));
            buf += sizeof(i32_t);
            b = buf;
            for (i = 0, n = 0; i < obj->len; i++) {
                str = str_from_symbol(AS_SYMBOL(obj)[i]);
                n = SYMBOL_STRLEN(AS_SYMBOL(obj)[i]) + 1;
                memcpy(b, str, n);
                b += n;
            }
            return ISIZEOF(i8_t) + 1 + sizeof(i32_t) + (b - buf);
        case TYPE_GUID:
            buf[0] = 0;  // attrs
            buf++;
            l = (i32_t)obj->len;
            memcpy(buf, &l, sizeof(i32_t));
            buf += sizeof(i32_t);
            n = obj->len * ISIZEOF(guid_t);
            memcpy(buf, obj->raw, n);
            return ISIZEOF(i8_t) + 1 + sizeof(i32_t) + n;
        case TYPE_NULL:
            buf[0] = 0;  // attrs
            buf++;
            memset(buf, 0, sizeof(i32_t));
            return ISIZEOF(i8_t) + 1 + ISIZEOF(i32_t);
        case TYPE_LIST:
            buf[0] = 0;  // attrs
            buf++;
            l = (i32_t)obj->len;
            memcpy(buf, &l, sizeof(i32_t));
            buf += sizeof(i32_t);
            b = buf;
            for (i = 0; i < obj->len; i++)
                b += raykx_ser_obj(b, AS_LIST(obj)[i]);
            return ISIZEOF(i8_t) + 1 + sizeof(i32_t) + (b - buf);
        case TYPE_TABLE:
        case TYPE_DICT:
            b += raykx_ser_obj(b, AS_LIST(obj)[0]);
            b += raykx_ser_obj(b, AS_LIST(obj)[1]);
            return ISIZEOF(i8_t) + (b - buf);
        case TYPE_ERR:
            l = AS_ERROR(obj)->msg->len;
            if (l == 0) {
                buf[0] = 0;
                return ISIZEOF(i8_t) + 1;
            }

            memcpy(buf, AS_C8(AS_ERROR(obj)->msg), l);
            buf += l;

            //Add an null terminator if there isn't one there already
            if (AS_C8(AS_ERROR(obj)->msg)[l - 1] != '\0'){
                buf[0] = '\0';
                l++;
                buf++;
            }

            return ISIZEOF(i8_t) + l;
        default:
            return 0;
    }
}

obj_p raykx_des_obj(u8_t *buf, i64_t *len) {
    i64_t id, i, l, n;
    obj_p k, v, obj;
    i8_t type;

    if (*len == 0)
        return NULL_OBJ;

    type = *buf;
    buf++;
    (*len)--;

    switch (type) {
        case -KB:
            return RAYKX_DES_ATOM(buf, len, u8, U8);
        case -KC:
            return RAYKX_DES_ATOM(buf, len, c8, C8);
        case -KG:
            return RAYKX_DES_ATOM(buf, len, u8, U8);
        case -KH:
            return RAYKX_DES_ATOM(buf, len, i16, I16);
        case -KI:
            return RAYKX_DES_ATOM(buf, len, i32, I32);
        case -KJ:
            return RAYKX_DES_ATOM(buf, len, i64, I64);
        case -KZ:
            return NULL_OBJ;
        case -KS:
            l = strlen((lit_p)buf);
            obj = symbol((lit_p)buf, l);
            buf += l + 1;
            (*len) -= l + 1;
            return obj;
        case -KF:
            return RAYKX_DES_ATOM(buf, len, f64, F64);
        case KC:
            return RAYKX_DES_VEC(buf, len, c8, C8);
        case KG:
            return RAYKX_DES_VEC(buf, len, u8, U8);
        case KH:
            return RAYKX_DES_VEC(buf, len, i16, I16);
        case KI:
            return RAYKX_DES_VEC(buf, len, i32, I32);
        case KT:
            v = RAYKX_DES_VEC(buf, len, i32, I32);
            v->type = TYPE_TIME;
            return v;
        case KD:
            v = RAYKX_DES_VEC(buf, len, i32, I32);
            v->type = TYPE_DATE;
            return v;
        case KZ:
            v = RAYKX_DES_VEC(buf, len, i64, I64);
            v->type = TYPE_TIMESTAMP;
            return v;
        case KN:
            v = RAYKX_DES_VEC(buf, len, i64, I64);
            v->type = TYPE_TIMESTAMP;
            return v;
        case KJ:
            return RAYKX_DES_VEC(buf, len, i64, I64);
        case KF:
            return RAYKX_DES_VEC(buf, len, f64, F64);
        case UU:
            return RAYKX_DES_VEC(buf, len, guid, GUID);
        case KS:
            buf++;  // attrs
            (*len)--;
            memcpy(&l, buf, sizeof(i32_t));
            buf += sizeof(i32_t);
            (*len) -= sizeof(i32_t);
            obj = SYMBOL(l);
            for (i = 0; i < l; i++) {
                n = strlen((lit_p)buf);
                id = symbols_intern((lit_p)buf, n);
                AS_SYMBOL(obj)[i] = id;
                buf += n + 1;
                (*len) -= n + 1;
            }
            return obj;
        case XD:
            l = *len;
            k = raykx_des_obj(buf, len);
            if (IS_ERR(k))
                return k;
            buf += l - *len;
            v = raykx_des_obj(buf, len);
            if (IS_ERR(v))
                return v;
            obj = table(k, v);
            obj->type = TYPE_DICT;
            return obj;
        case XT:
            buf++;  // attrs
            buf++;  // dict
            (*len) -= 2;
            l = *len;
            k = raykx_des_obj(buf, len);
            if (IS_ERR(k))
                return k;
            buf += l - *len;
            v = raykx_des_obj(buf, len);
            if (IS_ERR(v))
                return v;
            return table(k, v);
        case 0:     // LIST
            buf++;  // attrs
            (*len)--;
            memcpy(&l, buf, sizeof(i32_t));
            buf += sizeof(i32_t);
            (*len) -= sizeof(i32_t);
            obj = LIST(l);
            for (i = 0; i < l; i++) {
                i64_t start_len = *len;
                k = raykx_des_obj(buf, len);
                if (IS_ERR(k)) {
                    obj->len = i;
                    drop_obj(obj);
                    return k;
                }
                AS_LIST(obj)[i] = k;
                buf += (start_len - *len);  // Update buffer position based on consumed bytes
            }
            return obj;
        case -128:  // ERROR
            return error_str(ERR_IO, (lit_p)buf);
        default:
            return NULL_OBJ;
    }
}
