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
  #include <netinet/tcp.h>
  #include <poll.h>
  #include <unistd.h>
#endif

/* ===== Socket Implementation ===== */

ray_sock_t ray_sock_listen(uint16_t port)
{
    ray_sock_t fd = (ray_sock_t)socket(AF_INET, SOCK_STREAM, 0);
    if (fd == RAY_INVALID_SOCK) return RAY_INVALID_SOCK;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
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

ray_sock_t ray_sock_connect(const char* host, uint16_t port, int timeout_ms)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
        return RAY_INVALID_SOCK;

    ray_sock_t fd = (ray_sock_t)socket(res->ai_family, res->ai_socktype,
                                        res->ai_protocol);
    if (fd == RAY_INVALID_SOCK) {
        freeaddrinfo(res);
        return RAY_INVALID_SOCK;
    }

    /* Set send/recv timeout if requested */
    if (timeout_ms > 0) {
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
    }

    if (connect(fd, res->ai_addr, (socklen_t)res->ai_addrlen) < 0) {
        ray_sock_close(fd);
        freeaddrinfo(res);
        return RAY_INVALID_SOCK;
    }
    freeaddrinfo(res);

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
