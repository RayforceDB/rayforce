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

#define hist_FILE_PATH ".rayhist.dat"
#define hist_SIZE 4096
#define COMMANDS_LIST "\
  :?  - Displays help.\n\
  :g  - Use rich graphic formatting: [0|1].\n\
  :q  - Exits the application: [exit code]."

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

    fd = fs_fopen(hist_FILE_PATH, GENERIC_READ | GENERIC_WRITE);

    if (fd == -1)
    {
        perror("can't open history file");
        return NULL;
    }

    fsize = fs_fsize(fd);
    if (fsize == 0)
    {
        // Set initial file size if the file is empty
        if (extend_file_size(fd, hist_SIZE) == -1)
        {
            perror("can't truncate history file");
            fs_fclose(fd);
            return NULL;
        }

        fsize = hist_SIZE;
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
    HANDLE hConsoleInput = GetStdHandle(STD_INPUT_HANDLE);

    hist = hist_create();
    if (hist == NULL)
        panic("can't create history");

    term = (term_p)heap_mmap(sizeof(struct term_t));

    if (term == NULL)
        return NULL;

    // Save the current input mode
    GetConsoleMode(hConsoleInput, &term->oldMode);

    // For windows 10, set the output encoding to UTF-8
    // [Console]::OutputEncoding = [System.Text.Encoding]::UTF8

    // Set the new input mode (disable line input, echo input, etc.)
    term->newMode = term->oldMode;
    term->newMode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    SetConsoleMode(hConsoleInput, term->newMode);

    // Initialize the input buffer
    term->lock = mutex_create();
    term->buf_len = 0;
    term->buf_pos = 0;
    term->out = NULL_OBJ;
    prompt_fmt_into(&term->out);
    term->hist = hist;
    term->fnidx = 0;
    term->varidx = 0;
    term->colidx = 0;

    return term;
}

nil_t term_destroy(term_p term)
{
    HANDLE hConsoleInput;

    // Restore the terminal attributes
    hConsoleInput = GetStdHandle(STD_INPUT_HANDLE);
    SetConsoleMode(hConsoleInput, term->oldMode);

    hist_destroy(term->hist);

    mutex_destroy(&term->lock);

    drop_obj(term->out);

    // Unmap the terminal structure
    heap_unmap(term, sizeof(struct term_t));
}

b8_t __getc(c8_t *c)
{
    DWORD bytesRead;
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);

    return ReadFile(hStdin, (LPVOID)c, 1, &bytesRead, NULL);
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
    term->newattr.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &term->newattr);

    // Initialize the input buffer
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
    tcsetattr(STDIN_FILENO, TCSANOW, &term->oldattr);

    hist_destroy(term->hist);

    // Unmap the terminal structure
    heap_unmap(term, sizeof(struct term_t));
}

b8_t __getc(c8_t *c)
{
    return read(STDIN_FILENO, c, 1) == 1;
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

i32_t term_out_diff(obj_p old_out, obj_p new_out)
{
    i32_t pos, old_len, new_len, len;
    str_p old_str, new_str;

    old_len = old_out->len;
    new_len = new_out->len;
    old_str = as_string(old_out);
    new_str = as_string(new_out);

    len = (old_len < new_len) ? old_len : new_len;

    // if (old_len == 0)
    //     return 0;

    for (pos = 0; pos < len; pos++)
    {
        if (old_str[pos] != new_str[pos])
            return pos;
    }

    return len;
}

nil_t term_redraw(term_p term)
{
    obj_p out = NULL_OBJ;
    i32_t n, offset, diff;

    term_redraw_into(term, &out);

    // Compare old out with new out
    diff = term_out_diff(term->out, out);

    if (diff < term->out->len - 1)
    {
        offset = term->out->len - diff;
        cursor_move_left(offset);
        // line_clear();
    }

    printf("%s", as_string(out) + diff);
    fflush(stdout);

    drop_obj(term->out);
    term->out = out;
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

obj_p term_read(term_p term)
{
    i64_t exit_code;
    u64_t l, n;
    obj_p res = NULL;

    mutex_lock(&term->lock);

    switch (term->nextc)
    {
    case KEYCODE_RETURN:
        if (term->buf_len == 0)
        {
            res = NULL_OBJ;
            goto ret;
        }

        term->buf[term->buf_len] = '\0';

        if (strncmp(term->buf, ":q", 2) == 0)
        {
            exit_code = (term->buf_len > 2) ? i64_from_str(term->buf + 2, term->buf_len - 3) : 0;
            poll_exit(runtime_get()->poll, exit_code);
        }
        else if (strncmp(term->buf, ":?", 2) == 0)
        {
            printf("\n%s** Commands list:\n%s%s", YELLOW, COMMANDS_LIST, RESET);
            fflush(stdout);
        }
        else
        {
            res = cstring_from_str(term->buf, term->buf_len);
            hist_add(term->hist, term->buf, term->buf_len);
        }

    ret:
        term_reset_idx(term);
        hist_reset_current(term->hist);
        term->buf_len = 0;
        term->buf_pos = 0;
        printf("\n");
        fflush(stdout);

        term_redraw(term);

        break;
    case KEYCODE_BACKSPACE:
    case KEYCODE_DELETE:
        term_reset_idx(term);
        hist_reset_current(term->hist);
        term_backspace(term);
        break;
    case KEYCODE_TAB:
        hist_save_current(term->hist, term->buf, term->buf_len);
        term_autocomplete(term);
        break;
    case KEYCODE_CTRL_C:
        poll_exit(runtime_get()->poll, 0);
        break;
    // case 0: // Non-printable character, handle arrow keys and other control keys
    //     switch (inputRecord.Event.KeyEvent.wVirtualKeyCode)
    //     {
    //     case VK_UP: // Up arrow key
    //         hist_save_current(term->hist, term->buf, term->buf_len);
    //         l = hist_prev(term->hist, term->buf);
    //         if (l > 0)
    //         {
    //             term->buf_len = l;
    //             term->buf_pos = l;
    //             term_redraw(term);
    //         }
    //         break;
    //     case VK_DOWN: // Down arrow key
    //         l = hist_next(term->hist, term->buf);
    //         if (l > 0)
    //         {
    //             term->buf_len = l;
    //             term->buf_pos = l;
    //         }
    //         else
    //         {
    //             l = hist_restore_current(term->hist, term->buf);
    //             term->buf_len = l;
    //             term->buf_pos = l;
    //         }

    //         term_redraw(term);
    //         break;
    //     case VK_RIGHT: // Right arrow key
    //         if (term->buf_pos < term->buf_len)
    //         {
    //             term->buf_pos++;
    //             cursor_move_right(1);
    //             fflush(stdout);
    //         }
    //         break;
    //     case VK_LEFT: // Left arrow key
    //         if (term->buf_pos > 0)
    //         {
    //             term->buf_pos--;
    //             cursor_move_left(1);
    //             fflush(stdout);
    //         }
    //         break;
    //     case VK_HOME: // Home key
    //         if (term->buf_pos > 0)
    //         {
    //             cursor_move_left(term->buf_pos);
    //             term->buf_pos = 0;
    //             fflush(stdout);
    //         }
    //         break;
    //     case VK_END: // End key
    //         if (term->buf_len > 0)
    //         {
    //             cursor_move_right(term->buf_len - term->buf_pos);
    //             term->buf_pos = term->buf_len;
    //             fflush(stdout);
    //         }
    //         break;
    //     default:
    //         break;
    //     }
    //     return res;
    default:
        term_reset_idx(term);
        hist_reset_current(term->hist);

        if (term->buf_pos < term->buf_len)
        {
            memmove(term->buf + term->buf_pos + 1, term->buf + term->buf_pos, term->buf_len - term->buf_pos);
            term->buf[term->buf_pos] = term->nextc;
            term->buf_len++;
            term->buf_pos++;
        }
        else if (term->buf_pos == term->buf_len && term->buf_len < TERM_BUF_SIZE - 1)
        {
            term->buf[term->buf_len++] = term->nextc;
            term->buf_pos++;
        }

        term->buf[term->buf_len] = '\0';
        term_redraw(term);

        break;
    }

    mutex_unlock(&term->lock);

    return res;
}
