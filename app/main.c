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

#include "../core/rayforce.h"
#include "../core/runtime.h"
#include "../core/format.h"
#include "../core/util.h"
#include "../core/sys.h"

nil_t print_logo(sys_info_t *info)
{
  printf("%s"
         "  RayforceDB: %d.%d %s\n"
         "  %s %d(MB) %d core(s)\n"
         "  Using %d threads(s)\n"
         "  Documentation: https://rayforcedb.com/\n"
         "  Github: https://github.com/singaraiona/rayforce\n"
         "%s",
         BOLD, info->major_version, info->minor_version,
         info->build_date, info->cpu, info->mem, info->cores, info->threads, RESET);
}

i32_t main(i32_t argc, str_p argv[])
{
  i32_t code = -1;
  sys_info_t *info;

  runtime_init(argc, argv);
  info = &runtime_get()->sys_info;
  print_logo(info);
  code = runtime_run();
  runtime_cleanup();

  return code;
}
