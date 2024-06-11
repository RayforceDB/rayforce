/*
 *   Copyright (c) 2024 Anton Kundenko <singaraiona@gmail.com>
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

#ifndef TERM_H
#define TERM_H

#include "rayforce.h"
#include <termios.h>

#define TERM_BUF_SIZE 1024

typedef struct history_t
{
    i64_t fd;
    str_p lines;
    u64_t size;
    u64_t pos;
    u64_t index;
    i64_t search_dir;
    i64_t curr_saved;
    u64_t curr_len;
    c8_t curr[TERM_BUF_SIZE];
} *history_p;

typedef struct term_t
{
    struct termios oldattr;
    struct termios newattr;
    i32_t buf_len;
    i32_t buf_pos;
    c8_t buf[TERM_BUF_SIZE];
    u64_t tabidx;
    history_p history;
} *term_p;

history_p history_create();
nil_t history_destroy(history_p history);
nil_t history_add(history_p history, c8_t buf[], u64_t len);
i64_t history_prev(history_p history, c8_t buf[]);
i64_t history_next(history_p history, c8_t buf[]);
i64_t history_save_current(history_p history, c8_t buf[], u64_t len);
i64_t history_restore_current(history_p history, c8_t buf[]);

term_p term_create();
nil_t term_prompt(term_p term);
nil_t term_destroy(term_p term);
obj_p term_read(term_p term);

#endif // TERM_H
