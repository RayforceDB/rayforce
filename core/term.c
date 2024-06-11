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

#define HISTORY_FILE_PATH ".rayhist.dat"
#define HISTORY_SIZE 4096
#define COMMANDS_LIST "\
  :?  - Displays help.\n\
  :g  - Use rich graphic formatting: [0|1].\n\
  :q  - Exits the application: [exit code]."

// Function to extend the file size
i64_t extend_file_size(i64_t fd, u64_t new_size)
{
    if (lseek(fd, new_size - 1, SEEK_SET) == -1)
        return -1;

    if (write(fd, "", 1) == -1)
        return -1;

    return new_size;
}

history_p history_create()
{
    i64_t pos, fd, fsize;
    str_p lines;
    history_p history;

    fd = fs_fopen(HISTORY_FILE_PATH, O_RDWR | ATTR_CREAT);

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

term_p term_create()
{
    term_p term;

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
    term->history = history_create();
    term->tabidx = 0;

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

    printf("\r\033[K"); // Clear the line
    term_redraw_into(term, &fmt);
    printf("%s", as_string(fmt));
    n = term->buf_len - term->buf_pos;
    if (n > 0)
        printf("\033[%dD", n); // Move the cursor to the right position
    fflush(stdout);
    drop_obj(fmt);
}

nil_t term_autocomplete(term_p term)
{
    u64_t i, l, n;
    str_p verb;

    if (term->tabidx == 0)
    {
        debug("SAVE CURRENT HISTORY!!");
        history_save_current(term->history, term->buf, term->buf_len);
    }
    else
    {
        l = history_restore_current(term->history, term->buf);
        term->buf_len = l;
        term->buf_pos = l;
    }

    // Find start of the word
    for (i = term->buf_pos; i > 0; i--)
    {
        if (!is_alphanum(term->buf[i - 1]) && term->buf[i - 1] != '-')
            break;
    }

    n = term->buf_pos - i;

    verb = env_get_internal_function_lit(term->buf + i, n, &term->tabidx, B8_FALSE);
    if (verb != NULL)
    {
        l = strlen(verb);
        strncpy(term->buf + i, verb, l);
        term->buf_len = i + l;
        term->buf_pos = i + l;
        term_redraw(term);
        return;
    }

    verb = env_get_internal_kw_lit(term->buf + i, n, B8_FALSE);
    if (verb != NULL)
    {
        l = strlen(verb);
        strncpy(term->buf + i, verb, l);
        term->buf_len = i + l;
        term->buf_pos = i + l;
        term_redraw(term);
        return;
    }

    verb = env_get_internal_lit_lit(term->buf + i, n, B8_FALSE);
    if (verb != NULL)
    {
        l = strlen(verb);
        strncpy(term->buf + i, verb, l);
        term->buf_len = i + l;
        term->buf_pos = i + l;
        term_redraw(term);
        return;
    }
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

obj_p term_read(term_p term)
{
    c8_t c;
    i64_t l, exit_code;
    obj_p res = NULL;

    if (read(STDIN_FILENO, &c, 1) == 1)
    {
        // Process the character
        switch (c)
        {
        case '\n': // New line
            // Enter key pressed, process the input buffer
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
                res = (term->buf_len) ? cstring_from_str(term->buf, term->buf_len) : NULL_OBJ;

            history_add(term->history, term->buf, term->buf_len);

            term->buf_len = 0;
            term->buf_pos = 0;
            term->tabidx = 0;

            printf("\n");
            fflush(stdout);

            return res;
        case 127:  // Del
        case '\b': // Backspace
            term->tabidx = 0;
            term_backspace(term);
            return res;
        case '\t': // Tab
            term_autocomplete(term);
            return res;
        case '\033': // Escape sequence
            term->tabidx = 0;
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
                            printf("\033[%dC", 1);
                            fflush(stdout);
                        }
                        break;
                    case 'D': // Left arrow
                        if (term->buf_pos > 0)
                        {
                            term->buf_pos--;
                            printf("\033[%dD", 1);
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
            term->tabidx = 0;
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
