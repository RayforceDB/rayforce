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
#include <unistd.h>
#include "repl.h"
#include "io.h"
#include "eval.h"
#include "term.h"
#include "error.h"
#include "symbols.h"
#include "heap.h"
#include "poll.h"
#include "string.h"
#include "log.h"

option_t repl_on_data(poll_p poll, selector_p selector, raw_p data) {
    UNUSED(poll);

    b8_t error;
    repl_p repl;
    obj_p res, str;

    LOG_TRACE("repl_on_data");

    repl = (repl_p)selector->data;
    res = NULL_OBJ;
    str = (obj_p)data;

    if (IS_ERR(str))
        io_write(STDERR_FILENO, 2, str);
    else if (str != NULL_OBJ) {
        res = ray_eval_str(str, repl->name);
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
        return option_error(sys_error(ERR_IO, "term_getc failed"));
    }

    str = term_read(repl->term);

    if (str == NULL)
        return option_none();

    return option_some(str);
}

repl_p repl_create(poll_p poll) {
    repl_p repl;
    struct poll_registry_t registry = ZERO_INIT_STRUCT;

    repl = (repl_p)heap_alloc(sizeof(struct repl_t));
    repl->name = string_from_str("repl", 4);
    repl->term = term_create();

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

    if (repl->id == NULL_I64) {
        repl_destroy(repl);
        return NULL;
    }

    term_prompt(repl->term);

    return repl;
}

nil_t repl_destroy(repl_p repl) {
    drop_obj(repl->name);
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
