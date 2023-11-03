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

#include <errno.h>
#include <limits.h>
#include "io.h"
#include "fs.h"
#include "util.h"
#include "ops.h"
#include "sock.h"
#include "heap.h"
#include "serde.h"
#include "poll.h"
#include "runtime.h"

obj_t ray_hopen(obj_t x)
{
    i64_t fd;
    sock_addr_t addr;
    obj_t err;
    u8_t handshake[2] = {RAYFORCE_VERSION, 0x00};

    if (x->type != TYPE_CHAR)
        throw(ERR_TYPE, "hopen: expected char");

    if (sock_addr_from_str(as_string(x), &addr) == -1)
        throw(ERR_IO, "hopen: invalid address: %s", as_string(x));

    fd = sock_open(&addr);

    if (fd == -1)
        return sys_error(ERROR_TYPE_SOCK, "hopen");

    if (sock_send(fd, handshake, 2) == -1)
    {
        err = sys_error(ERROR_TYPE_SOCK, "hopen: send handshake");
        sock_close(fd);
        return err;
    }

    if (sock_recv(fd, handshake, 2) == -1)
    {
        err = sys_error(ERROR_TYPE_SOCK, "hopen: recv handshake");
        sock_close(fd);
        return err;
    }

    sock_set_nonblocking(fd, true);
    return i64(poll_register(runtime_get()->poll, fd, RAYFORCE_VERSION));
}

obj_t ray_hclose(obj_t x)
{
    poll_deregister(runtime_get()->poll, x->i64);

    return null(0);
}

obj_t ray_read(obj_t x)
{
    i64_t fd, size, c = 0;
    str_t buf;
    obj_t res;

    switch (x->type)
    {
    case TYPE_CHAR:
        fd = fs_fopen(as_string(x), ATTR_RDONLY);

        // error handling if file does not exist
        if (fd == -1)
            return sys_error(ERROR_TYPE_SYS, as_string(x));

        size = fs_fsize(fd);
        res = string(size);
        buf = as_string(res);

        c = fs_fread(fd, buf, size);
        fs_fclose(fd);

        if (c != size)
            return sys_error(ERROR_TYPE_SYS, as_string(x));

        return res;
    default:
        throw(ERR_TYPE, "read: unsupported type: '%s", typename(x->type));
    }
}

obj_t ray_write(obj_t x, obj_t y)
{
    str_t fmt;

    // send to sock handle
    if (x->type == -TYPE_I64)
    {
        // don't write to a stdin
        if (x->i64 == 0)
        {
            if (y->type == TYPE_CHAR)
                return eval_str(0, "ipc", as_string(y));
            else
                return eval_obj(0, "ipc", clone(y));
        }

        // stdout || stderr
        if (x->i64 == 1 || x->i64 == 2)
        {
            fmt = obj_fmt(x);
            fprintf((FILE *)x->i64, "%s\n", fmt);
            heap_free(fmt);
            return null(0);
        }

        // send ipc msg
        if (x->i64 > 2)
            return ipc_send_sync(runtime_get()->poll, x->i64, clone(y));
        else
            return ipc_send_async(runtime_get()->poll, -x->i64, clone(y));
    }

    throw(ERR_NOT_IMPLEMENTED, "write: not implemented");
}

obj_t parse_csv_field(type_t type, str_t start, str_t end, i64_t row, obj_t out)
{
    i64_t inum;
    f64_t fnum;

    switch (type)
    {
    case TYPE_I64:
        if (start == NULL || end == NULL)
        {
            as_i64(out)[row] = NULL_I64;
            break;
        }
        inum = strtoll(start, &end, 10);
        if ((inum == LONG_MAX || inum == LONG_MIN) && errno == ERANGE)
            as_i64(out)[row] = NULL_I64;
        else
            as_i64(out)[row] = inum;
        break;
        // TODO: parse timestamp literals
    case TYPE_TIMESTAMP:
        if (start == NULL || end == NULL)
        {
            as_timestamp(out)[row] = NULL_I64;
            break;
        }
        inum = strtoll(start, &end, 10);
        if ((inum == LONG_MAX || inum == LONG_MIN) && errno == ERANGE)
            as_timestamp(out)[row] = NULL_I64;
        else
            as_timestamp(out)[row] = inum;
        break;
    case TYPE_F64:
        if (start == NULL || end == NULL)
        {
            as_f64(out)[row] = NULL_F64;
            break;
        }
        fnum = strtod(start, &end);
        if (errno == ERANGE)
            as_f64(out)[row] = NULL_F64;
        else
            as_f64(out)[row] = fnum;
        break;
    case TYPE_SYMBOL:
        if (start == NULL || end == NULL)
        {
            as_symbol(out)[row] = NULL_I64;
            break;
        }
        as_symbol(out)[row] = intern_symbol(start, end - start);
        break;
    case TYPE_CHAR:
        as_list(out)[row] = string_from_str(start, end - start);
        break;
    default:
        throw(ERR_TYPE, "csv: unsupported type: '%s", typename(type));
    }

    return null(0);
}

obj_t parse_csv_line(type_t types[], i64_t cnt, str_t start, str_t end, i64_t row, obj_t cols)
{
    i64_t i, len;
    str_t prev, pos;
    obj_t res;

    for (i = 0, pos = start; i < cnt; i++)
    {
        if (pos == NULL || end == NULL)
        {
            res = parse_csv_field(types[i], NULL, NULL, row, as_list(cols)[i]);

            if (!is_null(res))
                return res;

            continue;
        }

        len = end - pos;
        prev = pos;

        // quoted field?
        if (len > 0 && *prev == '"')
        {
            prev++;
            pos = memchr(prev, '"', len - 1);

            if (pos == NULL)
                throw(ERR_LENGTH, "csv: invalid field: %s", prev);

            res = parse_csv_field(types[i], prev, pos, row, as_list(cols)[i]);
            pos += 2; // skip quote and comma

            if (!is_null(res))
                return res;

            continue;
        }

        if (len == 0)
        {
            res = parse_csv_field(types[i], NULL, NULL, row, as_list(cols)[i]);

            if (!is_null(res))
                return res;

            continue;
        }

        pos = memchr(pos, ',', len);
        if (pos == NULL)
            pos = end;

        res = parse_csv_field(types[i], prev, pos, row, as_list(cols)[i]);

        if (!is_null(res))
            return res;

        pos++;
    }

    return null(0);
}

obj_t ray_csv(obj_t *x, i64_t n)
{
    i64_t i, j, l, fd, len, lines, size;
    str_t buf, prev, pos, line;
    obj_t types, names, cols, res;
    type_t type;

    switch (n)
    {
    case 2:
        // expect vector of types as 1st arg:
        if (x[0]->type != TYPE_SYMBOL)
            throw(ERR_TYPE, "csv: expected vector of types as 1st argument, got: '%s", typename(x[0]->type));

        // expect string as 2nd arg:
        if (x[1]->type != TYPE_CHAR)
            throw(ERR_TYPE, "csv: expected string as 2nd argument, got: '%s", typename(x[1]->type));

        // check that all symbols are valid typenames and convert them to types
        l = x[0]->len;
        types = string(l);
        for (i = 0; i < l; i++)
        {
            type = env_get_type_by_typename(&runtime_get()->env, as_symbol(x[0])[i]);
            if (type == TYPE_ERROR)
            {
                drop(types);
                throw(ERR_TYPE, "csv: invalid type: '%s", symtostr(as_symbol(x[0])[i]));
            }

            if (type < 0)
                type = -type;

            as_string(types)[i] = type;
        }

        fd = fs_fopen(as_string(x[1]), ATTR_RDONLY);

        if (fd == -1)
        {
            drop(types);
            return sys_error(ERROR_TYPE_SYS, as_string(x[1]));
        }

        size = fs_fsize(fd);

        if (size == -1)
        {
            drop(types);
            fs_fclose(fd);
            throw(ERR_LENGTH, "get: file '%s': invalid size: %d", as_string(x[1]), size);
        }

        buf = (str_t)mmap_file(fd, size);

        if (buf == NULL)
        {
            drop(types);
            fs_fclose(fd);
            throw(ERR_IO, "csv: mmap failed");
        }

        pos = buf;
        lines = 0;

        while ((pos = memchr(pos, '\n', buf + size - pos)))
        {
            ++lines;
            ++pos; // Move past the newline character
        }

        if (lines == 0)
        {
            drop(types);
            fs_fclose(fd);
            mmap_free(buf, size);
            throw(ERR_LENGTH, "csv: file '%s': invalid size: %d", as_string(x[1]), size);
        }

        // Adjust for the file not ending with a newline
        if (size > 0 && buf[size - 1] != '\n')
            ++lines;

        // parse header
        pos = memchr(buf, '\n', size);
        len = pos - buf;
        line = pos + 1;

        names = vector_symbol(l);
        pos = buf;
        for (i = 0; i < l; i++)
        {
            prev = pos;
            pos = memchr(pos, ',', len);
            if (pos == NULL)
                pos = prev + len;
            as_symbol(names)[i] = intern_symbol(prev, pos - prev);
            pos++;
            len -= (pos - prev);
        }

        if (i != l)
        {
            drop(types);
            drop(names);
            fs_fclose(fd);
            mmap_free(buf, size);
            throw(ERR_LENGTH, "csv: file '%s': invalid header (number of fields is less then csv contains)", as_string(x[1]));
        }

        // exclude header
        lines--;

        // allocate columns
        cols = vector(TYPE_LIST, l);
        for (i = 0; i < l; i++)
        {
            if (as_string(types)[i] == TYPE_CHAR)
                as_list(cols)[i] = vector(TYPE_LIST, lines);
            else
                as_list(cols)[i] = vector(as_string(types)[i], lines);
        }

        // parse lines
        for (j = 0, prev = line; j < lines; j++)
        {
            line = memchr(prev, '\n', size);
            res = parse_csv_line((type_t *)as_string(types), l, prev, line, j, cols);
            if (!is_null(res))
            {
                drop(types);
                drop(names);
                drop(cols);
                fs_fclose(fd);
                mmap_free(buf, size);
                return res;
            }
            size -= ((++line) - prev);
            prev = line;
        }

        drop(types);
        fs_fclose(fd);
        mmap_free(buf, size);

        return table(names, cols);
    default:
        throw(ERR_LENGTH, "csv: expected 1 or 2 arguments, got %d", n);
    }
}

obj_t ray_parse(obj_t x)
{
    if (!x || x->type != TYPE_CHAR)
        throw(ERR_TYPE, "parse: expected string");

    return parse(&runtime_get()->parser, "repl", as_string(x));
}

obj_t ray_eval(obj_t x)
{
    if (!x || x->type != TYPE_CHAR)
        throw(ERR_TYPE, "eval: expected string");

    return eval_str(0, "repl", as_string(x));
}

obj_t ray_exec(obj_t x)
{
    return eval_obj(0, "repl", x);
}

obj_t ray_load(obj_t x)
{
    obj_t file, res;

    if (!x || x->type != TYPE_CHAR)
        throw(ERR_TYPE, "load: expected string");

    file = ray_read(x);
    if (is_error(file))
        return file;

    res = eval_str(0, as_string(x), as_string(file));
    drop(file);

    return res;
}