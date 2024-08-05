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
#include "time.h"
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

#define MAX_PATH_LEN 128
#define HIST_FILE_PATH ".rayhist.dat"
#define HIST_SIZE 4096 * 1024 // 4MB
#define COMMANDS_LIST "\
  :?  - Displays help.\n\
  :u  - Use unicode for graphic formatting: [0|1].\n\
  :t  - Turns on|off measurement of expressions: [0|1].\n\
  :q  - Exits the application: [exit code]."

#define is_cmd(t, c) ((t)->buf_len >= (i32_t)strlen(c) && strncmp((t)->buf, c, strlen(c)) == 0)
#define is_esc(t, e) ((t)->input_len == (i32_t)strlen(e) && strncmp((t)->input, e, strlen(e)) == 0)
#define PROGRESS_BAR_WIDTH 40

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
#if defined(OS_WINDOWS)

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

    fd = fs_fopen(HIST_FILE_PATH, ATTR_RDWR | ATTR_CREAT);

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
    hist->index = pos - 1;
    size = hist->size;
    index = hist->index;
    last_len = 0;

    // Find the previous line
    // while (index > 0)
    // {
    //     if (hist->lines[--index] == '\n')
    //     {
    //         last_len = hist->index - index - 1;
    //         // Check if the line is already in the history buffer
    //         if (last_len == len && strncmp(hist->lines + index + 1, buf, len) == 0)
    //             return;

    //         break;
    //     }
    // }

    if (index == 0)
    {
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

#if defined(OS_WINDOWS)

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
        format_use_unicode(B8_FALSE); // Disable unicode support

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
    term->hist = hist;
    term->autocp_idx.entry = 0;

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

    term->input[term->input_len++ % 8] = buf[0];

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
    // Set the minimum number of characters to read and the read timeout
    term->newattr.c_cc[VMIN] = 1;  // Minimum number of characters to read
    term->newattr.c_cc[VTIME] = 0; // Timeout (in deciseconds) for read
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &term->newattr);

    // Initialize the input buffer
    term->input_len = 0;
    memset(term->input, 0, 8);
    term->buf_len = 0;
    term->buf_pos = 0;
    term->hist = hist;
    term->autocp_idx.entry = 0;

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

    sz = (i64_t)read(STDIN_FILENO, term->input + (term->input_len++ % 8), 1);

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
    printf("%.*s", (i32_t)prompt->len, as_string(prompt));
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
            if ((i == 0 || !is_alphanum(term->buf[i - 1])) && is_alphanum(term->buf[i]))
            {
                for (j = i + 1; j < l; j++)
                {
                    if (!is_alphanum(term->buf[j]) && term->buf[j] != '-')
                        break;
                }

                verb = env_get_internal_keyword_name(term->buf + i, j - i, &term->autocp_idx.index, B8_TRUE);
                if (verb != NULL)
                {
                    n += str_fmt_into(dst, -1, "%s%s%s", GREEN, verb, RESET);
                    i += strlen(verb) - 1;
                    c = 1;
                    break;
                }

                verb = env_get_internal_function_name(term->buf + i, j - i, &term->autocp_idx.index, B8_TRUE);
                if (verb != NULL)
                {
                    n += str_fmt_into(dst, -1, "%s%s%s", GREEN, verb, RESET);
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
            else if (term->buf[i] == '\"')
            {
                if (i == 0 || term->buf[i - 1] != '\\')
                {
                    // string
                    for (j = i + 1; j < l; j++)
                    {
                        if (term->buf[j] == '\"' && term->buf[j - 1] != '\\')
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

    printf("%.*s", (i32_t)out->len, as_string(out));

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

nil_t autocp_idx_reset(autocp_idx_t *idx)
{
    idx->entry = 0;
    idx->index = 0;
    idx->sbidx = 0;
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
    i32_t i, p, squote, dquote;
    paren_t parens[TERM_BUF_SIZE];

    p = 0;
    squote = -1;
    dquote = -1;

    // Find the last open parenthesis
    for (i = 0; i < term->buf_pos; i++)
    {
        switch (term->buf[i])
        {
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
            if (i == 0 || term->buf[i - 1] != '\\')
            {
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

    switch (term->autocp_idx.entry)
    {
    case 0:
        word = env_get_internal_keyword_name(tbuf + start, n, &term->autocp_idx.index, B8_FALSE);
        if (word != NULL)
            goto redraw;
        term->autocp_idx.index = 0;
        term->autocp_idx.sbidx = 0;
        term->autocp_idx.entry++;
        // fallthrough
    case 1:
        word = env_get_internal_function_name(tbuf + start, n, &term->autocp_idx.index, B8_FALSE);
        if (word != NULL)
            goto redraw;
        term->autocp_idx.index = 0;
        term->autocp_idx.sbidx = 0;
        term->autocp_idx.entry++;
        // fallthrough
    case 2:
        word = env_get_global_name(tbuf + start, n, &term->autocp_idx.index, &term->autocp_idx.sbidx);
        if (word != NULL)
            goto redraw;
        term->autocp_idx.index = 0;
        term->autocp_idx.sbidx = 0;
        term->autocp_idx.entry++;
        // fallthrough
    default:
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

b8_t term_autocomplete_path(term_p term, u64_t start)
{
    u64_t i, l, n, m, end, len, path_len, prefix_len;
    obj_p files;
    str_p last_slash, file;
    c8_t path[MAX_PATH_LEN], prefix[MAX_PATH_LEN], *hbuf, *tbuf;

    len = term->hist->curr_len;
    hbuf = term->hist->curr;
    tbuf = term->buf;

    // Find end of the path
    for (end = start; end < len; end++)
    {
        if (is_whitespace(hbuf[end]) || hbuf[end] == KEYCODE_DQUOTE)
            break;
    }

    n = end - start;
    if (n == 0 || n > MAX_PATH_LEN - 1)
        return B8_FALSE;

    last_slash = str_rchr(hbuf + start, '/', n);
    if (last_slash != NULL)
    {
        path_len = last_slash - hbuf - start + 1;
        memcpy(path, hbuf + start, path_len);
        prefix_len = hbuf + end - last_slash - 1;
        memcpy(prefix, last_slash + 1, prefix_len);
    }
    else
    {
        path_len = 2;
        memcpy(path, "./", path_len);
        prefix_len = n;
        memcpy(prefix, hbuf + start, prefix_len);
    }

    path[path_len] = '\0';
    prefix[prefix_len] = '\0';

    files = fs_read_dir(path);
    l = files->len;

    for (i = term->autocp_idx.index; i < l; i++)
    {
        file = as_string(as_list(files)[i]);
        m = as_list(files)[i]->len;

        if (m > 0 && prefix_len <= m && strncmp(prefix, file, prefix_len) == 0)
        {
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

b8_t term_autocomplete_paren(term_p term)
{
    paren_t open_paren;

    open_paren = term_find_open_paren(term);

    if (open_paren.pos == -1)
    {
        return term_autocomplete_word(term);
    }
    else if (open_paren.type == KEYCODE_DQUOTE)
    {
        if (term_autocomplete_path(term, open_paren.pos + 1))
            return B8_TRUE;
    }
    else if (term_autocomplete_word(term))
    {
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

nil_t term_handle_tab(term_p term)
{
    term_autocomplete_paren(term);
}

obj_p term_handle_return(term_p term)
{
    i64_t exit_code;
    obj_p res = NULL_OBJ;
    b8_t onoff;

    if (term->buf_len == 0)
        return NULL_OBJ;

    term->buf[term->buf_len] = '\0';

    if (is_cmd(term, ":q"))
    {
        exit_code = (term->buf_len > 2) ? i64_from_str(term->buf + 2, term->buf_len - 3) : 0;
        poll_exit(runtime_get()->poll, exit_code);
        return NULL;
    }

    if (is_cmd(term, ":u"))
    {
        onoff = (term->buf_len > 2 && term->buf[3] == '1') ? B8_TRUE : B8_FALSE;
        format_use_unicode(onoff);
        printf("\n%s. Format use unicode: %s.%s", YELLOW, onoff ? "on" : "off", RESET);
        hist_add(term->hist, term->buf, term->buf_len);
        return NULL_OBJ;
    }

    if (is_cmd(term, ":t"))
    {
        onoff = (term->buf_len > 2 && term->buf[3] == '1') ? B8_TRUE : B8_FALSE;
        timeit_activate(onoff);
        printf("\n%s. Timeit is %s.%s", YELLOW, onoff ? "on" : "off", RESET);
        hist_add(term->hist, term->buf, term->buf_len);
        return NULL_OBJ;
    }

    if (is_cmd(term, ":?"))
    {
        printf("\n%s. Commands list:%s\n%s%s%s", YELLOW, RESET, GRAY, COMMANDS_LIST, RESET);
        return NULL_OBJ;
    }

    res = cstring_from_str(term->buf, term->buf_len);
    hist_add(term->hist, term->buf, term->buf_len);

    return res;
}

obj_p term_handle_escape(term_p term)
{
    u64_t l;

    // Up arrow esc
    if (is_esc(term, "\x1b[A"))
    {
        hist_save_current(term->hist, term->buf, term->buf_len);
        l = hist_prev(term->hist, term->buf);
        if (l > 0)
        {
            term->buf_len = l;
            term->buf_pos = l;
            term_redraw(term);
        }
        goto proceed;
    }

    // Down arrow esc
    if (is_esc(term, "\x1b[B"))
    {
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
        goto proceed;
    }

    // Right arrow esc
    if (is_esc(term, "\x1b[C"))
    {
        if (term->buf_pos < term->buf_len)
        {
            term->buf_pos++;
            cursor_move_right(1);
            fflush(stdout);
        }
        goto proceed;
    }

    // Left arrow esc
    if (is_esc(term, "\x1b[D"))
    {
        if (term->buf_pos > 0)
        {
            term->buf_pos--;
            cursor_move_left(1);
            fflush(stdout);
        }
        goto proceed;
    }

    // Home esc
    if (is_esc(term, "\x1b[H"))
    {
        if (term->buf_pos > 0)
        {
            cursor_move_left(term->buf_pos);
            term->buf_pos = 0;
            fflush(stdout);
        }
        goto proceed;
    }

    // End esc
    if (is_esc(term, "\x1b[F"))
    {
        if (term->buf_len > 0)
        {
            cursor_move_right(term->buf_len - term->buf_pos);
            term->buf_pos = term->buf_len;
            fflush(stdout);
        }
        goto proceed;
    }

    // Delete esc
    if (is_esc(term, "\x1b[3~"))
    {
        if (term->buf_pos < term->buf_len)
        {
            memmove(term->buf + term->buf_pos, term->buf + term->buf_pos + 1, term->buf_len - term->buf_pos);
            term->buf_len--;
            term_redraw(term);
        }
        goto proceed;
    }

    return NULL;

proceed:
    term->input_len = 0;
    return NULL;
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

#if defined(OS_WINDOWS)
    mutex_lock(&term->lock);
#endif

    switch (term->input[0])
    {
    case KEYCODE_RETURN:
        res = term_handle_return(term);
        hist_reset_current(term->hist);
        autocp_idx_reset(&term->autocp_idx);
        term->buf_len = 0;
        term->buf_pos = 0;
        term->input_len = 0;
        line_new();
        fflush(stdout);
        break;
    case KEYCODE_BACKSPACE:
    case KEYCODE_DELETE:
        hist_reset_current(term->hist);
        term_handle_backspace(term);
        autocp_idx_reset(&term->autocp_idx);
        term->input_len = 0;
        break;
    case KEYCODE_TAB:
        hist_save_current(term->hist, term->buf, term->buf_len);
        term_handle_tab(term);
        term->input_len = 0;
        break;
    case KEYCODE_CTRL_C:
        poll_exit(runtime_get()->poll, 0);
        term->input_len = 0;
        break;
    case KEYCODE_ESCAPE:
        res = term_handle_escape(term);
        break;
    default:
        term->input_len = 0;
        if (term->buf_len + 1 == TERM_BUF_SIZE)
            return NULL;
        hist_reset_current(term->hist);
        autocp_idx_reset(&term->autocp_idx);
        res = term_handle_symbol(term);
        break;
    }

#if defined(OS_WINDOWS)
    mutex_unlock(&term->lock);
#endif

    return res;
}

nil_t term_init_progress_bar(term_p term, u64_t parts)
{
    term->pb.parts = parts;
    term->pb.completed = 0;
}

nil_t term_update_progress_bar(term_p term, u64_t parts)
{
    i32_t i;

    term->pb.completed += parts;
    if (term->pb.completed > term->pb.parts)
        term->pb.completed = term->pb.parts;

    int percentage = (term->pb.completed * 100) / term->pb.parts;
    int filled_width = (PROGRESS_BAR_WIDTH * term->pb.completed) / term->pb.parts;

    printf("\r│");
    for (i = 0; i < PROGRESS_BAR_WIDTH; i++)
    {
        if (i < filled_width)
            printf("█");
        else
            printf("░");
    }
    printf("│ %d/%d (%d%%)", term->pb.completed, term->pb.parts, percentage);
    fflush(stdout);
}

nil_t term_finalize_progress_bar(term_p term)
{
    unused(term);
    i32_t i;

    // Move cursor to the beginning of the line
    printf("\r");

    // Overwrite the progress bar with spaces
    for (i = 0; i < PROGRESS_BAR_WIDTH + 20; i++)
        printf(" ");

    // Move cursor to the beginning of the line again
    printf("\r");
    fflush(stdout);
}
