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

test_result_t test_timsort_numbers() {
    TEST_ASSERT_EQ("(asc [5 2 8 1 9 3 7 4 6])", "[1 2 3 4 5 6 7 8 9]");
    TEST_ASSERT_EQ("(desc [5 2 8 1 9 3 7 4 6])", "[9 8 7 6 5 4 3 2 1]");
    TEST_ASSERT_EQ("(asc [-5 2 -8 1 -9 3 -7 4 -6])", "[-9 -8 -7 -6 -5 1 2 3 4]");
    TEST_ASSERT_EQ("(asc [3 1 4 1 5 9 2 6 5 3 5])", "[1 1 2 3 3 4 5 5 5 6 9]");
    TEST_ASSERT_EQ("(asc [42])", "[42]");
    TEST_ASSERT_EQ("(asc [])", "[]");
    TEST_ASSERT_EQ("(asc [1 2 3 4 5])", "[1 2 3 4 5]");
    TEST_ASSERT_EQ("(asc [5 4 3 2 1])", "[1 2 3 4 5]");
    TEST_ASSERT_EQ("(asc [9 8 7 6 5 4 3 2 1 0 -1 -2 -3 -4 -5])", "[-5 -4 -3 -2 -1 0 1 2 3 4 5 6 7 8 9]");

    PASS();
}

test_result_t test_sort_asc() {
    TEST_ASSERT_EQ("(iasc [true false true false])", "[1 3 0 2]");
    TEST_ASSERT_EQ("(asc [true false true false])", "[false false true true]");

    TEST_ASSERT_EQ("(iasc [0x03 0x01 0x04 0x02 0x01])", "[1 4 3 0 2]");
    TEST_ASSERT_EQ("(asc [0x03 0x01 0x04 0x02 0x01])", "[0x01 0x01 0x02 0x03 0x04]");

    TEST_ASSERT_EQ("(iasc \"\")", "[]");
    TEST_ASSERT_EQ("(iasc \"dacba\")", "[1 4 3 2 0]");
    TEST_ASSERT_EQ("(asc \"dacba\")", "\"aabcd\"");

    TEST_ASSERT_EQ("(iasc [-10h 20h -30h 40h 0h 0Nh])", "[5 2 0 4 1 3]");
    TEST_ASSERT_EQ("(asc [-10h 20h -30h 40h 0h 0Nh])", "[0Nh -30h -10h 0h 20h 40h]");

    TEST_ASSERT_EQ("(iasc [-10i 20i -30i 40i 0Ni 0i])", "[4 2 0 5 1 3]");
    TEST_ASSERT_EQ("(asc [-10i 20i -30i 40i 0Ni 0i])", "[0Ni -30i -10i 0i 20i 40i]");

    TEST_ASSERT_EQ("(iasc [2023.01.03 2023.01.01 2023.01.04 2023.01.02 0Nd])", "[4 1 3 0 2]");
    TEST_ASSERT_EQ("(asc [2023.01.03 2023.01.01 2023.01.04 2023.01.02 0Nd])",
                   "[0Nd 2023.01.01 2023.01.02 2023.01.03 2023.01.04]");

    TEST_ASSERT_EQ("(iasc [00:00:03.000 00:00:01.000 00:00:04.000 00:00:02.000 0Nt])", "[4 1 3 0 2]");
    TEST_ASSERT_EQ("(asc [00:00:03.000 00:00:01.000 00:00:04.000 00:00:02.000 0Nt])",
                   "[0Nt 00:00:01.000 00:00:02.000 00:00:03.000 00:00:04.000]");

    TEST_ASSERT_EQ("(iasc [-10 20 -30 40 0Nl 0])", "[4 2 0 5 1 3]");
    TEST_ASSERT_EQ("(asc [-10 20 -30 40 0Nl 0])", "[0Nl -30 -10 0 20 40]");

    TEST_ASSERT_EQ(
        "(iasc [2023.01.03D00:00:00.000000000 2023.01.01D00:00:00.000000000 "
        "2023.01.04D00:00:00.000000000 2023.01.02D00:00:00.000000000 0Np])",
        "[4 1 3 0 2]");
    TEST_ASSERT_EQ(
        "(asc [2023.01.03D00:00:00.000000000 2023.01.01D00:00:00.000000000 "
        "2023.01.04D00:00:00.000000000 2023.01.02D00:00:00.000000000 0Np])",
        "[0Np 2023.01.01D00:00:00.000000000 2023.01.02D00:00:00.000000000 "
        "2023.01.03D00:00:00.000000000 2023.01.04D00:00:00.000000000]");

    TEST_ASSERT_EQ("(iasc [-1.0 2.0 -3.0 4.0 0.0 0Nf])", "[5 2 0 4 1 3]");
    TEST_ASSERT_EQ("(asc [-1.0 2.0 -3.0 4.0 0.0 0Nf])", "[0Nf -3.0 -1.0 0.0 2.0 4.0]");

    TEST_ASSERT_EQ("(iasc [])", "[]");
    TEST_ASSERT_EQ("(asc [])", "[]");

    TEST_ASSERT_EQ("(iasc [42])", "[0]");
    TEST_ASSERT_EQ("(asc [42])", "[42]");

    TEST_ASSERT_EQ("(iasc (asc [4 3 2 1]))", "[0 1 2 3]");
    TEST_ASSERT_EQ("(asc (asc [4 3 2 1]))", "[1 2 3 4]");

    TEST_ASSERT_EQ("(iasc (list 'a 0 3i 'b 'aa [5 6] (list [1 2 3]) 3i [6i 7i] 0Nl))", "[0 4 3 9 1 2 7 6 8 5]");
    TEST_ASSERT_EQ("(asc (list 'a 0 3i 'b 'aa [5 6] (list [1 2 3]) 3i [6i 7i] 0Nl))",
                   "(list 'a 'aa 'b 0Nl 0 3i 3i (list [1 2 3]) [6i 7i] [5 6])");

    PASS();
}

test_result_t test_sort_desc() {
    TEST_ASSERT_EQ("(idesc [false true])", "[1 0]");
    TEST_ASSERT_EQ("(desc [true false true false])", "[true true false false]");

    TEST_ASSERT_EQ("(idesc [0x03 0x01 0x04 0x02])", "[2 0 3 1]");
    TEST_ASSERT_EQ("(desc [0x03 0x01 0x04 0x02])", "[0x04 0x03 0x02 0x01]");

    TEST_ASSERT_EQ("(idesc \"dacb\")", "[0 2 3 1]");
    TEST_ASSERT_EQ("(desc \"dacb\")", "\"dcba\"");

    TEST_ASSERT_EQ("(idesc [-10h 20h -30h 40h 0Nh 0h])", "[3 1 5 0 2 4]");
    TEST_ASSERT_EQ("(desc [-10h 20h -30h 40h 0Nh 0h])", "[40h 20h 0h -10h -30h 0Nh]");

    TEST_ASSERT_EQ("(idesc [-10i 20i -30i 40i 0Ni 0i])", "[3 1 5 0 2 4]");
    TEST_ASSERT_EQ("(desc [-10i 20i -30i 40i 0Ni 0i])", "[40i 20i 0i -10i -30i 0Ni]");

    TEST_ASSERT_EQ("(idesc [2023.01.03 2023.01.01 2023.01.04 2023.01.02 0Nd])", "[2 0 3 1 4]");
    TEST_ASSERT_EQ("(desc [2023.01.03 2023.01.01 2023.01.04 2023.01.02 0Nd])",
                   "[2023.01.04 2023.01.03 2023.01.02 2023.01.01 0Nd]");

    TEST_ASSERT_EQ("(idesc [00:00:03.000 00:00:01.000 00:00:04.000 00:00:02.000 0Nt])", "[2 0 3 1 4]");
    TEST_ASSERT_EQ("(desc [00:00:03.000 00:00:01.000 00:00:04.000 00:00:02.000 0Nt])",
                   "[00:00:04.000 00:00:03.000 00:00:02.000 00:00:01.000 0Nt]");

    TEST_ASSERT_EQ("(idesc [-10 20 -30 40 0Nl 0])", "[3 1 5 0 2 4]");
    TEST_ASSERT_EQ("(desc [-10 20 -30 40 0Nl 0])", "[40 20 0 -10 -30 0Nl]");

    TEST_ASSERT_EQ(
        "(idesc [2023.01.03D00:00:00.000000000 2023.01.01D00:00:00.000000000 "
        "2023.01.04D00:00:00.000000000 2023.01.02D00:00:00.000000000 0Np])",
        "[2 0 3 1 4]");
    TEST_ASSERT_EQ(
        "(desc [2023.01.03D00:00:00.000000000 2023.01.01D00:00:00.000000000 "
        "2023.01.04D00:00:00.000000000 2023.01.02D00:00:00.000000000 0Np])",
        "[2023.01.04D00:00:00.000000000 2023.01.03D00:00:00.000000000 "
        "2023.01.02D00:00:00.000000000 2023.01.01D00:00:00.000000000 0Np]");

    TEST_ASSERT_EQ("(idesc [-1.0 2.0 -3.0 4.0 0Nf 0.0])", "[3 1 5 0 2 4]");
    TEST_ASSERT_EQ("(desc [-1.0 2.0 -3.0 4.0 0Nf 0.0])", "[4.0 2.0 0.0 -1.0 -3.0 0Nf]");

    TEST_ASSERT_EQ("(idesc [])", "[]");
    TEST_ASSERT_EQ("(desc [])", "[]");

    TEST_ASSERT_EQ("(idesc [42])", "[0]");
    TEST_ASSERT_EQ("(desc [42])", "[42]");

    TEST_ASSERT_EQ("(idesc (desc [4 3 2 1]))", "[0 1 2 3]");
    TEST_ASSERT_EQ("(desc (desc [4 3 2 1]))", "[4 3 2 1]");

    TEST_ASSERT_EQ("(idesc (list 'a 0 3i 'b 'aa [5 6] (list [1 2 3]) 3i [6i 7i] 0Nl))", "[5 8 6 2 7 1 9 3 4 0]");
    TEST_ASSERT_EQ("(desc (list 'a 0 3i 'b 'aa [5 6] (list [1 2 3]) 3i [6i 7i] 0Nl))",
                   "(list [5 6] [6i 7i] (list [1 2 3]) 3i 3i 0 0Nl 'b 'aa 'a)");

    PASS();
}

test_result_t test_asc_desc() {
    TEST_ASSERT_EQ("(asc (desc [4 3 2 1]))", "[1 2 3 4]");
    TEST_ASSERT_EQ("(iasc (desc [4 3 2 1]))", "[3 2 1 0]");

    TEST_ASSERT_EQ("(desc (asc [4 3 2 1]))", "[4 3 2 1]");
    TEST_ASSERT_EQ("(idesc (asc [4 3 2 1]))", "[3 2 1 0]");

    PASS();
}

test_result_t test_sort_xasc() {
    TEST_ASSERT_EQ("(xasc (table ['a 'b] (list [3 1 2] [30 10 20])) 'a)", "(table ['a 'b] (list [1 2 3] [10 20 30]))");
    TEST_ASSERT_EQ("(xasc (table [a b c] (list [3 1 2] [30 10 20] [100 200 300])) 'b)",
                   "(table [a b c] (list [1 2 3] [10 20 30] [200 300 100]))");
    TEST_ASSERT_EQ("(xasc (table [a b] (list [2 1 2] [20 10 30])) 'a)", "(table [a b] (list [1 2 2] [10 20 30]))");

    TEST_ASSERT_EQ("(xasc (table ['a 'b] (list [2 1 2] [20 10 10])) ['a 'b])",
                   "(table ['a 'b] (list [1 2 2] [10 10 20]))");
    TEST_ASSERT_EQ("(xasc (table ['a 'b] (list [2 1 2] [20 10 10])) ['b 'a])",
                   "(table ['a 'b] (list [1 2 2] [10 10 20]))");
    TEST_ASSERT_EQ("(xasc (table ['a 'b] (list [1 1 1] [3 2 1])) ['a 'b])", "(table ['a 'b] (list [1 1 1] [1 2 3]))");

    PASS();
}

test_result_t test_sort_xdesc() {
    TEST_ASSERT_EQ("(xdesc (table ['a 'b] (list [3 1 2 1] [30 10 20 0])) 'a)",
                   "(table ['a 'b] (list [3 2 1 1] [30 20 10 0]))");
    TEST_ASSERT_EQ("(xdesc (table ['a 'b] (list [3 1 2 1] [30 0 20 10])) 'a)",
                   "(table ['a 'b] (list [3 2 1 1] [30 20 0 10]))");

    TEST_ASSERT_EQ("(xdesc (table ['a 'b] (list [1 1 2 2 3 3] [10 20 10 20 10 20])) ['a 'b])",
                   "(table ['a 'b] (list [3 3 2 2 1 1] [20 10 20 10 20 10]))");
    TEST_ASSERT_EQ("(xdesc (table ['a 'b] (list [1 1 2 2 3 3] [10 20 10 20 10 20])) ['b 'a])",
                   "(table ['a 'b] (list [3 2 1 3 2 1] [20 20 20 10 10 10]))");
    TEST_ASSERT_EQ("(xdesc (table ['a 'b] (list [1 1 1] [3 2 1])) ['a 'b])", "(table ['a 'b] (list [1 1 1] [3 2 1]))");
    PASS();
}

test_result_t test_sort_timsort_symbols() {
    TEST_ASSERT_EQ("(iasc (list 'zebra 'apple 'banana 'cherry))", "[1 2 3 0]");
    TEST_ASSERT_EQ("(asc (list 'zebra 'apple 'banana 'cherry))", "(list 'apple 'banana 'cherry 'zebra)");

    TEST_ASSERT_EQ("(iasc (list 'single))", "[0]");
    TEST_ASSERT_EQ("(asc (list 'single))", "(list 'single)");

    PASS();
}
