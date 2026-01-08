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

#include "../core/rayforce.h"
#include "../core/runtime.h"
#include "../core/format.h"
#include "../core/sys.h"
#include "../core/string.h"
#include "../core/io.h"
#include "repl.h"
#if defined(OS_WINDOWS)
#include <io.h>
#define isatty _isatty
#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#else
#include <unistd.h>
#endif

nil_t print_logo(sys_info_t *info) {
    printf(
        "%s"
        "  RayforceDB: %d.%d %s\n"
        "  %s %d(MB) %d core(s)\n"
        "  Using %d cores(s)\n"
        "  Started from: %s\n"
        "  Documentation: https://rayforcedb.com/\n"
        "  Github: https://github.com/singaraiona/rayforce\n"
        "%s",
        BOLD, info->major_version, info->minor_version, info->build_date, info->cpu, info->mem, info->cores,
        info->threads, info->cwd, RESET);
}

i32_t main(i32_t argc, str_p argv[]) {
    i32_t code = -1;
    sys_info_t *info;
    runtime_p runtime;
    obj_p interactive_arg, file_arg, res, fmt;
    b8_t interactive = B8_FALSE;
    b8_t has_file = B8_FALSE;
    b8_t file_error = B8_FALSE;

    runtime = runtime_create(argc, argv);
    if (runtime == NULL)
        return -1;

    // Check if interactive mode is explicitly requested (-i/--interactive)
    interactive_arg = runtime_get_arg("interactive");
    interactive = !is_null(interactive_arg);
    drop_obj(interactive_arg);

    // Check if a file was provided
    file_arg = runtime_get_arg("file");
    has_file = !is_null(file_arg);

    // Load startup script if specified
    if (has_file) {
        res = ray_load(file_arg);
        drop_obj(file_arg);
        if (IS_ERR(res)) {
            fmt = obj_fmt(res, B8_TRUE);
            printf("%.*s\n", (i32_t)fmt->len, AS_C8(fmt));
            drop_obj(fmt);
            file_error = B8_TRUE;
        }
        drop_obj(res);

        // Oneshot mode: file without -i flag = execute and exit (like Python)
        if (!interactive) {
            code = file_error ? 1 : 0;
            runtime_destroy();
            return code;
        }
    }

    // Interactive mode: no file, or file with -i flag
    // Only show logo and REPL if stdin is a TTY (not piped input)
    if (isatty(STDIN_FILENO)) {
        info = &runtime_get()->sys_info;
        print_logo(info);
    }

    // Create REPL for interactive mode (handles both TTY and piped input)
    if (runtime->poll)
        repl_create(runtime->poll);

    code = runtime_run();
    runtime_destroy();

    return code;
}
