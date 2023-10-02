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
#include "heap.h"
#include "env.h"
#include "parse.h"
#include "vm.h"
#include "poll.h"
#include "sock.h"

/*
 * Runtime structure.
 */
typedef struct runtime_t
{
    u16_t slaves;       // Number of slave threads.
    env_t env;          // Environment.
    parser_t parser;    // Parser.
    vm_t vm;            // Virtual machine.
    poll_t select;    // I/O event loop handle.
    obj_t args;         // Command line arguments.
    symbols_t *symbols; // vector_symbols pool.
    sock_addr_t addr;   // Socket address that a process listen.
} *runtime_t;

extern nil_t runtime_init(i32_t argc, str_t argv[]);
extern i32_t runtime_run();
extern nil_t runtime_cleanup();
extern runtime_t runtime_get();
extern obj_t runtime_get_arg(str_t key);

#endif
