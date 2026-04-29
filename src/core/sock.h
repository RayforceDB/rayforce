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

#ifndef RAY_SOCK_H
#define RAY_SOCK_H

#include <rayforce.h>

/* ===== Socket Abstraction ===== */

#ifdef RAY_OS_WINDOWS
  typedef intptr_t ray_sock_t;
  #define RAY_INVALID_SOCK ((ray_sock_t)-1)
#else
  typedef int ray_sock_t;
  #define RAY_INVALID_SOCK (-1)
#endif

ray_sock_t ray_sock_listen(uint16_t port);
ray_sock_t ray_sock_accept(ray_sock_t srv);
ray_sock_t ray_sock_connect(const char* host, uint16_t port, int timeout_ms);
int64_t    ray_sock_send(ray_sock_t s, const void* buf, size_t len);
int64_t    ray_sock_recv(ray_sock_t s, void* buf, size_t len);
void       ray_sock_close(ray_sock_t s);
ray_err_t  ray_sock_set_nonblocking(ray_sock_t s);

#endif /* RAY_SOCK_H */
