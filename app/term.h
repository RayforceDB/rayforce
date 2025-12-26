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

#include "../core/rayforce.h"
#if defined(OS_WINDOWS)
#include "../core/pool.h"
#define KEYCODE_RETURN '\r'
#else
#include <termios.h>
// Undefine macros from ncurses term.h that conflict with our code
#ifdef lines
#undef lines
#endif
#ifdef columns
#undef columns
#endif
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
#define KEYCODE_CTRL_A 0x01
#define KEYCODE_CTRL_B 0x02
#define KEYCODE_CTRL_C 0x03
#define KEYCODE_CTRL_D 0x04
#define KEYCODE_CTRL_E 0x05
#define KEYCODE_CTRL_F 0x06
#define KEYCODE_CTRL_K 0x0b
#define KEYCODE_CTRL_N 0x0e
#define KEYCODE_CTRL_P 0x10
#define KEYCODE_CTRL_U 0x15
#define KEYCODE_CTRL_W 0x17
#define KEYCODE_LPAREN '('
#define KEYCODE_RPAREN ')'
#define KEYCODE_LCURLY '{'
#define KEYCODE_RCURLY '}'
#define KEYCODE_SQUOTE '\''
#define KEYCODE_DQUOTE '"'
#define KEYCODE_LBRACKET '['
#define KEYCODE_RBRACKET ']'

#define TERM_BUF_SIZE 4096

typedef struct hist_t {
    i64_t fd;
    str_p entries;
    i64_t size;
    i64_t pos;
    i64_t index;
    i64_t search_dir;
    i64_t curr_saved;
    i64_t curr_len;
    c8_t curr[TERM_BUF_SIZE];
    // Line tracking for error source references
    i64_t line_count;       // Number of lines in history (1-based for display)
    i64_t *line_offsets;    // Byte offset of each line start (dynamically allocated)
    i64_t line_offsets_cap; // Capacity of line_offsets array
} *hist_p;

typedef struct {
    i32_t pos;
    c8_t type;
} paren_t;

typedef struct {
    i64_t entry;
    i64_t index;
    i64_t sbidx;
} autocp_idx_t;

typedef struct term_t {
#if defined(OS_WINDOWS)
    HANDLE h_stdin;
    HANDLE h_stdout;
    DWORD old_stdin_mode;
    DWORD new_stdin_mode;
    DWORD old_stdout_mode;
    DWORD new_stdout_mode;
    mutex_t lock;
#else
    struct termios oldattr;  // Store the old terminal attributes
    struct termios newattr;  // Store the new terminal attributes
#endif
    i32_t input_len;
    c8_t input[8];
    i32_t buf_len;
    i32_t buf_pos;
    c8_t buf[TERM_BUF_SIZE];
    i32_t multiline_len;                // Length of accumulated multi-line input
    c8_t multiline_buf[TERM_BUF_SIZE];  // Accumulated multi-line input
    autocp_idx_t autocp_idx;
    i32_t autocp_buf_len;
    i32_t autocp_buf_pos;
    c8_t autocp_buf[TERM_BUF_SIZE];
    hist_p hist;
    i32_t term_width;       // Terminal width in columns
    i32_t term_height;      // Terminal height in rows
    i32_t prompt_len;       // Length of the prompt (for wrapping calculation)
    i32_t last_total_rows;  // Number of rows used in last redraw
    i32_t last_cursor_row;  // Cursor row position from last redraw
    i64_t last_input_line;  // Line number of last input (for error traces)
} *term_p;

hist_p hist_create();
nil_t hist_destroy(hist_p history);
nil_t hist_add(hist_p hist, c8_t buf[], i64_t len);
i64_t hist_prev(hist_p hist, c8_t buf[]);
i64_t hist_next(hist_p hist, c8_t buf[]);
i64_t hist_save_current(hist_p hist, c8_t buf[], i64_t len);
i64_t hist_restore_current(hist_p hist, c8_t buf[]);

// Get current line number (for error traces)
i64_t hist_line_number(hist_p hist);
// Get source for a given line number (returns pointer into mmaped buffer)
lit_p hist_get_source(hist_p hist, i64_t line, i64_t *len);
// Get the line number of the last input (for passing to eval)
i64_t term_last_input_line(term_p term);

term_p term_create();
nil_t term_prompt(term_p term);
nil_t term_continuation_prompt(term_p term);
nil_t term_destroy(term_p term);
i64_t term_getc(term_p term);
obj_p term_read(term_p term);

nil_t cursor_move_start();
nil_t cursor_move_left(i32_t i);
nil_t cursor_move_right(i32_t i);
nil_t cursor_move_up(i32_t i);
nil_t cursor_move_down(i32_t i);
nil_t line_clear();
nil_t line_clear_below();
nil_t line_new();
nil_t cursor_hide();
nil_t cursor_show();
nil_t autocp_reset_current(term_p term);
nil_t term_get_size(term_p term);
i32_t term_visual_width(const c8_t *str, i32_t len);
nil_t term_goto_position(term_p term, i32_t from_pos, i32_t to_pos);

#endif  // TERM_H
