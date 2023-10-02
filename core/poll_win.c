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
#include <MSWSock.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "poll.h"
#include "string.h"
#include "hash.h"
#include "format.h"
#include "util.h"
#include "heap.h"

// Definitions and globals
#define STDIN_WAKER_ID ~0ull
#define STDIN_BUFFER_SIZE 256
#define MAX_IOCP_RESULTS 64

char_t _STDIN_BUFFER[STDIN_BUFFER_SIZE];

DWORD WINAPI StdinThread(LPVOID lpParam)
{
    HANDLE hCompletionPort = (HANDLE)lpParam;
    DWORD bytesRead;
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);

    while (true)
    {
        // Blocking read from stdin
        if (ReadFile(hStdin, _STDIN_BUFFER, STDIN_BUFFER_SIZE, &bytesRead, NULL))
        {
            _STDIN_BUFFER[bytesRead] = '\0';
            // Check for CTRL+C (ASCII code 3)
            if (bytesRead == 1 && _STDIN_BUFFER[0] == 0x03)
                PostQueuedCompletionStatus(hCompletionPort, 0, STDIN_WAKER_ID, NULL);
            else
                PostQueuedCompletionStatus(hCompletionPort, bytesRead, STDIN_WAKER_ID, NULL);
        }
    }

    return 0;
}

nil_t exit_werror()
{
    obj_t err;
    str_t fmt;

    err = sys_error(TYPE_WSAGETLASTERROR, "poll_init");
    fmt = obj_fmt(err);
    printf("%s\n", fmt);
    heap_free(fmt);
    drop(err);
    fflush(stdout);
    exit(1);
}

i64_t poll_accept(poll_t select, ipc_data_t data)
{
    i32_t code;
    LPFN_ACCEPTEX lpfnAcceptEx = NULL;
    GUID GuidAcceptEx = WSAID_ACCEPTEX;
    DWORD dwBytes;
    SOCKET sock_fd = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    ipc_data_t sock_data;

    if (sock_fd == INVALID_SOCKET)
        return -1;

    // Ensure the accept socket is associated with the IOCP.
    sock_data = poll_add(select, sock_fd);
    CreateIoCompletionPort((HANDLE)sock_fd, (HANDLE)select->poll_fd, (i64_t)sock_data, 0);

    // Load AcceptEx function
    if (WSAIoctl(select->ipc_fd, SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &GuidAcceptEx, sizeof(GuidAcceptEx),
                 &lpfnAcceptEx, sizeof(lpfnAcceptEx),
                 &dwBytes, NULL, NULL) == SOCKET_ERROR)
    {
        poll_del(select, sock_fd);
        code = WSAGetLastError();
        closesocket(sock_fd);
        WSASetLastError(code);
        return -1;
    }

    BOOL success = lpfnAcceptEx(select->ipc_fd, sock_fd,
                                data->rx.buf, 0,
                                sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16,
                                (LPDWORD)&data->rx.size, &data->rx.overlapped);
    if (!success)
    {
        code = WSAGetLastError();
        if (code != ERROR_IO_PENDING)
        {
            poll_del(select, sock_fd);
            code = WSAGetLastError();
            closesocket(sock_fd);
            WSASetLastError(code);
            return -1;
        }
    }

    data->tx.size = (i64_t)sock_fd;

    return (i64_t)sock_fd;
}

poll_t poll_init(i64_t port)
{
    WSADATA wsaData;
    i64_t ipc_fd = -1, poll_fd = -1;
    ipc_data_t data;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        exit_werror();

    poll_fd = (i64_t)CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);

    if (!poll_fd)
        exit_werror();

    // Create a thread to read from stdin
    CreateThread(NULL, 0, StdinThread, (HANDLE)poll_fd, 0, NULL);

    poll_t select = (poll_t)heap_alloc(sizeof(struct poll_t));

    select->poll_fd = poll_fd;

    if (port)
    {
        ipc_fd = sock_listen(port);
        if (ipc_fd == -1)
        {
            heap_free(select);
            exit_werror();
        }

        select->ipc_fd = ipc_fd;
        CreateIoCompletionPort((HANDLE)ipc_fd, (HANDLE)poll_fd, ipc_fd, 0);

        data = heap_alloc(sizeof(struct ipc_data_t));
        data->fd = ipc_fd;
        data->rx.buf = heap_alloc((sizeof(SOCKADDR_IN) + 16) * 2);
        data->rx.size = 0;
        data->next = NULL;
        memset(&data->rx.overlapped, 0, sizeof(data->rx.overlapped));
        select->data = data;

        if (poll_accept(select, data) == -1)
        {
            heap_free(select);
            heap_free(data);
            exit_werror();
        }
    }

    return select;
}

nil_t poll_cleanup(poll_t select)
{
    // Cleanup code
    CloseHandle((HANDLE)select->poll_fd);

    // TODO: free ipc_data_t linked list
    heap_free(select);

    WSACleanup();

    printf("\nBye.\n");
    fflush(stdout);
}

ipc_data_t poll_add(poll_t select, i64_t fd)
{
    ipc_data_t data = NULL;

    data = add_data(&select->data, fd, sizeof(header_t));

    CreateIoCompletionPort((HANDLE)fd, (HANDLE)select->poll_fd, (ULONG_PTR)data, 0);

    return data;
}

i64_t poll_del(poll_t select, i64_t fd)
{
    unused(select);
    unused(fd);
    return 0;
}

i64_t poll_dispatch(poll_t select)
{
    DWORD i, num = 5, size;
    OVERLAPPED *overlapped;
    HANDLE hPollFd = (HANDLE)select->poll_fd;
    OVERLAPPED_ENTRY events[MAX_IOCP_RESULTS];
    BOOL success;
    i64_t key, poll;
    obj_t res, v;
    str_t fmt;
    bool_t running = true;
    ipc_data_t data = NULL, sock_data = NULL;

    prompt();

    while (running)
    {
        success = GetQueuedCompletionStatusEx(
            hPollFd,
            events,
            MAX_IOCP_RESULTS,
            &num,
            INFINITE,
            TRUE // set this to TRUE if you want to return on alertable wait
        );

        // Handle IOCP events
        if (success)
        {
            for (i = 0; i < num; ++i)
            {
                key = events[i].lpCompletionKey;
                size = events[i].dwNumberOfBytesTransferred;
                overlapped = events[i].lpOverlapped;

                switch (key)
                {
                case STDIN_WAKER_ID:
                    if (size == 0)
                    {
                        running = false;
                        break;
                    }
                    res = eval_str(0, "stdin", _STDIN_BUFFER);
                    if (res)
                    {
                        fmt = obj_fmt(res);
                        printf("%s\n", fmt);
                        heap_free(fmt);
                        drop(res);
                    }

                    prompt();
                    break;

                default:
                    // Accept new connection
                    if (key == select->ipc_fd)
                    {
                        data = find_data(&select->data, key);
                        setsockopt((SOCKET)data->tx.size, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                                   (str_t)&select->ipc_fd, sizeof(select->ipc_fd));
                        sock_data = find_data(&select->data, data->tx.size);
                        if (sock_data == NULL)
                        {
                            poll_accept(select, data);
                            break;
                        }
                        poll = poll_recv(select, sock_data);
                        poll_accept(select, data);
                        if (poll == POLL_ERROR)
                        {
                            poll_del(select, key);
                            res = sys_error(TYPE_WSAGETLASTERROR, "ipc");
                            fmt = obj_fmt(res);
                            printf("%s\n", fmt);
                            heap_free(fmt);
                            drop(res);
                        }
                        else if (poll == POLL_READY)
                        {
                            res = de_raw(sock_data->rx.buf, sock_data->rx.size);
                            sock_data->rx.read_size = 0;
                            sock_data->rx.size = 0;
                            heap_free(sock_data->rx.buf);

                            // sync || async
                            if (data->msgtype < 2)
                            {
                                if (res->type == TYPE_CHAR)
                                {
                                    v = eval_str(0, "ipc", as_string(res));
                                    drop(res);
                                }
                                else
                                    v = eval_obj(0, "ipc", res);

                                // sync request
                                if (data->msgtype == MSG_TYPE_SYNC)
                                {
                                    ipc_enqueue_msg(data, v, MSG_TYPE_RESP);
                                    poll = poll_send(select, data);

                                    if (poll == POLL_ERROR)
                                        perror("send reply");
                                }
                                else
                                    drop(v);
                            }
                        }
                        break;
                    }

                    data = (ipc_data_t)key;

                    if (size == 0)
                    {
                        // Connection closed
                        remove_data(&select->data, data->fd);
                        break;
                    }

                    // recv
                    if (overlapped == &(data->rx.overlapped))
                    {
                        sock_data->rx.read_size += size;
                        poll = poll_recv(select, sock_data);
                        if (poll == POLL_ERROR)
                        {
                            poll_del(select, key);
                            res = sys_error(TYPE_WSAGETLASTERROR, "ipc");
                            fmt = obj_fmt(res);
                            printf("%s\n", fmt);
                            heap_free(fmt);
                            drop(res);
                        }
                        else if (poll == POLL_READY)
                        {
                            res = de_raw(data->rx.buf, data->rx.size);
                            data->rx.read_size = 0;
                            data->rx.size = 0;
                            heap_free(data->rx.buf);

                            // sync || async
                            if (data->msgtype < 2)
                            {
                                if (res->type == TYPE_CHAR)
                                {
                                    v = eval_str(0, "ipc", as_string(res));
                                    drop(res);
                                }
                                else
                                    v = eval_obj(0, "ipc", res);

                                // sync request
                                if (data->msgtype == MSG_TYPE_SYNC)
                                {
                                    ipc_enqueue_msg(data, v, MSG_TYPE_RESP);
                                    poll = poll_send(select, data);

                                    if (poll == POLL_ERROR)
                                        perror("send reply");
                                }
                                else
                                    drop(v);
                            }
                        }
                    }
                    // send
                    else if (overlapped == &(data->tx.overlapped))
                    {
                        poll = poll_send(select, data);

                        if (poll == POLL_ERROR)
                            perror("send reply");
                    }

                    break;
                }
            }
        }
        else
        {
            res = sys_error(TYPE_WSAGETLASTERROR, "poll_init");
            fmt = obj_fmt(res);
            printf("%s\n", fmt);
            heap_free(fmt);
            drop(res);
        }
    }

    return 0;
}

i64_t poll_recv(poll_t select, ipc_data_t data)
{
    unused(select);

    WSABUF wsaBuf;
    DWORD flags, size = 0;
    i32_t error, result;

    if (data->rx.size == 0)
    {
        data->rx.size = 128;
        data->rx.buf = heap_alloc(data->rx.size);
    }

    // Read handshake first
    if (data->rx.version == 0)
    {
        flags = 0;

        while (data->rx.read_size == 0 || data->rx.buf[data->rx.read_size - 1] != '\0')
        {
            wsaBuf.buf = (char_t *)&data->rx.buf[data->rx.read_size];
            wsaBuf.len = 1;
            result = WSARecv(data->fd, &wsaBuf, 1, &size, &flags, &data->rx.overlapped, NULL);

            if (result == SOCKET_ERROR)
            {
                error = WSAGetLastError();
                if (error == ERROR_IO_PENDING)
                    return POLL_PENDING;

                return POLL_ERROR;
            }

            data->rx.read_size += size;
        }

        // read version after creds
        if (size == 0)
        {
            wsaBuf.buf = (char_t *)&data->rx.buf[data->rx.read_size];
            wsaBuf.len = 1;
            result = WSARecv(data->fd, &wsaBuf, 1, &size, &flags, &data->rx.overlapped, NULL);
            if (result == SOCKET_ERROR)
            {
                error = WSAGetLastError();
                if (error == ERROR_IO_PENDING)
                    return POLL_PENDING;

                return POLL_ERROR;
            }

            data->rx.read_size += size;
        }

        data->rx.version = data->rx.buf[0];
        data->rx.read_size = 0;
        data->rx.size = 0;
    }

    // read header
    if (data->rx.read_size == 0)
    {
        wsaBuf.buf = (char_t *)data->rx.buf;
        wsaBuf.len = sizeof(header_t);
        flags = MSG_WAITALL;
        result = WSARecv(data->fd, &wsaBuf, 1, &size, &flags, &data->rx.overlapped, NULL);

        if (result == SOCKET_ERROR)
        {
            error = WSAGetLastError();
            if (error == ERROR_IO_PENDING)
                return POLL_PENDING;

            return POLL_ERROR;
        }
    }

    // parse header
    if (data->rx.size == 0)
    {
        data->msgtype = ((header_t *)data->rx.buf)->msgtype;
        data->rx.size = ((header_t *)data->rx.buf)->size;
        data->rx.buf = heap_realloc(data->rx.buf, data->rx.size);
    }

    wsaBuf.buf = (char_t *)data->rx.buf;
    wsaBuf.len = data->rx.size;
    flags = MSG_WAITALL;
    result = WSARecv(data->fd, &wsaBuf, 1, &size, &flags, &data->rx.overlapped, NULL);
    if (result == SOCKET_ERROR)
    {
        error = WSAGetLastError();
        if (error == ERROR_IO_PENDING)
            return POLL_PENDING;

        return POLL_ERROR;
    }

    return POLL_READY;
}

i64_t poll_send(poll_t select, ipc_data_t data)
{
    unused(select);

    i64_t size;
    obj_t obj;
    nil_t *v;
    i8_t msg_type = MSG_TYPE_RESP;
    WSABUF wsaBuf;
    DWORD flags = MSG_WAITALL, dsize = 0;
    i32_t result;

send:
    if (data->tx.size == 0)
    {
        v = queue_pop(&data->tx.queue);

        if (v)
        {
            obj = (obj_t)((i64_t)v & ~(3ll << 61));
            msg_type = (((i64_t)v & (3ll << 61)) >> 61);
            size = ser_raw(&data->tx.buf, obj);
            drop(obj);
            if (size == -1)
                return POLL_ERROR;

            data->tx.size = size;
            data->tx.write_size = 0;
            ((header_t *)data->tx.buf)->msgtype = msg_type;
            goto send;
        }
    }

    if (data->tx.write_size == 0)
    {
        wsaBuf.buf = (char_t *)data->rx.buf;
        wsaBuf.len = data->rx.size;
        result = WSASend(data->fd, &wsaBuf, 1, &dsize, flags, &data->tx.overlapped, NULL);

        if (result == SOCKET_ERROR)
            return POLL_ERROR;

        heap_free(data->tx.buf);

        data->tx.write_size = 0;
        data->tx.size = 0;
    }

    return 0;
}

ipc_data_t add_data(ipc_data_t *head, i32_t fd, i32_t size)
{
    ipc_data_t data;

    data = heap_alloc(sizeof(struct ipc_data_t));
    data->fd = fd;
    data->rx.version = 0;
    if (size < 1)
        data->rx.buf = NULL;
    else
        data->rx.buf = heap_alloc(size);
    data->rx.read_size = 0;
    data->rx.size = 0;
    data->tx.write_size = 0;
    data->tx.size = 0;
    data->tx.buf = NULL;
    data->next = *head;
    *head = data;
    if (size == -1)
        data->tx.queue = (queue_t){0};
    else
        data->tx.queue = queue_new(TX_QUEUE_SIZE);

    return data;
}

nil_t remove_data(ipc_data_t *head, i64_t fd)
{
    ipc_data_t *next, data;
    nil_t *v;

    next = head;
    while (*next)
    {
        if ((*next)->fd == fd)
        {
            data = *next;
            *next = data->next;
            sock_close(data->fd);
            heap_free(data->rx.buf);
            heap_free(data->tx.buf);

            while ((v = queue_pop(&data->tx.queue)))
                drop((obj_t)((i64_t)v & ~(3ll << 61)));

            queue_free(&data->tx.queue);
            heap_free(data);

            return;
        }
        next = &(*next)->next;
    }
}
