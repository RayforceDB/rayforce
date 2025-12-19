/*
 *   Copyright (c) 2024 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

// Parted table tests - create, load, and query parted data

static nil_t parted_cleanup() { system("rm -rf /tmp/rayforce_test_parted"); }

// Helper macro to create a parted table for testing
// 5 partitions (days), 100 rows each
// Columns: OrderId (i64), Price (f64), Size (i64)
#define PARTED_TEST_SETUP                                         \
    "(do "                                                        \
    "  (set dbpath \"/tmp/rayforce_test_parted/\")"               \
    "  (set n 100)"                                               \
    "  (set gen-partition "                                       \
    "    (fn [day]"                                               \
    "      (let p (format \"%/%/a/\" dbpath (+ 2024.01.01 day)))" \
    "      (let t (table [OrderId Price Size] "                   \
    "        (list "                                              \
    "          (+ (* day 1000) (til n))"                          \
    "          (/ (+ (* day 100.0) (til n)) 100.0)"               \
    "          (+ day (% (til n) 10))"                            \
    "        )"                                                   \
    "      ))"                                                    \
    "      (set-splayed p t)"                                     \
    "    )"                                                       \
    "  )"                                                         \
    "  (map gen-partition (til 5))"                               \
    "  (set t (get-parted \"/tmp/rayforce_test_parted/\" 'a))"    \
    ")"

test_result_t test_parted_load() {
    parted_cleanup();
    TEST_ASSERT_EQ(PARTED_TEST_SETUP "(count t)", "500");
    parted_cleanup();
    PASS();
}

test_result_t test_parted_select_where_date() {
    parted_cleanup();
    TEST_ASSERT_EQ(PARTED_TEST_SETUP "(count (select {from: t where: (== Date 2024.01.01)}))", "100");

    TEST_ASSERT_EQ(PARTED_TEST_SETUP "(count (select {from: t where: (in Date [2024.01.01 2024.01.03])}))", "200");
    parted_cleanup();
    PASS();
}

test_result_t test_parted_select_by_date() {
    parted_cleanup();
    TEST_ASSERT_EQ(PARTED_TEST_SETUP "(count (select {from: t by: Date c: (count OrderId)}))", "5");
    parted_cleanup();
    PASS();
}

test_result_t test_parted_select_multiple_aggregates() {
    parted_cleanup();
    TEST_ASSERT_EQ(PARTED_TEST_SETUP
                   "(count (select {from: t s: (sum Size) c: (count OrderId) mn: (min Price) mx: (max Price)}))",
                   "1");

    TEST_ASSERT_EQ(PARTED_TEST_SETUP "(count (select {from: t by: Date s: (sum Size) c: (count OrderId)}))", "5");
    parted_cleanup();
    PASS();
}

test_result_t test_parted_aggregate_by_date() {
    parted_cleanup();
    // Group by Date with sum aggregation
    TEST_ASSERT_EQ(PARTED_TEST_SETUP "(sum (at (select {from: t by: Date c: (count OrderId)}) 'c))", "500");

    // Group by Date with sum of Size
    // Size = day + (til 100) % 10, sum per day = 100*day + 45*10 = 100*day + 450
    // Total = 100*(0+1+2+3+4) + 5*450 = 1000 + 2250 = 3250
    TEST_ASSERT_EQ(PARTED_TEST_SETUP "(sum (at (select {from: t by: Date s: (sum Size)}) 's))", "3250");
    parted_cleanup();
    PASS();
}

test_result_t test_parted_aggregate_where() {
    parted_cleanup();
    // Filter by date then aggregate - returns one row per matching partition
    TEST_ASSERT_EQ(PARTED_TEST_SETUP "(at (select {from: t where: (== Date 2024.01.01) c: (count OrderId)}) 'c)",
                   "[100]");

    // Two partitions matching -> two rows, sum them to get total count
    TEST_ASSERT_EQ(PARTED_TEST_SETUP
                   "(sum (at (select {from: t where: (in Date [2024.01.01 2024.01.02]) c: (count OrderId)}) 'c))",
                   "200");
    parted_cleanup();
    PASS();
}

test_result_t test_parted_aggregate_f64() {
    parted_cleanup();
    // Test f64 aggregation by date - first should be 0.00, 1.00, 2.00, 3.00, 4.00
    TEST_ASSERT_EQ(PARTED_TEST_SETUP "(at (select {from: t by: Date f: (first Price)}) 'f)",
                   "[0.00 1.00 2.00 3.00 4.00]");

    // Test min/max for f64 - same as first since price increases within each partition
    TEST_ASSERT_EQ(PARTED_TEST_SETUP "(at (select {from: t by: Date mn: (min Price)}) 'mn)",
                   "[0.00 1.00 2.00 3.00 4.00]");
    parted_cleanup();
    PASS();
}

test_result_t test_parted_aggregate_i64() {
    parted_cleanup();
    // Test i64 aggregation by date
    TEST_ASSERT_EQ(PARTED_TEST_SETUP "(at (select {from: t by: Date f: (first OrderId)}) 'f)",
                   "[0 1000 2000 3000 4000]");

    TEST_ASSERT_EQ(PARTED_TEST_SETUP "(at (select {from: t by: Date l: (last OrderId)}) 'l)",
                   "[99 1099 2099 3099 4099]");
    parted_cleanup();
    PASS();
}

test_result_t test_parted_aggregate_minmax() {
    parted_cleanup();
    // Test min/max on i64 Size column
    // Size = day + (til n) % 10, so for day 0: 0-9, day 1: 1-10, etc.
    TEST_ASSERT_EQ(PARTED_TEST_SETUP "(at (select {from: t by: Date mn: (min Size)}) 'mn)", "[0 1 2 3 4]");

    TEST_ASSERT_EQ(PARTED_TEST_SETUP "(at (select {from: t by: Date mx: (max Size)}) 'mx)", "[9 10 11 12 13]");
    parted_cleanup();
    PASS();
}

// Extended setup with time (i32) column for temporal type tests
// Columns: OrderId (i64), Price (f64), Size (i64), Time (time/i32)
#define PARTED_TEST_SETUP_TIME                                    \
    "(do "                                                        \
    "  (set dbpath \"/tmp/rayforce_test_parted/\")"               \
    "  (set n 100)"                                               \
    "  (set gen-partition "                                       \
    "    (fn [day]"                                               \
    "      (let p (format \"%/%/a/\" dbpath (+ 2024.01.01 day)))" \
    "      (let t (table [OrderId Price Size Time] "              \
    "        (list "                                              \
    "          (+ (* day 1000) (til n))"                          \
    "          (/ (+ (* day 100.0) (til n)) 100.0)"               \
    "          (+ day (% (til n) 10))"                            \
    "          (+ 09:30:00.000 (* 1000 (+ (* day 100) (til n))))" \
    "        )"                                                   \
    "      ))"                                                    \
    "      (set-splayed p t)"                                     \
    "    )"                                                       \
    "  )"                                                         \
    "  (map gen-partition (til 5))"                               \
    "  (set t (get-parted \"/tmp/rayforce_test_parted/\" 'a))"    \
    ")"

test_result_t test_parted_aggregate_time() {
    parted_cleanup();
    // Test time (i32) aggregation by Date
    // Time = 09:30:00.000 + 1000*(day*100 + til n) ms
    // Day 0: first = 09:30:00.000, last = 09:31:39.000
    // Results are returned as integers (milliseconds since midnight)
    // 09:30:00 = 34200000ms, 09:31:40 = 34300000ms, etc.
    TEST_ASSERT_EQ(PARTED_TEST_SETUP_TIME "(at (select {from: t by: Date f: (first Time)}) 'f)",
                   "[34200000 34300000 34400000 34500000 34600000]");

    TEST_ASSERT_EQ(PARTED_TEST_SETUP_TIME "(at (select {from: t by: Date l: (last Time)}) 'l)",
                   "[34299000 34399000 34499000 34599000 34699000]");

    // Min should be same as first (time increases within partition)
    TEST_ASSERT_EQ(PARTED_TEST_SETUP_TIME "(at (select {from: t by: Date mn: (min Time)}) 'mn)",
                   "[34200000 34300000 34400000 34500000 34600000]");

    // Max should be same as last
    TEST_ASSERT_EQ(PARTED_TEST_SETUP_TIME "(at (select {from: t by: Date mx: (max Time)}) 'mx)",
                   "[34299000 34399000 34499000 34599000 34699000]");
    parted_cleanup();
    PASS();
}

test_result_t test_parted_aggregate_time_where() {
    parted_cleanup();
    // Test time aggregation with filter
    // Filter to single partition and aggregate
    // 09:30:00.000 = 34200000ms, 09:31:39.000 = 34299000ms
    TEST_ASSERT_EQ(PARTED_TEST_SETUP_TIME "(at (select {from: t where: (== Date 2024.01.01) f: (first Time)}) 'f)",
                   "[34200000]");

    TEST_ASSERT_EQ(PARTED_TEST_SETUP_TIME "(at (select {from: t where: (== Date 2024.01.01) l: (last Time)}) 'l)",
                   "[34299000]");

    // Filter to multiple partitions
    TEST_ASSERT_EQ(
        PARTED_TEST_SETUP_TIME
        "(count (at (select {from: t where: (in Date [2024.01.01 2024.01.02]) by: Date mn: (min Time)}) 'mn))",
        "2");
    parted_cleanup();
    PASS();
}

test_result_t test_parted_aggregate_time_sum() {
    parted_cleanup();
    // Test sum on time column (by date groups)
    // This tests the i32 sum path in PARTED_MAP
    TEST_ASSERT_EQ(PARTED_TEST_SETUP_TIME "(count (at (select {from: t by: Date s: (sum Time)}) 's))", "5");
    parted_cleanup();
    PASS();
}

// Extended setup with i16 (Qty) column for i16 type tests
// Columns: OrderId (i64), Price (f64), Size (i64), Qty (i16)
#define PARTED_TEST_SETUP_I16                                     \
    "(do "                                                        \
    "  (set dbpath \"/tmp/rayforce_test_parted/\")"               \
    "  (set n 100)"                                               \
    "  (set gen-partition "                                       \
    "    (fn [day]"                                               \
    "      (let p (format \"%/%/a/\" dbpath (+ 2024.01.01 day)))" \
    "      (let t (table [OrderId Price Size Qty] "               \
    "        (list "                                              \
    "          (+ (* day 1000) (til n))"                          \
    "          (/ (+ (* day 100.0) (til n)) 100.0)"               \
    "          (+ day (% (til n) 10))"                            \
    "          (as 'I16 (+ day (% (til n) 5)))"                   \
    "        )"                                                   \
    "      ))"                                                    \
    "      (set-splayed p t)"                                     \
    "    )"                                                       \
    "  )"                                                         \
    "  (map gen-partition (til 5))"                               \
    "  (set t (get-parted \"/tmp/rayforce_test_parted/\" 'a))"    \
    ")"

test_result_t test_parted_aggregate_i16() {
    parted_cleanup();
    // Test i16 aggregation - Qty = day + (til n) % 5
    // First values per day: day + 0 = 0, 1, 2, 3, 4
    TEST_ASSERT_EQ(PARTED_TEST_SETUP_I16 "(at (select {from: t by: Date f: (first Qty)}) 'f)", "[0 1 2 3 4]");

    // Last values per day: day + 99 % 5 = day + 4 = 4, 5, 6, 7, 8
    TEST_ASSERT_EQ(PARTED_TEST_SETUP_I16 "(at (select {from: t by: Date l: (last Qty)}) 'l)", "[4 5 6 7 8]");

    // Min per day: day + 0 = 0, 1, 2, 3, 4
    TEST_ASSERT_EQ(PARTED_TEST_SETUP_I16 "(at (select {from: t by: Date mn: (min Qty)}) 'mn)", "[0 1 2 3 4]");

    // Max per day: day + 4 = 4, 5, 6, 7, 8
    TEST_ASSERT_EQ(PARTED_TEST_SETUP_I16 "(at (select {from: t by: Date mx: (max Qty)}) 'mx)", "[4 5 6 7 8]");
    parted_cleanup();
    PASS();
}

test_result_t test_parted_aggregate_i16_sum() {
    parted_cleanup();
    // Test sum on i16 column (by date groups)
    // Qty = day + (til 100) % 5, sum per day = 100*day + (0+1+2+3+4)*20 = 100*day + 200
    // Day 0: 200, Day 1: 300, Day 2: 400, Day 3: 500, Day 4: 600
    // Check individual sums first
    TEST_ASSERT_EQ(PARTED_TEST_SETUP_I16 "(at (select {from: t by: Date s: (sum Qty)}) 's)", "[200 300 400 500 600]");
    parted_cleanup();
    PASS();
}
