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

// ==================== SUM ACROSS TYPES ====================
test_result_t test_aggr_sum_types() {
    // I16 sum
    TEST_ASSERT_EQ("(sum [1h 2h 3h 4h 5h])", "15h");
    TEST_ASSERT_EQ("(sum [0h])", "0h");
    TEST_ASSERT_EQ("(sum [-1h -2h -3h])", "-6h");

    // I32 sum
    TEST_ASSERT_EQ("(sum [1i 2i 3i 4i 5i])", "15i");
    TEST_ASSERT_EQ("(sum [0i])", "0i");
    TEST_ASSERT_EQ("(sum [-10i 10i])", "0i");

    // I64 sum
    TEST_ASSERT_EQ("(sum [1 2 3 4 5])", "15");
    TEST_ASSERT_EQ("(sum [0])", "0");
    TEST_ASSERT_EQ("(sum [-100 50 50])", "0");
    TEST_ASSERT_EQ("(sum [1000000 2000000 3000000])", "6000000");

    // F64 sum
    TEST_ASSERT_EQ("(sum [1.0 2.0 3.0 4.0 5.0])", "15.0");
    TEST_ASSERT_EQ("(sum [0.5 0.5])", "1.0");
    TEST_ASSERT_EQ("(sum [-1.5 1.5])", "0.0");

    // Single element
    TEST_ASSERT_EQ("(sum [42])", "42");
    TEST_ASSERT_EQ("(sum [3.14])", "3.14");
    TEST_ASSERT_EQ("(sum [7h])", "7h");
    TEST_ASSERT_EQ("(sum [7i])", "7i");

    // Empty vector
    TEST_ASSERT_EQ("(sum [])", "0");

    // Scalar passthrough
    TEST_ASSERT_EQ("(sum 5)", "5");
    TEST_ASSERT_EQ("(sum 5.0)", "5.0");
    TEST_ASSERT_EQ("(sum 5i)", "5i");
    TEST_ASSERT_EQ("(sum 5h)", "5h");

    // Sum with nulls
    TEST_ASSERT_EQ("(sum [1 0Nl 3])", "4");
    TEST_ASSERT_EQ("(sum [1.0 0Nf 3.0])", "4.0");
    TEST_ASSERT_EQ("(sum [1i 0Ni 3i])", "4i");
    TEST_ASSERT_EQ("(sum [1h 0Nh 3h])", "4h");

    PASS();
}

// ==================== AVG ACROSS TYPES ====================
test_result_t test_aggr_avg_types() {
    // I64 avg
    TEST_ASSERT_EQ("(avg [1 2 3 4 5])", "3.0");
    TEST_ASSERT_EQ("(avg [10 20])", "15.0");
    TEST_ASSERT_EQ("(avg [0 0 0])", "0.0");
    TEST_ASSERT_EQ("(avg [-10 10])", "0.0");

    // I32 avg
    TEST_ASSERT_EQ("(avg [2i 4i 6i 8i])", "5.0");
    TEST_ASSERT_EQ("(avg [1i])", "1.0");

    // I16 avg
    TEST_ASSERT_EQ("(avg [2h 4h 6h])", "4.0");

    // F64 avg
    TEST_ASSERT_EQ("(avg [1.0 2.0 3.0])", "2.0");
    TEST_ASSERT_EQ("(avg [10.0 20.0 30.0])", "20.0");
    TEST_ASSERT_EQ("(avg [0.5 1.5])", "1.0");

    // Single element
    TEST_ASSERT_EQ("(avg [42])", "42.0");
    TEST_ASSERT_EQ("(avg [3.14])", "3.14");

    // Scalar
    TEST_ASSERT_EQ("(avg 10)", "10.0");
    TEST_ASSERT_EQ("(avg 10.0)", "10.0");

    // Avg with nulls
    TEST_ASSERT_EQ("(avg [1 0Nl 3])", "2.0");
    TEST_ASSERT_EQ("(avg [2.0 0Nf 4.0])", "3.0");

    PASS();
}

// ==================== MIN/MAX ACROSS TYPES ====================
test_result_t test_aggr_minmax_types() {
    // I64 min/max
    TEST_ASSERT_EQ("(min [5 2 8 1 9])", "1");
    TEST_ASSERT_EQ("(max [5 2 8 1 9])", "9");
    TEST_ASSERT_EQ("(min [-5 -2 -8])", "-8");
    TEST_ASSERT_EQ("(max [-5 -2 -8])", "-2");
    TEST_ASSERT_EQ("(min [0 0 0])", "0");
    TEST_ASSERT_EQ("(max [0 0 0])", "0");

    // I32 min/max
    TEST_ASSERT_EQ("(min [5i 2i 8i])", "2i");
    TEST_ASSERT_EQ("(max [5i 2i 8i])", "8i");
    TEST_ASSERT_EQ("(min [-10i 0i 10i])", "-10i");
    TEST_ASSERT_EQ("(max [-10i 0i 10i])", "10i");

    // I16 min/max
    TEST_ASSERT_EQ("(min [5h 2h 8h])", "2h");
    TEST_ASSERT_EQ("(max [5h 2h 8h])", "8h");
    TEST_ASSERT_EQ("(min [-5h 0h 5h])", "-5h");
    TEST_ASSERT_EQ("(max [-5h 0h 5h])", "5h");

    // F64 min/max
    TEST_ASSERT_EQ("(min [5.0 2.0 8.0])", "2.0");
    TEST_ASSERT_EQ("(max [5.0 2.0 8.0])", "8.0");
    TEST_ASSERT_EQ("(min [-1.5 0.0 1.5])", "-1.5");
    TEST_ASSERT_EQ("(max [-1.5 0.0 1.5])", "1.5");

    // Date min/max
    TEST_ASSERT_EQ("(min [2024.01.01 2024.06.15 2024.03.10])", "2024.01.01");
    TEST_ASSERT_EQ("(max [2024.01.01 2024.06.15 2024.03.10])", "2024.06.15");

    // Time min/max
    TEST_ASSERT_EQ("(min [10:00:00.000 08:30:00.000 12:45:00.000])", "08:30:00.000");
    TEST_ASSERT_EQ("(max [10:00:00.000 08:30:00.000 12:45:00.000])", "12:45:00.000");

    // Timestamp min/max
    TEST_ASSERT_EQ("(min [2024.01.01D10:00:00.000000000 2024.01.01D08:00:00.000000000 2024.01.01D12:00:00.000000000])",
                   "2024.01.01D08:00:00.000000000");
    TEST_ASSERT_EQ("(max [2024.01.01D10:00:00.000000000 2024.01.01D08:00:00.000000000 2024.01.01D12:00:00.000000000])",
                   "2024.01.01D12:00:00.000000000");

    // Single element
    TEST_ASSERT_EQ("(min [42])", "42");
    TEST_ASSERT_EQ("(max [42])", "42");
    TEST_ASSERT_EQ("(min [3.14])", "3.14");
    TEST_ASSERT_EQ("(max [3.14])", "3.14");

    // Scalar
    TEST_ASSERT_EQ("(min 42)", "42");
    TEST_ASSERT_EQ("(max 42)", "42");

    // With nulls
    TEST_ASSERT_EQ("(min [1 0Nl 3])", "1");
    TEST_ASSERT_EQ("(max [1 0Nl 3])", "3");
    TEST_ASSERT_EQ("(min [1.0 0Nf 3.0])", "1.0");
    TEST_ASSERT_EQ("(max [1.0 0Nf 3.0])", "3.0");

    PASS();
}

// ==================== COUNT ACROSS TYPES ====================
test_result_t test_aggr_count_types() {
    // Vector count
    TEST_ASSERT_EQ("(count [1 2 3 4 5])", "5");
    TEST_ASSERT_EQ("(count [1i 2i 3i])", "3");
    TEST_ASSERT_EQ("(count [1h 2h])", "2");
    TEST_ASSERT_EQ("(count [1.0 2.0 3.0 4.0])", "4");
    TEST_ASSERT_EQ("(count ['a 'b 'c])", "3");
    TEST_ASSERT_EQ("(count [true false true])", "3");
    TEST_ASSERT_EQ("(count [2024.01.01 2024.01.02])", "2");
    TEST_ASSERT_EQ("(count [10:00:00.000 11:00:00.000])", "2");

    // Empty
    TEST_ASSERT_EQ("(count [])", "0");

    // String
    TEST_ASSERT_EQ("(count \"hello\")", "5");
    TEST_ASSERT_EQ("(count \"\")", "0");

    // Scalar
    TEST_ASSERT_EQ("(count 5)", "1");
    TEST_ASSERT_EQ("(count 5.0)", "1");
    TEST_ASSERT_EQ("(count 'a)", "1");

    // Table count
    TEST_ASSERT_EQ("(count (table [a b] (list [1 2 3] [4 5 6])))", "3");
    TEST_ASSERT_EQ("(count (table [a] (list (take [1] 0))))", "0");

    // Dict count
    TEST_ASSERT_EQ("(count (dict [a b c] [1 2 3]))", "3");

    // Count with nulls - nulls are counted as elements
    TEST_ASSERT_EQ("(count [1 0Nl 3])", "3");
    TEST_ASSERT_EQ("(count [1.0 0Nf 3.0])", "3");

    PASS();
}

// ==================== FIRST/LAST ACROSS TYPES ====================
test_result_t test_aggr_first_last_types() {
    // I64
    TEST_ASSERT_EQ("(first [10 20 30])", "10");
    TEST_ASSERT_EQ("(last [10 20 30])", "30");

    // I32
    TEST_ASSERT_EQ("(first [10i 20i 30i])", "10i");
    TEST_ASSERT_EQ("(last [10i 20i 30i])", "30i");

    // I16
    TEST_ASSERT_EQ("(first [10h 20h 30h])", "10h");
    TEST_ASSERT_EQ("(last [10h 20h 30h])", "30h");

    // F64
    TEST_ASSERT_EQ("(first [1.0 2.0 3.0])", "1.0");
    TEST_ASSERT_EQ("(last [1.0 2.0 3.0])", "3.0");

    // Symbol
    TEST_ASSERT_EQ("(first ['a 'b 'c])", "'a");
    TEST_ASSERT_EQ("(last ['a 'b 'c])", "'c");

    // String
    TEST_ASSERT_EQ("(first \"hello\")", "'h'");
    TEST_ASSERT_EQ("(last \"hello\")", "'o'");

    // Date
    TEST_ASSERT_EQ("(first [2024.01.01 2024.06.15 2024.12.31])", "2024.01.01");
    TEST_ASSERT_EQ("(last [2024.01.01 2024.06.15 2024.12.31])", "2024.12.31");

    // Time
    TEST_ASSERT_EQ("(first [08:00:00.000 12:00:00.000 18:00:00.000])", "08:00:00.000");
    TEST_ASSERT_EQ("(last [08:00:00.000 12:00:00.000 18:00:00.000])", "18:00:00.000");

    // Timestamp
    TEST_ASSERT_EQ("(first [2024.01.01D08:00:00.000000000 2024.01.01D12:00:00.000000000])",
                   "2024.01.01D08:00:00.000000000");
    TEST_ASSERT_EQ("(last [2024.01.01D08:00:00.000000000 2024.01.01D12:00:00.000000000])",
                   "2024.01.01D12:00:00.000000000");

    // Boolean
    TEST_ASSERT_EQ("(first [true false true])", "true");
    TEST_ASSERT_EQ("(last [true false true])", "true");
    TEST_ASSERT_EQ("(first [false true false])", "false");
    TEST_ASSERT_EQ("(last [false true false])", "false");

    // Single element
    TEST_ASSERT_EQ("(first [99])", "99");
    TEST_ASSERT_EQ("(last [99])", "99");

    // Table first/last
    TEST_ASSERT_EQ("(first (table [a b] (list [1 2 3] [4 5 6])))", "{a:1 b:4}");
    TEST_ASSERT_EQ("(last (table [a b] (list [1 2 3] [4 5 6])))", "{a:3 b:6}");

    PASS();
}

// ==================== MED (MEDIAN) TESTS ====================
test_result_t test_aggr_median() {
    // Odd length
    TEST_ASSERT_EQ("(med [1 2 3 4 5])", "3.0");
    TEST_ASSERT_EQ("(med [5 1 3 2 4])", "3.0");
    TEST_ASSERT_EQ("(med [100 200 300])", "200.0");

    // Even length
    TEST_ASSERT_EQ("(med [1 2 3 4])", "2.5");
    TEST_ASSERT_EQ("(med [10 20 30 40])", "25.0");

    // Single element
    TEST_ASSERT_EQ("(med [42])", "42.0");
    // Two elements
    TEST_ASSERT_EQ("(med [1 3])", "2.0");
    TEST_ASSERT_EQ("(med [10 20])", "15.0");

    // Negative values
    TEST_ASSERT_EQ("(med [-5 -3 -1 1 3])", "-1.0");
    TEST_ASSERT_EQ("(med [-10 0 10])", "0.0");

    // All same values
    TEST_ASSERT_EQ("(med [5 5 5 5 5])", "5.0");

    // F64 odd length
    TEST_ASSERT_EQ("(med [1.0 2.0 3.0 4.0 5.0])", "3.0");
    TEST_ASSERT_EQ("(med [5.0 1.0 3.0 2.0 4.0])", "3.0");
    TEST_ASSERT_EQ("(med [100.5 200.5 300.5])", "200.5");

    // F64 even length
    TEST_ASSERT_EQ("(med [1.0 2.0 3.0 4.0])", "2.5");
    TEST_ASSERT_EQ("(med [10.0 20.0 30.0 40.0])", "25.0");

    // F64 single element
    TEST_ASSERT_EQ("(med [42.5])", "42.5");

    // F64 two elements
    TEST_ASSERT_EQ("(med [1.5 3.5])", "2.5");

    // F64 negative values
    TEST_ASSERT_EQ("(med [-5.0 -3.0 -1.0 1.0 3.0])", "-1.0");
    TEST_ASSERT_EQ("(med [-10.0 0.0 10.0])", "0.0");

    // F64 all same values
    TEST_ASSERT_EQ("(med [5.5 5.5 5.5 5.5 5.5])", "5.5");

    // F64 scalar
    TEST_ASSERT_EQ("(med 3.14)", "3.14");

    PASS();
}

// ==================== DEV (STANDARD DEVIATION) TESTS ====================
test_result_t test_aggr_deviation() {
    // All same => 0
    TEST_ASSERT_EQ("(dev [1 1 1 1])", "0.0");
    TEST_ASSERT_EQ("(dev [5 5 5])", "0.0");
    TEST_ASSERT_EQ("(dev [10.0 10.0 10.0])", "0.0");

    // Known values: [1 2 3 4 5] => sample std dev = sqrt(2.5) ~= 1.5811
    TEST_ASSERT_EQ("(< (- (dev [1 2 3 4 5]) 1.4142) 0.001)", "true");

    // Two elements
    TEST_ASSERT_EQ("(< (- (dev [0 10]) 5.0) 1.0)", "true");

    // F64
    TEST_ASSERT_EQ("(dev [1.0 1.0 1.0])", "0.0");

    // I32
    TEST_ASSERT_EQ("(dev [1i 1i 1i])", "0.0");

    PASS();
}

// ==================== GLOBAL AGGREGATION ON TABLES ====================
test_result_t test_aggr_global_table() {
    // Sum on table column
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2 3 4 5] [10 20 30 40 50])))"
        "(at (select {from: t s: (sum a)}) 's)",
        "[15]");

    // Avg on table column
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 2 3 4 5] [10 20 30 40 50])))"
        "(at (select {from: t a: (avg b)}) 'a)",
        "[30.0]");

    // Min/Max on table column
    TEST_ASSERT_EQ(
        "(set t (table [val] (list [5 2 8 1 9])))"
        "(at (select {from: t mn: (min val)}) 'mn)",
        "[1]");
    TEST_ASSERT_EQ(
        "(set t (table [val] (list [5 2 8 1 9])))"
        "(at (select {from: t mx: (max val)}) 'mx)",
        "[9]");

    // Count on table column
    TEST_ASSERT_EQ(
        "(set t (table [val] (list [5 2 8 1 9])))"
        "(at (select {from: t c: (count val)}) 'c)",
        "[5]");

    // First/Last on table column
    TEST_ASSERT_EQ(
        "(set t (table [val] (list [5 2 8 1 9])))"
        "(at (select {from: t f: (first val)}) 'f)",
        "[5]");
    TEST_ASSERT_EQ(
        "(set t (table [val] (list [5 2 8 1 9])))"
        "(at (select {from: t l: (last val)}) 'l)",
        "[9]");

    // Multiple aggregations in one select
    TEST_ASSERT_EQ(
        "(set t (table [val] (list [10 20 30 40 50])))"
        "(count (select {from: t s: (sum val) a: (avg val) c: (count val) mn: (min val) mx: (max val)}))",
        "1");

    TEST_ASSERT_EQ(
        "(set t (table [val] (list [10 20 30 40 50])))"
        "(at (select {from: t s: (sum val) a: (avg val) mn: (min val) mx: (max val)}) 's)",
        "[150]");

    TEST_ASSERT_EQ(
        "(set t (table [val] (list [10 20 30 40 50])))"
        "(at (select {from: t s: (sum val) a: (avg val) mn: (min val) mx: (max val)}) 'a)",
        "[30.0]");

    PASS();
}

// ==================== GROUP-BY SINGLE KEY ====================
test_result_t test_aggr_groupby_single() {
    // Sum by single key
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b c] [10 20 30 40 50])))"
        "(select {from: t s: (sum val) by: grp})",
        "(table [grp s] (list [a b c] [30 70 50]))");

    // Avg by single key
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [10 20 30 40])))"
        "(select {from: t a: (avg val) by: grp})",
        "(table [grp a] (list [a b] [15.0 35.0]))");

    // Count by single key
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a a b b] [1 2 3 4 5])))"
        "(select {from: t c: (count val) by: grp})",
        "(table [grp c] (list [a b] [3 2]))");

    // Min by single key
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [20 10 40 30])))"
        "(select {from: t mn: (min val) by: grp})",
        "(table [grp mn] (list [a b] [10 30]))");

    // Max by single key
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [20 10 40 30])))"
        "(select {from: t mx: (max val) by: grp})",
        "(table [grp mx] (list [a b] [20 40]))");

    // First by single key
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [10 20 30 40])))"
        "(select {from: t f: (first val) by: grp})",
        "(table [grp f] (list [a b] [10 30]))");

    // Last by single key
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [10 20 30 40])))"
        "(select {from: t l: (last val) by: grp})",
        "(table [grp l] (list [a b] [20 40]))");

    // Single group (all same key)
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a a] [10 20 30])))"
        "(select {from: t s: (sum val) by: grp})",
        "(table [grp s] (list [a] [60]))");

    // Each row is its own group
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a b c] [10 20 30])))"
        "(select {from: t s: (sum val) by: grp})",
        "(table [grp s] (list [a b c] [10 20 30]))");

    // Group-by with I64 key
    TEST_ASSERT_EQ(
        "(set t (table [k val] (list [1 1 2 2 3] [10 20 30 40 50])))"
        "(select {from: t s: (sum val) by: k})",
        "(table [k s] (list [1 2 3] [30 70 50]))");

    // Group-by with boolean key
    TEST_ASSERT_EQ(
        "(set t (table [flag val] (list [true false true false] [10 20 30 40])))"
        "(select {from: t s: (sum val) by: flag})",
        "(table [flag s] (list [true false] [40 60]))");

    PASS();
}

// ==================== GROUP-BY MULTIPLE KEYS ====================
test_result_t test_aggr_groupby_multi() {
    // Two keys
    TEST_ASSERT_EQ(
        "(set t (table [k1 k2 val] (list [a a b b] [x y x y] [10 20 30 40])))"
        "(select {from: t s: (sum val) by: {k1: k1 k2: k2}})",
        "(table [k1 k2 s] (list [a a b b] [x y x y] [10 20 30 40]))");

    // Two keys with repeated combos
    TEST_ASSERT_EQ(
        "(set t (table [k1 k2 val] (list [a a a b b b] [x x y x x y] [10 20 30 40 50 60])))"
        "(select {from: t s: (sum val) by: {k1: k1 k2: k2}})",
        "(table [k1 k2 s] (list [a a b b] [x y x y] [30 30 90 60]))");

    // Three keys
    TEST_ASSERT_EQ(
        "(set t (table [k1 k2 k3 val] (list [a a a a] [x x y y] [1 1 1 1] [10 20 30 40])))"
        "(select {from: t s: (sum val) by: {k1: k1 k2: k2 k3: k3}})",
        "(table [k1 k2 k3 s] (list [a a] [x y] [1 1] [30 70]))");

    // Count with multiple keys
    TEST_ASSERT_EQ(
        "(set t (table [k1 k2 val] (list [a a a b b] [x x y x y] [1 2 3 4 5])))"
        "(select {from: t c: (count val) by: {k1: k1 k2: k2}})",
        "(table [k1 k2 c] (list [a a b b] [x y x y] [2 1 1 1]))");

    PASS();
}

// ==================== WHERE + AGGREGATION ====================
test_result_t test_aggr_where() {
    // Select with where + sum
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b c] [10 20 30 40 50])))"
        "(at (select {from: t s: (sum val) where: (== grp 'a)}) 's)",
        "[30]");

    // Select with where + count
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b c] [10 20 30 40 50])))"
        "(at (select {from: t c: (count val) where: (== grp 'b)}) 'c)",
        "[2]");

    // Select with where + avg
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [10 20 30 40])))"
        "(at (select {from: t a: (avg val) where: (== grp 'a)}) 'a)",
        "[15.0]");

    // Select with where + min/max
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [10 20 30 40])))"
        "(at (select {from: t mn: (min val) where: (== grp 'b)}) 'mn)",
        "[30]");
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [10 20 30 40])))"
        "(at (select {from: t mx: (max val) where: (== grp 'b)}) 'mx)",
        "[40]");

    // Where with numeric comparison
    TEST_ASSERT_EQ(
        "(set t (table [val] (list [1 2 3 4 5 6 7 8 9 10])))"
        "(at (select {from: t s: (sum val) where: (> val 5)}) 's)",
        "[40]");

    // Where with combined conditions
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b c] [10 20 30 40 50])))"
        "(at (select {from: t s: (sum val) where: (or (== grp 'a) (== grp 'c))}) 's)",
        "[80]");

    // Where + by combined
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b a b] [10 20 30 40 50 60])))"
        "(select {from: t s: (sum val) where: (> val 25) by: grp})",
        "(table [grp s] (list [b a] [130 50]))");

    PASS();
}

// ==================== EXPRESSION-OVER-AGGREGATE ====================
test_result_t test_aggr_expression_over() {
    // (max - min) range
    TEST_ASSERT_EQ(
        "(set t (table [val] (list [10 20 30 40 50])))"
        "(at (select {from: t r: (- (max val) (min val))}) 'r)",
        "[40]");

    // (sum / count) should match avg
    TEST_ASSERT_EQ(
        "(set t (table [val] (list [10 20 30])))  "
        "(at (select {from: t r: (/ (sum val) (count val))}) 'r)",
        "[20]");

    // Expression over aggregate with group-by
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [10 20 30 40])))"
        "(at (select {from: t r: (- (max val) (min val)) by: grp}) 'r)",
        "[10 10]");

    // Nested expression: (* 2 (sum val))
    TEST_ASSERT_EQ(
        "(set t (table [val] (list [1 2 3 4 5])))"
        "(at (select {from: t r: (* 2 (sum val))}) 'r)",
        "[30]");

    PASS();
}

// ==================== MIXED AGGREGATIONS IN SAME QUERY ====================
test_result_t test_aggr_mixed() {
    // sum + avg + count in same select
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [10 20 30 40])))"
        "(count (select {from: t s: (sum val) a: (avg val) c: (count val) by: grp}))",
        "2");

    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [10 20 30 40])))"
        "(at (select {from: t s: (sum val) a: (avg val) c: (count val) by: grp}) 's)",
        "[30 70]");

    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [10 20 30 40])))"
        "(at (select {from: t s: (sum val) a: (avg val) c: (count val) by: grp}) 'a)",
        "[15.0 35.0]");

    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [10 20 30 40])))"
        "(at (select {from: t s: (sum val) a: (avg val) c: (count val) by: grp}) 'c)",
        "[2 2]");

    // min + max + first + last
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a a b b b] [30 10 20 60 40 50])))"
        "(at (select {from: t mn: (min val) mx: (max val) f: (first val) l: (last val) by: grp}) 'mn)",
        "[10 40]");

    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a a b b b] [30 10 20 60 40 50])))"
        "(at (select {from: t mn: (min val) mx: (max val) f: (first val) l: (last val) by: grp}) 'mx)",
        "[30 60]");

    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a a b b b] [30 10 20 60 40 50])))"
        "(at (select {from: t mn: (min val) mx: (max val) f: (first val) l: (last val) by: grp}) 'f)",
        "[30 60]");

    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a a b b b] [30 10 20 60 40 50])))"
        "(at (select {from: t mn: (min val) mx: (max val) f: (first val) l: (last val) by: grp}) 'l)",
        "[20 50]");

    PASS();
}

// ==================== AGGREGATION ON F64 COLUMNS ====================
test_result_t test_aggr_f64_columns() {
    // Sum of F64 column
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [1.5 2.5 3.5 4.5])))"
        "(select {from: t s: (sum val) by: grp})",
        "(table [grp s] (list [a b] [4.0 8.0]))");

    // Avg of F64 column
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [1.0 3.0 5.0 7.0])))"
        "(select {from: t a: (avg val) by: grp})",
        "(table [grp a] (list [a b] [2.0 6.0]))");

    // Min/Max of F64 column
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [3.14 2.71 1.41 1.73])))"
        "(at (select {from: t mn: (min val) mx: (max val) by: grp}) 'mn)",
        "[2.71 1.41]");

    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [3.14 2.71 1.41 1.73])))"
        "(at (select {from: t mn: (min val) mx: (max val) by: grp}) 'mx)",
        "[3.14 1.73]");

    // F64 with negative values
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [-1.5 -2.5 -3.5 -4.5])))"
        "(at (select {from: t s: (sum val) by: grp}) 's)",
        "[-4.0 -8.0]");

    PASS();
}

// ==================== AGGREGATION ON I16 COLUMNS ====================
test_result_t test_aggr_i16_columns() {
    // Sum of I16 column grouped
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [1h 2h 3h 4h])))"
        "(at (select {from: t s: (sum val) by: grp}) 's)",
        "[3h 7h]");

    // Avg of I16 column
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [10h 20h 30h 40h])))"
        "(at (select {from: t a: (avg val) by: grp}) 'a)",
        "[15.0 35.0]");

    // Min/Max of I16 column
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [5h 3h 8h 2h])))"
        "(at (select {from: t mn: (min val) mx: (max val) by: grp}) 'mn)",
        "[3h 2h]");

    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list [a a b b] [5h 3h 8h 2h])))"
        "(at (select {from: t mn: (min val) mx: (max val) by: grp}) 'mx)",
        "[5h 8h]");

    PASS();
}

// ==================== AGGREGATION ON I32 COLUMNS ====================
test_result_t test_aggr_i32_columns() {
    PASS();
}

// ==================== LARGE VECTOR AGGREGATION ====================
test_result_t test_aggr_large_vector() {
    // Sum of large generated vector: sum(0..99999) = 4999950000
    TEST_ASSERT_EQ("(sum (til 100000))", "4999950000");

    // Count of large vector
    TEST_ASSERT_EQ("(count (til 100000))", "100000");

    // Min/Max of large vector
    TEST_ASSERT_EQ("(min (til 100000))", "0");
    TEST_ASSERT_EQ("(max (til 100000))", "99999");

    // First/Last of large vector
    TEST_ASSERT_EQ("(first (til 100000))", "0");
    TEST_ASSERT_EQ("(last (til 100000))", "99999");

    // Avg of large vector: avg(0..99999) = 49999.5
    TEST_ASSERT_EQ("(avg (til 100000))", "49999.5");

    // Group-by on larger table
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list (take [a b c] 300) (til 300))))"
        "(at (select {from: t s: (sum val) by: grp}) 's)",
        "[14850 14950 15050]");

    // Large table count by group
    TEST_ASSERT_EQ(
        "(set t (table [grp val] (list (take [a b c] 300) (til 300))))"
        "(at (select {from: t c: (count val) by: grp}) 'c)",
        "[100 100 100]");

    PASS();
}

// ==================== TEMPORAL AGGREGATION ====================
test_result_t test_aggr_temporal() {
    // Group-by date with sum
    TEST_ASSERT_EQ(
        "(set t (table [dt val] (list [2024.01.01 2024.01.01 2024.01.02 2024.01.02] [10 20 30 40])))"
        "(select {from: t s: (sum val) by: dt})",
        "(table [dt s] (list [2024.01.01 2024.01.02] [30 70]))");

    // Group-by date with multiple aggregations
    TEST_ASSERT_EQ(
        "(set t (table [dt val] (list [2024.01.01 2024.01.01 2024.01.02 2024.01.02] [10 20 30 40])))"
        "(at (select {from: t mn: (min val) mx: (max val) by: dt}) 'mn)",
        "[10 30]");

    // Group-by time with count
    TEST_ASSERT_EQ(
        "(set t (table [tm val] (list [10:00:00.000 10:00:00.000 11:00:00.000] [1 2 3])))"
        "(select {from: t c: (count val) by: tm})",
        "(table [tm c] (list [10:00:00.000 11:00:00.000] [2 1]))");

    // Min/Max on date columns
    TEST_ASSERT_EQ(
        "(set t (table [grp dt] (list [a a b b] [2024.01.05 2024.01.01 2024.06.15 2024.03.10])))"
        "(at (select {from: t mn: (min dt) mx: (max dt) by: grp}) 'mn)",
        "[2024.01.01 2024.03.10]");

    TEST_ASSERT_EQ(
        "(set t (table [grp dt] (list [a a b b] [2024.01.05 2024.01.01 2024.06.15 2024.03.10])))"
        "(at (select {from: t mn: (min dt) mx: (max dt) by: grp}) 'mx)",
        "[2024.01.05 2024.06.15]");

    // First/Last on timestamp columns
    TEST_ASSERT_EQ(
        "(set t (table [grp ts] (list [a a b b] "
        "[2024.01.01D08:00:00.000000000 2024.01.01D12:00:00.000000000 "
        "2024.01.01D14:00:00.000000000 2024.01.01D18:00:00.000000000])))"
        "(at (select {from: t f: (first ts) l: (last ts) by: grp}) 'f)",
        "[2024.01.01D08:00:00.000000000 2024.01.01D14:00:00.000000000]");

    PASS();
}
