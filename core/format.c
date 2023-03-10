#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "format.h"
#include "storm.h"
#include "alloc.h"

#define MAX_i64_t_WIDTH 20
#define MAX_ROW_WIDTH MAX_i64_t_WIDTH * 2
#define F64_PRECISION 4

const str_t PADDING = "                                                                                                   ";

extern str_t str_fmt(u32_t lim, str_t fmt, ...)
{
    i32_t n, size = lim > 0 ? lim : MAX_ROW_WIDTH;
    str_t p = (str_t)storm_malloc(size);

    while (1)
    {
        va_list args;
        va_start(args, fmt);
        n = vsnprintf(p, size, fmt, args);
        va_end(args);

        if (n > -1 && n < size)
            break;

        size *= 2;
        p = storm_realloc(p, size);
    }

    return p;
}

str_t vector_fmt(u32_t pad, u32_t lim, value_t *value)
{
    if (lim < 4)
        return NULL;

    str_t str, buf;
    i64_t count, remains, len;
    i8_t v_type = value->type;
    u8_t slim = lim - 4;

    remains = slim;
    str = buf = (str_t)storm_malloc(lim);
    len = snprintf(buf, remains, "%*.*s[", pad, pad, PADDING);

    if (len < 0)
    {
        storm_free(str);
        return NULL;
    }

    buf += len;

    count = value->list.len < 1 ? 0 : value->list.len - 1;
    len = 0;

    for (i64_t i = 0; i < count; i++)
    {
        remains = slim - (buf - str);

        if (v_type == TYPE_I64)
            len = snprintf(buf, remains, "%lld, ", ((i64_t *)value->list.ptr)[i]);
        else if (v_type == TYPE_F64)
            len = snprintf(buf, remains, "%.*f, ", F64_PRECISION, ((f64_t *)value->list.ptr)[i]);
        else if (v_type == TYPE_SYMBOL)
            len = snprintf(buf, remains, "%s, ", symbols_get(((i64_t *)value->list.ptr)[i]));

        if (len < 0)
        {
            storm_free(str);
            return NULL;
        }

        if (remains < 4)
        {
            buf = str + slim;
            break;
        }
        buf += len;
    }

    remains = slim - (buf - str);
    if (value->list.len > 0 && remains > 4)
    {
        if (v_type == TYPE_I64)
            len = snprintf(buf, remains, "%lld", ((i64_t *)value->list.ptr)[count]);
        else if (v_type == TYPE_F64)
            len = snprintf(buf, remains, "%.*f", F64_PRECISION, ((f64_t *)value->list.ptr)[count]);
        else if (v_type == TYPE_SYMBOL)
            len = snprintf(buf, remains, "%s", symbols_get(((i64_t *)value->list.ptr)[count]));

        if (len < 0)
        {
            storm_free(str);
            return NULL;
        }
        if (remains < len)
            buf = str + slim;
        else
            buf += len;
    }

    if (remains < 4)
        strncpy(buf, "..]", 4);
    else
        strncpy(buf, "]", 2);

    return str;
}

str_t list_fmt(u32_t pad, u32_t lim, value_t *value)
{
    if (lim < 4)
        return NULL;

    if (value->list.ptr == NULL)
        return str_fmt(0, "null");

    str_t s, str, buf;
    i64_t count, remains, len;

    str = buf = (str_t)storm_malloc(2048);
    len = snprintf(buf, remains, "%*.*s(\n", pad, pad, PADDING);
    buf += len;

    if (len < 0)
    {
        storm_free(str);
        return NULL;
    }

    count = value->list.len < 1 ? 0 : value->list.len - 1;

    for (u64_t i = 0; i < value->list.len; i++)
    {
        s = value_fmt(((value_t *)value->list.ptr) + i);
        buf += snprintf(buf, MAX_ROW_WIDTH, "  %s,\n", s);
    }

    strncpy(buf, ")", 2);
    return str;
}

str_t error_fmt(u32_t pad, u32_t lim, value_t *value)
{
    str_t code;

    switch (value->error.code)
    {
    case ERR_INIT:
        code = "init";
        break;
    case ERR_PARSE:
        code = "parse";
        break;
    default:
        code = "unknown";
        break;
    }

    return str_fmt(0, "** %s error: %s", code, value->error.message);
}

extern str_t value_fmt(value_t *value)
{
    switch (value->type)
    {
    case TYPE_LIST:
        return list_fmt(0, MAX_ROW_WIDTH, value);
    case -TYPE_I64:
        return str_fmt(0, "%lld", value->i64);
    case -TYPE_F64:
        return str_fmt(0, "%.*f", F64_PRECISION, value->f64);
    case -TYPE_SYMBOL:
        return str_fmt(0, "%s", symbols_get(value->i64));
    case TYPE_I64:
        return vector_fmt(0, MAX_ROW_WIDTH, value);
    case TYPE_F64:
        return vector_fmt(0, MAX_ROW_WIDTH, value);
    case TYPE_SYMBOL:
        return vector_fmt(0, MAX_ROW_WIDTH, value);
    case TYPE_STRING:
        return str_fmt(value->list.len, "\"%s\"", value->list.ptr);
    case TYPE_ERROR:
        return error_fmt(0, 0, value);
    default:
        return str_fmt(0, "null");
    }
}
