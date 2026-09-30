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

#include "core/platform.h"

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
/* As ray_sock_send, but gives up once the monotonic clock
 * (ray_time_now_ms) reaches deadline_ms, returning -1 with errno
 * ETIMEDOUT.  The frame may then be partly written, so the caller must
 * treat the stream as unusable.  deadline_ms < 0 = no deadline. */
int64_t    ray_sock_send_until(ray_sock_t s, const void* buf, size_t len,
                               int64_t deadline_ms);
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
/* Dead-peer detection for an established TCP connection (#589).
 * budget_ms > 0 turns on kernel keepalive tuned so that a silent peer is
 * declared dead about budget_ms after the last thing it sent: the first
 * probe goes out at budget_ms/2 of idle, then three more at budget_ms/6
 * (whole seconds, at least 1 each — the kernel's granularity).  Keepalive
 * only probes while nothing is in flight, so a live peer that is merely
 * slow to read or to answer is never affected.
 *
 * user_timeout additionally bounds how long sent data may stay
 * unacknowledged (TCP_USER_TIMEOUT on Linux, the nearest equivalent
 * elsewhere).  That also catches a peer that vanishes with our data in
 * flight, but it fires on a live peer that stops reading for budget_ms
 * too (a zero receive window), so it is opt-in only.
 *
 * budget_ms <= 0 turns keepalive off.  Best-effort: an option the
 * platform lacks is skipped.  Returns 0, or -1 if keepalive itself could
 * not be enabled. */
int        ray_sock_set_keepalive(ray_sock_t s, int budget_ms, bool user_timeout);
ray_err_t  ray_sock_set_blocking(ray_sock_t s);

/* ===== Link locality =====
 * True when the peer is on this machine: AF_UNIX, IPv4 127/8, IPv6 ::1,
 * or a v4-mapped loopback.  `sa` points at a `struct sockaddr`; it is
 * taken as void* so this header stays free of <sys/socket.h>.  A short
 * or NULL address reads as non-local rather than being trusted.
 *
 * ray_sock_peer_is_local resolves the peer with getpeername and answers
 * false when there is none (unconnected socket, bad fd) — an unknown
 * link must fall back to the conservative default, never to "local". */
bool ray_sock_addr_is_local(const void* sa, size_t salen);
bool ray_sock_peer_is_local(ray_sock_t s);

#endif /* RAY_SOCK_H */
