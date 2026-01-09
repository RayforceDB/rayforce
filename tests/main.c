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
#include "../core/pool.h"
#include "../core/sys.h"
#include "../core/eval.h"
#include "../core/error.h"

typedef enum test_status_t { TEST_PASS = 0, TEST_FAIL, TEST_SKIP } test_status_t;

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
    // Pool is now created inside runtime_create with all threads
}

nil_t teardown() {
    runtime_destroy();
}

#define PASS() \
    return (test_result_t) { TEST_PASS, NULL }
#define FAIL(msg) \
    return (test_result_t) { TEST_FAIL, msg }
#define SKIP(msg) \
    return (test_result_t) { TEST_SKIP, msg }

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

nil_t on_skip(str_p msg) { printf("%sSkipped%s (%s)\n", YELLOW, RESET, msg ? msg : "no reason"); }

// Macro to encapsulate the pattern
#define RUN_TEST(name, func, pass, skip)                           \
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
        } else if (res.status == TEST_SKIP) {                      \
            (*skip)++;                                             \
            on_skip(res.msg);                                      \
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
            drop_obj(lns);                                                                                             \
            drop_obj(le);                                                                                              \
            SKIP("error in eval");                                                                                     \
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
#include "parted.c"

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
    {"test_rank_xrank", test_rank_xrank},
    {"test_reverse", test_reverse},
    {"test_str_match", test_str_match},
    {"test_lang_map", test_lang_map},
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
    {"test_lang_raze", test_lang_raze},
    {"test_lang_filter", test_lang_filter},
    {"test_lang_in", test_lang_in},
    {"test_lang_except", test_lang_except},
    {"test_lang_or", test_lang_or},
    {"test_lang_and", test_lang_and},
    {"test_lang_bin", test_lang_bin},
    {"test_lang_timestamp", test_lang_timestamp},
    {"test_lang_aggregations", test_lang_aggregations},
    {"test_lang_joins", test_lang_joins},
    {"test_lang_temporal", test_lang_temporal},
    {"test_lang_iteration", test_lang_iteration},
    {"test_lang_conditionals", test_lang_conditionals},
    {"test_lang_dict", test_lang_dict},
    {"test_lang_list", test_lang_list},
    {"test_lang_alter", test_lang_alter},
    {"test_lang_null", test_lang_null},
    {"test_lang_set_ops", test_lang_set_ops},
    {"test_lang_cast", test_lang_cast},
    {"test_lang_lambda", test_lang_lambda},
    {"test_lang_group", test_lang_group},
    {"test_lang_find", test_lang_find},
    {"test_lang_rand", test_lang_rand},
    {"test_lang_unary_ops", test_lang_unary_ops},
    {"test_lang_string_ops", test_lang_string_ops},
    {"test_lang_do_let", test_lang_do_let},
    {"test_lang_error", test_lang_error},
    {"test_lang_safety", test_lang_safety},
    // Parted table tests
    {"test_parted_load", test_parted_load},
    {"test_parted_select_where_date", test_parted_select_where_date},
    {"test_parted_select_by_date", test_parted_select_by_date},
    {"test_parted_select_multiple_aggregates", test_parted_select_multiple_aggregates},
    {"test_parted_aggregate_by_date", test_parted_aggregate_by_date},
    {"test_parted_aggregate_where", test_parted_aggregate_where},
    {"test_parted_aggregate_f64", test_parted_aggregate_f64},
    {"test_parted_aggregate_i64", test_parted_aggregate_i64},
    {"test_parted_aggregate_minmax", test_parted_aggregate_minmax},
    // Extended parted tests with i32/time type
    {"test_parted_aggregate_time", test_parted_aggregate_time},
    {"test_parted_aggregate_time_where", test_parted_aggregate_time_where},
    {"test_parted_aggregate_time_sum", test_parted_aggregate_time_sum},
    // Extended parted tests with i16 type
    {"test_parted_aggregate_i16", test_parted_aggregate_i16},
    {"test_parted_aggregate_i16_sum", test_parted_aggregate_i16_sum},
    // Global aggregation tests (no by/where)
    {"test_parted_global_count", test_parted_global_count},
    {"test_parted_global_sum", test_parted_global_sum},
    {"test_parted_global_avg", test_parted_global_avg},
    {"test_parted_global_minmax", test_parted_global_minmax},
    {"test_parted_global_first_last", test_parted_global_first_last},
    {"test_parted_global_multiple", test_parted_global_multiple},
    // Timestamp type tests
    {"test_parted_timestamp_aggregate", test_parted_timestamp_aggregate},
    // Complex filter tests
    {"test_parted_filter_range", test_parted_filter_range},
    {"test_parted_filter_not_in", test_parted_filter_not_in},
    {"test_parted_filter_all_match", test_parted_filter_all_match},
    {"test_parted_filter_none_match", test_parted_filter_none_match},
    // Combined where + by tests
    {"test_parted_where_by_combined", test_parted_where_by_combined},
    // Materialization tests
    {"test_parted_materialize_column", test_parted_materialize_column},
    {"test_parted_materialize_filtered", test_parted_materialize_filtered},
    {"test_parted_materialize_sorted", test_parted_materialize_sorted},
    // Average aggregation tests
    {"test_parted_avg_by_date", test_parted_avg_by_date},
    {"test_parted_avg_f64", test_parted_avg_f64},
    // Edge cases
    {"test_parted_single_partition", test_parted_single_partition},
    {"test_parted_first_partition", test_parted_first_partition},
    {"test_parted_last_partition", test_parted_last_partition},
    // Multi-type mixed operations
    {"test_parted_mixed_types", test_parted_mixed_types},
    {"test_parted_all_aggregates", test_parted_all_aggregates},
    // Date column operations
    {"test_parted_date_column", test_parted_date_column},
    // Large/small partition tests
    {"test_parted_many_partitions", test_parted_many_partitions},
    {"test_parted_small_data", test_parted_small_data},
    // Filter on data column tests
    {"test_parted_filter_data_column", test_parted_filter_data_column},
    {"test_parted_filter_data_with_aggr", test_parted_filter_data_with_aggr},
    {"test_parted_filter_data_min", test_parted_filter_data_min},
    {"test_parted_filter_data_sum", test_parted_filter_data_sum},
    // Symbol column tests
    {"test_parted_symbol_load", test_parted_symbol_load},
    {"test_parted_symbol_count_by_date", test_parted_symbol_count_by_date},
    {"test_parted_symbol_first_last", test_parted_symbol_first_last},
    {"test_parted_symbol_filter", test_parted_symbol_filter},
    // GUID column tests
    {"test_parted_guid_load", test_parted_guid_load},
    {"test_parted_guid_count_by_date", test_parted_guid_count_by_date},
    {"test_parted_guid_with_other_aggr", test_parted_guid_with_other_aggr},
    // U8 column tests
    {"test_parted_u8_load", test_parted_u8_load},
    {"test_parted_u8_count", test_parted_u8_count},
    // Splayed table tests
    {"test_splayed_load", test_splayed_load},
    {"test_splayed_select_all", test_splayed_select_all},
    {"test_splayed_select_where", test_splayed_select_where},
    {"test_splayed_aggregate", test_splayed_aggregate},
    {"test_splayed_aggregate_group", test_splayed_aggregate_group},
    {"test_splayed_minmax", test_splayed_minmax},
    {"test_splayed_first_last", test_splayed_first_last},
    {"test_splayed_avg", test_splayed_avg},
    // Splayed with symbol tests
    {"test_splayed_symbol_load", test_splayed_symbol_load},
    {"test_splayed_symbol_access", test_splayed_symbol_access},
    {"test_splayed_symbol_aggregate", test_splayed_symbol_aggregate},
    // Data column filter + aggregation tests
    {"test_parted_filter_price_max", test_parted_filter_price_max},
    {"test_parted_filter_price_min", test_parted_filter_price_min},
    {"test_parted_filter_price_sum", test_parted_filter_price_sum},
    {"test_parted_filter_price_count", test_parted_filter_price_count},
    {"test_parted_filter_price_avg", test_parted_filter_price_avg},
    {"test_parted_filter_size_sum", test_parted_filter_size_sum},
    {"test_parted_filter_orderid_first", test_parted_filter_orderid_first},
    {"test_parted_filter_orderid_last", test_parted_filter_orderid_last},
    // Combined filter tests
    {"test_parted_filter_date_and_price", test_parted_filter_date_and_price},
    {"test_parted_filter_date_or_price", test_parted_filter_date_or_price},
    // Multi-type tests
    {"test_parted_multi_type_load", test_parted_multi_type_load},
    {"test_parted_multi_type_sum", test_parted_multi_type_sum},
    {"test_parted_multi_type_by_date", test_parted_multi_type_by_date},
    {"test_parted_multi_type_filter_aggr", test_parted_multi_type_filter_aggr},
    // Single partition tests
    {"test_parted_single_day", test_parted_single_day},
    {"test_parted_single_day_filter", test_parted_single_day_filter},
    // Boolean column tests
    {"test_parted_bool_load", test_parted_bool_load},
    {"test_parted_bool_filter", test_parted_bool_filter},
    {"test_parted_bool_count", test_parted_bool_count},
    // Date column tests
    {"test_parted_date_col_load", test_parted_date_col_load},
    {"test_parted_date_col_first_last", test_parted_date_col_first_last},
    {"test_parted_date_col_minmax", test_parted_date_col_minmax},
    {"test_parted_date_col_filter", test_parted_date_col_filter},
    // Float special values tests
    {"test_parted_float_special", test_parted_float_special},
    // Few match tests
    {"test_parted_filter_few_match", test_parted_filter_few_match},
    // Large data tests
    {"test_parted_large_data", test_parted_large_data},
    {"test_parted_large_aggregate", test_parted_large_aggregate},
    {"test_parted_large_filter", test_parted_large_filter},
    // Multi aggregation with filter tests
    {"test_parted_multi_aggr_filter", test_parted_multi_aggr_filter},
    {"test_parted_multi_aggr_filter_count", test_parted_multi_aggr_filter_count},
    {"test_parted_multi_aggr_filter_min", test_parted_multi_aggr_filter_min},
    // Average on i16 tests
    {"test_parted_avg_i16_by_date", test_parted_avg_i16_by_date},
    {"test_parted_avg_i16_global", test_parted_avg_i16_global},
    {"test_parted_avg_i16_filter", test_parted_avg_i16_filter},
    // Average on i32/time tests
    {"test_parted_avg_time_by_date", test_parted_avg_time_by_date},
    {"test_parted_avg_time_global", test_parted_avg_time_global},
    {"test_parted_avg_i32_by_date", test_parted_avg_i32_by_date},
    {"test_parted_avg_i32_global", test_parted_avg_i32_global},
    {"test_parted_avg_i32_filter", test_parted_avg_i32_filter},
    // Complex filter with avg tests
    {"test_parted_avg_complex_filter", test_parted_avg_complex_filter},
    {"test_parted_avg_price_filter", test_parted_avg_price_filter},
    {"test_parted_avg_size_filter", test_parted_avg_size_filter},
    // Average with multiple aggregates tests
    {"test_parted_avg_with_other_aggr", test_parted_avg_with_other_aggr},
    {"test_parted_avg_filter_by_date", test_parted_avg_filter_by_date},
    // Date column avg tests
    {"test_parted_avg_date_col", test_parted_avg_date_col},
    {"test_parted_avg_date_col_by_date", test_parted_avg_date_col_by_date},
    // I16 column with filters tests
    {"test_parted_i16_filter_aggr", test_parted_i16_filter_aggr},
    {"test_parted_i16_global_minmax", test_parted_i16_global_minmax},
    // I32/time filter tests
    {"test_parted_time_filter_aggr", test_parted_time_filter_aggr},
    // Dev (standard deviation) tests
    {"test_parted_dev_i64", test_parted_dev_i64},
    {"test_parted_dev_global", test_parted_dev_global},
    {"test_parted_dev_i16", test_parted_dev_i16},
    {"test_parted_dev_i32", test_parted_dev_i32},
    // Med (median) tests
    {"test_parted_med_i64", test_parted_med_i64},
    {"test_parted_med_global", test_parted_med_global},
    // Count tests for parted types
    {"test_parted_count_i16", test_parted_count_i16},
    {"test_parted_count_i32", test_parted_count_i32},
    {"test_parted_count_time", test_parted_count_time},
    // Parted distinct tests
    {"test_parted_distinct_i64", test_parted_distinct_i64},
};
// ---

i32_t main() {
    i32_t i, num_tests, num_passed = 0, num_skipped = 0;

    num_tests = sizeof(tests) / sizeof(test_entry_t);
    printf("%sTotal tests: %s%d\n", YELLOW, RESET, num_tests);

    for (i = 0; i < num_tests; ++i) {
        RUN_TEST(tests[i].name, tests[i].func, &num_passed, &num_skipped);
    }

    i32_t num_failed = num_tests - num_passed - num_skipped;
    if (num_failed > 0)
        printf("%sPassed%s %d/%d tests (%d skipped, %d failed).\n", YELLOW, RESET, num_passed, num_tests, num_skipped, num_failed);
    else if (num_skipped > 0)
        printf("%sAll tests passed!%s (%d skipped)\n", GREEN, RESET, num_skipped);
    else
        printf("%sAll tests passed!%s\n", GREEN, RESET);

    return num_failed > 0;
}
