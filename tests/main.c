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
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include "../core/rayforce.h"
#include "../core/format.h"
#include "../core/unary.h"
#include "../core/heap.h"
#include "../core/vm.h"
#include "../core/hash.h"
#include "../core/symbols.h"
#include "../core/string.h"
#include "../core/util.h"
#include "../core/parse.h"
#include "../core/runtime.h"

typedef enum test_status_t
{
    TEST_PASS = 0,
    TEST_FAIL
} test_status_t;

typedef struct test_result_t
{
    test_status_t status;
    str_t msg;
} test_result_t;

// Test function prototype
typedef test_result_t (*test_func)();

// Define a struct to hold a test function and its name
typedef struct test_entry_t
{
    str_t name;
    test_func func;
} test_entry_t;

// Setup and Teardown functions
nil_t setup()
{
    runtime_init(0, NULL);
}

nil_t teardown()
{
    runtime_cleanup();
}

#define PASS() \
    return (test_result_t) { TEST_PASS, NULL }
#define FAIL(msg) \
    return (test_result_t) { TEST_FAIL, msg }

// A tests assertion function
#define TEST_ASSERT(cond, msg) \
    {                          \
        if (!(cond))           \
            FAIL(msg);         \
    }

// Macro to encapsulate the pattern
#define RUN_TEST(name, func, pass)                                          \
    test_result_t res;                                                      \
    clock_t timer;                                                          \
    f64_t ms;                                                               \
    do                                                                      \
    {                                                                       \
        setup();                                                            \
        printf("%s  Running %s%s ... ", CYAN, RESET, name);                 \
        timer = clock();                                                    \
        res = func();                                                       \
        ms = (((f64_t)(clock() - timer)) / CLOCKS_PER_SEC) * 1000;          \
        if (res.status == TEST_PASS)                                        \
        {                                                                   \
            (*pass)++;                                                      \
            printf("%sPassed%s at: %.*f ms\n", GREEN, RESET, 4, ms);        \
        }                                                                   \
        else                                                                \
        {                                                                   \
            printf("%sFailed.%s \n          \\ %s\n", RED, RESET, res.msg); \
        }                                                                   \
        teardown();                                                         \
    } while (0)

// Include tests files
#include "heap.c"
#include "core.c"

// Add tests here
test_entry_t tests[] = {
    {"test_allocate_and_free", test_allocate_and_free},
    {"test_multiple_allocations", test_multiple_allocations},
    {"test_allocation_after_free", test_allocation_after_free},
    {"test_out_of_memory", test_out_of_memory},
    // {"test_large_number_of_allocations", test_large_number_of_allocations},
    {"test_varying_sizes", test_varying_sizes},
    {"test_str_match", test_str_match},
};
// ---

i32_t main()
{
    i32_t i, num_tests, num_passed = 0;

    num_tests = sizeof(tests) / sizeof(test_entry_t);
    printf("%sTotal tests: %s%d\n", YELLOW, RESET, num_tests);

    for (i = 0; i < num_tests; ++i)
    {
        RUN_TEST(tests[i].name, tests[i].func, &num_passed);
    }

    if (num_passed != num_tests)
        printf("%sPassed%s %d/%d tests.\n", YELLOW, RESET, num_passed, num_tests);
    else
        printf("%sAll tests passed!%s\n", GREEN, RESET);

    return num_passed != num_tests;
}
