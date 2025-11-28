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

#include <stdio.h>
#include <unistd.h>
#include "term.h"
#include "chrono.h"
#include "heap.h"
#include "string.h"
#include "error.h"
#include "ops.h"
#include "runtime.h"
#include "format.h"
#include "parse.h"
#include "env.h"
#include "mmap.h"
#include "fs.h"
#if defined(OS_WINDOWS)
#include <io.h>
#else
#include <sys/ioctl.h>
#include <sys/file.h>
#endif

#define MAX_PATH_LEN 128
#define HIST_FILE_PATH ".rayhist.dat"
#define HIST_SIZE 4096 * 1024  // 4MB
#define COMMANDS_LIST \
    "\
  :?  - Displays help.\n\
  :u  - Use unicode for graphic formatting: [0|1].\n\
  :t  - Turns on|off measurement of expressions: [0|1].\n\
  :q  - Exits the application: [exit code]."

#define IS_CMD(t, c) ((t)->buf_len >= (i32_t)strlen(c) && strncmp((t)->buf, c, strlen(c)) == 0)
#define IS_ESC(t, e) ((t)->input_len == (i32_t)strlen(e) && strncmp((t)->input, e, strlen(e)) == 0)
#define PROGRESS_BAR_WIDTH 40

nil_t cursor_move_start() { printf("\r"); }

nil_t cursor_move_left(i32_t i) {
    if (i > 0)
        printf("\033[%dD", i);
}

nil_t cursor_move_right(i32_t i) {
    if (i > 0)
        printf("\033[%dC", i);
}

nil_t cursor_move_up(i32_t i) {
    if (i > 0)
        printf("\033[%dA", i);
}

nil_t cursor_move_down(i32_t i) {
    if (i > 0)
        printf("\033[%dB", i);
}

nil_t line_clear() { printf("\r\033[K"); }

nil_t line_clear_below() { printf("\033[J"); }

nil_t line_new() { printf("\n"); }

nil_t cursor_hide() { printf("\e[?25l"); }

nil_t cursor_show() { printf("\e[?25h"); }

// Get terminal size
nil_t term_get_size(term_p term) {
#if defined(OS_WINDOWS)
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(term->h_stdout, &csbi)) {
        term->term_width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        term->term_height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    } else {
        term->term_width = 80;   // Default fallback
        term->term_height = 24;
    }
#else
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        term->term_width = w.ws_col;
        term->term_height = w.ws_row;
    } else {
        term->term_width = 80;   // Default fallback
        term->term_height = 24;
    }
#endif
}

// Calculate visual width of a string, excluding ANSI escape codes
i32_t term_visual_width(const c8_t *str, i32_t len) {
    i32_t i, width, in_escape;

    width = 0;
    in_escape = 0;

    for (i = 0; i < len; i++) {
        if (str[i] == '\033') {
            in_escape = 1;
        } else if (in_escape) {
            if (str[i] == 'm' || str[i] == 'K' || str[i] == 'H' ||
                str[i] == 'A' || str[i] == 'B' || str[i] == 'C' || str[i] == 'D') {
                in_escape = 0;
            }
        } else {
            // Handle UTF-8 multi-byte characters
            if ((str[i] & 0x80) == 0) {
                width++;  // ASCII
            } else if ((str[i] & 0xE0) == 0xC0) {
                width++;  // 2-byte UTF-8
            } else if ((str[i] & 0xF0) == 0xE0) {
                width++;  // 3-byte UTF-8
            } else if ((str[i] & 0xF8) == 0xF0) {
                width += 2;  // 4-byte UTF-8 (emoji, wide chars)
            }
            // Skip continuation bytes (0x80-0xBF)
        }
    }

    return width;
}

// Move cursor from one buffer position to another, handling line wrapping
nil_t term_goto_position(term_p term, i32_t from_pos, i32_t to_pos) {
    i32_t from_row, from_col, to_row, to_col, total_width;
    i32_t row_diff, col_diff;

    if (term->term_width <= 0)
        return;

    // Calculate total visual width at each position
    total_width = term->prompt_len + term_visual_width(term->buf, from_pos);
    from_row = total_width / term->term_width;
    from_col = total_width % term->term_width;

    total_width = term->prompt_len + term_visual_width(term->buf, to_pos);
    to_row = total_width / term->term_width;
    to_col = total_width % term->term_width;

    row_diff = to_row - from_row;
    col_diff = to_col - from_col;

    // Move vertically first
    if (row_diff < 0) {
        cursor_move_up(-row_diff);
    } else if (row_diff > 0) {
        cursor_move_down(row_diff);
    }

    // Then move horizontally
    if (col_diff < 0) {
        cursor_move_left(-col_diff);
    } else if (col_diff > 0) {
        cursor_move_right(col_diff);
    }

    // Update tracked cursor row
    term->last_cursor_row = to_row;
}

hist_p hist_create() {
    i64_t pos, fd, fsize;
    str_p lines;
    hist_p hist;

    fd = fs_fopen(HIST_FILE_PATH, ATTR_RDWR | ATTR_CREAT);

    if (fd == -1) {
        perror("can't open history file");
        return NULL;
    }

    // Lock file for reading existing history
#if defined(OS_WINDOWS)
    OVERLAPPED overlapped = {0};
    if (!LockFileEx((HANDLE)_get_osfhandle(fd), 0, 0, MAXDWORD, MAXDWORD, &overlapped)) {
        perror("can't lock history file for reading");
        fs_fclose(fd);
        return NULL;
    }
#else
    if (flock(fd, LOCK_SH) == -1) {
        perror("can't lock history file for reading");
        fs_fclose(fd);
        return NULL;
    }
#endif

    fsize = fs_fsize(fd);
    if (fsize == 0) {
        // Set initial file size if the file is empty
        if (fs_file_extend(fd, HIST_SIZE) == -1) {
            perror("can't truncate history file");
#if defined(OS_WINDOWS)
            OVERLAPPED overlapped_err = {0};
            UnlockFileEx((HANDLE)_get_osfhandle(fd), 0, MAXDWORD, MAXDWORD, &overlapped_err);
#else
            flock(fd, LOCK_UN);
#endif
            fs_fclose(fd);
            return NULL;
        }

        fsize = HIST_SIZE;
    }

    // Map file to memory
    lines = (str_p)mmap_file(fd, NULL, fsize, 0);
    if (lines == NULL) {
        perror("can't map history file");
#if defined(OS_WINDOWS)
        OVERLAPPED overlapped_err = {0};
        UnlockFileEx((HANDLE)_get_osfhandle(fd), 0, MAXDWORD, MAXDWORD, &overlapped_err);
#else
        flock(fd, LOCK_UN);
#endif
        fs_fclose(fd);
        return NULL;
    }

    hist = (hist_p)heap_mmap(sizeof(struct hist_t));
    if (hist == NULL) {
        perror("can't allocate memory for history");
#if defined(OS_WINDOWS)
        OVERLAPPED overlapped_err = {0};
        UnlockFileEx((HANDLE)_get_osfhandle(fd), 0, MAXDWORD, MAXDWORD, &overlapped_err);
#else
        flock(fd, LOCK_UN);
#endif
        fs_fclose(fd);
        mmap_free(lines, fsize);
        return NULL;
    }

    // Find the current end of the data in the file
    pos = 0;
    while (pos < fsize && lines[pos] != '\0')
        pos++;

    hist->fd = fd;
    hist->lines = lines;
    hist->size = fsize;
    hist->pos = pos;
    hist->search_dir = 1;
    hist->index = (pos == 0) ? 0 : pos - 1;
    hist->curr_saved = 0;
    hist->curr_len = 0;

    // Unlock file after reading
#if defined(OS_WINDOWS)
    OVERLAPPED overlapped_unlock = {0};
    UnlockFileEx((HANDLE)_get_osfhandle(fd), 0, MAXDWORD, MAXDWORD, &overlapped_unlock);
#else
    flock(fd, LOCK_UN);
#endif

    return hist;
}

nil_t hist_destroy(hist_p hist) {
    // Lock file exclusively for writing
#if defined(OS_WINDOWS)
    OVERLAPPED overlapped = {0};
    if (!LockFileEx((HANDLE)_get_osfhandle(hist->fd), LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &overlapped)) {
        perror("can't lock history file for writing");
    }
#else
    if (flock(hist->fd, LOCK_EX) == -1) {
        perror("can't lock history file for writing");
    }
#endif

    // Sync history buffer to file
    if (mmap_sync(hist->lines, hist->size) == -1)
        perror("can't sync history buffer");

    // Unlock file
#if defined(OS_WINDOWS)
    OVERLAPPED overlapped_unlock = {0};
    UnlockFileEx((HANDLE)_get_osfhandle(hist->fd), 0, MAXDWORD, MAXDWORD, &overlapped_unlock);
#else
    flock(hist->fd, LOCK_UN);
#endif

    mmap_free(hist->lines, hist->size);
    fs_fclose(hist->fd);
    heap_unmap(hist, sizeof(struct hist_t));
}

nil_t hist_add(hist_p hist, c8_t buf[], i64_t len) {
    i64_t pos, size, index, last_len;

    pos = hist->pos;
    hist->index = (pos > 0) ? pos - 1 : 0;
    size = hist->size;
    index = hist->index;
    last_len = 0;

    // Find the previous line
    while (index > 0) {
        if (hist->lines[--index] == '\n') {
            last_len = hist->index - index - 1;
            // Check if the line is already in the history buffer
            if (last_len == len && strncmp(hist->lines + index + 1, buf, len) == 0)
                return;

            break;
        }
    }

    if (index == 0) {
        // Check if the line is already in the history buffer
        if (last_len == len && strncmp(hist->lines, buf, len) == 0)
            return;
    }

    pos = hist->pos;

    if (len + pos + 1 > size)
        return;

    // Add the line to the history buffer
    memcpy(hist->lines + pos, buf, len);
    hist->lines[pos + len] = '\n';
    hist->pos += len + 1;
    hist->index = hist->pos - 1;
    hist->search_dir = 1;
}

i64_t hist_prev(hist_p hist, c8_t buf[]) {
    i64_t index = hist->index;
    i64_t len = 0;

    if (index == 0)
        return len;

    // Skip current line if search direction was next
    if (hist->search_dir == 0) {
        while (index > 0) {
            if (hist->lines[--index] == '\n')
                break;
        }

        hist->index = index;
        hist->search_dir = 1;
    }

    // Find the previous line
    while (index > 0) {
        if (hist->lines[--index] == '\n') {
            len = hist->index - index - 1;
            strncpy(buf, hist->lines + index + 1, len);
            buf[len] = '\0';
            hist->index = index;
            return len;
        }
    }

    len = hist->index;
    strncpy(buf, hist->lines, len);
    buf[len] = '\0';
    hist->index = index;
    hist->search_dir = 1;

    return len;
}

i64_t hist_next(hist_p hist, c8_t buf[]) {
    i64_t index = hist->index;
    i64_t len = 0;

    // Skip current line if search direction was previous
    if (hist->search_dir == 1) {
        while (index + 1 < hist->pos) {
            if (hist->lines[++index] == '\n')
                break;
        }

        hist->index = index;
        hist->search_dir = 0;
    }

    // Find the next line
    while (index + 1 < hist->pos) {
        if (hist->lines[++index] == '\n') {
            len = index - hist->index - 1;
            strncpy(buf, hist->lines + hist->index + 1, len);
            buf[len] = '\0';
            break;
        }
    }

    hist->index = index;

    if (len == 0)
        hist->search_dir = 1;

    return len;
}

i64_t hist_save_current(hist_p hist, c8_t buf[], i64_t len) {
    if (hist->curr_saved == 1)
        return 0;

    memcpy(hist->curr, buf, len);
    hist->curr[len] = '\0';
    hist->curr_len = len;
    hist->curr_saved = 1;
    return len;
}

i64_t hist_restore_current(hist_p hist, c8_t buf[]) {
    i64_t l;
    l = hist->curr_len;

    if (hist->curr_saved == 0)
        return l;

    memcpy(buf, hist->curr, l);
    buf[l] = '\0';
    hist->curr_saved = 0;
    return l;
}

nil_t hist_reset_current(hist_p hist) {
    hist->curr_saved = 0;
    hist->curr_len = 0;
}

#if defined(OS_WINDOWS)

term_p term_create() {
    term_p term;
    hist_p hist;
    HANDLE h_stdin, h_stdout;

    h_stdin = GetStdHandle(STD_INPUT_HANDLE);
    h_stdout = GetStdHandle(STD_OUTPUT_HANDLE);

    hist = hist_create();
    if (hist == NULL)
        PANIC("can't create history");

    term = (term_p)heap_mmap(sizeof(struct term_t));

    if (term == NULL)
        return NULL;

    // For windows 10, set the output encoding to UTF-8
    // [Console]::OutputEncoding = [System.Text.Encoding]::UTF8

    // Set the console output code page to UTF-8
    if (!SetConsoleOutputCP(CP_UTF8))
        format_set_use_unicode(B8_FALSE);  // Disable unicode support

    // Save the current input mode
    GetConsoleMode(h_stdin, &term->old_stdin_mode);

    // Set the new input mode (disable line input, echo input, etc.)
    term->new_stdin_mode = term->old_stdin_mode;
    term->new_stdin_mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    term->new_stdin_mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    SetConsoleMode(h_stdin, term->new_stdin_mode);

    // Enable ANSI escape codes for the console
    GetConsoleMode(h_stdout, &term->old_stdout_mode);
    term->new_stdout_mode = term->old_stdout_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(h_stdout, term->new_stdout_mode);

    // Initialize the input buffer
    term->h_stdin = h_stdin;
    term->h_stdout = h_stdout;
    term->lock = mutex_create();
    term->input_len = 0;
    memset(term->input, 0, 8);
    term->buf_len = 0;
    term->buf_pos = 0;
    term->multiline_len = 0;
    term->hist = hist;
    term->autocp_idx.entry = 0;
    term->term_width = 80;
    term->term_height = 24;
    term->prompt_len = 0;
    term->last_total_rows = 1;
    term->last_cursor_row = 0;
    term_get_size(term);

    return term;
}

nil_t term_destroy(term_p term) {
    // Restore the terminal attributes
    SetConsoleMode(term->h_stdin, term->old_stdin_mode);
    SetConsoleMode(term->h_stdout, term->old_stdout_mode);

    mutex_destroy(&term->lock);
    hist_destroy(term->hist);

    // Unmap the terminal structure
    heap_unmap(term, sizeof(struct term_t));
}

i64_t term_getc(term_p term) {
    c8_t buf[8];
    DWORD bytesRead;

    if (!ReadFile(term->h_stdin, (LPVOID)buf, 1, &bytesRead, NULL))
        return 0;

    mutex_lock(&term->lock);

    term->input[term->input_len++ % 8] = buf[0];

    mutex_unlock(&term->lock);

    return (i64_t)bytesRead;
}

#else

term_p term_create() {
    term_p term;
    hist_p hist;

    hist = hist_create();
    if (hist == NULL)
        PANIC("can't create history");

    term = (term_p)heap_mmap(sizeof(struct term_t));

    if (term == NULL)
        return NULL;

    // Set the terminal to non-canonical mode
    tcgetattr(STDIN_FILENO, &term->oldattr);
    term->newattr = term->oldattr;
    term->newattr.c_lflag &= ~(ICANON | ECHO | ISIG);
    // Set the minimum number of characters to read and the read timeout
    term->newattr.c_cc[VMIN] = 1;   // Minimum number of characters to read
    term->newattr.c_cc[VTIME] = 0;  // Timeout (in deciseconds) for read
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &term->newattr);

    // Initialize the input buffer
    term->input_len = 0;
    memset(term->input, 0, 8);
    term->buf_len = 0;
    term->buf_pos = 0;
    term->multiline_len = 0;
    term->hist = hist;
    term->autocp_idx.entry = 0;
    term->term_width = 80;
    term->term_height = 24;
    term->prompt_len = 0;
    term->last_total_rows = 1;
    term->last_cursor_row = 0;
    term_get_size(term);

    return term;
}

nil_t term_destroy(term_p term) {
    // Restore the terminal attributes
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &term->oldattr) == -1)
        perror("Failed to restore terminal attributes");

    hist_destroy(term->hist);

    // Unmap the terminal structure
    heap_unmap(term, sizeof(struct term_t));
}

i64_t term_getc(term_p term) {
    i64_t sz;

    sz = (i64_t)read(STDIN_FILENO, term->input + (term->input_len++ % 8), 1);

    if (sz == -1)
        return -1;

    return sz;
}

#endif

nil_t term_prompt(term_p term) {
    obj_p prompt = NULL_OBJ;

    prompt_fmt_into(&prompt);
    term->prompt_len = term_visual_width(AS_C8(prompt), (i32_t)prompt->len);
    printf("%.*s", (i32_t)prompt->len, AS_C8(prompt));
    fflush(stdout);
    drop_obj(prompt);

    // Refresh terminal size on each prompt (handles window resize)
    term_get_size(term);
}

i64_t term_redraw_into(term_p term, obj_p *dst) {
    i64_t i, j, l, n, c;
    str_p verb;

    n = prompt_fmt_into(dst);

    l = term->buf_len;

    for (i = 0; i < l; i++) {
        c = 0;
        switch (term->buf[i]) {
            case KEYCODE_LPAREN:
            case KEYCODE_LCURLY:
            case KEYCODE_LBRACKET:
            case KEYCODE_RPAREN:
            case KEYCODE_RCURLY:
            case KEYCODE_RBRACKET:
                n += str_fmt_into(dst, -1, "%s%c%s", GRAY, term->buf[i], RESET);
                break;
            case ':':
                j = i + 1;
                if (i == 0 && l > 1) {
                    for (; j < l; j++) {
                        if (!is_alphanum(term->buf[j]) && term->buf[j] != '?')
                            break;
                    }

                    c = 1;
                }

                n += str_fmt_into(dst, -1, "%s%.*s%s", GRAY, j - i, term->buf + i, RESET);
                i = j - 1;

                break;
            default:
                if ((i == 0 || !is_alphanum(term->buf[i - 1])) && is_alphanum(term->buf[i])) {
                    for (j = i + 1; j < l; j++) {
                        if (!is_alphanum(term->buf[j]) && term->buf[j] != '-')
                            break;
                    }

                    verb = env_get_internal_keyword_name(term->buf + i, j - i, &term->autocp_idx.index, B8_TRUE);
                    if (verb != NULL) {
                        n += str_fmt_into(dst, -1, "%s%s%s", GREEN, verb, RESET);
                        i += strlen(verb) - 1;
                        c = 1;
                        break;
                    }

                    verb = env_get_internal_function_name(term->buf + i, j - i, &term->autocp_idx.index, B8_TRUE);
                    if (verb != NULL) {
                        n += str_fmt_into(dst, -1, "%s%s%s", GREEN, verb, RESET);
                        i += strlen(verb) - 1;
                        c = 1;
                        break;
                    }
                } else if (is_op(term->buf[i])) {
                    n += str_fmt_into(dst, -1, "%s%c%s", LIGHT_BLUE, term->buf[i], RESET);
                    c = 1;
                } else if (term->buf[i] == '\"') {
                    if (i == 0 || term->buf[i - 1] != '\\') {
                        // string
                        for (j = i + 1; j < l; j++) {
                            if (term->buf[j] == '\"' && term->buf[j - 1] != '\\') {
                                n += str_fmt_into(dst, -1, "%s", YELLOW);
                                n += str_fmt_into(dst, -1, "%.*s", j - i + 1, term->buf + i);
                                n += str_fmt_into(dst, -1, "%s", RESET);
                                i = j;
                                c = 1;
                                break;
                            }
                        }
                    }
                } else if (term->buf[i] == '\'') {
                    // Modified character literal detection
                    if (i + 1 < l && term->buf[i + 1] == '\'') {
                        // This is an empty character literal ('')
                        n += str_fmt_into(dst, -1, "%s%.*s%s", SALAD, 2, term->buf + i, RESET);
                        i += 1;
                        c = 1;
                    } else if (i + 2 < l && term->buf[i + 2] == '\'') {
                        // This is a character literal ('x')
                        n += str_fmt_into(dst, -1, "%s%.*s%s", SALAD, 3, term->buf + i, RESET);
                        i += 2;
                        c = 1;
                    } else {
                        // This is a quoted symbol ('xyz)
                        for (j = i + 1; j < l; j++) {
                            if (!is_alphanum(term->buf[j]) && term->buf[j] != '-')
                                break;
                        }
                        n += str_fmt_into(dst, -1, "%s%.*s%s", CYAN, j - i, term->buf + i, RESET);
                        i = j - 1;
                        c = 1;
                    }
                }

                if (c == 0)
                    n += str_fmt_into(dst, -1, "%c", term->buf[i]);

                break;
        }
    }

    return n;
}

nil_t term_redraw(term_p term) {
    i32_t total_width, i;
    obj_p out = NULL_OBJ;

    cursor_hide();

    // Refresh terminal size
    term_get_size(term);

    // Strategy: Move cursor back to start using backspaces, then clear forward
    // This is more reliable than trying to calculate rows with wrapping

    // First, move cursor to the very beginning by going to column 0 and clearing
    printf("\r");  // Return to column 0 of current line

    // Now we need to clear all the lines we might have used
    // Move up enough lines to cover any possible wrapping
    if (term->last_total_rows > 1) {
        for (i = 1; i < term->last_total_rows; i++) {
            cursor_move_up(1);
            printf("\r");  // Start of each line
        }
    }

    // Now we should be at the start of the first line
    // Clear from here to end of screen to remove all old content
    printf("\033[J");  // Clear from cursor to end of screen

    // Redraw prompt and input (term_redraw_into includes the prompt)
    term_redraw_into(term, &out);
    printf("%.*s", (i32_t)out->len, AS_C8(out));
    drop_obj(out);

    // Calculate and save how many rows the output spans for next time
    total_width = term->prompt_len + term_visual_width(term->buf, term->buf_len);
    if (term->term_width > 0) {
        term->last_total_rows = (total_width + term->term_width - 1) / term->term_width;
        if (term->last_total_rows == 0) term->last_total_rows = 1;
    }

    // Position cursor at buf_pos
    // We're currently at the end of the output
    term_goto_position(term, term->buf_len, term->buf_pos);

    cursor_show();
    fflush(stdout);
    autocp_reset_current(term);
}

// Helper function to calculate display width of a UTF-8 character
static i64_t utf8_char_width(const c8_t *str, i64_t pos) {
    c8_t byte = str[pos];

    // ASCII character
    if ((byte & 0x80) == 0)
        return 1;

    // UTF-8 start byte
    if ((byte & 0xE0) == 0xC0)
        return 1;  // 2-byte sequence
    if ((byte & 0xF0) == 0xE0)
        return 1;  // 3-byte sequence
    if ((byte & 0xF8) == 0xF0)
        return 2;  // 4-byte sequence (emoji, etc)

    return 1;  // Default fallback
}

// Helper function to find the start of the previous UTF-8 character
static i64_t find_prev_utf8_char(const c8_t *str, i64_t pos) {
    if (pos == 0)
        return 0;

    // Move back one byte
    pos--;

    // If we hit an ASCII character, we're done
    if ((str[pos] & 0x80) == 0)
        return pos;

    // For UTF-8 continuation bytes (10xxxxxx), keep moving back
    while (pos > 0 && (str[pos] & 0xC0) == 0x80) {
        pos--;
    }

    return pos;
}

nil_t term_handle_delete_char(term_p term) {
    if (term->buf_pos < term->buf_len) {
        memmove(term->buf + term->buf_pos, term->buf + term->buf_pos + 1, term->buf_len - term->buf_pos);
        term->buf_len--;
    }
}

nil_t term_handle_backspace(term_p term) {
    if (term->buf_pos == 0)
        return;
    else if (term->buf_pos == term->buf_len) {
        // Find the start of the previous UTF-8 character
        i64_t prev_pos = find_prev_utf8_char(term->buf, term->buf_pos);
        // Calculate display width of the character being removed
        i64_t char_width = utf8_char_width(term->buf, prev_pos);

        term->buf_pos = prev_pos;
        term->buf[term->buf_pos] = '\0';
        term->buf_len = prev_pos;

        // Move cursor back by the display width
        cursor_move_left(char_width);
    } else {
        // Find the start of the previous UTF-8 character
        i64_t prev_pos = find_prev_utf8_char(term->buf, term->buf_pos);
        // Calculate display width of the character being removed
        i64_t char_width = utf8_char_width(term->buf, prev_pos);

        // Calculate how many bytes to move
        i64_t bytes_to_move = term->buf_len - term->buf_pos;
        memmove(term->buf + prev_pos, term->buf + term->buf_pos, bytes_to_move);
        term->buf_len -= (term->buf_pos - prev_pos);
        term->buf_pos = prev_pos;

        // Move cursor back by the display width
        cursor_move_left(char_width);
    }

    term_redraw(term);
}

nil_t autocp_save_current(term_p term) {
    term->autocp_buf_len = term->buf_len;
    term->autocp_buf_pos = term->buf_pos;
    memcpy(term->autocp_buf, term->buf, term->buf_len);
}

nil_t autocp_reset_current(term_p term) {
    term->autocp_buf_len = 0;
    term->autocp_buf_pos = 0;
    term->autocp_idx.entry = 0;
    term->autocp_idx.index = 0;
    term->autocp_idx.sbidx = 0;
}

nil_t term_highlight_pos(term_p term, i64_t pos) {
    i64_t n;

    n = term->buf_pos - pos;

    cursor_hide();
    cursor_move_left(n);
    printf("%s%c%s", BACK_CYAN, term->buf[pos], RESET);
    fflush(stdout);
    timer_sleep(80);
    cursor_show();
}

c8_t opposite_paren(c8_t c) {
    switch (c) {
        case KEYCODE_LPAREN:
            return KEYCODE_RPAREN;
        case KEYCODE_LCURLY:
            return KEYCODE_RCURLY;
        case KEYCODE_LBRACKET:
            return KEYCODE_RBRACKET;
        case KEYCODE_RPAREN:
            return KEYCODE_LPAREN;
        case KEYCODE_RCURLY:
            return KEYCODE_LCURLY;
        case KEYCODE_RBRACKET:
            return KEYCODE_LBRACKET;
        default:
            return c;
    }
}

paren_t term_find_open_paren(term_p term) {
    i32_t i, p, squote, dquote;
    paren_t parens[TERM_BUF_SIZE];

    p = 0;
    squote = -1;
    dquote = -1;

    // Find the last open parenthesis
    for (i = 0; i < term->buf_pos; i++) {
        switch (term->buf[i]) {
            case KEYCODE_RPAREN:
            case KEYCODE_RCURLY:
            case KEYCODE_RBRACKET:
                if (p > 0 && opposite_paren(parens[p - 1].type) == term->buf[i])
                    p--;
                break;
            case KEYCODE_LPAREN:
            case KEYCODE_LCURLY:
            case KEYCODE_LBRACKET:
                parens[p].pos = i;
                parens[p].type = term->buf[i];
                p++;
                break;
            case KEYCODE_SQUOTE:
                if (squote == -1)
                    squote = i;
                else
                    squote = -1;
                break;
            case KEYCODE_DQUOTE:
                if (i == 0 || term->buf[i - 1] != '\\') {
                    if (dquote == -1)
                        dquote = i;
                    else
                        dquote = -1;
                }
                break;
            default:
                break;
        }
    }

    if (squote != -1)
        return (paren_t){squote, KEYCODE_SQUOTE};

    if (dquote != -1)
        return (paren_t){dquote, KEYCODE_DQUOTE};

    if (p > 0)
        return parens[p - 1];

    return (paren_t){-1, 0};
}

b8_t term_autocomplete_word(term_p term) {
    i64_t l, n, len, pos, start, end;
    c8_t *tbuf, *hbuf;
    str_p word;

    tbuf = term->buf;
    hbuf = term->autocp_buf;

    if (term->autocp_buf_len == 0)
        autocp_save_current(term);

    pos = term->autocp_buf_pos;
    len = term->autocp_buf_len;

    // Find start of the word
    for (start = pos; start > 0; start--) {
        if (!is_alphanum(hbuf[start - 1]) && hbuf[start - 1] != '-')
            break;
    }

    // Find end of the word
    for (end = start; end < len; end++) {
        if (!is_alphanum(hbuf[end]) && hbuf[end] != '-')
            break;
    }

    n = end - start;
    if (n == 0)
        return B8_FALSE;

    switch (term->autocp_idx.entry) {
        case 0:
            word = env_get_internal_keyword_name(hbuf + start, n, &term->autocp_idx.index, B8_FALSE);
            if (word != NULL)
                goto redraw;
            term->autocp_idx.index = 0;
            term->autocp_idx.sbidx = 0;
            term->autocp_idx.entry++;
        // fallthrough
        case 1:
            word = env_get_internal_function_name(hbuf + start, n, &term->autocp_idx.index, B8_FALSE);
            if (word != NULL)
                goto redraw;
            term->autocp_idx.index = 0;
            term->autocp_idx.sbidx = 0;
            term->autocp_idx.entry++;
            // fallthrough
        case 2:
            word = env_get_global_name(hbuf + start, n, &term->autocp_idx.index, &term->autocp_idx.sbidx);
            if (word != NULL)
                goto redraw;
            term->autocp_idx.index = 0;
            term->autocp_idx.sbidx = 0;
            term->autocp_idx.entry++;
            // fallthrough
        default:
            term->autocp_idx.index = 0;
            term->autocp_idx.sbidx = 0;
            term->autocp_idx.entry = 0;
            break;
    }

    return B8_FALSE;

    // Note: word is not being freed here bacause it is a global string like entry or symbol
redraw:
    l = strlen(word);
    memcpy(tbuf + start, word, l);
    memcpy(tbuf + start + l, hbuf + end, len - end);
    tbuf[start + l + len - end] = '\0';
    term->buf_len = start + l + len - end;
    term->buf_pos = start + l;
    term_redraw(term);

    return B8_TRUE;
}

b8_t term_autocomplete_path(term_p term, i64_t start) {
    i64_t i, l, n, m, end, len, path_len, prefix_len;
    obj_p files;
    str_p last_slash, file;
    c8_t path[MAX_PATH_LEN], prefix[MAX_PATH_LEN], *hbuf, *tbuf;

    if (term->autocp_buf_len == 0)
        autocp_save_current(term);

    len = term->autocp_buf_len;
    hbuf = term->autocp_buf;
    tbuf = term->buf;

    // Find end of the path
    for (end = start; end < len; end++) {
        if (is_whitespace(hbuf[end]) || hbuf[end] == '"')
            break;
    }

    n = end - start;
    if (n == 0 || n > MAX_PATH_LEN - 1)
        return B8_FALSE;

    last_slash = str_rchr(hbuf + start, '/', n);
    if (last_slash != NULL) {
        path_len = last_slash - hbuf - start + 1;
        memcpy(path, hbuf + start, path_len);
        prefix_len = hbuf + end - last_slash - 1;
        memcpy(prefix, last_slash + 1, prefix_len);
    } else {
        path_len = 2;
        memcpy(path, "./", path_len);
        prefix_len = n;
        memcpy(prefix, hbuf + start, prefix_len);
    }

    path[path_len] = '\0';
    prefix[prefix_len] = '\0';

    files = fs_read_dir(path);
    l = files->len;

    for (i = term->autocp_idx.index; i < l; i++) {
        file = AS_C8(AS_LIST(files)[i]);
        m = AS_LIST(files)[i]->len;

        if (m > 0 && prefix_len <= m && strncmp(prefix, file, prefix_len) == 0) {
            // if the word is the same as the current one, then skip it
            if (prefix_len == m)
                continue;

            strncpy(tbuf + start, path, path_len);
            strncpy(tbuf + start + path_len, file, m);
            strncpy(tbuf + start + path_len + m, hbuf + end, len - end);
            term->buf_len = start + path_len + m + len - end;
            term->buf_pos = start + path_len + m;
            term->autocp_idx.index = i + 1;
            term_redraw(term);
            drop_obj(files);
            return B8_TRUE;
        }
    }

    drop_obj(files);

    return B8_FALSE;
}

b8_t term_autocomplete_paren(term_p term) {
    paren_t open_paren;

    open_paren = term_find_open_paren(term);

    if (open_paren.pos == -1) {
        return term_autocomplete_word(term);
    } else if (open_paren.type == KEYCODE_DQUOTE) {
        if (term_autocomplete_path(term, open_paren.pos + 1))
            return B8_TRUE;
    } else if (term_autocomplete_word(term)) {
        return B8_TRUE;
    }

    term_highlight_pos(term, open_paren.pos);

    if (term->buf_pos < term->buf_len)
        memmove(term->buf + term->buf_pos + 1, term->buf + term->buf_pos, term->buf_len - term->buf_pos);

    term->buf[term->buf_pos] = opposite_paren(open_paren.type);
    term->buf_pos++;
    term->buf_len++;

    term_redraw(term);

    return B8_TRUE;
}

nil_t term_handle_tab(term_p term) { term_autocomplete_paren(term); }

// Check if parentheses, brackets, and braces are balanced,
// AND that all double-quoted strings are properly closed (no unclosed double-quoted strings).
// Checks the buffer passed in (either term->buf or term->multiline_buf)
b8_t term_check_balance(c8_t *buf, i32_t len) {
    i32_t i, depth, in_dquote, escape;
    c8_t stack[TERM_BUF_SIZE];

    depth = 0;
    in_dquote = 0;
    escape = 0;

    for (i = 0; i < len; i++) {
        c8_t c = buf[i];

        // Handle escape sequences
        if (escape) {
            escape = 0;
            continue;
        }

        if (c == '\\') {
            escape = 1;
            continue;
        }

        // Toggle double-quote state
        if (c == KEYCODE_DQUOTE) {
            in_dquote = !in_dquote;
            continue;
        }

        // Skip characters inside double-quoted strings
        if (in_dquote)
            continue;

        // Handle single-quote (symbol literal) - skip the next non-whitespace sequence
        if (c == KEYCODE_SQUOTE) {
            // Single quotes in this language start symbol literals, not strings
            // They don't need balancing, just skip them
            continue;
        }

        // Track opening brackets/parens/braces
        if (c == KEYCODE_LPAREN || c == KEYCODE_LBRACKET || c == KEYCODE_LCURLY) {
            if (depth >= TERM_BUF_SIZE)
                return B8_FALSE;  // Stack overflow
            stack[depth++] = c;
        }
        // Track closing brackets/parens/braces
        else if (c == KEYCODE_RPAREN || c == KEYCODE_RBRACKET || c == KEYCODE_RCURLY) {
            if (depth == 0)
                return B8_FALSE;  // More closing than opening

            c8_t expected = opposite_paren(stack[depth - 1]);
            if (c != expected)
                return B8_FALSE;  // Mismatched brackets

            depth--;
        }
    }

    // Balanced if: no unclosed brackets AND not inside a double-quoted string
    return (depth == 0 && !in_dquote) ? B8_TRUE : B8_FALSE;
}

obj_p term_handle_return(term_p term) {
    i64_t r, exit_code = 0;
    obj_p res = NULL_OBJ;
    b8_t onoff;
    i32_t total_len;

    if (term->buf_len == 0 && term->multiline_len == 0)
        return NULL_OBJ;

    term->buf[term->buf_len] = '\0';

    // Handle commands only if not in multi-line mode
    if (term->multiline_len == 0) {
        if (IS_CMD(term, ":q")) {
            r = i64_from_str(term->buf + 2, term->buf_len - 2, &exit_code);
            if (r != term->buf_len - 2)
                exit_code = 0;
            poll_exit(runtime_get()->poll, exit_code);
            return NULL;
        }

        if (IS_CMD(term, ":u")) {
            onoff = (term->buf_len > 2 && term->buf[3] == '1') ? B8_TRUE : B8_FALSE;
            format_set_use_unicode(onoff);
            printf("\n%s. Format use unicode: %s.%s", YELLOW, onoff ? "on" : "off", RESET);
            hist_add(term->hist, term->buf, term->buf_len);
            return NULL_OBJ;
        }

        if (IS_CMD(term, ":t")) {
            onoff = (term->buf_len > 2 && term->buf[3] == '1') ? B8_TRUE : B8_FALSE;
            timeit_activate(onoff);
            printf("\n%s. Timeit is %s.%s", YELLOW, onoff ? "on" : "off", RESET);
            hist_add(term->hist, term->buf, term->buf_len);
            return NULL_OBJ;
        }

        if (IS_CMD(term, ":?")) {
            printf("\n%s. Commands list:%s\n%s%s%s", YELLOW, RESET, GRAY, COMMANDS_LIST, RESET);
            return NULL_OBJ;
        }
    }

    // Append current line to multiline buffer
    total_len = term->multiline_len + term->buf_len;
    if (total_len < TERM_BUF_SIZE) {
        memcpy(term->multiline_buf + term->multiline_len, term->buf, term->buf_len);
        term->multiline_len = total_len;
        term->multiline_buf[term->multiline_len] = '\0';
    } else {
        // Buffer overflow - handle error and skip further processing
        fprintf(stderr, "%sError: input too long for multiline buffer.%s\n", RED, RESET);
        return NULL_OBJ;
    }

    // Check if parentheses/brackets are balanced on the complete multiline buffer
    if (!term_check_balance(term->multiline_buf, term->multiline_len)) {
        // Unbalanced - add newline to multiline buffer and continue
        if (term->multiline_len + 1 < TERM_BUF_SIZE) {
            term->multiline_buf[term->multiline_len++] = '\n';
            term->multiline_buf[term->multiline_len] = '\0';
        } else {
            // Buffer overflow: reset multiline buffer and inform user
            term->multiline_len = 0;
            printf("\n%sError: Multiline input too long, buffer reset.%s\n", RED, RESET);
        }
        return NULL;  // Signal "not ready to evaluate"
    }

    // Balanced - create result from multiline buffer
    res = cstring_from_str(term->multiline_buf, term->multiline_len);
    hist_add(term->hist, term->multiline_buf, term->multiline_len);

    // Reset multiline buffer
    term->multiline_len = 0;

    return res;
}

obj_p term_handle_escape(term_p term) {
    i64_t l;

    // Up arrow esc
    if (IS_ESC(term, "\x1b[A")) {
        hist_save_current(term->hist, term->buf, term->buf_len);
        l = hist_prev(term->hist, term->buf);
        if (l > 0) {
            term->buf_len = l;
            term->buf_pos = l;
            term_redraw(term);
        }
        goto proceed;
    }

    // Down arrow esc
    if (IS_ESC(term, "\x1b[B")) {
        l = hist_next(term->hist, term->buf);
        if (l > 0) {
            term->buf_len = l;
            term->buf_pos = l;
        } else {
            l = hist_restore_current(term->hist, term->buf);
            term->buf_len = l;
            term->buf_pos = l;
        }
        term_redraw(term);
        goto proceed;
    }

    // CTRL+Right/ALT+Right (wordwise) esc
    if (IS_ESC(term, "\x1b""f") || IS_ESC(term,"\x1b[5C")) {
        if(term->buf_pos < term->buf_len) {
            i32_t old_pos = term->buf_pos;
            term->buf_pos++;
            while(term->buf_pos < term->buf_len && is_alphanum(term->buf[term->buf_pos]))term->buf_pos++;
            term_goto_position(term, old_pos, term->buf_pos);
            fflush(stdout);
        }
        goto proceed;
    }

    // Right arrow esc
    if (IS_ESC(term, "\x1b[C")) {
        if (term->buf_pos < term->buf_len) {
            i32_t old_pos = term->buf_pos;
            term->buf_pos++;
            term_goto_position(term, old_pos, term->buf_pos);
            fflush(stdout);
        }
        goto proceed;
    }

    // CTRL+Left/ALT+Left (wordwise) esc
    if (IS_ESC(term, "\x1b""b") || IS_ESC(term,"\x1b[5D")) {
        if(term->buf_pos > 0) {
            i32_t old_pos = term->buf_pos;
            term->buf_pos--;
            while(term->buf_pos > 0 && is_alphanum(term->buf[term->buf_pos-1]))term->buf_pos--;
            term_goto_position(term, old_pos, term->buf_pos);
            fflush(stdout);
        }
        goto proceed;
    }

    // Left arrow esc
    if (IS_ESC(term, "\x1b[D")) {
        if (term->buf_pos > 0) {
            i32_t old_pos = term->buf_pos;
            term->buf_pos--;
            term_goto_position(term, old_pos, term->buf_pos);
            fflush(stdout);
        }
        goto proceed;
    }

    // Home esc
    if (IS_ESC(term, "\x1b[1~")||IS_ESC(term, "\x1b[H")) {
        if (term->buf_pos > 0) {
            i32_t old_pos = term->buf_pos;
            term->buf_pos = 0;
            term_goto_position(term, old_pos, term->buf_pos);
            fflush(stdout);
        }
        goto proceed;
    }

    // End esc
    if (IS_ESC(term,"\x1b[4~")||IS_ESC(term, "\x1b[F")) {
        if (term->buf_len > 0) {
            i32_t old_pos = term->buf_pos;
            term->buf_pos = term->buf_len;
            term_goto_position(term, old_pos, term->buf_pos);
            fflush(stdout);
        }
        goto proceed;
    }

    // Delete esc
    if (IS_ESC(term, "\x1b[3~")) {
        term_handle_delete_char(term);
        term_redraw(term);
        goto proceed;
    }

    return NULL;

proceed:
    term->input_len = 0;
    return NULL;
}

obj_p term_handle_symbol(term_p term) {
    if (term->buf_len + 1 >= TERM_BUF_SIZE)
        return NULL;

    if (term->buf_pos < term->buf_len)
        memmove(term->buf + term->buf_pos + 1, term->buf + term->buf_pos, term->buf_len - term->buf_pos);

    term->buf[term->buf_pos] = term->input[0];
    term->buf_len++;
    term->buf_pos++;
    term->buf[term->buf_len] = '\0';

    term_redraw(term);

    return NULL;
}

nil_t term_handle_ctrl_u(term_p term) {
    // Move cursor to start of line
    cursor_move_left(term->buf_pos);
    // Clear the entire line
    line_clear();
    // Reset buffer position and length
    term->buf_pos = 0;
    term->buf_len = 0;
    term->buf[0] = '\0';
    // Reset history state
    hist_reset_current(term->hist);
    // Redraw the prompt
    term_prompt(term);
}

obj_p term_read(term_p term) {
    obj_p res = NULL;

#if defined(OS_WINDOWS)
    mutex_lock(&term->lock);
#endif

    switch (term->input[0]) {
        case KEYCODE_RETURN:
            res = term_handle_return(term);
            autocp_reset_current(term);
            term->input_len = 0;

            // Only reset buffer and add newline if we got a result to evaluate
            if (res != NULL && res != NULL_OBJ) {
                term->buf_len = 0;
                term->buf_pos = 0;
                term->multiline_len = 0;
                line_new();
            } else if (res == NULL) {
                // Unbalanced expression - clear current line buffer and show continuation prompt
                term->buf_len = 0;
                term->buf_pos = 0;
                line_new();
                term_prompt(term);
            } else {
                // NULL_OBJ means command was processed, reset buffers
                term->buf_len = 0;
                term->buf_pos = 0;
                term->multiline_len = 0;
                line_new();
            }

            fflush(stdout);
            break;
        case KEYCODE_BACKSPACE:
        case KEYCODE_DELETE:
            autocp_reset_current(term);
            term_handle_backspace(term);
            term->input_len = 0;
            break;
        case KEYCODE_TAB:
            term_handle_tab(term);
            term->input_len = 0;
            break;
        case KEYCODE_CTRL_U:
        case KEYCODE_CTRL_C:
            autocp_reset_current(term);
            term_handle_ctrl_u(term);
            term->input_len = 0;
            break;
        case KEYCODE_CTRL_A:
            term_goto_position(term, term->buf_pos, 0);fflush(stdout);
            term->buf_pos = 0;term->input_len = 0;
            break;

        case KEYCODE_CTRL_B:
            if(term->buf_pos) {
                term_goto_position(term, term->buf_pos, term->buf_pos-1);
                term->buf_pos--;
                fflush(stdout);
            }
            term->input_len = 0;
            break;

        case KEYCODE_CTRL_D:
            if(term->buf_pos == 0 && term->buf_len == 0){
                poll_exit(runtime_get()->poll, 0);
            } else {
                term_handle_delete_char(term);
                term_redraw(term);
            }
            term->input_len = 0;
            break;

        case KEYCODE_CTRL_E:
            term_goto_position(term, term->buf_pos, term->buf_len);fflush(stdout);
            term->buf_pos = term->buf_len;
            term->input_len = 0;
            break;

        case KEYCODE_CTRL_F:
            if(term->buf_pos < term->buf_len) {
                term_goto_position(term, term->buf_pos, term->buf_pos + 1);
                term->buf_pos++;
                fflush(stdout);
            }
            term->input_len = 0;
            break;

        case KEYCODE_CTRL_K:
            while(term->buf_pos<term->buf_len)term_handle_delete_char(term);
            term_redraw(term);
            term->input_len = 0;
            break;
        case KEYCODE_CTRL_N:
            hist_save_current(term->hist, term->buf, term->buf_len);
            term->buf_len = term->buf_pos = hist_prev(term->hist, term->buf);
            goto update_history;
        case KEYCODE_CTRL_P:
            hist_save_current(term->hist, term->buf, term->buf_len);
            term->buf_len = term->buf_pos = hist_next(term->hist, term->buf);
        update_history:
            if(term->buf_len < 0)term->buf_len = term->buf_pos = 0;
            term_redraw(term);
            term->input_len = 0;
            break;
        case KEYCODE_CTRL_W:
            autocp_reset_current(term);
            while(term->buf_pos>0&&is_alphanum(term->buf[term->buf_pos-1]))term_handle_backspace(term);
            term->input_len = 0;
            break;
        case KEYCODE_ESCAPE:
            res = term_handle_escape(term);
            break;
        default:
            autocp_reset_current(term);
            res = term_handle_symbol(term);
            term->input_len = 0;
            break;
    }

#if defined(OS_WINDOWS)
    mutex_unlock(&term->lock);
#endif

    return res;
}
