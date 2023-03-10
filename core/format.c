#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "format.h"
#include "storm.h"
#include "alloc.h"

#define MAX_i64_t_WIDTH 20
#define MAX_ROW_WIDTH MAX_i64_t_WIDTH * 2
#define F64_PRECISION 4

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

str_t vector_fmt(value_t *value)
{
    str_t str, buf;
    i64_t count, remains, len;
    i8_t v_type = value->type;

    str = buf = (str_t)storm_malloc(MAX_ROW_WIDTH + 4);
    strncpy(buf, "[", 2);
    buf += 1;

    count = value->s0.len < 1 ? 0 : value->s0.len - 1;
    remains = MAX_ROW_WIDTH;
    len = 0;

    for (i64_t i = 0; i < count; i++)
    {
        remains = MAX_ROW_WIDTH - (buf - str);

        if (v_type == TYPE_I64)
            len = snprintf(buf, remains, "%lld, ", ((i64_t *)value->s0.ptr)[i]);
        else if (v_type == TYPE_F64)
            len = snprintf(buf, remains, "%.*f, ", F64_PRECISION, ((f64_t *)value->s0.ptr)[i]);
        else if (v_type == TYPE_SYMBOL)
            len = snprintf(buf, remains, "%s, ", symbols_get(((i64_t *)value->s0.ptr)[i]));

        if (len < 0)
        {
            storm_free(str);
            return NULL;
        }

        if (remains < 3)
        {
            buf = str + MAX_ROW_WIDTH - 2;
            break;
        }
        buf += len;
    }

    remains = MAX_ROW_WIDTH - (buf - str);
    if (value->s0.len > 0 && remains > 3)
    {
        if (v_type == TYPE_I64)
            len = snprintf(buf, remains, "%lld", ((i64_t *)value->s0.ptr)[count]);
        else if (v_type == TYPE_F64)
            len = snprintf(buf, remains, "%.*f", F64_PRECISION, ((f64_t *)value->s0.ptr)[count]);
        else if (v_type == TYPE_SYMBOL)
            len = snprintf(buf, remains, "%s", symbols_get(((i64_t *)value->s0.ptr)[count]));

        if (len < 0)
        {
            storm_free(str);
            return NULL;
        }
        if (remains < len)
            buf = str + MAX_ROW_WIDTH - 2;
        else
            buf += len;
    }

    if (remains < 3)
        strncpy(buf, "..]", 4);
    else
        strncpy(buf, "]", 2);

    return str;
}

str_t error_fmt(value_t *value)
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

    return str_fmt("** %s error: %s", code, value->error.message);
}

extern str_t value_fmt(value_t *value)
{
    switch (value->type)
    {
    case TYPE_S0:
        return str_fmt("null");
    case -TYPE_I64:
        return str_fmt("%lld", value->i64);
    case -TYPE_F64:
        return str_fmt("%.*f", F64_PRECISION, value->f64);
    case -TYPE_SYMBOL:
        return str_fmt("%s", symbols_get(value->i64));
    case TYPE_I64:
        return vector_fmt(value);
    case TYPE_F64:
        return vector_fmt(value);
    case TYPE_SYMBOL:
        return vector_fmt(value);
    case TYPE_ERROR:
        return error_fmt(value);
    default:
        return str_fmt("null");
    }
}
