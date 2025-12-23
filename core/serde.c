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

#include "serde.h"
#include "symbols.h"
#include "string.h"
#include "lambda.h"
#include "env.h"
#include "error.h"

i64_t size_of_type(i8_t type) {
    switch (type) {
        case TYPE_B8:
            return ISIZEOF(b8_t);
        case TYPE_U8:
            return ISIZEOF(u8_t);
        case TYPE_I16:
            return ISIZEOF(i16_t);
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            return ISIZEOF(i32_t);
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
            return ISIZEOF(i64_t);
        case TYPE_F64:
            return ISIZEOF(f64_t);
        case TYPE_GUID:
            return ISIZEOF(guid_t);
        case TYPE_C8:
            return ISIZEOF(c8_t);
        case TYPE_LIST:
        case TYPE_NULL:
            return ISIZEOF(obj_p);
        default:
            PANIC("sizeof: unknown type: %d", type);
    }
}

i64_t size_of(obj_p obj) {
    i64_t size = ISIZEOF(struct obj_t);

    if (IS_ATOM(obj))
        return size;

    if (IS_VECTOR(obj)) {
        size += obj->len * size_of_type(obj->type);
        return size;
    }

    switch (obj->type) {
        case TYPE_ENUM:
            size += obj->len * ISIZEOF(i64_t);
            return size;
        case TYPE_MAPLIST:
            size += obj->len * ISIZEOF(i64_t);
            return size;
        case TYPE_NULL:
            return 0;
        default:
            PANIC("sizeof: unknown type: %d", obj->type);
    }
}

/*
 * Returns size (in bytes) that an obj occupy in memory via serialization
 */
i64_t size_obj(obj_p obj) {
    i64_t i, l, size;

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
            return ISIZEOF(i8_t) + 1 + ISIZEOF(i64_t) + obj->len * ISIZEOF(guid_t);
        case TYPE_B8:
            return ISIZEOF(i8_t) + 1 + ISIZEOF(i64_t) + obj->len * ISIZEOF(b8_t);
        case TYPE_U8:
            return ISIZEOF(i8_t) + 1 + ISIZEOF(i64_t) + obj->len * ISIZEOF(u8_t);
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            return ISIZEOF(i8_t) + 1 + ISIZEOF(i64_t) + obj->len * ISIZEOF(i32_t);
        case TYPE_I64:
        case TYPE_TIMESTAMP:
            return ISIZEOF(i8_t) + 1 + ISIZEOF(i64_t) + obj->len * ISIZEOF(i64_t);
        case TYPE_F64:
            return ISIZEOF(i8_t) + 1 + ISIZEOF(i64_t) + obj->len * ISIZEOF(f64_t);
        case TYPE_C8:
            return ISIZEOF(i8_t) + 1 + ISIZEOF(i64_t) + obj->len * ISIZEOF(c8_t);
        case TYPE_SYMBOL:
            l = obj->len;
            size = ISIZEOF(i8_t) + 1 + ISIZEOF(i64_t);
            for (i = 0; i < l; i++)
                size += SYMBOL_STRLEN(AS_SYMBOL(obj)[i]) + 1;
            return size;
        case TYPE_LIST:
            l = obj->len;
            size = ISIZEOF(i8_t) + 1 + ISIZEOF(i64_t);
            for (i = 0; i < l; i++)
                size += size_obj(AS_LIST(obj)[i]);
            return size;
        case TYPE_TABLE:
        case TYPE_DICT:
            return ISIZEOF(i8_t) + 1 + size_obj(AS_LIST(obj)[0]) + size_obj(AS_LIST(obj)[1]);
        case TYPE_LAMBDA:
            return ISIZEOF(i8_t) + 1 + ISIZEOF(i64_t) + size_obj(AS_LAMBDA(obj)->args) + size_obj(AS_LAMBDA(obj)->body);
        case TYPE_UNARY:
        case TYPE_BINARY:
        case TYPE_VARY:
            return ISIZEOF(i8_t) + SYMBOL_STRLEN(env_get_internal_id(obj)) + 1;
        case TYPE_NULL:
            return ISIZEOF(i8_t);
        case TYPE_ERR:
            // Error type (8 bytes) + message length
            return ISIZEOF(i8_t) + 8 + strlen(ray_err_msg(obj)) + 1;
        default:
            return 0;
    }
}

i64_t ser_raw(u8_t *buf, obj_p obj) {
    i64_t i, l, c;
    str_p s;

    buf[0] = obj->type;
    buf++;

    switch (obj->type) {
        case TYPE_NULL:
            return ISIZEOF(i8_t);
        case -TYPE_B8:
            buf[0] = obj->b8;
            return ISIZEOF(i8_t) + ISIZEOF(b8_t);
        case -TYPE_U8:
            buf[0] = obj->u8;
            return ISIZEOF(i8_t) + ISIZEOF(u8_t);
        case -TYPE_I16:
            memcpy(buf, &obj->i16, ISIZEOF(i16_t));
            return ISIZEOF(i8_t) + ISIZEOF(i16_t);
        case -TYPE_I32:
        case -TYPE_DATE:
        case -TYPE_TIME:
            memcpy(buf, &obj->i32, ISIZEOF(i32_t));
            return ISIZEOF(i8_t) + ISIZEOF(i32_t);
        case -TYPE_I64:
        case -TYPE_TIMESTAMP:
            memcpy(buf, &obj->i64, ISIZEOF(i64_t));
            return ISIZEOF(i8_t) + ISIZEOF(i64_t);
        case -TYPE_F64:
            memcpy(buf, &obj->f64, ISIZEOF(f64_t));
            return ISIZEOF(i8_t) + ISIZEOF(f64_t);
        case -TYPE_SYMBOL:
            s = str_from_symbol(obj->i64);
            return ISIZEOF(i8_t) + str_cpy((str_p)buf, s) + 1;
        case -TYPE_C8:
            buf[0] = obj->c8;
            return ISIZEOF(i8_t) + ISIZEOF(c8_t);
        case -TYPE_GUID:
            memcpy(buf, AS_C8(obj), ISIZEOF(guid_t));
            return ISIZEOF(i8_t) + ISIZEOF(guid_t);
        case TYPE_B8:
        case TYPE_U8:
            buf[0] = 0;  // attrs
            buf++;
            l = obj->len;
            memcpy(buf, &l, ISIZEOF(i64_t));
            buf += ISIZEOF(i64_t);
            for (i = 0; i < l; i++)
                buf[i] = AS_U8(obj)[i];

            return ISIZEOF(i8_t) + ISIZEOF(i64_t) + l * ISIZEOF(u8_t) + 1;
        case TYPE_C8:
            buf[0] = 0;  // attrs
            buf++;
            l = ops_count(obj);
            memcpy(buf, &l, ISIZEOF(i64_t));
            buf += ISIZEOF(i64_t);
            memcpy(buf, AS_C8(obj), l);

            return ISIZEOF(i8_t) + ISIZEOF(i64_t) + l * ISIZEOF(c8_t) + 1;
        case TYPE_I16:
            buf[0] = 0;  // attrs
            buf++;
            l = obj->len;
            memcpy(buf, &l, ISIZEOF(i64_t));
            buf += ISIZEOF(i64_t);
            for (i = 0; i < l; i++)
                memcpy(buf + i * ISIZEOF(i16_t), &AS_I16(obj)[i], ISIZEOF(i16_t));

            return ISIZEOF(i8_t) + ISIZEOF(i64_t) + l * ISIZEOF(i16_t) + 1;
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            buf[0] = 0;  // attrs
            buf++;
            l = obj->len;
            memcpy(buf, &l, ISIZEOF(i64_t));
            buf += ISIZEOF(i64_t);
            for (i = 0; i < l; i++)
                memcpy(buf + i * ISIZEOF(i32_t), &AS_I32(obj)[i], ISIZEOF(i32_t));

            return ISIZEOF(i8_t) + ISIZEOF(i64_t) + l * ISIZEOF(i32_t) + 1;
        case TYPE_I64:
        case TYPE_TIMESTAMP:
            buf[0] = 0;  // attrs
            buf++;
            l = obj->len;
            memcpy(buf, &l, ISIZEOF(i64_t));
            buf += ISIZEOF(i64_t);
            for (i = 0; i < l; i++)
                memcpy(buf + i * ISIZEOF(i64_t), &AS_I64(obj)[i], ISIZEOF(i64_t));

            return ISIZEOF(i8_t) + ISIZEOF(i64_t) + l * ISIZEOF(i64_t) + 1;
        case TYPE_F64:
            buf[0] = 0;  // attrs
            buf++;
            l = obj->len;
            memcpy(buf, &l, ISIZEOF(i64_t));
            buf += ISIZEOF(i64_t);
            for (i = 0; i < l; i++)
                memcpy(buf + i * ISIZEOF(f64_t), &AS_F64(obj)[i], ISIZEOF(f64_t));

            return ISIZEOF(i8_t) + ISIZEOF(i64_t) + l * ISIZEOF(f64_t) + 1;
        case TYPE_SYMBOL:
            buf[0] = 0;  // attrs
            buf++;
            l = obj->len;
            memcpy(buf, &l, ISIZEOF(i64_t));
            buf += ISIZEOF(i64_t);
            for (i = 0, c = 0; i < l; i++) {
                s = str_from_symbol(AS_SYMBOL(obj)[i]);
                c += str_cpy((str_p)buf + c, s);
                buf[c] = '\0';
                c++;
            }

            return ISIZEOF(i8_t) + ISIZEOF(i64_t) + c + 1;
        case TYPE_GUID:
            buf[0] = 0;  // attrs
            buf++;
            l = obj->len;
            memcpy(buf, &l, ISIZEOF(i64_t));
            buf += ISIZEOF(i64_t);
            memcpy(buf, AS_C8(obj), l * ISIZEOF(guid_t));
            return ISIZEOF(i8_t) + ISIZEOF(i64_t) + l * ISIZEOF(guid_t) + 1;
        case TYPE_LIST:
            buf[0] = 0;  // attrs
            buf++;
            l = obj->len;
            memcpy(buf, &l, ISIZEOF(i64_t));
            buf += ISIZEOF(i64_t);
            for (i = 0, c = 0; i < l; i++)
                c += ser_raw(buf + c, AS_LIST(obj)[i]);

            return ISIZEOF(i8_t) + ISIZEOF(i64_t) + c + 1;
        case TYPE_TABLE:
        case TYPE_DICT:
            buf[0] = 0;  // attrs
            buf++;
            c = ser_raw(buf, AS_LIST(obj)[0]);
            c += ser_raw(buf + c, AS_LIST(obj)[1]);
            return ISIZEOF(i8_t) + c + 1;
        case TYPE_LAMBDA:
            buf[0] = 0;  // attrs
            buf++;
            c = ser_raw(buf, AS_LAMBDA(obj)->args);
            c += ser_raw(buf + c, AS_LAMBDA(obj)->body);
            return ISIZEOF(i8_t) + c + 1;
        case TYPE_UNARY:
        case TYPE_BINARY:
        case TYPE_VARY:
            c = str_cpy((str_p)buf, env_get_internal_name(obj));
            return ISIZEOF(i8_t) + c + 1;
        case TYPE_ERR: {
            lit_p err_type = ray_err_msg(obj);
            lit_p err_msg = ray_err_msg(obj);
            i64_t msg_len = strlen(err_msg);
            // Serialize: type (8 bytes, null-padded) + message (null-terminated)
            memset(buf, 0, 8);
            memcpy(buf, err_type, strlen(err_type));
            c = 8;
            memcpy(buf + c, err_msg, msg_len + 1);
            c += msg_len + 1;
            return ISIZEOF(i8_t) + c;
        }
        default:
            return 0;
    }
}

obj_p ser_obj(obj_p obj) {
    i64_t size = size_obj(obj);
    obj_p buf;
    ipc_header_t *header;

    if (size == 0)
        return ray_err(ERR_TYPE);

    buf = vector(TYPE_U8, ISIZEOF(struct ipc_header_t) + size);
    header = (ipc_header_t *)AS_U8(buf);

    header->prefix = SERDE_PREFIX;
    header->version = RAYFORCE_VERSION;
    header->flags = 0;
    header->endian = 0;
    header->msgtype = 0;
    header->size = size;

    if (ser_raw(AS_U8(buf) + ISIZEOF(struct ipc_header_t), obj) == 0) {
        drop_obj(buf);
        return ray_err(ERR_TYPE);
    }

    return buf;
}

obj_p de_raw(u8_t *buf, i64_t *len) {
    i64_t i, l, c, id;
    obj_p obj, k, v;
    i8_t type;

    if (*len == 0)
        return NULL_OBJ;

    type = buf[0];
    buf++;
    (*len)--;

    switch (type) {
        case TYPE_NULL:
            return NULL_OBJ;
        case -TYPE_B8:
            if (*len < 1)
                return ray_err(ERR_UFLOW);
            obj = b8(buf[0]);
            buf++;
            (*len)--;
            return obj;
        case -TYPE_U8:
            if (*len < 1)
                return ray_err(ERR_UFLOW);
            obj = u8(buf[0]);
            buf++;
            (*len)--;
            return obj;
        case -TYPE_I16:
            if (*len < ISIZEOF(i16_t))
                return ray_err(ERR_UFLOW);
            obj = i16(0);
            memcpy(&obj->i16, buf, ISIZEOF(i16_t));
            buf += ISIZEOF(i16_t);
            (*len) -= ISIZEOF(i16_t);
            return obj;
        case -TYPE_I32:
        case -TYPE_DATE:
        case -TYPE_TIME:
            if (*len < ISIZEOF(i32_t))
                return ray_err(ERR_UFLOW);
            obj = i32(0);
            memcpy(&obj->i32, buf, ISIZEOF(i32_t));
            buf += ISIZEOF(i32_t);
            (*len) -= ISIZEOF(i32_t);
            obj->type = type;
            return obj;
        case -TYPE_I64:
        case -TYPE_TIMESTAMP:
            if (*len < ISIZEOF(i64_t))
                return ray_err(ERR_UFLOW);
            obj = i64(0);
            memcpy(&obj->i64, buf, ISIZEOF(i64_t));
            buf += ISIZEOF(i64_t);
            (*len) -= ISIZEOF(i64_t);
            obj->type = type;
            return obj;
        case -TYPE_F64:
            if (*len < ISIZEOF(f64_t))
                return ray_err(ERR_UFLOW);
            obj = f64(0);
            memcpy(&obj->f64, buf, ISIZEOF(f64_t));
            buf += ISIZEOF(f64_t);
            (*len) -= ISIZEOF(f64_t);
            return obj;
        case -TYPE_SYMBOL:
            if (*len < 1)
                return ray_err(ERR_UFLOW);
            l = str_len((str_p)buf, *len);
            if (l >= *len)
                return ray_err(ERR_BAD);
            i = symbols_intern((str_p)buf, l);
            obj = symboli64(i);
            buf += l + 1;
            (*len) -= l + 1;
            return obj;
        case -TYPE_C8:
            if (*len < 1)
                return ray_err(ERR_UFLOW);
            obj = c8(buf[0]);
            buf++;
            (*len)--;
            return obj;
        case -TYPE_GUID:
            if (*len < ISIZEOF(guid_t))
                return ray_err(ERR_UFLOW);
            obj = guid(buf);
            buf += ISIZEOF(guid_t);
            (*len) -= ISIZEOF(guid_t);
            return obj;
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_C8:
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
        case TYPE_I64:
        case TYPE_TIMESTAMP:
        case TYPE_F64:
        case TYPE_SYMBOL:
        case TYPE_GUID:
        case TYPE_LIST:
            if (*len < ISIZEOF(i64_t))
                return ray_err(ERR_UFLOW);

            buf++;  // skip attrs
            memcpy(&l, buf, ISIZEOF(i64_t));
            buf += ISIZEOF(i64_t);
            (*len) -= ISIZEOF(i64_t) + 1;

            // Check for unreasonable length values that might indicate corruption
            if (l > 1000000000)  // 1 billion elements is likely a corrupted value
                return ray_err(ERR_BAD);

            // Continue with type-specific handling
            switch (type) {
                case TYPE_B8:
                case TYPE_U8:
                    if (*len < l * ISIZEOF(u8_t))
                        return ray_err(ERR_UFLOW);
                    obj = U8(l);
                    obj->type = type;
                    if (IS_ERR(obj))
                        return obj;
                    memcpy(AS_U8(obj), buf, l * ISIZEOF(u8_t));
                    buf += l * ISIZEOF(u8_t);
                    (*len) -= l * ISIZEOF(u8_t);
                    return obj;
                case TYPE_C8:
                    if (*len < l * ISIZEOF(c8_t))
                        return ray_err(ERR_UFLOW);
                    obj = C8(l);
                    if (IS_ERR(obj))
                        return obj;
                    memcpy(AS_C8(obj), buf, l * ISIZEOF(c8_t));
                    buf += l * ISIZEOF(c8_t);
                    (*len) -= l * ISIZEOF(c8_t);
                    return obj;
                case TYPE_I32:
                case TYPE_TIME:
                case TYPE_DATE:
                    if (*len < l * ISIZEOF(i32_t))
                        return ray_err(ERR_UFLOW);
                    obj = I32(l);
                    if (IS_ERR(obj))
                        return obj;
                    memcpy(AS_I32(obj), buf, l * ISIZEOF(i32_t));
                    buf += l * ISIZEOF(i32_t);
                    (*len) -= l * ISIZEOF(i32_t);
                    obj->type = type;
                    return obj;
                case TYPE_I64:
                case TYPE_TIMESTAMP:
                    if (*len < l * ISIZEOF(i64_t))
                        return ray_err(ERR_UFLOW);
                    obj = I64(l);
                    if (IS_ERR(obj))
                        return obj;
                    memcpy(AS_I64(obj), buf, l * ISIZEOF(i64_t));
                    buf += l * ISIZEOF(i64_t);
                    (*len) -= l * ISIZEOF(i64_t);
                    obj->type = type;
                    return obj;
                case TYPE_F64:
                    if (*len < l * ISIZEOF(f64_t))
                        return ray_err(ERR_UFLOW);
                    obj = F64(l);
                    if (IS_ERR(obj))
                        return obj;
                    memcpy(AS_F64(obj), buf, l * ISIZEOF(f64_t));
                    buf += l * ISIZEOF(f64_t);
                    (*len) -= l * ISIZEOF(f64_t);
                    return obj;
                case TYPE_SYMBOL:
                    obj = SYMBOL(l);
                    if (IS_ERR(obj))
                        return obj;
                    for (i = 0; i < l; i++) {
                        if (*len < 1) {
                            obj->len = i;
                            drop_obj(obj);
                            return ray_err(ERR_UFLOW);
                        }
                        c = str_len((str_p)buf, *len);
                        if (c >= *len) {
                            obj->len = i;
                            drop_obj(obj);
                            return ray_err(ERR_BAD);
                        }
                        id = symbols_intern((str_p)buf, c);
                        AS_SYMBOL(obj)[i] = id;
                        buf += c + 1;
                        (*len) -= c + 1;
                    }
                    return obj;
                case TYPE_GUID:
                    if (*len < l * ISIZEOF(guid_t))
                        return ray_err(ERR_UFLOW);
                    obj = GUID(l);
                    if (IS_ERR(obj))
                        return obj;
                    memcpy(AS_C8(obj), buf, l * ISIZEOF(guid_t));
                    buf += l * ISIZEOF(guid_t);
                    (*len) -= l * ISIZEOF(guid_t);
                    return obj;
                case TYPE_LIST:
                    obj = LIST(l);
                    if (IS_ERR(obj))
                        return obj;
                    c = *len;
                    for (i = 0; i < l; i++) {
                        v = de_raw(buf + c - *len, len);
                        if (IS_ERR(v)) {
                            obj->len = i;
                            drop_obj(obj);
                            return v;
                        }

                        AS_LIST(obj)[i] = v;
                    }
                    return obj;
            }
            // Should never reach here
            return ray_err(ERR_BAD);

        case TYPE_TABLE:
        case TYPE_DICT:
            buf++;  // skip attrs
            (*len) -= 1;
            c = *len;
            k = de_raw(buf, len);

            if (IS_ERR(k))
                return k;

            v = de_raw(buf + c - *len, len);

            if (IS_ERR(v)) {
                drop_obj(k);
                return v;
            }

            if (type == TYPE_TABLE)
                return table(k, v);
            else
                return dict(k, v);

        case TYPE_LAMBDA:
            buf++;  // skip attrs
            (*len) -= 1;
            c = *len;
            k = de_raw(buf, len);

            if (IS_ERR(k))
                return k;

            v = de_raw(buf + c - *len, len);

            if (IS_ERR(v)) {
                drop_obj(k);
                return v;
            }

            return lambda(k, v, NULL_OBJ);

        case TYPE_UNARY:
        case TYPE_BINARY:
        case TYPE_VARY:
            if (*len < 1)
                return ray_err(ERR_UFLOW);
            // Check for null terminator within buffer
            for (i = 0; i < *len; i++) {
                if (buf[i] == 0)
                    break;
            }
            if (i >= *len)
                return ray_err(ERR_BAD);

            k = env_get_internal_function((str_p)buf);
            buf += i + 1;
            (*len) -= i + 1;

            return k;

        case TYPE_ERR: {
            lit_p err_msg;
            if (*len < 9)  // At least 8 bytes for type + 1 for message
                return ray_err(ERR_UFLOW);
            // Skip 8 bytes of type (for legacy format compatibility)
            buf += 8;
            (*len) -= 8;
            err_msg = (lit_p)buf;
            i64_t msg_len = strlen(err_msg);
            buf += msg_len + 1;
            (*len) -= msg_len + 1;
            return ray_err(err_msg);
        }

        default:
            return ray_err(ERR_TYPE);
    }
}

obj_p de_obj(obj_p obj) {
    i64_t len;
    u8_t *buf;
    ipc_header_t *header;

    len = obj->len;
    buf = AS_U8(obj);

    // Check if buffer is large enough to contain a header
    if (len < ISIZEOF(struct ipc_header_t))
        return ray_err(ERR_BAD);

    header = (ipc_header_t *)buf;

    // Check for valid header prefix
    if (header->prefix != SERDE_PREFIX)
        return ray_err(ERR_BAD);

    if (header->version > RAYFORCE_VERSION)
        return ray_err(ERR_TYPE);

    // Check for reasonable size values
    if (header->size > 1000000000)  // 1GB max size
        return ray_err(ERR_BAD);

    if (header->size + ISIZEOF(struct ipc_header_t) != len)
        return ray_err(ERR_BAD);

    len = header->size;
    buf += ISIZEOF(struct ipc_header_t);

    return de_raw(buf, &len);
}
