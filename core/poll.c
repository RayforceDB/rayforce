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

#if defined(_WIN32) || defined(__CYGWIN__)
#include "iocp.c"
#elif defined(__APPLE__) && defined(__MACH__)
#include "kqueue.c"
#elif defined(__linux__)
#include "epoll.c"
#elif defined(__EMSCRIPTEN__)
#include "wasm.c"
#else
#error "Unsupported platform"
#endif

#include "eval.h"

nil_t prompt(nil_t)
{
    printf("%s> %s", GREEN, RESET);
    fflush(stdout);
}

nil_t poll_exit(poll_p poll, i64_t code)
{
    poll->code = code;
}
