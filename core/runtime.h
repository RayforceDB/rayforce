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

#ifndef RUNTIME_H
#define RUNTIME_H

#include "rayforce.h"
#include "symbols.h"
#include "heap.h"
#include "env.h"
#include "eval.h"
#include "poll.h"
#include "pool.h"
#include "sys.h"
#include "query.h"
#include "thread.h"

/*
 * Runtime structure.
 */
typedef struct runtime_t {
    obj_p args;             // Command line arguments.
    sys_info_t sys_info;    // System information.
    env_t env;              // Environment.
    symbols_p symbols;      // vector_symbols pool.
    poll_p poll;            // I/O event loop handle.
    obj_p fdmaps;           // File descriptors mappings.
    query_ctx_p query_ctx;  // Query context stack.
    pool_p pool;            // Executors pool.
    obj_p dynlibs;          // Dynamic libraries.
} *runtime_p;

extern runtime_p __RUNTIME;

runtime_p runtime_create(i32_t argc, str_p argv[]);
i32_t runtime_run(nil_t);
nil_t runtime_destroy(nil_t);
obj_p runtime_get_arg(lit_p key);
nil_t runtime_fdmap_push(runtime_p runtime, obj_p assoc, obj_p fdmap);
obj_p runtime_fdmap_pop(runtime_p runtime, obj_p assoc);
obj_p runtime_fdmap_get(runtime_p runtime, obj_p assoc);
inline __attribute__((always_inline)) runtime_p runtime_get(nil_t) { return __RUNTIME; }
runtime_p runtime_get_ext(nil_t);

#endif  // RUNTIME_H
