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

#define _POSIX_C_SOURCE 1

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include "../core/mmap.h"
#include "../core/rayforce.h"
#include "../core/format.h"
#include "../core/unary.h"
#include "../core/vm.h"
#include "../core/parse.h"
#include "../core/runtime.h"
#include "../core/cc.h"
#include "../core/util.h"
#include "../core/debuginfo.h"
#include "../core/heap.h"

#define LINE_SIZE 2048
#define PROMPT "> "
#define VERSION "0.0.1"
#define LOGO "\n\
  ▒█▀▀█ █▀▀█ █░░█ ▒█▀▀▀ █▀▀█ █▀▀█ █▀▀ █▀▀ | Version: %s\n\
  ▒█▄▄▀ █▄▄█ █▄▄█ ▒█▀▀▀ █░░█ █▄▄▀ █░░ █▀▀ | Documentation: https://singaraiona.github.io/rayforce\n\
  ▒█░▒█ ▀░░▀ ▄▄▄█ ▒█░░░ ▀▀▀▀ ▀░▀▀ ▀▀▀ ▀▀▀ | Official: https://github.com/singaraiona/rayforce\n\n"

static volatile bool_t running = true;

nil_t usage()
{
    printf("%s%s%s", BOLD, YELLOW, "Usage: rayforce [-f] [file...]\n");
    exit(EXIT_FAILURE);
}

nil_t print_logo()
{
    str_t logo = str_fmt(0, LOGO, VERSION);
    printf("%s%s%s", DARK_CYAN, logo, RESET);
    heap_free(logo);
}

obj_t parse_cmdline(i32_t argc, str_t argv[])
{
    i32_t opt, len;
    obj_t keys = vector_symbol(0), vals = list(0), str;

    for (opt = 1; opt < argc && argv[opt][0] == '-'; opt++)
    {
        switch (argv[opt][1])
        {
        case 'f':
            opt++;

            if (argv[opt] == NULL)
                usage();

            join_sym(&keys, "file");
            len = strlen(argv[opt]);
            str = string(len);
            strncpy(as_string(str), argv[opt], len);
            join_obj(vals, str);
            break;
        default:
            usage();
        }
    }

    argv += opt;

    return dict(keys, vals);
}

nil_t repl(str_t name, parser_t *parser, str_t buf, i32_t len)
{
    obj_t parsed, compiled, executed;
    str_t formatted = NULL;

    parsed = parse(parser, name, buf);
    formatted = obj_fmt(parsed);
    printf("%s\n", formatted);
    drop(parsed);
    heap_free(formatted);
    return;

    if (is_error(parsed))
    {
        print_error(parsed, name, buf, len);
        drop(parsed);
        return;
    }

    compiled = cc_compile(parsed, &parser->debuginfo);
    if (is_error(compiled))
    {
        print_error(compiled, name, buf, len);
        drop(parsed);
        drop(compiled);
        return;
    }

    // printf("%s\n", vm_code_fmt(&compiled));

    // release rc's of parsed asap
    drop(parsed);

    executed = vm_exec(&runtime_get()->vm, compiled);
    drop(compiled);

    if (is_error(executed))
        print_error(executed, name, buf, len);
    else if (!is_null(executed))
    {
        formatted = obj_fmt(executed);
        if (formatted != NULL)
        {
            printf("%s\n", formatted);
            heap_free(formatted);
        }
    }

    drop(executed);

    return;
}

nil_t int_handler(i32_t sig)
{
    unused(sig);
    running = false;
}

i32_t main(i32_t argc, str_t argv[])
{
#if defined(__linux__) || defined(__APPLE__) && defined(__MACH__)
    struct sigaction sa;
    sa.sa_handler = int_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) == -1)
        perror("Error: cannot handle SIGINT");
#endif

    runtime_init(0);

    obj_t args = parse_cmdline(argc, argv), filename, file;
    str_t line, ptr;
    parser_t parser = parser_new();

#if defined(__linux__) || defined(__APPLE__) && defined(__MACH__)
    print_logo();
#endif

    line = (str_t)mmap_malloc(LINE_SIZE);

    // load file
    // filename = dict_get(args, symfile);
    // if (!is_null(filename))
    // {
    //     file = rf_fread(filename);
    //     if (file->type == TYPE_ERROR)
    //         print_error(file, as_string(filename), NULL, 0);
    //     else
    //         repl(as_string(filename), &parser, as_string(file), file->len);

    //     drop(file);
    //     drop(filename);
    // }
    // --

    while (running)
    {
        printf("%s%s%s", GREEN, PROMPT, RESET);
        ptr = fgets(line, LINE_SIZE, stdin);
        if (!ptr)
            break;

        repl("top-level", &parser, line, LINE_SIZE);
    }

    drop(args);
    parser_free(&parser);
    mmap_free(line, LINE_SIZE);

    runtime_cleanup();

    return 0;
}
