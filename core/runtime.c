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
#include "cc.h"
#include "poll.h"
#include "unary.h"
#include "io.h"

// Global runtime reference
__thread runtime_t _RUNTIME = NULL;

nil_t usage()
{
    printf("%s%s%s", BOLD, YELLOW, "Usage: rayforce [-f file...] [-p port]\n");
    exit(EXIT_FAILURE);
}

obj_t parse_cmdline(i32_t argc, str_t argv[])
{
    i32_t opt;
    obj_t keys = vector_symbol(0), vals = list(0), str;

    for (opt = 1; opt < argc && argv[opt][0] == '-'; opt++)
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

    argv += opt;

    return dict(keys, vals);
}

nil_t runtime_init(i32_t argc, str_t argv[])
{
    i64_t i;
    obj_t filename = null(0), file = null(0), res;
    str_t fmt;

    heap_init();
    _RUNTIME = mmap_malloc(sizeof(struct runtime_t));
    _RUNTIME->symbols = symbols_new();
    _RUNTIME->env = create_env();
    _RUNTIME->parser = parser_new();
    _RUNTIME->vm = vm_new(NULL);
    _RUNTIME->args = parse_cmdline(argc, argv);
    _RUNTIME->addr = (sock_addr_t){0};

    i = find_sym(as_list(_RUNTIME->args)[0], "port");
    if (i < (i64_t)as_list(_RUNTIME->args)[0]->len)
        _RUNTIME->addr.port = atoi(as_string(as_list(as_list(_RUNTIME->args)[1])[i]));

    _RUNTIME->slaves = 0;

    // load file
    filename = runtime_get_arg("file");
    if (filename != NULL)
    {
        file = ray_read(filename);
        if (file->type == TYPE_ERROR)
            printf("No such file: '%s'\n", as_string(filename));
        else
        {
            res = eval_str(0, as_string(filename), as_string(file));
            fmt = obj_fmt(res);
            printf("%s\n", fmt);
            heap_free(fmt);
            drop(res);
        }

        drop(file);
    }

    drop(filename);

    _RUNTIME->select = poll_init(_RUNTIME->addr.port);
}

i32_t runtime_run()
{
    return poll_dispatch(_RUNTIME->select);
}

nil_t runtime_cleanup()
{
    drop(_RUNTIME->args);
    poll_cleanup(_RUNTIME->select);
    symbols_free(_RUNTIME->symbols);
    mmap_free(_RUNTIME->symbols, sizeof(symbols_t));
    free_env(&_RUNTIME->env);
    parser_free(&_RUNTIME->parser);
    vm_free(&_RUNTIME->vm);
    mmap_free(_RUNTIME, sizeof(struct runtime_t));
    heap_cleanup();
    _RUNTIME = NULL;
}

runtime_t runtime_get()
{
    return _RUNTIME;
}

obj_t runtime_get_arg(str_t key)
{
    i64_t i = find_sym(as_list(_RUNTIME->args)[0], key);
    if (i < (i64_t)as_list(_RUNTIME->args)[0]->len)
        return at_idx(as_list(_RUNTIME->args)[1], i);
    return null(0);
}