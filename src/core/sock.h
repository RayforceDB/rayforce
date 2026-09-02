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
/* Bind to a specific IPv4 address; host NULL/empty means INADDR_ANY.  An
 * unparseable host fails the listen (errno EINVAL), never falls back. */
ray_sock_t ray_sock_listen_at(const char* host, uint16_t port);
ray_sock_t ray_sock_accept(ray_sock_t srv);
/* Connect to host:port.  timeout_ms > 0 bounds the connect: the socket
 * connects non-blocking and waits at most timeout_ms for completion (a
 * blocking connect() ignores SO_*TIMEO and would otherwise hang for the
 * OS default), then the same value is applied as SO_RCVTIMEO/SO_SNDTIMEO
 * for the subsequent handshake I/O.  timeout_ms <= 0 = blocking connect,
 * no I/O timeout.  On a connect timeout, errno is set to ETIMEDOUT and
 * RAY_INVALID_SOCK is returned. */
ray_sock_t ray_sock_connect(const char* host, uint16_t port, int timeout_ms);
int64_t    ray_sock_send(ray_sock_t s, const void* buf, size_t len);
int64_t    ray_sock_recv(ray_sock_t s, void* buf, size_t len);
/* Block until s is readable (or hung up).  timeout_ms < 0 = no timeout.
 * Returns 1 readable, 0 timed out, -1 error. */
int        ray_sock_wait_readable(ray_sock_t s, int timeout_ms);
/* Same, but returns -2 when interrupted by a signal (does not retry). */
int        ray_sock_wait_readable_intr(ray_sock_t s, int timeout_ms);
/* Send one byte of TCP urgent (out-of-band) data (raises SIGURG on the peer). */
int        ray_sock_send_oob(ray_sock_t s, char byte);
/* Route this socket's SIGURG (out-of-band arrival) to our process (POSIX). */
void       ray_sock_set_oob_owner(ray_sock_t s);
/* Consume one pending urgent byte on s; returns 1 if present (async-signal-safe). */
int        ray_sock_take_oob(ray_sock_t s);
void       ray_sock_close(ray_sock_t s);
ray_err_t  ray_sock_set_nonblocking(ray_sock_t s);
ray_err_t  ray_sock_set_blocking(ray_sock_t s);

#endif /* RAY_SOCK_H */
