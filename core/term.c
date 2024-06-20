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
#include "timer.h"
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
#include "eval.h"

#define HIST_FILE_PATH ".rayhist.dat"
#define HIST_SIZE 4096
#define COMMANDS_LIST "\
  :?  - Displays help.\n\
  :g  - Use rich graphic formatting: [0|1].\n\
  :q  - Exits the application: [exit code]."
#define is_cmd(t, c) ((t)->buf_len == strlen(c) && strncmp((t)->buf, c, strlen(c)) == 0)

nil_t cursor_move_start()
{
    printf("\r");
}

nil_t cursor_move_left(i32_t i)
{
    printf("\033[%dD", i);
}

nil_t cursor_move_right(i32_t i)
{
    printf("\033[%dC", i);
}

nil_t line_clear()
{
    printf("\r\033[K");
}

nil_t line_new()
{
    printf("\n");
}

nil_t cursor_hide()
{
    printf("\e[?25l");
}

nil_t cursor_show()
{
    printf("\e[?25h");
}

// Function to extend the file size
#if defined(_WIN32) || defined(__CYGWIN__)

i64_t extend_file_size(i64_t fd, u64_t new_size)
{
    HANDLE hFile = (HANDLE)fd;
    LARGE_INTEGER li;
    li.QuadPart = new_size;

    // Move the file pointer to the desired position
    if (!SetFilePointerEx(hFile, li, NULL, FILE_BEGIN))
        return -1;

    // Set the end of the file to the current position of the file pointer
    if (!SetEndOfFile(hFile))
        return -1;

    return new_size;
}

#else

i64_t extend_file_size(i64_t fd, u64_t new_size)
{
    if (lseek(fd, new_size - 1, SEEK_SET) == -1)
        return -1;

    if (write(fd, "", 1) == -1)
        return -1;

    return new_size;
}

#endif

hist_p hist_create()
{
    i64_t pos, fd, fsize;
    str_p lines;
    hist_p hist;

    fd = fs_fopen(HIST_FILE_PATH, ATTR_RDWR);

    if (fd == -1)
    {
        perror("can't open history file");
        return NULL;
    }

    fsize = fs_fsize(fd);
    if (fsize == 0)
    {
        // Set initial file size if the file is empty
        if (extend_file_size(fd, HIST_SIZE) == -1)
        {
            perror("can't truncate history file");
            fs_fclose(fd);
            return NULL;
        }

        fsize = HIST_SIZE;
    }

    // Map file to memory
    lines = (str_p)mmap_file(fd, fsize, 1);
    if (lines == NULL)
    {
        perror("can't map history file");
        fs_fclose(fd);
        return NULL;
    }

    hist = (hist_p)heap_mmap(sizeof(struct hist_t));
    if (hist == NULL)
    {
        perror("can't allocate memory for history");
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

    return hist;
}

nil_t hist_destroy(hist_p hist)
{
    if (mmap_sync(hist->lines, hist->size) == -1)
        perror("can't sync history buffer");

    mmap_free(hist->lines, hist->size);
    fs_fclose(hist->fd);
    heap_unmap(hist, sizeof(struct hist_t));
}

nil_t hist_add(hist_p hist, c8_t buf[], u64_t len)
{
    u64_t pos, size, index, last_len;

    pos = hist->pos;
    size = hist->size;
    index = hist->index;

    // Find the previous line
    while (index > 0)
    {
        if (hist->lines[--index] == '\n')
        {
            last_len = hist->index - index - 1;
            // Check if the line is already in the history buffer
            if (last_len == len && strncmp(hist->lines + index + 1, buf, len) == 0)
                return;

            break;
        }
    }

    if (index == 0)
    {
        // Check if the line is already in the history buffer
        if (hist->index == len && strncmp(hist->lines, buf, len) == 0)
            return;
    }

    pos = hist->pos;

    if (len + pos + 1 > size)
    {
        // Resize the history buffer
        size = size * 2;
        hist->lines = (str_p)mmap_reserve(hist->lines, size);
        if (hist->lines == NULL)
        {
            perror("can't resize history buffer");
            return;
        }

        hist->size = size;
    }

    // Add the line to the history buffer
    memcpy(hist->lines + pos, buf, len);
    hist->lines[pos + len] = '\n';
    hist->pos += len + 1;
    hist->index = hist->pos - 1;
    hist->search_dir = 1;

    // Sync the history buffer to the file
    if (mmap_sync(hist->lines, hist->size) == -1)
        perror("can't sync history buffer");
}

i64_t hist_prev(hist_p hist, c8_t buf[])
{
    u64_t index = hist->index;
    i64_t len = 0;

    if (index == 0)
        return len;

    // Skip current line if search direction was next
    if (hist->search_dir == 0)
    {
        while (index > 0)
        {
            if (hist->lines[--index] == '\n')
                break;
        }

        hist->index = index;
        hist->search_dir = 1;
    }

    // Find the previous line
    while (index > 0)
    {
        if (hist->lines[--index] == '\n')
        {
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

i64_t hist_next(hist_p hist, c8_t buf[])
{
    u64_t index = hist->index;
    i64_t len = 0;

    // Skip current line if search direction was previous
    if (hist->search_dir == 1)
    {
        while (index + 1 < hist->pos)
        {
            if (hist->lines[++index] == '\n')
                break;
        }

        hist->index = index;
        hist->search_dir = 0;
    }

    // Find the next line
    while (index + 1 < hist->pos)
    {
        if (hist->lines[++index] == '\n')
        {
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

i64_t hist_save_current(hist_p hist, c8_t buf[], u64_t len)
{
    if (hist->curr_saved == 1)
        return 0;

    memcpy(hist->curr, buf, len);
    hist->curr[len] = '\0';
    hist->curr_len = len;
    hist->curr_saved = 1;
    return len;
}

i64_t hist_restore_current(hist_p hist, c8_t buf[])
{
    u64_t l;
    l = hist->curr_len;

    if (hist->curr_saved == 0)
        return l;

    memcpy(buf, hist->curr, l);
    buf[l] = '\0';
    hist->curr_saved = 0;
    return l;
}

nil_t hist_reset_current(hist_p hist)
{
    hist->curr_saved = 0;
    hist->curr_len = 0;
}

#if defined(_WIN32) || defined(__CYGWIN__)

term_p term_create()
{
    term_p term;
    hist_p hist;
    HANDLE h_stdin, h_stdout;

    h_stdin = GetStdHandle(STD_INPUT_HANDLE);
    h_stdout = GetStdHandle(STD_OUTPUT_HANDLE);

    hist = hist_create();
    if (hist == NULL)
        panic("can't create history");

    term = (term_p)heap_mmap(sizeof(struct term_t));

    if (term == NULL)
        return NULL;

    // For windows 10, set the output encoding to UTF-8
    // [Console]::OutputEncoding = [System.Text.Encoding]::UTF8

    // Set the console output code page to UTF-8
    if (!SetConsoleOutputCP(CP_UTF8))
        use_unicode(B8_FALSE); // Disable unicode support

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
    term->parens_len = 0;
    term->hist = hist;
    term->fnidx = 0;
    term->varidx = 0;
    term->colidx = 0;

    return term;
}

nil_t term_destroy(term_p term)
{
    // Restore the terminal attributes
    SetConsoleMode(term->h_stdin, term->old_stdin_mode);
    SetConsoleMode(term->h_stdout, term->old_stdout_mode);

    mutex_destroy(&term->lock);
    hist_destroy(term->hist);

    // Unmap the terminal structure
    heap_unmap(term, sizeof(struct term_t));
}

i64_t term_getc(term_p term)
{
    c8_t buf[8];
    DWORD bytesRead;

    if (!ReadFile(term->h_stdin, (LPVOID)buf, 1, &bytesRead, NULL))
        return 0;

    mutex_lock(&term->lock);

    if (term->input[0] != KEYCODE_ESCAPE || term->input_len == 3)
        term->input_len = 0;

    term->input[term->input_len++] = buf[0];

    mutex_unlock(&term->lock);

    return (i64_t)bytesRead;
}

#else

term_p term_create()
{
    term_p term;
    hist_p hist;

    hist = hist_create();
    if (hist == NULL)
        panic("can't create history");

    term = (term_p)heap_mmap(sizeof(struct term_t));

    if (term == NULL)
        return NULL;

    // Set the terminal to non-canonical mode
    tcgetattr(STDIN_FILENO, &term->oldattr);
    term->newattr = term->oldattr;
    term->newattr.c_lflag &= ~(ICANON | ECHO | ISIG);
    // Deny long pressing keys as repeated input
    term->newattr.c_cc[VMIN] = 1;
    term->newattr.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &term->newattr);

    // Initialize the input buffer
    term->input_len = 0;
    memset(term->input, 0, 8);
    term->buf_len = 0;
    term->buf_pos = 0;
    term->hist = hist;
    term->fnidx = 0;
    term->varidx = 0;
    term->colidx = 0;

    return term;
}

nil_t term_destroy(term_p term)
{
    // Restore the terminal attributes
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &term->oldattr);

    hist_destroy(term->hist);

    // Unmap the terminal structure
    heap_unmap(term, sizeof(struct term_t));
}

i64_t term_getc(term_p term)
{
    i64_t sz;

    if (term->input[0] != KEYCODE_ESCAPE || term->input_len == 3)
        term->input_len = 0;

    sz = (i64_t)read(STDIN_FILENO, term->input + term->input_len++, 1);

    if (sz == -1)
        return -1;

    return sz;
}

#endif

nil_t term_prompt(term_p term)
{
    unused(term);
    obj_p prompt = NULL_OBJ;

    prompt_fmt_into(&prompt);
    printf("%s", as_string(prompt));
    fflush(stdout);
    drop_obj(prompt);
}

i64_t term_redraw_into(term_p term, obj_p *dst)
{
    u64_t i, j, l, n, c;
    str_p verb;

    n = prompt_fmt_into(dst);

    l = term->buf_len;

    for (i = 0; i < l; i++)
    {
        c = 0;
        switch (term->buf[i])
        {
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
            if (i == 0 && l > 1)
            {
                for (; j < l; j++)
                {
                    if (!is_alphanum(term->buf[j]) && term->buf[j] != '?')
                        break;
                }

                c = 1;
            }

            n += str_fmt_into(dst, -1, "%s%.*s%s", GRAY, j - i, term->buf + i, RESET);
            i = j - 1;

            break;
        default:
            if ((i == 0 || !is_alpha(term->buf[i - 1])) && is_alpha(term->buf[i]))
            {
                for (j = i + 1; j < l; j++)
                {
                    if (!is_alphanum(term->buf[j]) && term->buf[j] != '-')
                        break;
                }

                // try to find in a verbs list
                verb = env_get_internal_function_lit(term->buf + i, j - i, NULL, B8_TRUE);
                if (verb != NULL)
                {
                    n += str_fmt_into(dst, -1, "%s%s%s", GREEN, verb, RESET);
                    i += strlen(verb) - 1;
                    c = 1;
                    break;
                }

                verb = env_get_internal_kw_lit(term->buf + i, j - i, B8_TRUE);
                if (verb != NULL)
                {
                    n += str_fmt_into(dst, -1, "%s%s%s%s", BOLD, YELLOW, verb, RESET);
                    i += strlen(verb) - 1;
                    c = 1;
                    break;
                }

                verb = env_get_internal_lit_lit(term->buf + i, j - i, B8_TRUE);
                if (verb != NULL)
                {
                    n += str_fmt_into(dst, -1, "%s%s%s%s", BOLD, GREEN, verb, RESET);
                    i += strlen(verb) - 1;
                    c = 1;
                    break;
                }
            }
            else if (is_op(term->buf[i]))
            {
                n += str_fmt_into(dst, -1, "%s%c%s", LIGHT_BLUE, term->buf[i], RESET);
                c = 1;
            }
            else if (term->buf[i] == '"')
            {
                // string
                for (j = i + 1; j < l; j++)
                {
                    if (term->buf[j] == '"')
                    {
                        n += str_fmt_into(dst, -1, "%s", YELLOW);
                        n += str_fmt_into(dst, -1, "%.*s", j - i + 1, term->buf + i);
                        n += str_fmt_into(dst, -1, "%s", RESET);
                        i = j;
                        c = 1;
                        break;
                    }
                }
            }
            else if (term->buf[i] == '\'')
            {
                // char
                for (j = i + 1; j < l; j++)
                {
                    if (term->buf[j] == '\'')
                    {
                        n += str_fmt_into(dst, -1, "%s", SALAD);
                        n += str_fmt_into(dst, -1, "%.*s", j - i + 1, term->buf + i);
                        n += str_fmt_into(dst, -1, "%s", RESET);
                        i = j;
                        c = 1;
                        break;
                    }
                }
            }

            if (c == 0)
                n += str_fmt_into(dst, -1, "%c", term->buf[i]);

            break;
        }
    }

    return n;
}

nil_t term_redraw(term_p term)
{
    u64_t n;
    obj_p out = NULL_OBJ;

    term_redraw_into(term, &out);

    cursor_hide();
    cursor_move_start();
    line_clear();

    printf("%s", as_string(out));

    n = term->buf_len - term->buf_pos;
    if (n > 0)
        cursor_move_left(n);

    cursor_show();

    fflush(stdout);

    drop_obj(out);
}

nil_t term_handle_backspace(term_p term)
{
    if (term->buf_pos == 0)
        return;
    else if (term->buf_pos == term->buf_len)
    {
        term->buf_pos--;
        term->buf[term->buf_pos] = '\0';
        term->buf_len--;
    }
    else
    {
        memmove(term->buf + term->buf_pos - 1, term->buf + term->buf_pos, term->buf_len - term->buf_pos);
        term->buf_len--;
        term->buf_pos--;
    }

    term_redraw(term);
}

b8_t term_autocomplete_word(term_p term)
{
    u64_t l, n, len, pos, start, end;
    c8_t *tbuf, *hbuf;
    str_p word;

    pos = term->buf_pos;
    len = term->hist->curr_len;
    hbuf = term->hist->curr;
    tbuf = term->buf;

    // Find start of the word
    for (start = pos; start > 0; start--)
    {
        if (!is_alphanum(tbuf[start - 1]) && tbuf[start - 1] != '-')
            break;
    }

    // Find end of the word
    for (end = start; end < len; end++)
    {
        if (!is_alphanum(hbuf[end]) && hbuf[end] != '-')
            break;
    }

    n = end - start;
    if (n == 0)
        return B8_FALSE;

    word = env_get_internal_function_lit(tbuf + start, n, &term->fnidx, B8_FALSE);
    if (word != NULL)
        goto redraw;

    word = env_get_internal_kw_lit(tbuf + start, n, B8_FALSE);
    if (word != NULL)
        goto redraw;

    word = env_get_internal_lit_lit(tbuf + start, n, B8_FALSE);
    if (word != NULL)
        goto redraw;

    word = env_get_global_lit_lit(tbuf + start, n, &term->varidx, &term->colidx);
    if (word != NULL)
        goto redraw;

    return B8_FALSE;

redraw:
    l = strlen(word);

    // if the word is the same as the current one, then skip it
    if (l == n && strncmp(word, hbuf + start, n) == 0)
        return B8_FALSE;

    strncpy(tbuf + start, word, l);
    strncpy(tbuf + start + l, hbuf + end, len - end);
    term->buf_len = start + l + len - end;
    term->buf_pos = start + l;
    term_redraw(term);

    return B8_TRUE;
}

nil_t term_highlight_pos(term_p term, u64_t pos)
{
    u64_t n;

    n = term->buf_pos - pos;

    cursor_hide();
    cursor_move_left(n);
    printf("%s%c%s", BACK_CYAN, term->buf[pos], RESET);
    fflush(stdout);
    timer_sleep(80);
    cursor_show();
}

c8_t opposite_paren(c8_t c)
{
    switch (c)
    {
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

paren_t term_find_open_paren(term_p term)
{
    i32_t i, p;
    paren_t parens[TERM_BUF_SIZE];

    if (term->buf_pos == 0 || term->buf_pos < term->buf_len)
        return (paren_t){-1, 0};

    p = 0;

    // Find the last open parenthesis
    for (i = term->buf_pos - 1; i >= 0; i--)
    {
        switch (term->buf[i])
        {
        case KEYCODE_RPAREN:
        case KEYCODE_RCURLY:
        case KEYCODE_RBRACKET:
            parens[p].pos = i;
            parens[p].type = term->buf[i];
            p++;
            break;
        case KEYCODE_LPAREN:
        case KEYCODE_LCURLY:
        case KEYCODE_LBRACKET:
            if (p == 0)
                return (paren_t){i, term->buf[i]};

            // if the current parenthesis is the opposite of the last one, then pop it
            if (opposite_paren(parens[p - 1].type) == term->buf[i])
                p--;
            else
                return (paren_t){-1, 0}; // mismatched parenthesis
        default:
            break;
        }
    }

    return (paren_t){-1, 0};
}

b8_t term_autocomplete_paren(term_p term)
{
    paren_t open_paren;

    open_paren = term_find_open_paren(term);

    if (open_paren.pos == -1)
        return B8_FALSE;

    term_highlight_pos(term, open_paren.pos);

    term->buf[term->buf_pos] = opposite_paren(open_paren.type);
    term->buf_pos++;
    term->buf_len++;

    term_redraw(term);

    return B8_TRUE;
}

nil_t term_handle_tab(term_p term)
{
    if (term_autocomplete_word(term))
        return;

    if (term_autocomplete_paren(term))
        return;
}

nil_t term_reset_idx(term_p term)
{
    term->fnidx = 0;
    term->varidx = 0;
    term->colidx = 0;
}

obj_p term_handle_return(term_p term)
{
    i64_t exit_code;
    obj_p res = NULL_OBJ;

    if (term->buf_len == 0)
        return NULL_OBJ;

    term->buf[term->buf_len] = '\0';

    if (is_cmd(term, ":q"))
    {
        exit_code = (term->buf_len > 2) ? i64_from_str(term->buf + 2, term->buf_len - 3) : 0;
        poll_exit(runtime_get()->poll, exit_code);
        return NULL;
    }

    if (is_cmd(term, ":?"))
    {
        printf("\n%s** Commands list:\n%s%s", YELLOW, COMMANDS_LIST, RESET);
        return NULL_OBJ;
    }

    res = cstring_from_str(term->buf, term->buf_len);
    hist_add(term->hist, term->buf, term->buf_len);

    return res;
}

obj_p term_handle_escape(term_p term)
{
    u64_t l;

    // wait for the next key
    if (term->input_len < 3)
        return NULL;

    switch (term->input[2])
    {
    case KEYCODE_UP: // Up arrow key
        hist_save_current(term->hist, term->buf, term->buf_len);
        l = hist_prev(term->hist, term->buf);
        if (l > 0)
        {
            term->buf_len = l;
            term->buf_pos = l;
            term_redraw(term);
        }
        return NULL;
    case KEYCODE_DOWN: // Down arrow key
        l = hist_next(term->hist, term->buf);
        if (l > 0)
        {
            term->buf_len = l;
            term->buf_pos = l;
        }
        else
        {
            l = hist_restore_current(term->hist, term->buf);
            term->buf_len = l;
            term->buf_pos = l;
        }
        term_redraw(term);
        return NULL;
    case KEYCODE_RIGHT: // Right arrow key
        if (term->buf_pos < term->buf_len)
        {
            term->buf_pos++;
            cursor_move_right(1);
            fflush(stdout);
        }
        return NULL;
    case KEYCODE_LEFT: // Left arrow key
        if (term->buf_pos > 0)
        {
            term->buf_pos--;
            cursor_move_left(1);
            fflush(stdout);
        }
        return NULL;
    case KEYCODE_HOME: // Home key
        if (term->buf_pos > 0)
        {
            cursor_move_left(term->buf_pos);
            term->buf_pos = 0;
            fflush(stdout);
        }
        return NULL;
    case KEYCODE_END: // End key
        if (term->buf_len > 0)
        {
            cursor_move_right(term->buf_len - term->buf_pos);
            term->buf_pos = term->buf_len;
            fflush(stdout);
        }
        return NULL;
    default:
        return NULL;
    }
}

obj_p term_handle_symbol(term_p term)
{
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

obj_p term_read(term_p term)
{
    obj_p res = NULL;

#if defined(_WIN32) || defined(__CYGWIN__)
    mutex_lock(&term->lock);
#endif

    switch (term->input[0])
    {
    case KEYCODE_RETURN:
        res = term_handle_return(term);
        term_reset_idx(term);
        hist_reset_current(term->hist);
        term->buf_len = 0;
        term->buf_pos = 0;
        line_new();
        fflush(stdout);
        break;
    case KEYCODE_BACKSPACE:
    case KEYCODE_DELETE:
        term_reset_idx(term);
        hist_reset_current(term->hist);
        term_handle_backspace(term);
        break;
    case KEYCODE_TAB:
        hist_save_current(term->hist, term->buf, term->buf_len);
        term_handle_tab(term);
        break;
    case KEYCODE_CTRL_C:
        poll_exit(runtime_get()->poll, 0);
        break;
    case KEYCODE_ESCAPE:
        res = term_handle_escape(term);
        break;
    default:
        term_reset_idx(term);
        hist_reset_current(term->hist);
        res = term_handle_symbol(term);
        break;
    }

#if defined(_WIN32) || defined(__CYGWIN__)
    mutex_unlock(&term->lock);
#endif

    return res;
}
