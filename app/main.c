/*
 *   Copyright (c) 2023 Anton Kundenko <singaraiona@gmail.com>
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
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../core/rayforce.h"
#include "../core/format.h"
#include "../core/unary.h"
#include "../core/vm.h"
#include "../core/vector.h"
#include "../core/parse.h"
#include "../core/runtime.h"
#include "../core/cc.h"
#include "../core/debuginfo.h"

#define LINE_SIZE 2048

#define RED "\033[1;31m"
#define TOMATO "\033[1;38;5;9m"
#define GREEN "\033[1;32m"
#define YELLOW "\033[1;33m"
#define BLUE "\033[1;34m"
#define MAGENTA "\033[1;35m"
#define CYAN "\033[1;36m"
#define WHITE "\033[1;37m"
#define BLACK "\033[1;30m"
#define GRAY "\033[1;38;5;8m"
#define ORANGE "\033[1;38;5;202m"
#define PURPLE "\033[1;38;5;141m"
#define TEAL "\033[1;38;5;45m"
#define AQUA "\033[1;38;5;37m"
#define SALAD "\033[1;38;5;118m"
#define DARK_CYAN "\033[1;38;5;30m"
#define BOLD "\033[1m"
#define RESET "\033[0m"

#define PROMPT "> "
#define VERSION "0.0.1"
#define LOGO "\n\
  ▒█▀▀█ █▀▀█ █░░█ ▒█▀▀▀ █▀▀█ █▀▀█ █▀▀ █▀▀ | Version: %s\n\
  ▒█▄▄▀ █▄▄█ █▄▄█ ▒█▀▀▀ █░░█ █▄▄▀ █░░ █▀▀ | Documentation: https://singaraiona.github.io/rayforce\n\
  ▒█░▒█ ▀░░▀ ▄▄▄█ ▒█░░░ ▀▀▀▀ ▀░▀▀ ▀▀▀ ▀▀▀ | Official: https://github.com/singaraiona/rayforce\n\n"

null_t usage()
{
    printf("%s%s%s", BOLD, YELLOW, "Usage: rayforce [-f] [file...]\n");
    exit(EXIT_FAILURE);
}

null_t print_logo()
{
    str_t logo = str_fmt(0, LOGO, VERSION);
    printf("%s%s%s", DARK_CYAN, logo, RESET);
    rf_free(logo);
}

null_t print_error(rf_object_t *error, str_t filename, str_t source, u32_t len)
{
    u16_t line_number = 0, i;
    str_t start = source;
    str_t end = NULL;
    str_t error_desc, lf = "";
    span_t span = error->adt->span;

    switch (error->adt->code)
    {
    case ERR_PARSE:
        error_desc = "parse";
        break;
    case ERR_TYPE:
        error_desc = "type";
        break;
    case ERR_INDEX:
        error_desc = "index";
        break;
    case ERR_LENGTH:
        error_desc = "length";
        break;
    default:
        error_desc = "unknown";
    }

    printf("%s** [E%.3d] error%s: %s\n %s-->%s %s:%d:%d\n    %s|%s\n", TOMATO, error->adt->code, RESET,
           error_desc, CYAN, RESET, filename, span.end_line, span.end_column, CYAN, RESET);

    while (1)
    {
        end = strchr(start, '\n');
        if (end == NULL)
        {
            end = source + len;
            lf = "\n";
        }

        u32_t line_len = end - start + 1;

        if (line_number >= span.start_line && line_number <= span.end_line)
        {
            printf("%.3d %s|%s %.*s", line_number, CYAN, RESET, line_len, start);

            // Print the arrow or span for the error
            if (span.start_line == span.end_line)
            {
                printf("%s    %s|%s ", lf, CYAN, RESET);
                for (i = 0; i < span.start_column; i++)
                    printf(" ");

                for (i = span.start_column; i <= span.end_column; i++)
                    printf("%s^%s", TOMATO, RESET);

                printf(" %s%s%s\n", TOMATO, as_string(error), RESET);
            }
            else
            {
                if (line_number == span.start_line)
                {
                    printf("    %s|%s ", CYAN, RESET);
                    for (i = 0; i < span.start_column; i++)
                        printf(" ");

                    printf("%s^%s\n", TOMATO, RESET);
                }
                else if (line_number == span.end_line)
                {
                    for (i = 0; i < span.end_column + 6; i++)
                        printf(" ");

                    printf("%s^ %s%s\n", TOMATO, as_string(error), RESET);
                }
            }
        }

        if (line_number > span.end_line)
            break;

        line_number++;
        start = end + 1;
    }
}

rf_object_t parse_cmdline(i32_t argc, str_t argv[])
{
    i32_t opt, len;
    rf_object_t keys = list(0), vals = list(0);

    for (opt = 1; opt < argc && argv[opt][0] == '-'; opt++)
    {
        switch (argv[opt][1])
        {
        case 'f':
            opt++;

            if (argv[opt] == NULL)
                usage();

            list_push(&keys, symbol("file"));
            len = strlen(argv[opt]) + 1;
            rf_object_t str = string(len);
            strncpy(as_string(&str), argv[opt], len);
            list_push(&vals, str);
            break;
        default:
            usage();
        }
    }

    argv += opt;
    keys = list_flatten(keys);

    return dict(keys, vals);
}

null_t load_file(parser_t *parser, str_t filename)
{
    i32_t fd;
    str_t file, buf;
    struct stat st;
    rf_object_t rf_object;

    fd = open(filename, O_RDONLY); // open the file for reading

    if (fd == -1)
    { // error handling if file does not exist
        printf("Error opening the file.\n");
        return;
    }

    fstat(fd, &st); // get the size of the file

    file = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0); // memory-map the file

    if (file == MAP_FAILED)
    {
        // error handling if memory-mapping fails
        printf("Error mapping the file.\n");
        return;
    }

    rf_object = parse(parser, filename, file);
    buf = rf_object_fmt(&rf_object);

    // printf("%s%s%s\n", TOMATO, buf, RESET);
    if (is_error(&rf_object))
        print_error(&rf_object, filename, file, st.st_size);
    else
        printf("%s\n", buf);

    munmap(file, st.st_size); // unmap the buffer
    close(fd);                // close the file

    rf_object_free(&rf_object);
    rf_free(buf);
}

i32_t main(i32_t argc, str_t argv[])
{
    runtime_init();

    rf_object_t args = parse_cmdline(argc, argv), parsed, executed, compiled;
    i8_t run = 1;
    str_t line = (str_t)rf_malloc(LINE_SIZE), ptr; //, filename = NULL;
    parser_t parser = parser_new();
    vm_t *vm;

    print_logo();

    vm = vm_new();

    // load_file(&parser, "/tmp/test.ray");
    rf_object_free(&args);

    while (run)
    {

        printf("%s%s%s", GREEN, PROMPT, RESET);
        ptr = fgets(line, LINE_SIZE, stdin);
        if ((ptr) == NULL)
            break;

        parsed = parse(&parser, "REPL", line);
        // printf("%s\n", rf_object_fmt(&parsed));

        if (is_error(&parsed))
        {
            print_error(&parsed, "REPL", line, LINE_SIZE);
            continue;
        }

        compiled = cc_compile_function("top-level", &parsed, &parser.debuginfo);
        if (is_error(&compiled))
        {
            print_error(&compiled, "REPL", line, LINE_SIZE);
            rf_object_free(&parsed);
            rf_object_free(&compiled);
            continue;
        }

        // printf("CODE: %s\n", cc_code_fmt(&compiled));
        executed = vm_exec(vm, &compiled);

        if (is_error(&executed))
            print_error(&executed, "REPL", line, LINE_SIZE);
        else
            printf("%s\n", rf_object_fmt(&executed));

        rf_object_free(&parsed);
        rf_object_free(&executed);
        rf_object_free(&compiled);
    }

    rf_free(line);
    vm_free(vm);

    runtime_cleanup();

    return 0;
}
