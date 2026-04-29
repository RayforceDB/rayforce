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

#ifndef RAY_IO_REPL_H
#define RAY_IO_REPL_H

#include <rayforce.h>

typedef struct ray_term ray_term_t;
typedef struct ray_poll ray_poll_t;

typedef struct ray_repl {
    ray_t*              _block;
    ray_term_t*         term;      /* NULL if piped/non-tty */
    bool                timeit;    /* :timeit toggle — print expression timing */
    ray_poll_t*         poll;      /* event loop — NULL in piped mode */
    int64_t             id;        /* selector id for stdin in poll */
} ray_repl_t;

ray_repl_t* ray_repl_create(ray_poll_t* poll);
void       ray_repl_destroy(ray_repl_t* repl);
void       ray_repl_run(ray_repl_t* repl);

/* Run a Rayfall script file in batch (script) mode.  Contract:
 *   - returns 0 on success
 *   - returns 1 on any eval error (script execution stops at first
 *     error; subsequent forms are not run)
 * Distinct from ray_repl_run / stdin pipe which use REPL semantics
 * (errors are printed but do not terminate the loop). */
int        ray_repl_run_file(const char* path);

#endif /* RAY_IO_REPL_H */
