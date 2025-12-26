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
#include "../core/string.h"
#include "../core/log.h"
#include "../core/error.h"
#include "../core/error.h"

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
        if (repl->silent)
            poll_exit(poll, 1);
    } else if (str != NULL_OBJ) {
        i64_t line = repl->term ? term_last_input_line(repl->term) : 0;
        res = ray_eval_str_line(str, repl->name, line);
        error = IS_ERR(res);
        if (error) {
            io_write(STDERR_FILENO, 2, res);
            if (repl->silent)
                poll_exit(poll, 1);
        } else if (!repl->silent)  // Only print output if not in silent mode
            io_write(STDOUT_FILENO, 2, res);

        if (!error && !repl->silent)
            timeit_print();
    }

    drop_obj(res);
    drop_obj(str);

    // Only show regular prompt if not in multiline mode and not exiting
    // (continuation prompt is already shown by term_read when in multiline mode)
    if (!repl->silent && repl->term->multiline_len == 0 && poll->code == NULL_I64)
        term_prompt(repl->term);

    return option_none();
}

option_t repl_read(poll_p poll, selector_p selector) {
    obj_p str;
    repl_p repl;
    c8_t line_buf[TERM_BUF_SIZE];
    i64_t len;

    LOG_TRACE("repl_read");

    repl = (repl_p)selector->data;

    if (repl->silent) {
        // In silent mode, just read entire lines directly
        len = read(STDIN_FILENO, line_buf, TERM_BUF_SIZE - 1);

        if (len <= 0) {
            poll->code = (len < 0) ? 1 : 0;
            return option_error(err_os());
        }

        line_buf[len] = '\0';
        str = string_from_str(line_buf, len);
        return option_some(str);
    }

    if (!term_getc(repl->term)) {
        poll->code = 1;
        return option_error(err_os());
    }

    str = term_read(repl->term);

    if (str == NULL)
        return option_none();

    return option_some(str);
}

repl_p repl_create(poll_p poll, b8_t silent) {
    repl_p repl;

    repl = (repl_p)heap_alloc(sizeof(struct repl_t));
    repl->name = string_from_str("repl", 4);
    repl->silent = silent;

#if defined(OS_WINDOWS)
    // On Windows, STDIN is handled by iocp.c's StdinThread
    // The poll_init() function sets up STDIN handling internally
    // Use poll->term instead of creating a separate one
    repl->term = poll->term;
    repl->id = 0;       // Placeholder ID for Windows
    poll->repl = repl;  // Store repl pointer for cleanup
#else
    // Only create term if not in silent mode
    if (!silent) {
        repl->term = term_create();
    } else {
        repl->term = NULL;
    }

    // Only register stdin if it's a TTY (epoll doesn't work on regular files/pipes)
    if (isatty(STDIN_FILENO)) {
        struct poll_registry_t registry = ZERO_INIT_STRUCT;
        registry.fd = STDIN_FILENO;
        registry.type = SELECTOR_TYPE_STDIN;
        registry.events = POLL_EVENT_READ | POLL_EVENT_ERROR | POLL_EVENT_HUP;
        registry.recv_fn = NULL;
        registry.read_fn = repl_read;
        registry.close_fn = repl_on_close;
        registry.error_fn = repl_on_error;
        registry.data_fn = repl_on_data;
        registry.data = repl;

        repl->id = poll_register(poll, &registry);

        if (repl->id == -1) {
            repl_destroy(repl);
            return NULL;
        }
    } else {
        // stdin is not a TTY, no point in creating a REPL
        // Clean up and return NULL to avoid memory leak
        repl_destroy(repl);
        return NULL;
    }
#endif

    if (!silent)
        term_prompt(repl->term);

    return repl;
}

nil_t repl_destroy(repl_p repl) {
    drop_obj(repl->name);
#if !defined(OS_WINDOWS)
    // On Windows, term is shared with poll->term, so don't destroy here
    if (repl->term)
        term_destroy(repl->term);
#endif
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
