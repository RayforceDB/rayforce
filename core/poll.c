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

#include "poll.h"
#include "format.h"
#include "sock.h"

#if defined(OS_WINDOWS)
#include "iocp.c"
#elif defined(OS_MACOS)
#include "kqueue.c"
#elif defined(OS_LINUX)
#include "epoll.c"
#elif defined(OS_WASM)
#include "wasm.c"
#endif

#include "eval.h"
#include "binary.h"

nil_t poll_set_usr_fd(i64_t fd) {
    obj_p s, k, v;

    s = symbol(".z.w", 4);
    k = i64(fd);
    v = binary_set(s, k);
    drop_obj(k);
    drop_obj(v);
    drop_obj(s);
}

nil_t poll_exit(poll_p poll, i64_t code) { poll->code = code; }
