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

#define _POSIX_C_SOURCE 1

#include "../core/rayforce.h"
#include "../core/runtime.h"
#include "../core/format.h"
#include "../core/util.h"
#include "../core/wasm.h"

#define LOGO "\
  RayforceDB: %d.%d %s\n\
  WASM\n\
  Documentation: https://rayforcedb.com/\n\
  Github: https://github.com/singaraiona/rayforce\n"

nil_t print_logo(nil_t)
{
    str_t logo = str_fmt(0, LOGO, RAYFORCE_MAJOR_VERSION, RAYFORCE_MINOR_VERSION, __DATE__);
    str_t fmt = str_fmt(0, "%s%s%s", BOLD, logo, RESET);
    printjs(fmt);
    heap_free(logo);
    heap_free(fmt);
}

EMSCRIPTEN_KEEPALIVE i32_t main(i32_t argc, str_t argv[])
{
    atexit(runtime_cleanup);
    runtime_init(argc, argv);
    print_logo();

    return runtime_run();
}
