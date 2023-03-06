#include "format.h"
#include "storm.h"
#include <stdio.h>
#include <string.h>
#include "alloc.h"

#define MAX_i64_t_WIDTH 20
#define MAX_ROW_WIDTH MAX_i64_t_WIDTH * 2

extern str_t str_fmt(str_t fmt, ...)
{
    int n, size = MAX_ROW_WIDTH;
    str_t p = (str_t)storm_malloc(size);

    while (1)
    {
        va_list ap;
        va_start(ap, fmt);
        n = vsnprintf(p, size, fmt, ap);
        va_end(ap);

        if (n > -1 && n < size)
            break;

        size *= 2;
        p = storm_realloc(p, size);
    }

    return p;
}

str_t vector_i64_fmt(value_t value)
{
    str_t str, buf;
    i64_t count, remains, len;

    str = buf = (str_t)storm_malloc(MAX_ROW_WIDTH + 4);
    strncpy(buf, "[", 2);
    buf += 1;

    count = value->list_value.len < 1 ? 0 : value->list_value.len - 1;
    remains = MAX_ROW_WIDTH;
    len = 0;

    for (i64_t i = 0; i < count; i++)
    {
        remains = MAX_ROW_WIDTH - (buf - str);
        len = snprintf(buf, remains, "%lld, ", ((i64_t *)value->list_value.ptr)[i]);
        if (len < 0)
        {
            storm_free(str);
            return ERR_FORMAT;
        }

        if (remains < len)
        {
            buf = str + MAX_ROW_WIDTH - 1;
            break;
        }
        buf += len;
    }

    remains = MAX_ROW_WIDTH - (buf - str);
    if (value->list_value.len > 0 && remains > 0)
    {
        len = snprintf(buf, remains, "%lld", ((i64_t *)value->list_value.ptr)[count]);

        if (len < 0)
        {
            storm_free(str);
            return ERR_FORMAT;
        }
        if (remains < len)
            buf = str + MAX_ROW_WIDTH - 1;
        else
            buf += len;
    }

    if (remains < len)
    {
        strncpy(buf, "..", 3);
        buf += 2;
    }

    strncpy(buf, "]", 2);

    return str;
}

str_t error_fmt(value_t value)
{
    str_t code;

    switch (value->error_value.code)
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

    return str_fmt("** %s error: %s", code, value->error_value.message);
}

extern str_t value_fmt(value_t value)
{
    switch (value->type)
    {
    case -TYPE_i64:
        return str_fmt("%lld", value->i64_t_value);
    case TYPE_i64:
        return vector_i64_fmt(value);
    case TYPE_ERR:
        return error_fmt(value);
    default:
        return TYPE_ERR;
    }
}
