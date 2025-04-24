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

// Forward declaration of callback handlers
poll_result_t stdin_event_handler(poll_p poll, selector_p selector, u32_t events);
poll_result_t listener_handler(poll_p poll, selector_p selector, u32_t events);
poll_result_t default_handler(poll_p poll, selector_p selector, u32_t events);

// Definitions and globals
#define STDIN_WAKER_ID ~0ull
#define MAX_IOCP_RESULTS 64

typedef struct listener_t {
    u8_t buf[(sizeof(SOCKADDR_IN) + 16) * 2];
    OVERLAPPED overlapped;
    DWORD dwBytes;
    SOCKET hAccepted;
    SOCKET listenSocket;  // Store the listener socket here
} *listener_p;

typedef struct stdin_thread_ctx_t {
    HANDLE h_cp;
    term_p term;
} *stdin_thread_ctx_p;

listener_p __LISTENER = NULL;
stdin_thread_ctx_p __STDIN_THREAD_CTX = NULL;

#define _RECV_OP(poll, selector)                                                                      \
    {                                                                                                 \
        i32_t poll_result;                                                                            \
                                                                                                      \
        poll_result = WSARecv(selector->fd, &selector->rx.wsa_buf, 1, &selector->rx.bytes_transfered, \
                              &selector->rx.flags, &selector->rx.overlapped, NULL);                   \
                                                                                                      \
        if (poll_result == SOCKET_ERROR) {                                                            \
            if (WSAGetLastError() == ERROR_IO_PENDING)                                                \
                return POLL_PENDING;                                                                  \
                                                                                                      \
            return POLL_ERROR;                                                                        \
        }                                                                                             \
    }

#define _SEND_OP(poll, selector)                                                                      \
    {                                                                                                 \
        i32_t poll_result;                                                                            \
                                                                                                      \
        poll_result = WSASend(selector->fd, &selector->tx.wsa_buf, 1, &selector->tx.bytes_transfered, \
                              selector->tx.flags, &selector->tx.overlapped, NULL);                    \
                                                                                                      \
        if (poll_result == SOCKET_ERROR) {                                                            \
            if (WSAGetLastError() == ERROR_IO_PENDING)                                                \
                return POLL_PENDING;                                                                  \
                                                                                                      \
            return POLL_ERROR;                                                                        \
        }                                                                                             \
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

i64_t poll_accept(SOCKET listenSocket) {
    i32_t code;
    LPFN_ACCEPTEX lpfnAcceptEx = NULL;
    GUID GuidAcceptEx = WSAID_ACCEPTEX;
    DWORD dwBytes;
    SOCKET sock_fd = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    b8_t success;

    if (sock_fd == INVALID_SOCKET)
        return -1;

    // Load AcceptEx function
    if (WSAIoctl(listenSocket, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidAcceptEx, sizeof(GuidAcceptEx), &lpfnAcceptEx,
                 sizeof(lpfnAcceptEx), &dwBytes, NULL, NULL) == SOCKET_ERROR) {
        code = WSAGetLastError();
        closesocket(sock_fd);
        WSASetLastError(code);
        return -1;
    }

    success = lpfnAcceptEx(listenSocket, sock_fd, __LISTENER->buf, 0, sizeof(SOCKADDR_IN) + 16,
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

    // Create a completion port
    HANDLE h_cp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (h_cp == NULL) {
        WSACleanup();
        return NULL;
    }

    g_iocp = h_cp;

    poll = (poll_p)heap_alloc(sizeof(struct poll_t));
    poll->code = NULL_I64;
    poll->poll_fd = (i64_t)h_cp;
    poll->replfile = string_from_str("repl", 4);
    poll->ipcfile = string_from_str("ipc", 3);
    poll->term = term_create();
    poll->selectors = freelist_create(128);
    poll->timers = timers_create(16);

    // Initialize the listener
    __LISTENER = (listener_p)heap_alloc(sizeof(struct listener_t));
    memset(__LISTENER, 0, sizeof(struct listener_t));

    // Create the stdin thread context
    __STDIN_THREAD_CTX = (stdin_thread_ctx_p)heap_alloc(sizeof(struct stdin_thread_ctx_t));
    __STDIN_THREAD_CTX->h_cp = (HANDLE)poll->poll_fd;
    __STDIN_THREAD_CTX->term = poll->term;

    // Create a thread to handle stdin
    HANDLE stdin_thread = CreateThread(NULL, 0, StdinThread, __STDIN_THREAD_CTX, 0, NULL);
    if (stdin_thread == NULL) {
        WSACleanup();
        exit_werror();
    }

    // Create a listening socket for the server if port was specified
    if (port)
        listen_fd = poll_listen(poll, port);

    term_prompt(poll->term);

    return poll;
}

i64_t poll_listen(poll_p poll, i64_t port) {
    SOCKET listen_fd;
    struct sockaddr_in addr;
    i64_t res;

    // Check for invalid poll
    if (poll == NULL)
        return -1;

    // Create a listening socket
    listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd == INVALID_SOCKET)
        return -1;

    // Set up the sockaddr structure
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((u16_t)port);

    // Bind the socket to the port
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(listen_fd);
        return -1;
    }

    // Start listening
    if (listen(listen_fd, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(listen_fd);
        return -1;
    }

    // Register the listener socket with a callback
    __LISTENER->listenSocket = listen_fd;
    res = poll_register_with_callback(poll, (i64_t)listen_fd, listener_handler, NULL);
    if (res == -1) {
        closesocket(listen_fd);
        return -1;
    }

    // Start the first async accept operation
    poll_accept(listen_fd);

    return (i64_t)listen_fd;
}

nil_t poll_destroy(poll_p poll) {
    i64_t i, l;

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

    if (__LISTENER != NULL) {
        if (__LISTENER->listenSocket != INVALID_SOCKET)
            closesocket(__LISTENER->listenSocket);
        heap_free(__LISTENER);
        __LISTENER = NULL;
    }

    if (__STDIN_THREAD_CTX != NULL) {
        heap_free(__STDIN_THREAD_CTX);
        __STDIN_THREAD_CTX = NULL;
    }

    CloseHandle((HANDLE)poll->poll_fd);
    heap_free(poll);
    WSACleanup();
}

i64_t poll_register(poll_p poll, i64_t fd) {
    // Default to using the default handler
    return poll_register_with_callback(poll, fd, default_handler, NULL);
}

i64_t poll_register_with_callback(poll_p poll, i64_t fd, event_callback_t callback, void *user_data) {
    i64_t id;
    selector_p selector;

    selector = heap_alloc(sizeof(struct selector_t));
    id = freelist_push(poll->selectors, (i64_t)selector) + SELECTOR_ID_OFFSET;
    selector->id = id;
    selector->handshake_completed = B8_FALSE;  // Start with handshake not completed
    selector->fd = fd;
    selector->callback = callback;
    selector->user_data = user_data;
    selector->tx.ignore = B8_FALSE;
    selector->rx.ignore = B8_FALSE;
    selector->rx.header = B8_FALSE;
    selector->rx.buf = NULL;
    selector->rx.size = 0;
    selector->rx.bytes_transfered = 0;
    selector->tx.buf = NULL;
    selector->tx.size = 0;
    selector->tx.bytes_transfered = 0;
    selector->tx.queue = queue_create(TX_QUEUE_SIZE);
    memset(&selector->rx.overlapped, 0, sizeof(OVERLAPPED));
    memset(&selector->tx.overlapped, 0, sizeof(OVERLAPPED));
    selector->rx.flags = 0;
    selector->tx.flags = 0;

    // Associate the socket with the completion port
    if (CreateIoCompletionPort((HANDLE)fd, (HANDLE)poll->poll_fd, (ULONG_PTR)selector, 0) == NULL) {
        heap_free(selector);
        return -1;
    }

    return id;
}

// Event handler for stdin
poll_result_t stdin_event_handler(poll_p poll, selector_p selector, u32_t events) {
    UNUSED(selector);
    UNUSED(events);

    obj_p str, res;
    b8_t error;

    if (!term_getc(poll->term)) {
        poll->code = 1;
        return POLL_ERROR;
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

    return POLL_DONE;
}

// Event handler for listener socket
poll_result_t listener_handler(poll_p poll, selector_p selector, u32_t events) {
    UNUSED(events);
    SOCKET newSocket;

    // Complete the accept operation and get the new client socket
    newSocket = __LISTENER->hAccepted;

    if (newSocket != INVALID_SOCKET) {
        // Register the new socket
        poll_register(poll, (i64_t)newSocket);

        // Start another accept operation
        poll_accept(selector->fd);
    }

    return POLL_DONE;
}

// Default handler for regular socket connections
poll_result_t default_handler(poll_p poll, selector_p selector, u32_t events) {
    poll_result_t poll_result;

    // Handle read events
    if (events & EPOLLIN) {
        poll_result = _recv(poll, selector);
        if (poll_result == POLL_PENDING)
            return POLL_PENDING;

        if (poll_result == POLL_ERROR) {
            return POLL_ERROR;
        }

        process_request(poll, selector);
    }

    // Handle write events
    if (events & EPOLLOUT) {
        poll_result = _send(poll, selector);
        if (poll_result == POLL_ERROR)
            return POLL_ERROR;
    }

    return POLL_DONE;
}
