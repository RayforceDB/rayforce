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
u64_t obj_size(obj_t obj)
{
    u64_t i, l, size;
    switch (obj->type)
    {
    case -TYPE_BOOL:
        return sizeof(type_t) + sizeof(bool_t);
    case -TYPE_BYTE:
        return sizeof(type_t) + sizeof(u8_t);
    case -TYPE_I64:
    case -TYPE_TIMESTAMP:
        return sizeof(type_t) + sizeof(i64_t);
    case -TYPE_F64:
        return sizeof(type_t) + sizeof(f64_t);
    case -TYPE_SYMBOL:
        return sizeof(type_t) + strlen(symtostr(obj->i64)) + 1;
    case TYPE_LIST:
        l = obj->len;
        size = sizeof(type_t);
        for (i = 0; i < l; i++)
            size += obj_size(as_list(obj)[i]);
        return size;
    default:
        panic(str_fmt(0, "obj_size: unsupported type: %d", obj->type));
    }
}

u64_t _ser(byte_t *buf, u64_t len, obj_t obj)
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
        return sizeof(type_t) + string_len(s, len) + 1;
    default:
        panic(str_fmt(0, "ser: unsupported type: %d", obj->type));
    }
}

obj_t ser(obj_t obj)
{
    u64_t size = obj_size(obj);
    obj_t buf = vector(TYPE_BYTE, sizeof(struct header_t) + size);
    header_t *header = (header_t *)as_byte(buf);

    header->version = RAYFORCE_VERSION;
    header->flags = 0;
    header->reserved = 0;
    header->padding = 0;
    header->size = size;

    _ser(as_byte(buf) + sizeof(struct header_t), size, obj);

    return buf;
}

obj_t _de(byte_t *buf, u64_t len)
{
    u64_t i, l;
    obj_t obj;
    type_t type = *buf;
    buf++;

    switch (type)
    {
    case -TYPE_BOOL:
        obj = bool(buf[0]);
        return obj;
    case -TYPE_BYTE:
        obj = byte(buf[0]);
        return obj;
    case -TYPE_I64:
    case -TYPE_TIMESTAMP:
        obj = i64(0);
        memcpy(&obj->i64, buf, sizeof(i64_t));
        return obj;
    case -TYPE_SYMBOL:
        l = string_len(buf, len);
        i = intern_symbol(buf, l);
        obj = symboli64(i);
        return obj;
    default:
        panic(str_fmt(0, "de: unsupported type: %d", *buf));
    }
}

obj_t de(obj_t buf)
{
    header_t *header = (header_t *)as_byte(buf);

    if (header->version > RAYFORCE_VERSION)
        return error(ERR_NOT_SUPPORTED, "de: version is higher than supported");

    if (header->size != buf->len - sizeof(struct header_t))
        return error(ERR_IO, "de: corrupted data in a buffer");

    return _de(as_byte(buf) + sizeof(struct header_t), header->size);
}