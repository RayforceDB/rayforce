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

#ifndef RAY_TERM_H
#define RAY_TERM_H

#include <rayforce.h>

#if defined(RAY_OS_WINDOWS)
#include <windows.h>
#define KEYCODE_RETURN '\r'
#else
#include <termios.h>
#define KEYCODE_RETURN '\n'
#endif

#define KEYCODE_BACKSPACE '\b'
#define KEYCODE_DELETE    0x7f
#define KEYCODE_TAB       '\t'
#define KEYCODE_UP        'A'
#define KEYCODE_DOWN      'B'
#define KEYCODE_LEFT      'D'
#define KEYCODE_RIGHT     'C'
#define KEYCODE_HOME      'H'
#define KEYCODE_END       'F'
#define KEYCODE_ESCAPE    0x1b
#define KEYCODE_CTRL_A    0x01
#define KEYCODE_CTRL_B    0x02
#define KEYCODE_CTRL_C    0x03
#define KEYCODE_CTRL_D    0x04
#define KEYCODE_CTRL_E    0x05
#define KEYCODE_CTRL_F    0x06
#define KEYCODE_CTRL_K    0x0b
#define KEYCODE_CTRL_N    0x0e
#define KEYCODE_CTRL_P    0x10
#define KEYCODE_CTRL_R    0x12
#define KEYCODE_CTRL_U    0x15
#define KEYCODE_CTRL_W    0x17

#define TERM_BUF_SIZE 4096
#define HIST_DEFAULT_CAP 256

typedef struct ray_hist {
    char**   entries;
    int32_t  count;
    int32_t  capacity;
    int32_t  index;
    int32_t  curr_saved;
    char     curr[TERM_BUF_SIZE];
    int32_t  curr_len;
} ray_hist_t;

typedef struct ray_term {
    ray_t*    _block;
#if defined(RAY_OS_WINDOWS)
    HANDLE   h_stdin;
    HANDLE   h_stdout;
    DWORD    old_stdin_mode;
    DWORD    old_stdout_mode;
#else
    struct termios oldattr;
    struct termios newattr;
#endif
    int32_t  input_len;
    char     input[8];
    int32_t  buf_len;
    int32_t  buf_pos;
    char     buf[TERM_BUF_SIZE];
    int32_t  term_width;
    int32_t  term_height;
    int32_t  prompt_len;
    /* Optional prefix shown before the standard `‣` prompt (e.g. the
     * remote host:port when in remote-REPL mode).  Empty by default.
     * Set via ray_term_set_prompt_prefix(); both the byte string and
     * its rendered visual width must be in sync — visual is what the
     * line editor uses for cursor math. */
    char     prompt_prefix[80];
    int32_t  prompt_prefix_len;
    int32_t  prompt_prefix_vis;
    int32_t  last_total_rows;
    int32_t  last_cursor_row;
    ray_hist_t hist;
    int32_t  search_mode;
    char     search_buf[256];
    int32_t  search_len;
    int32_t  search_match_idx;
    /* Ghost text (inline completion suggestion) */
    char     ghost[TERM_BUF_SIZE];
    int32_t  ghost_len;
    int32_t  ghost_word_start; /* position in buf where the completed word starts */
    int32_t  ghost_word_len;   /* length of the prefix that was matched */
    /* Multi-source completion candidates */
    const char* comp_items[256];  /* borrowed pointers — valid until next collect */
    int32_t     comp_count;
    /* Tab-cycle completion state */
    int32_t     comp_cycling;     /* 1 if currently cycling completions */
    int32_t     comp_cycle_idx;   /* index into comp_items for current cycle */
    int32_t     comp_cycle_start; /* buf position where cycled word starts */
    int32_t     comp_cycle_len;   /* length of currently inserted completion */
    /* Scratch buffer for null-terminated completion word copies */
    char        comp_scratch[TERM_BUF_SIZE];
    int32_t     comp_scratch_len;
    /* Multi-line input state */
    char        multiline_buf[TERM_BUF_SIZE];
    int32_t     multiline_len;
    /* Escape sequence state machine (for event-driven feed) */
    int32_t     esc_state;     /* 0=normal, 1=ESC, 2=ESC[, 3=ESCO, 4=ESC[3, 5=unknown CSI */
    int32_t     esc_buf_len;   /* bytes accumulated in unknown CSI sequence */
} ray_term_t;

ray_term_t* ray_term_create(void);
void       ray_term_destroy(ray_term_t* term);
int64_t    ray_term_getc(ray_term_t* term);
void       ray_term_get_size(ray_term_t* term);

void ray_cursor_move_start(void);
void ray_cursor_move_left(int32_t n);
void ray_cursor_move_right(int32_t n);
void ray_cursor_move_up(int32_t n);
void ray_cursor_move_down(int32_t n);
void ray_line_clear(void);
void ray_line_clear_below(void);
void ray_cursor_hide(void);
void ray_cursor_show(void);

int32_t ray_term_visual_width(const char* str, int32_t len);
void    ray_term_goto_position(ray_term_t* term, int32_t from_pos, int32_t to_pos);

ray_t*  ray_term_read(ray_term_t* term);
void   ray_term_redraw(ray_term_t* term);
void   ray_term_prompt(ray_term_t* term);

/* Set (or clear, when prefix == NULL or empty) the prompt prefix.
 * Used by the remote-REPL session to put the server address ahead
 * of `‣` so the user can never mistake it for a local prompt.  The
 * prefix bytes are copied into the term struct; caller may free
 * after the call. */
void   ray_term_set_prompt_prefix(ray_term_t* term, const char* prefix);

/* Event-driven terminal API — split ray_term_read into begin + feed.
 * ray_term_begin: show prompt, reset line state.
 * ray_term_feed:  process one byte from term->input[0].
 *   Returns ray_t* string when a complete line is ready,
 *   NULL when more input is needed,
 *   RAY_TERM_EOF on EOF/Ctrl-D at empty buffer. */
#define RAY_TERM_EOF ((ray_t*)(uintptr_t)1)
void    ray_term_begin(ray_term_t* term);
ray_t*  ray_term_feed(ray_term_t* term);

void    ray_hist_create(ray_hist_t* hist);
void    ray_hist_destroy(ray_hist_t* hist);
void    ray_hist_add(ray_hist_t* hist, const char* buf, int32_t len);
int32_t ray_hist_prev(ray_hist_t* hist, char* buf, int32_t buf_len);
int32_t ray_hist_next(ray_hist_t* hist, char* buf);
void    ray_hist_load(ray_hist_t* hist, const char* path);
void    ray_hist_save(ray_hist_t* hist, const char* path);
int32_t ray_hist_search(ray_hist_t* hist, const char* needle, int32_t needle_len,
                       int32_t start_idx);

int32_t ray_term_find_matching_paren(const char* buf, int32_t buf_len,
                                    int32_t cursor_pos);

/* Collect completion candidates from all sources (env, keywords, columns,
 * history words).  Stores results in term->comp_items / comp_count.
 * prefix/prefix_len is the word being completed. */
void ray_term_collect_completions(ray_term_t* term, const char* prefix,
                                 int32_t prefix_len);


/* Multi-line input: count unmatched opening brackets in multiline_buf + buf */
int32_t ray_term_count_unmatched(ray_term_t* term);
void    ray_term_continuation_prompt(ray_term_t* term);

/* Signal handling — install handlers to restore terminal on exit */
void ray_term_install_signals(ray_term_t* term);

/* Global interrupt flag — set by SIGINT handler, checked by eval loop */
int  ray_term_interrupted(void);
void ray_term_clear_interrupt(void);

/* Temporarily enable ISIG so Ctrl-C generates SIGINT during eval.
 * Call ray_term_eval_end() to restore raw mode after eval returns. */
void ray_term_eval_begin(ray_term_t* term);
void ray_term_eval_end(ray_term_t* term);

#define HIST_MAX_ENTRIES 1000
#define HIST_DEFAULT_PATH ".rayforce_history"

#endif /* RAY_TERM_H */
