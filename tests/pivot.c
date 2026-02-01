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

// Basic pivot with single index column and sum aggregation
test_result_t test_pivot_basic_sum() {
    TEST_ASSERT_EQ(
        "(do "
        "  (set t (table [sym side val] (list [A A B B] [X Y X Y] [10 20 30 40]))) "
        "  (at (pivot t 'sym 'side 'val sum) 'X))",
        "[10 30]");

    TEST_ASSERT_EQ(
        "(do "
        "  (set t (table [sym side val] (list [A A B B] [X Y X Y] [10 20 30 40]))) "
        "  (at (pivot t 'sym 'side 'val sum) 'Y))",
        "[20 40]");

    PASS();
}

// Pivot with count aggregation
test_result_t test_pivot_count() {
    TEST_ASSERT_EQ(
        "(do "
        "  (set t (table [sym side val] (list [A A A B B] [X X Y X Y] [1 2 3 4 5]))) "
        "  (at (pivot t 'sym 'side 'val count) 'X))",
        "[2 1]");

    TEST_ASSERT_EQ(
        "(do "
        "  (set t (table [sym side val] (list [A A A B B] [X X Y X Y] [1 2 3 4 5]))) "
        "  (at (pivot t 'sym 'side 'val count) 'Y))",
        "[1 1]");

    PASS();
}

// Pivot with avg aggregation
test_result_t test_pivot_avg() {
    TEST_ASSERT_EQ(
        "(do "
        "  (set t (table [sym side val] (list [A A B B] [X Y X Y] [10 20 30 40]))) "
        "  (at (pivot t 'sym 'side 'val avg) 'X))",
        "[10.00 30.00]");

    TEST_ASSERT_EQ(
        "(do "
        "  (set t (table [sym side val] (list [A A A B] [X X Y X] [10 20 30 40]))) "
        "  (first (at (pivot t 'sym 'side 'val avg) 'X)))",
        "15.00");

    PASS();
}

// Pivot with min aggregation
test_result_t test_pivot_min() {
    TEST_ASSERT_EQ(
        "(do "
        "  (set t (table [sym side val] (list [A A A B B] [X X Y X Y] [10 5 20 30 40]))) "
        "  (at (pivot t 'sym 'side 'val min) 'X))",
        "[5 30]");

    PASS();
}

// Pivot with max aggregation
test_result_t test_pivot_max() {
    TEST_ASSERT_EQ(
        "(do "
        "  (set t (table [sym side val] (list [A A A B B] [X X Y X Y] [10 50 20 30 40]))) "
        "  (at (pivot t 'sym 'side 'val max) 'X))",
        "[50 30]");

    PASS();
}

// Pivot with first aggregation
test_result_t test_pivot_first() {
    TEST_ASSERT_EQ(
        "(do "
        "  (set t (table [sym side val] (list [A A A B B] [X X Y X Y] [10 20 30 40 50]))) "
        "  (at (pivot t 'sym 'side 'val first) 'X))",
        "[10 40]");

    PASS();
}

// Pivot with last aggregation
test_result_t test_pivot_last() {
    TEST_ASSERT_EQ(
        "(do "
        "  (set t (table [sym side val] (list [A A A B B] [X X Y X Y] [10 20 30 40 50]))) "
        "  (at (pivot t 'sym 'side 'val last) 'X))",
        "[20 40]");

    PASS();
}

// Pivot with median aggregation
test_result_t test_pivot_med() {
    // Mixed data: both A and B have X and Y values
    // A has X values [10, 30], median = 20.00
    // B has X values [40], median = 40.00
    TEST_ASSERT_EQ(
        "(do "
        "  (set t (table [sym side val] (list [A A A B B B] [X Y X X Y Y] [10 20 30 40 50 60]))) "
        "  (at (pivot t 'sym 'side 'val med) 'X))",
        "[20.00 40.00]");

    // A has Y values [20], median = 20.00
    // B has Y values [50, 60], median = 55.00
    TEST_ASSERT_EQ(
        "(do "
        "  (set t (table [sym side val] (list [A A A B B B] [X Y X X Y Y] [10 20 30 40 50 60]))) "
        "  (at (pivot t 'sym 'side 'val med) 'Y))",
        "[20.00 55.00]");

    // Single value per group returns that value
    TEST_ASSERT_EQ(
        "(do "
        "  (set t (table [sym side val] (list [A B] [X X] [100 200]))) "
        "  (at (pivot t 'sym 'side 'val med) 'X))",
        "[100.00 200.00]");

    PASS();
}

// Pivot with multiple index columns
test_result_t test_pivot_multi_index() {
    TEST_ASSERT_EQ(
        "(do "
        "  (set t (table [date sym side val] (list "
        "    [2024.01.01 2024.01.01 2024.01.02 2024.01.02] "
        "    [A B A B] "
        "    [X X X X] "
        "    [10 20 30 40]))) "
        "  (count (pivot t [date sym] 'side 'val sum)))",
        "4");

    PASS();
}

// Pivot preserves index column values
test_result_t test_pivot_index_values() {
    TEST_ASSERT_EQ(
        "(do "
        "  (set t (table [sym side val] (list [A B C] [X X X] [10 20 30]))) "
        "  (at (pivot t 'sym 'side 'val sum) 'sym))",
        "[A B C]");

    PASS();
}

// Pivot with multiple pivot values creates multiple columns
test_result_t test_pivot_multiple_columns() {
    TEST_ASSERT_EQ(
        "(do "
        "  (set t (table [sym side val] (list [A A B B] [X Y X Y] [10 20 30 40]))) "
        "  (count (key (pivot t 'sym 'side 'val sum))))",
        "3");  // sym + X + Y columns

    PASS();
}

// Pivot with float values
test_result_t test_pivot_float_values() {
    TEST_ASSERT_EQ(
        "(do "
        "  (set t (table [sym side val] (list [A A B B] [X Y X Y] [1.5 2.5 3.5 4.5]))) "
        "  (at (pivot t 'sym 'side 'val sum) 'X))",
        "[1.5 3.5]");

    PASS();
}

// Pivot error: wrong argument types
test_result_t test_pivot_errors() {
    // Not a table
    TEST_ASSERT_ER("(pivot [1 2 3] 'a 'b 'c sum)", "type");

    // Wrong arity
    TEST_ASSERT_ER("(pivot (table [a] (list [1])) 'a 'b 'c)", "arity");

    // Invalid aggfunc (must be a function, not a symbol)
    TEST_ASSERT_ER(
        "(pivot (table [a b c] (list [1] [2] [3])) 'a 'b 'c 'invalid)",
        "type");

    PASS();
}

// Pivot with larger dataset
test_result_t test_pivot_large() {
    TEST_ASSERT_EQ(
        "(do "
        "  (set t (table [sym side val] (list "
        "    (take [A B C D E] 100) "
        "    (take [X Y] 100) "
        "    (til 100)))) "
        "  (sum (at (pivot t 'sym 'side 'val sum) 'X)))",
        "2450");  // sum of even indices 0,2,4,...,98

    PASS();
}

// Pivot with symbols as pivot values
test_result_t test_pivot_symbol_columns() {
    TEST_ASSERT_EQ(
        "(do "
        "  (set t (table [sym metric val] (list [A A B B] [price volume price volume] [100 1000 200 2000]))) "
        "  (at (pivot t 'sym 'metric 'val sum) 'price))",
        "[100 200]");

    TEST_ASSERT_EQ(
        "(do "
        "  (set t (table [sym metric val] (list [A A B B] [price volume price volume] [100 1000 200 2000]))) "
        "  (at (pivot t 'sym 'metric 'val sum) 'volume))",
        "[1000 2000]");

    PASS();
}
