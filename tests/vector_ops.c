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

// ==================== TIL TESTS ====================
test_result_t test_vec_til() {
    // Basic til
    TEST_ASSERT_EQ("(til 0)", "[]");
    TEST_ASSERT_EQ("(til 1)", "[0]");
    TEST_ASSERT_EQ("(til 5)", "[0 1 2 3 4]");
    TEST_ASSERT_EQ("(til 10)", "[0 1 2 3 4 5 6 7 8 9]");
    // Til type is I64
    TEST_ASSERT_EQ("(type (til 5))", "'I64");
    // Negative til is error
    TEST_ASSERT_ER("(til -1)", "domain");
    TEST_ASSERT_ER("(til -100)", "domain");
    // Til combined with ops
    TEST_ASSERT_EQ("(sum (til 10))", "45");
    TEST_ASSERT_EQ("(sum (til 100))", "4950");
    TEST_ASSERT_EQ("(count (til 100))", "100");
    TEST_ASSERT_EQ("(first (til 5))", "0");
    TEST_ASSERT_EQ("(last (til 5))", "4");
    TEST_ASSERT_EQ("(reverse (til 5))", "[4 3 2 1 0]");
    // Til with arithmetic
    TEST_ASSERT_EQ("(+ 1 (til 5))", "[1 2 3 4 5]");
    TEST_ASSERT_EQ("(* 2 (til 5))", "[0 2 4 6 8]");
    // Large til
    TEST_ASSERT_EQ("(count (til 10000))", "10000");
    TEST_ASSERT_EQ("(last (til 10000))", "9999");

    PASS();
}

// ==================== TAKE TESTS ====================
test_result_t test_vec_take() {
    // Positive take
    TEST_ASSERT_EQ("(take [1 2 3 4 5] 0)", "[]");
    TEST_ASSERT_EQ("(take [1 2 3 4 5] 1)", "[1]");
    TEST_ASSERT_EQ("(take [1 2 3 4 5] 3)", "[1 2 3]");
    TEST_ASSERT_EQ("(take [1 2 3 4 5] 5)", "[1 2 3 4 5]");
    // Negative take (from end)
    TEST_ASSERT_EQ("(take [1 2 3 4 5] -1)", "[5]");
    TEST_ASSERT_EQ("(take [1 2 3 4 5] -3)", "[3 4 5]");
    TEST_ASSERT_EQ("(take [1 2 3 4 5] -5)", "[1 2 3 4 5]");
    // Over-take (cyclic wrapping)
    TEST_ASSERT_EQ("(take [1 2 3] 7)", "[1 2 3 1 2 3 1]");
    TEST_ASSERT_EQ("(take [1 2] 5)", "[1 2 1 2 1]");
    TEST_ASSERT_EQ("(take [42] 3)", "[42 42 42]");
    // Take from atom (repeat)
    TEST_ASSERT_EQ("(take 5 3)", "[5 5 5]");
    TEST_ASSERT_EQ("(take 0 5)", "[0 0 0 0 0]");
    // Take different types
    TEST_ASSERT_EQ("(take [1i 2i 3i] 2)", "[1i 2i]");
    TEST_ASSERT_EQ("(take [1i 2i 3i] -2)", "[2i 3i]");
    TEST_ASSERT_EQ("(take [1.0 2.0 3.0] 2)", "[1.0 2.0]");
    TEST_ASSERT_EQ("(take [1.0 2.0 3.0] -1)", "[3.0]");
    TEST_ASSERT_EQ("(take [true false true] 2)", "[true false]");
    TEST_ASSERT_EQ("(take [true false true] -2)", "[false true]");
    TEST_ASSERT_EQ("(take ['a 'b 'c] 2)", "['a 'b]");
    TEST_ASSERT_EQ("(take ['a 'b 'c] -1)", "['c]");
    // Take from empty
    TEST_ASSERT_EQ("(take [] 0)", "[]");
    // Take from list of mixed types
    TEST_ASSERT_EQ("(take (list 1 \"hello\" 'a) 2)", "(list 1 \"hello\")");
    TEST_ASSERT_EQ("(take (list 1 \"hello\" 'a) -1)", "(list 'a)");

    PASS();
}

// ==================== INDEXING (AT) TESTS ====================
test_result_t test_vec_at() {
    // Scalar index
    TEST_ASSERT_EQ("(at [10 20 30 40 50] 0)", "10");
    TEST_ASSERT_EQ("(at [10 20 30 40 50] 2)", "30");
    TEST_ASSERT_EQ("(at [10 20 30 40 50] 4)", "50");
    // Vector index
    TEST_ASSERT_EQ("(at [10 20 30 40 50] [0 2 4])", "[10 30 50]");
    TEST_ASSERT_EQ("(at [10 20 30 40 50] [4 3 2 1 0])", "[50 40 30 20 10]");
    TEST_ASSERT_EQ("(at [10 20 30] [0 0 0])", "[10 10 10]");
    // Out of bounds returns null
    TEST_ASSERT_EQ("(at [1 2 3] 10)", "0Nl");
    TEST_ASSERT_EQ("(at [] 0)", "0Nl");
    // Different types
    TEST_ASSERT_EQ("(at [1i 2i 3i] 1)", "2i");
    TEST_ASSERT_EQ("(at [1.0 2.0 3.0] 0)", "1.0");
    TEST_ASSERT_EQ("(at [true false true] 1)", "false");
    TEST_ASSERT_EQ("(at ['a 'b 'c] 2)", "'c");
    TEST_ASSERT_EQ("(at [1h 2h 3h] 0)", "1h");
    // Atom at - not supported, test error
    TEST_ASSERT_ER("(at 42 0)", "type");
    // At on table
    TEST_ASSERT_EQ("(at (table [a b] (list [1 2 3] [4 5 6])) 0)", "{a:1 b:4}");
    TEST_ASSERT_EQ("(at (table [a b] (list [1 2 3] [4 5 6])) 'a)", "[1 2 3]");
    // At on dict
    TEST_ASSERT_EQ("(at {a:1 b:2} 'a)", "1");
    // At on list
    TEST_ASSERT_EQ("(at (list \"hello\" \"world\") 0)", "\"hello\"");
    TEST_ASSERT_EQ("(at (list [1 2] [3 4]) 1)", "[3 4]");
    // Empty vector index returns null
    TEST_ASSERT_EQ("(at [10 20 30] [])", "0Nl");
    // Single element vector index
    TEST_ASSERT_EQ("(at [10 20 30] [1])", "[20]");

    PASS();
}

// ==================== WHERE TESTS ====================
test_result_t test_vec_where() {
    // Boolean where
    TEST_ASSERT_EQ("(where [true false true false true])", "[0 2 4]");
    TEST_ASSERT_EQ("(where [false false false])", "[]");
    TEST_ASSERT_EQ("(where [true true true])", "[0 1 2]");
    TEST_ASSERT_EQ("(where [true])", "[0]");
    TEST_ASSERT_EQ("(where [false])", "[]");
    // Where with comparison
    TEST_ASSERT_EQ("(where (> [1 2 3 4 5] 3))", "[3 4]");
    TEST_ASSERT_EQ("(where (== [1 2 3 2 1] 2))", "[1 3]");
    TEST_ASSERT_EQ("(where (< [10 20 30] 25))", "[0 1]");
    TEST_ASSERT_EQ("(where (>= [1 2 3 4 5] 3))", "[2 3 4]");
    // Where + at for filtering
    TEST_ASSERT_EQ("(at [10 20 30 40 50] (where (> [10 20 30 40 50] 25)))", "[30 40 50]");
    TEST_ASSERT_EQ("(at ['a 'b 'c 'd] (where [true false true false]))", "['a 'c]");
    // Where on all true/false
    TEST_ASSERT_EQ("(count (where (> (til 100) 50)))", "49");

    PASS();
}

// ==================== DISTINCT TESTS ====================
test_result_t test_vec_distinct() {
    // I64 distinct
    TEST_ASSERT_EQ("(distinct [1 2 3 2 1])", "[1 2 3]");
    TEST_ASSERT_EQ("(distinct [1 1 1 1])", "[1]");
    TEST_ASSERT_EQ("(distinct [5])", "[5]");
    TEST_ASSERT_EQ("(distinct [])", "[]");
    // Already distinct
    TEST_ASSERT_EQ("(distinct [1 2 3 4 5])", "[1 2 3 4 5]");
    // I32 distinct
    TEST_ASSERT_EQ("(distinct [1i 2i 1i 3i])", "[1i 2i 3i]");
    // I16 distinct
    TEST_ASSERT_EQ("(distinct [1h 2h 2h])", "[1 2]");
    // Bool distinct (order: false first, then true)
    TEST_ASSERT_EQ("(distinct [true true false true])", "[false true]");
    TEST_ASSERT_EQ("(distinct [true true])", "[true]");
    // Symbol distinct
    TEST_ASSERT_EQ("(distinct ['a 'b 'a 'c 'b])", "['a 'b 'c]");
    // Date distinct
    TEST_ASSERT_EQ("(distinct [2012.12.12 2012.12.12])", "[2012.12.12]");
    // Time distinct
    TEST_ASSERT_EQ("(distinct [10:00:00.000 20:10:10.500 10:00:00.000])", "[10:00:00.000 20:10:10.500]");
    // Timestamp distinct
    TEST_ASSERT_EQ("(distinct [2024.01.01D10:00:00.000000000 2024.01.01D10:00:00.000000000])",
                   "[2024.01.01D10:00:00.000000000]");
    // Byte distinct
    TEST_ASSERT_EQ("(distinct [0x12 0x12 0x10])", "[0x10 0x12]");
    // With null - null is dropped
    TEST_ASSERT_EQ("(distinct [1i 0Ni 1i])", "[1i]");
    // Count after distinct
    TEST_ASSERT_EQ("(count (distinct [1 1 2 2 3 3 4 4 5 5]))", "5");
    // List distinct
    TEST_ASSERT_EQ("(distinct (list [3i 3i] 2i [3i 3i] 2i))", "(list 2i [3i 3i])");
    // String distinct
    TEST_ASSERT_EQ("(distinct \"aabbcc\")", "\"abc\"");
    // F64 distinct
    TEST_ASSERT_EQ("(distinct [1.0 2.0 1.0 3.0])", "[1.0 2.0 3.0]");
    TEST_ASSERT_EQ("(distinct [1.5 1.5 1.5])", "[1.5]");
    TEST_ASSERT_EQ("(distinct [0.0 -0.0 1.0])", "[0.0 1.0]");

    PASS();
}

// ==================== GROUP TESTS ====================
test_result_t test_vec_group() {
    // Basic group
    TEST_ASSERT_EQ("(group [1 1 2 2 3])", "{1:[0 1] 2:[2 3] 3:[4]}");
    TEST_ASSERT_EQ("(group ['a 'a 'b 'b 'c])", "{a:[0 1] b:[2 3] c:[4]}");
    TEST_ASSERT_EQ("(group [true false true false])", "{true:[0 2] false:[1 3]}");
    // Empty group
    TEST_ASSERT_EQ("(group [])", "{}");
    // All unique
    TEST_ASSERT_EQ("(group [1 2 3])", "{1:[0] 2:[1] 3:[2]}");
    // Single element
    TEST_ASSERT_EQ("(group [42])", "{42:[0]}");
    // All same
    TEST_ASSERT_EQ("(group [5 5 5 5])", "{5:[0 1 2 3]}");
    // Key/value of grouped dict
    TEST_ASSERT_EQ("(key (group [1 2 1]))", "[1 2]");
    TEST_ASSERT_EQ("(count (group [1 2 1 3]))", "3");
    // Group of string chars (keys are quoted chars)
    TEST_ASSERT_EQ("(count (group \"abba\"))", "2");
    TEST_ASSERT_EQ("(count (group \"\"))", "0");
    // Group list of strings
    TEST_ASSERT_EQ("(group (list \"apple\" \"banana\" \"apple\"))", "{\"apple\":[0 2] \"banana\":[1]}");

    PASS();
}

// ==================== IN TESTS ====================
test_result_t test_vec_in() {
    // Scalar in vector
    TEST_ASSERT_EQ("(in 3 [1 2 3 4 5])", "true");
    TEST_ASSERT_EQ("(in 6 [1 2 3 4 5])", "false");
    TEST_ASSERT_EQ("(in 1i [1i 2i 3i])", "true");
    TEST_ASSERT_EQ("(in 1.0 [1.0 2.0 3.0])", "true");
    TEST_ASSERT_EQ("(in 4.0 [1.0 2.0 3.0])", "false");
    // Vector in vector
    TEST_ASSERT_EQ("(in [1 3 5] [1 2 3])", "[true true false]");
    TEST_ASSERT_EQ("(in [1 2 3] [1 2 3])", "[true true true]");
    TEST_ASSERT_EQ("(in [4 5 6] [1 2 3])", "[false false false]");
    // In with empty
    TEST_ASSERT_EQ("(in 1 [])", "false");
    // Cross-type in
    TEST_ASSERT_EQ("(in 2h [1 2 3])", "true");
    TEST_ASSERT_EQ("(in 2i [1 2 3])", "true");
    TEST_ASSERT_EQ("(in [1h 2h 3h] [2 3])", "[false true true]");
    // Null in
    TEST_ASSERT_EQ("(in 0Nl [1 0Nl 3])", "true");
    TEST_ASSERT_EQ("(in 0Nl [1 2 3])", "false");
    TEST_ASSERT_EQ("(in [0 1 0Nl] 0Nl)", "[false false true]");
    // In with dates
    TEST_ASSERT_EQ("(in 2012.12.12 [2012.12.12 2012.12.13])", "true");
    // In with times
    TEST_ASSERT_EQ("(in 10:00:00.000 [10:00:00.000 20:10:10.500])", "true");
    // In with booleans
    TEST_ASSERT_EQ("(in true [true false])", "true");
    TEST_ASSERT_EQ("(in false [true])", "false");
    // Scalar in scalar
    TEST_ASSERT_EQ("(in 2 2)", "true");
    TEST_ASSERT_EQ("(in 2 3)", "false");

    PASS();
}

// ==================== EXCEPT TESTS ====================
test_result_t test_vec_except() {
    // Basic except
    TEST_ASSERT_EQ("(except [1 2 3 4 5] [2 4])", "[1 3 5]");
    TEST_ASSERT_EQ("(except [1 2 3] [1 2 3])", "[]");
    TEST_ASSERT_EQ("(except [1 2 3] [4 5 6])", "[1 2 3]");
    // Empty cases
    TEST_ASSERT_EQ("(except [] [1 2 3])", "[]");
    TEST_ASSERT_EQ("(except [1 2 3] [])", "[1 2 3]");
    TEST_ASSERT_EQ("(except [] [])", "[]");
    // Scalar except
    TEST_ASSERT_EQ("(except [1 2 3 4 5] 3)", "[1 2 4 5]");
    TEST_ASSERT_EQ("(except [1 2 3] 9)", "[1 2 3]");
    // Duplicates
    TEST_ASSERT_EQ("(except [1 1 2 2 3] [1 3])", "[2 2]");
    // Symbol except
    TEST_ASSERT_EQ("(except ['a 'b 'c] ['a 'c])", "[b]");
    TEST_ASSERT_EQ("(except ['a 'b 'c] ['x 'y])", "[a b c]");
    // Single element
    TEST_ASSERT_EQ("(except [42] [42])", "[]");
    TEST_ASSERT_EQ("(except [42] [99])", "[42]");

    PASS();
}

// ==================== INTER (SECT) TESTS ====================
test_result_t test_vec_sect() {
    // Basic intersection
    TEST_ASSERT_EQ("(sect [1 2 3 4] [2 4 6])", "[2 4]");
    TEST_ASSERT_EQ("(sect [1 2 3] [1 2 3])", "[1 2 3]");
    TEST_ASSERT_EQ("(sect [1 2 3] [4 5 6])", "[]");
    // Empty cases
    TEST_ASSERT_EQ("(sect [] [1 2 3])", "[]");
    TEST_ASSERT_EQ("(sect [1 2 3] [])", "[]");
    TEST_ASSERT_EQ("(sect [] [])", "[]");
    // Symbol intersection
    TEST_ASSERT_EQ("(sect ['a 'b 'c] ['b 'c 'd])", "[b c]");
    TEST_ASSERT_EQ("(sect ['a 'b] ['c 'd])", "[]");
    // Single element
    TEST_ASSERT_EQ("(sect [1] [1])", "[1]");
    TEST_ASSERT_EQ("(sect [1] [2])", "[]");
    // Order follows first argument
    TEST_ASSERT_EQ("(sect [3 1 2] [2 3])", "[3 2]");

    PASS();
}

// ==================== UNION TESTS ====================
test_result_t test_vec_union() {
    // Basic union
    TEST_ASSERT_EQ("(union [1 2 3] [3 4 5])", "[1 2 3 4 5]");
    TEST_ASSERT_EQ("(union [1 2 3] [1 2 3])", "[1 2 3]");
    // Empty cases
    TEST_ASSERT_EQ("(union [] [1 2 3])", "[1 2 3]");
    TEST_ASSERT_EQ("(union [1 2 3] [])", "[1 2 3]");
    TEST_ASSERT_EQ("(union [] [])", "[]");
    // No overlap
    TEST_ASSERT_EQ("(union [1 2] [3 4])", "[1 2 3 4]");
    // Symbol union
    TEST_ASSERT_EQ("(union ['a 'b] ['b 'c])", "[a b c]");
    TEST_ASSERT_EQ("(union ['a] ['b])", "[a b]");
    // Single elements
    TEST_ASSERT_EQ("(union [1] [1])", "[1]");
    TEST_ASSERT_EQ("(union [1] [2])", "[1 2]");

    PASS();
}

// ==================== RAZE TESTS ====================
test_result_t test_vec_raze() {
    // Basic raze
    TEST_ASSERT_EQ("(raze (list [1 2] [3 4]))", "[1 2 3 4]");
    TEST_ASSERT_EQ("(raze (list [1 2] [3 4] [5 6]))", "[1 2 3 4 5 6]");
    // Mixed types in raze
    TEST_ASSERT_EQ("(raze (list [1 2] [3.0 4.0]))", "(list 1 2 3.0 4.0)");
    // Raze nested list
    TEST_ASSERT_EQ("(raze (list [1 2] (list 3 4)))", "[1 2 3 4]");
    TEST_ASSERT_EQ("(raze (list (list 1 2) (list 3 4)))", "[1 2 3 4]");
    // Raze empty
    TEST_ASSERT_EQ("(raze (list))", "()");
    // Raze single element
    TEST_ASSERT_EQ("(raze (list [1 2 3]))", "[1 2 3]");
    // Raze atom
    TEST_ASSERT_EQ("(raze 42)", "42");
    // Raze strings
    TEST_ASSERT_EQ("(raze (list \"hel\" \"lo\"))", "\"hello\"");
    TEST_ASSERT_EQ("(raze (list \"\" \"\"))", "\"\"");
    // Count after raze
    TEST_ASSERT_EQ("(count (raze (list [1 2] [3 4 5])))", "5");
    // Raze + til
    TEST_ASSERT_EQ("(raze (list (til 3) (til 2)))", "[0 1 2 0 1]");

    PASS();
}

// ==================== ENLIST TESTS ====================
test_result_t test_vec_enlist() {
    // Enlist atom
    TEST_ASSERT_EQ("(enlist 5)", "[5]");
    TEST_ASSERT_EQ("(enlist 'a)", "['a]");
    TEST_ASSERT_EQ("(enlist true)", "[true]");
    TEST_ASSERT_EQ("(enlist 1i)", "[1i]");
    TEST_ASSERT_EQ("(enlist 1.0)", "[1.0]");
    // Enlist vector -> list of 1 vector
    TEST_ASSERT_EQ("(enlist [1 2 3])", "(list [1 2 3])");
    TEST_ASSERT_EQ("(enlist \"hello\")", "(list \"hello\")");
    // Multi-arg enlist
    TEST_ASSERT_EQ("(enlist 1 2 3)", "[1 2 3]");
    TEST_ASSERT_EQ("(enlist 'a 'b 'c)", "['a 'b 'c]");
    TEST_ASSERT_EQ("(enlist true false)", "[true false]");
    // Mixed type enlist -> list
    TEST_ASSERT_EQ("(enlist 1 \"hello\" 'a)", "(list 1 \"hello\" 'a)");
    // Count
    TEST_ASSERT_EQ("(count (enlist 42))", "1");
    TEST_ASSERT_EQ("(count (enlist [1 2 3]))", "1");
    TEST_ASSERT_EQ("(count (enlist 1 2 3))", "3");

    PASS();
}

// ==================== REVERSE TESTS ====================
test_result_t test_vec_reverse() {
    // I64
    TEST_ASSERT_EQ("(reverse [1 2 3 4 5])", "[5 4 3 2 1]");
    TEST_ASSERT_EQ("(reverse [42])", "[42]");
    TEST_ASSERT_EQ("(reverse [])", "[]");
    // I32
    TEST_ASSERT_EQ("(reverse [1i 2i 3i])", "[3i 2i 1i]");
    // F64
    TEST_ASSERT_EQ("(reverse [1.0 2.0 3.0])", "[3.0 2.0 1.0]");
    // Bool
    TEST_ASSERT_EQ("(reverse [true false true])", "[true false true]");
    TEST_ASSERT_EQ("(reverse [true false])", "[false true]");
    // Bytes
    TEST_ASSERT_EQ("(reverse [0x01 0x02 0x03])", "[0x03 0x02 0x01]");
    // Symbols
    TEST_ASSERT_EQ("(reverse ['a 'b 'c])", "['c 'b 'a]");
    // String
    TEST_ASSERT_EQ("(reverse \"hello\")", "\"olleh\"");
    TEST_ASSERT_EQ("(reverse \"\")", "\"\"");
    // List
    TEST_ASSERT_EQ("(reverse (list 1 \"hello\" 'a))", "(list 'a \"hello\" 1)");
    // Double reverse = identity
    TEST_ASSERT_EQ("(reverse (reverse [1 2 3 4 5]))", "[1 2 3 4 5]");
    TEST_ASSERT_EQ("(reverse (reverse \"hello\"))", "\"hello\"");
    // Dates
    TEST_ASSERT_EQ("(reverse [2024.01.01 2024.01.02 2024.01.03])", "[2024.01.03 2024.01.02 2024.01.01]");
    // Times
    TEST_ASSERT_EQ("(reverse [10:00:00.000 20:00:00.000])", "[20:00:00.000 10:00:00.000]");

    PASS();
}

// ==================== COUNT TESTS ====================
test_result_t test_vec_count() {
    // Atom count = 1
    TEST_ASSERT_EQ("(count 42)", "1");
    TEST_ASSERT_EQ("(count 'a)", "1");
    TEST_ASSERT_EQ("(count true)", "1");
    TEST_ASSERT_EQ("(count 1.0)", "1");
    // Vector counts
    TEST_ASSERT_EQ("(count [])", "0");
    TEST_ASSERT_EQ("(count [1])", "1");
    TEST_ASSERT_EQ("(count [1 2 3 4 5])", "5");
    TEST_ASSERT_EQ("(count (til 100))", "100");
    // String counts
    TEST_ASSERT_EQ("(count \"\")", "0");
    TEST_ASSERT_EQ("(count \"hello\")", "5");
    // List counts
    TEST_ASSERT_EQ("(count (list))", "0");
    TEST_ASSERT_EQ("(count (list 1 2 3))", "3");
    TEST_ASSERT_EQ("(count (list [1 2] [3 4]))", "2");
    // Table count
    TEST_ASSERT_EQ("(count (table [a b] (list [1 2 3] [4 5 6])))", "3");
    // Dict count
    TEST_ASSERT_EQ("(count {a:1 b:2 c:3})", "3");
    // Symbol vector count
    TEST_ASSERT_EQ("(count ['a 'b 'c])", "3");
    // Different types
    TEST_ASSERT_EQ("(count [1i 2i 3i 4i])", "4");
    TEST_ASSERT_EQ("(count [1h 2h 3h])", "3");
    TEST_ASSERT_EQ("(count [1.0 2.0])", "2");
    TEST_ASSERT_EQ("(count [true false])", "2");
    TEST_ASSERT_EQ("(count [0x01 0x02 0x03 0x04])", "4");

    PASS();
}

// ==================== FIRST/LAST TESTS ====================
test_result_t test_vec_first_last() {
    // First on vectors
    TEST_ASSERT_EQ("(first [10 20 30])", "10");
    TEST_ASSERT_EQ("(first [42])", "42");
    TEST_ASSERT_EQ("(first [])", "0Nl");
    // Last on vectors
    TEST_ASSERT_EQ("(last [10 20 30])", "30");
    TEST_ASSERT_EQ("(last [42])", "42");
    TEST_ASSERT_EQ("(last [])", "0Nl");
    // First/last on different types
    TEST_ASSERT_EQ("(first [1i 2i 3i])", "1i");
    TEST_ASSERT_EQ("(last [1i 2i 3i])", "3i");
    TEST_ASSERT_EQ("(first [1.0 2.0 3.0])", "1.0");
    TEST_ASSERT_EQ("(last [1.0 2.0 3.0])", "3.0");
    TEST_ASSERT_EQ("(first [true false])", "true");
    TEST_ASSERT_EQ("(last [true false])", "false");
    TEST_ASSERT_EQ("(first ['a 'b 'c])", "'a");
    TEST_ASSERT_EQ("(last ['a 'b 'c])", "'c");
    TEST_ASSERT_EQ("(first [1h 2h 3h])", "1h");
    TEST_ASSERT_EQ("(last [1h 2h 3h])", "3h");
    // First/last on atom
    TEST_ASSERT_EQ("(first 42)", "42");
    TEST_ASSERT_EQ("(last 42)", "42");
    // First/last on string
    TEST_ASSERT_EQ("(first \"hello\")", "'h'");
    TEST_ASSERT_EQ("(last \"hello\")", "'o'");
    TEST_ASSERT_EQ("(first \"\")", "(as 'c8 0)");
    TEST_ASSERT_EQ("(last \"\")", "(as 'c8 0)");
    // First/last on list
    TEST_ASSERT_EQ("(first (list [1 2] [3 4]))", "[1 2]");
    TEST_ASSERT_EQ("(last (list [1 2] [3 4]))", "[3 4]");
    TEST_ASSERT_EQ("(first (list))", "null");
    TEST_ASSERT_EQ("(last (list))", "null");
    // First/last with dates
    TEST_ASSERT_EQ("(first [2024.01.01 2024.01.02])", "2024.01.01");
    TEST_ASSERT_EQ("(last [2024.01.01 2024.01.02])", "2024.01.02");

    PASS();
}

// ==================== FIND TESTS ====================
test_result_t test_vec_find() {
    // Basic find
    TEST_ASSERT_EQ("(find [10 20 30 40] 30)", "2");
    TEST_ASSERT_EQ("(find [10 20 30 40] 50)", "0Nl");
    TEST_ASSERT_EQ("(find [10 20 30 40] 10)", "0");
    TEST_ASSERT_EQ("(find [10 20 30 40] 40)", "3");
    // Find multiple
    TEST_ASSERT_EQ("(find [10 20 30 40] [20 40])", "[1 3]");
    TEST_ASSERT_EQ("(find [1 2 3] [4 2 5])", "[0Nl 1 0Nl]");
    // Find in empty
    TEST_ASSERT_EQ("(find [] 1)", "0Nl");
    TEST_ASSERT_EQ("(find [] [1 2])", "[]");
    // Find with symbols
    TEST_ASSERT_EQ("(find ['a 'b 'c] 'b)", "1");
    TEST_ASSERT_EQ("(find ['a 'b 'c] 'd)", "0Nl");
    TEST_ASSERT_EQ("(find ['apple 'banana 'cherry] 'banana)", "1");
    // Find in string (char find)
    TEST_ASSERT_EQ("(find \"hello\" 'l')", "2");
    TEST_ASSERT_EQ("(find \"hello\" 'z')", "0Nl");
    TEST_ASSERT_EQ("(find \"\" 'a')", "0Nl");
    // Find returns first occurrence
    TEST_ASSERT_EQ("(find [1 2 1 2 1] 1)", "0");
    TEST_ASSERT_EQ("(find [1 2 1 2 1] 2)", "1");
    // Large I64 find
    TEST_ASSERT_EQ("(find [1000000000 2000000000 3000000000] 2000000000)", "1");
    TEST_ASSERT_EQ("(find [1000000000 2000000000] 9999999999)", "0Nl");

    PASS();
}

// ==================== FILTER TESTS ====================
test_result_t test_vec_filter() {
    // Basic filter
    TEST_ASSERT_EQ("(filter [10 20 30 40 50] [true false true false true])", "[10 30 50]");
    TEST_ASSERT_EQ("(filter [10 20 30] [true true true])", "[10 20 30]");
    TEST_ASSERT_EQ("(filter [10 20 30] [false false false])", "[]");
    // Different types
    TEST_ASSERT_EQ("(filter [1i 2i 3i] [true false true])", "[1i 3i]");
    TEST_ASSERT_EQ("(filter [1.0 2.0 3.0] [false true false])", "[2.0]");
    TEST_ASSERT_EQ("(filter [true false true] [true true false])", "[true false]");
    TEST_ASSERT_EQ("(filter ['a 'b 'c] [false true false])", "['b]");
    TEST_ASSERT_EQ("(filter [1h 2h 3h] [true false true])", "[1h 3h]");
    // Filter string
    TEST_ASSERT_EQ("(filter \"hello\" [true false true false true])", "\"hlo\"");
    TEST_ASSERT_EQ("(filter \"abc\" [true true true])", "\"abc\"");
    // Filter with computed mask
    TEST_ASSERT_EQ("(filter [1 2 3 4 5] (> [1 2 3 4 5] 3))", "[4 5]");
    TEST_ASSERT_EQ("(filter [10 20 30 40] (< [10 20 30 40] 25))", "[10 20]");
    // Filter on list
    TEST_ASSERT_EQ("(filter (list [1 2] [3 4] [5 6]) [true false true])", "(list [1 2] [5 6])");
    // Filter dates
    TEST_ASSERT_EQ("(filter [2024.01.01 2024.01.02] [true false])", "[2024.01.01]");
    // Filter with null
    TEST_ASSERT_EQ("(filter [1 0Nl 3] [true true false])", "[1 0Nl]");
    // Length mismatch error
    TEST_ASSERT_ER("(filter [1 2 3] [true false])", "length");
    // Type error
    TEST_ASSERT_ER("(filter [true false] [1 2])", "type");
    // Filter on table
    TEST_ASSERT_EQ("(first (filter (table [a b] (list [1 2 3] [4 5 6])) [false true false]))", "{a:2 b:5}");

    PASS();
}

// ==================== REMOVE TESTS ====================
test_result_t test_vec_remove() {
    // Remove by index
    TEST_ASSERT_EQ("(remove [1 2 3 4 5] 0)", "[2 3 4 5]");
    TEST_ASSERT_EQ("(remove [1 2 3 4 5] 2)", "[1 2 4 5]");
    TEST_ASSERT_EQ("(remove [1 2 3 4 5] 4)", "[1 2 3 4]");
    // Remove from single element
    TEST_ASSERT_EQ("(remove [42] 0)", "[]");
    // Remove different types
    TEST_ASSERT_EQ("(remove [1i 2i 3i] 1)", "[1i 3i]");
    TEST_ASSERT_EQ("(remove [1.0 2.0 3.0] 0)", "[2.0 3.0]");
    TEST_ASSERT_EQ("(remove ['a 'b 'c] 1)", "['a 'c]");
    TEST_ASSERT_EQ("(remove [true false true] 1)", "[true true]");
    // Remove from string
    TEST_ASSERT_EQ("(remove \"hello\" 0)", "\"ello\"");
    TEST_ASSERT_EQ("(remove \"hello\" 4)", "\"hell\"");
    TEST_ASSERT_EQ("(remove \"hello\" 2)", "\"helo\"");
    // Count after remove
    TEST_ASSERT_EQ("(count (remove [1 2 3 4 5] 0))", "4");

    PASS();
}

// ==================== WITHIN TESTS ====================
test_result_t test_vec_within() {
    // Basic within
    TEST_ASSERT_EQ("(within [5] [1 10])", "[true]");
    TEST_ASSERT_EQ("(within [0] [1 10])", "[false]");
    TEST_ASSERT_EQ("(within [11] [1 10])", "[false]");
    // Multiple values
    TEST_ASSERT_EQ("(within [5 0 15] [1 10])", "[true false false]");
    // Boundary: both inclusive
    TEST_ASSERT_EQ("(within [1] [1 10])", "[true]");
    TEST_ASSERT_EQ("(within [10] [1 10])", "[true]");
    // Non-I64 within gives type error
    TEST_ASSERT_ER("(within [2024.01.15] [2024.01.01 2024.02.01])", "type");
    TEST_ASSERT_ER("(within [1.5] [1.0 2.0])", "type");

    PASS();
}

// ==================== COMBINED VECTOR OPERATIONS ====================
test_result_t test_vec_combined() {
    // Til + take
    TEST_ASSERT_EQ("(take (til 10) 5)", "[0 1 2 3 4]");
    TEST_ASSERT_EQ("(take (til 10) -3)", "[7 8 9]");
    // Where + at
    TEST_ASSERT_EQ("(at (til 10) (where (> (til 10) 5)))", "[6 7 8 9]");
    // Distinct + count
    TEST_ASSERT_EQ("(count (distinct [1 1 2 2 3 3]))", "3");
    // Union should be distinct
    TEST_ASSERT_EQ("(union [1 2 3] [2 3 4])", "[1 2 3 4]");
    // Except + count
    TEST_ASSERT_EQ("(count (except [1 2 3 4 5] [2 4]))", "3");
    // Enlist + first = identity for atoms
    TEST_ASSERT_EQ("(first (enlist 42))", "42");
    // Concat + count
    TEST_ASSERT_EQ("(count (concat [1 2 3] [4 5]))", "5");
    // Filter + sum
    TEST_ASSERT_EQ("(sum (filter [1 2 3 4 5] (> [1 2 3 4 5] 3)))", "9");
    // Nested til + raze
    TEST_ASSERT_EQ("(raze (list (til 3) (+ 10 (til 3))))", "[0 1 2 10 11 12]");
    // Group + key
    TEST_ASSERT_EQ("(key (group [1 2 1 2 3]))", "[1 2 3]");
    // Find + at
    TEST_ASSERT_EQ("(at [10 20 30] (find [10 20 30] 20))", "20");
    // Complex chain
    TEST_ASSERT_EQ("(sum (distinct (concat [1 2 3] [3 4 5])))", "15");
    // Sect preserves first arg order
    TEST_ASSERT_EQ("(sect [3 1 2] [2 3])", "[3 2]");

    PASS();
}
