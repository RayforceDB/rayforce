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

/*
 * Returns size (in bytes) that an obj occupy in memory via serialization
 */
u64_t size_obj(obj_t obj)
{
    u64_t i, l, size;
    switch (obj->type)
    {
    case -TYPE_BOOL:
        return sizeof(type_t) + sizeof(bool_t);
    case -TYPE_BYTE:
        return sizeof(type_t) + sizeof(byte_t);
    case -TYPE_I64:
    case -TYPE_TIMESTAMP:
        return sizeof(type_t) + sizeof(i64_t);
    case -TYPE_F64:
        return sizeof(type_t) + sizeof(f64_t);
    case -TYPE_SYMBOL:
        return sizeof(type_t) + strlen(symtostr(obj->i64)) + 1;
    case -TYPE_CHAR:
        return sizeof(type_t) + sizeof(char_t);
    case TYPE_GUID:
        return sizeof(type_t) + sizeof(u64_t) + obj->len * sizeof(guid_t);
    case TYPE_BOOL:
        return sizeof(type_t) + sizeof(u64_t) + obj->len * sizeof(bool_t);
    case TYPE_BYTE:
        return sizeof(type_t) + sizeof(u64_t) + obj->len * sizeof(byte_t);
    case TYPE_I64:
    case TYPE_TIMESTAMP:
        return sizeof(type_t) + sizeof(u64_t) + obj->len * sizeof(i64_t);
    case TYPE_F64:
        return sizeof(type_t) + sizeof(u64_t) + obj->len * sizeof(f64_t);
    case TYPE_SYMBOL:
        l = obj->len;
        size = sizeof(type_t) + sizeof(u64_t);
        for (i = 0; i < l; i++)
            size += strlen(symtostr(as_symbol(obj)[i])) + 1;
        return size;
    case TYPE_LIST:
        l = obj->len;
        size = sizeof(type_t);
        for (i = 0; i < l; i++)
            size += size_obj(as_list(obj)[i]);
        return size;
    default:
        panic(str_fmt(0, "size_obj: unsupported type: %d", obj->type));
    }
}

u64_t save_obj(byte_t *buf, u64_t len, obj_t obj)
{
    u64_t i, l;
    str_t s;

    *buf = obj->type;
    buf++;

    switch (obj->type)
    {
    case -TYPE_BOOL:
        buf[0] = obj->bool;
        return sizeof(type_t) + sizeof(bool_t);
    case -TYPE_BYTE:
        buf[0] = obj->byte;
        return sizeof(type_t) + sizeof(u8_t);
    case -TYPE_I64:
    case -TYPE_TIMESTAMP:
        memcpy(buf, &obj->i64, sizeof(i64_t));
        return sizeof(type_t) + sizeof(i64_t);
    case -TYPE_SYMBOL:
        s = symtostr(obj->i64);
        strncpy(buf, s, len);
        return sizeof(type_t) + str_len(s, len) + 1;
    case -TYPE_CHAR:
        buf[0] = obj->vchar;
        return sizeof(type_t) + sizeof(char_t);
    // case TYPE_GUID:
    //     memcpy(buf, &obj->guid, sizeof(guid_t));
    //     return sizeof(type_t) + sizeof(guid_t);
    case TYPE_BOOL:
        l = obj->len;
        memcpy(buf, &l, sizeof(u64_t));
        buf += sizeof(u64_t);
        for (i = 0; i < l; i++)
            buf[i] = as_bool(obj)[i];
        return sizeof(type_t) + sizeof(u64_t) + l * sizeof(bool_t);
    case TYPE_BYTE:
        l = obj->len;
        memcpy(buf, &l, sizeof(u64_t));
        buf += sizeof(u64_t);
        for (i = 0; i < l; i++)
            buf[i] = as_byte(obj)[i];
        return sizeof(type_t) + sizeof(u64_t) + l * sizeof(byte_t);
    case TYPE_I64:
    case TYPE_TIMESTAMP:
        l = obj->len;
        memcpy(buf, &l, sizeof(u64_t));
        buf += sizeof(u64_t);
        for (i = 0; i < l; i++)
            memcpy(buf + i * sizeof(i64_t), &as_i64(obj)[i], sizeof(i64_t));
        return sizeof(type_t) + sizeof(u64_t) + l * sizeof(i64_t);
    case TYPE_F64:
        l = obj->len;
        memcpy(buf, &l, sizeof(u64_t));
        buf += sizeof(u64_t);
        for (i = 0; i < l; i++)
            memcpy(buf + i * sizeof(f64_t), &as_f64(obj)[i], sizeof(f64_t));
        return sizeof(type_t) + sizeof(u64_t) + l * sizeof(f64_t);
    case TYPE_SYMBOL:
        l = obj->len;
        memcpy(buf, &l, sizeof(u64_t));
        buf += sizeof(u64_t);
        for (i = 0; i < l; i++)
        {
            s = symtostr(as_symbol(obj)[i]);
            buf += str_cpy(buf, s);
            *buf = '\0';
            buf++;
        }
        return sizeof(type_t) + sizeof(u64_t) + l * sizeof(i64_t);
    case TYPE_LIST:
        l = obj->len;
        memcpy(buf, &l, sizeof(u64_t));
        buf += sizeof(u64_t);
        for (i = 0; i < l; i++)
            buf += save_obj(buf, len, as_list(obj)[i]);
        return sizeof(type_t) + sizeof(u64_t) + l * sizeof(obj_t);
    default:
        panic(str_fmt(0, "ser: unsupported type: %d", obj->type));
    }
}

obj_t ser(obj_t obj)
{
    u64_t size = size_obj(obj);
    obj_t buf = vector(TYPE_BYTE, sizeof(struct header_t) + size);
    header_t *header = (header_t *)as_byte(buf);

    header->prefix = SERDE_PREFIX;
    header->version = RAYFORCE_VERSION;
    header->flags = 0;
    header->reserved = 0;
    header->size = size;

    save_obj(as_byte(buf) + sizeof(struct header_t), size, obj);

    return buf;
}

obj_t load_obj(byte_t **buf, u64_t len)
{
    u64_t i, l;
    obj_t obj;
    type_t type = **buf;
    (*buf)++;

    switch (type)
    {
    case -TYPE_BOOL:
        obj = bool((*buf)[0]);
        (*buf)++;
        return obj;
    case -TYPE_BYTE:
        obj = vbyte((*buf)[0]);
        (*buf)++;
        return obj;
    case -TYPE_I64:
    case -TYPE_TIMESTAMP:
        obj = i64(0);
        memcpy(&obj->i64, *buf, sizeof(i64_t));
        (*buf) += sizeof(i64_t);
        return obj;
    case -TYPE_SYMBOL:
        l = str_len(*buf, len);
        i = intern_symbol(*buf, l);
        obj = symboli64(i);
        (*buf) += l + 1;
        return obj;
    case -TYPE_CHAR:
        obj = vchar((*buf)[0]);
        (*buf)++;
        return obj;
    // case TYPE_GUID:
    //     obj = guid(buf);
    //     return obj;
    case TYPE_BOOL:
        memcpy(&l, *buf, sizeof(u64_t));
        (*buf) += sizeof(u64_t);
        obj = vector_bool(l);
        memcpy(as_bool(obj), *buf, l * sizeof(bool_t));
        (*buf) += l * sizeof(bool_t);
        return obj;
    case TYPE_BYTE:
        memcpy(&l, *buf, sizeof(u64_t));
        (*buf) += sizeof(u64_t);
        obj = vector_byte(l);
        memcpy(as_byte(obj), *buf, l * sizeof(byte_t));
        (*buf) += l * sizeof(byte_t);
        return obj;
    case TYPE_I64:
    case TYPE_TIMESTAMP:
        memcpy(&l, *buf, sizeof(u64_t));
        (*buf) += sizeof(u64_t);
        obj = vector_i64(l);
        memcpy(as_i64(obj), *buf, l * sizeof(i64_t));
        (*buf) += l * sizeof(i64_t);
        return obj;
    case TYPE_F64:
        memcpy(&l, *buf, sizeof(u64_t));
        (*buf) += sizeof(u64_t);
        obj = vector_f64(l);
        memcpy(as_f64(obj), *buf, l * sizeof(f64_t));
        (*buf) += l * sizeof(f64_t);
        return obj;
    case TYPE_SYMBOL:
        memcpy(&l, *buf, sizeof(u64_t));
        (*buf) += sizeof(u64_t);
        obj = vector_symbol(l);
        for (i = 0; i < l; i++)
        {
            u64_t id = intern_symbol(*buf, str_len(*buf, len));
            as_symbol(obj)[i] = id;
            (*buf) += str_len(*buf, len) + 1;
        }
        return obj;
    case TYPE_LIST:
        memcpy(&l, *buf, sizeof(u64_t));
        (*buf) += sizeof(u64_t);
        obj = list(l);
        for (i = 0; i < l; i++)
            as_list(obj)[i] = load_obj(buf, len);
        return obj;
    default:
        panic(str_fmt(0, "de: unsupported type: %d", *buf));
    }
}

obj_t de(obj_t buf)
{
    header_t *header = (header_t *)as_byte(buf);
    byte_t *b;

    if (header->version > RAYFORCE_VERSION)
        return error(ERR_NOT_SUPPORTED, "de: version is higher than supported");

    if (header->size != buf->len - sizeof(struct header_t))
        return error(ERR_IO, "de: corrupted data in a buffer");

    b = as_byte(buf) + sizeof(struct header_t);
    return load_obj(&b, header->size);
}