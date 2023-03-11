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
#include "../core/storm.h"
#include "../core/format.h"
#include "../core/monad.h"
#include "../core/alloc.h"
#include "../core/vm.h"
#include "parse.h"
#include <time.h>

#include <stdlib.h>

#define LINE_SIZE 2048

int main()
{
    storm_alloc_init();

    i8_t run = 1;
    str_t line = (str_t)storm_malloc(LINE_SIZE), ptr;
    memset(line, 0, LINE_SIZE);
    value_t value;
    vm_t vm;
    u8_t *code;

    vm = vm_create();

    while (run)
    {

        printf(">");
        ptr = fgets(line, LINE_SIZE, stdin);
        UNUSED(ptr);

        value = parse("REPL", line);

        str_t buf = value_fmt(&value);
        if (buf == NULL)
            continue;
        printf("%s\n", buf);

        code = compile(value);
        vm_exec(vm, code);

        value_free(&value);
    }

    storm_free(line);
    vm_free(vm);

    storm_alloc_deinit();

    return 0;
}
