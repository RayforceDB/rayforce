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

#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "parse.h"
#include "hash.h"
#include "format.h"
#include "util.h"
#include "string.h"
#include "poll.h"
#include "heap.h"
#include "runtime.h"

#define LOGO "\
  RayforceDB: %d.%d %s\n\
  WASM target\n\
  Documentation: https://rayforcedb.com/\n\
  Github: https://github.com/singaraiona/rayforce\n"

poll_t poll_init(i64_t port)
{
    poll_t poll = (poll_t)heap_alloc(sizeof(struct poll_t));
    poll->code = NULL_I64;
    poll->replfile = string_from_str("wasm", 4);

    return poll;
}

nil_t poll_cleanup(poll_t poll)
{
    drop(poll->replfile);
    heap_free(poll);
}

i64_t poll_run(poll_t poll)
{
    unused(poll);
    return 0;
}

i64_t poll_register(poll_t poll, i64_t fd, u8_t version)
{
    unused(poll);
    unused(fd);
    unused(version);
    return 0;
}

nil_t poll_deregister(poll_t poll, i64_t id)
{
    unused(poll);
    unused(id);
}

obj_t ipc_send_sync(poll_t poll, i64_t id, obj_t msg)
{
    unused(poll);
    unused(id);
    unused(msg);
    return NULL_OBJ;
}

obj_t ipc_send_async(poll_t poll, i64_t id, obj_t msg)
{
    unused(poll);
    unused(id);
    unused(msg);
    return NULL_OBJ;
}

// nil_t print_logo(nil_t)
// {
//     str_t logo = str_fmt(0, LOGO, RAYFORCE_MAJOR_VERSION, RAYFORCE_MINOR_VERSION, __DATE__);
//     str_t fmt = str_fmt(0, "%s%s%s", BOLD, logo, RESET);
//     printjs(fmt);
//     heap_free(logo);
//     heap_free(fmt);
// }

EMSCRIPTEN_KEEPALIVE i32_t main(i32_t argc, str_t argv[])
{
    atexit(runtime_cleanup);
    runtime_init(argc, argv);
    return runtime_run();
}
