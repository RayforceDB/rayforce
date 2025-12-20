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

#include <stdarg.h>
#include "util.h"
#include "log.h"

#if defined(DEBUG)

#if defined(OS_WINDOWS)

// #include <dbghelp.h>
// #include <stdio.h>
// #include <stdlib.h>

// #pragma comment(lib, "dbghelp.lib")

nil_t dump_stack(nil_t) {
    // // Initialize DbgHelp
    // if (!SymInitialize(GetCurrentProcess(), NULL, TRUE))
    // {
    //     fprintf(stderr, "SymInitialize failed: %lu\n", GetLastError());
    //     return;
    // }

    // // Capture stack backtrace
    // void *stack[100];
    // USHORT frames = CaptureStackBackTrace(0, 100, stack, NULL);

    // // Allocate memory for symbol information
    // SYMBOL_INFO *symbol = (SYMBOL_INFO *)calloc(sizeof(SYMBOL_INFO) + 256 * sizeof(char), 1);
    // symbol->MaxNameLen = 255;
    // symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

    // // Print stack trace
    // for (USHORT i = 0; i < frames; i++)
    // {
    //     DWORD64 address = (DWORD64)(stack[i]);
    //     if (SymFromAddr(GetCurrentProcess(), address, 0, symbol))
    //     {
    //         printf("%i: %s - 0x%0X\n", frames - i - 1, symbol->Name, symbol->Address);
    //     }
    //     else
    //     {
    //         printf("%i: No symbol found for 0x%0X\n", frames - i - 1, address);
    //     }
    // }

    // // Cleanup
    // free(symbol);
    // SymCleanup(GetCurrentProcess());
}

#elif defined(OS_LINUX)

#include <execinfo.h>

nil_t dump_stack(nil_t) {
    raw_p array[10];  // Array to store the backtrace addresses
    i64_t i, size;
    str_p *strings;

    size = backtrace(array, 10);               // Capture the backtrace
    strings = backtrace_symbols(array, size);  // Translate addresses to an array of strings

    if (strings != NULL) {
        LOG_ERROR("Stack trace:");
        for (i = 0; i < size; i++)
            LOG_ERROR("%s", strings[i]);
        free(strings);  // Free the memory allocated by backtrace_symbols
    }
}

#else

nil_t dump_stack(nil_t) {}

#endif

#endif

u32_t next_power_of_two_u32(u32_t n) {
    if (n == 0)
        return 1;
    // If n is already a power of 2
    if ((n & (n - 1)) == 0)
        return n;

    return 1 << (32 - __builtin_clzl(n));
}

i64_t next_power_of_two_u64(i64_t n) {
    if (n == 0)
        return 1;
    // If n is already a power of 2
    if ((n & (n - 1)) == 0)
        return n;

    return 1ull << (64 - __builtin_clzll(n));
}

b8_t is_valid(obj_p obj) {
    // clang-format off
    return (obj->type >= -TYPE_C8               && obj->type <= TYPE_C8)
           || obj->type == TYPE_TABLE           || obj->type == TYPE_DICT   
           || obj->type == TYPE_LAMBDA          || obj->type == TYPE_UNARY 
           || obj->type == TYPE_BINARY          || obj->type == TYPE_VARY   
           || obj->type == TYPE_ENUM            || obj->type == TYPE_MAPLIST       
           || obj->type == TYPE_MAPFILTER       || obj->type == TYPE_MAPGROUP
           || obj->type == TYPE_MAPFD           || (obj->type >= TYPE_PARTEDB8 && obj->type <= TYPE_PARTEDGUID)
           || obj->type == TYPE_LIST            
           || obj->type == TYPE_PARTEDLIST      || obj->type == TYPE_PARTEDTIMESTAMP 
           || obj->type == TYPE_MAPCOMMON       || obj->type == TYPE_PARTEDENUM      
           || obj->type == TYPE_ERR
           || obj->type == TYPE_TOKEN           || obj->type == TYPE_NULL;
    // clang-format on
}
