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
#include "error.h"
#include "timestamp.h"
#include "sys.h"

obj_p ray_hopen(obj_p x)
{
    i64_t fd;
    sock_addr_t addr;
    obj_p err;
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

    sock_set_nonblocking(fd, B8_TRUE);
    return i64(poll_register(runtime_get()->poll, fd, RAYFORCE_VERSION));
}

obj_p ray_hclose(obj_p x)
{
    poll_deregister(runtime_get()->poll, x->i64);

    return NULL_OBJ;
}

obj_p ray_read(obj_p x)
{
    i64_t fd, size, c = 0;
    str_p buf;
    obj_p res;

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
        {
            drop_obj(res);
            return sys_error(ERROR_TYPE_SYS, as_string(x));
        }

        return res;
    default:
        throw(ERR_TYPE, "read: unsupported type: '%s", type_name(x->type));
    }
}

obj_p io_write(i64_t fd, u8_t msg_type, obj_p obj)
{
    str_p fmt;

    if (obj == NULL_OBJ)
        return NULL_OBJ;

    // don't write to a stdin
    switch (fd)
    {
    case 0:
        if (obj->type == TYPE_CHAR)
            return ray_eval_str(obj, NULL);
        else
            return eval_obj(obj);
    case 1:
        fmt = obj_fmt(obj);
        fprintf(stdout, "%s\n", fmt);
        heap_free(fmt);
        return NULL_OBJ;
    case 2:
        fmt = obj_fmt(obj);
        fprintf(stderr, "%s\n", fmt);
        heap_free(fmt);
        return NULL_OBJ;
    default:
        // send ipc msg
        switch (msg_type)
        {
        case MSG_TYPE_RESP:
            return ipc_send_async(runtime_get()->poll, fd, clone_obj(obj));
        case MSG_TYPE_ASYN:
            return ipc_send_async(runtime_get()->poll, fd, clone_obj(obj));
        case MSG_TYPE_SYNC:
            return ipc_send_sync(runtime_get()->poll, fd, clone_obj(obj));
        default:
            throw(ERR_TYPE, "write: unsupported msg type: '%d", msg_type);
        }
    }
}

obj_p ray_write(obj_p x, obj_p y)
{
    // send to sock handle
    if (x->type == -TYPE_I64)
    {
        // send ipc msg
        if (x->i64 < 0)
            return io_write(-x->i64, MSG_TYPE_ASYN, y);
        else
            return io_write(x->i64, MSG_TYPE_SYNC, y);
    }

    throw(ERR_NOT_IMPLEMENTED, "write: not implemented");
}

obj_p parse_csv_field(i8_t type, str_p start, str_p end, i64_t row, obj_p out)
{
    i64_t n, inum;
    f64_t fnum;

    switch (type)
    {
    case TYPE_U8:
        if (start == NULL || end == NULL)
        {
            as_u8(out)[row] = 0;
            break;
        }
        inum = strtoll(start, &end, 10);
        if ((inum == LONG_MAX || inum == LONG_MIN) && errno == ERANGE)
            as_u8(out)[row] = 0;
        else
            as_u8(out)[row] = inum;
        break;
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
    case TYPE_TIMESTAMP:
        if (start == NULL || end == NULL)
        {
            as_timestamp(out)[row] = NULL_I64;
            break;
        }
        inum = timestamp_into_i64(timestamp_from_str(start));
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
            as_symbol(out)[row] = 0;
            break;
        }
        n = end - start;
        if ((n > 0) && (*(end - 1) == '\r'))
            n--;
        as_symbol(out)[row] = intern_symbol(start, n);
        break;
    case TYPE_CHAR:
        n = end - start;
        if ((n > 0) && (*(end - 1) == '\r'))
            n--;
        as_list(out)[row] = string_from_str(start, n);
        break;
    default:
        throw(ERR_TYPE, "csv: unsupported type: '%s", type_name(type));
    }

    return NULL_OBJ;
}

obj_p parse_csv_line(i8_t types[], i64_t cnt, str_p start, str_p end, i64_t row, obj_p cols, char_t sep)
{
    i64_t i, len;
    str_p prev, pos;
    obj_p res;

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
            pos = (str_p)memchr(prev, '"', len - 1);

            if (pos == NULL)
                throw(ERR_LENGTH, "csv: line: %lld invalid input: %s", row + 1, prev);

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

        pos = (str_p)memchr(pos, sep, len);
        if (pos == NULL)
        {
            if (i < cnt - 1)
                throw(ERR_LENGTH, "csv: line: %lld invalid input: %s", row + 1, prev);
            pos = end;
        }

        res = parse_csv_field(types[i], prev, pos, row, as_list(cols)[i]);

        if (!is_null(res))
            return res;

        pos++;
    }

    return NULL_OBJ;
}

obj_p ray_csv(obj_p *x, i64_t n)
{
    i64_t i, j, l, fd, len, lines, size;
    str_p buf, prev, pos, line;
    obj_p types, names, cols, res;
    i8_t type;
    char_t sep = ',';

    switch (n)
    {
    case 2:
    case 3:
        if (n == 3)
        {
            if (x[2]->type != -TYPE_CHAR)
                throw(ERR_TYPE, "csv: expected 'char' as 3rd argument, got: '%s", type_name(x[2]->type));

            sep = x[2]->u8;
        }
        // expect vector of types as 1st arg:
        if (x[0]->type != TYPE_SYMBOL)
            throw(ERR_TYPE, "csv: expected vector of types as 1st argument, got: '%s", type_name(x[0]->type));

        // expect string as 2nd arg:
        if (x[1]->type != TYPE_CHAR)
            throw(ERR_TYPE, "csv: expected string as 2nd argument, got: '%s", type_name(x[1]->type));

        // check that all symbols are valid typenames and convert them to types
        l = x[0]->len;
        types = string(l);
        for (i = 0; i < l; i++)
        {
            type = env_get_type_by_type_name(&runtime_get()->env, as_symbol(x[0])[i]);
            if (type == TYPE_ERROR)
            {
                drop_obj(types);
                throw(ERR_TYPE, "csv: invalid type: '%s", strof_sym(as_symbol(x[0])[i]));
            }

            if (type < 0)
                type = -type;

            as_string(types)[i] = type;
        }

        fd = fs_fopen(as_string(x[1]), ATTR_RDONLY);

        if (fd == -1)
        {
            drop_obj(types);
            return sys_error(ERROR_TYPE_SYS, as_string(x[1]));
        }

        size = fs_fsize(fd);

        if (size == -1)
        {
            drop_obj(types);
            fs_fclose(fd);
            throw(ERR_LENGTH, "get: file '%s': invalid size: %d", as_string(x[1]), size);
        }

        buf = (str_p)mmap_file(fd, size);

        if (buf == NULL)
        {
            drop_obj(types);
            fs_fclose(fd);
            throw(ERR_IO, "csv: mmap failed");
        }

        pos = buf;
        lines = 0;

        while ((pos = (str_p)memchr(pos, '\n', buf + size - pos)))
        {
            ++lines;
            ++pos; // Move past the newline character
        }

        if (lines == 0)
        {
            drop_obj(types);
            fs_fclose(fd);
            mmap_free(buf, size);
            throw(ERR_LENGTH, "csv: file '%s': invalid size: %d", as_string(x[1]), size);
        }

        // Adjust for the file not ending with a newline
        if (size > 0 && buf[size - 1] != '\n')
            ++lines;

        // parse header
        pos = (str_p)memchr(buf, '\n', size);
        len = pos - buf;
        line = pos + 1;

        names = vector_symbol(l);
        pos = buf;

        for (i = 0; i < l; i++)
        {
            prev = pos;
            pos = (str_p)memchr(pos, sep, len);
            if (pos == NULL)
            {
                if (i < l - 1)
                {
                    drop_obj(types);
                    drop_obj(names);
                    fs_fclose(fd);
                    mmap_free(buf, size);
                    throw(ERR_LENGTH, "csv: file '%s': invalid header (number of fields is less then csv contains)", as_string(x[1]));
                }

                pos = prev + len;
                // truncate \r (if any)
                if ((len > 0) && (pos[-1] == '\r'))
                    pos--;
            }

            as_symbol(names)[i] = intern_symbol(prev, pos - prev);
            pos++;
            len -= (pos - prev);
        }

        if (i != l)
        {
            drop_obj(types);
            drop_obj(names);
            fs_fclose(fd);
            mmap_free(buf, size);
            throw(ERR_LENGTH, "csv: file '%s': invalid header (number of fields is less then csv contains)", as_string(x[1]));
        }

        // exclude header
        lines--;

        // allocate columns
        cols = list(l);
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
            line = (str_p)memchr(prev, '\n', size);
            // the last line may not end with \n
            if (line == NULL)
            {
                if (j < lines - 1)
                {
                    drop_obj(types);
                    drop_obj(names);
                    drop_obj(cols);
                    fs_fclose(fd);
                    mmap_free(buf, size);
                    throw(ERR_LENGTH, "csv: file '%s': invalid line (number of fields is less then csv contains)", as_string(x[1]));
                }

                line = prev + size;
                // truncate \r (if any)
                if ((size > 0) && (line[-1] == '\r'))
                    line--;
            }
            res = parse_csv_line((i8_t *)as_string(types), l, prev, line, j, cols, sep);
            if (!is_null(res))
            {
                drop_obj(types);
                drop_obj(names);
                drop_obj(cols);
                fs_fclose(fd);
                mmap_free(buf, size);
                return res;
            }
            size -= ((++line) - prev);
            prev = line;
        }

        drop_obj(types);
        fs_fclose(fd);
        mmap_free(buf, size);

        return table(names, cols);
    default:
        throw(ERR_LENGTH, "csv: expected 1..3 arguments, got %d", n);
    }
}

obj_p ray_parse(obj_p x)
{
    if (!x || x->type != TYPE_CHAR)
        throw(ERR_TYPE, "parse: expected string");

    return parse_str(0, x, NULL);
}

obj_p ray_eval(obj_p x)
{
    if (!x || x->type != TYPE_CHAR)
        throw(ERR_TYPE, "eval: expected string");

    return ray_eval_str(x, NULL_OBJ);
}

obj_p ray_load(obj_p x)
{
    obj_p file, res;

    if (!x || x->type != TYPE_CHAR)
        throw(ERR_TYPE, "load: expected string");

    file = ray_read(x);
    if (is_error(file))
        return file;

    res = ray_eval_str(file, x);
    drop_obj(file);

    return res;
}
