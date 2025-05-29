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

#include "def.h"
#include <stdio.h>
#include <stdlib.h>
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>

// Define STDOUT_FILENO for Windows if not already defined
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif

#include "rayforce.h"
#include "poll.h"
#include "string.h"
#include "hash.h"
#include "format.h"
#include "util.h"
#include "sock.h"
#include "heap.h"
#include "io.h"
#include "error.h"
#include "symbols.h"
#include "eval.h"
#include "sys.h"
#include "chrono.h"
#include "binary.h"

// Link with Ws2_32.lib
#pragma comment(lib, "Ws2_32.lib")
// Link with Mswsock.lib
#pragma comment(lib, "Mswsock.lib")

// Global IOCP handle
HANDLE g_iocp = INVALID_HANDLE_VALUE;

// Definitions and globals
#define STDIN_WAKER_ID ~0ull
#define MAX_IOCP_RESULTS 64

typedef struct listener_t {
    u8_t buf[(sizeof(SOCKADDR_IN) + 16) * 2];
    OVERLAPPED overlapped;
    DWORD dwBytes;
    SOCKET hAccepted;
} *listener_p;

typedef struct stdin_thread_ctx_t {
    HANDLE h_cp;
    term_p term;
} *stdin_thread_ctx_p;

listener_p __LISTENER = NULL;
stdin_thread_ctx_p __STDIN_THREAD_CTX = NULL;

#define _RECV_OP(poll, selector)                                                                               \
    {                                                                                                          \
        i32_t poll_result;                                                                                     \
                                                                                                               \
        poll_result = WSARecv(selector->fd, &selector->rx.wsa_buf, 1, &selector->rx.size, &selector->rx.flags, \
                              &selector->rx.overlapped, NULL);                                                 \
                                                                                                               \
        if (poll_result == SOCKET_ERROR) {                                                                     \
            if (WSAGetLastError() == ERROR_IO_PENDING)                                                         \
                return POLL_OK;                                                                                \
                                                                                                               \
            return POLL_ERROR;                                                                                 \
        }                                                                                                      \
    }

#define _SEND_OP(poll, selector)                                                                              \
    {                                                                                                         \
        i32_t poll_result;                                                                                    \
                                                                                                              \
        poll_result = WSASend(selector->fd, &selector->tx.wsa_buf, 1, &selector->tx.size, selector->tx.flags, \
                              &selector->tx.overlapped, NULL);                                                \
                                                                                                              \
        if (poll_result == SOCKET_ERROR) {                                                                    \
            if (WSAGetLastError() == ERROR_IO_PENDING)                                                        \
                return POLL_OK;                                                                               \
                                                                                                              \
            return POLL_ERROR;                                                                                \
        }                                                                                                     \
    }

DWORD WINAPI StdinThread(LPVOID prm) {
    stdin_thread_ctx_p ctx = (stdin_thread_ctx_p)prm;
    term_p term = ctx->term;
    HANDLE h_cp = ctx->h_cp;
    DWORD bytes;

    for (;;) {
        bytes = (DWORD)term_getc(term);
        if (bytes == 0)
            break;

        PostQueuedCompletionStatus(h_cp, bytes, STDIN_WAKER_ID, NULL);
    }

    PostQueuedCompletionStatus(h_cp, 0, STDIN_WAKER_ID, NULL);

    return 0;
}

nil_t exit_werror() {
    obj_p fmt, err;

    err = sys_error(ERROR_TYPE_SOCK, "poll_init");
    fmt = obj_fmt(err, B8_TRUE);
    printf("%s\n", AS_C8(fmt));
    drop_obj(fmt);
    drop_obj(err);
    fflush(stdout);
    exit(1);
}

i64_t poll_accept(poll_p poll) {
    i32_t code;
    LPFN_ACCEPTEX lpfnAcceptEx = NULL;
    GUID GuidAcceptEx = WSAID_ACCEPTEX;
    DWORD dwBytes;
    SOCKET sock_fd = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    b8_t success;

    if (sock_fd == INVALID_SOCKET)
        return -1;

    // Ensure the accept socket is associated with the IOCP.
    // CreateIoCompletionPort((HANDLE)sock_fd, (HANDLE)poll->poll_fd, 0, 0);

    // Load AcceptEx function
    if (WSAIoctl(poll->ipc_fd, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidAcceptEx, sizeof(GuidAcceptEx), &lpfnAcceptEx,
                 sizeof(lpfnAcceptEx), &dwBytes, NULL, NULL) == SOCKET_ERROR) {
        code = WSAGetLastError();
        closesocket(sock_fd);
        WSASetLastError(code);
        return -1;
    }

    success = lpfnAcceptEx(poll->ipc_fd, sock_fd, __LISTENER->buf, 0, sizeof(SOCKADDR_IN) + 16,
                           sizeof(SOCKADDR_IN) + 16, &__LISTENER->dwBytes, &__LISTENER->overlapped);
    if (!success) {
        code = WSAGetLastError();
        if (code != ERROR_IO_PENDING) {
            code = WSAGetLastError();
            closesocket(sock_fd);
            WSASetLastError(code);
            return -1;
        }
    }

    __LISTENER->hAccepted = sock_fd;

    return (i64_t)sock_fd;
}

/**
 * Initialize the IOCP polling system
 * @param port The port to listen on
 * @return A poll_t structure or NULL on failure
 */
poll_p poll_init(i64_t port) {
    i64_t listen_fd = -1;
    poll_p poll;
    WSADATA wsaData;
    int result;

    // Initialize Winsock
    result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", result);
        return NULL;
    }

    // Create IOCP
    g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (g_iocp == NULL) {
        fprintf(stderr, "CreateIoCompletionPort failed: %d\n", GetLastError());
        WSACleanup();
        return NULL;
    }

    poll = (poll_p)heap_alloc(sizeof(struct poll_t));
    if (poll == NULL) {
        CloseHandle(g_iocp);
        WSACleanup();
        return NULL;
    }

    poll->code = NULL_I64;
    poll->poll_fd = (i64_t)g_iocp;
    poll->ipc_fd = -1;
    poll->replfile = string_from_str("repl", 4);
    poll->ipcfile = string_from_str("ipc", 3);
    poll->term = term_create();
    poll->selectors = freelist_create(128);
    poll->timers = timers_create(16);

    // Add server socket if port is specified
    if (port) {
        listen_fd = poll_listen(poll, port);
        if (listen_fd == -1) {
            fprintf(stderr, "Failed to listen on port %lld\n", port);
            poll_destroy(poll);
            return NULL;
        }
    }

    __LISTENER = (listener_p)heap_alloc(sizeof(struct listener_t));
    memset(__LISTENER, 0, sizeof(struct listener_t));

    if (poll_accept(poll) == -1) {
        heap_free(poll);
        exit_werror();
    }

    __STDIN_THREAD_CTX = (stdin_thread_ctx_p)heap_alloc(sizeof(struct stdin_thread_ctx_t));
    __STDIN_THREAD_CTX->h_cp = (HANDLE)poll->poll_fd;
    __STDIN_THREAD_CTX->term = poll->term;

    // Create a thread to read from stdin
    CreateThread(NULL, 0, StdinThread, (LPVOID)__STDIN_THREAD_CTX, 0, NULL);

    return poll;
}

/**
 * Start listening on a port
 * @param poll The poll_t structure
 * @param port The port to listen on
 * @return The socket file descriptor or -1 on error
 */
i64_t poll_listen(poll_p poll, i64_t port) {
    SOCKET listen_fd;
    struct sockaddr_in addr;
    int opt = 1;

    if (poll == NULL)
        return -1;

    if (poll->ipc_fd != -1)
        return -2;

    // Create socket
    listen_fd = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (listen_fd == INVALID_SOCKET)
        return -1;

    // Set socket options
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt)) == SOCKET_ERROR) {
        closesocket(listen_fd);
        return -1;
    }

    // Setup address structure
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u16_t)port);

    // Bind
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(listen_fd);
        return -1;
    }

    // Listen
    if (listen(listen_fd, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(listen_fd);
        return -1;
    }

    // Associate the listen socket with the IOCP
    if (CreateIoCompletionPort((HANDLE)listen_fd, (HANDLE)poll->poll_fd, 0, 0) == NULL) {
        closesocket(listen_fd);
        return -1;
    }

    poll->ipc_fd = listen_fd;
    return listen_fd;
}

/**
 * Clean up the polling system
 * @param poll The poll_t structure
 */
nil_t poll_destroy(poll_p poll) {
    i64_t i, l;

    if (poll == NULL)
        return;

    if (poll->ipc_fd != -1)
        closesocket((SOCKET)poll->ipc_fd);

    // Free all selectors
    l = poll->selectors->data_pos;
    for (i = 0; i < l; i++) {
        if (poll->selectors->data[i] != NULL_I64)
            poll_deregister(poll, i + SELECTOR_ID_OFFSET);
    }

    drop_obj(poll->replfile);
    drop_obj(poll->ipcfile);

    term_destroy(poll->term);

    freelist_free(poll->selectors);
    timers_destroy(poll->timers);

    if (g_iocp != INVALID_HANDLE_VALUE) {
        CloseHandle(g_iocp);
        g_iocp = INVALID_HANDLE_VALUE;
    }

    WSACleanup();
    heap_free(poll);

    heap_free(__LISTENER);
    heap_free(__STDIN_THREAD_CTX);
}

nil_t poll_deregister(poll_p poll, i64_t id) {
    i64_t idx;
    selector_p selector;

    idx = freelist_pop(poll->selectors, id - SELECTOR_ID_OFFSET);

    if (idx == NULL_I64)
        return;

    selector = (selector_p)idx;

    CloseHandle((HANDLE)selector->fd);

    heap_free(selector->rx.buf);
    heap_free(selector->tx.buf);
    queue_free(selector->tx.queue);
    heap_free(selector);
}

i64_t poll_register(poll_p poll, i64_t fd, u8_t version) {
    i64_t id;
    selector_p selector;

    selector = heap_alloc(sizeof(struct selector_t));
    id = freelist_push(poll->selectors, (i64_t)selector) + SELECTOR_ID_OFFSET;
    selector->id = id;
    selector->version = version;
    selector->fd = fd;
    selector->rx.flags = 0;
    selector->rx.ignore = B8_FALSE;
    selector->rx.header = B8_FALSE;
    selector->rx.buf = NULL;
    selector->rx.size = 0;
    selector->rx.wsa_buf.buf = NULL;
    selector->rx.wsa_buf.len = 0;
    selector->rx.overlapped.hEvent = CreateEvent(NULL, B8_TRUE, B8_FALSE, NULL);
    selector->rx.size = 0;
    selector->tx.flags = 0;
    selector->tx.ignore = B8_FALSE;
    selector->tx.buf = NULL;
    selector->tx.size = 0;
    selector->tx.wsa_buf.buf = NULL;
    selector->tx.wsa_buf.len = 0;
    selector->tx.size = 0;
    selector->tx.overlapped.hEvent = CreateEvent(NULL, B8_TRUE, B8_FALSE, NULL);
    selector->tx.queue = queue_create(TX_QUEUE_SIZE);

    CreateIoCompletionPort((HANDLE)fd, (HANDLE)poll->poll_fd, (ULONG_PTR)selector, 0);

    // prevent the IOCP mechanism from getting signaled on synchronous completions
    SetFileCompletionNotificationModes((HANDLE)fd, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);

    return id;
}

poll_result_t _recv(poll_p poll, selector_p selector) {
    UNUSED(poll);

    i64_t sz, size;
    u8_t handshake[2] = {RAYFORCE_VERSION, 0x00};
    ipc_header_t *header;

    // wait for handshake
    while (selector->version == 0) {
        // malformed handshake
        if ((selector->rx.size == 0) ||
            (selector->rx.wsa_buf.len == sizeof(struct ipc_header_t) && selector->rx.size == 1))
            return POLL_ERROR;

        // incomplete handshake
        if (selector->rx.wsa_buf.buf[selector->rx.size - 1] != '\0') {
            selector->rx.wsa_buf.len = selector->rx.wsa_buf.len - selector->rx.size;
            if (selector->rx.wsa_buf.len == 0) {
                size = selector->rx.size;
                selector->rx.size *= 2;
                selector->rx.buf = heap_realloc(selector->rx.buf, selector->rx.size);
                selector->rx.wsa_buf.buf = (str_p)selector->rx.buf + size;
                selector->rx.wsa_buf.len = size;
            } else {
                selector->rx.wsa_buf.buf = selector->rx.wsa_buf.buf + selector->rx.size;
            }

            _RECV_OP(poll, selector);

            continue;
        }

        selector->version = selector->rx.wsa_buf.buf[selector->rx.size - 2];

        // malformed version
        if (selector->version == 0)
            return POLL_ERROR;

        selector->rx.wsa_buf.buf = (str_p)selector->rx.buf;
        selector->rx.wsa_buf.len = sizeof(struct ipc_header_t);
        selector->rx.size = 0;

        // send handshake response
        size = 0;
        while (size < (i64_t)sizeof(handshake)) {
            sz = sock_send(selector->fd, &handshake[size], sizeof(handshake) - size);

            if (sz == -1)
                return POLL_ERROR;

            size += sz;
        }
    }

    if (selector->rx.buf == NULL) {
        selector->rx.buf = heap_alloc(sizeof(struct ipc_header_t));
        selector->rx.size = sizeof(struct ipc_header_t);
        selector->rx.wsa_buf.buf = (str_p)selector->rx.buf;
        selector->rx.wsa_buf.len = selector->rx.size;
    }

    // read header
    while (!selector->rx.header) {
        selector->rx.wsa_buf.buf += selector->rx.size;
        selector->rx.wsa_buf.len -= selector->rx.size;

        if (selector->rx.wsa_buf.len != 0) {
            _RECV_OP(poll, selector);
            continue;
        }

        header = (ipc_header_t *)selector->rx.buf;

        // malformed header
        if (header->size == 0)
            return POLL_ERROR;

        selector->rx.header = B8_TRUE;
        selector->rx.size = header->size + sizeof(struct ipc_header_t);
        selector->rx.msgtype = header->msgtype;
        selector->rx.buf = heap_realloc(selector->rx.buf, selector->rx.size);
        selector->rx.wsa_buf.buf = (str_p)selector->rx.buf + sizeof(struct ipc_header_t);
        selector->rx.wsa_buf.len = selector->rx.size - sizeof(struct ipc_header_t);
        selector->rx.size = 0;
    }

    // read body
    while (selector->rx.wsa_buf.len > 0) {
        selector->rx.wsa_buf.buf += selector->rx.size;
        selector->rx.wsa_buf.len -= selector->rx.size;

        if (selector->rx.wsa_buf.len == 0)
            break;

        _RECV_OP(poll, selector);
    }

    selector->rx.header = B8_FALSE;

    return POLL_DONE;
}

poll_result_t _recv_initiate(poll_p poll, selector_p selector) {
    selector->rx.buf = heap_alloc(sizeof(struct ipc_header_t));
    selector->rx.size = sizeof(struct ipc_header_t);
    selector->rx.wsa_buf.buf = (str_p)selector->rx.buf;
    selector->rx.wsa_buf.len = selector->rx.size;

    _RECV_OP(poll, selector);

    return _recv(poll, selector);
}

poll_result_t _send(poll_p poll, selector_p selector) {
    UNUSED(poll);

    obj_p obj;
    nil_t *v;
    u8_t msg_type;
    i64_t size;

send:
    while (selector->tx.wsa_buf.len > 0) {
        selector->tx.wsa_buf.buf += selector->tx.size;
        selector->tx.wsa_buf.len -= selector->tx.size;

        if (selector->tx.wsa_buf.len != 0)
            _SEND_OP(poll, selector);
    }

    if (selector->tx.buf) {
        heap_free(selector->tx.buf);
        selector->tx.buf = NULL;
        selector->tx.size = 0;
    }

    v = queue_pop(selector->tx.queue);

    if (v != NULL) {
        obj = (obj_p)((i64_t)v & ~(3ll << 61));
        msg_type = (((i64_t)v & (3ll << 61)) >> 61);
        size = ser_raw(&selector->tx.buf, obj);
        drop_obj(obj);
        if (size == -1)
            return POLL_ERROR;
        selector->tx.size = size;
        selector->tx.wsa_buf.buf = (str_p)selector->tx.buf;
        selector->tx.wsa_buf.len = selector->tx.size;
        selector->tx.size = 0;
        ((ipc_header_t *)selector->tx.buf)->msgtype = msg_type;
        goto send;
    }

    return POLL_DONE;
}

obj_p read_obj(selector_p selector) {
    obj_p res;

    res = de_raw(selector->rx.buf, selector->rx.size);
    heap_free(selector->rx.buf);
    selector->rx.buf = NULL;
    selector->rx.size = 0;
    selector->rx.wsa_buf.buf = NULL;
    selector->rx.wsa_buf.len = 0;
    selector->rx.size = 0;

    return res;
}

nil_t process_request(poll_p poll, selector_p selector) {
    obj_p v, res;
    poll_result_t poll_result;

    res = read_obj(selector);

    if (IS_ERR(res) || is_null(res))
        v = res;
    if (res->type == TYPE_C8) {
        v = ray_eval_str(poll->ipcfile, res);
        drop_obj(res);
    } else
        v = eval_obj(res);

    // sync request
    if (selector->rx.msgtype == MSG_TYPE_SYNC) {
        queue_push(selector->tx.queue, (nil_t *)((i64_t)v | ((i64_t)MSG_TYPE_RESP << 61)));
        poll_result = _send(poll, selector);

        if (poll_result == POLL_ERROR)
            poll_deregister(poll, selector->id);
    } else
        drop_obj(v);
}

i64_t poll_run(poll_p poll) {
    DWORD i, num = 5, size;
    OVERLAPPED *overlapped;
    HANDLE hPollFd = (HANDLE)poll->poll_fd;
    SOCKET hAccepted;
    OVERLAPPED_ENTRY events[MAX_EVENTS];
    b8_t success, error;
    i64_t key, poll_result, idx;
    obj_p str, fmt, res;
    selector_p selector;

    term_prompt(poll->term);

    while (poll->code == NULL_I64) {
        success = GetQueuedCompletionStatusEx(hPollFd, events, MAX_IOCP_RESULTS, &num, INFINITE,
                                              B8_TRUE  // set this to B8_TRUE if you want to return on alertable wait
        );
        // Handle IOCP events
        if (success) {
            for (i = 0; i < num; ++i) {
                key = events[i].lpCompletionKey;
                size = events[i].dwNumberOfBytesTransferred;
                overlapped = events[i].lpOverlapped;

                switch (key) {
                    case STDIN_WAKER_ID:
                        if (size == 0) {
                            poll->code = 0;
                            break;
                        }

                        str = term_read(poll->term);
                        if (str != NULL) {
                            if (IS_ERR(str))
                                io_write(STDOUT_FILENO, MSG_TYPE_RESP, str);
                            else if (str != NULL_OBJ) {
                                res = ray_eval_str(str, poll->replfile);
                                drop_obj(str);
                                io_write(STDOUT_FILENO, MSG_TYPE_RESP, res);
                                error = IS_ERR(res);
                                drop_obj(res);
                                if (!error)
                                    timeit_print();
                            }

                            term_prompt(poll->term);
                        }

                        break;

                    default:
                        // Accept new connection
                        if (key == poll->ipc_fd) {
                            hAccepted = __LISTENER->hAccepted;

                            if (hAccepted != INVALID_SOCKET) {
                                idx = poll_register(poll, hAccepted, 0);
                                selector = (selector_p)freelist_get(poll->selectors, idx - SELECTOR_ID_OFFSET);
                                poll_result = _recv_initiate(poll, selector);

                                if (poll_result == POLL_ERROR)
                                    poll_deregister(poll, selector->id);
                            }

                            poll_accept(poll);
                        } else {
                            selector = (selector_p)key;

                            // Connection closed
                            if (size == 0) {
                                poll_deregister(poll, selector->id);
                                break;
                            }

                            // recv
                            if (overlapped == &(selector->rx.overlapped)) {
                                if (selector->rx.ignore) {
                                    selector->rx.ignore = B8_FALSE;
                                    selector->rx.size = 0;
                                } else
                                    selector->rx.size = size;
                            recv:
                                poll_result = _recv(poll, selector);

                                if (poll_result == POLL_ERROR) {
                                    poll_deregister(poll, selector->id);
                                    break;
                                }

                                if (poll_result == POLL_DONE) {
                                    process_request(poll, selector);
                                    // setup next recv
                                    goto recv;
                                }
                            }

                            // send
                            if (overlapped == &(selector->tx.overlapped)) {
                                if (selector->tx.ignore) {
                                    selector->tx.ignore = B8_FALSE;
                                    selector->tx.size = 0;
                                } else
                                    selector->tx.size = size;

                                // setup next send
                                poll_result = _send(poll, selector);

                                if (poll_result == POLL_ERROR)
                                    poll_deregister(poll, selector->id);
                            }
                        }

                }  // switch
            }  // for
        } else {
            res = sys_error(ERROR_TYPE_SOCK, "poll_init");
            fmt = obj_fmt(res, B8_TRUE);
            printf("%s\n", AS_C8(fmt));
            drop_obj(fmt);
            drop_obj(res);
        }
    }

    return 0;
}

obj_p ipc_send_sync(poll_p poll, i64_t id, obj_p msg) {
    poll_result_t poll_result = POLL_OK;
    selector_p selector;
    i64_t idx;
    obj_p res;
    DWORD dwResult;

    idx = freelist_get(poll->selectors, id - SELECTOR_ID_OFFSET);

    if (idx == NULL_I64)
        THROW(ERR_IO, "ipc_send_sync: invalid socket fd: %lld", id);

    selector = (selector_p)idx;

    queue_push(selector->tx.queue, (nil_t *)((i64_t)msg | ((i64_t)MSG_TYPE_SYNC << 61)));

    // set ignore flag to tx
    selector->tx.ignore = B8_TRUE;

    while (poll_result == POLL_OK) {
        poll_result = _send(poll, selector);

        if (poll_result != POLL_OK)
            break;

        dwResult = WaitForSingleObject(selector->tx.overlapped.hEvent, INFINITE);

        if (dwResult == WAIT_FAILED)
            THROW(ERR_IO, "ipc_send_sync: error waiting for event");

        if (!GetOverlappedResult((HANDLE)selector->fd, &selector->tx.overlapped, &selector->tx.size, B8_FALSE))
            THROW(ERR_IO, "ipc_send_sync: error getting result");
    }

    if (poll_result == POLL_ERROR) {
        poll_deregister(poll, selector->id);
        THROW(ERR_IO, "ipc_send_sync: error sending message");
    }

    poll_result = POLL_OK;

    // set ignore flag to rx
    selector->rx.ignore = B8_TRUE;

    if (selector->rx.buf == NULL) {
        selector->rx.buf = heap_alloc(sizeof(struct ipc_header_t));
        selector->rx.size = sizeof(struct ipc_header_t);
        selector->rx.wsa_buf.buf = (str_p)selector->rx.buf;
        selector->rx.wsa_buf.len = selector->rx.size;
        poll_result = _recv(poll, selector);
    }

recv:
    while (poll_result == POLL_OK) {
        dwResult = WaitForSingleObject(selector->rx.overlapped.hEvent, INFINITE);

        if (dwResult == WAIT_FAILED)
            THROW(ERR_IO, "ipc_send_sync: error waiting for event");

        if (!GetOverlappedResult((HANDLE)selector->fd, &selector->rx.overlapped, &selector->rx.size, B8_FALSE))
            THROW(ERR_IO, "ipc_send_sync: error getting result");

        poll_result = _recv(poll, selector);
    }

    if (poll_result == POLL_ERROR) {
        poll_deregister(poll, selector->id);
        THROW(ERR_IO, "ipc_send_sync: error receiving message");
    }

    // recv until we get response
    switch (selector->rx.msgtype) {
        case MSG_TYPE_RESP:
            res = read_obj(selector);
            break;
        default:
            poll_result = POLL_OK;
            process_request(poll, selector);
            goto recv;
    }

    return res;
}

obj_p ipc_send_async(poll_p poll, i64_t id, obj_p msg) {
    i64_t idx;
    selector_p selector;

    idx = freelist_get(poll->selectors, id - SELECTOR_ID_OFFSET);

    if (idx == NULL_I64)
        THROW(ERR_IO, "ipc_send_async: invalid socket fd: %lld", id);

    selector = (selector_p)idx;
    if (selector == NULL)
        THROW(ERR_IO, "ipc_send_async: invalid socket fd: %lld", id);

    queue_push(selector->tx.queue, (nil_t *)msg);

    if (_send(poll, selector) == POLL_ERROR)
        THROW(ERR_IO, "ipc_send_async: error sending message");

    return NULL_OBJ;
}
