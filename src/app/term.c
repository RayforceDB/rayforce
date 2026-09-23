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

#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#elif !defined(RAY_OS_WINDOWS)
#define _GNU_SOURCE
#endif


#include "app/term.h"
#include "lang/env.h"
#include "lang/eval.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

#if defined(RAY_OS_WINDOWS)
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#define hist_open(p, f, m)  _open((p), (f), (m))
#define hist_read(fd, b, n) _read((fd), (b), (unsigned)(n))
#define hist_write(fd, b, n) _write((fd), (b), (unsigned)(n))
#define hist_close(fd)      _close(fd)
#define hist_fstat(fd, st)  _fstat((fd), (st))
typedef struct _stat hist_stat_t;
#define HIST_PATH_SEP       '\\'
#else
#include <unistd.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/stat.h>
#define hist_open(p, f, m)  open((p), (f), (m))
#define hist_read(fd, b, n) read((fd), (b), (n))
#define hist_write(fd, b, n) do { ssize_t r_ = write((fd), (b), (n)); (void)r_; } while(0)
#define hist_close(fd)      close(fd)
#define hist_fstat(fd, st)  fstat((fd), (st))
typedef struct stat hist_stat_t;
#define HIST_PATH_SEP       '/'
#endif

/* Recover ray_t* block pointer from a ray_data() result pointer.
 * ray_data() returns bytes immediately after the 32-byte ray_t header. */
#define RAY_BLOCK_FROM_DATA(ptr) ((ray_t*)((char*)(ptr) - sizeof(ray_t)))

/* Suppress -Wunused-result for terminal I/O writes to stdout. */
#if defined(RAY_OS_WINDOWS)
static inline void term_write(const void* buf, size_t len) {
    int r = _write(1, buf, (unsigned)len);
    (void)r;
}
#else
static inline void term_write(const void* buf, size_t len) {
    ssize_t r = write(STDOUT_FILENO, buf, len);
    (void)r;
}
#endif

/* ===== Signal handling ===== */

static volatile sig_atomic_t g_interrupted = 0;
static ray_term_t* g_active_term = NULL;

static void signal_handler(int sig) {
    g_interrupted = 1;
    ray_eval_request_interrupt();
#if defined(RAY_OS_WINDOWS)
    if (sig == SIGTERM) {
#else
    if (sig == SIGTERM || sig == SIGQUIT) {
#endif
        /* Restore terminal and exit for fatal signals */
        if (g_active_term) {
#if defined(RAY_OS_WINDOWS)
            SetConsoleMode(g_active_term->h_stdin,  g_active_term->old_stdin_mode);
            SetConsoleMode(g_active_term->h_stdout, g_active_term->old_stdout_mode);
#else
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_active_term->oldattr);
#endif
        }
        /* Re-raise with default handler to get correct exit status */
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

static void atexit_handler(void) {
    if (g_active_term) {
#if defined(RAY_OS_WINDOWS)
        SetConsoleMode(g_active_term->h_stdin,  g_active_term->old_stdin_mode);
        SetConsoleMode(g_active_term->h_stdout, g_active_term->old_stdout_mode);
#else
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_active_term->oldattr);
#endif
        g_active_term = NULL;
    }
}

int ray_term_interrupted(void) {
    return g_interrupted != 0;
}

void ray_term_clear_interrupt(void) {
    g_interrupted = 0;
}

void ray_term_install_signals(ray_term_t* term) {
    static int atexit_registered = 0;
    g_active_term = term;
    if (!atexit_registered) {
        atexit(atexit_handler);
        atexit_registered = 1;
    }

#if defined(RAY_OS_WINDOWS)
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
#endif
}

void ray_term_eval_begin(ray_term_t* term) {
#if !defined(RAY_OS_WINDOWS)
    /* Enable ISIG so Ctrl-C generates SIGINT during evaluation */
    struct termios tio;
    tcgetattr(STDIN_FILENO, &tio);
    tio.c_lflag |= ISIG;
    tcsetattr(STDIN_FILENO, TCSANOW, &tio);
#endif
    (void)term;
}

void ray_term_eval_end(ray_term_t* term) {
#if !defined(RAY_OS_WINDOWS)
    /* Restore raw mode (ISIG off) for input handling */
    tcsetattr(STDIN_FILENO, TCSANOW, &term->newattr);
#endif
    (void)term;
}

/* ===== Cursor helpers ===== */

void ray_cursor_move_start(void) { putchar('\r'); }
void ray_cursor_move_left(int32_t n)  { if (n > 0) printf("\033[%dD", n); }
void ray_cursor_move_right(int32_t n) { if (n > 0) printf("\033[%dC", n); }
void ray_cursor_move_up(int32_t n)    { if (n > 0) printf("\033[%dA", n); }
void ray_cursor_move_down(int32_t n)  { if (n > 0) printf("\033[%dB", n); }
void ray_line_clear(void)       { printf("\r\033[K"); }
void ray_line_clear_below(void) { printf("\033[J"); }
void ray_cursor_hide(void)      { printf("\033[?25l"); }
void ray_cursor_show(void)      { printf("\033[?25h"); }

/* ===== Terminal size ===== */

void ray_term_get_size(ray_term_t* term) {
#if defined(RAY_OS_WINDOWS)
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(term->h_stdout, &csbi)) {
        term->term_width  = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        term->term_height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    } else {
        term->term_width  = 80;
        term->term_height = 24;
    }
#else
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        term->term_width  = w.ws_col;
        term->term_height = w.ws_row;
    } else {
        term->term_width  = 80;
        term->term_height = 24;
    }
#endif
}

/* ===== Visual width ===== */

int32_t ray_term_visual_width(const char* str, int32_t len) {
    int32_t width = 0;
    /* 0=normal, 1=just saw ESC (next byte is the CSI/SS3 introducer:
     *   '[' or 'O', or rarely a 1-byte sequence), 2=in CSI parameter
     *   bytes; terminate on final byte in [0x40,0x7E].  Without the
     *   intermediate state the introducer '[' (0x5B) would itself be
     *   mistaken for a final byte and end the escape one char early —
     *   the SGR digits/`;`/`m` then leak into width counting. */
    int32_t in_escape = 0;

    for (int32_t i = 0; i < len; i++) {
        if (str[i] == '\033') {
            in_escape = 1;
        } else if (in_escape == 1) {
            /* Introducer byte after ESC.  Standard CSI uses '[',
             * SS3 uses 'O'; both transition to body state.  Any
             * other byte is treated as a 2-byte ESC sequence and
             * we exit the escape state immediately. */
            if (str[i] == '[' || str[i] == 'O') {
                in_escape = 2;
            } else {
                in_escape = 0;
            }
        } else if (in_escape == 2) {
            if (str[i] >= 0x40 && str[i] <= 0x7E) {
                in_escape = 0;
            }
        } else {
            unsigned char c = (unsigned char)str[i];
            if ((c & 0x80) == 0) {
                width++;
            } else if ((c & 0xE0) == 0xC0) {
                width++;
            } else if ((c & 0xF0) == 0xE0) {
                width++;
            } else if ((c & 0xF8) == 0xF0) {
                width += 2;
            }
        }
    }
    return width;
}

/* ===== Cursor positioning ===== */

void ray_term_goto_position(ray_term_t* term, int32_t from_pos, int32_t to_pos) {
    if (term->term_width <= 0)
        return;

    int32_t from_total = term->prompt_len + ray_term_visual_width(term->buf, from_pos);
    int32_t from_row = from_total / term->term_width;
    int32_t from_col = from_total % term->term_width;

    int32_t to_total = term->prompt_len + ray_term_visual_width(term->buf, to_pos);
    int32_t to_row = to_total / term->term_width;
    int32_t to_col = to_total % term->term_width;

    int32_t row_diff = to_row - from_row;
    int32_t col_diff = to_col - from_col;

    if (row_diff < 0) ray_cursor_move_up(-row_diff);
    else if (row_diff > 0) ray_cursor_move_down(row_diff);

    if (col_diff < 0) ray_cursor_move_left(-col_diff);
    else if (col_diff > 0) ray_cursor_move_right(col_diff);

    term->last_cursor_row = to_row;
}

/* ===== Terminal create / destroy ===== */

#if defined(RAY_OS_WINDOWS)

ray_term_t* ray_term_create(void) {
    ray_t* block = ray_alloc(sizeof(ray_term_t));
    if (!block) return NULL;
    ray_term_t* term = (ray_term_t*)ray_data(block);
    memset(term, 0, sizeof(*term));
    term->_block = block;

    term->h_stdin  = GetStdHandle(STD_INPUT_HANDLE);
    term->h_stdout = GetStdHandle(STD_OUTPUT_HANDLE);

    SetConsoleOutputCP(CP_UTF8);

    GetConsoleMode(term->h_stdin, &term->old_stdin_mode);
    DWORD mode = term->old_stdin_mode;
    mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    SetConsoleMode(term->h_stdin, mode);

    GetConsoleMode(term->h_stdout, &term->old_stdout_mode);
    SetConsoleMode(term->h_stdout, term->old_stdout_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    term->term_width  = 80;
    term->term_height = 24;
    term->last_total_rows = 1;
    ray_term_get_size(term);
    ray_hist_create(&term->hist);
    ray_hist_load(&term->hist, NULL);

    return term;
}

void ray_term_destroy(ray_term_t* term) {
    if (!term) return;
    if (g_active_term == term) g_active_term = NULL;
    ray_hist_save(&term->hist, NULL);
    ray_hist_destroy(&term->hist);
    SetConsoleMode(term->h_stdin,  term->old_stdin_mode);
    SetConsoleMode(term->h_stdout, term->old_stdout_mode);
    ray_free(term->_block);
}

int64_t ray_term_getc(ray_term_t* term) {
    char c;
    DWORD n;
    if (!ReadFile(term->h_stdin, &c, 1, &n, NULL))
        return -1;
    term->input[0] = c;
    return (int64_t)n;
}

#else /* Unix */

ray_term_t* ray_term_create(void) {
    ray_t* block = ray_alloc(sizeof(ray_term_t));
    if (!block) return NULL;
    ray_term_t* term = (ray_term_t*)ray_data(block);
    memset(term, 0, sizeof(*term));
    term->_block = block;

    tcgetattr(STDIN_FILENO, &term->oldattr);
    term->newattr = term->oldattr;
    term->newattr.c_lflag &= ~(ICANON | ECHO | ISIG);
    term->newattr.c_cc[VMIN]  = 1;
    term->newattr.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &term->newattr);

    term->term_width  = 80;
    term->term_height = 24;
    term->last_total_rows = 1;
    ray_term_get_size(term);
    ray_hist_create(&term->hist);
    ray_hist_load(&term->hist, NULL);

    return term;
}

void ray_term_destroy(ray_term_t* term) {
    if (!term) return;
    if (g_active_term == term) g_active_term = NULL;
    ray_hist_save(&term->hist, NULL);
    ray_hist_destroy(&term->hist);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &term->oldattr);
    ray_free(term->_block);
}

int64_t ray_term_getc(ray_term_t* term) {
    /* Simple blocking read — only called when poll confirms data ready,
     * or in fallback mode where VMIN=1 provides blocking behavior. */
    for (;;) {
        int64_t sz = (int64_t)read(STDIN_FILENO, term->input, 1);
        if (sz > 0) return sz;
        if (sz < 0 && errno == EINTR) {
            if (g_interrupted) return -2;
            continue;
        }
        if (sz == 0) return -1;  /* EOF */
        return sz;  /* real error */
    }
}

#endif /* _WIN32 */

/* ===== History ===== */

void ray_hist_create(ray_hist_t* hist) {
    hist->capacity = HIST_DEFAULT_CAP;
    ray_t* block = ray_alloc((int64_t)(hist->capacity * (int64_t)sizeof(char*)));
    if (!block) {
        hist->entries = NULL;
        hist->capacity = 0;
    } else {
        hist->entries = (char**)ray_data(block);
    }
    hist->count = 0;
    hist->index = 0;
    hist->curr_saved = 0;
    hist->curr_len = 0;
}

void ray_hist_destroy(ray_hist_t* hist) {
    if (!hist->entries) return;
    for (int32_t i = 0; i < hist->count; i++) {
        ray_t* block = RAY_BLOCK_FROM_DATA(hist->entries[i]);
        ray_free(block);
    }
    ray_t* block = RAY_BLOCK_FROM_DATA(hist->entries);
    ray_free(block);
    hist->entries = NULL;
    hist->count = 0;
}

void ray_hist_add(ray_hist_t* hist, const char* buf, int32_t len) {
    if (len <= 0) return;
    /* Skip if same as last entry */
    if (hist->count > 0) {
        const char* last = hist->entries[hist->count - 1];
        if ((int32_t)strlen(last) == len && memcmp(last, buf, (size_t)len) == 0)
            goto reset;
    }
    /* Evict oldest if at max entries */
    if (hist->count >= HIST_MAX_ENTRIES) {
        ray_t* old = RAY_BLOCK_FROM_DATA(hist->entries[0]);
        ray_free(old);
        memmove(hist->entries, hist->entries + 1,
                (size_t)(hist->count - 1) * sizeof(char*));
        hist->count--;
    }
    /* Grow if needed */
    if (hist->count >= hist->capacity) {
        int32_t new_cap = hist->capacity * 2;
        ray_t* new_block = ray_alloc((int64_t)(new_cap * (int64_t)sizeof(char*)));
        if (!new_block) goto reset;
        char** new_entries = (char**)ray_data(new_block);
        memcpy(new_entries, hist->entries, (size_t)(hist->count) * sizeof(char*));
        ray_t* old_block = RAY_BLOCK_FROM_DATA(hist->entries);
        ray_free(old_block);
        hist->entries = new_entries;
        hist->capacity = new_cap;
    }
    /* Allocate and copy the entry */
    ray_t* entry_block = ray_alloc((int64_t)(len + 1));
    if (!entry_block) goto reset;
    char* entry = (char*)ray_data(entry_block);
    memcpy(entry, buf, (size_t)len);
    entry[len] = '\0';
    hist->entries[hist->count++] = entry;

reset:
    hist->index = hist->count;
    hist->curr_saved = 0;
}

int32_t ray_hist_prev(ray_hist_t* hist, char* buf, int32_t buf_len) {
    if (hist->count == 0 || hist->index <= 0) return -1;
    /* Save current input on first navigation */
    if (!hist->curr_saved) {
        if (buf_len > TERM_BUF_SIZE - 1) buf_len = TERM_BUF_SIZE - 1;
        memcpy(hist->curr, buf, (size_t)buf_len);
        hist->curr_len = buf_len;
        hist->curr_saved = 1;
    }
    hist->index--;
    const char* entry = hist->entries[hist->index];
    int32_t len = (int32_t)strlen(entry);
    if (len > TERM_BUF_SIZE - 1) len = TERM_BUF_SIZE - 1;
    memcpy(buf, entry, (size_t)len);
    buf[len] = '\0';
    return len;
}

int32_t ray_hist_next(ray_hist_t* hist, char* buf) {
    if (hist->index >= hist->count) return -1;
    hist->index++;
    if (hist->index >= hist->count) {
        /* Restore saved current input */
        if (hist->curr_saved) {
            memcpy(buf, hist->curr, (size_t)hist->curr_len);
            buf[hist->curr_len] = '\0';
            hist->curr_saved = 0;
            return hist->curr_len;
        }
        return 0; /* empty buffer */
    }
    const char* entry = hist->entries[hist->index];
    int32_t len = (int32_t)strlen(entry);
    if (len > TERM_BUF_SIZE - 1) len = TERM_BUF_SIZE - 1;
    memcpy(buf, entry, (size_t)len);
    buf[len] = '\0';
    return len;
}

/* ===== History search ===== */

int32_t ray_hist_search(ray_hist_t* hist, const char* needle, int32_t needle_len,
                       int32_t start_idx) {
    if (needle_len <= 0 || hist->count == 0) return -1;
    if (start_idx < 0) return -1;
    if (start_idx >= hist->count) start_idx = hist->count - 1;

    for (int32_t i = start_idx; i >= 0; i--) {
        const char* entry = hist->entries[i];
        int32_t elen = (int32_t)strlen(entry);
        if (elen < needle_len) continue;
        /* Substring search */
        for (int32_t j = 0; j <= elen - needle_len; j++) {
            if (memcmp(entry + j, needle, (size_t)needle_len) == 0)
                return i;
        }
    }
    return -1;
}

/* ===== History persistence ===== */

static void hist_build_path(char* out, int32_t out_size) {
    const char* home = getenv("HOME");
#if defined(RAY_OS_WINDOWS)
    if (!home) home = getenv("USERPROFILE");
#endif
    if (!home) home = ".";
    snprintf(out, (size_t)out_size, "%s%c%s", home, HIST_PATH_SEP, HIST_DEFAULT_PATH);
}

void ray_hist_load(ray_hist_t* hist, const char* path) {
    char pathbuf[1024];
    if (!path) {
        hist_build_path(pathbuf, (int32_t)sizeof(pathbuf));
        path = pathbuf;
    }

    int fd = hist_open(path, O_RDONLY, 0);
    if (fd < 0) return;

    /* Get file size */
    hist_stat_t st;
    if (hist_fstat(fd, &st) != 0 || st.st_size == 0) {
        hist_close(fd);
        return;
    }

    /* Read entire file into a temp buffer */
    int64_t fsize = st.st_size;
    if (fsize > TERM_BUF_SIZE * 100) fsize = TERM_BUF_SIZE * 100; /* sanity cap */
    ray_t* fbuf_block = ray_alloc(fsize + 1);
    if (!fbuf_block) { hist_close(fd); return; }
    char* fbuf = (char*)ray_data(fbuf_block);

    int64_t total = 0;
    while (total < fsize) {
        int64_t n = hist_read(fd, fbuf + total, (size_t)(fsize - total));
        if (n <= 0) break;
        total += n;
    }
    hist_close(fd);

    /* Parse null-byte delimited entries */
    char* p = fbuf;
    char* end = fbuf + total;
    while (p < end) {
        char* entry_start = p;
        /* Find next null byte or end */
        while (p < end && *p != '\0') p++;
        int32_t len = (int32_t)(p - entry_start);
        if (len > 0)
            ray_hist_add(hist, entry_start, len);
        if (p < end) p++; /* skip null delimiter */
    }

    ray_free(fbuf_block);
}

void ray_hist_save(ray_hist_t* hist, const char* path) {
    char pathbuf[1024];
    if (!path) {
        hist_build_path(pathbuf, (int32_t)sizeof(pathbuf));
        path = pathbuf;
    }

    if (hist->count == 0) return;

    int fd = hist_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;

    /* Only save last HIST_MAX_ENTRIES entries */
    int32_t start = 0;
    if (hist->count > HIST_MAX_ENTRIES)
        start = hist->count - HIST_MAX_ENTRIES;

    for (int32_t i = start; i < hist->count; i++) {
        const char* entry = hist->entries[i];
        int32_t len = (int32_t)strlen(entry);
        hist_write(fd, entry, (size_t)len);
        if (i < hist->count - 1) {
            hist_write(fd, "\0", 1);
        }
    }
    /* Write trailing null so load knows where last entry ends */
    hist_write(fd, "\0", 1);

    hist_close(fd);
}

/* ===== UTF-8 helpers ===== */

static int32_t find_prev_utf8(const char* buf, int32_t pos) {
    if (pos == 0) return 0;
    pos--;
    while (pos > 0 && ((unsigned char)buf[pos] & 0xC0) == 0x80)
        pos--;
    return pos;
}

static int32_t find_next_utf8(const char* buf, int32_t pos, int32_t len) {
    if (pos >= len) return len;
    pos++;
    while (pos < len && ((unsigned char)buf[pos] & 0xC0) == 0x80)
        pos++;
    return pos;
}

/* ===== ANSI color constants ===== */

#define CLR_GREEN      "\033[1;32m"
#define CLR_YELLOW     "\033[1;33m"
#define CLR_GRAY       "\033[1;38;5;8m"
#define CLR_LIGHT_BLUE "\033[1;38;5;39m"
#define CLR_SALAD      "\033[1;38;5;118m"
#define CLR_RED        "\033[1;31m"
#define CLR_REVERSE    "\033[7m"
#define CLR_RESET      "\033[0m"
#define CLR_BACK_CYAN  "\033[46m"

/* ===== Syntax highlighting helpers ===== */

static int is_alphanum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '?' || c == '!';
}

static int is_op_char(char c) {
    return c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
           c == '<' || c == '>' || c == '=' || c == '!' || c == '&' || c == '|';
}

static int32_t term_word_left_pos(ray_term_t* term, int32_t pos) {
    while (pos > 0) {
        int32_t prev = find_prev_utf8(term->buf, pos);
        if (is_alphanum(term->buf[prev])) break;
        pos = prev;
    }
    while (pos > 0) {
        int32_t prev = find_prev_utf8(term->buf, pos);
        if (!is_alphanum(term->buf[prev])) break;
        pos = prev;
    }
    return pos;
}

static int32_t term_word_right_pos(ray_term_t* term, int32_t pos) {
    while (pos < term->buf_len && !is_alphanum(term->buf[pos]))
        pos = find_next_utf8(term->buf, pos, term->buf_len);
    while (pos < term->buf_len && is_alphanum(term->buf[pos]))
        pos = find_next_utf8(term->buf, pos, term->buf_len);
    return pos;
}

static void term_delete_range(ray_term_t* term, int32_t start, int32_t end) {
    if (start < 0) start = 0;
    if (end > term->buf_len) end = term->buf_len;
    if (start >= end) return;
    memmove(term->buf + start, term->buf + end, (size_t)(term->buf_len - end));
    term->buf_len -= end - start;
    if (term->buf_pos > end)
        term->buf_pos -= end - start;
    else if (term->buf_pos > start)
        term->buf_pos = start;
    term->buf[term->buf_len] = '\0';
}

static void term_move_word_left(ray_term_t* term) {
    int32_t next = term_word_left_pos(term, term->buf_pos);
    if (next != term->buf_pos) {
        term->buf_pos = next;
        term->comp_cycling = 0;
        ray_term_redraw(term);
    }
}

static void term_move_word_right(ray_term_t* term) {
    int32_t next = term_word_right_pos(term, term->buf_pos);
    if (next != term->buf_pos) {
        term->buf_pos = next;
        term->comp_cycling = 0;
        ray_term_redraw(term);
    }
}

static void term_delete_word_left(ray_term_t* term) {
    if (term->buf_pos <= 0) return;
    int32_t start = term_word_left_pos(term, term->buf_pos);
    term_delete_range(term, start, term->buf_pos);
    term->buf_pos = start;
    term->comp_cycling = 0;
    ray_term_redraw(term);
}

static void term_delete_word_right(ray_term_t* term) {
    if (term->buf_pos >= term->buf_len) return;
    int32_t end = term_word_right_pos(term, term->buf_pos);
    term_delete_range(term, term->buf_pos, end);
    term->comp_cycling = 0;
    ray_term_redraw(term);
}

static void term_transpose_chars(ray_term_t* term) {
    if (term->buf_len < 2 || term->buf_pos <= 0) return;

    int32_t left_start;
    int32_t right_start;
    int32_t right_end;

    if (term->buf_pos >= term->buf_len) {
        right_end = term->buf_len;
        right_start = find_prev_utf8(term->buf, right_end);
        left_start = find_prev_utf8(term->buf, right_start);
    } else {
        right_start = term->buf_pos;
        right_end = find_next_utf8(term->buf, right_start, term->buf_len);
        left_start = find_prev_utf8(term->buf, right_start);
    }

    int32_t left_len = right_start - left_start;
    int32_t right_len = right_end - right_start;
    if (left_len <= 0 || right_len <= 0 || left_len > 8 || right_len > 8)
        return;

    char left[8];
    memcpy(left, term->buf + left_start, (size_t)left_len);
    memmove(term->buf + left_start, term->buf + right_start, (size_t)right_len);
    memcpy(term->buf + left_start + right_len, left, (size_t)left_len);
    term->buf_pos = left_start + right_len + left_len;
    term->comp_cycling = 0;
    ray_term_redraw(term);
}

static void term_clear_screen(ray_term_t* term) {
    printf("\033[2J\033[H");
    term->last_total_rows = 1;
    ray_term_redraw(term);
}

/* ===== Bracket matching ===== */

static int is_open_bracket(char c) {
    return c == '(' || c == '[' || c == '{';
}

static int is_close_bracket(char c) {
    return c == ')' || c == ']' || c == '}';
}

static char opposite_bracket(char c) {
    switch (c) {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '{': return '}';
    case '}': return '{';
    default:  return 0;
    }
}

static int is_escaped(const char* buf, int32_t pos) {
    int bs = 0;
    while (pos - 1 - bs >= 0 && buf[pos - 1 - bs] == '\\') bs++;
    return bs % 2 != 0;
}

static int32_t term_find_unmatched_bracket(const char* buf, int32_t buf_len) {
    int32_t stack[TERM_BUF_SIZE];
    int32_t top = 0;
    int in_str = 0;

    for (int32_t i = 0; i < buf_len; i++) {
        char c = buf[i];
        if (in_str) {
            if (c == '\\' && i + 1 < buf_len) { i++; continue; }
            if (c == '"') in_str = 0;
            continue;
        }
        if (c == '"') { in_str = 1; continue; }
        if (c == ';') break;

        if (is_open_bracket(c)) {
            if (top < TERM_BUF_SIZE)
                stack[top++] = i;
        } else if (is_close_bracket(c)) {
            if (top <= 0)
                return i;
            char open = buf[stack[top - 1]];
            if (opposite_bracket(open) != c)
                return i;
            top--;
        }
    }

    return top > 0 ? stack[top - 1] : -1;
}

static int in_string_at(const char* buf, int32_t pos) {
    int in_str = 0;
    for (int32_t i = 0; i < pos; i++) {
        if (buf[i] == '"' && !is_escaped(buf, i))
            in_str = !in_str;
    }
    return in_str;
}

int32_t ray_term_find_matching_paren(const char* buf, int32_t buf_len,
                                    int32_t cursor_pos) {
    if (cursor_pos < 0 || cursor_pos >= buf_len)
        return -1;

    char c = buf[cursor_pos];
    if (!is_open_bracket(c) && !is_close_bracket(c))
        return -1;

    if (in_string_at(buf, cursor_pos))
        return -1;

    char target = opposite_bracket(c);
    int depth = 0;

    if (is_open_bracket(c)) {
        /* Scan forward, tracking string state incrementally */
        int in_str = 0;
        for (int32_t i = cursor_pos; i < buf_len; i++) {
            if (buf[i] == '"' && !is_escaped(buf, i)) {
                if (i > cursor_pos) in_str = !in_str;
            }
            if (in_str) continue;
            if (buf[i] == c) depth++;
            else if (buf[i] == target) {
                depth--;
                if (depth == 0) return i;
            }
        }
    } else {
        /* Scan backward: build string-state bitmap in one forward pass,
         * then use it for the backward scan.  O(n) total. */
        uint8_t str_map[TERM_BUF_SIZE];
        int s = 0;
        for (int32_t i = 0; i < buf_len; i++) {
            if (buf[i] == '"' && !is_escaped(buf, i))
                s = !s;
            str_map[i] = (uint8_t)s;
        }
        for (int32_t i = cursor_pos; i >= 0; i--) {
            // cppcheck-suppress uninitvar // entry guard ensures cursor_pos < buf_len, so the fill loop above covered [0, cursor_pos]
            if (str_map[i]) continue;
            if (buf[i] == c) depth++;
            else if (buf[i] == target) {
                depth--;
                if (depth == 0) return i;
            }
        }
    }

    return -1;
}

/* Write highlighted buffer content into dst. Returns bytes written. */
static int32_t term_highlight_into(char* dst, int32_t dst_cap,
                                   const char* buf, int32_t buf_len,
                                   int32_t match_pos1, int32_t match_pos2,
                                   int32_t bad_pos) {
    int32_t n = 0;

#define HL_APPEND(s, slen) do { \
    if (n + (slen) < dst_cap) { memcpy(dst + n, (s), (size_t)(slen)); n += (slen); } \
} while (0)
#define HL_LIT(s) HL_APPEND((s), (int32_t)strlen(s))

    for (int32_t i = 0; i < buf_len; i++) {
        char c = buf[i];
        int colored = 0;

        switch (c) {
        case '(': case ')': case '[': case ']': case '{': case '}':
            if (i == bad_pos) {
                HL_LIT(CLR_RED);
            } else if (i == match_pos1 || i == match_pos2) {
                HL_LIT(CLR_BACK_CYAN);
            } else {
                HL_LIT(CLR_GRAY);
            }
            HL_APPEND(&c, 1);
            HL_LIT(CLR_RESET);
            colored = 1;
            break;

        case ':':
            /* Dict key colon */
            HL_LIT(CLR_GRAY);
            HL_APPEND(&c, 1);
            HL_LIT(CLR_RESET);
            colored = 1;
            break;

        case '"': {
            /* String literal */
            int32_t j = i + 1;
            while (j < buf_len) {
                if (buf[j] == '"' && !is_escaped(buf, j)) {
                    j++;
                    break;
                }
                j++;
            }
            HL_LIT(CLR_YELLOW);
            HL_APPEND(buf + i, j - i);
            HL_LIT(CLR_RESET);
            i = j - 1;
            colored = 1;
            break;
        }

        case '\'': {
            /* Quoted symbol: 'name */
            int32_t j = i + 1;
            while (j < buf_len && is_alphanum(buf[j])) j++;
            if (j > i + 1) {
                HL_LIT(CLR_SALAD);
                HL_APPEND(buf + i, j - i);
                HL_LIT(CLR_RESET);
                i = j - 1;
                colored = 1;
            }
            break;
        }

        case ';': {
            /* Comment to end of line */
            int32_t j = i;
            while (j < buf_len && buf[j] != '\n') j++;
            HL_LIT(CLR_GRAY);
            HL_APPEND(buf + i, j - i);
            HL_LIT(CLR_RESET);
            i = j - 1;
            colored = 1;
            break;
        }

        default:
            /* Check for word at word boundary.  Also accepts a leading `.`
             * followed by alphanum so reserved-namespace builtins like
             * `.sys.gc` / `.csv.read` are scanned as one token instead
             * of three pieces (`.`, `sys`, `.`, `gc`).  Internal `.`
             * extends the word only when followed by another alphanum,
             * keeping `foo.` or `1.5)` from being mis-joined. */
            {
                int prev_ok = (i == 0 ||
                               (!is_alphanum(buf[i - 1]) && buf[i - 1] != '.'));
                int start_ok = is_alphanum(c) ||
                               (c == '.' && i + 1 < buf_len && is_alphanum(buf[i + 1]));
                if (prev_ok && start_ok) {
                    int32_t j = i + 1;
                    while (j < buf_len) {
                        if (is_alphanum(buf[j])) { j++; continue; }
                        if (buf[j] == '.' && j + 1 < buf_len &&
                            is_alphanum(buf[j + 1])) { j++; continue; }
                        break;
                    }
                    int32_t wlen = j - i;

                    /* Exact-match env check — prefix lookup with max=1
                     * returns only the first alphabetical match, which
                     * would misclassify e.g. `de` when `del`/`desc` sort
                     * earlier and hit the same prefix. */
                    if (ray_env_has_name(buf + i, wlen)) {
                        HL_LIT(CLR_GREEN);
                        HL_APPEND(buf + i, wlen);
                        HL_LIT(CLR_RESET);
                    } else {
                        /* Not a builtin — emit plain */
                        HL_APPEND(buf + i, wlen);
                    }
                    i = j - 1;
                    colored = 1;
                    break;
                }
            }
            if (is_op_char(c)) {
                /* Check operator is standing alone (not part of a word) */
                int prev_alnum = (i > 0 && is_alphanum(buf[i - 1]));
                int next_alnum = (i + 1 < buf_len && is_alphanum(buf[i + 1]));
                if (!prev_alnum && !next_alnum) {
                    HL_LIT(CLR_LIGHT_BLUE);
                    HL_APPEND(&c, 1);
                    HL_LIT(CLR_RESET);
                    colored = 1;
                }
            }
            break;
        }

        if (!colored) {
            HL_APPEND(&c, 1);
        }
    }

#undef HL_LIT
#undef HL_APPEND

    return n;
}

/* ===== Ghost text (inline completion) ===== */

/* Find the start of the word at/before the cursor.  Identifiers may
 * include internal `.` (as in user `math.pi` or reserved `.sys.gc`) —
 * a dot counts as part of the word if alphanum flanks it, or if it's
 * the leading `.` of a reserved-namespace name followed by alphanum. */
static int32_t find_word_start(const char* buf, int32_t pos) {
    int32_t i = pos;
    while (i > 0) {
        char prev = buf[i - 1];
        if (is_alphanum(prev)) { i--; continue; }
        if (prev == '.') {
            if (i - 1 == 0) { i--; break; }              /* leading dot at buf[0] */
            if (is_alphanum(buf[i - 2])) { i--; continue; } /* internal dot */
            i--; break;                                  /* leading dot after punct */
        }
        break;
    }
    return i;
}

/* Compute ghost text suggestion based on the word at cursor.
 * Sets term->ghost, ghost_len, ghost_word_start, ghost_word_len.
 * Ghost text is the REMAINING part of the best match (after the typed prefix).
 * Also populates term->comp_items/comp_count via collect_completions. */
static void ray_term_update_ghost(ray_term_t* term) {
    term->ghost_len = 0;

    /* While Tab cycling is active, leave comp_items/comp_count alone —
     * the user is rotating through a fixed candidate set and a fresh
     * collect (against the now-completed word) would either clobber
     * with zero entries or with a different list, both of which break
     * the next Tab's modulo.  Just suppress ghost text. */
    if (term->comp_cycling)
        return;

    /* Only show ghost at end of buffer or end of a word */
    if (term->buf_pos != term->buf_len) {
        term->comp_count = 0;
        return;
    }
    if (term->buf_len == 0) {
        term->comp_count = 0;
        return;
    }

    /* Extract word before cursor */
    int32_t ws = find_word_start(term->buf, term->buf_pos);
    int32_t wlen = term->buf_pos - ws;
    if (wlen <= 0) {
        term->comp_count = 0;
        return;
    }

    /* Keep the word span in sync with comp_items on EVERY path that
     * leaves comp_count > 0.  Tab starts a completion cycle from
     * ghost_word_start/len whenever comp_count >= 2 — if these still
     * described a word from a previous (longer) line, comp_cycle_insert
     * would compute a negative tail and memmove with a huge size_t. */
    term->ghost_word_start = ws;
    term->ghost_word_len = wlen;

    /* Collect completions from all sources */
    ray_term_collect_completions(term, term->buf + ws, wlen);
    if (term->comp_count <= 0)
        return;

    /* Use the first (alphabetically) match */
    const char* match = term->comp_items[0];
    int32_t mlen = (int32_t)strlen(match);
    if (mlen <= wlen)
        return; /* already fully typed */

    /* Ghost = the remaining characters */
    int32_t remaining = mlen - wlen;
    if (remaining >= TERM_BUF_SIZE)
        remaining = TERM_BUF_SIZE - 1;
    memcpy(term->ghost, match + wlen, (size_t)remaining);
    term->ghost_len = remaining;
}

/* Accept ghost text into the buffer */
static void ray_term_accept_ghost(ray_term_t* term) {
    if (term->ghost_len <= 0)
        return;
    if (term->buf_len + term->ghost_len >= TERM_BUF_SIZE)
        return;

    memcpy(term->buf + term->buf_len, term->ghost, (size_t)term->ghost_len);
    term->buf_len += term->ghost_len;
    term->buf_pos = term->buf_len;
    term->ghost_len = 0;
}

/* ===== Multi-source completion collection ===== */

/* Max completion candidates stored in ray_term_t::comp_items */
#define COMP_MAX 256
#define COMP_SRC_ENV  1
#define COMP_SRC_COL  2
#define COMP_SRC_HIST 3

static void comp_sort_candidates(ray_term_t* term) {
    for (int32_t i = 1; i < term->comp_count; i++) {
        const char* item = term->comp_items[i];
        int32_t src = term->comp_sources[i];
        int32_t j = i - 1;
        while (j >= 0 && strcmp(term->comp_items[j], item) > 0) {
            term->comp_items[j + 1] = term->comp_items[j];
            term->comp_sources[j + 1] = term->comp_sources[j];
            j--;
        }
        term->comp_items[j + 1] = item;
        term->comp_sources[j + 1] = src;
    }
}

/* Check if name is already in results[0..count) */
static int comp_has(const char** results, int32_t count, const char* name) {
    for (int32_t i = 0; i < count; i++) {
        if (strcmp(results[i], name) == 0) return 1;
    }
    return 0;
}

static int comp_name_char(char c) {
    return is_alphanum(c) || c == '.';
}

static int comp_table_from_at(const char* buf, int32_t buf_len,
                              int32_t pos, int32_t* name_start,
                              int32_t* name_len) {
    int32_t j = pos + 5;
    while (j < buf_len && (buf[j] == ' ' || buf[j] == '\t')) j++;
    if (j < buf_len && buf[j] == '\'') j++;
    if (j >= buf_len || !comp_name_char(buf[j])) return 0;
    *name_start = j;
    while (j < buf_len && comp_name_char(buf[j])) j++;
    *name_len = j - *name_start;
    return *name_len > 0;
}

static ray_t* comp_find_from_table_range(const char* buf, int32_t buf_len,
                                         int32_t start, int32_t end,
                                         int32_t cursor_pos) {
    ray_t* best = NULL;
    int32_t best_dist = INT32_MAX;
    int in_str = 0;

    if (start < 0) start = 0;
    if (end > buf_len) end = buf_len;
    if (cursor_pos < start) cursor_pos = start;
    if (cursor_pos > end) cursor_pos = end;

    for (int32_t i = start; i + 5 <= end; i++) {
        char c = buf[i];
        if (in_str) {
            if (c == '\\' && i + 1 < end) { i++; continue; }
            if (c == '"') in_str = 0;
            continue;
        }
        if (c == '"') { in_str = 1; continue; }
        if (c == ';') break;
        if (memcmp(buf + i, "from:", 5) != 0) continue;

        int32_t nstart = 0;
        int32_t nlen = 0;
        if (!comp_table_from_at(buf, end, i, &nstart, &nlen)) continue;
        /* Look up the name (read-only) and check env */
        int64_t sym = ray_sym_find(buf + nstart, (size_t)nlen);
        if (sym < 0) continue;
        ray_t* val = ray_env_get(sym);
        if (val && val->type == RAY_TABLE) {
            int32_t dist = cursor_pos >= i ? cursor_pos - i : i - cursor_pos;
            if (!best || dist < best_dist) {
                best = val;
                best_dist = dist;
            }
        }
    }

    return best;
}

static int comp_find_query_scope(const char* buf, int32_t buf_len,
                                 int32_t cursor_pos,
                                 int32_t* start, int32_t* end) {
    int32_t stack[64];
    int32_t top = 0;
    int in_str = 0;

    if (cursor_pos < 0) cursor_pos = 0;
    if (cursor_pos > buf_len) cursor_pos = buf_len;

    for (int32_t i = 0; i < cursor_pos; i++) {
        char c = buf[i];
        if (in_str) {
            if (c == '\\' && i + 1 < cursor_pos) { i++; continue; }
            if (c == '"') in_str = 0;
            continue;
        }
        if (c == '"') { in_str = 1; continue; }
        if (c == ';') break;
        if (c == '{') {
            if (top < (int32_t)(sizeof(stack) / sizeof(stack[0])))
                stack[top++] = i;
        } else if (c == '}' && top > 0) {
            top--;
        }
    }

    if (top <= 0)
        return 0;

    *start = stack[0];
    int32_t depth = 0;
    in_str = 0;
    for (int32_t i = *start; i < buf_len; i++) {
        char c = buf[i];
        if (in_str) {
            if (c == '\\' && i + 1 < buf_len) { i++; continue; }
            if (c == '"') in_str = 0;
            continue;
        }
        if (c == '"') { in_str = 1; continue; }
        if (c == ';') break;
        if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) {
                *end = i + 1;
                return 1;
            }
        }
    }
    *end = buf_len;
    return 1;
}

/* Try to extract the active query table from the current buffer.
 * Returns the env value (a table) or NULL. */
static ray_t* comp_find_from_table(ray_term_t* term) {
    int32_t start = 0;
    int32_t end = term->buf_len;
    if (comp_find_query_scope(term->buf, term->buf_len, term->buf_pos,
                              &start, &end)) {
        ray_t* scoped = comp_find_from_table_range(term->buf, term->buf_len,
                                                   start, end,
                                                   term->buf_pos);
        if (scoped) return scoped;
    }
    return comp_find_from_table_range(term->buf, term->buf_len, 0,
                                      term->buf_len, term->buf_pos);
}

void ray_term_collect_completions(ray_term_t* term, const char* prefix,
                                 int32_t prefix_len) {
    term->comp_count = 0;
    term->comp_scratch_len = 0;
    if (prefix_len <= 0) return;

    const char** out = term->comp_items;
    int32_t* srcs = term->comp_sources;
    int32_t cap = COMP_MAX;
    int32_t n = 0;

    /* Source 1: env builtins + user variables (already sorted) */
    {
        const char* env_results[128];
        int64_t ec = ray_env_lookup_prefix(prefix, (int64_t)prefix_len,
                                           env_results, 128);
        for (int64_t i = 0; i < ec && n < cap; i++) {
            out[n] = env_results[i];
            srcs[n++] = COMP_SRC_ENV;
        }
    }

    /* Source 2: static keywords (s_keywords[] is in env.c and already scanned
     * by ray_env_lookup_prefix, so nothing extra needed here — they are included
     * in source 1).  This is a no-op; the plan's "binary search on prefix" is
     * satisfied by env_lookup_prefix which already scans the keyword list. */

    /* Source 3: column names from a table referenced in the buffer */
    {
        ray_t* tbl = comp_find_from_table(term);
        if (tbl) {
            int64_t ncols = ray_table_ncols(tbl);
            for (int64_t ci = 0; ci < ncols && n < cap; ci++) {
                int64_t sym = ray_table_col_name(tbl, ci);
                if (sym < 0) continue;
                ray_t* s = ray_sym_str(sym);
                if (!s) continue;
                const char* cname = ray_str_ptr(s);
                if (!cname) continue;
                int64_t clen = (int64_t)strlen(cname);
                if (clen >= prefix_len &&
                    strncmp(cname, prefix, (size_t)prefix_len) == 0 &&
                    !comp_has(out, n, cname)) {
                    out[n] = cname;
                    srcs[n++] = COMP_SRC_COL;
                }
            }
        }
    }

    /* Source 4: history words — tokenize history entries, match prefix.
     * Matching words are copied into comp_scratch for stable null-terminated
     * pointers (reset each completion cycle). */
    {
        ray_hist_t* hist = &term->hist;
        for (int32_t hi = hist->count - 1; hi >= 0 && n < cap; hi--) {
            const char* entry = hist->entries[hi];
            int32_t elen = (int32_t)strlen(entry);
            int32_t wi = 0;
            while (wi < elen && n < cap) {
                /* skip non-alphanum */
                while (wi < elen && !is_alphanum(entry[wi])) wi++;
                if (wi >= elen) break;
                int32_t ws = wi;
                while (wi < elen && is_alphanum(entry[wi])) wi++;
                int32_t wlen = wi - ws;
                /* Must be longer than what's typed (otherwise not a useful completion) */
                if (wlen > prefix_len &&
                    strncmp(entry + ws, prefix, (size_t)prefix_len) == 0) {
                    /* Length-aware dedup against existing candidates */
                    int dup = 0;
                    for (int32_t di = 0; di < n; di++) {
                        int32_t olen = (int32_t)strlen(out[di]);
                        if (olen == wlen &&
                            strncmp(out[di], entry + ws, (size_t)wlen) == 0) {
                            dup = 1;
                            break;
                        }
                    }
                    if (!dup) {
                        /* Copy word into scratch buffer for a stable
                         * null-terminated pointer (avoids polluting the
                         * global sym table with arbitrary history words) */
                        if (term->comp_scratch_len + wlen + 1 <= TERM_BUF_SIZE) {
                            memcpy(term->comp_scratch + term->comp_scratch_len,
                                   entry + ws, (size_t)wlen);
                            term->comp_scratch[term->comp_scratch_len + wlen] = '\0';
                            out[n] = term->comp_scratch + term->comp_scratch_len;
                            srcs[n++] = COMP_SRC_HIST;
                            term->comp_scratch_len += wlen + 1;
                        }
                    }
                }
            }
        }
    }

    term->comp_count = n;
    if (term->comp_count > 1)
        comp_sort_candidates(term);
}

/* ===== Inline tab-cycle completion ===== */

/* Replace the word at comp_cycle_start..+comp_cycle_len with comp_items[idx] */
static void comp_cycle_insert(ray_term_t* term, int32_t idx) {
    const char* item = term->comp_items[idx];
    int32_t ilen = (int32_t)strlen(item);
    int32_t ws = term->comp_cycle_start;
    int32_t old_len = term->comp_cycle_len;
    /* The cycle span must lie within the current buffer.  If the buffer
     * changed under an active cycle, a negative tail below would become
     * a huge size_t in memmove.  Drop the cycle instead of writing. */
    if (ws < 0 || old_len < 0 || ws + old_len > term->buf_len) {
        term->comp_cycling = 0;
        return;
    }
    int32_t tail = term->buf_len - (ws + old_len);
    if (ws + ilen + tail >= TERM_BUF_SIZE) return;
    memmove(term->buf + ws + ilen, term->buf + ws + old_len, (size_t)tail);
    memcpy(term->buf + ws, item, (size_t)ilen);
    term->buf_len = ws + ilen + tail;
    term->buf_pos = ws + ilen;
    term->comp_cycle_len = ilen;
    term->comp_cycle_idx = idx;
}

/* ===== Completion menu ===== */

static const char* comp_source_label(int32_t src) {
    switch (src) {
    case COMP_SRC_COL:  return "col";
    case COMP_SRC_HIST: return "hist";
    case COMP_SRC_ENV:
    default:            return "env";
    }
}

static void menu_append(char* dst, int32_t cap, int32_t* n,
                        int32_t* vis, const char* s) {
    int32_t len = (int32_t)strlen(s);
    if (*n + len >= cap) return;
    memcpy(dst + *n, s, (size_t)len);
    *n += len;
    *vis += ray_term_visual_width(s, len);
}

static int32_t completion_menu_into(ray_term_t* term, char* dst,
                                    int32_t dst_cap) {
    if (!term->comp_cycling || term->comp_count < 2 || term->term_width <= 10)
        return 0;

    int32_t selected = term->comp_cycle_idx;
    if (selected < 0 || selected >= term->comp_count)
        selected = 0;

    int32_t max_items = 5;
    int32_t start = selected - 2;
    if (start < 0) start = 0;
    if (start + max_items > term->comp_count) {
        start = term->comp_count - max_items;
        if (start < 0) start = 0;
    }

    int32_t n = 0;
    int32_t vis = 0;
    int32_t limit = term->term_width - 1;
    menu_append(dst, dst_cap, &n, &vis, CLR_GRAY);
    menu_append(dst, dst_cap, &n, &vis, "  ");

    for (int32_t i = start, shown = 0; i < term->comp_count && shown < max_items; i++, shown++) {
        const char* item = term->comp_items[i];
        const char* label = comp_source_label(term->comp_sources[i]);
        int32_t item_vis = ray_term_visual_width(item, (int32_t)strlen(item));
        int32_t label_vis = (int32_t)strlen(label) + 2; /* [] */
        int32_t need = item_vis + label_vis + 3;
        if (vis + need > limit)
            break;

        if (i == selected)
            menu_append(dst, dst_cap, &n, &vis, CLR_REVERSE);
        menu_append(dst, dst_cap, &n, &vis, "[");
        menu_append(dst, dst_cap, &n, &vis, label);
        menu_append(dst, dst_cap, &n, &vis, "] ");
        menu_append(dst, dst_cap, &n, &vis, item);
        if (i == selected)
            menu_append(dst, dst_cap, &n, &vis, CLR_RESET CLR_GRAY);
        menu_append(dst, dst_cap, &n, &vis, "  ");
    }

    if (start + max_items < term->comp_count && vis + 6 < limit) {
        char more[24];
        snprintf(more, sizeof(more), "+%d", term->comp_count - (start + max_items));
        menu_append(dst, dst_cap, &n, &vis, more);
    }

    menu_append(dst, dst_cap, &n, &vis, CLR_RESET);
    return n;
}

/* ===== Multi-line input ===== */

int32_t ray_term_count_unmatched(ray_term_t* term) {
    int32_t depth = 0;
    int32_t in_string = 0;

    /* Scan multiline_buf first */
    for (int32_t i = 0; i < term->multiline_len; i++) {
        char c = term->multiline_buf[i];
        if (in_string) {
            if (c == '\\' && i + 1 < term->multiline_len) { i++; continue; }
            if (c == '"') in_string = 0;
            continue;
        }
        if (c == '"') { in_string = 1; continue; }
        if (c == ';') {
            while (i < term->multiline_len && term->multiline_buf[i] != '\n') i++;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') { if (depth > 0) depth--; }
    }

    /* Then scan current buf — no newlines possible here (single line),
       but skip from ';' to end of buf for consistency. */
    for (int32_t i = 0; i < term->buf_len; i++) {
        char c = term->buf[i];
        if (in_string) {
            if (c == '\\' && i + 1 < term->buf_len) { i++; continue; }
            if (c == '"') in_string = 0;
            continue;
        }
        if (c == '"') { in_string = 1; continue; }
        if (c == ';') break; /* rest of current line is a comment */
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') { if (depth > 0) depth--; }
    }

    return depth;
}

/* ===== Prompt ===== */

/* Green ‣ (U+2023) prompt, matching Rayforce style.
 *
 * No console font Windows ships has U+2023 — not Consolas, Cascadia Mono,
 * Lucida Console or Courier New — and the classic console does no font
 * fallback, so the prompt renders as "?" there.  Windows uses ► (U+25BA),
 * the nearest filled triangle all four of them do have; it is also three
 * UTF-8 bytes, so the byte and visual widths below are unchanged. */
#if defined(RAY_OS_WINDOWS)
#define PROMPT_STR "\033[32m\xe2\x96\xba\033[0m "
#else
#define PROMPT_STR "\033[32m\xe2\x80\xa3\033[0m "
#endif
#define PROMPT_LEN 13  /* ESC[32m (5) + ‣ (3) + ESC[0m (4) + space (1) = 13 bytes */
#define PROMPT_VIS  2  /* visual: ‣ + space */
#define CONT_PROMPT_STR "\033[90m\xe2\x80\xa6\033[0m "  /* gray … (U+2026) */
#define CONT_PROMPT_LEN 13  /* ESC[90m (5) + … (3) + ESC[0m (4) + space (1) = 13 bytes */
#define CONT_PROMPT_VIS  2  /* visual: … + space */

void ray_term_prompt(ray_term_t* term) {
    if (term->prompt_prefix_len > 0)
        term_write(term->prompt_prefix, (size_t)term->prompt_prefix_len);
    term_write(PROMPT_STR, PROMPT_LEN);
    term->prompt_len = term->prompt_prefix_vis + PROMPT_VIS;
}

void ray_term_continuation_prompt(ray_term_t* term) {
    /* Continuation prompt mirrors the prefix so multi-line input
     * stays visually aligned with the active session indicator. */
    if (term->prompt_prefix_len > 0)
        term_write(term->prompt_prefix, (size_t)term->prompt_prefix_len);
    term_write(CONT_PROMPT_STR, CONT_PROMPT_LEN);
    term->prompt_len = term->prompt_prefix_vis + CONT_PROMPT_VIS;
}

void ray_term_set_prompt_prefix(ray_term_t* term, const char* prefix) {
    if (!term) return;
    if (!prefix || !*prefix) {
        term->prompt_prefix_len = 0;
        term->prompt_prefix_vis = 0;
        term->prompt_prefix[0]  = '\0';
        return;
    }
    /* Wrap the user-visible bytes in a soft yellow ANSI tint so the
     * remote prefix is visually distinct from local input.  The visual
     * width is whatever ray_term_visual_width sees in the *plain*
     * bytes — the ANSI escapes themselves don't take columns. */
    int n = snprintf(term->prompt_prefix, sizeof(term->prompt_prefix),
                     "\033[33m%s\033[0m ", prefix);
    if (n < 0 || n >= (int)sizeof(term->prompt_prefix)) {
        /* Fallback: prefix too long for the buffer — clear instead of
         * truncating mid-escape, which would leak ANSI state on screen. */
        term->prompt_prefix_len = 0;
        term->prompt_prefix_vis = 0;
        term->prompt_prefix[0]  = '\0';
        return;
    }
    term->prompt_prefix_len = n;
    term->prompt_prefix_vis = ray_term_visual_width(prefix, strlen(prefix)) + 1;
}

/* ===== Redraw ===== */

void ray_term_redraw(ray_term_t* term) {
    int32_t total_width;
    int32_t ghost_vis = 0;
    int32_t menu_len = 0;
    char menu[1024];

    /* Recompute ghost text on every redraw */
    ray_term_update_ghost(term);
    ray_term_get_size(term);
    menu_len = completion_menu_into(term, menu, (int32_t)sizeof(menu));

    ray_cursor_hide();

    /* Move to start of first line */
    printf("\r");
    if (term->last_total_rows > 1) {
        for (int32_t i = 1; i < term->last_total_rows; i++) {
            ray_cursor_move_up(1);
            printf("\r");
        }
    }

    /* Clear from cursor to end of screen */
    printf("\033[J");

    /* Write prompt + highlighted buffer into temp buf, then single write */
    {
        /* Each char can expand to ~15 bytes with ANSI escapes */
        char hlbuf[TERM_BUF_SIZE * 8];
        int32_t hlen = 0;
        /* Prepend the prompt prefix (e.g. "host:port" in remote-REPL
         * mode) so it stays visible across keystroke redraws.
         * Without this, only ray_term_prompt's first render shows the
         * prefix and the next keystroke wipes it, leaving the user
         * unsure whether they're typing into the local or remote
         * REPL.  The visual-width add-on for prompt_len is already
         * computed by ray_term_prompt; we just have to keep the
         * bytes flowing through every redraw. */
        if (term->prompt_prefix_len > 0 &&
            (int32_t)sizeof(hlbuf) > term->prompt_prefix_len) {
            memcpy(hlbuf, term->prompt_prefix, (size_t)term->prompt_prefix_len);
            hlen = term->prompt_prefix_len;
        }
        if (term->multiline_len > 0) {
            memcpy(hlbuf + hlen, CONT_PROMPT_STR, CONT_PROMPT_LEN);
            hlen += CONT_PROMPT_LEN;
        } else {
            memcpy(hlbuf + hlen, PROMPT_STR, PROMPT_LEN);
            hlen += PROMPT_LEN;
        }
        if (term->buf_len > 0) {
            /* Find bracket match at cursor */
            int32_t match_pos1 = -1, match_pos2 = -1;
            int32_t bad_pos = -1;
            int32_t cursor = term->buf_pos;
            /* Check char at cursor, or char before cursor */
            if (cursor < term->buf_len) {
                int32_t m = ray_term_find_matching_paren(term->buf, term->buf_len, cursor);
                if (m >= 0) { match_pos1 = cursor; match_pos2 = m; }
            }
            if (match_pos1 < 0 && cursor > 0) {
                int32_t m = ray_term_find_matching_paren(term->buf, term->buf_len, cursor - 1);
                if (m >= 0) { match_pos1 = cursor - 1; match_pos2 = m; }
            }
            if (match_pos1 < 0)
                bad_pos = term_find_unmatched_bracket(term->buf, term->buf_len);
            hlen += term_highlight_into(hlbuf + hlen,
                                        (int32_t)sizeof(hlbuf) - hlen,
                                        term->buf, term->buf_len,
                                        match_pos1, match_pos2, bad_pos);
        }
        /* Append ghost text in gray after buffer content */
        if (term->ghost_len > 0 && term->buf_pos == term->buf_len) {
            const char* g_pre = CLR_GRAY;
            const char* g_post = CLR_RESET;
            int32_t g_pre_len = (int32_t)strlen(g_pre);
            int32_t g_post_len = (int32_t)strlen(g_post);
            if (hlen + g_pre_len + term->ghost_len + g_post_len < (int32_t)sizeof(hlbuf)) {
                memcpy(hlbuf + hlen, g_pre, (size_t)g_pre_len);
                hlen += g_pre_len;
                memcpy(hlbuf + hlen, term->ghost, (size_t)term->ghost_len);
                hlen += term->ghost_len;
                memcpy(hlbuf + hlen, g_post, (size_t)g_post_len);
                hlen += g_post_len;
            }
        }
        fflush(stdout);
        term_write(hlbuf, (size_t)hlen);
    }

    if (menu_len > 0) {
        term_write("\n", 1);
        term_write(menu, (size_t)menu_len);
    }

    /* Track rows used — include ghost text width for row calculation */
    ghost_vis = (term->ghost_len > 0 && term->buf_pos == term->buf_len)
                ? ray_term_visual_width(term->ghost, term->ghost_len) : 0;
    total_width = term->prompt_len + ray_term_visual_width(term->buf, term->buf_len) + ghost_vis;
    if (term->term_width > 0) {
        term->last_total_rows = (total_width + term->term_width - 1) / term->term_width;
        if (term->last_total_rows == 0)
            term->last_total_rows = 1;
    }

    /* Position cursor at buf_pos.  The menu, when visible, is drawn on
     * the line below input; otherwise the physical cursor sits after any
     * ghost text.  Move back to the end of the real buffer before using
     * the normal prompt-relative cursor math. */
    if (menu_len > 0) {
        int32_t input_vis = term->prompt_len +
            ray_term_visual_width(term->buf, term->buf_len);
        ray_cursor_move_up(1);
        printf("\r");
        if (term->term_width > 0)
            ray_cursor_move_right(input_vis % term->term_width);
        else
            ray_cursor_move_right(input_vis);
    } else if (ghost_vis > 0) {
        ray_cursor_move_left(ghost_vis);
    }
    ray_term_goto_position(term, term->buf_len, term->buf_pos);


    ray_cursor_show();
    fflush(stdout);
}

/* ===== Search mode redraw ===== */

#define SEARCH_PROMPT     "(search) `"
#define SEARCH_PROMPT_LEN 10
#define SEARCH_HIGHLIGHT  "\033[7m"
#define SEARCH_RESET      "\033[0m"

static void ray_term_search_redraw(ray_term_t* term) {
    ray_cursor_hide();

    /* Move to start */
    printf("\r");
    if (term->last_total_rows > 1) {
        for (int32_t i = 1; i < term->last_total_rows; i++) {
            ray_cursor_move_up(1);
            printf("\r");
        }
    }
    printf("\033[J");
    fflush(stdout);

    /* Write search prompt: (search) `query`: matched_entry */
    term_write(SEARCH_PROMPT, SEARCH_PROMPT_LEN);
    if (term->search_len > 0)
        term_write(term->search_buf, (size_t)term->search_len);
    term_write("': ", 3);

    /* Show matching entry with highlighted match substring */
    if (term->search_match_idx >= 0) {
        const char* entry = term->hist.entries[term->search_match_idx];
        int32_t elen = (int32_t)strlen(entry);

        /* Find match position within entry */
        int32_t match_pos = -1;
        if (term->search_len > 0 && elen >= term->search_len) {
            for (int32_t j = 0; j <= elen - term->search_len; j++) {
                if (memcmp(entry + j, term->search_buf, (size_t)term->search_len) == 0) {
                    match_pos = j;
                    break;
                }
            }
        }

        if (match_pos >= 0) {
            /* Before match */
            if (match_pos > 0)
                term_write(entry, (size_t)match_pos);
            /* Highlighted match */
            term_write(SEARCH_HIGHLIGHT, 4);
            term_write(entry + match_pos, (size_t)term->search_len);
            term_write(SEARCH_RESET, 4);
            /* After match */
            int32_t after = match_pos + term->search_len;
            if (after < elen)
                term_write(entry + after, (size_t)(elen - after));
        } else {
            term_write(entry, (size_t)elen);
        }
    }

    /* Update row tracking */
    int32_t total_vis = SEARCH_PROMPT_LEN + term->search_len + 3;
    if (term->search_match_idx >= 0)
        total_vis += (int32_t)strlen(term->hist.entries[term->search_match_idx]);
    if (term->term_width > 0) {
        term->last_total_rows = (total_vis + term->term_width - 1) / term->term_width;
        if (term->last_total_rows == 0) term->last_total_rows = 1;
    }

    /* Position cursor right after the search query closing tick */
    int32_t cursor_col = SEARCH_PROMPT_LEN + term->search_len;
    int32_t end_col = total_vis;
    int32_t diff = end_col - cursor_col;
    if (diff > 0) ray_cursor_move_left(diff);

    ray_cursor_show();
    fflush(stdout);
}

/* ===== Event-driven line editing ===== */

/* Show prompt and reset line state.  Called once per input line. */
void ray_term_begin(ray_term_t* term) {
    ray_term_prompt(term);
    fflush(stdout);
    term->buf_len = 0;
    term->buf_pos = 0;
    term->multiline_len = 0;
    term->last_total_rows = 1;
    term->esc_state = 0;
    term->esc_buf_len = 0;
    term->esc_buf[0] = '\0';
}

/* Forward declaration — feed_normal handles all normal-mode keys. */
static ray_t* feed_normal(ray_term_t* term, int key);

static void csi_buf_reset(ray_term_t* term, int byte) {
    term->esc_buf_len = 0;
    if (byte >= 0 && term->esc_buf_len < (int32_t)sizeof(term->esc_buf) - 1) {
        term->esc_buf[term->esc_buf_len++] = (char)byte;
        term->esc_buf[term->esc_buf_len] = '\0';
    }
}

static void csi_buf_append(ray_term_t* term, int byte) {
    if (term->esc_buf_len < (int32_t)sizeof(term->esc_buf) - 1) {
        term->esc_buf[term->esc_buf_len++] = (char)byte;
        term->esc_buf[term->esc_buf_len] = '\0';
    } else {
        term->esc_state = 0;
        term->esc_buf_len = 0;
        term->esc_buf[0] = '\0';
    }
}

static int csi_first_param(ray_term_t* term) {
    int v = 0;
    int seen = 0;
    for (int32_t i = 0; i < term->esc_buf_len; i++) {
        char c = term->esc_buf[i];
        if (c < '0' || c > '9') break;
        seen = 1;
        v = v * 10 + (c - '0');
    }
    return seen ? v : 0;
}

static int csi_last_param(ray_term_t* term) {
    int32_t start = 0;
    for (int32_t i = 0; i < term->esc_buf_len; i++) {
        if (term->esc_buf[i] == ';')
            start = i + 1;
    }
    int v = 0;
    int seen = 0;
    for (int32_t i = start; i < term->esc_buf_len; i++) {
        char c = term->esc_buf[i];
        if (c < '0' || c > '9') break;
        seen = 1;
        v = v * 10 + (c - '0');
    }
    return seen ? v : 0;
}

static int csi_word_modifier(ray_term_t* term) {
    int mod = csi_last_param(term);
    return mod >= 3 && (((mod - 1) & 0x06) != 0);
}

static void term_forward_delete(ray_term_t* term) {
    if (term->buf_pos < term->buf_len) {
        int32_t next = find_next_utf8(term->buf, term->buf_pos, term->buf_len);
        term_delete_range(term, term->buf_pos, next);
        ray_term_redraw(term);
    }
}

static ray_t* feed_csi_final(ray_term_t* term, int final) {
    int first = csi_first_param(term);
    int word = csi_word_modifier(term);
    term->esc_state = 0;

    switch (final) {
    case 'A': return feed_normal(term, -KEYCODE_UP);
    case 'B': return feed_normal(term, -KEYCODE_DOWN);
    case 'C':
        if (word) { term_move_word_right(term); return NULL; }
        return feed_normal(term, -KEYCODE_RIGHT);
    case 'D':
        if (word) { term_move_word_left(term); return NULL; }
        return feed_normal(term, -KEYCODE_LEFT);
    case 'H': return feed_normal(term, -KEYCODE_HOME);
    case 'F': return feed_normal(term, -KEYCODE_END);
    case '~':
        if (first == 1 || first == 7)
            return feed_normal(term, -KEYCODE_HOME);
        if (first == 4 || first == 8)
            return feed_normal(term, -KEYCODE_END);
        if (first == 3) {
            if (word) term_delete_word_right(term);
            else term_forward_delete(term);
        }
        /* Bracketed-paste boundaries are intentionally ignored here:
         * pasted bytes still flow through the normal feed path. */
        return NULL;
    default:
        return NULL;
    }
}

/* Process one byte while in escape-sequence state.
 * Returns a line (via feed_normal) or NULL. */
static ray_t* feed_escape(ray_term_t* term, int byte) {
    switch (term->esc_state) {
    case 1: /* Got ESC, waiting for [ or O */
        if (byte == '[') { term->esc_state = 2; return NULL; }
        if (byte == 'O') { term->esc_state = 3; return NULL; }
        if (byte == 'b' || byte == 'B') {
            term->esc_state = 0;
            term_move_word_left(term);
            return NULL;
        }
        if (byte == 'f' || byte == 'F') {
            term->esc_state = 0;
            term_move_word_right(term);
            return NULL;
        }
        if (byte == 'd' || byte == 'D') {
            term->esc_state = 0;
            term_delete_word_right(term);
            return NULL;
        }
        if (byte == KEYCODE_BACKSPACE || byte == KEYCODE_DELETE) {
            term->esc_state = 0;
            term_delete_word_left(term);
            return NULL;
        }
        /* Bare ESC — cancel tab cycling if active */
        term->esc_state = 0;
        if (term->comp_cycling) {
            term->comp_cycling = 0;
            ray_term_redraw(term);
        }
        return NULL;

    case 2: /* Got ESC [, waiting for final byte */
        term->esc_state = 0;
        switch (byte) {
        case 'A': return feed_normal(term, -KEYCODE_UP);
        case 'B': return feed_normal(term, -KEYCODE_DOWN);
        case 'C': return feed_normal(term, -KEYCODE_RIGHT);
        case 'D': return feed_normal(term, -KEYCODE_LEFT);
        case 'H': return feed_normal(term, -KEYCODE_HOME);
        case 'F': return feed_normal(term, -KEYCODE_END);
        case '3':
            term->esc_state = 4;
            csi_buf_reset(term, byte);
            return NULL; /* waiting for ~ or a modifier suffix */
        default:
            /* Unknown CSI — if this is a final byte we are done */
            if (byte >= 0x40 && byte <= 0x7E) return NULL;
            /* Otherwise consume until final byte */
            term->esc_state = 5;
            csi_buf_reset(term, byte);
            return NULL;
        }

    case 3: /* Got ESC O */
        term->esc_state = 0;
        if (byte == 'H') return feed_normal(term, -KEYCODE_HOME);
        if (byte == 'F') return feed_normal(term, -KEYCODE_END);
        return NULL;

    case 4: /* Got ESC [ 3, waiting for ~ (Delete key) */
        if (byte == '~') {
            term->esc_state = 0;
            term_forward_delete(term);
        } else if (byte == ';') {
            term->esc_state = 5;
            csi_buf_append(term, byte);
        } else {
            term->esc_state = 0;
        }
        return NULL;

    case 5: /* Consuming unknown CSI until final byte */
        if (byte >= 0x40 && byte <= 0x7E)
            return feed_csi_final(term, byte);
        csi_buf_append(term, byte);
        return NULL;
    }
    term->esc_state = 0;
    return NULL;
}

/* Process one byte while in search mode.
 * Returns a line if search-then-Enter fires, NULL otherwise. */
static ray_t* feed_search(ray_term_t* term, int skey) {
    if (skey == KEYCODE_RETURN) {
        /* Accept match into buffer, then submit via normal Enter path */
        if (term->search_match_idx >= 0) {
            const char* entry = term->hist.entries[term->search_match_idx];
            int32_t len = (int32_t)strlen(entry);
            if (len > TERM_BUF_SIZE - 1) len = TERM_BUF_SIZE - 1;
            memcpy(term->buf, entry, (size_t)len);
            term->buf_len = len;
            term->buf_pos = len;
        }
        term->search_mode = 0;
        term->multiline_len = 0;
        return feed_normal(term, KEYCODE_RETURN);
    }

    if (skey == KEYCODE_ESCAPE) {
        /* Exit search mode; escape sequence bytes will arrive later
         * and be consumed by the escape state machine. */
        term->search_mode = 0;
        term->esc_state = 1;  /* expect continuation byte */
        term->esc_buf_len = 0;
        return NULL;
    }

    if (skey == KEYCODE_CTRL_C) {
        term->search_mode = 0;
        term->buf_len = 0;
        term->buf_pos = 0;
        term->multiline_len = 0;
        term_write("^C\n", 3);
        ray_term_prompt(term);
        fflush(stdout);
        return NULL;
    }

    if (skey == KEYCODE_CTRL_R) {
        /* Search further back */
        if (term->search_match_idx > 0 && term->search_len > 0) {
            int32_t idx = ray_hist_search(&term->hist,
                                         term->search_buf,
                                         term->search_len,
                                         term->search_match_idx - 1);
            if (idx >= 0)
                term->search_match_idx = idx;
        }
        ray_term_search_redraw(term);
        return NULL;
    }

    if (skey == KEYCODE_BACKSPACE || skey == KEYCODE_DELETE) {
        if (term->search_len > 0) {
            term->search_len--;
            if (term->search_len > 0) {
                term->search_match_idx = ray_hist_search(
                    &term->hist, term->search_buf,
                    term->search_len, term->hist.count - 1);
            } else {
                term->search_match_idx = -1;
            }
        }
        ray_term_search_redraw(term);
        return NULL;
    }

    /* Printable character — append to search query */
    if ((unsigned char)skey >= 0x20 && term->search_len < 255) {
        term->search_buf[term->search_len++] = (char)skey;
        int32_t start = (term->search_match_idx >= 0)
                        ? term->search_match_idx
                        : term->hist.count - 1;
        term->search_match_idx = ray_hist_search(
            &term->hist, term->search_buf,
            term->search_len, start);
        ray_term_search_redraw(term);
    }
    return NULL;
}

/* Process one normal-mode key.  key is positive for ASCII/control,
 * negative for special keys (arrow, home, end). */
static ray_t* feed_normal(ray_term_t* term, int key) {
    /* Reset tab-cycle on any non-Tab key */
    if (key != KEYCODE_TAB)
        term->comp_cycling = 0;

    /* Arrow keys are encoded as negative to distinguish from printable chars */
    if (key == -KEYCODE_UP || key == KEYCODE_CTRL_P) {
        int32_t len = ray_hist_prev(&term->hist, term->buf, term->buf_len);
        if (len >= 0) {
            term->buf_len = len;
            term->buf_pos = len;
            ray_term_redraw(term);
        }
        return NULL;
    }

    if (key == -KEYCODE_DOWN || key == KEYCODE_CTRL_N) {
        int32_t len = ray_hist_next(&term->hist, term->buf);
        if (len >= 0) {
            term->buf_len = len;
            term->buf_pos = len;
            ray_term_redraw(term);
        }
        return NULL;
    }

    if (key == -KEYCODE_LEFT || key == KEYCODE_CTRL_B) {
        if (term->buf_pos > 0) {
            int32_t prev = find_prev_utf8(term->buf, term->buf_pos);
            term->buf_pos = prev;
            ray_term_redraw(term);
        }
        return NULL;
    }

    if (key == -KEYCODE_RIGHT || key == KEYCODE_CTRL_F) {
        if (term->buf_pos < term->buf_len) {
            int32_t next = find_next_utf8(term->buf, term->buf_pos, term->buf_len);
            term->buf_pos = next;
            ray_term_redraw(term);
        } else if (term->ghost_len > 0) {
            ray_term_accept_ghost(term);
            ray_term_redraw(term);
        }
        return NULL;
    }

    if (key == -KEYCODE_HOME || key == KEYCODE_CTRL_A) {
        term->buf_pos = 0;
        ray_term_redraw(term);
        return NULL;
    }

    if (key == -KEYCODE_END || key == KEYCODE_CTRL_E) {
        term->buf_pos = term->buf_len;
        ray_term_redraw(term);
        return NULL;
    }

    switch (key) {
    case KEYCODE_RETURN: {
        term->buf[term->buf_len] = '\0';
        int32_t unmatched = ray_term_count_unmatched(term);
        if (unmatched > 0) {
            term->comp_cycling = 0;
            if (term->multiline_len + term->buf_len + 1 < TERM_BUF_SIZE) {
                memcpy(term->multiline_buf + term->multiline_len,
                       term->buf, (size_t)term->buf_len);
                term->multiline_len += term->buf_len;
                term->multiline_buf[term->multiline_len++] = '\n';
            } else {
                fprintf(stderr, "\ninput too long (max %d bytes)\n",
                        TERM_BUF_SIZE - 1);
                fflush(stderr);
                term->multiline_len = 0;
                term->buf_len = 0;
                term->buf_pos = 0;
                ray_term_prompt(term);
                fflush(stdout);
                return NULL;
            }
            term->buf_len = 0;
            term->buf_pos = 0;
            putchar('\n');
            fflush(stdout);
            ray_term_continuation_prompt(term);
            fflush(stdout);
            return NULL;
        }
        /* Redraw line without bracket highlights before submitting */
        term->ghost_len = 0;
        term->comp_cycling = 0;
        {
            ray_cursor_hide();
            printf("\r\033[J");
            char hlbuf[TERM_BUF_SIZE * 8];
            int32_t hlen = 0;
            /* Mirror the redraw path: prepend the remote-REPL prefix
             * so the line that's echoed on Enter (the final un-
             * highlighted render before submitting to eval) keeps
             * the host:port indicator instead of dropping back to
             * the bare local prompt. */
            if (term->prompt_prefix_len > 0 &&
                (int32_t)sizeof(hlbuf) > term->prompt_prefix_len) {
                memcpy(hlbuf, term->prompt_prefix,
                       (size_t)term->prompt_prefix_len);
                hlen = term->prompt_prefix_len;
            }
            if (term->multiline_len > 0) {
                memcpy(hlbuf + hlen, CONT_PROMPT_STR, CONT_PROMPT_LEN);
                hlen += CONT_PROMPT_LEN;
            } else {
                memcpy(hlbuf + hlen, PROMPT_STR, PROMPT_LEN);
                hlen += PROMPT_LEN;
            }
            if (term->buf_len > 0)
                hlen += term_highlight_into(hlbuf + hlen,
                            (int32_t)sizeof(hlbuf) - hlen,
                            term->buf, term->buf_len, -1, -1, -1);
            fflush(stdout);
            term_write(hlbuf, (size_t)hlen);
            ray_cursor_show();
            fflush(stdout);
        }
        putchar('\n');
        fflush(stdout);
        if (term->multiline_len > 0) {
            if (term->multiline_len + term->buf_len < TERM_BUF_SIZE) {
                memcpy(term->multiline_buf + term->multiline_len,
                       term->buf, (size_t)term->buf_len);
                term->multiline_len += term->buf_len;
            }
            ray_hist_add(&term->hist, term->multiline_buf, term->multiline_len);
            ray_t* result = ray_str(term->multiline_buf, (size_t)term->multiline_len);
            term->multiline_len = 0;
            return RAY_IS_ERR(result) ? NULL : result;
        }
        ray_hist_add(&term->hist, term->buf, term->buf_len);
        if (term->buf_len == 0) {
            ray_t* result = ray_str("", 0);
            return RAY_IS_ERR(result) ? NULL : result;
        }
        { ray_t* result = ray_str(term->buf, (size_t)term->buf_len);
          return RAY_IS_ERR(result) ? NULL : result; }
    }

    case KEYCODE_CTRL_D: {
        if (term->buf_len == 0) {
            putchar('\n');
            fflush(stdout);
            return RAY_TERM_EOF;
        }
        if (term->buf_pos < term->buf_len) {
            int32_t next = find_next_utf8(term->buf, term->buf_pos, term->buf_len);
            int32_t bytes = next - term->buf_pos;
            memmove(term->buf + term->buf_pos,
                    term->buf + term->buf_pos + bytes,
                    (size_t)(term->buf_len - term->buf_pos - bytes));
            term->buf_len -= bytes;
            ray_term_redraw(term);
        }
        return NULL;
    }

    case KEYCODE_CTRL_C: {
        term->comp_cycling = 0;
        term->esc_state = 0;
        term->buf_len = 0;
        term->buf_pos = 0;
        term->multiline_len = 0;
        term_write("^C\n", 3);
        ray_term_prompt(term);
        fflush(stdout);
        return NULL;
    }

    case KEYCODE_BACKSPACE:
    case KEYCODE_DELETE: {
        if (term->buf_pos > 0) {
            int32_t prev = find_prev_utf8(term->buf, term->buf_pos);
            int32_t bytes = term->buf_pos - prev;
            memmove(term->buf + prev,
                    term->buf + term->buf_pos,
                    (size_t)(term->buf_len - term->buf_pos));
            term->buf_len -= bytes;
            term->buf_pos = prev;
            ray_term_redraw(term);
        }
        return NULL;
    }

    case KEYCODE_CTRL_K: {
        term->buf_len = term->buf_pos;
        ray_term_redraw(term);
        return NULL;
    }

    case KEYCODE_CTRL_U: {
        term->buf_len = 0;
        term->buf_pos = 0;
        ray_term_redraw(term);
        return NULL;
    }

    case KEYCODE_CTRL_W: {
        term_delete_word_left(term);
        return NULL;
    }

    case KEYCODE_CTRL_L: {
        term_clear_screen(term);
        return NULL;
    }

    case KEYCODE_CTRL_T: {
        term_transpose_chars(term);
        return NULL;
    }

    case KEYCODE_CTRL_R: {
        term->search_mode = 1;
        term->search_len = 0;
        term->search_match_idx = -1;
        ray_term_search_redraw(term);
        return NULL;
    }

    case KEYCODE_TAB: {
        if (term->comp_cycling) {
            /* update_ghost (called from each redraw) resets comp_count to
             * 0 once the inserted word fully matches itself — without
             * the guard the next Tab would `% 0` and crash.  Just exit
             * cycling cleanly when the candidate list has gone away. */
            if (term->comp_count <= 0) {
                term->comp_cycling = 0;
                return NULL;
            }
            int32_t next = (term->comp_cycle_idx + 1) % term->comp_count;
            comp_cycle_insert(term, next);
            term->ghost_len = 0;
            ray_term_redraw(term);
            return NULL;
        }
        if (term->ghost_len > 0 && term->comp_count == 1) {
            ray_term_accept_ghost(term);
            ray_term_update_ghost(term);
            ray_term_redraw(term);
        } else if (term->comp_count >= 2) {
            term->comp_cycling = 1;
            term->comp_cycle_start = term->ghost_word_start;
            term->comp_cycle_len = term->ghost_word_len;
            term->comp_cycle_idx = -1;
            comp_cycle_insert(term, 0);
            term->ghost_len = 0;
            ray_term_redraw(term);
        } else if (term->ghost_len > 0) {
            ray_term_accept_ghost(term);
            ray_term_update_ghost(term);
            ray_term_redraw(term);
        }
        return NULL;
    }

    default: {
        if ((unsigned char)key >= 0x20 && term->buf_len < TERM_BUF_SIZE - 1) {
            if (term->buf_pos < term->buf_len) {
                memmove(term->buf + term->buf_pos + 1,
                        term->buf + term->buf_pos,
                        (size_t)(term->buf_len - term->buf_pos));
            }
            term->buf[term->buf_pos] = (char)key;
            term->buf_pos++;
            term->buf_len++;
            ray_term_redraw(term);
        }
        return NULL;
    }
    }
}

/* Process one byte from term->input[0].
 * Returns a ray_t* string when a complete line is ready,
 * NULL when more input is needed,
 * RAY_TERM_EOF on EOF/Ctrl-D at empty buffer. */
ray_t* ray_term_feed(ray_term_t* term) {
    int key = (unsigned char)term->input[0];

    /* Escape sequence state machine */
    if (term->esc_state > 0)
        return feed_escape(term, key);

    /* Search mode */
    if (term->search_mode)
        return feed_search(term, key);

    /* Start of escape sequence */
    if (key == KEYCODE_ESCAPE) {
        term->esc_state = 1;
        term->esc_buf_len = 0;
        return NULL;
    }

    return feed_normal(term, key);
}
