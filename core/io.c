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
    u8_t handshake[2] = {0, RAYFORCE_VERSION};
    ipc_data_t data;
    sock_addr_t addr;

    if (x->type != TYPE_CHAR)
        emit(ERR_TYPE, "hopen: expected char");

    if (sock_addr_from_str(as_string(x), &addr) == -1)
        emit(ERR_IO, "hopen: invalid address: %s", as_string(x));

    fd = sock_open(&addr);

    if (fd == -1)
        return sys_error(TYPE_GETLASTERROR, "hopen");

    if (sock_send(fd, handshake, 2) == -1)
    {
        sock_close(fd);
        return sys_error(TYPE_GETLASTERROR, "hopen");
    }

    if (sock_recv(fd, handshake, 2) == -1)
    {
        sock_close(fd);
        return sys_error(TYPE_GETLASTERROR, "hopen");
    }

    data = poll_add(runtime_get()->select, fd);
    data->rx.version = handshake[1];

    return i64(fd);
}

obj_t ray_hclose(obj_t x)
{
    sock_close(x->i64);

    // poll_del(runtime_get()->select, x->i64);

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
            return sys_error(TYPE_GETLASTERROR, as_string(x));

        size = fs_fsize(fd);
        res = string(size);
        buf = as_string(res);

        c = fs_fread(fd, buf, size);
        fs_fclose(fd);

        if (c != size)
            return sys_error(TYPE_GETLASTERROR, as_string(x));

        return res;
    default:
        emit(ERR_TYPE, "read: unsupported type: %d", x->type);
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
                return eval_obj(0, "ipc", y);
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
            return ipc_send_sync(runtime_get()->select, x->i64, y);
        else
            return ipc_send_async(runtime_get()->select, -x->i64, y);
    }

    emit(ERR_NOT_IMPLEMENTED, "write: not implemented");
}
