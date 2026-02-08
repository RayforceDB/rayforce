/*
 *   Copyright (c) 2025 Anton Kundenko <singaraiona@gmail.com>
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

#include <stdio.h>
#if defined(OS_WINDOWS)
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define read _read
#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif
#else
#include <unistd.h>
#endif
#include "repl.h"
#include "term.h"
#include "../core/io.h"
#include "../core/eval.h"
#include "../core/heap.h"
#include "../core/poll.h"
#include "../core/str.h"
#include "../core/log.h"
#include "../core/error.h"

#if defined(OS_WINDOWS)
// Stdin thread context and globals (owned by repl on Windows)
typedef struct stdin_thread_ctx_t {
    HANDLE h_cp;
    term_p term;
    volatile LONG stop;
} *stdin_thread_ctx_p;

static stdin_thread_ctx_p __STDIN_THREAD_CTX = NULL;
static HANDLE __STDIN_THREAD_HANDLE = NULL;

static DWORD WINAPI StdinThread(LPVOID prm) {
    stdin_thread_ctx_p ctx = (stdin_thread_ctx_p)prm;
    term_p term = ctx->term;
    HANDLE h_cp = ctx->h_cp;
    DWORD bytes;

    for (;;) {
        if (InterlockedCompareExchange(&ctx->stop, 0, 0) != 0)
            break;

        bytes = (DWORD)term_getc(term);

        if (InterlockedCompareExchange(&ctx->stop, 0, 0) != 0)
            break;

        if (bytes == 0)
            break;

        PostQueuedCompletionStatus(h_cp, bytes, STDIN_WAKER_ID, NULL);
    }

    PostQueuedCompletionStatus(h_cp, 0, STDIN_WAKER_ID, NULL);
    return 0;
}

static nil_t repl_stdin_handler(poll_p poll, raw_p data) {
    repl_p repl = (repl_p)data;
    b8_t error;
    obj_p str, res;

    str = term_read(repl->term);
    if (str != NULL) {
        if (IS_ERR(str))
            io_write(STDERR_FILENO, 2, str);
        else if (str != NULL_OBJ) {
            i64_t line = repl->term ? term_last_input_line(repl->term) : 0;
            res = ray_eval_str_line(str, repl->name, line);
            drop_obj(str);
            error = IS_ERR(res);
            if (error)
                io_write(STDERR_FILENO, 2, res);
            else
                io_write(STDOUT_FILENO, 2, res);
            drop_obj(res);
            if (!error)
                timeit_print();
        }

        if (repl->term->multiline_len == 0 && poll->code == NULL_I64)
            term_prompt(repl->term);
    }
}

static nil_t repl_stop_stdin_thread(nil_t) {
    if (__STDIN_THREAD_HANDLE != NULL && __STDIN_THREAD_CTX != NULL) {
        DWORD wait_result;
        InterlockedExchange(&__STDIN_THREAD_CTX->stop, 1);
        CancelSynchronousIo(__STDIN_THREAD_HANDLE);
        wait_result = WaitForSingleObject(__STDIN_THREAD_HANDLE, 100);
        if (wait_result == WAIT_TIMEOUT)
            TerminateThread(__STDIN_THREAD_HANDLE, 0);
        CloseHandle(__STDIN_THREAD_HANDLE);
        __STDIN_THREAD_HANDLE = NULL;
    }
    if (__STDIN_THREAD_CTX) {
        heap_free(__STDIN_THREAD_CTX);
        __STDIN_THREAD_CTX = NULL;
    }
}
#endif

option_t repl_on_data(poll_p poll, selector_p selector, raw_p data) {
    UNUSED(poll);

    b8_t error;
    repl_p repl;
    obj_p res, str;

    LOG_TRACE("repl_on_data");

    repl = (repl_p)selector->data;
    res = NULL_OBJ;
    str = (obj_p)data;

    if (IS_ERR(str)) {
        io_write(STDERR_FILENO, 2, str);
    } else if (str != NULL_OBJ) {
        i64_t line = repl->term ? term_last_input_line(repl->term) : 0;
        res = ray_eval_str_line(str, repl->name, line);
        error = IS_ERR(res);
        if (error)
            io_write(STDERR_FILENO, 2, res);
        else
            io_write(STDOUT_FILENO, 2, res);

        if (!error)
            timeit_print();
    }

    drop_obj(res);
    drop_obj(str);

    // Only show regular prompt if not in multiline mode and not exiting
    // (continuation prompt is already shown by term_read when in multiline mode)
    if (repl->term->multiline_len == 0 && poll->code == NULL_I64)
        term_prompt(repl->term);

    return option_none();
}

option_t repl_read(poll_p poll, selector_p selector) {
    obj_p str;
    repl_p repl;

    LOG_TRACE("repl_read");

    repl = (repl_p)selector->data;

    if (!term_getc(repl->term)) {
        poll->code = 1;
        return option_error(err_os());
    }

    str = term_read(repl->term);

    if (str == NULL)
        return option_none();

    return option_some(str);
}

repl_p repl_create(poll_p poll) {
    repl_p repl;

    repl = (repl_p)heap_alloc(sizeof(struct repl_t));
    repl->name = string_from_str("repl", 4);

#if defined(OS_WINDOWS)
    // On Windows, repl owns the terminal and stdin thread
    repl->term = term_create();
    repl->id = 0;

    if (repl->term != NULL) {
        // Register stdin callback with poll
        poll_set_stdin(poll, repl_stdin_handler, repl);

        // Create stdin thread to post IOCP events on keypress
        __STDIN_THREAD_CTX = (stdin_thread_ctx_p)heap_alloc(sizeof(struct stdin_thread_ctx_t));
        __STDIN_THREAD_CTX->h_cp = (HANDLE)poll->poll_fd;
        __STDIN_THREAD_CTX->term = repl->term;
        __STDIN_THREAD_CTX->stop = 0;
        __STDIN_THREAD_HANDLE = CreateThread(NULL, 0, StdinThread, (LPVOID)__STDIN_THREAD_CTX, 0, NULL);
    }
#else
    // Only register stdin if it's a TTY (epoll doesn't work on regular files/pipes)
    if (isatty(STDIN_FILENO)) {
        repl->term = term_create();

        struct poll_registry_t registry = ZERO_INIT_STRUCT;
        registry.fd = STDIN_FILENO;
        registry.type = SELECTOR_TYPE_STDIN;
        registry.events = POLL_EVENT_READ | POLL_EVENT_ERROR | POLL_EVENT_HUP;
        registry.recv_fn = NULL;
        registry.read_fn = repl_read;
        // epoll poll_destroy already calls close_fn on shutdown; main owns repl lifetime
        registry.close_fn = NULL;
        registry.error_fn = repl_on_error;
        registry.data_fn = repl_on_data;
        registry.data = repl;

        repl->id = poll_register(poll, &registry);

        if (repl->id == -1) {
            repl_destroy(repl);
            return NULL;
        }
    } else {
        // Piped input - read and eval synchronously, then exit (oneshot mode, like Python)
        c8_t buf[TERM_BUF_SIZE];
        i64_t len;
        obj_p str, res, fmt;
        b8_t has_error = B8_FALSE;

        repl->term = NULL;

        while ((len = read(STDIN_FILENO, buf, TERM_BUF_SIZE - 1)) > 0) {
            buf[len] = '\0';
            str = string_from_str(buf, len);
            res = ray_eval_str_line(str, repl->name, 0);
            drop_obj(str);

            if (IS_ERR(res)) {
                fmt = obj_fmt(res, B8_TRUE);
                printf("%.*s\n", (i32_t)fmt->len, AS_C8(fmt));
                drop_obj(fmt);
                has_error = B8_TRUE;
            }
            drop_obj(res);
        }

        // Print timing info and signal to exit
        timeit_print();
        poll->code = has_error ? 1 : 0;

        repl_destroy(repl);
        return NULL;
    }
#endif

    term_prompt(repl->term);

    return repl;
}

nil_t repl_destroy(repl_p repl) {
#if defined(OS_WINDOWS)
    repl_stop_stdin_thread();
#endif
    drop_obj(repl->name);
    if (repl->term)
        term_destroy(repl->term);
    heap_free(repl);
}

nil_t repl_on_close(poll_p poll, selector_p selector) {
    UNUSED(poll);

    repl_destroy(selector->data);
}

nil_t repl_on_error(poll_p poll, selector_p selector) {
    UNUSED(poll);
    UNUSED(selector);

    perror("repl_on_error");
}
