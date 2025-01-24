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
#include "util.h"
#include "format.h"
#include "symbols.h"
#include "string.h"
#include "heap.h"
#include "lambda.h"
#include "env.h"
#include "eval.h"
#include "error.h"
#include "sys.h"

u64_t size_of_type(i8_t type) {
    switch (type) {
        case TYPE_B8:
            return sizeof(b8_t);
        case TYPE_U8:
            return sizeof(u8_t);
        case TYPE_I16:
            return sizeof(i16_t);
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            return sizeof(i32_t);
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
            return sizeof(i64_t);
        case TYPE_F64:
            return sizeof(f64_t);
        case TYPE_GUID:
            return sizeof(guid_t);
        case TYPE_C8:
            return sizeof(c8_t);
        case TYPE_LIST:
        case TYPE_NULL:
            return sizeof(obj_p);
        default:
            PANIC("sizeof: unknown type: %d", type);
    }
}

u64_t size_of(obj_p obj) {
    u64_t size = sizeof(struct obj_t);

    if (IS_ATOM(obj))
        return size;

    if (IS_VECTOR(obj)) {
        size += obj->len * size_of_type(obj->type);
        return size;
    }

    switch (obj->type) {
        case TYPE_ENUM:
            size += obj->len * sizeof(i64_t);
            return size;
        case TYPE_MAPLIST:
            size += obj->len * sizeof(i64_t);
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
u64_t size_obj(obj_p obj) {
    u64_t i, l, size;

    switch (obj->type) {
        case -TYPE_B8:
            return sizeof(i8_t) + sizeof(b8_t);
        case -TYPE_U8:
            return sizeof(i8_t) + sizeof(u8_t);
        case -TYPE_I16:
            return sizeof(i8_t) + sizeof(i16_t);
        case -TYPE_I32:
        case -TYPE_DATE:
        case -TYPE_TIME:
            return sizeof(i8_t) + sizeof(i32_t);
        case -TYPE_I64:
        case -TYPE_TIMESTAMP:
            return sizeof(i8_t) + sizeof(i64_t);
        case -TYPE_F64:
            return sizeof(i8_t) + sizeof(f64_t);
        case -TYPE_SYMBOL:
            return sizeof(i8_t) + strlen(str_from_symbol(obj->i64)) + 1;
        case -TYPE_C8:
            return sizeof(i8_t) + sizeof(c8_t);
        case -TYPE_GUID:
            return sizeof(i8_t) + sizeof(guid_t);
        case TYPE_GUID:
            return sizeof(i8_t) + sizeof(u64_t) + obj->len * sizeof(guid_t);
        case TYPE_B8:
            return sizeof(i8_t) + sizeof(u64_t) + obj->len * sizeof(b8_t);
        case TYPE_U8:
            return sizeof(i8_t) + sizeof(u64_t) + obj->len * sizeof(u8_t);
        case TYPE_I64:
        case TYPE_TIMESTAMP:
            return sizeof(i8_t) + sizeof(u64_t) + obj->len * sizeof(i64_t);
        case TYPE_F64:
            return sizeof(i8_t) + sizeof(u64_t) + obj->len * sizeof(f64_t);
        case TYPE_C8:
            return sizeof(i8_t) + sizeof(u64_t) + obj->len * sizeof(c8_t);
        case TYPE_SYMBOL:
            l = obj->len;
            size = sizeof(i8_t) + sizeof(u64_t);
            for (i = 0; i < l; i++)
                size += strlen(str_from_symbol(AS_SYMBOL(obj)[i])) + 1;
            return size;
        case TYPE_LIST:
            l = obj->len;
            size = sizeof(i8_t) + sizeof(u64_t);
            for (i = 0; i < l; i++)
                size += size_obj(AS_LIST(obj)[i]);
            return size;
        case TYPE_TABLE:
        case TYPE_DICT:
            return sizeof(i8_t) + size_obj(AS_LIST(obj)[0]) + size_obj(AS_LIST(obj)[1]);
        case TYPE_LAMBDA:
            return sizeof(i8_t) + size_obj(AS_LAMBDA(obj)->args) + size_obj(AS_LAMBDA(obj)->body);
        case TYPE_UNARY:
        case TYPE_BINARY:
        case TYPE_VARY:
            return sizeof(i8_t) + strlen(env_get_internal_name(obj)) + 1;
        case TYPE_NULL:
            return sizeof(i8_t);
        case TYPE_ERROR:
            return sizeof(i8_t) + sizeof(i8_t) + size_obj(AS_ERROR(obj)->msg);
        default:
            return 0;
    }
}

u64_t save_obj(u8_t *buf, u64_t len, obj_p obj) {
    u64_t i, l, c;
    str_p s;

    buf[0] = obj->type;
    buf++;

    switch (obj->type) {
        case TYPE_NULL:
            return sizeof(i8_t);
        case -TYPE_B8:
            buf[0] = obj->b8;
            return sizeof(i8_t) + sizeof(b8_t);

        case -TYPE_U8:
            buf[0] = obj->u8;
            return sizeof(i8_t) + sizeof(u8_t);

        case -TYPE_I16:
            memcpy(buf, &obj->i16, sizeof(i16_t));
            return sizeof(i8_t) + sizeof(i16_t);

        case -TYPE_I32:
        case -TYPE_DATE:
        case -TYPE_TIME:
            memcpy(buf, &obj->i32, sizeof(i32_t));
            return sizeof(i8_t) + sizeof(i32_t);

        case -TYPE_I64:
        case -TYPE_TIMESTAMP:
            memcpy(buf, &obj->i64, sizeof(i64_t));
            return sizeof(i8_t) + sizeof(i64_t);

        case -TYPE_F64:
            memcpy(buf, &obj->f64, sizeof(f64_t));
            return sizeof(i8_t) + sizeof(f64_t);

        case -TYPE_SYMBOL:
            s = str_from_symbol(obj->i64);
            return sizeof(i8_t) + str_cpy((str_p)buf, s) + 1;

        case -TYPE_C8:
            buf[0] = obj->c8;
            return sizeof(i8_t) + sizeof(c8_t);
        case -TYPE_GUID:
            memcpy(buf, AS_C8(obj), sizeof(guid_t));
            return sizeof(i8_t) + sizeof(guid_t);

        case TYPE_B8:
            l = obj->len;
            memcpy(buf, &l, sizeof(u64_t));
            buf += sizeof(u64_t);
            for (i = 0; i < l; i++)
                buf[i] = AS_B8(obj)[i];

            return sizeof(i8_t) + sizeof(u64_t) + l * sizeof(b8_t);

        case TYPE_U8:
            l = obj->len;
            memcpy(buf, &l, sizeof(u64_t));
            buf += sizeof(u64_t);
            for (i = 0; i < l; i++)
                buf[i] = AS_U8(obj)[i];

            return sizeof(i8_t) + sizeof(u64_t) + l * sizeof(u8_t);

        case TYPE_C8:
            l = ops_count(obj);
            memcpy(buf, &l, sizeof(u64_t));
            buf += sizeof(u64_t);
            memcpy(buf, AS_C8(obj), l);

            return sizeof(i8_t) + sizeof(u64_t) + l * sizeof(c8_t);

        case TYPE_I16:
            l = obj->len;
            memcpy(buf, &l, sizeof(u64_t));
            buf += sizeof(u64_t);
            for (i = 0; i < l; i++)
                memcpy(buf + i * sizeof(i16_t), &AS_I16(obj)[i], sizeof(i16_t));

            return sizeof(i8_t) + sizeof(u64_t) + l * sizeof(i16_t);

        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            l = obj->len;
            memcpy(buf, &l, sizeof(u64_t));
            buf += sizeof(u64_t);
            for (i = 0; i < l; i++)
                memcpy(buf + i * sizeof(i32_t), &AS_I32(obj)[i], sizeof(i32_t));

            return sizeof(i8_t) + sizeof(u64_t) + l * sizeof(i32_t);

        case TYPE_I64:
        case TYPE_TIMESTAMP:
            l = obj->len;
            memcpy(buf, &l, sizeof(u64_t));
            buf += sizeof(u64_t);
            for (i = 0; i < l; i++)
                memcpy(buf + i * sizeof(i64_t), &AS_I64(obj)[i], sizeof(i64_t));

            return sizeof(i8_t) + sizeof(u64_t) + l * sizeof(i64_t);

        case TYPE_F64:
            l = obj->len;
            memcpy(buf, &l, sizeof(u64_t));
            buf += sizeof(u64_t);
            for (i = 0; i < l; i++)
                memcpy(buf + i * sizeof(f64_t), &AS_F64(obj)[i], sizeof(f64_t));

            return sizeof(i8_t) + sizeof(u64_t) + l * sizeof(f64_t);

        case TYPE_SYMBOL:
            l = obj->len;
            memcpy(buf, &l, sizeof(u64_t));
            buf += sizeof(u64_t);
            for (i = 0, c = 0; i < l; i++) {
                s = str_from_symbol(AS_SYMBOL(obj)[i]);
                c += str_cpy((str_p)buf + c, s);
                buf[c] = '\0';
                c++;
            }

            return sizeof(i8_t) + sizeof(u64_t) + c;

        case TYPE_GUID:
            l = obj->len;
            memcpy(buf, &l, sizeof(u64_t));
            buf += sizeof(u64_t);
            memcpy(buf, AS_C8(obj), l * sizeof(guid_t));
            return sizeof(i8_t) + sizeof(u64_t) + l * sizeof(guid_t);

        case TYPE_LIST:
            l = obj->len;
            memcpy(buf, &l, sizeof(u64_t));
            buf += sizeof(u64_t);
            for (i = 0, c = 0; i < l; i++)
                c += save_obj(buf + c, len, AS_LIST(obj)[i]);

            return sizeof(i8_t) + sizeof(u64_t) + c;

        case TYPE_TABLE:
        case TYPE_DICT:
            c = save_obj(buf, len, AS_LIST(obj)[0]);
            c += save_obj(buf + c, len, AS_LIST(obj)[1]);
            return sizeof(i8_t) + c;

        case TYPE_LAMBDA:
            c = save_obj(buf, len, AS_LAMBDA(obj)->args);
            c += save_obj(buf + c, len, AS_LAMBDA(obj)->body);
            return sizeof(i8_t) + c;

        case TYPE_UNARY:
        case TYPE_BINARY:
        case TYPE_VARY:
            c = str_cpy((str_p)buf, env_get_internal_name(obj));
            return sizeof(i8_t) + c + 1;

        case TYPE_ERROR:
            buf[0] = (i8_t)AS_ERROR(obj)->code;
            c = sizeof(i8_t);
            c += save_obj(buf + c, len, AS_ERROR(obj)->msg);
            return sizeof(i8_t) + c;

        default:
            return 0;
    }
}

i64_t ser_raw(u8_t **buf, obj_p obj) {
    u64_t size = size_obj(obj);
    header_t *header;

    if (size == 0)
        return -1;

    *buf = (u8_t *)heap_realloc(*buf, sizeof(struct header_t) + size);
    header = (header_t *)*buf;

    header->prefix = SERDE_PREFIX;
    header->version = RAYFORCE_VERSION;
    header->flags = 0;
    header->endian = 0;
    header->msgtype = 0;
    header->size = size;

    if (save_obj(*buf + sizeof(struct header_t), size, obj) == 0) {
        heap_free(*buf);
        *buf = NULL;
        return -1;
    }

    return sizeof(struct header_t) + size;
}

obj_p ser_obj(obj_p obj) {
    u64_t size = size_obj(obj);
    obj_p buf;
    header_t *header;

    if (size == 0)
        THROW(ERR_NOT_SUPPORTED, "ser: unsupported type: %d", obj->type);

    buf = vector(TYPE_U8, sizeof(struct header_t) + size);
    header = (header_t *)AS_U8(buf);

    header->prefix = SERDE_PREFIX;
    header->version = RAYFORCE_VERSION;
    header->flags = 0;
    header->endian = 0;
    header->msgtype = 0;
    header->size = size;

    if (save_obj(AS_U8(buf) + sizeof(struct header_t), size, obj) == 0) {
        drop_obj(buf);
        THROW(ERR_NOT_SUPPORTED, "ser: unsupported type: %d", obj->type);
    }

    return buf;
}

obj_p load_obj(u8_t **buf, u64_t *len) {
    i8_t code;
    u64_t i, l, c, id;
    obj_p obj, k, v;
    i8_t type;

    if (*len == 0)
        return NULL_OBJ;

    type = **buf;
    (*buf)++;
    (*len)--;

    switch (type) {
        case TYPE_NULL:
            return NULL_OBJ;
        case -TYPE_B8:
            obj = b8(**buf);
            (*buf)++;
            return obj;

        case -TYPE_U8:
            obj = u8(**buf);
            (*buf)++;
            (*len)--;
            return obj;

        case -TYPE_I16:
            obj = i16(0);
            memcpy(&obj->i16, *buf, sizeof(i16_t));
            *buf += sizeof(i16_t);
            *len -= sizeof(i16_t);
            return obj;

        case -TYPE_I32:
        case -TYPE_DATE:
        case -TYPE_TIME:
            obj = i32(0);
            memcpy(&obj->i32, *buf, sizeof(i32_t));
            *buf += sizeof(i32_t);
            *len -= sizeof(i32_t);
            return obj;

        case -TYPE_I64:
        case -TYPE_TIMESTAMP:
            obj = i64(0);
            memcpy(&obj->i64, *buf, sizeof(i64_t));
            *buf += sizeof(i64_t);
            *len -= sizeof(i64_t);
            obj->type = type;
            return obj;

        case -TYPE_F64:
            obj = f64(0);
            memcpy(&obj->f64, *buf, sizeof(f64_t));
            *buf += sizeof(f64_t);
            *len -= sizeof(f64_t);
            return obj;

        case -TYPE_SYMBOL:
            l = str_len((str_p)*buf, *len);
            i = symbols_intern((str_p)*buf, l);
            obj = symboli64(i);
            *buf += l + 1;
            *len -= l + 1;
            return obj;

        case -TYPE_C8:
            obj = c8((*buf)[0]);
            (*buf)++;
            (*len)--;
            return obj;

        case -TYPE_GUID:
            obj = guid(*buf);
            buf += sizeof(guid_t);
            *len -= sizeof(guid_t);
            return obj;

        case TYPE_B8:
            memcpy(&l, *buf, sizeof(u64_t));
            *buf += sizeof(u64_t);
            *len -= sizeof(u64_t);
            obj = B8(l);
            memcpy(AS_B8(obj), *buf, l * sizeof(b8_t));
            *buf += l * sizeof(b8_t);
            *len -= l * sizeof(b8_t);
            return obj;

        case TYPE_U8:
            memcpy(&l, *buf, sizeof(u64_t));
            *buf += sizeof(u64_t);
            *len -= sizeof(u64_t);
            obj = U8(l);
            memcpy(AS_U8(obj), *buf, l * sizeof(u8_t));
            *buf += l * sizeof(u8_t);
            *len -= l * sizeof(u8_t);
            return obj;

        case TYPE_C8:
            memcpy(&l, *buf, sizeof(u64_t));
            *buf += sizeof(u64_t);
            *len -= sizeof(u64_t);
            obj = C8(l);
            memcpy(AS_C8(obj), *buf, l * sizeof(c8_t));
            *buf += l * sizeof(c8_t);
            *len -= l * sizeof(c8_t);
            return obj;

        case TYPE_I64:
        case TYPE_TIMESTAMP:
            memcpy(&l, *buf, sizeof(u64_t));
            *buf += sizeof(u64_t);
            *len -= sizeof(u64_t);
            obj = I64(l);
            memcpy(AS_I64(obj), *buf, l * sizeof(i64_t));
            *buf += l * sizeof(i64_t);
            *len -= l * sizeof(i64_t);
            obj->type = type;
            return obj;

        case TYPE_F64:
            memcpy(&l, *buf, sizeof(u64_t));
            *buf += sizeof(u64_t);
            *len -= sizeof(u64_t);
            obj = F64(l);
            memcpy(AS_F64(obj), *buf, l * sizeof(f64_t));
            *buf += l * sizeof(f64_t);
            *len -= l * sizeof(f64_t);
            return obj;

        case TYPE_SYMBOL:
            memcpy(&l, *buf, sizeof(u64_t));
            *buf += sizeof(u64_t);
            *len -= sizeof(u64_t);
            obj = SYMBOL(l);
            for (i = 0; i < l; i++) {
                c = str_len((str_p)*buf, *len);
                id = symbols_intern((str_p)*buf, c);
                AS_SYMBOL(obj)[i] = id;
                *buf += c + 1;
                *len -= c + 1;
            }
            return obj;

        case TYPE_GUID:
            memcpy(&l, *buf, sizeof(u64_t));
            *buf += sizeof(u64_t);
            *len -= sizeof(u64_t);
            obj = GUID(l);
            memcpy(AS_C8(obj), *buf, l * sizeof(guid_t));
            *buf += l * sizeof(guid_t);
            *len -= l * sizeof(guid_t);
            return obj;

        case TYPE_LIST:
            memcpy(&l, *buf, sizeof(u64_t));
            *buf += sizeof(u64_t);
            *len -= sizeof(u64_t);
            obj = LIST(l);
            for (i = 0; i < l; i++) {
                v = load_obj(buf, len);
                if (IS_ERROR(v)) {
                    obj->len = i;
                    drop_obj(obj);
                    return v;
                }

                AS_LIST(obj)[i] = v;
            }
            return obj;

        case TYPE_TABLE:
        case TYPE_DICT:
            k = load_obj(buf, len);

            if (IS_ERROR(k))
                return k;

            v = load_obj(buf, len);

            if (IS_ERROR(v)) {
                drop_obj(k);
                return v;
            }

            if (type == TYPE_TABLE)
                return table(k, v);
            else
                return dict(k, v);

        case TYPE_LAMBDA:
            k = load_obj(buf, len);

            if (IS_ERROR(k))
                return k;

            v = load_obj(buf, len);

            if (IS_ERROR(v)) {
                drop_obj(k);
                return v;
            }

            return lambda(k, v, NULL_OBJ);

        case TYPE_UNARY:
        case TYPE_BINARY:
        case TYPE_VARY:
            k = env_get_internal_function((str_p)*buf);
            *buf += strlen((str_p)*buf) + 1;

            return k;

        case TYPE_ERROR:
            code = **buf;
            (*buf)++;
            (*len)--;
            v = load_obj(buf, len);
            obj = error_obj(code, v);
            return obj;

        default:
            THROW(ERR_NOT_SUPPORTED, "load_obj: unsupported type: %d", type);
    }
}

obj_p de_raw(u8_t *buf, u64_t len) {
    u64_t l;
    header_t *header = (header_t *)buf;

    if (header->version > RAYFORCE_VERSION)
        THROW(ERR_NOT_SUPPORTED, "de: version '%d' is higher than supported", header->version);

    if (header->size + sizeof(struct header_t) != len)
        return error_str(ERR_IO, "de: corrupted data in a buffer");

    buf += sizeof(struct header_t);
    l = header->size;

    return load_obj(&buf, &l);
}

obj_p de_obj(obj_p obj) { return de_raw(AS_U8(obj), obj->len); }
