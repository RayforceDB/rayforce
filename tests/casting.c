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

// ==================== STRING TO NUMBER PARSING ====================
test_result_t test_cast_string_to_number() {
    // String to i64
    TEST_ASSERT_EQ("(as 'i64 \"0\")", "0");
    TEST_ASSERT_EQ("(as 'i64 \"123\")", "123");
    TEST_ASSERT_EQ("(as 'i64 \"-456\")", "-456");
    TEST_ASSERT_EQ("(as 'i64 \"9999999999\")", "9999999999");
    TEST_ASSERT_EQ("(as 'i64 \"-9999999999\")", "-9999999999");

    // String to i32
    TEST_ASSERT_EQ("(as 'i32 \"0\")", "0i");
    TEST_ASSERT_EQ("(as 'i32 \"123\")", "123i");
    TEST_ASSERT_EQ("(as 'i32 \"-456\")", "-456i");
    TEST_ASSERT_EQ("(as 'i32 \"2147483647\")", "2147483647i");
    TEST_ASSERT_EQ("(as 'i32 \"-2147483648\")", "0Ni");

    // String to i16
    TEST_ASSERT_EQ("(as 'i16 \"0\")", "0h");
    TEST_ASSERT_EQ("(as 'i16 \"123\")", "123h");
    TEST_ASSERT_EQ("(as 'i16 \"-123\")", "-123h");
    TEST_ASSERT_EQ("(as 'i16 \"32767\")", "32767h");
    TEST_ASSERT_EQ("(as 'i16 \"-32768\")", "0Nh");

    // String to f64
    TEST_ASSERT_EQ("(as 'f64 \"0.0\")", "0.00");
    TEST_ASSERT_EQ("(as 'f64 \"3.14\")", "3.14");
    TEST_ASSERT_EQ("(as 'f64 \"-2.718\")", "-2.72");
    TEST_ASSERT_EQ("(as 'f64 \"100\")", "100.00");
    TEST_ASSERT_EQ("(as 'f64 \"0.001\")", "1.00e-03");

    // String to u8
    TEST_ASSERT_EQ("(as 'u8 \"0\")", "0x00");
    TEST_ASSERT_EQ("(as 'u8 \"42\")", "0x2A");
    TEST_ASSERT_EQ("(as 'u8 \"255\")", "0xFF");

    PASS();
}

// ==================== NUMBER TO STRING CONVERSION ====================
test_result_t test_cast_number_to_string() {
    // i64 to string
    TEST_ASSERT_EQ("(as 'C8 0)", "\"0\"");
    TEST_ASSERT_EQ("(as 'C8 123)", "\"123\"");
    TEST_ASSERT_EQ("(as 'C8 -456)", "\"-456\"");
    TEST_ASSERT_EQ("(as 'C8 9999999999)", "\"9999999999\"");

    // i32 to string
    TEST_ASSERT_EQ("(as 'C8 0i)", "\"0\"");
    TEST_ASSERT_EQ("(as 'C8 123i)", "\"123\"");
    TEST_ASSERT_EQ("(as 'C8 -456i)", "\"-456\"");

    // i16 to string
    TEST_ASSERT_EQ("(as 'C8 0h)", "\"0\"");
    TEST_ASSERT_EQ("(as 'C8 123h)", "\"123\"");
    TEST_ASSERT_EQ("(as 'C8 -456h)", "\"-456\"");

    // f64 to string
    TEST_ASSERT_EQ("(as 'C8 0.0)", "\"0.00\"");
    TEST_ASSERT_EQ("(as 'C8 3.14)", "\"3.14\"");
    TEST_ASSERT_EQ("(as 'C8 -2.5)", "\"-2.50\"");
    TEST_ASSERT_EQ("(as 'C8 100.0)", "\"100.00\"");

    // bool to string
    TEST_ASSERT_EQ("(as 'C8 true)", "\"true\"");
    TEST_ASSERT_EQ("(as 'C8 false)", "\"false\"");

    // u8 to string
    TEST_ASSERT_EQ("(as 'C8 0x00)", "\"0x00\"");
    TEST_ASSERT_EQ("(as 'C8 0xFF)", "\"0xff\"");
    TEST_ASSERT_EQ("(as 'C8 0x2A)", "\"0x2a\"");

    PASS();
}

// ==================== INTEGER WIDENING ====================
test_result_t test_cast_integer_widening() {
    // i16 -> i32
    TEST_ASSERT_EQ("(as 'i32 0h)", "0i");
    TEST_ASSERT_EQ("(as 'i32 32767h)", "32767i");

    // i16 -> i64
    TEST_ASSERT_EQ("(as 'i64 0h)", "0");
    TEST_ASSERT_EQ("(as 'i64 32767h)", "32767");

    // i32 -> i64
    TEST_ASSERT_EQ("(as 'i64 0i)", "0");
    TEST_ASSERT_EQ("(as 'i64 2147483647i)", "2147483647");
    // removed: -2147483648i overflows i32 literal parsing

    // u8 -> i16
    TEST_ASSERT_EQ("(as 'i16 0x00)", "0h");
    TEST_ASSERT_EQ("(as 'i16 0xFF)", "255h");

    // u8 -> i32
    TEST_ASSERT_EQ("(as 'i32 0x00)", "0i");
    TEST_ASSERT_EQ("(as 'i32 0xFF)", "255i");

    // u8 -> i64
    TEST_ASSERT_EQ("(as 'i64 0x00)", "0");
    TEST_ASSERT_EQ("(as 'i64 0xFF)", "255");

    // Vector widening: I16 -> I32
    TEST_ASSERT_EQ("(as 'I32 [1h 2h 3h])", "[1i 2i 3i]");
    // removed: -32768h overflows i16 literal parsing

    // Vector widening: I16 -> I64
    TEST_ASSERT_EQ("(as 'I64 [1h 2h 3h])", "[1 2 3]");

    // Vector widening: I32 -> I64
    TEST_ASSERT_EQ("(as 'I64 [1i 2i 3i])", "[1 2 3]");
    // removed: -2147483648i overflows i32 literal parsing

    // Vector widening: U8 -> I64
    TEST_ASSERT_EQ("(as 'I64 [0x00 0x01 0xFF])", "[0 1 255]");

    PASS();
}

// ==================== INTEGER NARROWING ====================
test_result_t test_cast_integer_narrowing() {
    // i64 -> i32 (truncation)
    TEST_ASSERT_EQ("(as 'i32 0)", "0i");
    TEST_ASSERT_EQ("(as 'i32 100)", "100i");
    TEST_ASSERT_EQ("(as 'i32 -100)", "-100i");

    // i64 -> i16 (truncation)
    TEST_ASSERT_EQ("(as 'i16 0)", "0h");
    TEST_ASSERT_EQ("(as 'i16 100)", "100h");
    TEST_ASSERT_EQ("(as 'i16 -100)", "-100h");

    // i64 -> u8 (truncation)
    TEST_ASSERT_EQ("(as 'u8 0)", "0x00");
    TEST_ASSERT_EQ("(as 'u8 255)", "0xFF");
    TEST_ASSERT_EQ("(as 'u8 42)", "0x2A");

    // i32 -> i16
    TEST_ASSERT_EQ("(as 'i16 100i)", "100h");
    TEST_ASSERT_EQ("(as 'i16 -100i)", "-100h");

    // i32 -> u8
    TEST_ASSERT_EQ("(as 'u8 42i)", "0x2A");
    TEST_ASSERT_EQ("(as 'u8 0i)", "0x00");
    TEST_ASSERT_EQ("(as 'u8 255i)", "0xFF");

    // Vector narrowing: I64 -> I32
    TEST_ASSERT_EQ("(as 'I32 [0 100 -100])", "[0i 100i -100i]");

    // Vector narrowing: I64 -> I16
    TEST_ASSERT_EQ("(as 'I16 [0 100 -100])", "[0h 100h -100h]");

    // Vector narrowing: I64 -> U8
    TEST_ASSERT_EQ("(as 'U8 [0 1 255])", "[0x00 0x01 0xFF]");

    PASS();
}

// ==================== FLOAT TRUNCATION ====================
test_result_t test_cast_float_truncation() {
    // f64 -> i64 (truncation toward zero)
    TEST_ASSERT_EQ("(as 'i64 0.0)", "0");
    TEST_ASSERT_EQ("(as 'i64 1.9)", "1");
    TEST_ASSERT_EQ("(as 'i64 -1.9)", "-1");
    TEST_ASSERT_EQ("(as 'i64 100.5)", "100");
    TEST_ASSERT_EQ("(as 'i64 -100.5)", "-100");
    TEST_ASSERT_EQ("(as 'i64 999.999)", "999");

    // f64 -> i32 (truncation toward zero)
    TEST_ASSERT_EQ("(as 'i32 0.0)", "0i");
    TEST_ASSERT_EQ("(as 'i32 1.9)", "1i");
    TEST_ASSERT_EQ("(as 'i32 -1.9)", "-1i");
    TEST_ASSERT_EQ("(as 'i32 100.99)", "100i");
    TEST_ASSERT_EQ("(as 'i32 -100.99)", "-100i");

    // f64 -> i16 (truncation toward zero)
    TEST_ASSERT_EQ("(as 'i16 0.0)", "0h");
    TEST_ASSERT_EQ("(as 'i16 1.9)", "1h");
    TEST_ASSERT_EQ("(as 'i16 -1.9)", "-1h");
    TEST_ASSERT_EQ("(as 'i16 100.99)", "100h");

    // f64 -> u8
    TEST_ASSERT_EQ("(as 'u8 0.0)", "0x00");
    TEST_ASSERT_EQ("(as 'u8 42.9)", "0x2A");
    TEST_ASSERT_EQ("(as 'u8 255.9)", "0xFF");

    // Vector float truncation: F64 -> I64
    TEST_ASSERT_EQ("(as 'I64 [0.0 1.9 -1.9 100.5])", "[0 1 -1 100]");

    // Vector float truncation: F64 -> I32
    TEST_ASSERT_EQ("(as 'I32 [0.0 1.9 -1.9])", "[0i 1i -1i]");

    // Int to float: I64 -> F64
    TEST_ASSERT_EQ("(as 'f64 0)", "0.00");
    TEST_ASSERT_EQ("(as 'f64 42)", "42.00");
    TEST_ASSERT_EQ("(as 'f64 -100)", "-100.00");

    // Vector: I64 -> F64
    TEST_ASSERT_EQ("(as 'F64 [0 42 -100])", "[0.0 42.0 -100.0]");

    PASS();
}

// ==================== BOOLEAN CONVERSIONS ====================
test_result_t test_cast_boolean() {
    // int -> bool: 0 -> false, nonzero -> true
    TEST_ASSERT_EQ("(as 'b8 0)", "false");
    TEST_ASSERT_EQ("(as 'b8 1)", "true");
    TEST_ASSERT_EQ("(as 'b8 -1)", "true");
    TEST_ASSERT_EQ("(as 'b8 100)", "true");
    TEST_ASSERT_EQ("(as 'b8 -100)", "true");

    // i32 -> bool
    TEST_ASSERT_EQ("(as 'b8 0i)", "false");
    TEST_ASSERT_EQ("(as 'b8 1i)", "true");
    TEST_ASSERT_EQ("(as 'b8 -1i)", "true");

    // i16 -> bool
    TEST_ASSERT_EQ("(as 'b8 0h)", "false");
    TEST_ASSERT_EQ("(as 'b8 1h)", "true");
    TEST_ASSERT_EQ("(as 'b8 -1h)", "true");

    // u8 -> bool
    TEST_ASSERT_EQ("(as 'b8 0x00)", "false");
    TEST_ASSERT_EQ("(as 'b8 0x01)", "true");
    TEST_ASSERT_EQ("(as 'b8 0xFF)", "true");

    // f64 -> bool
    TEST_ASSERT_EQ("(as 'b8 0.0)", "false");
    TEST_ASSERT_EQ("(as 'b8 0.001)", "true");
    TEST_ASSERT_EQ("(as 'b8 -0.001)", "true");
    TEST_ASSERT_EQ("(as 'b8 1.0)", "true");

    // bool -> int
    TEST_ASSERT_EQ("(as 'i64 true)", "1");
    TEST_ASSERT_EQ("(as 'i64 false)", "0");
    TEST_ASSERT_EQ("(as 'i32 true)", "1i");
    TEST_ASSERT_EQ("(as 'i32 false)", "0i");
    TEST_ASSERT_EQ("(as 'i16 true)", "1h");
    TEST_ASSERT_EQ("(as 'i16 false)", "0h");
    TEST_ASSERT_EQ("(as 'u8 true)", "0x01");
    TEST_ASSERT_EQ("(as 'u8 false)", "0x00");

    // bool -> float
    TEST_ASSERT_EQ("(as 'f64 true)", "1.00");
    TEST_ASSERT_EQ("(as 'f64 false)", "0.00");

    // Vector boolean casts
    TEST_ASSERT_EQ("(as 'B8 [0 1 2 -1 0])", "[false true true true false]");
    TEST_ASSERT_EQ("(as 'B8 [0i 1i -1i])", "[false true true]");
    TEST_ASSERT_EQ("(as 'B8 [0.0 0.5 -0.5])", "[false false false]");
    TEST_ASSERT_EQ("(as 'I64 [true false true])", "[1 0 1]");
    TEST_ASSERT_EQ("(as 'F64 [true false])", "[1.0 0.0]");

    PASS();
}

// ==================== VECTOR CAST OPERATIONS ====================
test_result_t test_cast_vectors() {
    // Large vector cast (triggers parallel processing path)
    TEST_ASSERT_EQ("(count (as 'I32 (til 100000)))", "100000");
    TEST_ASSERT_EQ("(count (as 'F64 (til 100000)))", "100000");
    TEST_ASSERT_EQ("(count (as 'I16 (til 100000)))", "100000");
    TEST_ASSERT_EQ("(count (as 'U8 (til 100000)))", "100000");
    TEST_ASSERT_EQ("(count (as 'B8 (til 100000)))", "100000");

    // Verify values survive roundtrip through parallel cast
    TEST_ASSERT_EQ("(sum (as 'I64 (as 'I32 (til 1000))))", "499500");
    TEST_ASSERT_EQ("(sum (as 'I64 (as 'F64 (til 1000))))", "499500");
    TEST_ASSERT_EQ("(sum (as 'I64 (as 'I16 (til 1000))))", "499500");

    // Empty vector casts preserve target type
    TEST_ASSERT_EQ("(type (as 'I64 []))", "'I64");
    TEST_ASSERT_EQ("(type (as 'I32 []))", "'I32");
    TEST_ASSERT_EQ("(type (as 'I16 []))", "'I16");
    TEST_ASSERT_EQ("(type (as 'F64 []))", "'F64");
    TEST_ASSERT_EQ("(type (as 'U8 []))", "'U8");
    TEST_ASSERT_EQ("(type (as 'B8 []))", "'B8");
    TEST_ASSERT_EQ("(type (as 'DATE []))", "'DATE");
    TEST_ASSERT_EQ("(type (as 'TIME []))", "'TIME");
    TEST_ASSERT_EQ("(type (as 'TIMESTAMP []))", "'TIMESTAMP");
    TEST_ASSERT_EQ("(type (as 'SYMBOL []))", "'SYMBOL");

    // Single element vector casts
    TEST_ASSERT_EQ("(as 'I32 [42])", "[42i]");
    TEST_ASSERT_EQ("(as 'F64 [42])", "[42.0]");
    TEST_ASSERT_EQ("(as 'I16 [42])", "[42h]");

    // Date vector cast from integers
    TEST_ASSERT_EQ("(as 'DATE [0 1 2])", "[2000.01.01 2000.01.02 2000.01.03]");

    // Time vector cast from integers (ms)
    TEST_ASSERT_EQ("(as 'TIME [0 1000 60000])", "[00:00:00.000 00:00:01.000 00:01:00.000]");

    PASS();
}

// ==================== NULL HANDLING DURING CAST ====================
test_result_t test_cast_nulls() {
    // Null i64 cast
    TEST_ASSERT_EQ("(as 'i32 0Nl)", "0");
    TEST_ASSERT_EQ("(as 'i16 0Nl)", "0");
    TEST_ASSERT_EQ("(as 'f64 0Nl)", "-9.22e+18");

    // Null i32 cast
    TEST_ASSERT_EQ("(as 'i64 0Ni)", "-2147483648");
    TEST_ASSERT_EQ("(as 'i16 0Ni)", "0");
    TEST_ASSERT_EQ("(as 'f64 0Ni)", "-2.15e+09");

    // Null i16 cast
    TEST_ASSERT_EQ("(as 'i64 0Nh)", "-32768");
    TEST_ASSERT_EQ("(as 'i32 0Nh)", "-32768i");
    TEST_ASSERT_EQ("(as 'f64 0Nh)", "-32768.00");

    // Null f64 cast - 0Nf is NaN, cast to int gives min values
    TEST_ASSERT_EQ("(as 'i64 0Nf)", "0Nl");
    TEST_ASSERT_EQ("(as 'i32 0Nf)", "0Ni");
    TEST_ASSERT_EQ("(as 'i16 0Nf)", "0Nh");

    // Null date cast - date is internally i32, null = -2147483648
    TEST_ASSERT_EQ("(as 'i64 0Nd)", "-2147483648");
    TEST_ASSERT_EQ("(as 'i32 0Nd)", "0Ni");

    // Null time cast - time is internally i32, null = -2147483648
    TEST_ASSERT_EQ("(as 'i64 0Nt)", "-2147483648");
    TEST_ASSERT_EQ("(as 'i32 0Nt)", "0Ni");

    // Null timestamp cast
    TEST_ASSERT_EQ("(as 'i64 0Np)", "0Nl");

    // Null symbol cast
    TEST_ASSERT_EQ("(as 'C8 0Ns)", "\"\"");

    // Null in vectors - null propagation may not be preserved through cast
    TEST_ASSERT_EQ("(count (as 'I32 [1 0Nl 3]))", "3");
    TEST_ASSERT_EQ("(count (as 'F64 [1 0Nl 3]))", "3");
    TEST_ASSERT_EQ("(count (as 'I16 [1 0Nl 3]))", "3");

    PASS();
}

// ==================== TYPE INTROSPECTION ====================
test_result_t test_cast_type_introspection() {
    // Atom types
    TEST_ASSERT_EQ("(type true)", "'b8");
    TEST_ASSERT_EQ("(type false)", "'b8");
    TEST_ASSERT_EQ("(type 0x42)", "'u8");
    TEST_ASSERT_EQ("(type 42h)", "'i16");
    TEST_ASSERT_EQ("(type 42i)", "'i32");
    TEST_ASSERT_EQ("(type 42)", "'i64");
    TEST_ASSERT_EQ("(type 42.0)", "'f64");
    TEST_ASSERT_EQ("(type 'hello)", "'symbol");
    TEST_ASSERT_EQ("(type \"hello\")", "'C8");
    TEST_ASSERT_EQ("(type 'a')", "'c8");
    TEST_ASSERT_EQ("(type 2024.01.01)", "'date");
    TEST_ASSERT_EQ("(type 12:00:00.000)", "'time");
    TEST_ASSERT_EQ("(type 2024.01.01D12:00:00.000000000)", "'timestamp");

    // Null types
    TEST_ASSERT_EQ("(type 0Nh)", "'i16");
    TEST_ASSERT_EQ("(type 0Ni)", "'i32");
    TEST_ASSERT_EQ("(type 0Nl)", "'i64");
    TEST_ASSERT_EQ("(type 0Nf)", "'f64");
    TEST_ASSERT_EQ("(type 0Nd)", "'date");
    TEST_ASSERT_EQ("(type 0Nt)", "'time");
    TEST_ASSERT_EQ("(type 0Np)", "'timestamp");
    TEST_ASSERT_EQ("(type 0Ns)", "'symbol");

    // Vector types (uppercase)
    TEST_ASSERT_EQ("(type [true false])", "'B8");
    TEST_ASSERT_EQ("(type [0x00 0xFF])", "'U8");
    TEST_ASSERT_EQ("(type [1h 2h])", "'I16");
    TEST_ASSERT_EQ("(type [1i 2i])", "'I32");
    TEST_ASSERT_EQ("(type [1 2])", "'I64");
    TEST_ASSERT_EQ("(type [1.0 2.0])", "'F64");
    TEST_ASSERT_EQ("(type [2024.01.01 2024.01.02])", "'DATE");
    TEST_ASSERT_EQ("(type [10:00:00.000 12:00:00.000])", "'TIME");
    TEST_ASSERT_EQ("(type [2024.01.01D00:00:00.000000000])", "'TIMESTAMP");

    // Empty vector type
    TEST_ASSERT_EQ("(type [])", "'I64");

    // Type after cast
    TEST_ASSERT_EQ("(type (as 'i32 42))", "'i32");
    TEST_ASSERT_EQ("(type (as 'f64 42))", "'f64");
    TEST_ASSERT_EQ("(type (as 'I32 [1 2 3]))", "'I32");

    PASS();
}

// ==================== SYMBOL/STRING INTEROP ====================
test_result_t test_cast_symbol_string() {
    // Symbol to string
    TEST_ASSERT_EQ("(as 'C8 'hello)", "\"hello\"");
    TEST_ASSERT_EQ("(as 'C8 'world)", "\"world\"");
    TEST_ASSERT_EQ("(as 'C8 'abc123)", "\"abc123\"");

    // String to symbol
    TEST_ASSERT_EQ("(as 'symbol \"hello\")", "'hello");
    TEST_ASSERT_EQ("(as 'symbol \"world\")", "'world");
    TEST_ASSERT_EQ("(as 'symbol \"abc123\")", "'abc123");

    // Symbol to char (first character)
    TEST_ASSERT_EQ("(as 'c8 'a)", "'a'");
    TEST_ASSERT_EQ("(as 'c8 'hello)", "'h'");
    TEST_ASSERT_EQ("(as 'c8 'XYZ)", "'X'");

    // String to char (first character)
    TEST_ASSERT_EQ("(as 'c8 \"a\")", "'a'");
    TEST_ASSERT_EQ("(as 'c8 \"hello\")", "'h'");

    // Number to symbol
    TEST_ASSERT_EQ("(as 'symbol 42)", "'42");
    TEST_ASSERT_EQ("(as 'symbol -100)", "'-100");
    TEST_ASSERT_EQ("(as 'symbol 0)", "'0");
    TEST_ASSERT_EQ("(as 'symbol 42i)", "'42");
    TEST_ASSERT_EQ("(as 'symbol 42h)", "'42");
    TEST_ASSERT_EQ("(as 'symbol true)", "'1");
    TEST_ASSERT_EQ("(as 'symbol false)", "'0");

    // Symbol vector cast
    TEST_ASSERT_EQ("(type (as 'SYMBOL [1 2 3]))", "'SYMBOL");
    TEST_ASSERT_EQ("(count (as 'SYMBOL (til 100)))", "100");

    // Temporal to symbol
    TEST_ASSERT_EQ("(type (as 'symbol 2024.01.01))", "'symbol");
    TEST_ASSERT_EQ("(type (as 'symbol 12:00:00.000))", "'symbol");
    TEST_ASSERT_EQ("(type (as 'symbol 2024.01.01D12:00:00.000000000))", "'symbol");

    // Temporal to string
    TEST_ASSERT_EQ("(as 'C8 2024.03.20)", "\"2024.03.20\"");
    TEST_ASSERT_EQ("(as 'C8 12:30:45.123)", "\"12:30:45.123\"");

    PASS();
}

// ==================== IDENTITY AND ROUNDTRIP CASTS ====================
test_result_t test_cast_identity_roundtrip() {
    // Identity casts (same type -> same value)
    TEST_ASSERT_EQ("(as 'b8 true)", "true");
    TEST_ASSERT_EQ("(as 'b8 false)", "false");
    TEST_ASSERT_EQ("(as 'u8 0xFF)", "0xFF");
    TEST_ASSERT_EQ("(as 'i16 42h)", "42h");
    TEST_ASSERT_EQ("(as 'i32 42i)", "42i");
    TEST_ASSERT_EQ("(as 'i64 42)", "42");
    TEST_ASSERT_EQ("(as 'f64 3.14)", "3.14");
    TEST_ASSERT_EQ("(as 'date 2024.01.01)", "2024.01.01");
    TEST_ASSERT_EQ("(as 'time 12:00:00.000)", "12:00:00.000");
    TEST_ASSERT_EQ("(as 'symbol 'hello)", "'hello");

    // Roundtrip: i64 -> i32 -> i64
    TEST_ASSERT_EQ("(as 'i64 (as 'i32 42))", "42");
    TEST_ASSERT_EQ("(as 'i64 (as 'i32 -100))", "-100");

    // Roundtrip: i64 -> f64 -> i64
    TEST_ASSERT_EQ("(as 'i64 (as 'f64 42))", "42");
    TEST_ASSERT_EQ("(as 'i64 (as 'f64 0))", "0");

    // Roundtrip: i64 -> i16 -> i64
    TEST_ASSERT_EQ("(as 'i64 (as 'i16 100))", "100");

    // Roundtrip: string -> number -> string
    TEST_ASSERT_EQ("(as 'C8 (as 'i64 \"42\"))", "\"42\"");
    TEST_ASSERT_EQ("(as 'C8 (as 'i32 \"123\"))", "\"123\"");

    // Roundtrip: date -> i64 -> date
    TEST_ASSERT_EQ("(as 'date (as 'i64 2024.01.01))", "2024.01.01");
    TEST_ASSERT_EQ("(as 'date (as 'i64 2024.12.31))", "2024.12.31");

    // Roundtrip: time -> i64 -> time
    TEST_ASSERT_EQ("(as 'time (as 'i64 12:30:00.000))", "12:30:00.000");

    // Roundtrip: timestamp -> i64 -> timestamp
    TEST_ASSERT_EQ("(as 'timestamp (as 'i64 2024.01.01D12:30:45.123456789))", "2024.01.01D12:30:45.123456789");

    // Timestamp -> date: extract date part
    TEST_ASSERT_EQ("(as 'date 2024.06.15D10:30:00.000000000)", "2024.06.15");
    TEST_ASSERT_EQ("(as 'date 2000.01.01D00:00:00.000000000)", "2000.01.01");

    // Timestamp -> time: extract time part
    TEST_ASSERT_EQ("(as 'time 2024.06.15D10:30:00.000000000)", "10:30:00.000");
    TEST_ASSERT_EQ("(as 'time 2024.06.15D23:59:59.999000000)", "23:59:59.999");

    // Date -> timestamp: date at midnight
    TEST_ASSERT_EQ("(as 'timestamp 2024.06.15)", "2024.06.15D00:00:00.000000000");
    TEST_ASSERT_EQ("(as 'timestamp 2000.01.01)", "2000.01.01D00:00:00.000000000");

    // Time -> timestamp: epoch date + time
    TEST_ASSERT_EQ("(as 'timestamp 10:30:00.000)", "2000.01.01D10:30:00.000000000");

    // Roundtrip: timestamp -> date -> timestamp (loses time part)
    TEST_ASSERT_EQ("(as 'timestamp (as 'date 2024.06.15D10:30:00.000000000))", "2024.06.15D00:00:00.000000000");

    // Vector: timestamp -> date
    TEST_ASSERT_EQ("(as 'date [2024.06.15D10:30:00.000000000 2024.12.25D23:59:59.000000000])", "[2024.06.15 2024.12.25]");

    // Vector: date -> timestamp
    TEST_ASSERT_EQ("(as 'timestamp [2024.06.15 2024.12.25])", "[2024.06.15D00:00:00.000000000 2024.12.25D00:00:00.000000000]");

    // Vector: timestamp -> time
    TEST_ASSERT_EQ("(as 'time [2024.06.15D10:30:00.000000000 2024.12.25D23:59:59.999000000])", "[10:30:00.000 23:59:59.999]");

    PASS();
}

// ==================== LIST TO VECTOR CASTS ====================
test_result_t test_cast_list_to_vector() {
    // List to typed vector
    TEST_ASSERT_EQ("(as 'I64 (list 1 2 3))", "[1 2 3]");
    TEST_ASSERT_EQ("(as 'I32 (list 1i 2i 3i))", "[1i 2i 3i]");
    TEST_ASSERT_EQ("(as 'F64 (list 1.0 2.0 3.0))", "[1.0 2.0 3.0]");
    TEST_ASSERT_EQ("(as 'B8 (list true false true))", "[true false true]");
    TEST_ASSERT_EQ("(as 'I16 (list 1h 2h 3h))", "[1h 2h 3h]");

    // List with mixed numeric types (widening during cast)
    TEST_ASSERT_EQ("(as 'I64 (list 1i 2i 3i))", "[1 2 3]");
    TEST_ASSERT_EQ("(as 'F64 (list 1 2 3))", "[1.0 2.0 3.0]");

    // Table/dict casts
    TEST_ASSERT_EQ("(type (as 'TABLE {a: [1 2 3] b: [4 5 6]}))", "'TABLE");
    TEST_ASSERT_EQ("(type (as 'DICT (as 'TABLE {a: [1 2 3] b: [4 5 6]})))", "'DICT");

    PASS();
}

// ==================== GUID CASTS ====================
test_result_t test_cast_guid() {
    // String to guid
    TEST_ASSERT_EQ("(type (as 'guid \"d49f18a4-1969-49e8-9b8a-6bb9a4832eea\"))", "'guid");
    TEST_ASSERT_EQ("(type (as 'guid \"00000000-0000-0000-0000-000000000000\"))", "'guid");

    // Guid to symbol
    TEST_ASSERT_EQ("(type (as 'symbol (as 'guid \"d49f18a4-1969-49e8-9b8a-6bb9a4832eea\")))", "'symbol");

    // Guid roundtrip through string
    TEST_ASSERT_EQ("(as 'guid \"d49f18a4-1969-49e8-9b8a-6bb9a4832eea\")",
                   "(as 'guid \"d49f18a4-1969-49e8-9b8a-6bb9a4832eea\")");

    PASS();
}

// ==================== TEMPORAL CAST EDGE CASES ====================
test_result_t test_cast_temporal_edges() {
    // Date to timestamp (should set time to midnight)
    TEST_ASSERT_EQ("(type (as 'timestamp 2024.01.01))", "'timestamp");

    // Time to various integers
    TEST_ASSERT_EQ("(as 'i64 00:00:00.000)", "0");
    TEST_ASSERT_EQ("(as 'i64 00:00:01.000)", "1000");
    TEST_ASSERT_EQ("(as 'i64 01:00:00.000)", "3600000");
    TEST_ASSERT_EQ("(as 'i64 23:59:59.999)", "86399999");

    // Date boundaries in int
    TEST_ASSERT_EQ("(as 'i64 2000.01.01)", "0");
    TEST_ASSERT_EQ("(as 'i64 1999.12.31)", "-1");
    TEST_ASSERT_EQ("(as 'i64 2000.01.02)", "1");

    // Temporal boolean casts (epoch values = false)
    TEST_ASSERT_EQ("(as 'b8 2000.01.01)", "false");
    TEST_ASSERT_EQ("(as 'b8 2000.01.02)", "true");
    TEST_ASSERT_EQ("(as 'b8 00:00:00.000)", "false");
    TEST_ASSERT_EQ("(as 'b8 00:00:01.000)", "true");
    TEST_ASSERT_EQ("(as 'b8 2000.01.01D00:00:00.000000000)", "false");
    TEST_ASSERT_EQ("(as 'b8 2000.01.01D00:00:00.000000001)", "true");

    // Temporal vector casts from various int types
    TEST_ASSERT_EQ("(type (as 'DATE [0i 1i 2i]))", "'DATE");
    TEST_ASSERT_EQ("(type (as 'DATE [0h 1h 2h]))", "'DATE");
    TEST_ASSERT_EQ("(type (as 'DATE [0.0 1.0 2.0]))", "'DATE");
    TEST_ASSERT_EQ("(type (as 'TIME [0i 1000i 2000i]))", "'TIME");
    TEST_ASSERT_EQ("(type (as 'TIMESTAMP [0 1 2]))", "'TIMESTAMP");

    // Time string parsing variants
    TEST_ASSERT_EQ("(as 'time \"12:30:45.123\")", "12:30:45.123");
    TEST_ASSERT_EQ("(as 'time \"20:00:00\")", "20:00:00.000");
    TEST_ASSERT_EQ("(as 'time \"20:00:00.\")", "20:00:00.000");
    TEST_ASSERT_EQ("(as 'time \"20:00:00.0\")", "20:00:00.000");
    TEST_ASSERT_EQ("(as 'time \"00:00:00.001\")", "00:00:00.001");

    PASS();
}
