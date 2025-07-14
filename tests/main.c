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
#include <string.h>
#include <time.h>
#include "../core/rayforce.h"
#include "../core/format.h"
#include "../core/unary.h"
#include "../core/heap.h"
#include "../core/eval.h"
#include "../core/hash.h"
#include "../core/symbols.h"
#include "../core/string.h"
#include "../core/util.h"
#include "../core/parse.h"
#include "../core/runtime.h"
#include "../core/cmp.h"
#include "../core/eval.h"

typedef enum test_status_t { TEST_PASS = 0, TEST_FAIL } test_status_t;

typedef struct test_result_t {
    test_status_t status;
    str_p msg;
} test_result_t;

// Test function prototype
typedef test_result_t (*test_func)();

// Define a struct to hold a test function and its name
typedef struct test_entry_t {
    str_p name;
    test_func func;
} test_entry_t;

// Setup and Teardown functions
nil_t setup() {
#ifdef STOP_ON_FAIL
    runtime_create(1, NULL);
#else
    runtime_create(0, NULL);
#endif
    // heap_create(0);
}

nil_t teardown() {
    runtime_destroy();
    // heap_destroy();
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

nil_t on_pass(f64_t ms) { printf("%sPassed%s at: %.*f ms\n", GREEN, RESET, 4, ms); }

nil_t on_fail(str_p msg) {
    printf("%sFailed.%s \n          \\ %s\n", RED, RESET, msg);
#ifdef STOP_ON_FAIL
    runtime_run();
#endif
}

// Macro to encapsulate the pattern
#define RUN_TEST(name, func, pass)                                 \
    test_result_t res;                                             \
    clock_t timer;                                                 \
    f64_t ms;                                                      \
    do {                                                           \
        setup();                                                   \
        printf("%s  Running %s%s ... ", CYAN, RESET, name);        \
        fflush(stdout);                                            \
        timer = clock();                                           \
        res = func();                                              \
        ms = (((f64_t)(clock() - timer)) / CLOCKS_PER_SEC) * 1000; \
        if (res.status == TEST_PASS) {                             \
            (*pass)++;                                             \
            on_pass(ms);                                           \
        } else {                                                   \
            on_fail(res.msg);                                      \
        }                                                          \
        teardown();                                                \
    } while (0)

#define TEST_ASSERT_EQ(lhs, rhs)                                                                                       \
    {                                                                                                                  \
        obj_p le = eval_str(lhs);                                                                                      \
        obj_p lns = obj_fmt(le, B8_TRUE);                                                                              \
        if (IS_ERR(le)) {                                                                                              \
            obj_p fmt = str_fmt(-1, "Input error: %s\n -- at: %s:%d", AS_C8(lns), __FILE__, __LINE__);                 \
            TEST_ASSERT(0, AS_C8(lns));                                                                                \
            drop_obj(lns);                                                                                             \
            drop_obj(fmt);                                                                                             \
        } else {                                                                                                       \
            obj_p re = eval_str(rhs);                                                                                  \
            obj_p rns = obj_fmt(re, B8_TRUE);                                                                          \
            obj_p fmt = str_fmt(-1, "Expected %s, got %s\n -- at: %s:%d", AS_C8(rns), AS_C8(lns), __FILE__, __LINE__); \
            TEST_ASSERT(str_cmp(AS_C8(lns), lns->len, AS_C8(rns), rns->len) == 0, AS_C8(fmt));                         \
            drop_obj(fmt);                                                                                             \
            drop_obj(re);                                                                                              \
            drop_obj(le);                                                                                              \
            drop_obj(lns);                                                                                             \
            drop_obj(rns);                                                                                             \
        }                                                                                                              \
    }

#define TEST_ASSERT_ER(lhs, rhs)                                                                                \
    {                                                                                                           \
        obj_p le = eval_str(lhs);                                                                               \
        obj_p lns = obj_fmt(le, B8_TRUE);                                                                       \
        if (!IS_ERR(le)) {                                                                                      \
            obj_p fmt = str_fmt(-1, "Expected error: %s\n -- at: %s:%d", AS_C8(lns), __FILE__, __LINE__);       \
            TEST_ASSERT(0, AS_C8(lns));                                                                         \
            drop_obj(lns);                                                                                      \
            drop_obj(fmt);                                                                                      \
            drop_obj(le);                                                                                       \
        } else {                                                                                                \
            lit_p err_text = AS_C8(lns);                                                                        \
            if (err_text == NULL || strstr(err_text, rhs) == NULL) {                                            \
                obj_p fmt =                                                                                     \
                    str_fmt(-1, "Expect \"%s\", in: \"%s\"\n -- at: %s:%d", rhs, err_text, __FILE__, __LINE__); \
                TEST_ASSERT(0, AS_C8(fmt));                                                                     \
                drop_obj(fmt);                                                                                  \
            }                                                                                                   \
            drop_obj(le);                                                                                       \
            drop_obj(lns);                                                                                      \
        }                                                                                                       \
    }

// Include tests files
#include "heap.c"
#include "hash.c"
#include "string.c"
#include "env.c"
#include "sort.c"
#include "lang.c"
#include "serde.c"

// Add tests here
test_entry_t tests[] = {
    {"test_allocate_and_free", test_allocate_and_free},
    {"test_multiple_allocations", test_multiple_allocations},
    {"test_allocation_after_free", test_allocation_after_free},
    {"test_out_of_memory", test_out_of_memory},
    {"test_varying_sizes", test_varying_sizes},
    {"test_multiple_allocs_and_frees", test_multiple_allocs_and_frees},
    {"test_multiple_allocs_and_frees_rand", test_multiple_allocs_and_frees_rand},
    {"test_realloc_larger_and_smaller", test_realloc_larger_and_smaller},
    {"test_realloc", test_realloc},
    {"test_realloc_same_size", test_realloc_same_size},
    {"test_alloc_dealloc_stress", test_alloc_dealloc_stress},
    {"test_allocate_and_free_obj", test_allocate_and_free_obj},
    {"test_hash", test_hash},
    {"test_env", test_env},
    {"test_sort_asc", test_sort_asc},
    {"test_sort_desc", test_sort_desc},
    {"test_asc_desc", test_asc_desc},
    {"test_sort_xasc", test_sort_xasc},
    {"test_sort_xdesc", test_sort_xdesc},
    {"test_str_match", test_str_match},
    {"test_lang_basic", test_lang_basic},
    {"test_lang_math", test_lang_math},
    {"test_lang_take", test_lang_take},
    {"test_lang_query", test_lang_query},
    {"test_lang_update", test_lang_update},
    {"test_lang_serde", test_lang_serde},
    {"test_lang_literals", test_lang_literals},
    {"test_lang_cmp", test_lang_cmp},
    {"test_lang_split", test_lang_split},
    {"test_serde_different_sizes", test_serde_different_sizes},
    {"test_lang_distinct", test_lang_distinct},
    {"test_lang_concat", test_lang_concat},
    {"test_lang_filter", test_lang_filter},
    {"test_lang_in", test_lang_in},
    {"test_lang_or", test_lang_or},
    {"test_lang_and", test_lang_and},
    //{"test_sort_lists_by_type", test_sort_lists_by_type},
    //{"test_sort_lists_nested", test_sort_lists_nested},
    {"test_timsort_numbers", test_timsort_numbers},
};
// ---

i32_t main() {
    i32_t i, num_tests, num_passed = 0;

    num_tests = sizeof(tests) / sizeof(test_entry_t);
    printf("%sTotal tests: %s%d\n", YELLOW, RESET, num_tests);

    for (i = 0; i < num_tests; ++i) {
        RUN_TEST(tests[i].name, tests[i].func, &num_passed);
    }

    if (num_passed != num_tests)
        printf("%sPassed%s %d/%d tests.\n", YELLOW, RESET, num_passed, num_tests);
    else
        printf("%sAll tests passed!%s\n", GREEN, RESET);

    return num_passed != num_tests;
}
