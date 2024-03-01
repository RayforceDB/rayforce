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

#if defined(_WIN32) || defined(__CYGWIN__)
#include <winsock2.h>
#include <ws2tcpip.h>
#define MSG_NOSIGNAL 0
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#endif
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include "sock.h"
#include "string.h"
#include "util.h"

i64_t sock_addr_from_str(str_p addr_str, sock_addr_t *addr)
{
    char_t temp_str[256]; // Temporary string for tokenization
    str_p token;

    // Check for NULL pointers
    if (addr_str == NULL || addr == NULL)
        return -1;

    strncpy(temp_str, addr_str, sizeof(temp_str) - 1);
    temp_str[sizeof(temp_str) - 1] = '\0'; // Ensure null termination

    // Get IP part
    token = strtok(temp_str, ":");
    if (token == NULL)
        return -1;

    strncpy(addr->ip, token, sizeof(addr->ip) - 1);
    addr->ip[sizeof(addr->ip) - 1] = '\0'; // Ensure null termination

    // Get port part
    token = strtok(NULL, ":");
    if (token == NULL)
        return -1;

    addr->port = atoi(token);

    return 0;
}

#if defined(_WIN32) || defined(__CYGWIN__)

i64_t sock_set_nonblocking(i64_t fd, b8_t flag)
{
    u_long mode = flag ? 1 : 0; // 1 to set non-blocking, 0 to set blocking
    if (ioctlsocket(fd, FIONBIO, &mode) != 0)
        return -1;

    return 0;
}

i64_t sock_open(sock_addr_t *addr)
{
    SOCKET fd;
    struct sockaddr_in addrin;
    i32_t code;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET)
        return -1;

    memset(&addrin, 0, sizeof(addrin));
    addrin.sin_family = AF_INET;
    addrin.sin_port = htons(addr->port);
    addrin.sin_addr.s_addr = inet_addr(addr->ip);

    if (connect(fd, (struct sockaddr *)&addrin, sizeof(addrin)) == SOCKET_ERROR)
    {
        code = WSAGetLastError();
        closesocket(fd);
        WSASetLastError(code);
        return -1;
    }

    return (i64_t)fd;
}

i64_t sock_accept(i64_t fd)
{
    struct sockaddr_in addr;
    i32_t code, len = sizeof(addr);
    SOCKET acc_fd;

    acc_fd = accept((SOCKET)fd, (struct sockaddr *)&addr, &len);
    if (acc_fd == INVALID_SOCKET)
        return -1;
    if (sock_set_nonblocking(acc_fd, 1) == SOCKET_ERROR)
    {
        code = WSAGetLastError();
        closesocket(acc_fd);
        WSASetLastError(code);
        return -1;
    }

    return (i64_t)acc_fd;
}

i64_t sock_listen(i64_t port)
{
    struct sockaddr_in addr;
    SOCKET fd;
    char_t opt = 1;
    i32_t code;

    fd = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (fd == INVALID_SOCKET)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        code = WSAGetLastError();
        closesocket(fd);
        WSASetLastError(code);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
    {
        code = WSAGetLastError();
        closesocket(fd);
        WSASetLastError(code);
        return -1;
    }
    if (listen(fd, SOMAXCONN) == -1)
    {
        code = WSAGetLastError();
        closesocket(fd);
        WSASetLastError(code);
        return -1;
    }

    return (i64_t)fd;
}

i64_t sock_close(i64_t fd)
{
    return closesocket((SOCKET)fd);
}

i64_t sock_recv(i64_t fd, u8_t *buf, i64_t size)
{
    i64_t sz = recv(fd, (str_p)buf, size, MSG_NOSIGNAL);

    switch (sz)
    {
    case -1:
        if ((WSAGetLastError() == ERROR_IO_PENDING) || (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        else
            return -1;

    case 0:
        return -1;

    default:
        return sz;
    }
}

i64_t sock_send(i64_t fd, u8_t *buf, i64_t size)
{
    i64_t sz = send(fd, (str_p)buf, size, MSG_NOSIGNAL);

    switch (sz)
    {
    case -1:
        if ((WSAGetLastError() == ERROR_IO_PENDING) || (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        else
            return -1;

    case 0:
        return -1;

    default:
        return sz;
    }
}

#else

i64_t sock_set_nonblocking(i64_t fd, b8_t flag)
{
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

i64_t sock_open(sock_addr_t *addr)
{
    i64_t fd;
    struct sockaddr_in addrin;
    struct linger linger_opt;

    fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd == -1)
        return -1;

    memset(&addrin, 0, sizeof(addrin));
    addrin.sin_family = AF_INET;
    addrin.sin_port = htons(addr->port);
    inet_pton(AF_INET, addr->ip, &addrin.sin_addr);

    if (connect(fd, (struct sockaddr *)&addrin, sizeof(addrin)) == -1)
        return -1;

    linger_opt.l_onoff = 1;  // Enable SO_LINGER
    linger_opt.l_linger = 0; // Timeout in seconds (0 means terminate immediately)

    // Apply the linger option to the socket
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger_opt, sizeof(linger_opt));

    return fd;
}

i64_t sock_accept(i64_t fd)
{
    struct sockaddr_in addr;
    struct linger linger_opt;
    socklen_t len = sizeof(addr);
    i64_t acc_fd;

    acc_fd = accept(fd, (struct sockaddr *)&addr, &len);
    if (acc_fd == -1)
        return -1;
    if (sock_set_nonblocking(acc_fd, B8_TRUE) == -1)
        return -1;

    linger_opt.l_onoff = 1;  // Enable SO_LINGER
    linger_opt.l_linger = 0; // Timeout in seconds (0 means terminate immediately)

    // Apply the linger option to the socket
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger_opt, sizeof(linger_opt));

    return acc_fd;
}

i64_t sock_listen(i64_t port)
{
    struct sockaddr_in addr;
    i64_t fd, opt = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        return -1;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
        return -1;
    if (listen(fd, 5) == -1)
        return -1;

    return fd;
}

i64_t sock_close(i64_t fd)
{
    return close(fd);
}

i64_t sock_recv(i64_t fd, u8_t *buf, i64_t size)
{
    i64_t sz;

recv:
    sz = recv(fd, (str_p)buf, size, MSG_NOSIGNAL);

    switch (sz)
    {
    case -1:
        if (errno == EINTR)
            goto recv;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        else
            return -1;

    case 0:
        return -1;

    default:
        return sz;
    }
}

i64_t sock_send(i64_t fd, u8_t *buf, i64_t size)
{
    i64_t sz;
send:
    sz = send(fd, (str_p)buf, size, MSG_NOSIGNAL);
    switch (sz)
    {
    case -1:
        if (errno == EINTR)
            goto send;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        else
            return -1;

    case 0:
        return -1;

    default:
        return sz;
    }
}

#endif
