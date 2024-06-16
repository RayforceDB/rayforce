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

#define HISTORY_FILE_PATH ".rayhist.dat"
#define HISTORY_SIZE 4096
#define COMMANDS_LIST "\
  :?  - Displays help.\n\
  :g  - Use rich graphic formatting: [0|1].\n\
  :q  - Exits the application: [exit code]."

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

history_p history_create()
{
    i64_t pos, fd, fsize;
    str_p lines;
    history_p history;

    fd = fs_fopen(HISTORY_FILE_PATH, GENERIC_READ | GENERIC_WRITE);

    if (fd == -1)
    {
        perror("can't open history file");
        return NULL;
    }

    fsize = fs_fsize(fd);
    if (fsize == 0)
    {
        // Set initial file size if the file is empty
        if (extend_file_size(fd, HISTORY_SIZE) == -1)
        {
            perror("can't truncate history file");
            fs_fclose(fd);
            return NULL;
        }

        fsize = HISTORY_SIZE;
    }

    // Map file to memory
    lines = (str_p)mmap_file(fd, fsize, 1);
    if (lines == NULL)
    {
        perror("can't map history file");
        fs_fclose(fd);
        return NULL;
    }

    history = (history_p)heap_mmap(sizeof(struct history_t));
    if (history == NULL)
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

    history->fd = fd;
    history->lines = lines;
    history->size = fsize;
    history->pos = pos;
    history->search_dir = 1;
    history->index = (pos == 0) ? 0 : pos - 1;
    history->curr_saved = 0;
    history->curr_len = 0;

    return history;
}

nil_t history_destroy(history_p history)
{
    if (mmap_sync(history->lines, history->size) == -1)
        perror("can't sync history buffer");

    mmap_free(history->lines, history->size);
    fs_fclose(history->fd);
    heap_unmap(history, sizeof(struct history_t));
}

nil_t history_add(history_p history, c8_t buf[], u64_t len)
{
    u64_t pos, size, index, last_len;

    pos = history->pos;
    size = history->size;
    index = history->index;

    // Find the previous line
    while (index > 0)
    {
        if (history->lines[--index] == '\n')
        {
            last_len = history->index - index - 1;
            // Check if the line is already in the history buffer
            if (last_len == len && strncmp(history->lines + index + 1, buf, len) == 0)
                return;

            break;
        }
    }

    if (index == 0)
    {
        // Check if the line is already in the history buffer
        if (history->index == len && strncmp(history->lines, buf, len) == 0)
            return;
    }

    pos = history->pos;

    if (len + pos + 1 > size)
    {
        // Resize the history buffer
        size = size * 2;
        history->lines = (str_p)mmap_reserve(history->lines, size);
        if (history->lines == NULL)
        {
            perror("can't resize history buffer");
            return;
        }

        history->size = size;
    }

    // Add the line to the history buffer
    memcpy(history->lines + pos, buf, len);
    history->lines[pos + len] = '\n';
    history->pos += len + 1;
    history->index = history->pos - 1;
    history->search_dir = 1;

    // Sync the history buffer to the file
    if (mmap_sync(history->lines, history->size) == -1)
        perror("can't sync history buffer");
}

i64_t history_prev(history_p history, c8_t buf[])
{
    u64_t index = history->index;
    i64_t len = 0;

    if (index == 0)
        return len;

    // Skip current line if search direction was next
    if (history->search_dir == 0)
    {
        while (index > 0)
        {
            if (history->lines[--index] == '\n')
                break;
        }

        history->index = index;
        history->search_dir = 1;
    }

    // Find the previous line
    while (index > 0)
    {
        if (history->lines[--index] == '\n')
        {
            len = history->index - index - 1;
            strncpy(buf, history->lines + index + 1, len);
            buf[len] = '\0';
            history->index = index;
            return len;
        }
    }

    len = history->index;
    strncpy(buf, history->lines, len);
    buf[len] = '\0';
    history->index = index;
    history->search_dir = 1;

    return len;
}

i64_t history_next(history_p history, c8_t buf[])
{
    u64_t index = history->index;
    i64_t len = 0;

    // Skip current line if search direction was previous
    if (history->search_dir == 1)
    {
        while (index + 1 < history->pos)
        {
            if (history->lines[++index] == '\n')
                break;
        }

        history->index = index;
        history->search_dir = 0;
    }

    // Find the next line
    while (index + 1 < history->pos)
    {
        if (history->lines[++index] == '\n')
        {
            len = index - history->index - 1;
            strncpy(buf, history->lines + history->index + 1, len);
            buf[len] = '\0';
            break;
        }
    }

    history->index = index;

    if (len == 0)
        history->search_dir = 1;

    return len;
}

i64_t history_save_current(history_p history, c8_t buf[], u64_t len)
{
    if (history->curr_saved == 1)
        return 0;

    memcpy(history->curr, buf, len);
    history->curr[len] = '\0';
    history->curr_len = len;
    history->curr_saved = 1;
    return len;
}

i64_t history_restore_current(history_p history, c8_t buf[])
{
    u64_t l;
    l = history->curr_len;

    if (history->curr_saved == 0)
        return l;

    memcpy(buf, history->curr, l);
    buf[l] = '\0';
    history->curr_saved = 0;
    return l;
}

nil_t history_reset_current(history_p history)
{
    history->curr_saved = 0;
    history->curr_len = 0;
}

#if defined(_WIN32) || defined(__CYGWIN__)

term_p term_create()
{
    term_p term;
    history_p history;

    history = history_create();
    if (history == NULL)
        panic("can't create history");

    term = (term_p)heap_mmap(sizeof(struct term_t));

    if (term == NULL)
        return NULL;

    // Save the current input mode
    GetConsoleMode(STDIN_FILENO, &term->oldMode);

    // For windows 10, set the output encoding to UTF-8
    // [Console]::OutputEncoding = [System.Text.Encoding]::UTF8

    // Set the new input mode (disable line input, echo input, etc.)
    term->newMode = term->oldMode;
    term->newMode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    // term->newMode &= ~ENABLE_LINE_INPUT;
    SetConsoleMode(STDIN_FILENO, term->newMode);

    // Initialize the input buffer
    term->buf_len = 0;
    term->buf_pos = 0;
    term->history = history;
    term->fnidx = 0;
    term->varidx = 0;
    term->colidx = 0;

    return term;
}

nil_t term_destroy(term_p term)
{
    // Restore the terminal attributes
    SetConsoleMode(STDIN_FILENO, term->oldMode);

    history_destroy(term->history);

    // Unmap the terminal structure
    heap_unmap(term, sizeof(struct term_t));
}

#else

term_p term_create()
{
    term_p term;
    history_p history;

    history = history_create();
    if (history == NULL)
        panic("can't create history");

    term = (term_p)heap_mmap(sizeof(struct term_t));

    if (term == NULL)
        return NULL;

    // Set the terminal to non-canonical mode
    tcgetattr(STDIN_FILENO, &term->oldattr);
    term->newattr = term->oldattr;
    term->newattr.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &term->newattr);

    // Initialize the input buffer
    term->buf_len = 0;
    term->buf_pos = 0;
    term->history = history;
    term->fnidx = 0;
    term->varidx = 0;
    term->colidx = 0;

    return term;
}

nil_t term_destroy(term_p term)
{
    // Restore the terminal attributes
    tcsetattr(STDIN_FILENO, TCSANOW, &term->oldattr);

    history_destroy(term->history);

    // Unmap the terminal structure
    heap_unmap(term, sizeof(struct term_t));
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
        case '(':
        case ')':
        case '[':
        case ']':
        case '{':
        case '}':
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
                    n += str_fmt_into(dst, -1, "%s%s%s%s", BOLD, GREEN, verb, RESET);
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
    obj_p fmt = NULL_OBJ;
    i32_t n;

    line_clear();
    term_redraw_into(term, &fmt);
    printf("%s", as_string(fmt));
    n = term->buf_len - term->buf_pos;
    if (n > 0)
        cursor_move_left(n);
    fflush(stdout);
    drop_obj(fmt);
}

nil_t term_backspace(term_p term)
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

nil_t term_autocomplete(term_p term)
{
    u64_t l, n, len, pos, start, end;
    c8_t *tbuf, *hbuf;
    str_p verb;

    pos = term->buf_pos;
    len = term->history->curr_len;
    hbuf = term->history->curr;
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
        return;

    verb = env_get_internal_function_lit(tbuf + start, n, &term->fnidx, B8_FALSE);
    if (verb != NULL)
        goto redraw;

    verb = env_get_internal_kw_lit(tbuf + start, n, B8_FALSE);
    if (verb != NULL)
        goto redraw;

    verb = env_get_internal_lit_lit(tbuf + start, n, B8_FALSE);
    if (verb != NULL)
        goto redraw;

    verb = env_get_global_lit_lit(tbuf + start, n, &term->varidx, &term->colidx);
    if (verb != NULL)
        goto redraw;

    return;

redraw:
    l = strlen(verb);
    strncpy(tbuf + start, verb, l);
    strncpy(tbuf + start + l, hbuf + end, len - end);
    term->buf_len = start + l + len - end;
    term->buf_pos = start + l;
    term_redraw(term);
}

nil_t term_reset_idx(term_p term)
{
    term->fnidx = 0;
    term->varidx = 0;
    term->colidx = 0;
}

#if defined(_WIN32) || defined(__CYGWIN__)

obj_p term_read(term_p term)
{
    INPUT_RECORD inputRecord;
    DWORD events;
    i64_t exit_code;
    u64_t l;
    c8_t c = term->input;
    obj_p res = NULL;

    switch (c)
    {
    case '\r': // Enter key
        if (term->buf_len == 0)
        {
            res = NULL_OBJ;
            goto ret;
        }

        term->buf[term->buf_len] = '\0';

        if (strncmp(term->buf, ":q", 2) == 0)
        {
            exit_code = (term->buf_len > 2) ? i64_from_str(term->buf + 2, term->buf_len - 3) : 0;
            poll_exit(runtime_get(), exit_code);
        }
        else if (strncmp(term->buf, ":?", 2) == 0)
        {
            printf("\n%s** Commands list:\n%s%s", YELLOW, COMMANDS_LIST, RESET);
            fflush(stdout);
            res = NULL_OBJ;
        }
        else
            res = cstring_from_str(term->buf, term->buf_len);

        history_add(term->history, term->buf, term->buf_len);

    ret:
        term->buf_len = 0;
        term->buf_pos = 0;
        term_reset_idx(term);
        history_reset_current(term->history);
        printf("\n");
        fflush(stdout);

        return res;
    case '\b': // Backspace key
        term_reset_idx(term);
        history_reset_current(term->history);
        term_backspace(term);
        return res;
    case '\t': // Tab key
        history_save_current(term->history, term->buf, term->buf_len);
        term_autocomplete(term);
        return res;
    case 0: // Non-printable character, handle arrow keys and other control keys
        switch (inputRecord.Event.KeyEvent.wVirtualKeyCode)
        {
        case VK_UP: // Up arrow key
            history_save_current(term->history, term->buf, term->buf_len);
            l = history_prev(term->history, term->buf);
            if (l > 0)
            {
                term->buf_len = l;
                term->buf_pos = l;
                term_redraw(term);
            }
            break;
        case VK_DOWN: // Down arrow key
            l = history_next(term->history, term->buf);
            if (l > 0)
            {
                term->buf_len = l;
                term->buf_pos = l;
            }
            else
            {
                l = history_restore_current(term->history, term->buf);
                term->buf_len = l;
                term->buf_pos = l;
            }

            term_redraw(term);
            break;
        case VK_RIGHT: // Right arrow key
            if (term->buf_pos < term->buf_len)
            {
                term->buf_pos++;
                cursor_move_right(1);
                fflush(stdout);
            }
            break;
        case VK_LEFT: // Left arrow key
            if (term->buf_pos > 0)
            {
                term->buf_pos--;
                cursor_move_left(1);
                fflush(stdout);
            }
            break;
        case VK_HOME: // Home key
            if (term->buf_pos > 0)
            {
                cursor_move_left(term->buf_pos);
                term->buf_pos = 0;
                fflush(stdout);
            }
            break;
        case VK_END: // End key
            if (term->buf_len > 0)
            {
                cursor_move_right(term->buf_len - term->buf_pos);
                term->buf_pos = term->buf_len;
                fflush(stdout);
            }
            break;
        default:
            break;
        }
        return res;
    default:
        term_reset_idx(term);
        history_reset_current(term->history);
        // Regular character
        if (term->buf_pos < term->buf_len)
        {
            memmove(term->buf + term->buf_pos + 1, term->buf + term->buf_pos, term->buf_len - term->buf_pos);
            term->buf[term->buf_pos] = c;
            term->buf_len++;
            term->buf_pos++;
        }
        else if (term->buf_pos == term->buf_len && term->buf_len < TERM_BUF_SIZE - 1)
        {
            term->buf[term->buf_len++] = c;
            term->buf_pos++;
        }

        term->buf[term->buf_len] = '\0';

        term_redraw(term);
        return res;
    }

    return res;
}

#else

obj_p term_read(term_p term)
{
    c8_t c;
    u64_t l;
    i64_t exit_code;
    obj_p res = NULL;

    if (read(STDIN_FILENO, &c, 1) == 1)
    {
        // Process the character
        switch (c)
        {
        case '\n': // New line
            if (term->buf_len == 0)
            {
                res = NULL_OBJ;
                goto ret;
            }

            term->buf[term->buf_len] = '\0';

            if (strncmp(term->buf, ":q", 2) == 0)
            {
                if (term->buf_len > 2)
                    exit_code = i64_from_str(term->buf + 2, term->buf_len - 3);
                else
                    exit_code = 0;

                poll_exit(runtime_get()->poll, exit_code);
            }
            else if (strncmp(term->buf, ":?", 2) == 0)
            {
                printf("\n%s** Commands list:\n%s%s", YELLOW, COMMANDS_LIST, RESET);
                fflush(stdout);
                res = NULL_OBJ;
            }
            else
                res = cstring_from_str(term->buf, term->buf_len);

            history_add(term->history, term->buf, term->buf_len);

        ret:
            term->buf_len = 0;
            term->buf_pos = 0;
            term_reset_idx(term);
            history_reset_current(term->history);
            printf("\n");
            fflush(stdout);

            return res;
        case 127:  // Del
        case '\b': // Backspace
            term_reset_idx(term);
            history_reset_current(term->history);
            term_backspace(term);
            return res;
        case '\t': // Tab
            history_save_current(term->history, term->buf, term->buf_len);
            term_autocomplete(term);
            return res;
        case '\033': // Escape sequence
            term_reset_idx(term);
            if (read(STDIN_FILENO, &c, 1) == 1 && c == '[')
            {
                if (read(STDIN_FILENO, &c, 1) == 1)
                {
                    switch (c)
                    {
                    case 'A': // Up arrow
                        history_save_current(term->history, term->buf, term->buf_len);
                        l = history_prev(term->history, term->buf);
                        if (l > 0)
                        {
                            term->buf_len = l;
                            term->buf_pos = l;
                            term_redraw(term);
                        }
                        break;
                    case 'B': // Down arrow
                        l = history_next(term->history, term->buf);
                        if (l > 0)
                        {
                            term->buf_len = l;
                            term->buf_pos = l;
                        }
                        else
                        {
                            l = history_restore_current(term->history, term->buf);
                            term->buf_len = l;
                            term->buf_pos = l;
                        }

                        term_redraw(term);
                        break;
                    case 'C': // Right arrow
                        if (term->buf_pos < term->buf_len)
                        {
                            term->buf_pos++;
                            cursor_move_right(1);
                            fflush(stdout);
                        }
                        break;
                    case 'D': // Left arrow
                        if (term->buf_pos > 0)
                        {
                            term->buf_pos--;
                            cursor_move_left(1);
                            fflush(stdout);
                        }
                        break;
                    case 'H': // Home
                        if (term->buf_pos > 0)
                        {
                            cursor_move_left(term->buf_pos);
                            term->buf_pos = 0;
                            fflush(stdout);
                        }
                        break;
                    case 'F': // End
                        if (term->buf_len > 0)
                        {
                            cursor_move_right(term->buf_len - term->buf_pos);
                            term->buf_pos = term->buf_len;
                            fflush(stdout);
                        }
                        break;
                    default:
                        break;
                    }
                }
            }

            return res;
        default:
            term_reset_idx(term);
            history_reset_current(term->history);

            // regular character
            if (term->buf_pos < term->buf_len)
            {
                memmove(term->buf + term->buf_pos + 1, term->buf + term->buf_pos, term->buf_len - term->buf_pos);
                term->buf[term->buf_pos] = c;
                term->buf_len++;
                term->buf_pos++;
            }
            else if (term->buf_pos == term->buf_len && term->buf_len < TERM_BUF_SIZE - 1)
            {
                term->buf[term->buf_len++] = c;
                term->buf_pos++;
            }

            term_redraw(term);

            return res;
        }
    }

    return res;
}

#endif
