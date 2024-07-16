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
#if defined(OS_WINDOWS)
#include "pool.h"
#define KEYCODE_RETURN '\r'
#else
#include <termios.h>
#define KEYCODE_RETURN '\n'
#endif
#define KEYCODE_BACKSPACE '\b'
#define KEYCODE_DELETE 0x7f
#define KEYCODE_TAB '\t'
#define KEYCODE_UP 'A'
#define KEYCODE_DOWN 'B'
#define KEYCODE_LEFT 'D'
#define KEYCODE_RIGHT 'C'
#define KEYCODE_HOME 'H'
#define KEYCODE_END 'F'
#define KEYCODE_ESCAPE 0x1b
#define KEYCODE_CTRL_C 0x03
#define KEYCODE_LPAREN '('
#define KEYCODE_RPAREN ')'
#define KEYCODE_LCURLY '{'
#define KEYCODE_RCURLY '}'
#define KEYCODE_SQUOTE '\''
#define KEYCODE_DQUOTE '"'
#define KEYCODE_LBRACKET '['
#define KEYCODE_RBRACKET ']'

#define TERM_BUF_SIZE 4096

typedef struct hist_t
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
} *hist_p;

typedef struct
{
    i32_t pos;
    c8_t type;
} paren_t;

typedef struct
{
    u64_t entry;
    u64_t index;
    u64_t sbidx;
} autocp_idx_t;

typedef struct term_t
{
#if defined(OS_WINDOWS)
    HANDLE h_stdin;
    HANDLE h_stdout;
    DWORD old_stdin_mode;
    DWORD new_stdin_mode;
    DWORD old_stdout_mode;
    DWORD new_stdout_mode;
    mutex_t lock;
#else
    struct termios oldattr; // Store the old terminal attributes
    struct termios newattr; // Store the new terminal attributes
#endif
    i32_t input_len;
    c8_t input[8];
    i32_t buf_len;
    i32_t buf_pos;
    c8_t buf[TERM_BUF_SIZE];
    autocp_idx_t autocp_idx;
    hist_p hist;
} *term_p;

hist_p hist_create();
nil_t hist_destroy(hist_p history);
nil_t hist_add(hist_p hist, c8_t buf[], u64_t len);
i64_t hist_prev(hist_p hist, c8_t buf[]);
i64_t hist_next(hist_p hist, c8_t buf[]);
i64_t hist_save_current(hist_p hist, c8_t buf[], u64_t len);
i64_t hist_restore_current(hist_p hist, c8_t buf[]);

term_p term_create();
nil_t term_prompt(term_p term);
nil_t term_destroy(term_p term);
i64_t term_getc(term_p term);
obj_p term_read(term_p term);

#endif // TERM_H
