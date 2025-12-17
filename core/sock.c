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

#include <errno.h>
#include <stdlib.h>
#if defined(OS_WINDOWS)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif
#include "sock.h"
#include "string.h"
#include "log.h"
#include "ops.h"

i64_t sock_addr_from_str(str_p str, i64_t len, sock_addr_t *addr) {
    i64_t r;
    str_p tok;

    // Check for NULL pointers
    if (str == NULL || addr == NULL)
        return -1;

    // Get host part
    tok = (str_p)memchr(str, ':', len);
    if (tok == NULL)
        return -1;

    if (tok - str >= ISIZEOF(addr->ip))
        return -1;

    memcpy(addr->ip, str, tok - str);
    addr->ip[tok - str] = '\0';
    tok++;

    // Get port part
    len -= tok - str;
    r = i64_from_str(tok, len, &addr->port);

    if (r != len)
        return -1;

    return 0;
}

#if defined(OS_WINDOWS)

i64_t sock_set_nonblocking(i64_t fd, b8_t flag) {
    u_long mode = flag ? 1 : 0;  // 1 to set non-blocking, 0 to set blocking
    if (ioctlsocket(fd, FIONBIO, &mode) != 0)
        return -1;

    return 0;
}

i64_t sock_open(sock_addr_t *addr, i64_t timeout) {
    SOCKET fd = INVALID_SOCKET;
    struct addrinfo hints, *result = NULL, *rp;
    i32_t code;
    i32_t last_error = 0;
    struct timeval tm;
    char port_str[16];

    fprintf(stderr, "[DEBUG] sock_open: addr=%s port=%lld timeout=%lld\n", addr->ip, addr->port, timeout);
    fflush(stderr);

    // Convert port to string for getaddrinfo
    _snprintf(port_str, sizeof(port_str), "%lld", addr->port);

    // Set up hints for getaddrinfo
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;      // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;  // TCP socket
    hints.ai_flags = 0;
    hints.ai_protocol = 0;  // Any protocol

    // Get address info
    code = getaddrinfo(addr->ip, port_str, &hints, &result);
    fprintf(stderr, "[DEBUG] sock_open: getaddrinfo returned %d\n", code);
    fflush(stderr);
    if (code != 0) {
        code = WSAGetLastError();
        LOG_ERROR("Failed to resolve hostname %s: %d", addr->ip, code);
        WSASetLastError(code);
        return -1;
    }

    // Try each address until we successfully connect
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        fprintf(stderr, "[DEBUG] sock_open: trying family=%d socktype=%d protocol=%d\n", 
                rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        fflush(stderr);
        
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == INVALID_SOCKET) {
            last_error = WSAGetLastError();
            fprintf(stderr, "[DEBUG] sock_open: socket() failed, error=%d\n", last_error);
            fflush(stderr);
            continue;
        }
        fprintf(stderr, "[DEBUG] sock_open: socket created fd=%lld\n", (i64_t)fd);
        fflush(stderr);

        // Set timeout for connect operation
        if (timeout > 0) {
            tm.tv_sec = timeout / 1000;
            tm.tv_usec = (timeout % 1000) * 1000;
            if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tm, sizeof(tm)) == SOCKET_ERROR) {
                last_error = WSAGetLastError();
                fprintf(stderr, "[DEBUG] sock_open: SO_RCVTIMEO failed, error=%d\n", last_error);
                fflush(stderr);
                closesocket(fd);
                continue;
            }
            if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tm, sizeof(tm)) == SOCKET_ERROR) {
                last_error = WSAGetLastError();
                fprintf(stderr, "[DEBUG] sock_open: SO_SNDTIMEO failed, error=%d\n", last_error);
                fflush(stderr);
                closesocket(fd);
                continue;
            }
        }

        // Try to connect
        fprintf(stderr, "[DEBUG] sock_open: connecting...\n");
        fflush(stderr);
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) != SOCKET_ERROR) {
            fprintf(stderr, "[DEBUG] sock_open: connect succeeded!\n");
            fflush(stderr);
            break;  // Success
        }

        last_error = WSAGetLastError();
        fprintf(stderr, "[DEBUG] sock_open: connect failed, error=%d\n", last_error);
        fflush(stderr);
        closesocket(fd);
        fd = INVALID_SOCKET;
    }

    freeaddrinfo(result);

    if (fd == INVALID_SOCKET) {
        LOG_ERROR("Could not connect to %s:%lld: %d", addr->ip, addr->port, last_error);
        WSASetLastError(last_error);
        return -1;
    }

    return (i64_t)fd;
}

i64_t sock_accept(i64_t fd) {
    struct sockaddr_in addr;
    struct linger linger_opt;
    socklen_t len = sizeof(addr);
    SOCKET acc_fd;
    char ip_str[INET_ADDRSTRLEN];

    acc_fd = accept((SOCKET)fd, (struct sockaddr *)&addr, &len);
    if (acc_fd == INVALID_SOCKET)
        return -1;
    if (sock_set_nonblocking(acc_fd, 1) == SOCKET_ERROR) {
        i32_t wsa_err = WSAGetLastError();
        closesocket(acc_fd);
        WSASetLastError(wsa_err);
        return -1;
    }

    linger_opt.l_onoff = 1;   // Enable SO_LINGER
    linger_opt.l_linger = 0;  // Timeout in seconds (0 means terminate immediately)

    // Apply the linger option to the accepted socket
    if (setsockopt(acc_fd, SOL_SOCKET, SO_LINGER, (const char *)&linger_opt, sizeof(linger_opt)) < 0) {
        LOG_ERROR("Failed to set SO_LINGER on accepted socket: %s", strerror(errno));
        closesocket(acc_fd);
        return -1;
    }

    inet_ntop(AF_INET, &addr.sin_addr, ip_str, INET_ADDRSTRLEN);
    LOG_DEBUG("Accepted new connection on fd %lld from %s:%d", acc_fd, ip_str, ntohs(addr.sin_port));
    return (i64_t)acc_fd;
}

i64_t sock_listen(i64_t port) {
    struct sockaddr_in addr;
    SOCKET fd;
    c8_t opt = 1;
    i32_t code;

    LOG_INFO("Starting socket listener on port %lld", port);

    fd = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (fd == INVALID_SOCKET)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        code = WSAGetLastError();
        closesocket(fd);
        WSASetLastError(code);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        code = WSAGetLastError();
        closesocket(fd);
        WSASetLastError(code);
        return -1;
    }
    if (listen(fd, SOMAXCONN) == -1) {
        code = WSAGetLastError();
        closesocket(fd);
        WSASetLastError(code);
        return -1;
    }

    LOG_DEBUG("Socket listener started successfully on fd %lld", fd);
    return (i64_t)fd;
}

i64_t sock_close(i64_t fd) {
    LOG_DEBUG("Closing socket fd %lld", fd);
    return closesocket((SOCKET)fd);
}

i64_t sock_recv(i64_t fd, u8_t *buf, i64_t size) {
    i64_t sz = recv(fd, (str_p)buf, size, MSG_NOSIGNAL);

    switch (sz) {
        case -1:
            if ((WSAGetLastError() == ERROR_IO_PENDING) || (errno == EAGAIN || errno == EWOULDBLOCK))
                return 0;
            else {
                LOG_ERROR("Failed to receive data on fd %lld: %s", fd, strerror(errno));
                return -1;
            }

        case 0:
            LOG_DEBUG("Connection closed by peer on fd %lld", fd);
            return -1;

        default:
            LOG_TRACE("Received %lld bytes on fd %lld", sz, fd);
            return sz;
    }
}

i64_t sock_send(i64_t fd, u8_t *buf, i64_t size) {
    i64_t sz, total = 0;

send:
    sz = send(fd, (const char *)(buf + total), size - total, MSG_NOSIGNAL);
    switch (sz) {
        case -1:
            if (errno == EINTR)
                goto send;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // If we've sent some data, return what we've sent so far
                if (total > 0)
                    return total;
                return 0;
            } else {
                LOG_ERROR("Failed to send data on fd %lld: %s", fd, strerror(errno));
                return -1;
            }

        case 0:
            // If we've sent some data, return what we've sent so far
            if (total > 0)
                return total;
            return -1;

        default:
            total += sz;
            if (total < size)
                goto send;
            LOG_TRACE("Sent %lld bytes on fd %lld", total, fd);
            return total;
    }
}

#else

i64_t sock_set_nonblocking(i64_t fd, b8_t flag) {
    i64_t flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;

    if (flag)
        flags |= O_NONBLOCK;
    else
        flags &= ~O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags) == -1)
        return -1;

    return 0;
}

i64_t sock_open(sock_addr_t *addr, i64_t timeout) {
    i64_t fd = -1;
    struct addrinfo hints, *result, *rp;
    struct linger linger_opt;
    struct timeval tm;
    char port_str[16];

    // Convert port to string for getaddrinfo
    snprintf(port_str, sizeof(port_str), "%lld", addr->port);

    // Set up hints for getaddrinfo
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;      // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;  // TCP socket
    hints.ai_flags = 0;
    hints.ai_protocol = 0;  // Any protocol

    // Get address info
    if (getaddrinfo(addr->ip, port_str, &hints, &result) != 0) {
        LOG_ERROR("Failed to resolve hostname %s: %s", addr->ip, strerror(errno));
        return -1;
    }

    // Try each address until we successfully connect
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1)
            continue;

        // Set timeout
        tm.tv_sec = timeout;  // Timeout in seconds
        tm.tv_usec = 0;       // Timeout in microseconds
        if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tm, sizeof(tm)) < 0) {
            close(fd);
            continue;
        }

        // Set the behavior of a socket when it is closed using close()
        linger_opt.l_onoff = 1;   // Enable SO_LINGER
        linger_opt.l_linger = 0;  // Timeout in seconds (0 means terminate immediately)
        if (setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger_opt, sizeof(linger_opt))) {
            close(fd);
            continue;
        }

        // Try to connect
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) != -1)
            break;  // Success

        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);

    if (fd == -1) {
        LOG_ERROR("Could not connect to %s:%lld: %s", addr->ip, addr->port, strerror(errno));
        return -1;
    }

    return fd;
}

i64_t sock_accept(i64_t fd) {
    struct sockaddr_in addr;
    struct linger linger_opt;
    socklen_t len = sizeof(addr);
    i64_t acc_fd;

    acc_fd = accept(fd, (struct sockaddr *)&addr, &len);
    if (acc_fd == -1) {
        LOG_ERROR("Failed to accept connection: %s", strerror(errno));
        return -1;
    }
    if (sock_set_nonblocking(acc_fd, B8_TRUE) == -1)
        return -1;

    linger_opt.l_onoff = 1;   // Enable SO_LINGER
    linger_opt.l_linger = 0;  // Timeout in seconds (0 means terminate immediately)

    // Apply the linger option to the accepted socket
    if (setsockopt(acc_fd, SOL_SOCKET, SO_LINGER, &linger_opt, sizeof(linger_opt)) < 0) {
        LOG_ERROR("Failed to set SO_LINGER on accepted socket: %s", strerror(errno));
        close(acc_fd);
        return -1;
    }

    LOG_DEBUG("Accepted new connection on fd %lld from %s:%d", acc_fd, inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    return acc_fd;
}

i64_t sock_listen(i64_t port) {
    struct sockaddr_in addr;
    i64_t fd, opt = 1;

    LOG_INFO("Starting socket listener on port %lld", port);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("Failed to create socket: %s", strerror(errno));
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOG_ERROR("Failed to set socket options: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        LOG_ERROR("Failed to bind socket: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 5) == -1) {
        LOG_ERROR("Failed to listen on socket: %s", strerror(errno));
        close(fd);
        return -1;
    }

    LOG_DEBUG("Socket listener started successfully on fd %lld", fd);
    return fd;
}

i64_t sock_close(i64_t fd) {
    LOG_DEBUG("Closing socket fd %lld", fd);
    return close(fd);
}

i64_t sock_recv(i64_t fd, u8_t *buf, i64_t size) {
    i64_t sz;

recv:
    sz = recv(fd, (str_p)buf, size, MSG_NOSIGNAL);

    switch (sz) {
        case -1:
            if (errno == EINTR)
                goto recv;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            else {
                LOG_ERROR("Failed to receive data on fd %lld: %s", fd, strerror(errno));
                return -1;
            }

        case 0:
            LOG_DEBUG("Connection closed by peer on fd %lld", fd);
            return -1;

        default:
            LOG_TRACE("Received %lld bytes on fd %lld", sz, fd);
            return sz;
    }
}

i64_t sock_send(i64_t fd, u8_t *buf, i64_t size) {
    i64_t sz, total = 0;

send:
    sz = send(fd, buf + total, size - total, MSG_NOSIGNAL);
    switch (sz) {
        case -1:
            if (errno == EINTR)
                goto send;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // If we've sent some data, return what we've sent so far
                if (total > 0)
                    return total;
                return 0;
            } else {
                LOG_ERROR("Failed to send data on fd %lld: %s", fd, strerror(errno));
                return -1;
            }

        case 0:
            // If we've sent some data, return what we've sent so far
            if (total > 0)
                return total;
            return -1;

        default:
            total += sz;
            if (total < size)
                goto send;
            LOG_TRACE("Sent %lld bytes on fd %lld", total, fd);
            return total;
    }
}

#endif
