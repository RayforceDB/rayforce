/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
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

#if defined(RAY_OS_WINDOWS)

#include "core/poll.h"
#include <stdio.h>

/* Windows IOCP implementation — stub for now.
 * Full IOCP support is deferred to a future release. */

ray_poll_t* ray_poll_create(void)
{
    fprintf(stderr, "ray_poll_create: IOCP not yet implemented\n");
    return NULL;
}

void ray_poll_destroy(ray_poll_t* poll)
{
    (void)poll;
}

int64_t ray_poll_register(ray_poll_t* poll, ray_poll_reg_t* reg)
{
    (void)poll; (void)reg;
    return -1;
}

void ray_poll_deregister(ray_poll_t* poll, int64_t id)
{
    (void)poll; (void)id;
}

int64_t ray_poll_run(ray_poll_t* poll)
{
    (void)poll;
    return -1;
}

#endif /* _WIN32 */
