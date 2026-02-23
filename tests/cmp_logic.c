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

// ==================== I16 COMPARISON OPERATORS ====================
test_result_t test_cmp_i16() {
    // i16 scalar-scalar all operators
    TEST_ASSERT_EQ("(== 5h 5h)", "true");
    TEST_ASSERT_EQ("(== 5h 6h)", "false");
    TEST_ASSERT_EQ("(!= 5h 6h)", "true");
    TEST_ASSERT_EQ("(!= 5h 5h)", "false");
    TEST_ASSERT_EQ("(< 5h 6h)", "true");
    TEST_ASSERT_EQ("(< 6h 5h)", "false");
    TEST_ASSERT_EQ("(< 5h 5h)", "false");
    TEST_ASSERT_EQ("(> 6h 5h)", "true");
    TEST_ASSERT_EQ("(> 5h 6h)", "false");
    TEST_ASSERT_EQ("(> 5h 5h)", "false");
    TEST_ASSERT_EQ("(<= 5h 5h)", "true");
    TEST_ASSERT_EQ("(<= 5h 6h)", "true");
    TEST_ASSERT_EQ("(<= 6h 5h)", "false");
    TEST_ASSERT_EQ("(>= 5h 5h)", "true");
    TEST_ASSERT_EQ("(>= 6h 5h)", "true");
    TEST_ASSERT_EQ("(>= 5h 6h)", "false");

    // i16 negative values
    TEST_ASSERT_EQ("(< -5h 5h)", "true");
    TEST_ASSERT_EQ("(> 5h -5h)", "true");
    TEST_ASSERT_EQ("(== -5h -5h)", "true");
    TEST_ASSERT_EQ("(!= -5h 5h)", "true");

    // i16 with zero
    TEST_ASSERT_EQ("(== 0h 0h)", "true");
    TEST_ASSERT_EQ("(< -1h 0h)", "true");
    TEST_ASSERT_EQ("(> 1h 0h)", "true");

    // i16 null comparisons
    TEST_ASSERT_EQ("(== 0Nh 0Nh)", "true");
    TEST_ASSERT_EQ("(== 0Nh 5h)", "false");
    TEST_ASSERT_EQ("(!= 0Nh 5h)", "true");
    TEST_ASSERT_EQ("(< 0Nh 5h)", "true");
    TEST_ASSERT_EQ("(> 5h 0Nh)", "true");

    // i16 vector comparisons
    TEST_ASSERT_EQ("(== [1h 2h 3h] [1h 3h 3h])", "[true false true]");
    TEST_ASSERT_EQ("(!= [1h 2h 3h] [1h 3h 3h])", "[false true false]");
    TEST_ASSERT_EQ("(< [1h 2h 3h] [2h 2h 2h])", "[true false false]");
    TEST_ASSERT_EQ("(> [3h 2h 1h] [2h 2h 2h])", "[true false false]");
    TEST_ASSERT_EQ("(<= [1h 2h 3h] [2h 2h 2h])", "[true true false]");
    TEST_ASSERT_EQ("(>= [3h 2h 1h] [2h 2h 2h])", "[true true false]");

    // i16 scalar-vector
    TEST_ASSERT_EQ("(== 2h [1h 2h 3h])", "[false true false]");
    TEST_ASSERT_EQ("(< 2h [1h 2h 3h])", "[false false true]");
    TEST_ASSERT_EQ("(> 2h [1h 2h 3h])", "[true false false]");

    // i16 vector-scalar
    TEST_ASSERT_EQ("(== [1h 2h 3h] 2h)", "[false true false]");
    TEST_ASSERT_EQ("(< [1h 2h 3h] 2h)", "[true false false]");
    TEST_ASSERT_EQ("(> [1h 2h 3h] 2h)", "[false false true]");

    // i16 null in vector
    TEST_ASSERT_EQ("(== [0Nh 2h 0Nh] [0Nh 2h 3h])", "[true true false]");

    PASS();
}

// ==================== I32 COMPARISON OPERATORS ====================
test_result_t test_cmp_i32() {
    // i32 scalar-scalar all operators
    TEST_ASSERT_EQ("(== 5i 5i)", "true");
    TEST_ASSERT_EQ("(== 5i 6i)", "false");
    TEST_ASSERT_EQ("(!= 5i 6i)", "true");
    TEST_ASSERT_EQ("(!= 5i 5i)", "false");
    TEST_ASSERT_EQ("(< 5i 6i)", "true");
    TEST_ASSERT_EQ("(< 6i 5i)", "false");
    TEST_ASSERT_EQ("(> 6i 5i)", "true");
    TEST_ASSERT_EQ("(> 5i 6i)", "false");
    TEST_ASSERT_EQ("(<= 5i 5i)", "true");
    TEST_ASSERT_EQ("(<= 5i 6i)", "true");
    TEST_ASSERT_EQ("(<= 6i 5i)", "false");
    TEST_ASSERT_EQ("(>= 5i 5i)", "true");
    TEST_ASSERT_EQ("(>= 6i 5i)", "true");
    TEST_ASSERT_EQ("(>= 5i 6i)", "false");

    // i32 negative values
    TEST_ASSERT_EQ("(< -100i 100i)", "true");
    TEST_ASSERT_EQ("(> 100i -100i)", "true");
    TEST_ASSERT_EQ("(== -100i -100i)", "true");

    // i32 null comparisons
    TEST_ASSERT_EQ("(== 0Ni 0Ni)", "true");
    TEST_ASSERT_EQ("(== 0Ni 5i)", "false");
    TEST_ASSERT_EQ("(!= 0Ni 5i)", "true");
    TEST_ASSERT_EQ("(< 0Ni 5i)", "true");
    TEST_ASSERT_EQ("(> 5i 0Ni)", "true");

    // i32 vector comparisons
    TEST_ASSERT_EQ("(== [1i 2i 3i] [1i 3i 3i])", "[true false true]");
    TEST_ASSERT_EQ("(!= [1i 2i 3i] [1i 3i 3i])", "[false true false]");
    TEST_ASSERT_EQ("(< [1i 2i 3i] [2i 2i 2i])", "[true false false]");
    TEST_ASSERT_EQ("(> [3i 2i 1i] [2i 2i 2i])", "[true false false]");

    // i32 scalar-vector
    TEST_ASSERT_EQ("(== 2i [1i 2i 3i])", "[false true false]");
    TEST_ASSERT_EQ("(< 2i [1i 2i 3i])", "[false false true]");

    // i32 vector-scalar
    TEST_ASSERT_EQ("(== [1i 2i 3i] 2i)", "[false true false]");
    TEST_ASSERT_EQ("(> [1i 2i 3i] 2i)", "[false false true]");

    // i32 null in vector
    TEST_ASSERT_EQ("(== [0Ni 2i 0Ni] [0Ni 2i 3i])", "[true true false]");
    TEST_ASSERT_EQ("(!= [0Ni 2i] [0Ni 3i])", "[false true]");

    PASS();
}

// ==================== I64 COMPARISON OPERATORS ====================
test_result_t test_cmp_i64() {
    // i64 basic comparisons
    TEST_ASSERT_EQ("(== 100 100)", "true");
    TEST_ASSERT_EQ("(== 100 101)", "false");
    TEST_ASSERT_EQ("(!= 100 101)", "true");
    TEST_ASSERT_EQ("(< -100 100)", "true");
    TEST_ASSERT_EQ("(> 100 -100)", "true");
    TEST_ASSERT_EQ("(<= 100 100)", "true");
    TEST_ASSERT_EQ("(>= 100 100)", "true");

    // i64 null comparisons
    TEST_ASSERT_EQ("(== 0Nl 0Nl)", "true");
    TEST_ASSERT_EQ("(== 0Nl 5)", "false");
    TEST_ASSERT_EQ("(!= 0Nl 5)", "true");
    TEST_ASSERT_EQ("(< 0Nl 5)", "true");
    TEST_ASSERT_EQ("(> 5 0Nl)", "true");

    // i64 vector comparisons
    TEST_ASSERT_EQ("(== [1 2 3] [1 3 3])", "[true false true]");
    TEST_ASSERT_EQ("(!= [1 2 3] [1 3 3])", "[false true false]");
    TEST_ASSERT_EQ("(< [1 2 3] [2 2 2])", "[true false false]");
    TEST_ASSERT_EQ("(> [3 2 1] [2 2 2])", "[true false false]");
    TEST_ASSERT_EQ("(<= [1 2 3] [2 2 2])", "[true true false]");
    TEST_ASSERT_EQ("(>= [3 2 1] [2 2 2])", "[true true false]");

    // i64 null in vector
    TEST_ASSERT_EQ("(== [0Nl 2 0Nl] [0Nl 2 3])", "[true true false]");
    TEST_ASSERT_EQ("(!= [0Nl 1] [0Nl 2])", "[false true]");

    // Large value comparisons
    TEST_ASSERT_EQ("(== 1000000000000 1000000000000)", "true");
    TEST_ASSERT_EQ("(< 999999999999 1000000000000)", "true");
    TEST_ASSERT_EQ("(> 1000000000001 1000000000000)", "true");

    PASS();
}

// ==================== F64 COMPARISON OPERATORS ====================
test_result_t test_cmp_f64() {
    // f64 basic comparisons
    TEST_ASSERT_EQ("(== 1.5 1.5)", "true");
    TEST_ASSERT_EQ("(== 1.5 2.5)", "false");
    TEST_ASSERT_EQ("(!= 1.5 2.5)", "true");
    TEST_ASSERT_EQ("(< 1.5 2.5)", "true");
    TEST_ASSERT_EQ("(> 2.5 1.5)", "true");
    TEST_ASSERT_EQ("(<= 1.5 1.5)", "true");
    TEST_ASSERT_EQ("(>= 1.5 1.5)", "true");

    // f64 negative values
    TEST_ASSERT_EQ("(< -1.5 1.5)", "true");
    TEST_ASSERT_EQ("(> 1.5 -1.5)", "true");
    TEST_ASSERT_EQ("(== -1.5 -1.5)", "true");

    // f64 zero comparisons
    TEST_ASSERT_EQ("(== 0.0 0.0)", "true");
    TEST_ASSERT_EQ("(== -0.0 0.0)", "true");
    TEST_ASSERT_EQ("(== 0.0 -0.0)", "true");
    TEST_ASSERT_EQ("(< -0.0 0.0)", "false");

    // f64 null comparisons
    TEST_ASSERT_EQ("(== 0Nf 0Nf)", "true");
    TEST_ASSERT_EQ("(== 0Nf 1.0)", "false");
    TEST_ASSERT_EQ("(!= 0Nf 1.0)", "true");
    TEST_ASSERT_EQ("(< 0Nf 1.0)", "true");
    TEST_ASSERT_EQ("(> 1.0 0Nf)", "true");

    // f64 vector comparisons
    TEST_ASSERT_EQ("(== [1.0 2.0 3.0] [1.0 3.0 3.0])", "[true false true]");
    TEST_ASSERT_EQ("(!= [1.0 2.0 3.0] [1.0 3.0 3.0])", "[false true false]");
    TEST_ASSERT_EQ("(< [1.0 2.0 3.0] [2.0 2.0 2.0])", "[true false false]");
    TEST_ASSERT_EQ("(> [3.0 2.0 1.0] [2.0 2.0 2.0])", "[true false false]");

    // f64 scalar-vector
    TEST_ASSERT_EQ("(== 2.0 [1.0 2.0 3.0])", "[false true false]");
    TEST_ASSERT_EQ("(< 2.0 [1.0 2.0 3.0])", "[false false true]");

    // f64 null in vector
    TEST_ASSERT_EQ("(== [0Nf 2.0 0Nf] [0Nf 2.0 3.0])", "[true true false]");
    TEST_ASSERT_EQ("(!= [0Nf 1.0] [0Nf 2.0])", "[false true]");

    PASS();
}

// ==================== MIXED TYPE COMPARISONS ====================
test_result_t test_cmp_mixed_types() {
    // i16 vs i32
    TEST_ASSERT_EQ("(== 5h 5i)", "true");
    TEST_ASSERT_EQ("(== 5h 6i)", "false");
    TEST_ASSERT_EQ("(< 5h 6i)", "true");
    TEST_ASSERT_EQ("(> 6h 5i)", "true");

    // i16 vs i64
    TEST_ASSERT_EQ("(== 5h 5)", "true");
    TEST_ASSERT_EQ("(< 5h 6)", "true");
    TEST_ASSERT_EQ("(> 6h 5)", "true");

    // i16 vs f64
    TEST_ASSERT_EQ("(== 5h 5.0)", "true");
    TEST_ASSERT_EQ("(< 5h 5.5)", "true");
    TEST_ASSERT_EQ("(> 6h 5.5)", "true");

    // i32 vs i64
    TEST_ASSERT_EQ("(== 5i 5)", "true");
    TEST_ASSERT_EQ("(< 5i 6)", "true");
    TEST_ASSERT_EQ("(> 6i 5)", "true");

    // i32 vs f64
    TEST_ASSERT_EQ("(== 5i 5.0)", "true");
    TEST_ASSERT_EQ("(< 5i 5.5)", "true");
    TEST_ASSERT_EQ("(> 6i 5.5)", "true");

    // i64 vs f64
    TEST_ASSERT_EQ("(== 5 5.0)", "true");
    TEST_ASSERT_EQ("(< 5 5.5)", "true");
    TEST_ASSERT_EQ("(> 6 5.5)", "true");

    // u8 comparisons
    TEST_ASSERT_EQ("(== 0x05 0x05)", "true");
    TEST_ASSERT_EQ("(!= 0x05 0x06)", "true");
    TEST_ASSERT_EQ("(< 0x05 0x06)", "true");
    TEST_ASSERT_EQ("(> 0x06 0x05)", "true");

    // u8 vs numeric
    TEST_ASSERT_EQ("(== 0x05 5i)", "true");
    TEST_ASSERT_EQ("(== 0x05 5)", "true");
    TEST_ASSERT_EQ("(== 0x05 5.0)", "true");

    // Mixed type vector comparisons
    TEST_ASSERT_EQ("(== [1h 2h 3h] [1i 2i 3i])", "[true true true]");
    TEST_ASSERT_EQ("(== [1i 2i 3i] [1 2 3])", "[true true true]");
    TEST_ASSERT_EQ("(== [1 2 3] [1.0 2.0 3.0])", "[true true true]");
    TEST_ASSERT_EQ("(< [1h 2h] [2i 1i])", "[true false]");
    TEST_ASSERT_EQ("(< [1i 2i] [2 1])", "[true false]");
    TEST_ASSERT_EQ("(< [1 2] [2.0 1.0])", "[true false]");

    // Mixed scalar-vector
    TEST_ASSERT_EQ("(== 5h [5i 6i 5i])", "[true false true]");
    TEST_ASSERT_EQ("(== 5i [5 6 5])", "[true false true]");
    TEST_ASSERT_EQ("(== 5 [5.0 6.0 5.0])", "[true false true]");

    PASS();
}

// ==================== NULL COMPARISON COMPREHENSIVE ====================
test_result_t test_cmp_null_comprehensive() {
    // null == null is true for all null types
    TEST_ASSERT_EQ("(== 0Nh 0Nh)", "true");
    TEST_ASSERT_EQ("(== 0Ni 0Ni)", "true");
    TEST_ASSERT_EQ("(== 0Nl 0Nl)", "true");
    TEST_ASSERT_EQ("(== 0Nf 0Nf)", "true");
    TEST_ASSERT_EQ("(== 0Nd 0Nd)", "true");
    TEST_ASSERT_EQ("(== 0Nt 0Nt)", "true");
    TEST_ASSERT_EQ("(== 0Np 0Np)", "true");
    TEST_ASSERT_EQ("(== 0Ns 0Ns)", "true");

    // null != value is true for all types
    TEST_ASSERT_EQ("(!= 0Nh 5h)", "true");
    TEST_ASSERT_EQ("(!= 0Ni 5i)", "true");
    TEST_ASSERT_EQ("(!= 0Nl 5)", "true");
    TEST_ASSERT_EQ("(!= 0Nf 5.0)", "true");
    TEST_ASSERT_EQ("(!= 0Nd 2024.01.01)", "true");
    TEST_ASSERT_EQ("(!= 0Nt 10:00:00.000)", "true");
    TEST_ASSERT_EQ("(!= 0Np 2024.01.01D10:00:00.000000000)", "true");
    TEST_ASSERT_EQ("(!= 0Ns 'abc)", "true");

    // null < value (null is smallest)
    TEST_ASSERT_EQ("(< 0Nh 5h)", "true");
    TEST_ASSERT_EQ("(< 0Ni 5i)", "true");
    TEST_ASSERT_EQ("(< 0Nl 5)", "true");
    TEST_ASSERT_EQ("(< 0Nf 5.0)", "true");

    // value > null
    TEST_ASSERT_EQ("(> 5h 0Nh)", "true");
    TEST_ASSERT_EQ("(> 5i 0Ni)", "true");
    TEST_ASSERT_EQ("(> 5 0Nl)", "true");
    TEST_ASSERT_EQ("(> 5.0 0Nf)", "true");

    // Cross-type null comparisons
    TEST_ASSERT_EQ("(== 0Nh 0Ni)", "true");
    TEST_ASSERT_EQ("(== 0Ni 0Nl)", "true");
    TEST_ASSERT_EQ("(== 0Nl 0Nf)", "true");

    // null in vectors
    TEST_ASSERT_EQ("(== [0Nl 1 2] [0Nl 1 3])", "[true true false]");
    TEST_ASSERT_EQ("(== [0Ni 1i 2i] [0Ni 1i 3i])", "[true true false]");
    TEST_ASSERT_EQ("(== [0Nf 1.0 2.0] [0Nf 1.0 3.0])", "[true true false]");
    TEST_ASSERT_EQ("(!= [0Nl 1] [0Nl 2])", "[false true]");
    TEST_ASSERT_EQ("(< [0Nl 1 5] [0Nl 2 3])", "[false true false]");

    PASS();
}

// ==================== BOOLEAN LOGIC: AND ====================
test_result_t test_logic_and() {
    // Scalar-scalar
    TEST_ASSERT_EQ("(and true true)", "true");
    TEST_ASSERT_EQ("(and true false)", "false");
    TEST_ASSERT_EQ("(and false true)", "false");
    TEST_ASSERT_EQ("(and false false)", "false");

    // Vector-vector
    TEST_ASSERT_EQ("(and [true true false false] [true false true false])",
                   "[true false false false]");

    // Scalar-vector
    TEST_ASSERT_EQ("(and true [true false true])", "[true false true]");
    TEST_ASSERT_EQ("(and false [true false true])", "[false false false]");

    // Vector-scalar
    TEST_ASSERT_EQ("(and [true false true] true)", "[true false true]");
    TEST_ASSERT_EQ("(and [true false true] false)", "[false false false]");

    // Multi-argument and
    TEST_ASSERT_EQ("(and true true true)", "true");
    TEST_ASSERT_EQ("(and true true false)", "false");
    TEST_ASSERT_EQ("(and [true true false] [true false false] [true true true])",
                   "[true false false]");

    // Single element vector
    TEST_ASSERT_EQ("(and [true] [false])", "[false]");
    TEST_ASSERT_EQ("(and [true] [true])", "[true]");

    PASS();
}

// ==================== BOOLEAN LOGIC: OR ====================
test_result_t test_logic_or() {
    // Scalar-scalar
    TEST_ASSERT_EQ("(or true true)", "true");
    TEST_ASSERT_EQ("(or true false)", "true");
    TEST_ASSERT_EQ("(or false true)", "true");
    TEST_ASSERT_EQ("(or false false)", "false");

    // Vector-vector
    TEST_ASSERT_EQ("(or [true true false false] [true false true false])",
                   "[true true true false]");

    // Scalar-vector
    TEST_ASSERT_EQ("(or true [true false true])", "[true true true]");
    TEST_ASSERT_EQ("(or false [true false true])", "[true false true]");

    // Vector-scalar
    TEST_ASSERT_EQ("(or [true false true] true)", "[true true true]");
    TEST_ASSERT_EQ("(or [true false true] false)", "[true false true]");

    // Multi-argument or
    TEST_ASSERT_EQ("(or false false false)", "false");
    TEST_ASSERT_EQ("(or false false true)", "true");
    TEST_ASSERT_EQ("(or [false false true] [false true false] [false false false])",
                   "[false true true]");

    // Single element vector
    TEST_ASSERT_EQ("(or [false] [false])", "[false]");
    TEST_ASSERT_EQ("(or [false] [true])", "[true]");

    PASS();
}

// ==================== BOOLEAN LOGIC: NOT ====================
test_result_t test_logic_not() {
    // Scalar
    TEST_ASSERT_EQ("(not true)", "false");
    TEST_ASSERT_EQ("(not false)", "true");

    // Vector
    TEST_ASSERT_EQ("(not [true false true false])", "[false true false true]");
    TEST_ASSERT_EQ("(not [true true true])", "[false false false]");
    TEST_ASSERT_EQ("(not [false false false])", "[true true true]");

    // Single element
    TEST_ASSERT_EQ("(not [true])", "[false]");
    TEST_ASSERT_EQ("(not [false])", "[true]");

    // Double negation
    TEST_ASSERT_EQ("(not (not true))", "true");
    TEST_ASSERT_EQ("(not (not false))", "false");
    TEST_ASSERT_EQ("(not (not [true false]))", "[true false]");

    PASS();
}

// ==================== WHERE FUNCTION ON BOOLEANS ====================
test_result_t test_cmp_where() {
    // Basic where
    TEST_ASSERT_EQ("(where [true false true false true])", "[0 2 4]");
    TEST_ASSERT_EQ("(where [false false false])", "[]");
    TEST_ASSERT_EQ("(where [true true true])", "[0 1 2]");

    // Where on comparison results
    TEST_ASSERT_EQ("(where (> [5 3 8 1 9] 4))", "[0 2 4]");
    TEST_ASSERT_EQ("(where (== [1 2 3 2 1] 2))", "[1 3]");
    TEST_ASSERT_EQ("(where (< [1 2 3] 2))", "[0]");
    TEST_ASSERT_EQ("(where (<= [1 2 3] 2))", "[0 1]");
    TEST_ASSERT_EQ("(where (>= [1 2 3] 2))", "[1 2]");
    TEST_ASSERT_EQ("(where (!= [1 2 3] 2))", "[0 2]");

    // Where on combined boolean expressions
    TEST_ASSERT_EQ("(where (and (> [1 5 3 8 2] 2) (< [1 5 3 8 2] 6)))", "[1 2]");
    TEST_ASSERT_EQ("(where (or (< [1 5 3] 2) (> [1 5 3] 4)))", "[0 1]");

    // Where + indexing pattern
    TEST_ASSERT_EQ("(set v [10 20 30 40 50]) (at v (where (> v 25)))", "[30 40 50]");
    TEST_ASSERT_EQ("(set v [10 20 30 40 50]) (at v (where (== v 30)))", "[30]");
    TEST_ASSERT_EQ("(set v [10 20 30 40 50]) (at v (where (< v 25)))", "[10 20]");

    // Single element
    TEST_ASSERT_EQ("(where [true])", "[0]");
    TEST_ASSERT_EQ("(where [false])", "[]");

    // All true / all false
    TEST_ASSERT_EQ("(where [true true true true])", "[0 1 2 3]");
    TEST_ASSERT_EQ("(where [false false false false])", "[]");

    PASS();
}

// ==================== NESTED BOOLEAN EXPRESSIONS ====================
test_result_t test_cmp_nested_boolean() {
    // and + or combinations
    TEST_ASSERT_EQ("(and (or true false) (or false true))", "true");
    TEST_ASSERT_EQ("(and (or false false) (or true true))", "false");
    TEST_ASSERT_EQ("(or (and true false) (and true true))", "true");
    TEST_ASSERT_EQ("(or (and false false) (and false true))", "false");

    // not + and/or
    TEST_ASSERT_EQ("(not (and true true))", "false");
    TEST_ASSERT_EQ("(not (and true false))", "true");
    TEST_ASSERT_EQ("(not (or false false))", "true");
    TEST_ASSERT_EQ("(not (or true false))", "false");

    // De Morgan's laws (vector)
    TEST_ASSERT_EQ("(not (and [true false] [false true]))", "[true true]");
    TEST_ASSERT_EQ("(or (not [true false]) (not [false true]))", "[true true]");
    TEST_ASSERT_EQ("(not (or [true false] [false true]))", "[false false]");
    TEST_ASSERT_EQ("(and (not [true false]) (not [false true]))", "[false false]");

    // Complex nested with comparisons
    TEST_ASSERT_EQ("(and (> 5 3) (< 5 10))", "true");
    TEST_ASSERT_EQ("(and (> 5 3) (< 5 4))", "false");
    TEST_ASSERT_EQ("(or (< 5 3) (> 5 4))", "true");
    TEST_ASSERT_EQ("(or (< 5 3) (> 5 10))", "false");

    // Nested with vectors
    TEST_ASSERT_EQ("(and (> [5 3 8] 4) (< [5 3 8] 7))", "[true false false]");
    TEST_ASSERT_EQ("(or (< [5 3 8] 4) (> [5 3 8] 7))", "[false true true]");
    TEST_ASSERT_EQ("(not (== [1 2 3] [1 3 3]))", "[false true false]");

    // Triple nesting
    TEST_ASSERT_EQ("(and (or (> 5 3) (< 2 1)) (not (== 3 4)))", "true");

    PASS();
}

// ==================== SYMBOL COMPARISON EDGE CASES ====================
test_result_t test_cmp_symbol() {
    // Basic symbol equality
    TEST_ASSERT_EQ("(== 'abc 'abc)", "true");
    TEST_ASSERT_EQ("(== 'abc 'def)", "false");
    TEST_ASSERT_EQ("(!= 'abc 'def)", "true");
    TEST_ASSERT_EQ("(!= 'abc 'abc)", "false");

    // Symbol ordering
    TEST_ASSERT_EQ("(< 'a 'b)", "true");
    TEST_ASSERT_EQ("(> 'b 'a)", "true");
    TEST_ASSERT_EQ("(<= 'a 'a)", "true");
    TEST_ASSERT_EQ("(>= 'b 'a)", "true");

    // Symbol null
    TEST_ASSERT_EQ("(== 0Ns 0Ns)", "true");
    TEST_ASSERT_EQ("(!= 0Ns 'abc)", "true");

    // Symbol vector comparisons
    TEST_ASSERT_EQ("(== ['a 'b 'c] ['a 'c 'c])", "[true false true]");
    TEST_ASSERT_EQ("(!= ['a 'b 'c] ['a 'c 'c])", "[false true false]");

    // Symbol scalar-vector
    TEST_ASSERT_EQ("(== 'b ['a 'b 'c])", "[false true false]");
    TEST_ASSERT_EQ("(!= 'b ['a 'b 'c])", "[true false true]");

    // Symbol vector-scalar
    TEST_ASSERT_EQ("(== ['a 'b 'c] 'b)", "[false true false]");

    // Multi-character symbols
    TEST_ASSERT_EQ("(== 'hello 'hello)", "true");
    TEST_ASSERT_EQ("(== 'hello 'world)", "false");
    TEST_ASSERT_EQ("(< 'abc 'abd)", "true");
    TEST_ASSERT_EQ("(< 'ab 'abc)", "false");

    PASS();
}

// ==================== STRING COMPARISON EDGE CASES ====================
test_result_t test_cmp_string() {
    // String equality
    TEST_ASSERT_EQ("(== \"hello\" \"hello\")", "true");
    TEST_ASSERT_EQ("(== \"hello\" \"world\")", "false");
    TEST_ASSERT_EQ("(!= \"hello\" \"world\")", "true");
    TEST_ASSERT_EQ("(!= \"hello\" \"hello\")", "false");

    // String ordering
    TEST_ASSERT_EQ("(< \"abc\" \"abd\")", "true");
    TEST_ASSERT_EQ("(> \"abd\" \"abc\")", "true");
    TEST_ASSERT_EQ("(<= \"abc\" \"abc\")", "true");
    TEST_ASSERT_EQ("(>= \"abc\" \"abc\")", "true");

    // Empty string comparisons
    TEST_ASSERT_EQ("(== \"\" \"\")", "true");
    TEST_ASSERT_EQ("(!= \"\" \"a\")", "true");
    TEST_ASSERT_EQ("(< \"\" \"a\")", "true");
    TEST_ASSERT_EQ("(> \"a\" \"\")", "true");

    // Prefix comparisons
    TEST_ASSERT_EQ("(< \"ab\" \"abc\")", "true");
    TEST_ASSERT_EQ("(> \"abc\" \"ab\")", "true");

    // Case sensitivity
    TEST_ASSERT_EQ("(< \"A\" \"a\")", "true");
    TEST_ASSERT_EQ("(> \"a\" \"A\")", "true");
    TEST_ASSERT_EQ("(== \"A\" \"a\")", "false");

    // Character vs string comparisons
    TEST_ASSERT_EQ("(== 'a' \"a\")", "true");
    TEST_ASSERT_EQ("(== 'a' \"b\")", "false");
    TEST_ASSERT_EQ("(< 'a' \"b\")", "true");
    TEST_ASSERT_EQ("(> 'b' \"a\")", "true");
    TEST_ASSERT_EQ("(== \"a\" 'a')", "true");
    TEST_ASSERT_EQ("(!= \"a\" 'b')", "true");

    PASS();
}

// ==================== DATE/TIME COMPARISON EDGE CASES ====================
test_result_t test_cmp_temporal() {
    // Date comparisons
    TEST_ASSERT_EQ("(== 2024.01.01 2024.01.01)", "true");
    TEST_ASSERT_EQ("(< 2024.01.01 2024.01.02)", "true");
    TEST_ASSERT_EQ("(> 2024.12.31 2024.01.01)", "true");
    TEST_ASSERT_EQ("(<= 2024.01.01 2024.01.01)", "true");
    TEST_ASSERT_EQ("(>= 2024.01.01 2024.01.01)", "true");

    // Date null comparisons
    TEST_ASSERT_EQ("(== 0Nd 0Nd)", "true");
    TEST_ASSERT_EQ("(!= 0Nd 2024.01.01)", "true");
    TEST_ASSERT_EQ("(< 0Nd 2024.01.01)", "true");

    // Date vector comparisons
    TEST_ASSERT_EQ("(== [2024.01.01 2024.01.02] [2024.01.01 2024.01.03])",
                   "[true false]");
    TEST_ASSERT_EQ("(< [2024.01.01 2024.01.02] [2024.01.02 2024.01.01])",
                   "[true false]");

    // Date scalar-vector
    TEST_ASSERT_EQ("(< 2024.01.15 [2024.01.01 2024.01.20 2024.01.15])",
                   "[false true false]");
    TEST_ASSERT_EQ("(> [2024.01.20 2024.01.01] 2024.01.15)",
                   "[true false]");

    // Time comparisons
    TEST_ASSERT_EQ("(== 10:30:00.000 10:30:00.000)", "true");
    TEST_ASSERT_EQ("(< 10:30:00.000 11:00:00.000)", "true");
    TEST_ASSERT_EQ("(> 23:59:59.999 00:00:00.000)", "true");

    // Time null
    TEST_ASSERT_EQ("(== 0Nt 0Nt)", "true");
    TEST_ASSERT_EQ("(!= 0Nt 10:00:00.000)", "true");

    // Time vector comparisons
    TEST_ASSERT_EQ("(== [10:00:00.000 11:00:00.000] [10:00:00.000 12:00:00.000])",
                   "[true false]");
    TEST_ASSERT_EQ("(< [10:00:00.000 11:00:00.000] [11:00:00.000 10:00:00.000])",
                   "[true false]");

    // Timestamp comparisons
    TEST_ASSERT_EQ("(== 2024.01.01D10:00:00.000000000 2024.01.01D10:00:00.000000000)", "true");
    TEST_ASSERT_EQ("(< 2024.01.01D10:00:00.000000000 2024.01.01D10:00:01.000000000)", "true");
    TEST_ASSERT_EQ("(> 2024.01.02D00:00:00.000000000 2024.01.01D23:59:59.000000000)", "true");

    // Timestamp null
    TEST_ASSERT_EQ("(== 0Np 0Np)", "true");
    TEST_ASSERT_EQ("(!= 0Np 2024.01.01D10:00:00.000000000)", "true");

    // Timestamp vector comparisons
    TEST_ASSERT_EQ("(== [2024.01.01D10:00:00.000000000] [2024.01.01D10:00:00.000000000])", "[true]");
    TEST_ASSERT_EQ("(< [2024.01.01D10:00:00.000000000] [2024.01.01D10:00:01.000000000])", "[true]");

    // Cross-temporal comparisons (date vs timestamp)
    // LT/GT: date promoted to midnight timestamp (precise ordering)
    TEST_ASSERT_EQ("(< 2024.01.01 2024.01.01D10:00:00.000000000)", "true");
    TEST_ASSERT_EQ("(> 2024.01.01D10:00:00.000000000 2024.01.01)", "true");
    TEST_ASSERT_EQ("(< [2024.01.01] [2024.01.01D10:00:00.000000000])", "[true]");
    // EQ/NE: timestamp truncated to date (same-day semantics)
    TEST_ASSERT_EQ("(== 2024.01.01D10:30:00.000000000 2024.01.01)", "true");
    TEST_ASSERT_EQ("(== 2024.01.01D23:59:59.999999999 2024.01.01)", "true");
    TEST_ASSERT_EQ("(== 2024.01.01D00:00:00.000000000 2024.01.01)", "true");
    TEST_ASSERT_EQ("(== 2024.01.02D00:00:00.000000000 2024.01.01)", "false");
    TEST_ASSERT_EQ("(!= 2024.01.01D10:30:00.000000000 2024.01.02)", "true");
    TEST_ASSERT_EQ("(== 2024.01.01 2024.01.01D10:30:00.000000000)", "true");
    // Vector: timestamp column filtered by date
    TEST_ASSERT_EQ("(== [2024.01.01D10:00:00.000000000 2024.01.02D05:00:00.000000000] 2024.01.01)", "[true false]");
    TEST_ASSERT_EQ("(== 2024.01.01 [2024.01.01D10:00:00.000000000 2024.01.02D05:00:00.000000000])", "[true false]");

    PASS();
}

// ==================== BOOLEAN-NUMERIC COMPARISON ====================
test_result_t test_cmp_bool_numeric() {
    // bool == numeric
    TEST_ASSERT_EQ("(== true 1)", "true");
    TEST_ASSERT_EQ("(== false 0)", "true");
    TEST_ASSERT_EQ("(== true 0)", "false");
    TEST_ASSERT_EQ("(== false 1)", "false");
    TEST_ASSERT_EQ("(== true 1.0)", "true");
    TEST_ASSERT_EQ("(== false 0.0)", "true");
    TEST_ASSERT_EQ("(== true 1i)", "true");
    TEST_ASSERT_EQ("(== false 0i)", "true");

    // numeric == bool
    TEST_ASSERT_EQ("(== 1 true)", "true");
    TEST_ASSERT_EQ("(== 0 false)", "true");
    TEST_ASSERT_EQ("(== 1.0 true)", "true");
    TEST_ASSERT_EQ("(== 0.0 false)", "true");

    // bool != numeric
    TEST_ASSERT_EQ("(!= true 0)", "true");
    TEST_ASSERT_EQ("(!= false 1)", "true");
    TEST_ASSERT_EQ("(!= true 2)", "true");

    // bool < numeric
    TEST_ASSERT_EQ("(< false true)", "true");
    TEST_ASSERT_EQ("(< true false)", "false");
    TEST_ASSERT_EQ("(< false 1)", "true");
    TEST_ASSERT_EQ("(> true 0)", "true");

    // bool-vector vs numeric-vector
    TEST_ASSERT_EQ("(== [true false true] [1 0 1])", "[true true true]");
    TEST_ASSERT_EQ("(== [true false] [0 1])", "[false false]");

    // bool vs bool vector comparisons
    TEST_ASSERT_EQ("(== [true false true] [true true false])", "[true false false]");
    TEST_ASSERT_EQ("(!= [true false] [false true])", "[true true]");
    TEST_ASSERT_EQ("(< [false false] [true false])", "[true false]");
    TEST_ASSERT_EQ("(> [true true] [true false])", "[false true]");

    // bool atom vs bool vector
    TEST_ASSERT_EQ("(== true [true false true])", "[true false true]");
    TEST_ASSERT_EQ("(== false [true false true])", "[false true false]");
    TEST_ASSERT_EQ("(<= true [true false])", "[true false]");
    TEST_ASSERT_EQ("(>= true [true false])", "[true true]");

    // numeric atom vs bool vector
    TEST_ASSERT_EQ("(== 1 [true false])", "[true false]");
    TEST_ASSERT_EQ("(== 0 [true false])", "[false true]");

    PASS();
}

// ==================== COMPARISON WITH EMPTY AND SINGLE-ELEMENT VECTORS ====================
test_result_t test_cmp_edge_vectors() {
    // Empty vector comparisons
    TEST_ASSERT_EQ("(== [] [])", "[]");
    TEST_ASSERT_EQ("(!= [] [])", "[]");
    TEST_ASSERT_EQ("(< [] [])", "[]");
    TEST_ASSERT_EQ("(> [] [])", "[]");

    // Single element vector
    TEST_ASSERT_EQ("(== [5] [5])", "[true]");
    TEST_ASSERT_EQ("(== [5] [6])", "[false]");
    TEST_ASSERT_EQ("(< [5] [6])", "[true]");
    TEST_ASSERT_EQ("(> [6] [5])", "[true]");

    // Single element scalar-vector
    TEST_ASSERT_EQ("(== 5 [5])", "[true]");
    TEST_ASSERT_EQ("(== 5 [6])", "[false]");
    TEST_ASSERT_EQ("(< 5 [6])", "[true]");
    TEST_ASSERT_EQ("(> 6 [5])", "[true]");

    // where on empty / single-element
    TEST_ASSERT_EQ("(where [true])", "[0]");
    TEST_ASSERT_EQ("(where [false])", "[]");

    // and/or on matching sizes
    TEST_ASSERT_EQ("(and [true] [true])", "[true]");
    TEST_ASSERT_EQ("(or [false] [false])", "[false]");

    PASS();
}
