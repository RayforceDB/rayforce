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

#ifndef RAY_OS_WINDOWS
  #define _GNU_SOURCE
#endif

#include "core/sock.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifdef RAY_OS_WINDOWS
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <fcntl.h>
  #include <netdb.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <netinet/tcp.h>
  #include <poll.h>
  #include <unistd.h>
#endif

/* ===== Socket Implementation ===== */

ray_sock_t ray_sock_listen_at(const char* host, uint16_t port)
{
    /* NULL/empty host keeps the historical INADDR_ANY bind.  A host that
     * does not parse as an IPv4 address is a loud bind failure — never a
     * silent fallback to all interfaces, which would defeat the point of
     * asking for a confined listener. */
    struct in_addr bind_addr;
    if (host && *host) {
        if (inet_pton(AF_INET, host, &bind_addr) != 1) {
            errno = EINVAL;
            return RAY_INVALID_SOCK;
        }
    } else {
        bind_addr.s_addr = htonl(INADDR_ANY);
    }

    ray_sock_t fd = (ray_sock_t)socket(AF_INET, SOCK_STREAM, 0);
    if (fd == RAY_INVALID_SOCK) return RAY_INVALID_SOCK;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr        = bind_addr;
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ray_sock_close(fd);
        return RAY_INVALID_SOCK;
    }
    if (listen(fd, 128) < 0) {
        ray_sock_close(fd);
        return RAY_INVALID_SOCK;
    }
    return fd;
}

ray_sock_t ray_sock_listen(uint16_t port)
{
    return ray_sock_listen_at(NULL, port);
}

ray_sock_t ray_sock_accept(ray_sock_t srv)
{
    ray_sock_t fd;
    do {
        fd = (ray_sock_t)accept(srv, NULL, NULL);
    } while (fd == RAY_INVALID_SOCK && errno == EINTR);

    if (fd == RAY_INVALID_SOCK) return RAY_INVALID_SOCK;

    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));
    return fd;
}

/* Connect an already-created socket `fd` to one resolved address.  With
 * timeout_ms > 0 the connect is driven non-blocking + poll (a blocking
 * connect() ignores SO_*TIMEO) and the same budget is then applied as the
 * handshake SO_RCVTIMEO/SO_SNDTIMEO; otherwise it is a plain blocking
 * connect.  Returns 0 on success, -1 on failure with errno set
 * (ETIMEDOUT on a connect timeout). */
static int sock_connect_one(ray_sock_t fd, const struct sockaddr* addr,
                            socklen_t addrlen, int timeout_ms)
{
    if (timeout_ms <= 0)
        return connect(fd, addr, addrlen) < 0 ? -1 : 0;

    ray_sock_set_nonblocking(fd);
    int rc = connect(fd, addr, addrlen);
    if (rc < 0) {
#ifdef RAY_OS_WINDOWS
        int werr = WSAGetLastError();
        int in_progress = (werr == WSAEWOULDBLOCK || werr == WSAEINPROGRESS);
#else
        int in_progress = (errno == EINPROGRESS);
#endif
        if (!in_progress) return -1;
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int pr;
#ifdef RAY_OS_WINDOWS
        pr = WSAPoll(&pfd, 1, timeout_ms);
#else
        do { pr = poll(&pfd, 1, timeout_ms); } while (pr < 0 && errno == EINTR);
#endif
        if (pr == 0) { errno = ETIMEDOUT; return -1; }
        if (pr < 0) return -1;
        /* Writable: harvest the pending connect result via SO_ERROR. */
        int soerr = 0;
        socklen_t soerr_len = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&soerr, &soerr_len) < 0
            || soerr != 0) {
            if (soerr != 0) errno = soerr;
            return -1;
        }
    }
    ray_sock_set_blocking(fd);

    /* Apply the same budget as the handshake send/recv timeout. */
#ifdef RAY_OS_WINDOWS
    DWORD tv = (DWORD)timeout_ms;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    return 0;
}

ray_sock_t ray_sock_connect(const char* host, uint16_t port, int timeout_ms)
{
    struct addrinfo hints, *res = NULL, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        errno = EHOSTUNREACH;           /* not a refusal, and not a timeout */
        return RAY_INVALID_SOCK;
    }

    /* Try every resolved address in turn, not just the first.  `localhost`
     * commonly resolves to BOTH ::1 (IPv6, often first) and 127.0.0.1; a
     * server bound IPv4-only refuses the ::1 attempt, so we must fall
     * through to the next candidate rather than give up. */
    ray_sock_t fd = RAY_INVALID_SOCK;
    int saved_errno = 0;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = (ray_sock_t)socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == RAY_INVALID_SOCK) { saved_errno = errno; continue; }
        if (sock_connect_one(fd, rp->ai_addr, (socklen_t)rp->ai_addrlen,
                             timeout_ms) == 0)
            break;                          /* connected */
        saved_errno = errno;
        ray_sock_close(fd);
        fd = RAY_INVALID_SOCK;
    }
    freeaddrinfo(res);

    if (fd == RAY_INVALID_SOCK) {
        errno = saved_errno;                /* preserve ETIMEDOUT / refused */
        return RAY_INVALID_SOCK;
    }

    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));
    return fd;
}

int64_t ray_sock_send(ray_sock_t s, const void* buf, size_t len)
{
    const uint8_t* p   = (const uint8_t*)buf;
    size_t         rem = len;
    while (rem > 0) {
#ifdef RAY_OS_WINDOWS
        int n = send(s, (const char*)p, (int)rem, 0);
#else
        ssize_t n = send(s, p, rem, MSG_NOSIGNAL);
#endif
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Wait for write-readiness before retry */
                struct pollfd pfd = { .fd = s, .events = POLLOUT };
                poll(&pfd, 1, -1);
                continue;
            }
            return -1;
        }
        p   += n;
        rem -= (size_t)n;
    }
    return (int64_t)len;
}

int64_t ray_sock_recv(ray_sock_t s, void* buf, size_t len)
{
    for (;;) {
#ifdef RAY_OS_WINDOWS
        int n = recv(s, (char*)buf, (int)len, 0);
#else
        ssize_t n = recv(s, buf, len, 0);
#endif
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        return (int64_t)n;   /* 0 = peer closed */
    }
}

int ray_sock_wait_readable(ray_sock_t s, int timeout_ms)
{
    /* Same wait primitive ray_sock_send uses for write-readiness. */
    struct pollfd pfd = { .fd = s, .events = POLLIN };
    for (;;) {
#ifdef RAY_OS_WINDOWS
        int n = WSAPoll(&pfd, 1, timeout_ms);
#else
        int n = poll(&pfd, 1, timeout_ms);
#endif
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return 0;
        return 1;  /* POLLIN/POLLHUP/POLLERR all mean "go recv" */
    }
}

/* Like ray_sock_wait_readable but returns -2 (instead of silently retrying)
 * when the wait is interrupted by a signal.  A blocking IPC client uses this so
 * a SIGINT (Ctrl-C) unblocks it promptly and it can forward a cancel to the
 * server, rather than being stuck in the EINTR-retry loop above. */
int ray_sock_wait_readable_intr(ray_sock_t s, int timeout_ms)
{
    struct pollfd pfd = { .fd = s, .events = POLLIN };
#ifdef RAY_OS_WINDOWS
    int n = WSAPoll(&pfd, 1, timeout_ms);
#else
    int n = poll(&pfd, 1, timeout_ms);
#endif
    if (n < 0) return (errno == EINTR) ? -2 : -1;
    return n == 0 ? 0 : 1;
}

/* Send one byte of TCP urgent (out-of-band) data.  A client uses this to signal
 * a cancel to a server that is busy evaluating and therefore not reading the
 * normal stream: OOB data raises SIGURG on the peer even mid-compute. */
int ray_sock_send_oob(ray_sock_t s, char byte)
{
    return (int)send(s, &byte, 1, MSG_OOB);
}

/* Route SIGURG for out-of-band data on this socket to our process, so a peer's
 * urgent-data cancel raises SIGURG here even while we are busy evaluating and
 * not reading the normal stream.  POSIX only; a no-op elsewhere. */
void ray_sock_set_oob_owner(ray_sock_t s)
{
#ifndef RAY_OS_WINDOWS
    fcntl((int)s, F_SETOWN, getpid());
#else
    (void)s;
#endif
}

/* Consume one pending TCP urgent (out-of-band) byte on a specific socket.
 * Returns 1 if an urgent byte was present (and taken), 0 otherwise.  Used to
 * ask "did THIS connection request a cancel?" — so a cancel from one client
 * can never affect another client's query.  Async-signal-safe (recv). */
int ray_sock_take_oob(ray_sock_t s)
{
#ifndef RAY_OS_WINDOWS
    char b;
    return recv((int)s, &b, 1, MSG_OOB) == 1 ? 1 : 0;
#else
    (void)s;
    return 0;
#endif
}

void ray_sock_close(ray_sock_t s)
{
    if (s == RAY_INVALID_SOCK) return;
#ifdef RAY_OS_WINDOWS
    closesocket(s);
#else
    close(s);
#endif
}

ray_err_t ray_sock_set_nonblocking(ray_sock_t s)
{
#ifdef RAY_OS_WINDOWS
    u_long mode = 1;
    if (ioctlsocket(s, FIONBIO, &mode) != 0)
        return RAY_ERR_IO;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return RAY_ERR_IO;
    if (fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0)
        return RAY_ERR_IO;
#endif
    return RAY_OK;
}

ray_err_t ray_sock_set_blocking(ray_sock_t s)
{
#ifdef RAY_OS_WINDOWS
    u_long mode = 0;
    if (ioctlsocket(s, FIONBIO, &mode) != 0)
        return RAY_ERR_IO;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return RAY_ERR_IO;
    if (fcntl(s, F_SETFL, flags & ~O_NONBLOCK) < 0)
        return RAY_ERR_IO;
#endif
    return RAY_OK;
}
