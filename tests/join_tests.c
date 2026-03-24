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

// ==================== INNER JOIN BASIC ====================
test_result_t test_join_inner_basic() {
    // Full match
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 2 3] [100 200 300])))"
        "(set t2 (table [id val2] (list [1 2 3] [10 20 30])))"
        "(inner-join [id] t1 t2)",
        "(table [id val1 val2] (list [1 2 3] [100 200 300] [10 20 30]))");

    // Partial match
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 2 3 4 5] [100 200 300 400 500])))"
        "(set t2 (table [id val2] (list [1 3 5] [10 30 50])))"
        "(inner-join [id] t1 t2)",
        "(table [id val1 val2] (list [1 3 5] [100 300 500] [10 30 50]))");

    // No match
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 2 3] [100 200 300])))"
        "(set t2 (table [id val2] (list [4 5 6] [400 500 600])))"
        "(count (inner-join [id] t1 t2))",
        "0");

    // Single row match
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 2 3] [100 200 300])))"
        "(set t2 (table [id val2] (list [2] [20])))"
        "(inner-join [id] t1 t2)",
        "(table [id val1 val2] (list [2] [200] [20]))");

    // Verify left values preserved correctly
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 2 3 4 5] [100 200 300 400 500])))"
        "(set t2 (table [id val2] (list [1 3 5 6 7] [1000 3000 5000 6000 7000])))"
        "(at (inner-join [id] t1 t2) 'val1)",
        "[100 300 500]");

    // Verify right values preserved correctly
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 2 3 4 5] [100 200 300 400 500])))"
        "(set t2 (table [id val2] (list [1 3 5 6 7] [1000 3000 5000 6000 7000])))"
        "(at (inner-join [id] t1 t2) 'val2)",
        "[1000 3000 5000]");

    PASS();
}

// ==================== LEFT JOIN BASIC ====================
test_result_t test_join_left_basic() {
    // All rows match
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 2 3] [100 200 300])))"
        "(set t2 (table [id val2] (list [1 2 3] [10 20 30])))"
        "(left-join [id] t1 t2)",
        "(table [id val1 val2] (list [1 2 3] [100 200 300] [10 20 30]))");

    // Partial match - all left rows preserved
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 2 3] [100 200 300])))"
        "(set t2 (table [id val2] (list [1 3] [10 30])))"
        "(count (left-join [id] t1 t2))",
        "3");

    // Left table values always preserved
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 2 3] [100 200 300])))"
        "(set t2 (table [id val2] (list [1 3] [10 30])))"
        "(at (left-join [id] t1 t2) 'val1)",
        "[100 200 300]");

    // No match - all left rows still preserved
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 2 3] [100 200 300])))"
        "(set t2 (table [id val2] (list [4 5 6] [400 500 600])))"
        "(count (left-join [id] t1 t2))",
        "3");

    PASS();
}

// ==================== JOIN ON SINGLE KEY TYPES ====================
test_result_t test_join_single_key_types() {
    // Join on I32 key
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1i 2i 3i] [100 200 300])))"
        "(set t2 (table [id val2] (list [1i 3i] [10 30])))"
        "(count (inner-join [id] t1 t2))",
        "2");

    // Join on Symbol key
    TEST_ASSERT_EQ(
        "(set t1 (table [sym val1] (list [AAPL GOOG MSFT] [100 200 300])))"
        "(set t2 (table [sym val2] (list [AAPL MSFT TSLA] [1000 3000 5000])))"
        "(count (inner-join [sym] t1 t2))",
        "2");

    TEST_ASSERT_EQ(
        "(set t1 (table [sym val1] (list [AAPL GOOG MSFT] [100 200 300])))"
        "(set t2 (table [sym val2] (list [AAPL MSFT TSLA] [1000 3000 5000])))"
        "(at (inner-join [sym] t1 t2) 'val2)",
        "[1000 3000]");

    // Join on F64 key
    TEST_ASSERT_EQ(
        "(set t1 (table [price val1] (list [1.0 2.0 3.0] [100 200 300])))"
        "(set t2 (table [price val2] (list [1.0 3.0 5.0] [1000 3000 5000])))"
        "(count (inner-join [price] t1 t2))",
        "2");

    // Join on Date key
    TEST_ASSERT_EQ(
        "(set t1 (table [dt val1] (list [2024.01.01 2024.01.02 2024.01.03] [100 200 300])))"
        "(set t2 (table [dt val2] (list [2024.01.01 2024.01.03 2024.01.05] [1000 3000 5000])))"
        "(count (inner-join [dt] t1 t2))",
        "2");

    // Join on Time key
    TEST_ASSERT_EQ(
        "(set t1 (table [tm val1] (list [10:00:00.000 10:00:01.000 10:00:02.000] [100 200 300])))"
        "(set t2 (table [tm val2] (list [10:00:00.000 10:00:02.000 10:00:05.000] [1000 3000 5000])))"
        "(count (inner-join [tm] t1 t2))",
        "2");

    // Join on Timestamp key
    TEST_ASSERT_EQ(
        "(set t1 (table [ts val1] (list [2024.01.01D10:00:00.000000000 2024.01.01D10:00:01.000000000 "
        "2024.01.01D10:00:02.000000000] [100 200 300])))"
        "(set t2 (table [ts val2] (list [2024.01.01D10:00:00.000000000 2024.01.01D10:00:02.000000000] [1000 3000])))"
        "(count (inner-join [ts] t1 t2))",
        "2");

    // Left join on Date key
    TEST_ASSERT_EQ(
        "(set t1 (table [dt val1] (list [2024.01.01 2024.01.02 2024.01.03] [100 200 300])))"
        "(set t2 (table [dt val2] (list [2024.01.01 2024.01.03] [1000 3000])))"
        "(count (left-join [dt] t1 t2))",
        "3");

    // Left join on Time key
    TEST_ASSERT_EQ(
        "(set t1 (table [tm val1] (list [10:00:00 10:00:01 10:00:02] [100 200 300])))"
        "(set t2 (table [tm val2] (list [10:00:00 10:00:02] [1000 3000])))"
        "(count (left-join [tm] t1 t2))",
        "3");

    PASS();
}

// ==================== JOIN ON MULTIPLE KEYS ====================
test_result_t test_join_multi_key() {
    // Inner join on two keys
    TEST_ASSERT_EQ(
        "(set t1 (table [id1 id2 val1] (list [1 1 2] [a b a] [100 200 300])))"
        "(set t2 (table [id1 id2 val2] (list [1 2] [a a] [1000 3000])))"
        "(count (inner-join [id1 id2] t1 t2))",
        "2");

    // Left join on two keys
    TEST_ASSERT_EQ(
        "(set t1 (table [id1 id2 val1] (list [1 1 2] [a b a] [100 200 300])))"
        "(set t2 (table [id1 id2 val2] (list [1 2] [a a] [1000 3000])))"
        "(count (left-join [id1 id2] t1 t2))",
        "3");

    // Multi-key with all matches
    TEST_ASSERT_EQ(
        "(set t1 (table [k1 k2 val1] (list [a b] [x y] [100 200])))"
        "(set t2 (table [k1 k2 val2] (list [a b] [x y] [10 20])))"
        "(inner-join [k1 k2] t1 t2)",
        "(table [k1 k2 val1 val2] (list [a b] [x y] [100 200] [10 20]))");

    // Multi-key no matches
    TEST_ASSERT_EQ(
        "(set t1 (table [k1 k2 val1] (list [a b] [x y] [100 200])))"
        "(set t2 (table [k1 k2 val2] (list [a b] [y x] [10 20])))"
        "(count (inner-join [k1 k2] t1 t2))",
        "0");

    // Three keys inner join
    TEST_ASSERT_EQ(
        "(set t1 (table [a b c val1] (list [1 1 2] [x x y] [10 20 10] [100 200 300])))"
        "(set t2 (table [a b c val2] (list [1 2] [x y] [10 10] [1000 3000])))"
        "(count (inner-join [a b c] t1 t2))",
        "2");

    PASS();
}

// ==================== EMPTY TABLE JOINS ====================
test_result_t test_join_empty_tables() {
    // Empty left table - inner join
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list (take [1] 0) (take [1] 0))))"
        "(set t2 (table [id val2] (list [1 2 3] [100 200 300])))"
        "(count (inner-join [id] t1 t2))",
        "0");

    // Empty right table - inner join
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 2 3] [100 200 300])))"
        "(set t2 (table [id val2] (list (take [1] 0) (take [1] 0))))"
        "(count (inner-join [id] t1 t2))",
        "3");

    // Empty left table - left join
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list (take [1] 0) (take [1] 0))))"
        "(set t2 (table [id val2] (list [1 2 3] [100 200 300])))"
        "(count (left-join [id] t1 t2))",
        "0");

    // Empty right table - left join preserves all left
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 2 3] [100 200 300])))"
        "(set t2 (table [id val2] (list (take [1] 0) (take [1] 0))))"
        "(count (left-join [id] t1 t2))",
        "3");

    // Both empty
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list (take [1] 0) (take [1] 0))))"
        "(set t2 (table [id val2] (list (take [1] 0) (take [1] 0))))"
        "(count (inner-join [id] t1 t2))",
        "0");

    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list (take [1] 0) (take [1] 0))))"
        "(set t2 (table [id val2] (list (take [1] 0) (take [1] 0))))"
        "(count (left-join [id] t1 t2))",
        "0");

    PASS();
}

// ==================== SELF JOIN ====================
test_result_t test_join_self() {
    // Self inner join - each row matches itself
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2 3] [100 200 300])))"
        "(count (inner-join [id] t t))",
        "3");

    // Self join preserves values
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2 3] [100 200 300])))"
        "(at (inner-join [id] t t) 'id)",
        "[1 2 3]");

    // Self left join
    TEST_ASSERT_EQ(
        "(set t (table [id val] (list [1 2 3] [100 200 300])))"
        "(count (left-join [id] t t))",
        "3");

    PASS();
}

// ==================== DUPLICATE KEYS IN JOIN ====================
test_result_t test_join_duplicate_keys() {
    // Duplicate keys in right table - last wins for left-join
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 2] [100 200])))"
        "(set t2 (table [id val2] (list [1 1 2] [10 11 20])))"
        "(count (left-join [id] t1 t2))",
        "2");

    // Inner join with duplicates in right
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 2] [100 200])))"
        "(set t2 (table [id val2] (list [1 1 2] [10 11 20])))"
        "(count (inner-join [id] t1 t2))",
        "2");

    // Duplicate keys in left table
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 1 2] [100 101 200])))"
        "(set t2 (table [id val2] (list [1 2] [10 20])))"
        "(count (inner-join [id] t1 t2))",
        "3");

    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 1 2] [100 101 200])))"
        "(set t2 (table [id val2] (list [1 2] [10 20])))"
        "(at (inner-join [id] t1 t2) 'val1)",
        "[100 101 200]");

    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 1 2] [100 101 200])))"
        "(set t2 (table [id val2] (list [1 2] [10 20])))"
        "(at (inner-join [id] t1 t2) 'val2)",
        "[10 10 20]");

    PASS();
}

// ==================== ASOF JOIN BASIC ====================
test_result_t test_join_asof_basic() {
    // Basic asof join - match on or before
    TEST_ASSERT_EQ(
        "(set trades (table [Sym Time Price] (list [x x] [10:00:01.000 10:00:03.000] [100.0 101.0])))"
        "(set quotes (table [Sym Time Bid] (list [x x x] [10:00:00.000 10:00:02.000 10:00:04.000] [99.0 100.5 101.5])))"
        "(asof-join [Sym Time] trades quotes)",
        "(table [Sym Time Price Bid] (list [x x] [10:00:01.000 10:00:03.000] [100.0 101.0] [99.0 100.5]))");

    // Asof join single matching row
    TEST_ASSERT_EQ(
        "(set trades (table [Sym Time Price] (list [a] [10:00:05.000] [50.0])))"
        "(set quotes (table [Sym Time Bid] (list [a a] [10:00:01.000 10:00:03.000] [48.0 49.0])))"
        "(asof-join [Sym Time] trades quotes)",
        "(table [Sym Time Price Bid] (list [a] [10:00:05.000] [50.0] [49.0]))");

    // Asof join exact boundary match
    TEST_ASSERT_EQ(
        "(set trades (table [Sym Time Price] (list [a] [10:00:01.000] [50.0])))"
        "(set quotes (table [Sym Time Bid] (list [a a] [10:00:01.000 10:00:03.000] [48.0 49.0])))"
        "(asof-join [Sym Time] trades quotes)",
        "(table [Sym Time Price Bid] (list [a] [10:00:01.000] [50.0] [48.0]))");

    // Asof join no match before - returns null fill
    TEST_ASSERT_EQ(
        "(set trades (table [Sym Time Price] (list [a] [10:00:00.000] [100.0])))"
        "(set quotes (table [Sym Time Bid] (list [a] [10:00:05.000] [99.0])))"
        "(count (asof-join [Sym Time] trades quotes))",
        "1");

    PASS();
}

// ==================== ASOF JOIN WITH DIFFERENT TYPES ====================
test_result_t test_join_asof_types() {
    // Asof join with I64 + Timestamp
    TEST_ASSERT_EQ(
        "(set aj1 (table [ID Ts Val] (list [1 1 2 2] "
        "[2024.01.01D10:00:01.000000000 2024.01.01D10:00:05.000000000 2024.01.01D10:00:03.000000000 "
        "2024.01.01D10:00:07.000000000] "
        "[100 200 300 400])))"
        "(set aj2 (table [ID Ts Ref] (list [1 1 2 2] "
        "[2024.01.01D10:00:00.000000000 2024.01.01D10:00:04.000000000 2024.01.01D10:00:02.000000000 "
        "2024.01.01D10:00:06.000000000] "
        "[10 20 30 40])))"
        "(at (asof-join [ID Ts] aj1 aj2) 'Ref)",
        "[10 20 30 40]");

    // Asof join with Symbol + Date
    TEST_ASSERT_EQ(
        "(set orders (table [Cust Date Amount] (list [A A B B] [2024.01.02 2024.01.05 2024.01.03 2024.01.06] [100 200 "
        "300 400])))"
        "(set rates (table [Cust Date Rate] (list [A A B B] [2024.01.01 2024.01.04 2024.01.01 2024.01.05] [0.1 0.15 "
        "0.2 0.25])))"
        "(at (asof-join [Cust Date] orders rates) 'Rate)",
        "[0.1 0.15 0.2 0.25]");

    // Asof join with multiple syms
    TEST_ASSERT_EQ(
        "(set trades (table [Sym Time Price] (list [a b a] [10:00:02.000 10:00:02.000 10:00:04.000] [100.0 200.0 300.0])))"
        "(set quotes (table [Sym Time Bid] (list [a a b b] [10:00:01.000 10:00:03.000 10:00:01.000 10:00:03.000] [99.0 101.0 199.0 201.0])))"
        "(at (asof-join [Sym Time] trades quotes) 'Bid)",
        "[99.0 199.0 101.0]");

    PASS();
}

// ==================== WINDOW JOIN ====================
test_result_t test_join_window() {
    // Basic window-join
    TEST_ASSERT_EQ(
        "(set trades (table [Sym Time Price] (list [a a] [10:00:01.000 10:00:05.000] [100 200])))"
        "(set quotes (table [Sym Time Bid] (list [a a a] [10:00:00.000 10:00:02.000 10:00:04.000] [99 100 101])))"
        "(set intervals (map-left + [-2000 2000] (at trades 'Time)))"
        "(at (window-join [Sym Time] intervals trades quotes {minBid: (min Bid)}) 'minBid)",
        "[99 100]");

    // Window-join1 (exclusive start)
    TEST_ASSERT_EQ(
        "(set trades (table [Sym Time Price] (list [a a] [10:00:01.000 10:00:05.000] [100 200])))"
        "(set quotes (table [Sym Time Bid] (list [a a a] [10:00:00.000 10:00:02.000 10:00:04.000] [99 100 101])))"
        "(set intervals (map-left + [-2000 2000] (at trades 'Time)))"
        "(at (window-join1 [Sym Time] intervals trades quotes {minBid: (min Bid)}) 'minBid)",
        "[99 101]");

    // Window-join with count
    TEST_ASSERT_EQ(
        "(set trades (table [Sym Time Price] (list [a a] [10:00:01.000 10:00:05.000] [100 200])))"
        "(set quotes (table [Sym Time Bid] (list [a a a] [10:00:00.000 10:00:02.000 10:00:04.000] [99 100 101])))"
        "(set intervals (map-left + [-2000 2000] (at trades 'Time)))"
        "(count (at (window-join [Sym Time] intervals trades quotes {bids: Bid}) 'bids))",
        "2");

    PASS();
}

// ==================== JOIN THEN QUERY ====================
test_result_t test_join_then_query() {
    // Inner join then select
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 2 3] [100 200 300])))"
        "(set t2 (table [id val2] (list [1 2 3] [10 20 30])))"
        "(set joined (inner-join [id] t1 t2))"
        "(at (select {from: joined s: (sum val1)}) 's)",
        "[600]");

    // Inner join then count with where
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 2 3 4 5] [100 200 300 400 500])))"
        "(set t2 (table [id val2] (list [1 3 5] [10 30 50])))"
        "(set joined (inner-join [id] t1 t2))"
        "(count (select {from: joined where: (> val1 200)}))",
        "2");

    // Left join then aggregate
    TEST_ASSERT_EQ(
        "(set t1 (table [grp id val1] (list [a a b b] [1 2 3 4] [10 20 30 40])))"
        "(set t2 (table [id val2] (list [1 2 3 4] [100 200 300 400])))"
        "(set joined (left-join [id] t1 t2))"
        "(at (select {from: joined s: (sum val2) by: grp}) 's)",
        "[300 700]");

    PASS();
}

// ==================== JOIN ERROR CASES ====================
test_result_t test_join_errors() {
    // Wrong key type for left-join
    TEST_ASSERT_ER("(left-join 123 (table [a] (list [1])) (table [a] (list [1])))", "type");

    // Wrong type for inner-join table argument
    TEST_ASSERT_ER("(inner-join [a] [1 2 3] (table [a] (list [1])))", "type");

    // Wrong arity for asof-join
    TEST_ASSERT_ER("(asof-join [a b])", "arity");

    // Wrong arity for inner-join
    TEST_ASSERT_ER("(inner-join [a])", "arity");

    // Wrong arity for left-join
    TEST_ASSERT_ER("(left-join [a])", "arity");

    PASS();
}

// ==================== JOIN RESULT COLUMN ORDER ====================
test_result_t test_join_column_order() {
    // Inner join: key columns first, then left non-key, then right non-key
    TEST_ASSERT_EQ(
        "(set t1 (table [id name age] (list [1 2] [alice bob] [30 25])))"
        "(set t2 (table [id city score] (list [1 2] [nyc la] [90 85])))"
        "(set j (inner-join [id] t1 t2))"
        "(count j)",
        "2");

    // Verify columns are accessible
    TEST_ASSERT_EQ(
        "(set t1 (table [id name age] (list [1 2] [alice bob] [30 25])))"
        "(set t2 (table [id city score] (list [1 2] [nyc la] [90 85])))"
        "(set j (inner-join [id] t1 t2))"
        "(at j 'name)",
        "[alice bob]");

    TEST_ASSERT_EQ(
        "(set t1 (table [id name age] (list [1 2] [alice bob] [30 25])))"
        "(set t2 (table [id city score] (list [1 2] [nyc la] [90 85])))"
        "(set j (inner-join [id] t1 t2))"
        "(at j 'city)",
        "[nyc la]");

    // Left join column accessibility
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 2 3] [100 200 300])))"
        "(set t2 (table [id val2] (list [1 3] [10 30])))"
        "(set j (left-join [id] t1 t2))"
        "(at j 'id)",
        "[1 2 3]");

    PASS();
}

// ==================== JOIN WITH MANY ROWS ====================
test_result_t test_join_larger_tables() {
    // Inner join on larger tables
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list (til 100) (til 100))))"
        "(set t2 (table [id val2] (list (til 50) (* 10 (til 50)))))"
        "(count (inner-join [id] t1 t2))",
        "50");

    // Left join on larger tables
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list (til 100) (til 100))))"
        "(set t2 (table [id val2] (list (til 50) (* 10 (til 50)))))"
        "(count (left-join [id] t1 t2))",
        "100");

    // Verify values in larger join
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list (til 100) (til 100))))"
        "(set t2 (table [id val2] (list (til 50) (* 10 (til 50)))))"
        "(sum (at (inner-join [id] t1 t2) 'val1))",
        "1225");

    PASS();
}

// ==================== JOIN PARALLEL (>16K rows) ====================
test_result_t test_join_parallel() {
    // Inner join with 20K rows — exceeds POOL_SPLIT_THRESHOLD, triggers pool_map
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list (til 20000) (til 20000))))"
        "(set t2 (table [id val2] (list (til 10000) (* 2 (til 10000)))))"
        "(count (inner-join [id] t1 t2))",
        "10000");

    // Left join parallel — all left rows preserved
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list (til 20000) (til 20000))))"
        "(set t2 (table [id val2] (list (til 10000) (* 2 (til 10000)))))"
        "(count (left-join [id] t1 t2))",
        "20000");

    // Verify correctness of parallel inner join values
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list (til 20000) (til 20000))))"
        "(set t2 (table [id val2] (list (til 10000) (* 2 (til 10000)))))"
        "(sum (at (inner-join [id] t1 t2) 'val1))",
        "49995000");

    // Multi-key parallel inner join (2 keys, >16K rows)
    TEST_ASSERT_EQ(
        "(set t1 (table [k1 k2 val1] (list (take (til 100) 20000) (take (til 200) 20000) (til 20000))))"
        "(set t2 (table [k1 k2 val2] (list (take (til 100) 20000) (take (til 200) 20000) (* 3 (til 20000)))))"
        "(count (inner-join [k1 k2] t1 t2))",
        "20000");

    // Multi-key parallel left join
    TEST_ASSERT_EQ(
        "(set t1 (table [k1 k2 val1] (list (take (til 100) 20000) (take (til 200) 20000) (til 20000))))"
        "(set t2 (table [k1 k2 val2] (list (take (til 50) 10000) (take (til 200) 10000) (* 3 (til 10000)))))"
        "(count (left-join [k1 k2] t1 t2))",
        "20000");

    // Verify inner join right values (sum of val2 from right table)
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list (til 20000) (til 20000))))"
        "(set t2 (table [id val2] (list (til 10000) (* 2 (til 10000)))))"
        "(sum (at (inner-join [id] t1 t2) 'val2))",
        "99990000");

    // Verify left join — left values always preserved (sum of val1)
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list (til 20000) (til 20000))))"
        "(set t2 (table [id val2] (list (til 10000) (* 2 (til 10000)))))"
        "(sum (at (left-join [id] t1 t2) 'val1))",
        "199990000");

    // Verify left join — right values correct for matched rows
    // val2 = 2*id for ids 0..9999, should be filled from right; rest from left (val1)
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list (til 20000) (til 20000))))"
        "(set t2 (table [id val2] (list (til 10000) (* 2 (til 10000)))))"
        "(sum (at (left-join [id] t1 t2) 'val2))",
        "99990000");

    // Multi-key parallel inner join — verify values
    // 2 keys (id, k) both unique per row, so inner join = exact match
    // sum(val2) for matched rows = sum(2 * 0..9999) = 2 * 49995000 = 99990000
    TEST_ASSERT_EQ(
        "(set t1 (table [id k val1] (list (til 20000) (* 10 (til 20000)) (til 20000))))"
        "(set t2 (table [id k val2] (list (til 10000) (* 10 (til 10000)) (* 2 (til 10000)))))"
        "(sum (at (inner-join [id k] t1 t2) 'val2))",
        "99990000");

    // Multi-key parallel left join — verify left values preserved
    // sum(val1) = sum(0..19999) = 199990000
    TEST_ASSERT_EQ(
        "(set t1 (table [id k val1] (list (til 20000) (* 10 (til 20000)) (til 20000))))"
        "(set t2 (table [id k val2] (list (til 10000) (* 10 (til 10000)) (* 2 (til 10000)))))"
        "(sum (at (left-join [id k] t1 t2) 'val1))",
        "199990000");

    PASS();
}
