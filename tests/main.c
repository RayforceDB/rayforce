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
#include "../core/str.h"
#include "../core/util.h"
#include "../core/parse.h"
#include "../core/runtime.h"
#include "../core/cmp.h"
#include "../core/pool.h"
#include "../core/sys.h"
#include "../core/eval.h"

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
    sys_info_t si;
#ifdef STOP_ON_FAIL
    runtime_create(1, NULL);
#else
    runtime_create(0, NULL);
#endif
    // Initialize thread pool for pmap tests
    si = sys_info(0);
    if (si.threads > 1)
        __RUNTIME->pool = pool_create(si.threads - 1);
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
            obj_p fmt = str_fmt(-1, "Input error: %s\n -- at: %s:%d", AS_C8(lns), __FILE__, __LINE__);                 \
            drop_obj(le);                                                                                              \
            drop_obj(lns);                                                                                             \
            FAIL(AS_C8(fmt));                                                                                          \
        } else {                                                                                                       \
            obj_p re = eval_str(rhs);                                                                                  \
            obj_p rns = obj_fmt(re, B8_TRUE);                                                                          \
            obj_p fmt = str_fmt(-1, "Expected %s, got %s\n -- at: %s:%d", AS_C8(rns), AS_C8(lns), __FILE__, __LINE__); \
            b8_t pass = str_cmp(AS_C8(lns), lns->len, AS_C8(rns), rns->len) == 0;                                      \
            drop_obj(re);                                                                                              \
            drop_obj(le);                                                                                              \
            drop_obj(lns);                                                                                             \
            drop_obj(rns);                                                                                             \
            if (!pass)                                                                                                 \
                FAIL(AS_C8(fmt));                                                                                      \
            drop_obj(fmt);                                                                                             \
        }                                                                                                              \
    }

#define TEST_ASSERT_ER(lhs, rhs)                                                                                \
    {                                                                                                           \
        obj_p le = eval_str(lhs);                                                                               \
        obj_p lns = obj_fmt(le, B8_TRUE);                                                                       \
        if (!IS_ERR(le)) {                                                                                      \
            obj_p fmt = str_fmt(-1, "Expected error: %s\n -- at: %s:%d", AS_C8(lns), __FILE__, __LINE__);       \
            drop_obj(lns);                                                                                      \
            drop_obj(le);                                                                                       \
            FAIL(AS_C8(fmt));                                                                                   \
        } else {                                                                                                \
            lit_p err_text = AS_C8(lns);                                                                        \
            if (err_text == NULL || strstr(err_text, rhs) == NULL) {                                            \
                obj_p fmt =                                                                                     \
                    str_fmt(-1, "Expect \"%s\", in: \"%s\"\n -- at: %s:%d", rhs, err_text, __FILE__, __LINE__); \
                drop_obj(le);                                                                                   \
                drop_obj(lns);                                                                                  \
                FAIL(AS_C8(fmt));                                                                               \
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
#include "ext.c"
#include "pivot.c"
#include "math_ops.c"
#include "cmp_logic.c"
#include "temporal_tests.c"
#include "casting.c"
#include "string_ops.c"
#include "vector_ops.c"
#include "aggr_tests.c"
#include "join_tests.c"
#include "table_query.c"
#include "advanced.c"
#include "heap_advanced.c"

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
    {"test_lang_read_csv", test_lang_read_csv},
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
    {"test_external", test_external},
    // Pivot tests
    {"test_pivot_basic_sum", test_pivot_basic_sum},
    {"test_pivot_count", test_pivot_count},
    {"test_pivot_avg", test_pivot_avg},
    {"test_pivot_min", test_pivot_min},
    {"test_pivot_max", test_pivot_max},
    {"test_pivot_first", test_pivot_first},
    {"test_pivot_last", test_pivot_last},
    {"test_pivot_med", test_pivot_med},
    {"test_pivot_multi_index", test_pivot_multi_index},
    {"test_pivot_index_values", test_pivot_index_values},
    {"test_pivot_multiple_columns", test_pivot_multiple_columns},
    {"test_pivot_float_values", test_pivot_float_values},
    {"test_pivot_errors", test_pivot_errors},
    {"test_pivot_large", test_pivot_large},
    {"test_pivot_symbol_columns", test_pivot_symbol_columns},
    // String operation tests
    {"test_string_concat_edge", test_string_concat_edge},
    {"test_string_count", test_string_count},
    {"test_string_indexing", test_string_indexing},
    {"test_string_take", test_string_take},
    {"test_string_first_last", test_string_first_last},
    {"test_string_like", test_string_like},
    {"test_string_split", test_string_split},
    {"test_string_comparison", test_string_comparison},
    {"test_string_reverse", test_string_reverse},
    {"test_string_find", test_string_find},
    {"test_string_distinct", test_string_distinct},
    {"test_string_filter", test_string_filter},
    {"test_string_type_cast", test_string_type_cast},
    {"test_string_list_ops", test_string_list_ops},
    {"test_string_where_in", test_string_where_in},
    {"test_string_combined", test_string_combined},
    {"test_symbols_intern_large", test_symbols_intern_large},
    // Vector operation tests
    {"test_vec_til", test_vec_til},
    {"test_vec_take", test_vec_take},
    {"test_vec_at", test_vec_at},
    {"test_vec_where", test_vec_where},
    {"test_vec_distinct", test_vec_distinct},
    {"test_vec_group", test_vec_group},
    {"test_vec_in", test_vec_in},
    {"test_vec_except", test_vec_except},
    {"test_vec_sect", test_vec_sect},
    {"test_vec_union", test_vec_union},
    {"test_vec_raze", test_vec_raze},
    {"test_vec_enlist", test_vec_enlist},
    {"test_vec_reverse", test_vec_reverse},
    {"test_vec_count", test_vec_count},
    {"test_vec_first_last", test_vec_first_last},
    {"test_vec_find", test_vec_find},
    {"test_vec_filter", test_vec_filter},
    {"test_vec_remove", test_vec_remove},
    {"test_vec_within", test_vec_within},
    {"test_vec_combined", test_vec_combined},
    // Math & arithmetic edge case tests
    {"test_math_i16_arithmetic", test_math_i16_arithmetic},
    {"test_math_i32_arithmetic", test_math_i32_arithmetic},
    {"test_math_type_promotion", test_math_type_promotion},
    {"test_math_null_propagation", test_math_null_propagation},
    {"test_math_division_edges", test_math_division_edges},
    {"test_math_modulo_edges", test_math_modulo_edges},
    {"test_math_abs_neg", test_math_abs_neg},
    {"test_math_sqrt_floor_ceil", test_math_sqrt_floor_ceil},
    {"test_math_log_exp", test_math_log_exp},
    {"test_math_vector_ops", test_math_vector_ops},
    {"test_math_large_numbers", test_math_large_numbers},
    {"test_math_unary_aggregations", test_math_unary_aggregations},
    {"test_math_chained_ops", test_math_chained_ops},
    {"test_math_u8_arithmetic", test_math_u8_arithmetic},
    {"test_math_type_errors", test_math_type_errors},
    {"test_math_f64_special", test_math_f64_special},
    // Comparison & logic tests
    {"test_cmp_i16", test_cmp_i16},
    {"test_cmp_i32", test_cmp_i32},
    {"test_cmp_i64", test_cmp_i64},
    {"test_cmp_f64", test_cmp_f64},
    {"test_cmp_mixed_types", test_cmp_mixed_types},
    {"test_cmp_null_comprehensive", test_cmp_null_comprehensive},
    {"test_logic_and", test_logic_and},
    {"test_logic_or", test_logic_or},
    {"test_logic_not", test_logic_not},
    {"test_cmp_where", test_cmp_where},
    {"test_cmp_nested_boolean", test_cmp_nested_boolean},
    {"test_cmp_symbol", test_cmp_symbol},
    {"test_cmp_string", test_cmp_string},
    {"test_cmp_temporal", test_cmp_temporal},
    {"test_cmp_bool_numeric", test_cmp_bool_numeric},
    {"test_cmp_edge_vectors", test_cmp_edge_vectors},
    // Temporal tests
    {"test_temporal_date_literals", test_temporal_date_literals},
    {"test_temporal_time_literals", test_temporal_time_literals},
    {"test_temporal_timestamp_literals", test_temporal_timestamp_literals},
    {"test_temporal_date_arithmetic", test_temporal_date_arithmetic},
    {"test_temporal_time_arithmetic", test_temporal_time_arithmetic},
    {"test_temporal_timestamp_arithmetic", test_temporal_timestamp_arithmetic},
    {"test_temporal_null_propagation", test_temporal_null_propagation},
    {"test_temporal_sorting", test_temporal_sorting},
    {"test_temporal_filtering", test_temporal_filtering},
    {"test_temporal_conversions", test_temporal_conversions},
    {"test_temporal_edge_cases", test_temporal_edge_cases},
    {"test_temporal_in_tables", test_temporal_in_tables},
    {"test_temporal_string_parsing", test_temporal_string_parsing},
    {"test_temporal_cross_type", test_temporal_cross_type},
    // Type casting tests
    {"test_cast_string_to_number", test_cast_string_to_number},
    {"test_cast_number_to_string", test_cast_number_to_string},
    {"test_cast_integer_widening", test_cast_integer_widening},
    {"test_cast_integer_narrowing", test_cast_integer_narrowing},
    {"test_cast_float_truncation", test_cast_float_truncation},
    {"test_cast_boolean", test_cast_boolean},
    {"test_cast_vectors", test_cast_vectors},
    {"test_cast_nulls", test_cast_nulls},
    {"test_cast_type_introspection", test_cast_type_introspection},
    {"test_cast_symbol_string", test_cast_symbol_string},
    {"test_cast_identity_roundtrip", test_cast_identity_roundtrip},
    {"test_cast_list_to_vector", test_cast_list_to_vector},
    {"test_cast_guid", test_cast_guid},
    {"test_cast_temporal_edges", test_cast_temporal_edges},
    // Aggregation tests
    {"test_aggr_sum_types", test_aggr_sum_types},
    {"test_aggr_avg_types", test_aggr_avg_types},
    {"test_aggr_minmax_types", test_aggr_minmax_types},
    {"test_aggr_count_types", test_aggr_count_types},
    {"test_aggr_first_last_types", test_aggr_first_last_types},
    {"test_aggr_median", test_aggr_median},
    {"test_aggr_deviation", test_aggr_deviation},
    {"test_aggr_global_table", test_aggr_global_table},
    {"test_aggr_groupby_single", test_aggr_groupby_single},
    {"test_aggr_groupby_multi", test_aggr_groupby_multi},
    {"test_aggr_where", test_aggr_where},
    {"test_aggr_expression_over", test_aggr_expression_over},
    {"test_aggr_mixed", test_aggr_mixed},
    {"test_aggr_f64_columns", test_aggr_f64_columns},
    {"test_aggr_i16_columns", test_aggr_i16_columns},
    {"test_aggr_i32_columns", test_aggr_i32_columns},
    {"test_aggr_large_vector", test_aggr_large_vector},
    {"test_aggr_temporal", test_aggr_temporal},
    // Join tests
    {"test_join_inner_basic", test_join_inner_basic},
    {"test_join_left_basic", test_join_left_basic},
    {"test_join_single_key_types", test_join_single_key_types},
    {"test_join_multi_key", test_join_multi_key},
    {"test_join_empty_tables", test_join_empty_tables},
    {"test_join_self", test_join_self},
    {"test_join_duplicate_keys", test_join_duplicate_keys},
    {"test_join_asof_basic", test_join_asof_basic},
    {"test_join_asof_types", test_join_asof_types},
    {"test_join_window", test_join_window},
    {"test_join_then_query", test_join_then_query},
    {"test_join_errors", test_join_errors},
    {"test_join_column_order", test_join_column_order},
    {"test_join_larger_tables", test_join_larger_tables},
    {"test_join_parallel", test_join_parallel},
    // Table & query tests
    {"test_table_creation", test_table_creation},
    {"test_table_metadata", test_table_metadata},
    {"test_select_basic", test_select_basic},
    {"test_select_where", test_select_where},
    {"test_select_where_types", test_select_where_types},
    {"test_select_aggregation", test_select_aggregation},
    {"test_select_groupby_single", test_select_groupby_single},
    {"test_select_groupby_multi", test_select_groupby_multi},
    {"test_select_groupby_xbar", test_select_groupby_xbar},
    {"test_select_where_by", test_select_where_by},
    {"test_select_computed_columns", test_select_computed_columns},
    {"test_select_empty_tables", test_select_empty_tables},
    {"test_select_large_tables", test_select_large_tables},
    {"test_select_string_groupby", test_select_string_groupby},
    {"test_select_parallel_filter", test_select_parallel_filter},
    {"test_update_basic", test_update_basic},
    {"test_update_where", test_update_where},
    {"test_update_by", test_update_by},
    {"test_update_inplace", test_update_inplace},
    {"test_update_types", test_update_types},
    {"test_insert_basic", test_insert_basic},
    {"test_insert_inplace", test_insert_inplace},
    {"test_insert_types", test_insert_types},
    {"test_upsert_basic", test_upsert_basic},
    {"test_upsert_multi", test_upsert_multi},
    {"test_upsert_inplace", test_upsert_inplace},
    {"test_alter_vectors", test_alter_vectors},
    {"test_alter_tables", test_alter_tables},
    {"test_table_concat", test_table_concat},
    {"test_table_nulls", test_table_nulls},
    {"test_query_complex", test_query_complex},
    {"test_table_all_types", test_table_all_types},
    {"test_table_dict_conversion", test_table_dict_conversion},
    {"test_table_edge_cases", test_table_edge_cases},
    {"test_query_chains", test_query_chains},
    {"test_select_take", test_select_take},
    {"test_select_sorting", test_select_sorting},
    {"test_select_dev_med", test_select_dev_med},
    {"test_select_xbar_dates", test_select_xbar_dates},
    {"test_update_where_by", test_update_where_by},
    {"test_set_operations", test_set_operations},
    {"test_select_where_expr", test_select_where_expr},
    {"test_insert_edge_cases", test_insert_edge_cases},
    {"test_upsert_edge_cases", test_upsert_edge_cases},
    {"test_alter_edge_cases", test_alter_edge_cases},
    {"test_select_distinct", test_select_distinct},
    {"test_table_operations_mixed", test_table_operations_mixed},
    // Advanced: dict, lambda, iteration, error tests
    {"test_dict_advanced", test_dict_advanced},
    {"test_lambda_advanced", test_lambda_advanced},
    {"test_iteration_advanced", test_iteration_advanced},
    {"test_do_let_advanced", test_do_let_advanced},
    {"test_conditionals_advanced", test_conditionals_advanced},
    {"test_error_handling", test_error_handling},
    {"test_type_errors", test_type_errors},
    {"test_domain_edge_cases", test_domain_edge_cases},
    {"test_lambda_with_tables", test_lambda_with_tables},
    {"test_scan_over", test_scan_over},
    {"test_map_edge_cases", test_map_edge_cases},
    {"test_each_operations", test_each_operations},
    // Heap advanced tests
    {"test_heap_slab_sizes", test_heap_slab_sizes},
    {"test_heap_boundary_sizes", test_heap_boundary_sizes},
    {"test_heap_rapid_cycles", test_heap_rapid_cycles},
    {"test_heap_realloc_boundaries", test_heap_realloc_boundaries},
    {"test_heap_zero_size", test_heap_zero_size},
    {"test_heap_alignment", test_heap_alignment},
    {"test_heap_obj_patterns", test_heap_obj_patterns},
    {"test_heap_stress_mixed", test_heap_stress_mixed},
    {"test_heap_alternating", test_heap_alternating},
    {"test_heap_obj_lifecycle", test_heap_obj_lifecycle},
    {"test_heap_large_alloc", test_heap_large_alloc},
    {"test_heap_realloc_data", test_heap_realloc_data},
    {"test_heap_power_of_two", test_heap_power_of_two},
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
        printf("%sPassed%s %d/%d tests (%d skipped, %d failed).\n", YELLOW, RESET, num_passed, num_tests, num_skipped,
               num_failed);
    else if (num_skipped > 0)
        printf("%sAll tests passed!%s (%d skipped)\n", GREEN, RESET, num_skipped);
    else
        printf("%sAll tests passed!%s\n", GREEN, RESET);

    return num_failed > 0;
}
