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

#include "runtime.h"
#include "mmap.h"
#include "heap.h"
#include "util.h"
#include "poll.h"
#include "unary.h"
#include "io.h"

// Global runtime reference
runtime_p __RUNTIME = NULL;

nil_t usage(nil_t)
{
    printf("%s%s%s", BOLD, YELLOW, "Usage: rayforce [-f file...] [-p port] [file]\n");
    exit(EXIT_FAILURE);
}

obj_p parse_cmdline(i32_t argc, str_p argv[])
{
    i32_t opt;
    obj_p keys = vector_symbol(0), vals = vn_list(0), str;
    b8_t file_handled = B8_FALSE; // flag to indicate if the file has been handled

    for (opt = 1; opt < argc; opt++)
    {
        if (argv[opt][0] == '-')
        {
            switch (argv[opt][1])
            {
            case 'f':
                opt++;

                if (argv[opt] == NULL)
                    usage();

                push_sym(&keys, "file");
                str = string_from_str(argv[opt], strlen(argv[opt]));
                push_obj(&vals, str);
                file_handled = B8_TRUE;
                break;

            case 'p':
                opt++;

                if (argv[opt] == NULL)
                    usage();

                push_sym(&keys, "port");
                str = string_from_str(argv[opt], strlen(argv[opt]));
                push_obj(&vals, str);
                break;

            default:
                usage();
            }
        }
        else
        {
            // Handle non-option argument (file path)
            if (!file_handled)
            {
                push_sym(&keys, "file");
                str = string_from_str(argv[opt], strlen(argv[opt]));
                push_obj(&vals, str);
                file_handled = B8_TRUE;
            }
            else
            {
                // If a file path has already been handled, treat as an error
                usage();
            }
        }
    }

    return dict(keys, vals);
}

i32_t runtime_init(i32_t argc, str_p argv[])
{
    i64_t i;
    obj_p file, res;
    str_p fmt;

    heap_init();

    __RUNTIME = (runtime_p)mmap_malloc(sizeof(struct runtime_t));
    __RUNTIME->symbols = symbols_new();
    __RUNTIME->env = create_env();
    __RUNTIME->addr = (sock_addr_t){0};
    __RUNTIME->slaves = 0;
    __RUNTIME->fds = dict(vector_i64(0), vector_i64(0));

    interpreter_new();

    if (argc)
    {
        __RUNTIME->args = parse_cmdline(argc, argv);
        i = find_sym(as_list(__RUNTIME->args)[0], "port");
        if (i < (i64_t)as_list(__RUNTIME->args)[0]->len)
            __RUNTIME->addr.port = atoi(as_string(as_list(as_list(__RUNTIME->args)[1])[i]));

        __RUNTIME->poll = poll_init(__RUNTIME->addr.port);

        // load file
        file = runtime_get_arg("file");
        if (!is_null(file))
        {
            res = ray_load(file);
            drop_obj(file);

            if (res)
            {
                fmt = obj_fmt(res);
                printf("%s\n", fmt);
                heap_free(fmt);
                drop_obj(res);
            }
        }
    }
    else
        __RUNTIME->poll = NULL;

    return 0;
}

i32_t runtime_run(nil_t)
{
    return poll_run(__RUNTIME->poll);
}

nil_t runtime_cleanup(nil_t)
{
    drop_obj(__RUNTIME->args);
    if (__RUNTIME->poll)
        poll_cleanup(__RUNTIME->poll);
    symbols_free(__RUNTIME->symbols);
    mmap_free(__RUNTIME->symbols, sizeof(symbols_t));
    free_env(&__RUNTIME->env);
    drop_obj(__RUNTIME->fds);
    interpreter_free();
    mmap_free(__RUNTIME, sizeof(struct runtime_t));
    heap_cleanup();
    __RUNTIME = NULL;
}

obj_p runtime_get_arg(str_p key)
{
    i64_t i = find_sym(as_list(__RUNTIME->args)[0], key);
    if (i < (i64_t)as_list(__RUNTIME->args)[0]->len)
        return at_idx(as_list(__RUNTIME->args)[1], i);
    return NULL_OBJ;
}
