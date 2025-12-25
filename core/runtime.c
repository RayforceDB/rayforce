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
#include "util.h"
#include "io.h"
#include "string.h"
#include "ipc.h"
#include "dynlib.h"
#include "heap.h"

// Global runtime reference
runtime_p __RUNTIME = NULL;

nil_t usage(nil_t) {
    printf("%s%s%s", BOLD, YELLOW, "Usage: rayforce [-f file] [-p port] [-t timeit] [-c cores] [-r repl] [file]\n");
    exit(EXIT_FAILURE);
}

obj_p parse_cmdline(i32_t argc, str_p argv[]) {
    i32_t opt;
    obj_p keys = SYMBOL(0), vals = LIST(0), usr_keys = SYMBOL(0), usr_vals = LIST(0), str, sym;
    b8_t file_handled = B8_FALSE, user_defined = B8_FALSE;
    str_p flag;

    for (opt = 1; opt < argc; opt++) {
        if (argv[opt][0] == '-') {
            flag = argv[opt] + 1;  // Skip '-'

            if (!user_defined && (strcmp(flag, "f") == 0 || strcmp(flag, "file") == 0)) {
                if (++opt >= argc)
                    usage();
                push_sym(&keys, "file");
                str = string_from_str(argv[opt], strlen(argv[opt]));
                push_obj(&vals, str);
                file_handled = B8_TRUE;
            } else if (!user_defined && (strcmp(flag, "p") == 0 || strcmp(flag, "port") == 0)) {
                if (++opt >= argc)
                    usage();
                push_sym(&keys, "port");
                str = string_from_str(argv[opt], strlen(argv[opt]));
                push_obj(&vals, str);
            } else if (!user_defined && (strcmp(flag, "c") == 0 || strcmp(flag, "cores") == 0)) {
                if (++opt >= argc)
                    usage();
                push_sym(&keys, "cores");
                str = string_from_str(argv[opt], strlen(argv[opt]));
                push_obj(&vals, str);
            } else if (!user_defined && (strcmp(flag, "t") == 0 || strcmp(flag, "timeit") == 0)) {
                if (++opt >= argc)
                    usage();
                push_sym(&keys, "timeit");
                str = string_from_str(argv[opt], strlen(argv[opt]));
                push_obj(&vals, str);
            } else if (!user_defined && (strcmp(flag, "r") == 0 || strcmp(flag, "repl") == 0)) {
                if (++opt >= argc)
                    usage();
                push_sym(&keys, "repl");
                str = string_from_str(argv[opt], strlen(argv[opt]));
                push_obj(&vals, str);
            } else if (!user_defined && (strcmp(flag, "-") == 0)) {
                user_defined = B8_TRUE;
            } else {
                if (!user_defined)
                    usage();

                if (++opt >= argc)
                    usage();

                sym = symbol(flag, strlen(flag));
                push_obj(&usr_keys, sym);
                str = string_from_str(argv[opt], strlen(argv[opt]));
                push_obj(&usr_vals, str);
            }
        } else {
            // Handle non-option arguments (files)
            if (!file_handled) {
                push_sym(&keys, "file");
                str = string_from_str(argv[opt], strlen(argv[opt]));
                push_obj(&vals, str);
                file_handled = B8_TRUE;
            } else {
                usage();
            }
        }
    }

    if (usr_keys->len == 0) {
        drop_obj(usr_keys);
        drop_obj(usr_vals);
    } else {
        push_sym(&keys, "uargs");
        push_obj(&vals, dict(usr_keys, usr_vals));
    }

    return dict(keys, vals);
}

runtime_p runtime_create(i32_t argc, str_p argv[]) {
    i64_t i, n;
    obj_p arg, fmt, res;
    symbols_p symbols;
    sys_info_t si;

    // Parse -c/--cores argument early (before pool creation)
    // We need to know thread count before creating pool
    n = 0;
    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-c", 2) == 0 && argv[i][2] == '\0' && i + 1 < argc) {
            i64_from_str(argv[i + 1], strlen(argv[i + 1]), &n);
            break;
        } else if (strncmp(argv[i], "--cores=", 8) == 0) {
            i64_from_str(argv[i] + 8, strlen(argv[i]) - 8, &n);
            break;
        } else if (strcmp(argv[i], "--cores") == 0 && i + 1 < argc) {
            i64_from_str(argv[i + 1], strlen(argv[i + 1]), &n);
            break;
        }
    }

    // Get system info with user-specified or default thread count
    si = sys_info(n);
    n = si.threads > 0 ? si.threads : 1;

    // Pool is always created; executor[0] is main thread with its VM/heap
    pool_p pool = pool_create(n);

    symbols = symbols_create();

    __RUNTIME = (runtime_p)heap_mmap(sizeof(struct runtime_t));
    __RUNTIME->symbols = symbols;
    __RUNTIME->env = env_create();
    __RUNTIME->fdmaps = dict(I64(0), LIST(0));
    __RUNTIME->args = NULL_OBJ;
    __RUNTIME->pool = pool;
    __RUNTIME->dynlibs = I64(0);
    __RUNTIME->sys_info = si;

    if (argc) {
        __RUNTIME->args = parse_cmdline(argc, argv);

        __RUNTIME->poll = poll_create();
        if (__RUNTIME->poll == NULL) {
            printf("Failed to create poll\n");
            return NULL;
        }

        // timeit
        arg = runtime_get_arg("timeit");
        if (!is_null(arg)) {
            i64_from_str(AS_C8(arg), arg->len, &n);
            drop_obj(arg);
            timeit_activate(n);
        }

        // load file
        arg = runtime_get_arg("file");
        if (!is_null(arg)) {
            res = ray_load(arg);
            drop_obj(arg);
            if (IS_ERR(res)) {
                fmt = obj_fmt(res, B8_TRUE);
                printf("%.*s\n", (i32_t)fmt->len, AS_C8(fmt));
                drop_obj(fmt);
            }
            drop_obj(res);
        }

    } else {
        // Library/embedded mode (argc == 0)
        // Still create poll for async operations (connections, I/O plugins, etc.)
        // REPL creation is controlled by RAYFORCE_NO_REPL flag
        __RUNTIME->sys_info = sys_info(1);
        if (__RUNTIME->sys_info.threads > 1)
            __RUNTIME->pool = pool_create(__RUNTIME->sys_info.threads - 1);

        __RUNTIME->poll = poll_create();
        if (__RUNTIME->poll == NULL) {
            printf("Failed to create poll\n");
            return NULL;
        }
    }

    return __RUNTIME;
}

i32_t runtime_run(nil_t) {
    i64_t port;
    obj_p arg;

    if (!__RUNTIME->poll)
        return 0;

    arg = runtime_get_arg("port");
    if (!is_null(arg)) {
        i64_from_str(AS_C8(arg), arg->len, &port);
        drop_obj(arg);
        if (ipc_listen(__RUNTIME->poll, port) == -1) {
            printf("Failed to listen on port %lld\n", port);
            return 1;
        }
    }

    return poll_run(__RUNTIME->poll);
}

nil_t runtime_destroy(nil_t) {
    i64_t i, l;
    dynlib_p dl;

    drop_obj(__RUNTIME->args);
    if (__RUNTIME->poll)
        poll_destroy(__RUNTIME->poll);
    symbols_destroy(__RUNTIME->symbols);
    heap_unmap(__RUNTIME->symbols, sizeof(struct symbols_t));
    env_destroy(&__RUNTIME->env);
    drop_obj(__RUNTIME->fdmaps);
    // destroy dynamic libraries
    l = __RUNTIME->dynlibs->len;
    for (i = 0; i < l; i++) {
        dl = (dynlib_p)AS_I64(__RUNTIME->dynlibs)[i];
        dynlib_close(dl);
    }
    drop_obj(__RUNTIME->dynlibs);
    // Pool always exists and contains main VM as executor[0]
    // Save runtime pointer before destroying pool (which destroys heap)
    runtime_p rt = __RUNTIME;
    pool_destroy(__RUNTIME->pool);
    // Use mmap_free directly since heap is destroyed by pool_destroy
    mmap_free(rt, sizeof(struct runtime_t));
    __RUNTIME = NULL;
}

obj_p runtime_get_arg(lit_p key) {
    i64_t i;

    i = find_sym(AS_LIST(__RUNTIME->args)[0], key);
    if (i != NULL_I64)
        return at_idx(AS_LIST(__RUNTIME->args)[1], i);

    return NULL_OBJ;
}

nil_t runtime_fdmap_push(runtime_p runtime, obj_p assoc, obj_p fdmap) {
    obj_p id, r;

    id = i64((i64_t)assoc);
    r = set_obj(&runtime->fdmaps, id, fdmap);
    drop_obj(id);

    if (IS_ERR(r)) {
        DEBUG_OBJ(r);
        return;
    }
}

obj_p runtime_fdmap_pop(runtime_p runtime, obj_p assoc) {
    obj_p id, fdmap;

    id = i64((i64_t)assoc);
    fdmap = remove_obj(&runtime->fdmaps, id);
    drop_obj(id);

    return fdmap;
}

obj_p runtime_fdmap_get(runtime_p runtime, obj_p assoc) {
    obj_p id, fdmap;

    id = i64((i64_t)assoc);
    fdmap = at_obj(runtime->fdmaps, id);
    drop_obj(id);

    return fdmap;
}

runtime_p runtime_get_ext(nil_t) { return __RUNTIME; }