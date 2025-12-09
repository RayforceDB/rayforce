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

// Map and pmap tests
test_result_t test_lang_map() {
    // Basic map
    TEST_ASSERT_EQ("(map count (list (list \"aaa\" \"bbb\")))", "[2]");
    TEST_ASSERT_EQ("(map (fn [x] (* x x)) [1 2 3 4 5])", "[1 4 9 16 25]");
    TEST_ASSERT_EQ("(map (fn [x] (sum (til 100))) (til 5))", "[4950 4950 4950 4950 4950]");

    // Basic pmap - same results as map
    TEST_ASSERT_EQ("(pmap (fn [x] (* x x)) [1 2 3 4 5])", "[1 4 9 16 25]");
    TEST_ASSERT_EQ("(pmap (fn [x] (sum (til 100))) (til 5))", "[4950 4950 4950 4950 4950]");

    PASS();
}

test_result_t test_lang_basic() {
    TEST_ASSERT_EQ("null", "null");
    TEST_ASSERT_EQ("0x1a", "0x1a");
    TEST_ASSERT_EQ("[0x1a 0x1b]", "[0x1a 0x1b]");
    TEST_ASSERT_EQ("true", "true");
    TEST_ASSERT_EQ("false", "false");
    TEST_ASSERT_EQ("1", "1");
    TEST_ASSERT_EQ("1.1", "1.10");
    TEST_ASSERT_EQ("(as 'i64 \" 1\")", "1");
    TEST_ASSERT_EQ("0.6", "0.60");
    TEST_ASSERT_EQ("0.599999999999999999", "0.60");
    TEST_ASSERT_EQ("0.599999999999999999999999999999999999999999999999999999999999999", "0.60");
    TEST_ASSERT_EQ("1.000000123555555555555555555555555e-02", "0.01");
    TEST_ASSERT_EQ("1.000000123555555555555555555555555e-01", "0.10");
    TEST_ASSERT_EQ("1.000000123555555555555555555555555e-00", "1.00");
    TEST_ASSERT_EQ("(as 'f64 \" 1.000000123555555555555555555555555e+01\")", "10.00");
    TEST_ASSERT_EQ("-1000123555555555555555555555555", "-1.000124e+30");
    TEST_ASSERT_ER("33000h", "Number is out of range");
    TEST_ASSERT_ER("-10001230000i", "Number is out of range");
    TEST_ASSERT_EQ("\"\"", "\"\"");
    TEST_ASSERT_EQ("'asd", "'asd");
    TEST_ASSERT_EQ("'", "0Ns");
    TEST_ASSERT_EQ("(as 'String ')", "\"\"");
    TEST_ASSERT_EQ("(as 'String ' )", "\"\"");
    TEST_ASSERT_EQ("\"asd\"", "\"asd\"");
    TEST_ASSERT_EQ("{a: \"asd\" b: 1 c: [1 2 3]}", "{a: \"asd\" b: 1 c: [1 2 3]}");
    TEST_ASSERT_EQ("{a: \"asd\" b: 1 c: [1 2 3] d: {e: 1 f: 2}}", "{a: \"asd\" b: 1 c: [1 2 3] d: {e: 1 f: 2}}");
    TEST_ASSERT_EQ("{a: \"asd\" b: 1 c: [1 2 3] d: {e: 1 f: 2 g: {h: 1 i: 2}}}",
                   "{a: \"asd\" b: 1 c: [1 2 3] d: {e: 1 f: 2 g: {h: 1 i: 2}}}");
    TEST_ASSERT_EQ("(list 1 2 3 \"asd\")", "(list 1 2 3 \"asd\")");
    TEST_ASSERT_EQ("(list 1 2 3 \"asd\" (list 1 2 3))", "(list 1 2 3 \"asd\" (list 1 2 3))");
    TEST_ASSERT_EQ("(list 1 2 3 \"asd\" (list 1 2 3 (list 1 2 3)))", "(list 1 2 3 \"asd\" (list 1 2 3 (list 1 2 3)))");
    TEST_ASSERT_EQ("(list 1 2 3)", "(list 1 2 3)");
    TEST_ASSERT_EQ("(enlist 1 2 3)", "[1 2 3]");

    PASS();
}

test_result_t test_lang_math() {
    TEST_ASSERT_EQ("(+ 0Ni 0Ni)", "0Ni");
    TEST_ASSERT_EQ("(+ 0i 0Ni)", "0Ni");
    TEST_ASSERT_EQ("(+ 0Ni -1i)", "0Ni");
    TEST_ASSERT_EQ("(+ 0Nl 0Nl)", "0Nl");
    TEST_ASSERT_EQ("(+ 0 0Nl)", "0Nl");
    TEST_ASSERT_EQ("(+ 0Ni -1i)", "0Ni");
    TEST_ASSERT_EQ("(+ 0Ni -10.00)", "0Nf");
    TEST_ASSERT_EQ("(+ 0Ni 0Nl)", "0Nl");
    TEST_ASSERT_EQ("(+ 0Nf 0Ni)", "0Nf");
    TEST_ASSERT_EQ("(+ 0Nf 5)", "0Nf");
    TEST_ASSERT_EQ("(+ 0.00 0Ni)", "0Nf");
    TEST_ASSERT_EQ("(+ 0Ni -0.00)", "0Nf");
    TEST_ASSERT_EQ("(+ -0.00 0Nl)", "0Nf");
    TEST_ASSERT_EQ("(+ 0Nf [-0.00])", "[0Nf]");
    TEST_ASSERT_ER("(+ 0Nf 2024.03.20)", "add: unsupported types: 'f64, 'date");

    TEST_ASSERT_EQ("(+ 3i 5i)", "8i");
    TEST_ASSERT_EQ("(+ 3i 5)", "8");
    TEST_ASSERT_EQ("(+ 3i 5.2)", "8.2");
    TEST_ASSERT_EQ("(+ 3i 2024.03.20)", "2024.03.23");
    TEST_ASSERT_EQ("(+ 3i 20:15:07.000)", "20:15:07.003");
    TEST_ASSERT_EQ("(+ 3i 2025.03.04D15:41:47.087221025)", "2025.03.04D15:41:47.087221028");
    TEST_ASSERT_EQ("(+ 2i [3i 5i])", "[5i 7i]");
    TEST_ASSERT_EQ("(+ 2i [3 5])", "[5 7]");
    TEST_ASSERT_EQ("(+ 2i [3.1 5.2])", "[5.1 7.2]");
    TEST_ASSERT_EQ("(+ 5i [2024.03.20 2023.02.07])", "[2024.03.25 2023.02.12]");
    TEST_ASSERT_EQ("(+ 60000i [20:15:07.000 15:41:47.087])", "[20:16:07.000 15:42:47.087]");
    TEST_ASSERT_EQ("(+ 1000000000i [2025.03.04D15:41:47.087221025])", "[2025.03.04D15:41:48.087221025]");

    TEST_ASSERT_EQ("(+ -3 5i)", "2");
    TEST_ASSERT_EQ("(+ -3 5)", "2");
    TEST_ASSERT_EQ("(+ -3 5.2)", "2.2");
    TEST_ASSERT_EQ("(+ -3 2024.03.20)", "2024.03.17");
    TEST_ASSERT_EQ("(+ -3000 20:15:07.000)", "20:15:04.000");
    TEST_ASSERT_EQ("(+ -3000000000 2025.03.04D15:41:47.087221025)", "2025.03.04D15:41:44.087221025");
    TEST_ASSERT_EQ("(+ -2 [3i 5i])", "[1i 3i]");
    TEST_ASSERT_EQ("(+ -2 [3 5])", "[1 3]");
    TEST_ASSERT_EQ("(+ -2 [3.1 5.2])", "[1.1 3.2]");
    TEST_ASSERT_EQ("(+ -5 [2024.03.20 2023.02.07])", "[2024.03.15 2023.02.02]");
    TEST_ASSERT_EQ("(+ 60000 [20:15:07.000 15:41:47.087])", "[20:16:07.000 15:42:47.087]");
    TEST_ASSERT_EQ("(+ -3000000000 [2025.03.04D15:41:47.087221025])", "[2025.03.04D15:41:44.087221025]");

    TEST_ASSERT_EQ("(+ 3.1 5i)", "8.1");
    TEST_ASSERT_EQ("(+ 3.1 -5)", "-1.9");
    TEST_ASSERT_EQ("(+ 3.1 5.2)", "8.3");
    TEST_ASSERT_EQ("(+ 2.5 [3i 5i])", "[5.5 7.5]");
    TEST_ASSERT_EQ("(+ 2.5 [3 5])", "[5.5 7.5]");
    TEST_ASSERT_EQ("(+ 2.5 [3.1 5.2])", "[5.6 7.7]");

    TEST_ASSERT_EQ("(+ 2024.03.20 5i)", "2024.03.25");
    TEST_ASSERT_EQ("(+ 2024.03.20 5)", "2024.03.25");
    TEST_ASSERT_EQ("(+ 2024.03.20 20:15:03.020)", "2024.03.20D20:15:03.020000000");
    TEST_ASSERT_EQ("(+ 2024.03.20 [5i])", "[2024.03.25]");
    TEST_ASSERT_EQ("(+ 2024.03.20 [5 5])", "[2024.03.25 2024.03.25]");
    TEST_ASSERT_EQ("(+ 2024.03.20 [20:15:03.020])", "[2024.03.20D20:15:03.020000000]");

    TEST_ASSERT_EQ("(+ 20:15:07.000 60000i)", "20:16:07.000");
    TEST_ASSERT_EQ("(+ 20:15:07.000 60000)", "20:16:07.000");
    TEST_ASSERT_EQ("(+ 10:15:07.000 05:41:47.087)", "15:56:54.087");
    TEST_ASSERT_EQ("(+ 20:15:07.000 2025.01.01)", "2025.01.01D20:15:07.000000000");
    TEST_ASSERT_EQ("(+ 02:00:00.000 2025.01.01D20:15:07.000000000)", "2025.01.01D22:15:07.000000000");
    TEST_ASSERT_EQ("(+ 20:15:07.000 [60000i])", "[20:16:07.000]");
    TEST_ASSERT_EQ("(+ 20:15:07.000 [60000])", "[20:16:07.000]");
    TEST_ASSERT_EQ("(+ 10:15:07.000 [05:41:47.087])", "[15:56:54.087]");
    TEST_ASSERT_EQ("(+ 20:15:07.000 [2025.01.01])", "[2025.01.01D20:15:07.000000000]");
    TEST_ASSERT_EQ("(+ 02:00:00.000 [2025.01.01D20:15:07.000000000])", "[2025.01.01D22:15:07.000000000]");

    TEST_ASSERT_EQ("(+ 2025.03.04D15:41:47.087221025 1000000000i)", "2025.03.04D15:41:48.087221025");
    TEST_ASSERT_EQ("(+ 2025.03.04D15:41:47.087221025 3000000000)", "2025.03.04D15:41:50.087221025");
    TEST_ASSERT_EQ("(+ 2025.03.04D15:41:47.087221025 01:01:00.000)", "2025.03.04D16:42:47.087221025");
    TEST_ASSERT_EQ("(+ 2025.03.04D15:41:47.087221025 [1000000000i])", "[2025.03.04D15:41:48.087221025]");
    TEST_ASSERT_EQ("(+ 2025.03.04D15:41:47.087221025 [3000000000])", "[2025.03.04D15:41:50.087221025]");
    TEST_ASSERT_EQ("(+ 2025.03.04D15:41:47.087221025 [01:01:00.000])", "[2025.03.04D16:42:47.087221025]");

    TEST_ASSERT_EQ("(+ [3i 5i] 2i)", "[5i 7i]");
    TEST_ASSERT_EQ("(+ [3i 5i] 2)", "[5 7]");
    TEST_ASSERT_EQ("(+ [3i 5i] 2.5)", "[5.5 7.5]");
    TEST_ASSERT_EQ("(+ [3i 5i] 2024.03.20)", "[2024.03.23 2024.03.25]");
    TEST_ASSERT_EQ("(+ [3i 5i] 20:15:07.000)", "[20:15:07.003 20:15:07.005]");
    TEST_ASSERT_EQ("(+ [3i] 2025.03.04D15:41:47.087221025)", "[2025.03.04D15:41:47.087221028]");
    TEST_ASSERT_EQ("(+ [3i 5i] [2i 3i])", "[5i 8i]");
    TEST_ASSERT_EQ("(+ [3i 5i] [2 3])", "[5 8]");
    TEST_ASSERT_EQ("(+ [3i 5i] [2.2 3.3])", "[5.2 8.3]");
    TEST_ASSERT_EQ("(+ [3i 5i] [2024.03.20 2024.03.20])", "[2024.03.23 2024.03.25]");
    TEST_ASSERT_EQ("(+ [3i 5i] [20:15:07.000 20:15:07.000])", "[20:15:07.003 20:15:07.005]");
    TEST_ASSERT_EQ("(+ [-3i] [2025.03.04D15:41:47.087221025])", "[2025.03.04D15:41:47.087221022]");

    TEST_ASSERT_EQ("(+ [3 -5] 2i)", "[5 -3]");
    TEST_ASSERT_EQ("(+ [3 -5] 2)", "[5 -3]");
    TEST_ASSERT_EQ("(+ [3 -5] 2.5)", "[5.5 -2.5]");
    TEST_ASSERT_EQ("(+ [3 -5] 2024.03.20)", "[2024.03.23 2024.03.15]");
    TEST_ASSERT_EQ("(+ [3 -5] 20:15:07.000)", "[20:15:07.003 20:15:06.995]");
    TEST_ASSERT_EQ("(+ [3 -5] 2025.03.04D15:41:47.087221025)",
                   "[2025.03.04D15:41:47.087221028 2025.03.04D15:41:47.087221020]");
    TEST_ASSERT_EQ("(+ [3 -5] [2i 3i])", "[5 -2]");
    TEST_ASSERT_EQ("(+ [3 -5] [2 3])", "[5 -2]");
    TEST_ASSERT_EQ("(+ [3 -5] [2.2 3.3])", "[5.2 -1.7]");
    TEST_ASSERT_EQ("(+ [-3] [2024.03.20])", "[2024.03.17]");
    TEST_ASSERT_EQ("(+ [-3] [20:15:07.000])", "[20:15:06.997]");
    TEST_ASSERT_EQ("(+ [-3] [2025.03.04D15:41:47.087221025])", "[2025.03.04D15:41:47.087221022]");

    TEST_ASSERT_EQ("(+ [3.1 5.2] 2i)", "[5.1 7.2]");
    TEST_ASSERT_EQ("(+ [3.1 5.2] 2)", "[5.1 7.2]");
    TEST_ASSERT_EQ("(+ [3.1 5.2] 2.5)", "[5.6 7.7]");
    TEST_ASSERT_EQ("(+ [3.1 -5.2] [2i 3i])", "[5.1 -2.2]");
    TEST_ASSERT_EQ("(+ [3.1 -5.2] [2 3])", "[5.1 -2.2]");
    TEST_ASSERT_EQ("(+ [3.1 -5.2] [2.2 3.3])", "[5.3 -1.9]");

    TEST_ASSERT_EQ("(+ [2024.03.20 2023.02.07] 5i)", "[2024.03.25 2023.02.12]");
    TEST_ASSERT_EQ("(+ [2024.03.20 2023.02.07] 5)", "[2024.03.25 2023.02.12]");
    TEST_ASSERT_EQ("(+ [2024.03.20 2023.02.07] 20:15:07.000)",
                   "[2024.03.20D20:15:07.000000000 2023.02.07D20:15:07.000000000]");
    TEST_ASSERT_EQ("(+ [2024.03.20 2023.02.07] [5i 10i])", "[2024.03.25 2023.02.17]");
    TEST_ASSERT_EQ("(+ [2024.03.20 2023.02.07] [5 10])", "[2024.03.25 2023.02.17]");
    TEST_ASSERT_EQ("(+ [2024.03.20] [20:15:07.000])", "[2024.03.20D20:15:07.000000000]");

    TEST_ASSERT_EQ("(+ [20:15:07.000 15:41:47.087] 60000i)", "[20:16:07.000 15:42:47.087]");
    TEST_ASSERT_EQ("(+ [20:15:07.000 15:41:47.087] 60000)", "[20:16:07.000 15:42:47.087]");
    TEST_ASSERT_EQ("(+ [20:15:07.000 15:41:47.087] 2022.01.15)",
                   "[2022.01.15D20:15:07.000000000 2022.01.15D15:41:47.087000000]");
    TEST_ASSERT_EQ("(+ [02:15:07.000 11:41:47.087] 10:30:00.000)", "[12:45:07.000 22:11:47.087]");
    TEST_ASSERT_EQ("(+ [02:00:00.000] 2025.03.04D15:41:47.087221025)", "[2025.03.04D17:41:47.087221025]");
    TEST_ASSERT_EQ("(+ [20:15:07.000] [60000i])", "[20:16:07.000]");
    TEST_ASSERT_EQ("(+ [20:15:07.000] [60000])", "[20:16:07.000]");
    TEST_ASSERT_EQ("(+ [20:15:07.000] [2022.01.15])", "[2022.01.15D20:15:07.000000000]");
    TEST_ASSERT_EQ("(+ [02:15:07.000] [11:41:47.087])", "[13:56:54.087]");
    TEST_ASSERT_EQ("(+ [02:00:00.000] [2025.03.04D15:41:47.087221025])", "[2025.03.04D17:41:47.087221025]");

    TEST_ASSERT_EQ("(+ [2025.03.04D15:41:47.087221025] 1000000000i)", "[2025.03.04D15:41:48.087221025]");
    TEST_ASSERT_EQ("(+ [2025.03.04D15:41:47.087221025] 3000000000)", "[2025.03.04D15:41:50.087221025]");
    TEST_ASSERT_EQ("(+ [2025.03.04D15:41:47.087221025] 01:01:00.000)", "[2025.03.04D16:42:47.087221025]");
    TEST_ASSERT_EQ("(+ [2025.03.04D15:41:47.087221025] [1000000000i])", "[2025.03.04D15:41:48.087221025]");
    TEST_ASSERT_EQ("(+ [2025.03.04D15:41:47.087221025] [3000000000])", "[2025.03.04D15:41:50.087221025]");
    TEST_ASSERT_EQ("(+ [2025.03.04D15:41:47.087221025] [01:01:00.000])", "[2025.03.04D16:42:47.087221025]");
    TEST_ASSERT_ER("(+ 2025.03.04D15:41:47.087221025 2025.12.13)", "add: unsupported types: 'timestamp, 'date");

    TEST_ASSERT_EQ("(+ (list -10i -10 -10.0) 5)", "(list -5 -5 -5.0)");
    TEST_ASSERT_EQ("(+ (list -10i -10i -10.0) 5)", "(list -5 -5 -5.0)");
    TEST_ASSERT_EQ("(+ (list -10 -10i -10.0) 5i)", "(list -5i -5i -5.0)");

    TEST_ASSERT_EQ("(- 3i 5i)", "-2i");
    TEST_ASSERT_EQ("(- 3i 5)", "-2");
    TEST_ASSERT_EQ("(- 3i 5.2)", "-2.2");
    TEST_ASSERT_EQ("(- 3i 20:15:07.000)", "-20:15:06.997");
    TEST_ASSERT_EQ("(- 2i [3i 5i])", "[-1i -3i]");
    TEST_ASSERT_EQ("(- 2i [3 5])", "[-1 -3]");
    TEST_ASSERT_EQ("(- 2i [3.1 5.2])", "[-1.1 -3.2]");
    TEST_ASSERT_EQ("(- 60000i [20:15:07.000 15:41:47.087])", "[-20:14:07.000 -15:40:47.087]");

    TEST_ASSERT_EQ("(- -3 5i)", "-8i");
    TEST_ASSERT_EQ("(- -3 5)", "-8");
    TEST_ASSERT_EQ("(- -3 5.2)", "-8.2");
    TEST_ASSERT_EQ("(- -3 20:15:07.000)", "-20:15:07.003");
    TEST_ASSERT_EQ("(- -2 [3i 5i])", "[-5i -7i]");
    TEST_ASSERT_EQ("(- -2 [3 5])", "[-5 -7]");
    TEST_ASSERT_EQ("(- -2 [3.1 5.2])", "[-5.1 -7.2]");
    TEST_ASSERT_EQ("(- 60000 [20:15:07.000 15:41:47.087])", "[-20:14:07.000 -15:40:47.087]");

    TEST_ASSERT_EQ("(- 3.1 5i)", "-1.9");
    TEST_ASSERT_EQ("(- 3.1 -5)", "8.1");
    TEST_ASSERT_EQ("(- 3.1 5.2)", "-2.1");
    TEST_ASSERT_EQ("(- 2.5 [3i 5i])", "[-0.5 -2.5]");
    TEST_ASSERT_EQ("(- 2.5 [3 5])", "[-0.5 -2.5]");
    TEST_ASSERT_EQ("(- 2.5 [3.1 5.2])", "[-0.6 -2.7]");
    TEST_ASSERT_EQ("(- -0.00 0.00)", "0.00")
    TEST_ASSERT_EQ("(- -0.00 0Nf)", "0Nf")

    TEST_ASSERT_EQ("(- 2024.03.20 5i)", "2024.03.15");
    TEST_ASSERT_EQ("(- 2024.03.20 5)", "2024.03.15");
    TEST_ASSERT_EQ("(- 2024.03.20 2023.02.07)", "407i");
    TEST_ASSERT_EQ("(- 2024.03.20 20:15:03.020)", "2024.03.19D03:44:56.980000000");
    TEST_ASSERT_EQ("(- 2024.03.20 [5i])", "[2024.03.15]");
    TEST_ASSERT_EQ("(- 2024.03.20 [5 5])", "[2024.03.15 2024.03.15]");
    TEST_ASSERT_EQ("(- 2024.03.20 [2023.02.07])", "[407i]");
    TEST_ASSERT_EQ("(- 2024.03.20 [20:15:03.020])", "[2024.03.19D03:44:56.980000000]");

    TEST_ASSERT_EQ("(- 20:15:07.000 60000i)", "20:14:07.000");
    TEST_ASSERT_EQ("(- 20:15:07.000 60000)", "20:14:07.000");
    TEST_ASSERT_EQ("(- 10:15:07.000 05:41:47.087)", "04:33:19.913");
    TEST_ASSERT_EQ("(- 20:15:07.000 [60000i])", "[20:14:07.000]");
    TEST_ASSERT_EQ("(- 20:15:07.000 [60000])", "[20:14:07.000]");
    TEST_ASSERT_EQ("(- 10:15:07.000 [05:41:47.087])", "[04:33:19.913]");

    TEST_ASSERT_EQ("(- 2025.03.04D15:41:47.087221025 1000000000i)", "2025.03.04D15:41:46.087221025");
    TEST_ASSERT_EQ("(- 2025.03.04D15:41:47.087221025 3000000000)", "2025.03.04D15:41:44.087221025");
    TEST_ASSERT_EQ("(- 2025.03.04D15:41:47.087221025 01:01:00.000)", "2025.03.04D14:40:47.087221025");
    TEST_ASSERT_EQ("(- 2025.03.04D15:41:47.087221025 2025.03.04D15:41:47.087221025)", "0");
    TEST_ASSERT_EQ("(- 2025.03.04D15:41:47.087221025 [1000000000i])", "[2025.03.04D15:41:46.087221025]");
    TEST_ASSERT_EQ("(- 2025.03.04D15:41:47.087221025 [3000000000])", "[2025.03.04D15:41:44.087221025]");
    TEST_ASSERT_EQ("(- 2025.03.04D15:41:47.087221025 [01:01:00.000])", "[2025.03.04D14:40:47.087221025]");
    TEST_ASSERT_EQ("(- 2025.03.04D15:41:47.087221025 [2025.03.04D15:41:47.087221025])", "[0]");

    TEST_ASSERT_EQ("(- [3i 5i] 2i)", "[1i 3i]");
    TEST_ASSERT_EQ("(- [3i 5i] 2)", "[1 3]");
    TEST_ASSERT_EQ("(- [3i 5i] 2.5)", "[0.5 2.5]");
    TEST_ASSERT_EQ("(- [3i 5i] 20:15:07.000)", "[-20:15:06.997 -20:15:06.995]");
    TEST_ASSERT_EQ("(- [3i 5i] [2i 3i])", "[1i 2i]");
    TEST_ASSERT_EQ("(- [3i 5i] [2 3])", "[1 2]");
    TEST_ASSERT_EQ("(- [3i 5i] [2.2 3.3])", "[0.8 1.7]");
    TEST_ASSERT_EQ("(- [3i 5i] [20:15:07.000 20:15:07.000])", "[-20:15:06.997 -20:15:06.995]");

    TEST_ASSERT_EQ("(- [3 -5] 2i)", "[1 -7]");
    TEST_ASSERT_EQ("(- [3 -5] 2)", "[1 -7]");
    TEST_ASSERT_EQ("(- [3 -5] 2.5)", "[0.5 -7.5]");
    TEST_ASSERT_EQ("(- [3 -5] 20:15:07.000)", "[-20:15:06.997 -20:15:07.005]");
    TEST_ASSERT_EQ("(- [3 -5] [2i 3i])", "[1 -8]");
    TEST_ASSERT_EQ("(- [3 -5] [2 3])", "[1 -8]");
    TEST_ASSERT_EQ("(- [3 -5] [2.2 3.3])", "[0.8 -8.3]");
    TEST_ASSERT_EQ("(- [-3] [20:15:07.000])", "[-20:15:07.003]");

    TEST_ASSERT_EQ("(- [3.1 5.2] 2i)", "[1.1 3.2]");
    TEST_ASSERT_EQ("(- [3.1 5.2] 2)", "[1.1 3.2]");
    TEST_ASSERT_EQ("(- [3.1 5.2] 2.5)", "[0.6 2.7]");
    TEST_ASSERT_EQ("(- [3.1 -5.2] [2i 3i])", "[1.1 -8.2]");
    TEST_ASSERT_EQ("(- [3.1 -5.2] [2 3])", "[1.1 -8.2]");
    TEST_ASSERT_EQ("(- [3.1 -5.2] [2.2 3.3])", "[0.9 -8.5]");

    TEST_ASSERT_EQ("(- [2024.03.20 2023.02.07] 5i)", "[2024.03.15 2023.02.02]");
    TEST_ASSERT_EQ("(- [2024.03.20 2023.02.07] 5)", "[2024.03.15 2023.02.02]");
    TEST_ASSERT_EQ("(- [2024.03.20 2023.02.07] 2022.01.15)", "[795 388]");
    TEST_ASSERT_EQ("(- [2024.03.20 2023.02.07] 20:15:07.000)",
                   "[2024.03.19D03:44:53.000000000 2023.02.06D03:44:53.000000000]");
    TEST_ASSERT_EQ("(- [2024.03.20 2023.02.07] [5i 10i])", "[2024.03.15 2023.01.28]");
    TEST_ASSERT_EQ("(- [2024.03.20 2023.02.07] [5 10])", "[2024.03.15 2023.01.28]");
    TEST_ASSERT_EQ("(- [2024.03.20 2023.02.07] [2022.01.15 2026.12.31])", "[795 -1423]");
    TEST_ASSERT_EQ("(- [2024.03.20] [20:15:07.000])", "[2024.03.19D03:44:53.000000000]");

    TEST_ASSERT_EQ("(- [20:15:07.000 15:41:47.087] 60000i)", "[20:14:07.000 15:40:47.087]");
    TEST_ASSERT_EQ("(- [20:15:07.000 15:41:47.087] 60000)", "[20:14:07.000 15:40:47.087]");
    TEST_ASSERT_EQ("(- [02:15:07.000 11:41:47.087] 10:30:00.000)", "[-08:14:53.000 01:11:47.087]");
    TEST_ASSERT_EQ("(- [20:15:07.000] [60000i])", "[20:14:07.000]");
    TEST_ASSERT_EQ("(- [20:15:07.000] [60000])", "[20:14:07.000]");
    TEST_ASSERT_EQ("(- [02:15:07.000] [11:41:47.087])", "[-09:26:40.087]");

    TEST_ASSERT_EQ("(- [2025.03.04D15:41:47.087221025] 1000000000i)", "[2025.03.04D15:41:46.087221025]");
    TEST_ASSERT_EQ("(- [2025.03.04D15:41:47.087221025] 3000000000)", "[2025.03.04D15:41:44.087221025]");
    TEST_ASSERT_EQ("(- [2025.03.04D15:41:47.087221025] 01:01:00.000)", "[2025.03.04D14:40:47.087221025]");
    TEST_ASSERT_EQ("(- [2025.03.04D15:41:47.087221025] 2025.03.04D15:41:47.087221025)", "[0]");
    TEST_ASSERT_EQ("(- [2025.03.04D15:41:47.087221025] [1000000000i])", "[2025.03.04D15:41:46.087221025]");
    TEST_ASSERT_EQ("(- [2025.03.04D15:41:47.087221025] [3000000000])", "[2025.03.04D15:41:44.087221025]");
    TEST_ASSERT_EQ("(- [2025.03.04D15:41:47.087221025] [01:01:00.000])", "[2025.03.04D14:40:47.087221025]");
    TEST_ASSERT_EQ("(- [2025.03.04D15:41:47.087221025] [2025.03.04D15:41:47.087221025])", "[0]");
    TEST_ASSERT_ER("(- 2025.03.04D15:41:47.087221025 2025.12.13)", "sub: unsupported types: 'timestamp, 'date");

    TEST_ASSERT_EQ("(* 3i 5i)", "15i");
    TEST_ASSERT_EQ("(* 3i 5)", "15");
    TEST_ASSERT_EQ("(* 3i 5.2)", "15.6");
    TEST_ASSERT_EQ("(* 3i 0.2)", "0.6");
    TEST_ASSERT_EQ("(* 3i 02:15:07.000)", "06:45:21.000");
    TEST_ASSERT_EQ("(* 2i [3i 5i])", "[6i 10i]");
    TEST_ASSERT_EQ("(* 2i [3 5])", "[6 10]");
    TEST_ASSERT_EQ("(* 2i [3.1 5.2])", "[6.2 10.4]");
    TEST_ASSERT_EQ("(* 2i [20:15:07.000 15:41:47.087])", "[40:30:14.000 31:23:34.174]");
    TEST_ASSERT_EQ("(* 0Ni 15:12:10.000)", "0Nt");

    TEST_ASSERT_EQ("(* -3 5i)", "-15");
    TEST_ASSERT_EQ("(* -3 5)", "-15");
    TEST_ASSERT_EQ("(* -3 5.2)", "-15.6");
    TEST_ASSERT_EQ("(* -2 00:15:07.000)", "-00:30:14.000");
    TEST_ASSERT_EQ("(* -2 [3i 5i])", "[-6 -10]");
    TEST_ASSERT_EQ("(* -2 [3 5])", "[-6 -10]");
    TEST_ASSERT_EQ("(* -2 [3.1 5.2])", "[-6.2 -10.4]");
    TEST_ASSERT_EQ("(* 6 [00:15:07.000 00:41:47.087])", "[01:30:42.000 04:10:42.522]");
    TEST_ASSERT_EQ("(* 0 -5.50)", "0.00");
    TEST_ASSERT_EQ("(* -10 [0.0])", "[0.00]");

    TEST_ASSERT_EQ("(* 3.1 5i)", "15.5");
    TEST_ASSERT_EQ("(* 3.1 -5)", "-15.5");
    TEST_ASSERT_EQ("(* 3.1 5.2)", "16.12");
    TEST_ASSERT_EQ("(* 2.5 [3i 5i])", "[7.5 12.5]");
    TEST_ASSERT_EQ("(* 2.5 [3 5])", "[7.5 12.5]");
    TEST_ASSERT_EQ("(* 2.5 [3.1 5.2])", "[7.75 13.0]");

    TEST_ASSERT_EQ("(* 00:15:07.000 6i)", "01:30:42.000");
    TEST_ASSERT_EQ("(* 00:15:07.000 6)", "01:30:42.000");
    TEST_ASSERT_EQ("(* 00:15:07.000 [6i])", "[01:30:42.000]");
    TEST_ASSERT_EQ("(* 00:15:07.000 [6])", "[01:30:42.000]");

    TEST_ASSERT_EQ("(* [3i 5i] 2i)", "[6i 10i]");
    TEST_ASSERT_EQ("(* [3i 5i] 2)", "[6 10]");
    TEST_ASSERT_EQ("(* [3i 5i] 2.5)", "[7.5 12.5]");
    TEST_ASSERT_EQ("(* [3i 5i] 02:15:07.000)", "[06:45:21.000 11:15:35.000]");
    TEST_ASSERT_EQ("(* [3i 5i] [2i 3i])", "[6i 15i]");
    TEST_ASSERT_EQ("(* [3i 5i] [2 3])", "[6 15]");
    TEST_ASSERT_EQ("(* [3i 5i] [2.2 3.3])", "[6.6 16.5]");
    TEST_ASSERT_EQ("(* [3i 5i] [00:15:07.000 01:15:07.000])", "[00:45:21.000 06:15:35.000]");

    TEST_ASSERT_EQ("(* [3 -5] 2i)", "[6 -10]");
    TEST_ASSERT_EQ("(* [3 -5] 2)", "[6 -10]");
    TEST_ASSERT_EQ("(* [3 -5] 2.5)", "[7.5 -12.5]");
    TEST_ASSERT_EQ("(* [3 -5] 03:15:07.000)", "[09:45:21.000 -16:15:35.000]");
    TEST_ASSERT_EQ("(* [3 -5] [2i 3i])", "[6 -15]");
    TEST_ASSERT_EQ("(* [3 -5] [2 3])", "[6 -15]");
    TEST_ASSERT_EQ("(* [3 -5] [2.2 3.3])", "[6.6 -16.5]");
    TEST_ASSERT_EQ("(* [-3] [20:15:07.000])", "[-60:45:21.000]");

    TEST_ASSERT_EQ("(* [3.1 5.2] 2i)", "[6.2 10.4]");
    TEST_ASSERT_EQ("(* [3.1 5.2] 2)", "[6.2 10.4]");
    TEST_ASSERT_EQ("(* [3.1 5.2] 2.5)", "[7.75 13.0]");
    TEST_ASSERT_EQ("(* [3.1 -5.2] [2i 3i])", "[6.2 -15.6]");
    TEST_ASSERT_EQ("(* [3.1 -5.2] [2 3])", "[6.2 -15.6]");
    TEST_ASSERT_EQ("(* [3.1 -5.2] [2.2 3.3])", "[6.82 -17.16]");

    TEST_ASSERT_EQ("(* [02:15:07.000 05:41:47.087] 5i)", "[11:15:35.000 28:28:55.435]");
    TEST_ASSERT_EQ("(* [02:15:07.000 05:41:47.087] 5)", "[11:15:35.000 28:28:55.435]");
    TEST_ASSERT_EQ("(* [02:15:07.000] [5i])", "[11:15:35.000]");
    TEST_ASSERT_EQ("(* [02:15:07.000] [5])", "[11:15:35.000]");
    TEST_ASSERT_ER("(* 02:15:07.000 02:15:07.000)", "mul: unsupported types: 'time, 'time");

    TEST_ASSERT_EQ("(/ -5 -5)", "1");
    TEST_ASSERT_EQ("(/ -5 -2)", "2");
    TEST_ASSERT_EQ("(/ -5 0Nl)", "0Nl");
    TEST_ASSERT_EQ("(/ -5 0)", "0Nl");
    TEST_ASSERT_EQ("(/ -5 6)", "-1");
    TEST_ASSERT_EQ("(/ -5 -5i)", "1");
    TEST_ASSERT_EQ("(/ -5 0Ni)", "0Nl");
    TEST_ASSERT_EQ("(/ -5 0i)", "0Nl");
    TEST_ASSERT_EQ("(/ -5 6i)", "-1");
    TEST_ASSERT_EQ("(/ -5 -2.00)", "2");
    TEST_ASSERT_EQ("(/ -5 -1.00)", "5");
    TEST_ASSERT_EQ("(/ -5 -0.60)", "8");
    TEST_ASSERT_EQ("(/ -5 -0.00)", "0Nl");
    TEST_ASSERT_EQ("(/ -5 0Nf)", "0Nl");
    TEST_ASSERT_EQ("(/ -5 0.00)", "0Nl");
    TEST_ASSERT_EQ("(/ -5 0.60)", "-9");
    TEST_ASSERT_EQ("(/ -5 1.00)", "-5");
    TEST_ASSERT_EQ("(/ -5 3.00)", "-2");
    TEST_ASSERT_EQ("(/ -2 -5)", "0");
    TEST_ASSERT_EQ("(/ -2 -2)", "1");
    TEST_ASSERT_EQ("(/ -2 0Nl)", "0Nl");
    TEST_ASSERT_EQ("(/ -2 0)", "0Nl");
    TEST_ASSERT_EQ("(/ -2 6)", "-1");
    TEST_ASSERT_EQ("(/ -2 -5i)", "0");
    TEST_ASSERT_EQ("(/ -2 0Ni)", "0Nl");
    TEST_ASSERT_EQ("(/ -2 0i)", "0Nl");
    TEST_ASSERT_EQ("(/ -2 6i)", "-1");
    TEST_ASSERT_EQ("(/ -2 -2.00)", "1");
    TEST_ASSERT_EQ("(/ -2 -1.00)", "2");
    TEST_ASSERT_EQ("(/ -2 -0.60)", "3");
    TEST_ASSERT_EQ("(/ -2 -0.00)", "0Nl");
    TEST_ASSERT_EQ("(/ -2 0Nf)", "0Nl");
    TEST_ASSERT_EQ("(/ -2 0.00)", "0Nl");
    TEST_ASSERT_EQ("(/ -2 0.60)", "-4");
    TEST_ASSERT_EQ("(/ -2 1.00)", "-2");
    TEST_ASSERT_EQ("(/ -2 3.00)", "-1");
    TEST_ASSERT_EQ("(/ 0Nl -5)", "0Nl");
    TEST_ASSERT_EQ("(/ 0Nl -2)", "0Nl");
    TEST_ASSERT_EQ("(/ 0Nl 0Nl)", "0Nl");
    TEST_ASSERT_EQ("(/ 0Nl 0)", "0Nl");
    TEST_ASSERT_EQ("(/ 0Nl 6)", "0Nl");
    TEST_ASSERT_EQ("(/ 0Nl -5i)", "0Nl");
    TEST_ASSERT_EQ("(/ 0Nl 0Ni)", "0Nl");
    TEST_ASSERT_EQ("(/ 0Nl 0i)", "0Nl");
    TEST_ASSERT_EQ("(/ 0Nl 6i)", "0Nl");
    TEST_ASSERT_EQ("(/ 0Nl -2.00)", "0Nl");
    TEST_ASSERT_EQ("(/ 0Nl -1.00)", "0Nl");
    TEST_ASSERT_EQ("(/ 0Nl -0.60)", "0Nl");
    TEST_ASSERT_EQ("(/ 0Nl -0.00)", "0Nl");
    TEST_ASSERT_EQ("(/ 0Nl 0Nf)", "0Nl");
    TEST_ASSERT_EQ("(/ 0Nl 0.00)", "0Nl");
    TEST_ASSERT_EQ("(/ 0Nl 0.60)", "0Nl");
    TEST_ASSERT_EQ("(/ 0Nl 1.00)", "0Nl");
    TEST_ASSERT_EQ("(/ 0Nl 3.00)", "0Nl");
    TEST_ASSERT_EQ("(/ 0 -5)", "0");
    TEST_ASSERT_EQ("(/ 0 -2)", "0");
    TEST_ASSERT_EQ("(/ 0 0Nl)", "0Nl");
    TEST_ASSERT_EQ("(/ 0 0)", "0Nl");
    TEST_ASSERT_EQ("(/ 0 6)", "0");
    TEST_ASSERT_EQ("(/ 0 -5i)", "0");
    TEST_ASSERT_EQ("(/ 0 0Ni)", "0Nl");
    TEST_ASSERT_EQ("(/ 0 0i)", "0Nl");
    TEST_ASSERT_EQ("(/ 0 6i)", "0");
    TEST_ASSERT_EQ("(/ 0 -2.00)", "0");
    TEST_ASSERT_EQ("(/ 0 -1.00)", "0");
    TEST_ASSERT_EQ("(/ 0 -0.60)", "0");
    TEST_ASSERT_EQ("(/ 0 -0.00)", "0Nl");
    TEST_ASSERT_EQ("(/ 0 0Nf)", "0Nl");
    TEST_ASSERT_EQ("(/ 0 0.00)", "0Nl");
    TEST_ASSERT_EQ("(/ 0 0.60)", "0");
    TEST_ASSERT_EQ("(/ 0 1.00)", "0");
    TEST_ASSERT_EQ("(/ 0 3.00)", "0");
    TEST_ASSERT_EQ("(/ 6 -5)", "-2");
    TEST_ASSERT_EQ("(/ 6 -2)", "-3");
    TEST_ASSERT_EQ("(/ 6 0Nl)", "0Nl");
    TEST_ASSERT_EQ("(/ 6 0)", "0Nl");
    TEST_ASSERT_EQ("(/ 6 6)", "1");
    TEST_ASSERT_EQ("(/ 6 -5i)", "-2");
    TEST_ASSERT_EQ("(/ 6 0Ni)", "0Nl");
    TEST_ASSERT_EQ("(/ 6 0i)", "0Nl");
    TEST_ASSERT_EQ("(/ 6 6i)", "1");
    TEST_ASSERT_EQ("(/ 6 -2.00)", "-3");
    TEST_ASSERT_EQ("(/ 6 -1.00)", "-6");
    TEST_ASSERT_EQ("(/ 6 -0.60)", "-10");
    TEST_ASSERT_EQ("(/ 6 -0.00)", "0Nl");
    TEST_ASSERT_EQ("(/ 6 0Nf)", "0Nl");
    TEST_ASSERT_EQ("(/ 6 0.00)", "0Nl");
    TEST_ASSERT_EQ("(/ 6 0.60)", "10");
    TEST_ASSERT_EQ("(/ 6 1.00)", "6");
    TEST_ASSERT_EQ("(/ 6 3.00)", "2");
    TEST_ASSERT_EQ("(/ -5i -5)", "1");
    TEST_ASSERT_EQ("(/ -5i -2)", "2");
    TEST_ASSERT_EQ("(/ -5i 0Nl)", "0Ni");
    TEST_ASSERT_EQ("(/ -5i 0)", "0Ni");
    TEST_ASSERT_EQ("(/ -5i 6)", "-1");
    TEST_ASSERT_EQ("(/ -5i -5i)", "1");
    TEST_ASSERT_EQ("(/ -5i 0Ni)", "0Ni");
    TEST_ASSERT_EQ("(/ -5i 0i)", "0Ni");
    TEST_ASSERT_EQ("(/ -5i 6i)", "-1");
    TEST_ASSERT_EQ("(/ -5i -2.00)", "2");
    TEST_ASSERT_EQ("(/ -5i -1.00)", "5");
    TEST_ASSERT_EQ("(/ -5i -0.60)", "8");
    TEST_ASSERT_EQ("(/ -5i -0.00)", "0Ni");
    TEST_ASSERT_EQ("(/ -5i 0Nf)", "0Ni");
    TEST_ASSERT_EQ("(/ -5i 0.00)", "0Ni");
    TEST_ASSERT_EQ("(/ -5i 0.60)", "-9");
    TEST_ASSERT_EQ("(/ -5i 1.00)", "-5");
    TEST_ASSERT_EQ("(/ -5i 3.00)", "-2");
    TEST_ASSERT_EQ("(/ 0Ni -5)", "0Ni");
    TEST_ASSERT_EQ("(/ 0Ni -2)", "0Ni");
    TEST_ASSERT_EQ("(/ 0Ni 0Nl)", "0Ni");
    TEST_ASSERT_EQ("(/ 0Ni 0)", "0Ni");
    TEST_ASSERT_EQ("(/ 0Ni 6)", "0Ni");
    TEST_ASSERT_EQ("(/ 0Ni -5i)", "0Ni");
    TEST_ASSERT_EQ("(/ 0Ni 0Ni)", "0Ni");
    TEST_ASSERT_EQ("(/ 0Ni 0i)", "0Ni");
    TEST_ASSERT_EQ("(/ 0Ni 6i)", "0Ni");
    TEST_ASSERT_EQ("(/ 0Ni -2.00)", "0Ni");
    TEST_ASSERT_EQ("(/ 0Ni -1.00)", "0Ni");
    TEST_ASSERT_EQ("(/ 0Ni -0.60)", "0Ni");
    TEST_ASSERT_EQ("(/ 0Ni -0.00)", "0Ni");
    TEST_ASSERT_EQ("(/ 0Ni 0Nf)", "0Ni");
    TEST_ASSERT_EQ("(/ 0Ni 0.00)", "0Ni");
    TEST_ASSERT_EQ("(/ 0Ni 0.60)", "0Ni");
    TEST_ASSERT_EQ("(/ 0Ni 1.00)", "0Ni");
    TEST_ASSERT_EQ("(/ 0Ni 3.00)", "0Ni");
    TEST_ASSERT_EQ("(/ 0i -5)", "0");
    TEST_ASSERT_EQ("(/ 0i -2)", "0");
    TEST_ASSERT_EQ("(/ 0i 0Nl)", "0Ni");
    TEST_ASSERT_EQ("(/ 0i 0)", "0Ni");
    TEST_ASSERT_EQ("(/ 0i 6)", "0");
    TEST_ASSERT_EQ("(/ 0i -5i)", "0");
    TEST_ASSERT_EQ("(/ 0i 0Ni)", "0Ni");
    TEST_ASSERT_EQ("(/ 0i 0i)", "0Ni");
    TEST_ASSERT_EQ("(/ 0i 6i)", "0");
    TEST_ASSERT_EQ("(/ 0i -2.00)", "0");
    TEST_ASSERT_EQ("(/ 0i -1.00)", "0");
    TEST_ASSERT_EQ("(/ 0i -0.60)", "0");
    TEST_ASSERT_EQ("(/ 0i -0.00)", "0Ni");
    TEST_ASSERT_EQ("(/ 0i 0Nf)", "0Ni");
    TEST_ASSERT_EQ("(/ 0i 0.00)", "0Ni");
    TEST_ASSERT_EQ("(/ 0i 0.60)", "0");
    TEST_ASSERT_EQ("(/ 0i 1.00)", "0");
    TEST_ASSERT_EQ("(/ 0i 3.00)", "0");
    TEST_ASSERT_EQ("(/ 6i -5)", "-2");
    TEST_ASSERT_EQ("(/ 6i -2)", "-3");
    TEST_ASSERT_EQ("(/ 6i 0Nl)", "0Ni");
    TEST_ASSERT_EQ("(/ 6i 0)", "0Ni");
    TEST_ASSERT_EQ("(/ 6i 6)", "1");
    TEST_ASSERT_EQ("(/ 6i -5i)", "-2");
    TEST_ASSERT_EQ("(/ 6i 0Ni)", "0Ni");
    TEST_ASSERT_EQ("(/ 6i 0i)", "0Ni");
    TEST_ASSERT_EQ("(/ 6i 6i)", "1");
    TEST_ASSERT_EQ("(/ 6i -2.00)", "-3");
    TEST_ASSERT_EQ("(/ 6i -1.00)", "-6");
    TEST_ASSERT_EQ("(/ 6i -0.60)", "-10");
    TEST_ASSERT_EQ("(/ 6i -0.00)", "0Ni");
    TEST_ASSERT_EQ("(/ 6i 0Nf)", "0Ni");
    TEST_ASSERT_EQ("(/ 6i 0.00)", "0Ni");
    TEST_ASSERT_EQ("(/ 6i 0.60)", "10");
    TEST_ASSERT_EQ("(/ 6i 1.00)", "6");
    TEST_ASSERT_EQ("(/ 6i 3.00)", "2");
    TEST_ASSERT_EQ("(/ -2.00 -5)", "0.00");
    TEST_ASSERT_EQ("(/ -2.00 -2)", "1.00");
    TEST_ASSERT_EQ("(/ -2.00 0Nl)", "0Nf");
    TEST_ASSERT_EQ("(/ -2.00 0)", "0Nf");
    TEST_ASSERT_EQ("(/ -2.00 6)", "-1.00");
    TEST_ASSERT_EQ("(/ -2.00 -5i)", "0.00");
    TEST_ASSERT_EQ("(/ -2.00 0Ni)", "0Nf");
    TEST_ASSERT_EQ("(/ -2.00 0i)", "0Nf");
    TEST_ASSERT_EQ("(/ -2.00 6i)", "-1.00");
    TEST_ASSERT_EQ("(/ -2.00 -2.00)", "1.00");
    TEST_ASSERT_EQ("(/ -2.00 -1.00)", "2.00");
    TEST_ASSERT_EQ("(/ -2.00 -0.60)", "3.00");
    TEST_ASSERT_EQ("(/ -2.00 -0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ -2.00 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(/ -2.00 0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ -2.00 0.60)", "-4.00");
    TEST_ASSERT_EQ("(/ -2.00 1.00)", "-2.00");
    TEST_ASSERT_EQ("(/ -2.00 3.00)", "-1.00");
    TEST_ASSERT_EQ("(/ -1.00 -5)", "0.00");
    TEST_ASSERT_EQ("(/ -1.00 -2)", "0.00");
    TEST_ASSERT_EQ("(/ -1.00 0Nl)", "0Nf");
    TEST_ASSERT_EQ("(/ -1.00 0)", "0Nf");
    TEST_ASSERT_EQ("(/ -1.00 6)", "-1.00");
    TEST_ASSERT_EQ("(/ -1.00 -5i)", "0.00");
    TEST_ASSERT_EQ("(/ -1.00 0Ni)", "0Nf");
    TEST_ASSERT_EQ("(/ -1.00 0i)", "0Nf");
    TEST_ASSERT_EQ("(/ -1.00 6i)", "-1.00");
    TEST_ASSERT_EQ("(/ -1.00 -2.00)", "0.00");
    TEST_ASSERT_EQ("(/ -1.00 -1.00)", "1.00");
    TEST_ASSERT_EQ("(/ -1.00 -0.60)", "1.00");
    TEST_ASSERT_EQ("(/ -1.00 -0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ -1.00 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(/ -1.00 0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ -1.00 0.60)", "-2.00");
    TEST_ASSERT_EQ("(/ -1.00 1.00)", "-1.00");
    TEST_ASSERT_EQ("(/ -1.00 3.00)", "-1.00");
    TEST_ASSERT_EQ("(/ -0.60 -5)", "0.00");
    TEST_ASSERT_EQ("(/ -0.60 -2)", "0.00");
    TEST_ASSERT_EQ("(/ -0.60 0Nl)", "0Nf");
    TEST_ASSERT_EQ("(/ -0.60 0)", "0Nf");
    TEST_ASSERT_EQ("(/ -0.60 6)", "-1.00");
    TEST_ASSERT_EQ("(/ -0.60 -5i)", "0.00");
    TEST_ASSERT_EQ("(/ -0.60 0Ni)", "0Nf");
    TEST_ASSERT_EQ("(/ -0.60 0i)", "0Nf");
    TEST_ASSERT_EQ("(/ -0.60 6i)", "-1.00");
    TEST_ASSERT_EQ("(/ -0.60 -2.00)", "0.00");
    TEST_ASSERT_EQ("(/ -0.60 -1.00)", "0.00");
    TEST_ASSERT_EQ("(/ -0.60 -0.60)", "1.00");
    TEST_ASSERT_EQ("(/ -0.60 -0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ -0.60 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(/ -0.60 0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ -0.60 0.60)", "-1.00");
    TEST_ASSERT_EQ("(/ -0.60 1.00)", "-1.00");
    TEST_ASSERT_EQ("(/ -0.60 3.00)", "-1.00");
    TEST_ASSERT_EQ("(/ -0.00 -5)", "0.00");
    TEST_ASSERT_EQ("(/ -0.00 -2)", "0.00");
    TEST_ASSERT_EQ("(/ -0.00 0Nl)", "0Nf");
    TEST_ASSERT_EQ("(/ -0.00 0)", "0Nf");
    TEST_ASSERT_EQ("(/ -0.00 6)", "0.00");
    TEST_ASSERT_EQ("(/ -0.00 -5i)", "0.00");
    TEST_ASSERT_EQ("(/ -0.00 0Ni)", "0Nf");
    TEST_ASSERT_EQ("(/ -0.00 0i)", "0Nf");
    TEST_ASSERT_EQ("(/ -0.00 6i)", "0.00");
    TEST_ASSERT_EQ("(/ -0.00 -2.00)", "0.00");
    TEST_ASSERT_EQ("(/ -0.00 -1.00)", "0.00");
    TEST_ASSERT_EQ("(/ -0.00 -0.60)", "0.00");
    TEST_ASSERT_EQ("(/ -0.00 -0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ -0.00 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(/ -0.00 0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ -0.00 0.60)", "0.00");
    TEST_ASSERT_EQ("(/ -0.00 1.00)", "0.00");
    TEST_ASSERT_EQ("(/ -0.00 3.00)", "0.00");
    TEST_ASSERT_EQ("(/ 0Nf -5)", "0Nf");
    TEST_ASSERT_EQ("(/ 0Nf -2)", "0Nf");
    TEST_ASSERT_EQ("(/ 0Nf 0Nl)", "0Nf");
    TEST_ASSERT_EQ("(/ 0Nf 0)", "0Nf");
    TEST_ASSERT_EQ("(/ 0Nf 6)", "0Nf");
    TEST_ASSERT_EQ("(/ 0Nf -5i)", "0Nf");
    TEST_ASSERT_EQ("(/ 0Nf 0Ni)", "0Nf");
    TEST_ASSERT_EQ("(/ 0Nf 0i)", "0Nf");
    TEST_ASSERT_EQ("(/ 0Nf 6i)", "0Nf");
    TEST_ASSERT_EQ("(/ 0Nf -2.00)", "0Nf");
    TEST_ASSERT_EQ("(/ 0Nf -1.00)", "0Nf");
    TEST_ASSERT_EQ("(/ 0Nf -0.60)", "0Nf");
    TEST_ASSERT_EQ("(/ 0Nf -0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ 0Nf 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(/ 0Nf 0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ 0Nf 0.60)", "0Nf");
    TEST_ASSERT_EQ("(/ 0Nf 1.00)", "0Nf");
    TEST_ASSERT_EQ("(/ 0Nf 3.00)", "0Nf");
    TEST_ASSERT_EQ("(/ 0.00 -5)", "0.00");
    TEST_ASSERT_EQ("(/ 0.00 -2)", "0.00");
    TEST_ASSERT_EQ("(/ 0.00 0Nl)", "0Nf");
    TEST_ASSERT_EQ("(/ 0.00 0)", "0Nf");
    TEST_ASSERT_EQ("(/ 0.00 6)", "0.00");
    TEST_ASSERT_EQ("(/ 0.00 -5i)", "0.00");
    TEST_ASSERT_EQ("(/ 0.00 0Ni)", "0Nf");
    TEST_ASSERT_EQ("(/ 0.00 0i)", "0Nf");
    TEST_ASSERT_EQ("(/ 0.00 6i)", "0.00");
    TEST_ASSERT_EQ("(/ 0.00 -2.00)", "0.00");
    TEST_ASSERT_EQ("(/ 0.00 -1.00)", "0.00");
    TEST_ASSERT_EQ("(/ 0.00 -0.60)", "0.00");
    TEST_ASSERT_EQ("(/ 0.00 -0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ 0.00 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(/ 0.00 0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ 0.00 0.60)", "0.00");
    TEST_ASSERT_EQ("(/ 0.00 1.00)", "0.00");
    TEST_ASSERT_EQ("(/ 0.00 3.00)", "0.00");
    TEST_ASSERT_EQ("(/ 0.60 -5)", "-1.00");
    TEST_ASSERT_EQ("(/ 0.60 -2)", "-1.00");
    TEST_ASSERT_EQ("(/ 0.60 0Nl)", "0Nf");
    TEST_ASSERT_EQ("(/ 0.60 0)", "0Nf");
    TEST_ASSERT_EQ("(/ 0.60 6)", "0.00");
    TEST_ASSERT_EQ("(/ 0.60 -5i)", "-1.00");
    TEST_ASSERT_EQ("(/ 0.60 0Ni)", "0Nf");
    TEST_ASSERT_EQ("(/ 0.60 0i)", "0Nf");
    TEST_ASSERT_EQ("(/ 0.60 6i)", "0.00");
    TEST_ASSERT_EQ("(/ 0.60 -2.00)", "-1.00");
    TEST_ASSERT_EQ("(/ 0.60 -1.00)", "-1.00");
    TEST_ASSERT_EQ("(/ 0.60 -0.60)", "-1.00");
    TEST_ASSERT_EQ("(/ 0.60 -0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ 0.60 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(/ 0.60 0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ 0.60 0.60)", "1.00");
    TEST_ASSERT_EQ("(/ 0.60 1.00)", "0.00");
    TEST_ASSERT_EQ("(/ 0.60 3.00)", "0.00");
    TEST_ASSERT_EQ("(/ 1.00 -5)", "-1.00");
    TEST_ASSERT_EQ("(/ 1.00 -2)", "-1.00");
    TEST_ASSERT_EQ("(/ 1.00 0Nl)", "0Nf");
    TEST_ASSERT_EQ("(/ 1.00 0)", "0Nf");
    TEST_ASSERT_EQ("(/ 1.00 6)", "0.00");
    TEST_ASSERT_EQ("(/ 1.00 -5i)", "-1.00");
    TEST_ASSERT_EQ("(/ 1.00 0Ni)", "0Nf");
    TEST_ASSERT_EQ("(/ 1.00 0i)", "0Nf");
    TEST_ASSERT_EQ("(/ 1.00 6i)", "0.00");
    TEST_ASSERT_EQ("(/ 1.00 -2.00)", "-1.00");
    TEST_ASSERT_EQ("(/ 1.00 -1.00)", "-1.00");
    TEST_ASSERT_EQ("(/ 1.00 -0.60)", "-2.00");
    TEST_ASSERT_EQ("(/ 1.00 -0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ 1.00 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(/ 1.00 0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ 1.00 0.60)", "1.00");
    TEST_ASSERT_EQ("(/ 1.00 1.00)", "1.00");
    TEST_ASSERT_EQ("(/ 1.00 3.00)", "0.00");
    TEST_ASSERT_EQ("(/ 3.00 -5)", "-1.00");
    TEST_ASSERT_EQ("(/ 3.00 -2)", "-2.00");
    TEST_ASSERT_EQ("(/ 3.00 0Nl)", "0Nf");
    TEST_ASSERT_EQ("(/ 3.00 0)", "0Nf");
    TEST_ASSERT_EQ("(/ 3.00 6)", "0.00");
    TEST_ASSERT_EQ("(/ 3.00 -5i)", "-1.00");
    TEST_ASSERT_EQ("(/ 3.00 0Ni)", "0Nf");
    TEST_ASSERT_EQ("(/ 3.00 0i)", "0Nf");
    TEST_ASSERT_EQ("(/ 3.00 6i)", "0.00");
    TEST_ASSERT_EQ("(/ 3.00 -2.00)", "-2.00");
    TEST_ASSERT_EQ("(/ 3.00 -1.00)", "-3.00");
    TEST_ASSERT_EQ("(/ 3.00 -0.60)", "-5.00");
    TEST_ASSERT_EQ("(/ 3.00 -0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ 3.00 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(/ 3.00 0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ 3.00 0.60)", "5.00");
    TEST_ASSERT_EQ("(/ 3.00 1.00)", "3.00");
    TEST_ASSERT_EQ("(/ 3.00 3.00)", "1.00");

    TEST_ASSERT_EQ("(/ [-5] -5)", "[1]");
    TEST_ASSERT_EQ("(/ [-5] -2)", "[2]");
    TEST_ASSERT_EQ("(/ [-5] 0Nl)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] 0)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] 6)", "[-1]");
    TEST_ASSERT_EQ("(/ [-5] -5i)", "[1]");
    TEST_ASSERT_EQ("(/ [-5] 0Ni)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] 0i)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] 6i)", "[-1]");
    TEST_ASSERT_EQ("(/ [-5] -2.00)", "[2]");
    TEST_ASSERT_EQ("(/ [-5] -1.00)", "[5]");
    TEST_ASSERT_EQ("(/ [-5] -0.60)", "[8]");
    TEST_ASSERT_EQ("(/ [-5] -0.00)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] 0Nf)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] 0.00)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] 0.60)", "[-9]");
    TEST_ASSERT_EQ("(/ [-5] 1.00)", "[-5]");
    TEST_ASSERT_EQ("(/ [-5] 3.00)", "[-2]");
    TEST_ASSERT_EQ("(/ [-2] -5)", "[0]");
    TEST_ASSERT_EQ("(/ [-2] -2)", "[1]");
    TEST_ASSERT_EQ("(/ [-2] 0Nl)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] 0)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] 6)", "[-1]");
    TEST_ASSERT_EQ("(/ [-2] -5i)", "[0]");
    TEST_ASSERT_EQ("(/ [-2] 0Ni)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] 0i)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] 6i)", "[-1]");
    TEST_ASSERT_EQ("(/ [-2] -2.00)", "[1]");
    TEST_ASSERT_EQ("(/ [-2] -1.00)", "[2]");
    TEST_ASSERT_EQ("(/ [-2] -0.60)", "[3]");
    TEST_ASSERT_EQ("(/ [-2] -0.00)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] 0Nf)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] 0.00)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] 0.60)", "[-4]");
    TEST_ASSERT_EQ("(/ [-2] 1.00)", "[-2]");
    TEST_ASSERT_EQ("(/ [-2] 3.00)", "[-1]");
    TEST_ASSERT_EQ("(/ [0Nl] -5)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] -2)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] 0Nl)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] 0)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] 6)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] -5i)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] 0Ni)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] 0i)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] 6i)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] -2.00)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] -1.00)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] -0.60)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] -0.00)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] 0Nf)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] 0.00)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] 0.60)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] 1.00)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] 3.00)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0] -5)", "[0]");
    TEST_ASSERT_EQ("(/ [0] -2)", "[0]");
    TEST_ASSERT_EQ("(/ [0] 0Nl)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0] 0)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0] 6)", "[0]");
    TEST_ASSERT_EQ("(/ [0] -5i)", "[0]");
    TEST_ASSERT_EQ("(/ [0] 0Ni)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0] 0i)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0] 6i)", "[0]");
    TEST_ASSERT_EQ("(/ [0] -2.00)", "[0]");
    TEST_ASSERT_EQ("(/ [0] -1.00)", "[0]");
    TEST_ASSERT_EQ("(/ [0] -0.60)", "[0]");
    TEST_ASSERT_EQ("(/ [0] -0.00)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0] 0Nf)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0] 0.00)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0] 0.60)", "[0]");
    TEST_ASSERT_EQ("(/ [0] 1.00)", "[0]");
    TEST_ASSERT_EQ("(/ [0] 3.00)", "[0]");
    TEST_ASSERT_EQ("(/ [6] -5)", "[-2]");
    TEST_ASSERT_EQ("(/ [6] -2)", "[-3]");
    TEST_ASSERT_EQ("(/ [6] 0Nl)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [6] 0)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [6] 6)", "[1]");
    TEST_ASSERT_EQ("(/ [6] -5i)", "[-2]");
    TEST_ASSERT_EQ("(/ [6] 0Ni)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [6] 0i)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [6] 6i)", "[1]");
    TEST_ASSERT_EQ("(/ [6] -2.00)", "[-3]");
    TEST_ASSERT_EQ("(/ [6] -1.00)", "[-6]");
    TEST_ASSERT_EQ("(/ [6] -0.60)", "[-10]");
    TEST_ASSERT_EQ("(/ [6] -0.00)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [6] 0Nf)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [6] 0.00)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [6] 0.60)", "[10]");
    TEST_ASSERT_EQ("(/ [6] 1.00)", "[6]");
    TEST_ASSERT_EQ("(/ [6] 3.00)", "[2]");
    TEST_ASSERT_EQ("(/ [-5i] -5)", "[1]");
    TEST_ASSERT_EQ("(/ [-5i] -2)", "[2]");
    TEST_ASSERT_EQ("(/ [-5i] 0Nl)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] 0)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] 6)", "[-1]");
    TEST_ASSERT_EQ("(/ [-5i] -5i)", "[1]");
    TEST_ASSERT_EQ("(/ [-5i] 0Ni)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] 0i)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] 6i)", "[-1]");
    TEST_ASSERT_EQ("(/ [-5i] -2.00)", "[2]");
    TEST_ASSERT_EQ("(/ [-5i] -1.00)", "[5]");
    TEST_ASSERT_EQ("(/ [-5i] -0.60)", "[8]");
    TEST_ASSERT_EQ("(/ [-5i] -0.00)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] 0Nf)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] 0.00)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] 0.60)", "[-9]");
    TEST_ASSERT_EQ("(/ [-5i] 1.00)", "[-5]");
    TEST_ASSERT_EQ("(/ [-5i] 3.00)", "[-2]");
    TEST_ASSERT_EQ("(/ [0Ni] -5)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] -2)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] 0Nl)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] 0)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] 6)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] -5i)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] 0Ni)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] 0i)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] 6i)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] -2.00)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] -1.00)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] -0.60)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] -0.00)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] 0Nf)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] 0.00)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] 0.60)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] 1.00)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] 3.00)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0i] -5)", "[0]");
    TEST_ASSERT_EQ("(/ [0i] -2)", "[0]");
    TEST_ASSERT_EQ("(/ [0i] 0Nl)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0i] 0)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0i] 6)", "[0]");
    TEST_ASSERT_EQ("(/ [0i] -5i)", "[0]");
    TEST_ASSERT_EQ("(/ [0i] 0Ni)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0i] 0i)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0i] 6i)", "[0]");
    TEST_ASSERT_EQ("(/ [0i] -2.00)", "[0]");
    TEST_ASSERT_EQ("(/ [0i] -1.00)", "[0]");
    TEST_ASSERT_EQ("(/ [0i] -0.60)", "[0]");
    TEST_ASSERT_EQ("(/ [0i] -0.00)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0i] 0Nf)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0i] 0.00)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0i] 0.60)", "[0]");
    TEST_ASSERT_EQ("(/ [0i] 1.00)", "[0]");
    TEST_ASSERT_EQ("(/ [0i] 3.00)", "[0]");
    TEST_ASSERT_EQ("(/ [6i] -5)", "[-2]");
    TEST_ASSERT_EQ("(/ [6i] -2)", "[-3]");
    TEST_ASSERT_EQ("(/ [6i] 0Nl)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [6i] 0)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [6i] 6)", "[1]");
    TEST_ASSERT_EQ("(/ [6i] -5i)", "[-2]");
    TEST_ASSERT_EQ("(/ [6i] 0Ni)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [6i] 0i)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [6i] 6i)", "[1]");
    TEST_ASSERT_EQ("(/ [6i] -2.00)", "[-3]");
    TEST_ASSERT_EQ("(/ [6i] -1.00)", "[-6]");
    TEST_ASSERT_EQ("(/ [6i] -0.60)", "[-10]");
    TEST_ASSERT_EQ("(/ [6i] -0.00)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [6i] 0Nf)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [6i] 0.00)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [6i] 0.60)", "[10]");
    TEST_ASSERT_EQ("(/ [6i] 1.00)", "[6]");
    TEST_ASSERT_EQ("(/ [6i] 3.00)", "[2]");
    TEST_ASSERT_EQ("(/ [-2.00] -5)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-2.00] -2)", "[1.00]");
    TEST_ASSERT_EQ("(/ [-2.00] 0Nl)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] 0)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] 6)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-2.00] -5i)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-2.00] 0Ni)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] 0i)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] 6i)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-2.00] -2.00)", "[1.00]");
    TEST_ASSERT_EQ("(/ [-2.00] -1.00)", "[2.00]");
    TEST_ASSERT_EQ("(/ [-2.00] -0.60)", "[3.00]");
    TEST_ASSERT_EQ("(/ [-2.00] -0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] 0Nf)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] 0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] 0.60)", "[-4.00]");
    TEST_ASSERT_EQ("(/ [-2.00] 1.00)", "[-2.00]");
    TEST_ASSERT_EQ("(/ [-2.00] 3.00)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-1.00] -5)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-1.00] -2)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-1.00] 0Nl)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] 0)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] 6)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-1.00] -5i)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-1.00] 0Ni)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] 0i)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] 6i)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-1.00] -2.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-1.00] -1.00)", "[1.00]");
    TEST_ASSERT_EQ("(/ [-1.00] -0.60)", "[1.00]");
    TEST_ASSERT_EQ("(/ [-1.00] -0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] 0Nf)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] 0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] 0.60)", "[-2.00]");
    TEST_ASSERT_EQ("(/ [-1.00] 1.00)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-1.00] 3.00)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-0.60] -5)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] -2)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] 0Nl)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] 0)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] 6)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-0.60] -5i)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] 0Ni)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] 0i)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] 6i)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-0.60] -2.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] -1.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] -0.60)", "[1.00]");
    TEST_ASSERT_EQ("(/ [-0.60] -0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] 0Nf)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] 0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] 0.60)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-0.60] 1.00)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-0.60] 3.00)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-0.00] -5)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.00] -2)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.00] 0Nl)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.00] 0)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.00] 6)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.00] -5i)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.00] 0Ni)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.00] 0i)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.00] 6i)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.00] -2.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.00] -1.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.00] -0.60)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.00] -0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.00] 0Nf)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.00] 0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.00] 0.60)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.00] 1.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.00] 3.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [0Nf] -5)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] -2)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] 0Nl)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] 0)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] 6)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] -5i)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] 0Ni)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] 0i)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] 6i)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] -2.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] -1.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] -0.60)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] -0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] 0Nf)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] 0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] 0.60)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] 1.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] 3.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.00] -5)", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] -2)", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] 0Nl)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.00] 0)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.00] 6)", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] -5i)", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] 0Ni)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.00] 0i)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.00] 6i)", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] -2.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] -1.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] -0.60)", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] -0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.00] 0Nf)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.00] 0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.00] 0.60)", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] 1.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] 3.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] -5)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [0.60] -2)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [0.60] 0Nl)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] 0)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] 6)", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] -5i)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [0.60] 0Ni)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] 0i)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] 6i)", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] -2.00)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [0.60] -1.00)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [0.60] -0.60)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [0.60] -0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] 0Nf)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] 0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] 0.60)", "[1.00]");
    TEST_ASSERT_EQ("(/ [0.60] 1.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] 3.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [1.00] -5)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [1.00] -2)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [1.00] 0Nl)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] 0)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] 6)", "[0.00]");
    TEST_ASSERT_EQ("(/ [1.00] -5i)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [1.00] 0Ni)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] 0i)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] 6i)", "[0.00]");
    TEST_ASSERT_EQ("(/ [1.00] -2.00)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [1.00] -1.00)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [1.00] -0.60)", "[-2.00]");
    TEST_ASSERT_EQ("(/ [1.00] -0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] 0Nf)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] 0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] 0.60)", "[1.00]");
    TEST_ASSERT_EQ("(/ [1.00] 1.00)", "[1.00]");
    TEST_ASSERT_EQ("(/ [1.00] 3.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [3.00] -5)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [3.00] -2)", "[-2.00]");
    TEST_ASSERT_EQ("(/ [3.00] 0Nl)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [3.00] 0)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [3.00] 6)", "[0.00]");
    TEST_ASSERT_EQ("(/ [3.00] -5i)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [3.00] 0Ni)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [3.00] 0i)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [3.00] 6i)", "[0.00]");
    TEST_ASSERT_EQ("(/ [3.00] -2.00)", "[-2.00]");
    TEST_ASSERT_EQ("(/ [3.00] -1.00)", "[-3.00]");
    TEST_ASSERT_EQ("(/ [3.00] -0.60)", "[-5.00]");
    TEST_ASSERT_EQ("(/ [3.00] -0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [3.00] 0Nf)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [3.00] 0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [3.00] 0.60)", "[5.00]");
    TEST_ASSERT_EQ("(/ [3.00] 1.00)", "[3.00]");
    TEST_ASSERT_EQ("(/ [3.00] 3.00)", "[1.00]");

    TEST_ASSERT_EQ("(/ -5 [-5])", "[1]");
    TEST_ASSERT_EQ("(/ -5 [-2])", "[2]");
    TEST_ASSERT_EQ("(/ -5 [0Nl])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -5 [0])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -5 [6])", "[-1]");
    TEST_ASSERT_EQ("(/ -5 [-5i])", "[1]");
    TEST_ASSERT_EQ("(/ -5 [0Ni])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -5 [0i])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -5 [6i])", "[-1]");
    TEST_ASSERT_EQ("(/ -5 [-2.00])", "[2]");
    TEST_ASSERT_EQ("(/ -5 [-1.00])", "[5]");
    TEST_ASSERT_EQ("(/ -5 [-0.60])", "[8]");
    TEST_ASSERT_EQ("(/ -5 [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -5 [0Nf])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -5 [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -5 [0.60])", "[-9]");
    TEST_ASSERT_EQ("(/ -5 [1.00])", "[-5]");
    TEST_ASSERT_EQ("(/ -5 [3.00])", "[-2]");
    TEST_ASSERT_EQ("(/ -2 [-5])", "[0]");
    TEST_ASSERT_EQ("(/ -2 [-2])", "[1]");
    TEST_ASSERT_EQ("(/ -2 [0Nl])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -2 [0])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -2 [6])", "[-1]");
    TEST_ASSERT_EQ("(/ -2 [-5i])", "[0]");
    TEST_ASSERT_EQ("(/ -2 [0Ni])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -2 [0i])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -2 [6i])", "[-1]");
    TEST_ASSERT_EQ("(/ -2 [-2.00])", "[1]");
    TEST_ASSERT_EQ("(/ -2 [-1.00])", "[2]");
    TEST_ASSERT_EQ("(/ -2 [-0.60])", "[3]");
    TEST_ASSERT_EQ("(/ -2 [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -2 [0Nf])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -2 [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -2 [0.60])", "[-4]");
    TEST_ASSERT_EQ("(/ -2 [1.00])", "[-2]");
    TEST_ASSERT_EQ("(/ -2 [3.00])", "[-1]");
    TEST_ASSERT_EQ("(/ 0Nl [-5])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0Nl [-2])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0Nl [0Nl])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0Nl [0])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0Nl [6])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0Nl [-5i])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0Nl [0Ni])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0Nl [0i])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0Nl [6i])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0Nl [-2.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0Nl [-1.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0Nl [-0.60])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0Nl [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0Nl [0Nf])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0Nl [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0Nl [0.60])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0Nl [1.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0Nl [3.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0 [-5])", "[0]");
    TEST_ASSERT_EQ("(/ 0 [-2])", "[0]");
    TEST_ASSERT_EQ("(/ 0 [0Nl])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0 [0])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0 [6])", "[0]");
    TEST_ASSERT_EQ("(/ 0 [-5i])", "[0]");
    TEST_ASSERT_EQ("(/ 0 [0Ni])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0 [0i])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0 [6i])", "[0]");
    TEST_ASSERT_EQ("(/ 0 [-2.00])", "[0]");
    TEST_ASSERT_EQ("(/ 0 [-1.00])", "[0]");
    TEST_ASSERT_EQ("(/ 0 [-0.60])", "[0]");
    TEST_ASSERT_EQ("(/ 0 [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0 [0Nf])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0 [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 0 [0.60])", "[0]");
    TEST_ASSERT_EQ("(/ 0 [1.00])", "[0]");
    TEST_ASSERT_EQ("(/ 0 [3.00])", "[0]");
    TEST_ASSERT_EQ("(/ 6 [-5])", "[-2]");
    TEST_ASSERT_EQ("(/ 6 [-2])", "[-3]");
    TEST_ASSERT_EQ("(/ 6 [0Nl])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 6 [0])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 6 [6])", "[1]");
    TEST_ASSERT_EQ("(/ 6 [-5i])", "[-2]");
    TEST_ASSERT_EQ("(/ 6 [0Ni])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 6 [0i])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 6 [6i])", "[1]");
    TEST_ASSERT_EQ("(/ 6 [-2.00])", "[-3]");
    TEST_ASSERT_EQ("(/ 6 [-1.00])", "[-6]");
    TEST_ASSERT_EQ("(/ 6 [-0.60])", "[-10]");
    TEST_ASSERT_EQ("(/ 6 [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 6 [0Nf])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 6 [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 6 [0.60])", "[10]");
    TEST_ASSERT_EQ("(/ 6 [1.00])", "[6]");
    TEST_ASSERT_EQ("(/ 6 [3.00])", "[2]");
    TEST_ASSERT_EQ("(/ -5i [-5])", "[1]");
    TEST_ASSERT_EQ("(/ -5i [-2])", "[2]");
    TEST_ASSERT_EQ("(/ -5i [0Nl])", "[0Ni]");
    TEST_ASSERT_EQ("(/ -5i [0])", "[0Ni]");
    TEST_ASSERT_EQ("(/ -5i [6])", "[-1]");
    TEST_ASSERT_EQ("(/ -5i [-5i])", "[1]");
    TEST_ASSERT_EQ("(/ -5i [0Ni])", "[0Ni]");
    TEST_ASSERT_EQ("(/ -5i [0i])", "[0Ni]");
    TEST_ASSERT_EQ("(/ -5i [6i])", "[-1]");
    TEST_ASSERT_EQ("(/ -5i [-2.00])", "[2]");
    TEST_ASSERT_EQ("(/ -5i [-1.00])", "[5]");
    TEST_ASSERT_EQ("(/ -5i [-0.60])", "[8]");
    TEST_ASSERT_EQ("(/ -5i [0.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ -5i [0Nf])", "[0Ni]");
    TEST_ASSERT_EQ("(/ -5i [0.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ -5i [0.60])", "[-9]");
    TEST_ASSERT_EQ("(/ -5i [1.00])", "[-5]");
    TEST_ASSERT_EQ("(/ -5i [3.00])", "[-2]");
    TEST_ASSERT_EQ("(/ 0Ni [-5])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0Ni [-2])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0Ni [0Nl])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0Ni [0])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0Ni [6])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0Ni [-5i])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0Ni [0Ni])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0Ni [0i])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0Ni [6i])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0Ni [-2.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0Ni [-1.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0Ni [-0.60])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0Ni [0.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0Ni [0Nf])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0Ni [0.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0Ni [0.60])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0Ni [1.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0Ni [3.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0i [-5])", "[0]");
    TEST_ASSERT_EQ("(/ 0i [-2])", "[0]");
    TEST_ASSERT_EQ("(/ 0i [0Nl])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0i [0])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0i [6])", "[0]");
    TEST_ASSERT_EQ("(/ 0i [-5i])", "[0]");
    TEST_ASSERT_EQ("(/ 0i [0Ni])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0i [0i])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0i [6i])", "[0]");
    TEST_ASSERT_EQ("(/ 0i [-2.00])", "[0]");
    TEST_ASSERT_EQ("(/ 0i [-1.00])", "[0]");
    TEST_ASSERT_EQ("(/ 0i [-0.60])", "[0]");
    TEST_ASSERT_EQ("(/ 0i [0.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0i [0Nf])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0i [0.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 0i [0.60])", "[0]");
    TEST_ASSERT_EQ("(/ 0i [1.00])", "[0]");
    TEST_ASSERT_EQ("(/ 0i [3.00])", "[0]");
    TEST_ASSERT_EQ("(/ 6i [-5])", "[-2]");
    TEST_ASSERT_EQ("(/ 6i [-2])", "[-3]");
    TEST_ASSERT_EQ("(/ 6i [0Nl])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 6i [0])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 6i [6])", "[1]");
    TEST_ASSERT_EQ("(/ 6i [-5i])", "[-2]");
    TEST_ASSERT_EQ("(/ 6i [0Ni])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 6i [0i])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 6i [6i])", "[1]");
    TEST_ASSERT_EQ("(/ 6i [-2.00])", "[-3]");
    TEST_ASSERT_EQ("(/ 6i [-1.00])", "[-6]");
    TEST_ASSERT_EQ("(/ 6i [-0.60])", "[-10]");
    TEST_ASSERT_EQ("(/ 6i [0.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 6i [0Nf])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 6i [0.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 6i [0.60])", "[10]");
    TEST_ASSERT_EQ("(/ 6i [1.00])", "[6]");
    TEST_ASSERT_EQ("(/ 6i [3.00])", "[2]");
    TEST_ASSERT_EQ("(/ -2.00 [-5])", "[0.00]");
    TEST_ASSERT_EQ("(/ -2.00 [-2])", "[1.00]");
    TEST_ASSERT_EQ("(/ -2.00 [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -2.00 [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -2.00 [6])", "[-1.00]");
    TEST_ASSERT_EQ("(/ -2.00 [-5i])", "[0.00]");
    TEST_ASSERT_EQ("(/ -2.00 [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -2.00 [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -2.00 [6i])", "[-1.00]");
    TEST_ASSERT_EQ("(/ -2.00 [-2.00])", "[1.00]");
    TEST_ASSERT_EQ("(/ -2.00 [-1.00])", "[2.00]");
    TEST_ASSERT_EQ("(/ -2.00 [-0.60])", "[3.00]");
    TEST_ASSERT_EQ("(/ -2.00 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -2.00 [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -2.00 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -2.00 [0.60])", "[-4.00]");
    TEST_ASSERT_EQ("(/ -2.00 [1.00])", "[-2.00]");
    TEST_ASSERT_EQ("(/ -2.00 [3.00])", "[-1.00]");
    TEST_ASSERT_EQ("(/ -1.00 [-5])", "[0.00]");
    TEST_ASSERT_EQ("(/ -1.00 [-2])", "[0.00]");
    TEST_ASSERT_EQ("(/ -1.00 [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -1.00 [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -1.00 [6])", "[-1.00]");
    TEST_ASSERT_EQ("(/ -1.00 [-5i])", "[0.00]");
    TEST_ASSERT_EQ("(/ -1.00 [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -1.00 [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -1.00 [6i])", "[-1.00]");
    TEST_ASSERT_EQ("(/ -1.00 [-2.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ -1.00 [-1.00])", "[1.00]");
    TEST_ASSERT_EQ("(/ -1.00 [-0.60])", "[1.00]");
    TEST_ASSERT_EQ("(/ -1.00 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -1.00 [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -1.00 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -1.00 [0.60])", "[-2.00]");
    TEST_ASSERT_EQ("(/ -1.00 [1.00])", "[-1.00]");
    TEST_ASSERT_EQ("(/ -1.00 [3.00])", "[-1.00]");
    TEST_ASSERT_EQ("(/ -0.60 [-5])", "[0.00]");
    TEST_ASSERT_EQ("(/ -0.60 [-2])", "[0.00]");
    TEST_ASSERT_EQ("(/ -0.60 [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -0.60 [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -0.60 [6])", "[-1.00]");
    TEST_ASSERT_EQ("(/ -0.60 [-5i])", "[0.00]");
    TEST_ASSERT_EQ("(/ -0.60 [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -0.60 [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -0.60 [6i])", "[-1.00]");
    TEST_ASSERT_EQ("(/ -0.60 [-2.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ -0.60 [-1.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ -0.60 [-0.60])", "[1.00]");
    TEST_ASSERT_EQ("(/ -0.60 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -0.60 [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -0.60 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -0.60 [0.60])", "[-1.00]");
    TEST_ASSERT_EQ("(/ -0.60 [1.00])", "[-1.00]");
    TEST_ASSERT_EQ("(/ -0.60 [3.00])", "[-1.00]");
    TEST_ASSERT_EQ("(/ 0.00 [-5])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.00 [-2])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.00 [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.00 [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.00 [6])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.00 [-5i])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.00 [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.00 [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.00 [6i])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.00 [-2.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.00 [-1.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.00 [-0.60])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.00 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.00 [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.00 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.00 [0.60])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.00 [1.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.00 [3.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0Nf [-5])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0Nf [-2])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0Nf [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0Nf [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0Nf [6])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0Nf [-5i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0Nf [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0Nf [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0Nf [6i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0Nf [-2.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0Nf [-1.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0Nf [-0.60])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0Nf [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0Nf [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0Nf [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0Nf [0.60])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0Nf [1.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0Nf [3.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.00 [-5])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.00 [-2])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.00 [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.00 [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.00 [6])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.00 [-5i])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.00 [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.00 [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.00 [6i])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.00 [-2.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.00 [-1.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.00 [-0.60])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.00 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.00 [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.00 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.00 [0.60])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.00 [1.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.00 [3.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.60 [-5])", "[-1.00]");
    TEST_ASSERT_EQ("(/ 0.60 [-2])", "[-1.00]");
    TEST_ASSERT_EQ("(/ 0.60 [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.60 [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.60 [6])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.60 [-5i])", "[-1.00]");
    TEST_ASSERT_EQ("(/ 0.60 [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.60 [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.60 [6i])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.60 [-2.00])", "[-1.00]");
    TEST_ASSERT_EQ("(/ 0.60 [-1.00])", "[-1.00]");
    TEST_ASSERT_EQ("(/ 0.60 [-0.60])", "[-1.00]");
    TEST_ASSERT_EQ("(/ 0.60 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.60 [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.60 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.60 [0.60])", "[1.00]");
    TEST_ASSERT_EQ("(/ 0.60 [1.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.60 [3.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ 1.00 [-5])", "[-1.00]");
    TEST_ASSERT_EQ("(/ 1.00 [-2])", "[-1.00]");
    TEST_ASSERT_EQ("(/ 1.00 [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 1.00 [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 1.00 [6])", "[0.00]");
    TEST_ASSERT_EQ("(/ 1.00 [-5i])", "[-1.00]");
    TEST_ASSERT_EQ("(/ 1.00 [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 1.00 [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 1.00 [6i])", "[0.00]");
    TEST_ASSERT_EQ("(/ 1.00 [-2.00])", "[-1.00]");
    TEST_ASSERT_EQ("(/ 1.00 [-1.00])", "[-1.00]");
    TEST_ASSERT_EQ("(/ 1.00 [-0.60])", "[-2.00]");
    TEST_ASSERT_EQ("(/ 1.00 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 1.00 [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 1.00 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 1.00 [0.60])", "[1.00]");
    TEST_ASSERT_EQ("(/ 1.00 [1.00])", "[1.00]");
    TEST_ASSERT_EQ("(/ 1.00 [3.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ 3.00 [-5])", "[-1.00]");
    TEST_ASSERT_EQ("(/ 3.00 [-2])", "[-2.00]");
    TEST_ASSERT_EQ("(/ 3.00 [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 3.00 [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 3.00 [6])", "[0.00]");
    TEST_ASSERT_EQ("(/ 3.00 [-5i])", "[-1.00]");
    TEST_ASSERT_EQ("(/ 3.00 [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 3.00 [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 3.00 [6i])", "[0.00]");
    TEST_ASSERT_EQ("(/ 3.00 [-2.00])", "[-2.00]");
    TEST_ASSERT_EQ("(/ 3.00 [-1.00])", "[-3.00]");
    TEST_ASSERT_EQ("(/ 3.00 [-0.60])", "[-5.00]");
    TEST_ASSERT_EQ("(/ 3.00 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 3.00 [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 3.00 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 3.00 [0.60])", "[5.00]");
    TEST_ASSERT_EQ("(/ 3.00 [1.00])", "[3.00]");
    TEST_ASSERT_EQ("(/ 3.00 [3.00])", "[1.00]");

    TEST_ASSERT_EQ("(/ [-5] [-5])", "[1]");
    TEST_ASSERT_EQ("(/ [-5] [-2])", "[2]");
    TEST_ASSERT_EQ("(/ [-5] [0Nl])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] [0])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] [6])", "[-1]");
    TEST_ASSERT_EQ("(/ [-5] [-5i])", "[1]");
    TEST_ASSERT_EQ("(/ [-5] [0Ni])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] [0i])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] [6i])", "[-1]");
    TEST_ASSERT_EQ("(/ [-5] [-2.00])", "[2]");
    TEST_ASSERT_EQ("(/ [-5] [-1.00])", "[5]");
    TEST_ASSERT_EQ("(/ [-5] [-0.60])", "[8]");
    TEST_ASSERT_EQ("(/ [-5] [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] [0Nf])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] [0.60])", "[-9]");
    TEST_ASSERT_EQ("(/ [-5] [1.00])", "[-5]");
    TEST_ASSERT_EQ("(/ [-5] [3.00])", "[-2]");
    TEST_ASSERT_EQ("(/ [-2] [-5])", "[0]");
    TEST_ASSERT_EQ("(/ [-2] [-2])", "[1]");
    TEST_ASSERT_EQ("(/ [-2] [0Nl])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] [0])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] [6])", "[-1]");
    TEST_ASSERT_EQ("(/ [-2] [-5i])", "[0]");
    TEST_ASSERT_EQ("(/ [-2] [0Ni])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] [0i])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] [6i])", "[-1]");
    TEST_ASSERT_EQ("(/ [-2] [-2.00])", "[1]");
    TEST_ASSERT_EQ("(/ [-2] [-1.00])", "[2]");
    TEST_ASSERT_EQ("(/ [-2] [-0.60])", "[3]");
    TEST_ASSERT_EQ("(/ [-2] [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] [0Nf])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] [0.60])", "[-4]");
    TEST_ASSERT_EQ("(/ [-2] [1.00])", "[-2]");
    TEST_ASSERT_EQ("(/ [-2] [3.00])", "[-1]");
    TEST_ASSERT_EQ("(/ [0Nl] [-5])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] [-2])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] [0Nl])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] [0])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] [6])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] [-5i])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] [0Ni])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] [0i])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] [6i])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] [-2.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] [-1.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] [-0.60])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] [0Nf])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] [0.60])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] [1.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0Nl] [3.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0] [-5])", "[0]");
    TEST_ASSERT_EQ("(/ [0] [-2])", "[0]");
    TEST_ASSERT_EQ("(/ [0] [0Nl])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0] [0])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0] [6])", "[0]");
    TEST_ASSERT_EQ("(/ [0] [-5i])", "[0]");
    TEST_ASSERT_EQ("(/ [0] [0Ni])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0] [0i])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0] [6i])", "[0]");
    TEST_ASSERT_EQ("(/ [0] [-2.00])", "[0]");
    TEST_ASSERT_EQ("(/ [0] [-1.00])", "[0]");
    TEST_ASSERT_EQ("(/ [0] [-0.60])", "[0]");
    TEST_ASSERT_EQ("(/ [0] [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0] [0Nf])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0] [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [0] [0.60])", "[0]");
    TEST_ASSERT_EQ("(/ [0] [1.00])", "[0]");
    TEST_ASSERT_EQ("(/ [0] [3.00])", "[0]");
    TEST_ASSERT_EQ("(/ [6] [-5])", "[-2]");
    TEST_ASSERT_EQ("(/ [6] [-2])", "[-3]");
    TEST_ASSERT_EQ("(/ [6] [0Nl])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [6] [0])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [6] [6])", "[1]");
    TEST_ASSERT_EQ("(/ [6] [-5i])", "[-2]");
    TEST_ASSERT_EQ("(/ [6] [0Ni])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [6] [0i])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [6] [6i])", "[1]");
    TEST_ASSERT_EQ("(/ [6] [-2.00])", "[-3]");
    TEST_ASSERT_EQ("(/ [6] [-1.00])", "[-6]");
    TEST_ASSERT_EQ("(/ [6] [-0.60])", "[-10]");
    TEST_ASSERT_EQ("(/ [6] [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [6] [0Nf])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [6] [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [6] [0.60])", "[10]");
    TEST_ASSERT_EQ("(/ [6] [1.00])", "[6]");
    TEST_ASSERT_EQ("(/ [6] [3.00])", "[2]");
    TEST_ASSERT_EQ("(/ [-5i] [-5])", "[1]");
    TEST_ASSERT_EQ("(/ [-5i] [-2])", "[2]");
    TEST_ASSERT_EQ("(/ [-5i] [0Nl])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] [0])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] [6])", "[-1]");
    TEST_ASSERT_EQ("(/ [-5i] [-5i])", "[1]");
    TEST_ASSERT_EQ("(/ [-5i] [0Ni])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] [0i])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] [6i])", "[-1]");
    TEST_ASSERT_EQ("(/ [-5i] [-2.00])", "[2]");
    TEST_ASSERT_EQ("(/ [-5i] [-1.00])", "[5]");
    TEST_ASSERT_EQ("(/ [-5i] [-0.60])", "[8]");
    TEST_ASSERT_EQ("(/ [-5i] [0.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] [0Nf])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] [0.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] [0.60])", "[-9]");
    TEST_ASSERT_EQ("(/ [-5i] [1.00])", "[-5]");
    TEST_ASSERT_EQ("(/ [-5i] [3.00])", "[-2]");
    TEST_ASSERT_EQ("(/ [0Ni] [-5])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] [-2])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] [0Nl])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] [0])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] [6])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] [-5i])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] [0Ni])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] [0i])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] [6i])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] [-2.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] [-1.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] [-0.60])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] [0.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] [0Nf])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] [0.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] [0.60])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] [1.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0Ni] [3.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0i] [-5])", "[0]");
    TEST_ASSERT_EQ("(/ [0i] [-2])", "[0]");
    TEST_ASSERT_EQ("(/ [0i] [0Nl])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0i] [0])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0i] [6])", "[0]");
    TEST_ASSERT_EQ("(/ [0i] [-5i])", "[0]");
    TEST_ASSERT_EQ("(/ [0i] [0Ni])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0i] [0i])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0i] [6i])", "[0]");
    TEST_ASSERT_EQ("(/ [0i] [-2.00])", "[0]");
    TEST_ASSERT_EQ("(/ [0i] [-1.00])", "[0]");
    TEST_ASSERT_EQ("(/ [0i] [-0.60])", "[0]");
    TEST_ASSERT_EQ("(/ [0i] [0.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0i] [0Nf])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0i] [0.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [0i] [0.60])", "[0]");
    TEST_ASSERT_EQ("(/ [0i] [1.00])", "[0]");
    TEST_ASSERT_EQ("(/ [0i] [3.00])", "[0]");
    TEST_ASSERT_EQ("(/ [6i] [-5])", "[-2]");
    TEST_ASSERT_EQ("(/ [6i] [-2])", "[-3]");
    TEST_ASSERT_EQ("(/ [6i] [0Nl])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [6i] [0])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [6i] [6])", "[1]");
    TEST_ASSERT_EQ("(/ [6i] [-5i])", "[-2]");
    TEST_ASSERT_EQ("(/ [6i] [0Ni])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [6i] [0i])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [6i] [6i])", "[1]");
    TEST_ASSERT_EQ("(/ [6i] [-2.00])", "[-3]");
    TEST_ASSERT_EQ("(/ [6i] [-1.00])", "[-6]");
    TEST_ASSERT_EQ("(/ [6i] [-0.60])", "[-10]");
    TEST_ASSERT_EQ("(/ [6i] [0.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [6i] [0Nf])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [6i] [0.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [6i] [0.60])", "[10]");
    TEST_ASSERT_EQ("(/ [6i] [1.00])", "[6]");
    TEST_ASSERT_EQ("(/ [6i] [3.00])", "[2]");
    TEST_ASSERT_EQ("(/ [-2.00] [-5])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-2.00] [-2])", "[1.00]");
    TEST_ASSERT_EQ("(/ [-2.00] [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] [6])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-2.00] [-5i])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-2.00] [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] [6i])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-2.00] [-2.00])", "[1.00]");
    TEST_ASSERT_EQ("(/ [-2.00] [-1.00])", "[2.00]");
    TEST_ASSERT_EQ("(/ [-2.00] [-0.60])", "[3.00]");
    TEST_ASSERT_EQ("(/ [-2.00] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] [0.60])", "[-4.00]");
    TEST_ASSERT_EQ("(/ [-2.00] [1.00])", "[-2.00]");
    TEST_ASSERT_EQ("(/ [-2.00] [3.00])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-1.00] [-5])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-1.00] [-2])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-1.00] [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] [6])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-1.00] [-5i])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-1.00] [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] [6i])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-1.00] [-2.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-1.00] [-1.00])", "[1.00]");
    TEST_ASSERT_EQ("(/ [-1.00] [-0.60])", "[1.00]");
    TEST_ASSERT_EQ("(/ [-1.00] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] [0.60])", "[-2.00]");
    TEST_ASSERT_EQ("(/ [-1.00] [1.00])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-1.00] [3.00])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-0.60] [-5])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] [-2])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] [6])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-0.60] [-5i])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] [6i])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-0.60] [-2.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] [-1.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] [-0.60])", "[1.00]");
    TEST_ASSERT_EQ("(/ [-0.60] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] [0.60])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-0.60] [1.00])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-0.60] [3.00])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [0.00] [-5])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] [-2])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.00] [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.00] [6])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] [-5i])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.00] [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.00] [6i])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] [-2.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] [-1.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] [-0.60])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.00] [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.00] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.00] [0.60])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] [1.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] [3.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0Nf] [-5])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] [-2])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] [6])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] [-5i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] [6i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] [-2.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] [-1.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] [-0.60])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] [0.60])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] [1.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0Nf] [3.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.00] [-5])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] [-2])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.00] [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.00] [6])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] [-5i])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.00] [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.00] [6i])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] [-2.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] [-1.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] [-0.60])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.00] [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.00] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.00] [0.60])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] [1.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.00] [3.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] [-5])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [0.60] [-2])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [0.60] [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] [6])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] [-5i])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [0.60] [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] [6i])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] [-2.00])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [0.60] [-1.00])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [0.60] [-0.60])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [0.60] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] [0.60])", "[1.00]");
    TEST_ASSERT_EQ("(/ [0.60] [1.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] [3.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [1.00] [-5])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [1.00] [-2])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [1.00] [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] [6])", "[0.00]");
    TEST_ASSERT_EQ("(/ [1.00] [-5i])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [1.00] [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] [6i])", "[0.00]");
    TEST_ASSERT_EQ("(/ [1.00] [-2.00])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [1.00] [-1.00])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [1.00] [-0.60])", "[-2.00]");
    TEST_ASSERT_EQ("(/ [1.00] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] [0.60])", "[1.00]");
    TEST_ASSERT_EQ("(/ [1.00] [1.00])", "[1.00]");
    TEST_ASSERT_EQ("(/ [1.00] [3.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [3.00] [-5])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [3.00] [-2])", "[-2.00]");
    TEST_ASSERT_EQ("(/ [3.00] [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [3.00] [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [3.00] [6])", "[0.00]");
    TEST_ASSERT_EQ("(/ [3.00] [-5i])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [3.00] [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [3.00] [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [3.00] [6i])", "[0.00]");
    TEST_ASSERT_EQ("(/ [3.00] [-2.00])", "[-2.00]");
    TEST_ASSERT_EQ("(/ [3.00] [-1.00])", "[-3.00]");
    TEST_ASSERT_EQ("(/ [3.00] [-0.60])", "[-5.00]");
    TEST_ASSERT_EQ("(/ [3.00] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [3.00] [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [3.00] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [3.00] [0.60])", "[5.00]");
    TEST_ASSERT_EQ("(/ [3.00] [1.00])", "[3.00]");
    TEST_ASSERT_EQ("(/ [3.00] [3.00])", "[1.00]");

    TEST_ASSERT_EQ("(/ 10 [])", "[]");
    TEST_ASSERT_ER("(/ 02:15:07.000 02:15:07.000)", "div: unsupported types: 'time, 'time");

    TEST_ASSERT_EQ("(% 10i 0i)", "0Ni");
    TEST_ASSERT_EQ("(% 10i 0)", "0Nl");
    TEST_ASSERT_EQ("(% 10i 0.0)", "0Nf");
    TEST_ASSERT_EQ("(% 10i 5i)", "0i");
    TEST_ASSERT_EQ("(% 11i 5i)", "1i");
    TEST_ASSERT_EQ("(% 11i 5)", "1");
    TEST_ASSERT_EQ("(% 11i 5.0)", "1.0");
    TEST_ASSERT_EQ("(% 10i [0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% 10i [5i])", "[0i]");
    TEST_ASSERT_EQ("(% 11i [5i])", "[1i]");
    TEST_ASSERT_EQ("(% 11i [5])", "[1]");
    TEST_ASSERT_EQ("(% 11i [5.0])", "[1.0]");
    TEST_ASSERT_EQ("(% -10i 0i)", "0Ni");
    TEST_ASSERT_EQ("(% -10i 0)", "0Nl");
    TEST_ASSERT_EQ("(% -10i 0.0)", "0Nf");
    TEST_ASSERT_EQ("(% -10i 5i)", "0i");
    TEST_ASSERT_EQ("(% -11i 5i)", "4i");
    TEST_ASSERT_EQ("(% -11i 5)", "4");
    TEST_ASSERT_EQ("(% -11i 5.0)", "4.0");
    TEST_ASSERT_EQ("(% -10i [0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% -10i [0])", "[0Nl]");
    TEST_ASSERT_EQ("(% -10i [0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% -10i [5i])", "[0i]");
    TEST_ASSERT_EQ("(% -11i [5i])", "[4i]");
    TEST_ASSERT_EQ("(% -11i [5])", "[4]");
    TEST_ASSERT_EQ("(% -11i [5.0])", "[4.0]");
    TEST_ASSERT_EQ("(% 10i -0i)", "0Ni");
    TEST_ASSERT_EQ("(% 10i -5i)", "0i");
    TEST_ASSERT_EQ("(% 11i -5i)", "-4i");
    TEST_ASSERT_EQ("(% 11i -5)", "-4");
    TEST_ASSERT_EQ("(% 11i -5.0)", "-4.0");
    TEST_ASSERT_EQ("(% 10i [-0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% 10i [-0])", "[0Nl]");
    TEST_ASSERT_EQ("(% 10i [-0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% 10i [-5i])", "[0i]");
    TEST_ASSERT_EQ("(% 11i [-5i])", "[-4i]");
    TEST_ASSERT_EQ("(% 11i [-5])", "[-4]");
    TEST_ASSERT_EQ("(% 11i [-5.0])", "[-4.0]");
    TEST_ASSERT_EQ("(% -10i -0i)", "0Ni");
    TEST_ASSERT_EQ("(% -10i -5i)", "0i");
    TEST_ASSERT_EQ("(% -11i -5i)", "-1i");
    TEST_ASSERT_EQ("(% -11i -5)", "-1");
    TEST_ASSERT_EQ("(% -11i -5.0)", "-1.0");
    TEST_ASSERT_EQ("(% -10i [-0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% -10i [-5i])", "[0i]");
    TEST_ASSERT_EQ("(% -11i [-5i])", "[-1i]");
    TEST_ASSERT_EQ("(% -11i [-5])", "[-1]");
    TEST_ASSERT_EQ("(% -11i [-5.0])", "[-1.0]");

    TEST_ASSERT_EQ("(% 10 0i)", "0Ni");
    TEST_ASSERT_EQ("(% 10 0)", "0Nl");
    TEST_ASSERT_EQ("(% 10 0.0)", "0Nf");
    TEST_ASSERT_EQ("(% 10 5i)", "0i");
    TEST_ASSERT_EQ("(% 11 5i)", "1i");
    TEST_ASSERT_EQ("(% 11 5)", "1");
    TEST_ASSERT_EQ("(% 11 5.0)", "1.0");
    TEST_ASSERT_EQ("(% 10 [0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% 10 [5i])", "[0i]");
    TEST_ASSERT_EQ("(% 11 [5i])", "[1i]");
    TEST_ASSERT_EQ("(% 11 [5])", "[1]");
    TEST_ASSERT_EQ("(% 11 [5.0])", "[1.0]");
    TEST_ASSERT_EQ("(% -10 0i)", "0Ni");
    TEST_ASSERT_EQ("(% -10 0)", "0Nl");
    TEST_ASSERT_EQ("(% -10 0.0)", "0Nf");
    TEST_ASSERT_EQ("(% -10 5i)", "0i");
    TEST_ASSERT_EQ("(% -11 5i)", "4i");
    TEST_ASSERT_EQ("(% -11 5)", "4");
    TEST_ASSERT_EQ("(% -11 5.0)", "4.0");
    TEST_ASSERT_EQ("(% -10 [0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% -10 [0])", "[0Nl]");
    TEST_ASSERT_EQ("(% -10 [0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% -10 [5i])", "[0i]");
    TEST_ASSERT_EQ("(% -11 [5i])", "[4i]");
    TEST_ASSERT_EQ("(% -11 [5])", "[4]");
    TEST_ASSERT_EQ("(% -11 [5.0])", "[4.0]");
    TEST_ASSERT_EQ("(% 10 -0i)", "0Ni");
    TEST_ASSERT_EQ("(% 10 -5i)", "0i");
    TEST_ASSERT_EQ("(% 11 -5i)", "-4i");
    TEST_ASSERT_EQ("(% 11 -5)", "-4");
    TEST_ASSERT_EQ("(% 11 -5.0)", "-4.0");
    TEST_ASSERT_EQ("(% 10 [-0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% 10 [-0])", "[0Nl]");
    TEST_ASSERT_EQ("(% 10 [-0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% 10 [-5i])", "[0i]");
    TEST_ASSERT_EQ("(% 11 [-5i])", "[-4i]");
    TEST_ASSERT_EQ("(% 11 [-5])", "[-4]");
    TEST_ASSERT_EQ("(% 11 [-5.0])", "[-4.0]");
    TEST_ASSERT_EQ("(% -10 -0i)", "0Ni");
    TEST_ASSERT_EQ("(% -10 -5i)", "0i");
    TEST_ASSERT_EQ("(% -11 -5i)", "-1i");
    TEST_ASSERT_EQ("(% -11 -5)", "-1");
    TEST_ASSERT_EQ("(% -11 -5.0)", "-1.0");
    TEST_ASSERT_EQ("(% -10 [-0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% -10 [-5i])", "[0i]");
    TEST_ASSERT_EQ("(% -11 [-5i])", "[-1i]");
    TEST_ASSERT_EQ("(% -11 [-5])", "[-1]");
    TEST_ASSERT_EQ("(% -11 [-5.0])", "[-1.0]");
    TEST_ASSERT_EQ("(% 100000000001 5i)", "1i");
    TEST_ASSERT_EQ("(% 100000000001 [5i])", "[1i]");

    TEST_ASSERT_EQ("(% 10.0 0i)", "0Nf");
    TEST_ASSERT_EQ("(% 10.0 0)", "0Nf");
    TEST_ASSERT_EQ("(% 10.0 0.0)", "0Nf");
    TEST_ASSERT_EQ("(% 10.0 5i)", "0.0");
    TEST_ASSERT_EQ("(% 11.0 5i)", "1.0");
    TEST_ASSERT_EQ("(% 11.0 5)", "1.0");
    TEST_ASSERT_EQ("(% 11.0 5.0)", "1.0");
    TEST_ASSERT_EQ("(% 10.0 [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(% 10.0 [5i])", "[0.0]");
    TEST_ASSERT_EQ("(% 11.0 [5i])", "[1.0]");
    TEST_ASSERT_EQ("(% 11.0 [5])", "[1.0]");
    TEST_ASSERT_EQ("(% 11.0 [5.0])", "[1.0]");
    TEST_ASSERT_EQ("(% -10.0 0i)", "0Nf");
    TEST_ASSERT_EQ("(% -10.0 0)", "0Nf");
    TEST_ASSERT_EQ("(% -10.0 0.0)", "0Nf");
    TEST_ASSERT_EQ("(% -10.0 5i)", "0.0");
    TEST_ASSERT_EQ("(% -11.0 5i)", "4.0");
    TEST_ASSERT_EQ("(% -11.0 5)", "4.0");
    TEST_ASSERT_EQ("(% -11.0 5.0)", "4.0");
    TEST_ASSERT_EQ("(% -10.0 [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(% -10.0 [0])", "[0Nf]");
    TEST_ASSERT_EQ("(% -10.0 [0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% -10.0 [5i])", "[0.0]");
    TEST_ASSERT_EQ("(% -11.0 [5i])", "[4.0]");
    TEST_ASSERT_EQ("(% -11.0 [5])", "[4.0]");
    TEST_ASSERT_EQ("(% -11.0 [5.0])", "[4.0]");
    TEST_ASSERT_EQ("(% 10.0 -0i)", "0Nf");
    TEST_ASSERT_EQ("(% 10.0 -5i)", "0.0");
    TEST_ASSERT_EQ("(% 11.0 -5i)", "-4.0");
    TEST_ASSERT_EQ("(% 11.0 -5)", "-4.0");
    TEST_ASSERT_EQ("(% 11.0 -5.0)", "-4.0");
    TEST_ASSERT_EQ("(% 10.0 [-0i])", "[0Nf]");
    TEST_ASSERT_EQ("(% 10.0 [-0])", "[0Nf]");
    TEST_ASSERT_EQ("(% 10.0 [-0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% 10.0 [-5i])", "[0.0]");
    TEST_ASSERT_EQ("(% 11.0 [-5i])", "[-4.0]");
    TEST_ASSERT_EQ("(% 11.0 [-5])", "[-4.0]");
    TEST_ASSERT_EQ("(% 11.0 [-5.0])", "[-4.0]");
    TEST_ASSERT_EQ("(% -10.0 -0i)", "0Nf");
    TEST_ASSERT_EQ("(% -10.0 -5i)", "0.0");
    TEST_ASSERT_EQ("(% -11.0 -5i)", "-1.0");
    TEST_ASSERT_EQ("(% -11.0 -5)", "-1.0");
    TEST_ASSERT_EQ("(% -11.0 -5.0)", "-1.0");
    TEST_ASSERT_EQ("(% -10.0 [-0i])", "[0Nf]");
    TEST_ASSERT_EQ("(% -10.0 [-5i])", "[0.0]");
    TEST_ASSERT_EQ("(% -11.0 [-5i])", "[-1.0]");
    TEST_ASSERT_EQ("(% -11.0 [-5])", "[-1.0]");
    TEST_ASSERT_EQ("(% -11.0 [-5.0])", "[-1.0]");
    TEST_ASSERT_EQ("(% 100000000001.0 5i)", "1.0");
    TEST_ASSERT_EQ("(% 100000000001.0 [5i])", "[1.0]");
    TEST_ASSERT_EQ("(% 18.4 5.1)", "3.1");

    TEST_ASSERT_EQ("(% [10i] 0i)", "[0Ni]");
    TEST_ASSERT_EQ("(% [10i] 0)", "[0Nl]");
    TEST_ASSERT_EQ("(% [10i] 0.0)", "[0Nf]");
    TEST_ASSERT_EQ("(% [10i] 5i)", "[0i]");
    TEST_ASSERT_EQ("(% [11i] 5i)", "[1i]");
    TEST_ASSERT_EQ("(% [11i] 5)", "[1]");
    TEST_ASSERT_EQ("(% [11i] 5.0)", "[1.0]");
    TEST_ASSERT_EQ("(% [10i] [0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% [10i] [5i])", "[0i]");
    TEST_ASSERT_EQ("(% [11i] [5i])", "[1i]");
    TEST_ASSERT_EQ("(% [11i] [5])", "[1]");
    TEST_ASSERT_EQ("(% [11i] [5.0])", "[1.0]");
    TEST_ASSERT_EQ("(% [-10i] 0i)", "[0Ni]");
    TEST_ASSERT_EQ("(% [-10i] 0)", "[0Nl]");
    TEST_ASSERT_EQ("(% [-10i] 0.0)", "[0Nf]");
    TEST_ASSERT_EQ("(% [-10i] 5i)", "[0i]");
    TEST_ASSERT_EQ("(% [-11i] 5i)", "[4i]");
    TEST_ASSERT_EQ("(% [-11i] 5)", "[4]");
    TEST_ASSERT_EQ("(% [-11i] 5.0)", "[4.0]");
    TEST_ASSERT_EQ("(% [-10i] [0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% [-10i] [0])", "[0Nl]");
    TEST_ASSERT_EQ("(% [-10i] [0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% [-10i] [5i])", "[0i]");
    TEST_ASSERT_EQ("(% [-11i] [5i])", "[4i]");
    TEST_ASSERT_EQ("(% [-11i] [5])", "[4]");
    TEST_ASSERT_EQ("(% [-11i] [5.0])", "[4.0]");
    TEST_ASSERT_EQ("(% [10i] -0i)", "[0Ni]");
    TEST_ASSERT_EQ("(% [10i] -5i)", "[0i]");
    TEST_ASSERT_EQ("(% [11i] -5i)", "[-4i]");
    TEST_ASSERT_EQ("(% [11i] -5)", "[-4]");
    TEST_ASSERT_EQ("(% [11i] -5.0)", "[-4.0]");
    TEST_ASSERT_EQ("(% [10i] [-0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% [10i] [-0])", "[0Nl]");
    TEST_ASSERT_EQ("(% [10i] [-0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% [10i] [-5i])", "[0i]");
    TEST_ASSERT_EQ("(% [11i] [-5i])", "[-4i]");
    TEST_ASSERT_EQ("(% [11i] [-5])", "[-4]");
    TEST_ASSERT_EQ("(% [11i] [-5.0])", "[-4.0]");
    TEST_ASSERT_EQ("(% [-10i] -0i)", "[0Ni]");
    TEST_ASSERT_EQ("(% [-10i] -5i)", "[0i]");
    TEST_ASSERT_EQ("(% [-11i] -5i)", "[-1i]");
    TEST_ASSERT_EQ("(% [-11i] -5)", "[-1]");
    TEST_ASSERT_EQ("(% [-11i] -5.0)", "[-1.0]");
    TEST_ASSERT_EQ("(% [-10i] [-0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% [-10i] [-5i])", "[0i]");
    TEST_ASSERT_EQ("(% [-11i] [-5i])", "[-1i]");
    TEST_ASSERT_EQ("(% [-11i] [-5])", "[-1]");
    TEST_ASSERT_EQ("(% [-11i] [-5.0])", "[-1.0]");

    TEST_ASSERT_EQ("(% [10] 0i)", "[0Ni]");
    TEST_ASSERT_EQ("(% [10] 0)", "[0Nl]");
    TEST_ASSERT_EQ("(% [10] 0.0)", "[0Nf]");
    TEST_ASSERT_EQ("(% [10] 5i)", "[0i]");
    TEST_ASSERT_EQ("(% [11] 5i)", "[1i]");
    TEST_ASSERT_EQ("(% [11] 5)", "[1]");
    TEST_ASSERT_EQ("(% [11] 5.0)", "[1.0]");
    TEST_ASSERT_EQ("(% [10] [0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% [10] [5i])", "[0i]");
    TEST_ASSERT_EQ("(% [11] [5i])", "[1i]");
    TEST_ASSERT_EQ("(% [11] [5])", "[1]");
    TEST_ASSERT_EQ("(% [11] [5.0])", "[1.0]");
    TEST_ASSERT_EQ("(% [-10] 0i)", "[0Ni]");
    TEST_ASSERT_EQ("(% [-10] 0)", "[0Nl]");
    TEST_ASSERT_EQ("(% [-10] 0.0)", "[0Nf]");
    TEST_ASSERT_EQ("(% [-10] 5i)", "[0i]");
    TEST_ASSERT_EQ("(% [-11] 5i)", "[4i]");
    TEST_ASSERT_EQ("(% [-11] 5)", "[4]");
    TEST_ASSERT_EQ("(% [-11] 5.0)", "[4.0]");
    TEST_ASSERT_EQ("(% [-10] [0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% [-10] [0])", "[0Nl]");
    TEST_ASSERT_EQ("(% [-10] [0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% [-10] [5i])", "[0i]");
    TEST_ASSERT_EQ("(% [-11] [5i])", "[4i]");
    TEST_ASSERT_EQ("(% [-11] [5])", "[4]");
    TEST_ASSERT_EQ("(% [-11] [5.0])", "[4.0]");
    TEST_ASSERT_EQ("(% [10] -0i)", "[0Ni]");
    TEST_ASSERT_EQ("(% [10] -5i)", "[0i]");
    TEST_ASSERT_EQ("(% [11] -5i)", "[-4i]");
    TEST_ASSERT_EQ("(% [11] -5)", "[-4]");
    TEST_ASSERT_EQ("(% [11] -5.0)", "[-4.0]");
    TEST_ASSERT_EQ("(% [10] [-0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% [10] [-0])", "[0Nl]");
    TEST_ASSERT_EQ("(% [10] [-0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% [10] [-5i])", "[0i]");
    TEST_ASSERT_EQ("(% [11] [-5i])", "[-4i]");
    TEST_ASSERT_EQ("(% [11] [-5])", "[-4]");
    TEST_ASSERT_EQ("(% [11] [-5.0])", "[-4.0]");
    TEST_ASSERT_EQ("(% [-10] -0i)", "[0Ni]");
    TEST_ASSERT_EQ("(% [-10] -5i)", "[0i]");
    TEST_ASSERT_EQ("(% [-11] -5i)", "[-1i]");
    TEST_ASSERT_EQ("(% [-11] -5)", "[-1]");
    TEST_ASSERT_EQ("(% [-11] -5.0)", "[-1.0]");
    TEST_ASSERT_EQ("(% [-10] [-0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% [-10] [-5i])", "[0i]");
    TEST_ASSERT_EQ("(% [-11] [-5i])", "[-1i]");
    TEST_ASSERT_EQ("(% [-11] [-5])", "[-1]");
    TEST_ASSERT_EQ("(% [-11] [-5.0])", "[-1.0]");
    TEST_ASSERT_EQ("(% [100000000001] 5i)", "[1i]");
    TEST_ASSERT_EQ("(% [100000000001] [5i])", "[1i]");

    TEST_ASSERT_EQ("(% [10.0] 0i)", "[0Nf]");
    TEST_ASSERT_EQ("(% [10.0] 0)", "[0Nf]");
    TEST_ASSERT_EQ("(% [10.0] 0.0)", "[0Nf]");
    TEST_ASSERT_EQ("(% [10.0] 5i)", "[0.0]");
    TEST_ASSERT_EQ("(% [11.0] 5i)", "[1.0]");
    TEST_ASSERT_EQ("(% [11.0] 5)", "[1.0]");
    TEST_ASSERT_EQ("(% [11.0] 5.0)", "[1.0]");
    TEST_ASSERT_EQ("(% [10.0] [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(% [10.0] [5i])", "[0.0]");
    TEST_ASSERT_EQ("(% [11.0] [5i])", "[1.0]");
    TEST_ASSERT_EQ("(% [11.0] [5])", "[1.0]");
    TEST_ASSERT_EQ("(% [11.0] [5.0])", "[1.0]");
    TEST_ASSERT_EQ("(% [-10.0] 0i)", "[0Nf]");
    TEST_ASSERT_EQ("(% [-10.0] 0)", "[0Nf]");
    TEST_ASSERT_EQ("(% [-10.0] 0.0)", "[0Nf]");
    TEST_ASSERT_EQ("(% [-10.0] 5i)", "[0.0]");
    TEST_ASSERT_EQ("(% [-11.0] 5i)", "[4.0]");
    TEST_ASSERT_EQ("(% [-11.0] 5)", "[4.0]");
    TEST_ASSERT_EQ("(% [-11.0] 5.0)", "[4.0]");
    TEST_ASSERT_EQ("(% [-10.0] [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(% [-10.0] [0])", "[0Nf]");
    TEST_ASSERT_EQ("(% [-10.0] [0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% [-10.0] [5i])", "[0.0]");
    TEST_ASSERT_EQ("(% [-11.0] [5i])", "[4.0]");
    TEST_ASSERT_EQ("(% [-11.0] [5])", "[4.0]");
    TEST_ASSERT_EQ("(% [-11.0] [5.0])", "[4.0]");
    TEST_ASSERT_EQ("(% [10.0] -0i)", "[0Nf]");
    TEST_ASSERT_EQ("(% [10.0] -5i)", "[0.0]");
    TEST_ASSERT_EQ("(% [11.0] -5i)", "[-4.0]");
    TEST_ASSERT_EQ("(% [11.0] -5)", "[-4.0]");
    TEST_ASSERT_EQ("(% [11.0] -5.0)", "[-4.0]");
    TEST_ASSERT_EQ("(% [10.0] [-0i])", "[0Nf]");
    TEST_ASSERT_EQ("(% [10.0] [-0])", "[0Nf]");
    TEST_ASSERT_EQ("(% [10.0] [-0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% [10.0] [-5i])", "[0.0]");
    TEST_ASSERT_EQ("(% [11.0] [-5i])", "[-4.0]");
    TEST_ASSERT_EQ("(% [11.0] [-5])", "[-4.0]");
    TEST_ASSERT_EQ("(% [11.0] [-5.0])", "[-4.0]");
    TEST_ASSERT_EQ("(% [-10.0] -0i)", "[0Nf]");
    TEST_ASSERT_EQ("(% [-10.0] -5i)", "[0.0]");
    TEST_ASSERT_EQ("(% [-11.0] -5i)", "[-1.0]");
    TEST_ASSERT_EQ("(% [-11.0] -5)", "[-1.0]");
    TEST_ASSERT_EQ("(% [-11.0] -5.0)", "[-1.0]");
    TEST_ASSERT_EQ("(% [-10.0] [-0i])", "[0Nf]");
    TEST_ASSERT_EQ("(% [-10.0] [-5i])", "[0.0]");
    TEST_ASSERT_EQ("(% [-11.0] [-5i])", "[-1.0]");
    TEST_ASSERT_EQ("(% [-11.0] [-5])", "[-1.0]");
    TEST_ASSERT_EQ("(% [-11.0] [-5.0])", "[-1.0]");
    TEST_ASSERT_EQ("(% [100000000001.0] 5i)", "[1.0]");
    TEST_ASSERT_EQ("(% [100000000001.0] [5i])", "[1.0]");
    TEST_ASSERT_EQ("(% [18.4] 5.1)", "[3.1]");
    TEST_ASSERT_ER("(% 02:15:07.000 02:15:07.000)", "mod: unsupported types: 'time, 'time");

    TEST_ASSERT_EQ("(/ 10:20:15.000 3)", "03:26:45.000");
    TEST_ASSERT_EQ("(/ 10:20:15.000 3i)", "03:26:45.000");
    TEST_ASSERT_EQ("(/ 10:20:15.000 3.0)", "03:26:45.000");
    TEST_ASSERT_EQ("(/ 10:20:15.000 [3])", "[03:26:45.000]");
    TEST_ASSERT_EQ("(/ 10:20:15.000 [3i])", "[03:26:45.000]");
    TEST_ASSERT_EQ("(/ 10:20:15.000 [3.0])", "[03:26:45.000]");
    TEST_ASSERT_EQ("(/ [10:20:15.000] 3)", "[03:26:45.000]");
    TEST_ASSERT_EQ("(/ [10:20:15.000] 3i)", "[03:26:45.000]");
    TEST_ASSERT_EQ("(/ [10:20:15.000] 3.0)", "[03:26:45.000]");
    TEST_ASSERT_EQ("(/ [10:20:15.000] [3])", "[03:26:45.000]");
    TEST_ASSERT_EQ("(/ [10:20:15.000] [3i])", "[03:26:45.000]");
    TEST_ASSERT_EQ("(/ [10:20:15.000] [3.0])", "[03:26:45.000]");
    TEST_ASSERT_EQ("(% 10:20:15.000 100000)", "00:00:15.000");
    TEST_ASSERT_EQ("(% 10:20:15.000 100000i)", "00:00:15.000");
    TEST_ASSERT_EQ("(% 10:20:15.000 [100000])", "[00:00:15.000]");
    TEST_ASSERT_EQ("(% 10:20:15.000 [100000i])", "[00:00:15.000]");
    TEST_ASSERT_EQ("(% [10:20:15.000] 100000)", "[00:00:15.000]");
    TEST_ASSERT_EQ("(% [10:20:15.000] 100000i)", "[00:00:15.000]");
    TEST_ASSERT_EQ("(% [10:20:15.000] [100000])", "[00:00:15.000]");
    TEST_ASSERT_EQ("(% [10:20:15.000] [100000i])", "[00:00:15.000]");

    TEST_ASSERT_EQ("(div 0i -5i)", "0.00");
    TEST_ASSERT_EQ("(div -10i 5i)", "-2.0");
    TEST_ASSERT_EQ("(div -9i 5i)", "-1.8");
    TEST_ASSERT_EQ("(div -3i 5i)", "-0.6");
    TEST_ASSERT_EQ("(div -3i 1i)", "-3.0");
    TEST_ASSERT_EQ("(div -3i 0i)", "0Nf");
    TEST_ASSERT_EQ("(div 3i 0i)", "0Nf");
    TEST_ASSERT_EQ("(div 3i 5i)", "0.6");
    TEST_ASSERT_EQ("(div 9i 5i)", "1.8");
    TEST_ASSERT_EQ("(div 10i 5i)", "2.0");
    TEST_ASSERT_EQ("(div -10i -5i)", "2.0");
    TEST_ASSERT_EQ("(div -9i -5i)", "1.8");
    TEST_ASSERT_EQ("(div -3i -5i)", "0.6");
    TEST_ASSERT_EQ("(div -3i -1i)", "3.0");
    TEST_ASSERT_EQ("(div -3i -0i)", "0Nf");
    TEST_ASSERT_EQ("(div 3i -0i)", "0Nf");
    TEST_ASSERT_EQ("(div 3i -5i)", "-0.6");
    TEST_ASSERT_EQ("(div 9i -5i)", "-1.8");
    TEST_ASSERT_EQ("(div 10i -5i)", "-2.0");
    TEST_ASSERT_EQ("(div -10i 5)", "-2.0");
    TEST_ASSERT_EQ("(div -9i 5)", "-1.8");
    TEST_ASSERT_EQ("(div -3i 5)", "-0.6");
    TEST_ASSERT_EQ("(div -3i 0)", "0Nf");
    TEST_ASSERT_EQ("(div 3i 0)", "0Nf");
    TEST_ASSERT_EQ("(div 3i 5)", "0.6");
    TEST_ASSERT_EQ("(div 9i 5)", "1.8");
    TEST_ASSERT_EQ("(div 10i 5)", "2.0");
    TEST_ASSERT_EQ("(div -10i -5)", "2.0");
    TEST_ASSERT_EQ("(div -9i -5)", "1.8");
    TEST_ASSERT_EQ("(div -3i -5)", "0.6");
    TEST_ASSERT_EQ("(div -3i -0)", "0Nf");
    TEST_ASSERT_EQ("(div 3i -0)", "0Nf");
    TEST_ASSERT_EQ("(div 3i -5)", "-0.6");
    TEST_ASSERT_EQ("(div 9i -5)", "-1.8");
    TEST_ASSERT_EQ("(div 10i -5)", "-2.0");
    TEST_ASSERT_EQ("(div -10i 5.0)", "-2.0");
    TEST_ASSERT_EQ("(div -9i 5.0)", "-1.8");
    TEST_ASSERT_EQ("(div -3i 5.0)", "-0.6");
    TEST_ASSERT_EQ("(div -3i 0.6)", "-5.0");
    TEST_ASSERT_EQ("(div -3i 0.0)", "0Nf");
    TEST_ASSERT_EQ("(div 3i 0.0)", "0Nf");
    TEST_ASSERT_EQ("(div 3i 5.0)", "0.6");
    TEST_ASSERT_EQ("(div 9i 5.0)", "1.8");
    TEST_ASSERT_EQ("(div 10i 5.0)", "2.0");
    TEST_ASSERT_EQ("(div -10i -5.0)", "2.0");
    TEST_ASSERT_EQ("(div -9i -5.0)", "1.8");
    TEST_ASSERT_EQ("(div -3i -5.0)", "0.6");
    TEST_ASSERT_EQ("(div -3i -0.6)", "5.0");
    TEST_ASSERT_EQ("(div -3i -0.0)", "0Nf");
    TEST_ASSERT_EQ("(div 3i -0.0)", "0Nf");
    TEST_ASSERT_EQ("(div 3i -5.0)", "-0.6");
    TEST_ASSERT_EQ("(div 9i -5.0)", "-1.8");
    TEST_ASSERT_EQ("(div 10i -5.0)", "-2.0");
    TEST_ASSERT_EQ("(div -10i [5i])", "[-2.0]");
    TEST_ASSERT_EQ("(div -9i [5i])", "[-1.8]");
    TEST_ASSERT_EQ("(div -3i [5i])", "[-0.6]");
    TEST_ASSERT_EQ("(div -3i [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3i [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3i [5i])", "[0.6]");
    TEST_ASSERT_EQ("(div 9i [5i])", "[1.8]");
    TEST_ASSERT_EQ("(div 10i [5i])", "[2.0]");
    TEST_ASSERT_EQ("(div -10i [-5i])", "[2.0]");
    TEST_ASSERT_EQ("(div -9i [-5i])", "[1.8]");
    TEST_ASSERT_EQ("(div -3i [-5i])", "[0.6]");
    TEST_ASSERT_EQ("(div -3i [-0i])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3i [-0i])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3i [-5i])", "[-0.6]");
    TEST_ASSERT_EQ("(div 9i [-5i])", "[-1.8]");
    TEST_ASSERT_EQ("(div 10i [-5i])", "[-2.0]");
    TEST_ASSERT_EQ("(div -10i [5])", "[-2.0]");
    TEST_ASSERT_EQ("(div -9i [5])", "[-1.8]");
    TEST_ASSERT_EQ("(div -3i [5])", "[-0.6]");
    TEST_ASSERT_EQ("(div -3i [0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3i [0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3i [5])", "[0.6]");
    TEST_ASSERT_EQ("(div 9i [5])", "[1.8]");
    TEST_ASSERT_EQ("(div 10i [5])", "[2.0]");
    TEST_ASSERT_EQ("(div -10i [-5])", "[2.0]");
    TEST_ASSERT_EQ("(div -9i [-5])", "[1.8]");
    TEST_ASSERT_EQ("(div -3i [-5])", "[0.6]");
    TEST_ASSERT_EQ("(div -3i [-0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3i [-0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3i [-5])", "[-0.6]");
    TEST_ASSERT_EQ("(div 9i [-5])", "[-1.8]");
    TEST_ASSERT_EQ("(div 10i [-5])", "[-2.0]");
    TEST_ASSERT_EQ("(div -10i [5.0])", "[-2.0]");
    TEST_ASSERT_EQ("(div -9i [5.0])", "[-1.8]");
    TEST_ASSERT_EQ("(div -3i [5.0])", "[-0.6]");
    TEST_ASSERT_EQ("(div -3i [0.6])", "[-5.0]");
    TEST_ASSERT_EQ("(div -3i [0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3i [0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3i [5.0])", "[0.6]");
    TEST_ASSERT_EQ("(div 9i [5.0])", "[1.8]");
    TEST_ASSERT_EQ("(div 10i [5.0])", "[2.0]");
    TEST_ASSERT_EQ("(div -10i [-5.0])", "[2.0]");
    TEST_ASSERT_EQ("(div -9i [-5.0])", "[1.8]");
    TEST_ASSERT_EQ("(div -3i [-5.0])", "[0.6]");
    TEST_ASSERT_EQ("(div -3i [-0.6])", "[5.0]");
    TEST_ASSERT_EQ("(div -3i [-0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3i [-0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3i [-5.0])", "[-0.6]");
    TEST_ASSERT_EQ("(div 9i [-5.0])", "[-1.8]");
    TEST_ASSERT_EQ("(div 10i [-5.0])", "[-2.0]");
    TEST_ASSERT_EQ("(div 10i [])", "[]");
    TEST_ASSERT_EQ("(div [10i] 0Ni)", "[0Nf]");
    TEST_ASSERT_EQ("(div [10i 5i] 5)", "[2.0 1.0]");
    TEST_ASSERT_EQ("(div [10i 5i] -5.0)", "[-2.0 -1.0]");
    TEST_ASSERT_EQ("(div [10i] [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(div [10i] [5])", "[2.0]");
    TEST_ASSERT_EQ("(div [10i] [-5.0])", "[-2.0]");

    TEST_ASSERT_EQ("(div -10 5i)", "-2.0");
    TEST_ASSERT_EQ("(div -9 5i)", "-1.8");
    TEST_ASSERT_EQ("(div -3 5i)", "-0.6");
    TEST_ASSERT_EQ("(div -3 0i)", "0Nf");
    TEST_ASSERT_EQ("(div 3 0i)", "0Nf");
    TEST_ASSERT_EQ("(div 3 5i)", "0.6");
    TEST_ASSERT_EQ("(div 9 5i)", "1.8");
    TEST_ASSERT_EQ("(div 10 5i)", "2.0");
    TEST_ASSERT_EQ("(div -10 -5i)", "2.0");
    TEST_ASSERT_EQ("(div -9 -5i)", "1.8");
    TEST_ASSERT_EQ("(div -3 -5i)", "0.6");
    TEST_ASSERT_EQ("(div -3 -0i)", "0Nf");
    TEST_ASSERT_EQ("(div 3 -0i)", "0Nf");
    TEST_ASSERT_EQ("(div 3 -5i)", "-0.6");
    TEST_ASSERT_EQ("(div 9 -5i)", "-1.8");
    TEST_ASSERT_EQ("(div 10 -5i)", "-2.0");
    TEST_ASSERT_EQ("(div -10 5)", "-2.0");
    TEST_ASSERT_EQ("(div -9 5)", "-1.8");
    TEST_ASSERT_EQ("(div -3 5)", "-0.6");
    TEST_ASSERT_EQ("(div -3 0)", "0Nf");
    TEST_ASSERT_EQ("(div 3 0)", "0Nf");
    TEST_ASSERT_EQ("(div 3 5)", "0.6");
    TEST_ASSERT_EQ("(div 9 5)", "1.8");
    TEST_ASSERT_EQ("(div 10 5)", "2.0");
    TEST_ASSERT_EQ("(div -10 -5)", "2.0");
    TEST_ASSERT_EQ("(div -9 -5)", "1.8");
    TEST_ASSERT_EQ("(div -3 -5)", "0.6");
    TEST_ASSERT_EQ("(div -3 -0)", "0Nf");
    TEST_ASSERT_EQ("(div 3 -0)", "0Nf");
    TEST_ASSERT_EQ("(div 3 -5)", "-0.6");
    TEST_ASSERT_EQ("(div 9 -5)", "-1.8");
    TEST_ASSERT_EQ("(div 10 -5)", "-2.0");
    TEST_ASSERT_EQ("(div -10 5.0)", "-2.0");
    TEST_ASSERT_EQ("(div -9 5.0)", "-1.8");
    TEST_ASSERT_EQ("(div -3 5.0)", "-0.6");
    TEST_ASSERT_EQ("(div -3 0.0)", "0Nf");
    TEST_ASSERT_EQ("(div 3 0.0)", "0Nf");
    TEST_ASSERT_EQ("(div -3 0.6)", "-5.0");
    TEST_ASSERT_EQ("(div 3 5.0)", "0.6");
    TEST_ASSERT_EQ("(div 9 5.0)", "1.8");
    TEST_ASSERT_EQ("(div 10 5.0)", "2.0");
    TEST_ASSERT_EQ("(div -10 -5.0)", "2.0");
    TEST_ASSERT_EQ("(div -9 -5.0)", "1.8");
    TEST_ASSERT_EQ("(div -3 -5.0)", "0.6");
    TEST_ASSERT_EQ("(div -3 -0.6)", "5.0");
    TEST_ASSERT_EQ("(div -3 -0.0)", "0Nf");
    TEST_ASSERT_EQ("(div 3 -0.0)", "0Nf");
    TEST_ASSERT_EQ("(div 3 -5.0)", "-0.6");
    TEST_ASSERT_EQ("(div 9 -5.0)", "-1.8");
    TEST_ASSERT_EQ("(div 10 -5.0)", "-2.0");
    TEST_ASSERT_EQ("(div -10 [5i])", "[-2.0]");
    TEST_ASSERT_EQ("(div -10 [5])", "[-2.0]");
    TEST_ASSERT_EQ("(div -9 [5])", "[-1.8]");
    TEST_ASSERT_EQ("(div -3 [5])", "[-0.6]");
    TEST_ASSERT_EQ("(div -3 [0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3 [0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3 [5])", "[0.6]");
    TEST_ASSERT_EQ("(div 9 [5])", "[1.8]");
    TEST_ASSERT_EQ("(div 10 [5])", "[2.0]");
    TEST_ASSERT_EQ("(div -10 [-5])", "[2.0]");
    TEST_ASSERT_EQ("(div -9 [-5])", "[1.8]");
    TEST_ASSERT_EQ("(div -3 [-5])", "[0.6]");
    TEST_ASSERT_EQ("(div -3 [-0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3 [-0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3 [-5])", "[-0.6]");
    TEST_ASSERT_EQ("(div 9 [-5])", "[-1.8]");
    TEST_ASSERT_EQ("(div 10 [-5])", "[-2.0]");
    TEST_ASSERT_EQ("(div -10 [5])", "[-2.0]");
    TEST_ASSERT_EQ("(div -9 [5])", "[-1.8]");
    TEST_ASSERT_EQ("(div -3 [5])", "[-0.6]");
    TEST_ASSERT_EQ("(div -3 [0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3 [0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3 [5])", "[0.6]");
    TEST_ASSERT_EQ("(div 9 [5])", "[1.8]");
    TEST_ASSERT_EQ("(div 10 [5])", "[2.0]");
    TEST_ASSERT_EQ("(div -10 [-5])", "[2.0]");
    TEST_ASSERT_EQ("(div -9 [-5])", "[1.8]");
    TEST_ASSERT_EQ("(div -3 [-5])", "[0.6]");
    TEST_ASSERT_EQ("(div -3 [-0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3 [-0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3 [-5])", "[-0.6]");
    TEST_ASSERT_EQ("(div 9 [-5])", "[-1.8]");
    TEST_ASSERT_EQ("(div 10 [-5])", "[-2.0]");
    TEST_ASSERT_EQ("(div -10 [5.0])", "[-2.0]");
    TEST_ASSERT_EQ("(div -9 [5.0])", "[-1.8]");
    TEST_ASSERT_EQ("(div -3 [5.0])", "[-0.6]");
    TEST_ASSERT_EQ("(div -3 [0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3 [0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(div -3 [0.6])", "[-5.0]");
    TEST_ASSERT_EQ("(div 3 [5.0])", "[0.6]");
    TEST_ASSERT_EQ("(div 9 [5.0])", "[1.8]");
    TEST_ASSERT_EQ("(div 10 [5.0])", "[2.0]");
    TEST_ASSERT_EQ("(div -10 [-5.0])", "[2.0]");
    TEST_ASSERT_EQ("(div -9 [-5.0])", "[1.8]");
    TEST_ASSERT_EQ("(div -3 [-5.0])", "[0.6]");
    TEST_ASSERT_EQ("(div -3 [-0.6])", "[5.0]");
    TEST_ASSERT_EQ("(div -3 [-0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3 [-0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3 [-5.0])", "[-0.6]");
    TEST_ASSERT_EQ("(div 9 [-5.0])", "[-1.8]");
    TEST_ASSERT_EQ("(div 10 [-5.0])", "[-2.0]");
    TEST_ASSERT_EQ("(div 10 [])", "[]");
    TEST_ASSERT_EQ("(div [10] 0Ni)", "[0Nf]");
    TEST_ASSERT_EQ("(div [10 5] 5)", "[2.0 1.0]");
    TEST_ASSERT_EQ("(div [10 5] -5.0)", "[-2.0 -1.0]");
    TEST_ASSERT_EQ("(div [10] [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(div [10] [5])", "[2.0]");
    TEST_ASSERT_EQ("(div [10] [-5.0])", "[-2.0]");

    TEST_ASSERT_EQ("(div -10.0 5i)", "-2.0");
    TEST_ASSERT_EQ("(div -9.0 5i)", "-1.8");
    TEST_ASSERT_EQ("(div -3.0 5i)", "-0.6");
    TEST_ASSERT_EQ("(div -3.0 0i)", "0Nf");
    TEST_ASSERT_EQ("(div 3.0 0i)", "0Nf");
    TEST_ASSERT_EQ("(div 3.0 5i)", "0.6");
    TEST_ASSERT_EQ("(div 9.0 5i)", "1.8");
    TEST_ASSERT_EQ("(div 10.0 5i)", "2.0");
    TEST_ASSERT_EQ("(div -10.0 -5i)", "2.0");
    TEST_ASSERT_EQ("(div -9.0 -5i)", "1.8");
    TEST_ASSERT_EQ("(div -3.0 -5i)", "0.6");
    TEST_ASSERT_EQ("(div -3.0 -0i)", "0Nf");
    TEST_ASSERT_EQ("(div 3.0 -0i)", "0Nf");
    TEST_ASSERT_EQ("(div 3.0 -5i)", "-0.6");
    TEST_ASSERT_EQ("(div 9.0 -5i)", "-1.8");
    TEST_ASSERT_EQ("(div 10.0 -5i)", "-2.0");
    TEST_ASSERT_EQ("(div -10.0 5)", "-2.0");
    TEST_ASSERT_EQ("(div -9.0 5)", "-1.8");
    TEST_ASSERT_EQ("(div -3.0 5)", "-0.6");
    TEST_ASSERT_EQ("(div -3.0 0)", "0Nf");
    TEST_ASSERT_EQ("(div 3.0 0)", "0Nf");
    TEST_ASSERT_EQ("(div 3.0 5)", "0.6");
    TEST_ASSERT_EQ("(div 9.0 5)", "1.8");
    TEST_ASSERT_EQ("(div 10.0 5)", "2.0");
    TEST_ASSERT_EQ("(div -10.0 -5)", "2.0");
    TEST_ASSERT_EQ("(div -9.0 -5)", "1.8");
    TEST_ASSERT_EQ("(div -3.0 -5)", "0.6");
    TEST_ASSERT_EQ("(div -3.0 -0)", "0Nf");
    TEST_ASSERT_EQ("(div 3.0 -0)", "0Nf");
    TEST_ASSERT_EQ("(div 3.0 -5)", "-0.6");
    TEST_ASSERT_EQ("(div 9.0 -5)", "-1.8");
    TEST_ASSERT_EQ("(div 10.0 -5)", "-2.0");
    TEST_ASSERT_EQ("(div -10.0 5.0)", "-2.0");
    TEST_ASSERT_EQ("(div -9.0 5.0)", "-1.8");
    TEST_ASSERT_EQ("(div -3.0 5.0)", "-0.6");
    TEST_ASSERT_EQ("(div -3.0 0.6)", "-5.0");
    TEST_ASSERT_EQ("(div -3.0 0.0)", "0Nf");
    TEST_ASSERT_EQ("(div 3.0 0.0)", "0Nf");
    TEST_ASSERT_EQ("(div 3.0 5.0)", "0.6");
    TEST_ASSERT_EQ("(div 9.0 5.0)", "1.8");
    TEST_ASSERT_EQ("(div 10.0 5.0)", "2.0");
    TEST_ASSERT_EQ("(div -10.0 -5.0)", "2.0");
    TEST_ASSERT_EQ("(div -9.0 -5.0)", "1.8");
    TEST_ASSERT_EQ("(div -3.0 -5.0)", "0.6");
    TEST_ASSERT_EQ("(div -3.0 -0.6)", "5.0");
    TEST_ASSERT_EQ("(div -3.0 -0.0)", "0Nf");
    TEST_ASSERT_EQ("(div 3.0 -0.0)", "0Nf");
    TEST_ASSERT_EQ("(div 3.0 -5.0)", "-0.6");
    TEST_ASSERT_EQ("(div 9.0 -5.0)", "-1.8");
    TEST_ASSERT_EQ("(div 10.0 -5.0)", "-2.0");
    TEST_ASSERT_EQ("(div -10.0 [5i])", "[-2.0]");
    TEST_ASSERT_EQ("(div -10.0 [5])", "[-2.0]");
    TEST_ASSERT_EQ("(div -9.0 [5])", "[-1.8]");
    TEST_ASSERT_EQ("(div -3.0 [5])", "[-0.6]");
    TEST_ASSERT_EQ("(div -3.0 [0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3.0 [0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3.0 [5])", "[0.6]");
    TEST_ASSERT_EQ("(div 9.0 [5])", "[1.8]");
    TEST_ASSERT_EQ("(div 10.0 [5])", "[2.0]");
    TEST_ASSERT_EQ("(div -10.0 [-5])", "[2.0]");
    TEST_ASSERT_EQ("(div -9.0 [-5])", "[1.8]");
    TEST_ASSERT_EQ("(div -3.0 [-5])", "[0.6]");
    TEST_ASSERT_EQ("(div -3.0 [-0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3.0 [-0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3.0 [-5])", "[-0.6]");
    TEST_ASSERT_EQ("(div 9.0 [-5])", "[-1.8]");
    TEST_ASSERT_EQ("(div 10.0 [-5])", "[-2.0]");
    TEST_ASSERT_EQ("(div -10.0 [5])", "[-2.0]");
    TEST_ASSERT_EQ("(div -9.0 [5])", "[-1.8]");
    TEST_ASSERT_EQ("(div -3.0 [5])", "[-0.6]");
    TEST_ASSERT_EQ("(div -3.0 [0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3.0 [0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3.0 [5])", "[0.6]");
    TEST_ASSERT_EQ("(div 9.0 [5])", "[1.8]");
    TEST_ASSERT_EQ("(div 10.0 [5])", "[2.0]");
    TEST_ASSERT_EQ("(div -10.0 [-5])", "[2.0]");
    TEST_ASSERT_EQ("(div -9.0 [-5])", "[1.8]");
    TEST_ASSERT_EQ("(div -3.0 [-5])", "[0.6]");
    TEST_ASSERT_EQ("(div -3.0 [-0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3.0 [-0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3.0 [-5])", "[-0.6]");
    TEST_ASSERT_EQ("(div 9.0 [-5])", "[-1.8]");
    TEST_ASSERT_EQ("(div 10.0 [-5])", "[-2.0]");
    TEST_ASSERT_EQ("(div -10.0 [5.0])", "[-2.0]");
    TEST_ASSERT_EQ("(div -9.0 [5.0])", "[-1.8]");
    TEST_ASSERT_EQ("(div -3.0 [5.0])", "[-0.6]");
    TEST_ASSERT_EQ("(div -3.0 [0.6])", "[-5.0]");
    TEST_ASSERT_EQ("(div -3.0 [0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3.0 [0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(div 3.0 [5.0])", "[0.6]");
    TEST_ASSERT_EQ("(div 9.0 [5.0])", "[1.8]");
    TEST_ASSERT_EQ("(div 10.0 [5.0])", "[2.0]");
    TEST_ASSERT_EQ("(div [-10.0] -5.0)", "[2.0]");
    TEST_ASSERT_EQ("(div [-9.0] -5.0)", "[1.8]");
    TEST_ASSERT_EQ("(div [-3.0] -5.0)", "[0.6]");
    TEST_ASSERT_EQ("(div [-3.0] -0.6)", "[5.0]");
    TEST_ASSERT_EQ("(div [-3.0] -0.0)", "[0Nf]");
    TEST_ASSERT_EQ("(div [3.0] [-0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(div [3.0] [-5.0])", "[-0.6]");
    TEST_ASSERT_EQ("(div [9.0] [-5.0])", "[-1.8]");
    TEST_ASSERT_EQ("(div [10.0] [-5.0])", "[-2.0]");
    TEST_ASSERT_EQ("(div [11.5] [1.0])", "[11.5]");
    TEST_ASSERT_EQ("(div 11.5 1.0)", "11.5");
    TEST_ASSERT_EQ("(div [10.0] 0Ni)", "[0Nf]");
    TEST_ASSERT_EQ("(div [10.0 5.0] 5)", "[2.0 1.0]");
    TEST_ASSERT_EQ("(div [10.0] [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(div [10.0] [5])", "[2.0]");

    TEST_ASSERT_ER("(div 02:15:07.000 02:15:07.000)", "fdiv: unsupported types: 'time, 'time");

    TEST_ASSERT_EQ("(xbar (- (til 10) 5) 3i)", "[-6 -6 -3 -3 -3 0 0 0 3 3]");
    TEST_ASSERT_EQ("(xbar (- (til 15) 5) 3)", "[-6 -6 -3 -3 -3 0 0 0 3 3 3 6 6 6 9]");
    TEST_ASSERT_EQ("(xbar (- (as 'F64 (til 9)) 5.0) 3.0)", "[-6.0 -6.0 -3.0 -3.0 -3.0 0.0 0.0 0.0 3.0]");
    TEST_ASSERT_EQ(
        "(xbar (list 10i 11i 12i 13i 14i [15i] [16i] [17i] [18i]) (list 4 4.0 [4i] [4] [4.0] 4i 4.0 [4i] [4.0]))",
        "(list 8 8.00 [12i] [12] [12.00] [12i] [16.00] [16i] [16.00])");
    TEST_ASSERT_EQ("(xbar (list [-3] [-2] [-1] [0] 1 2 3 4 5 6 ) (list [4.0] [4] [4i] 4.0 [4.0] [4] [4i] 4.0 4 4i))",
                   "(list [-4.0] [-4] [-4] [0.0] [0.0] [0] [0] 4.0 4 4)");
    TEST_ASSERT_EQ("(xbar (list [-4i] [7i] [8i] 9i) (list [4] 4.0 4 4i))", "(list [-4] [4.0] [8] 8i)");
    TEST_ASSERT_EQ("(xbar (list -5.0 -6.0 -7.0 -8.0 -9.0 -10.0 [-11.0] [-12.0]) (list [4i] [4] [4.0] 4i 4 4.0 4i 4))",
                   "(list [-8.0] [-8.0] [-8.0] -8.0 -12.0 -12.0 [-12.0] [-12.0])");
    TEST_ASSERT_EQ("(xbar (list [-13.0] [-14.0] [-15.0]) (list [4i] [4] [4.0]))", "(list [-16.0] [-16.0] [-16.0])");
    TEST_ASSERT_EQ(
        "(xbar (list 2020.01.01 2020.01.02 2020.01.03 2020.01.04 [2020.01.05] [2020.01.06] [2020.01.07] [2020.01.08]) "
        "(list 2i 2 [2i] [2] 2i 2 [2i] [2]))",
        "(list 2019.12.31 2020.01.02 [2020.01.02] [2020.01.04] [2020.01.04] [2020.01.06] [2020.01.06] [2020.01.08])");
    TEST_ASSERT_EQ(
        "(xbar (list 10:20:30.400 10:20:30.800 10:20:31.200 10:20:32.000 10:20:33.500 10:20:33.900) "
        "(list [1000i] 1000 00:00:01.000 1000i [1000] [00:00:01.000]))",
        "(list [10:20:30.000] 10:20:30.000 10:20:31.000 10:20:32.000 [10:20:33.000] [10:20:33.000])");
    TEST_ASSERT_EQ(
        "(xbar (list [10:20:30.400] [10:20:30.800] [10:20:31.200] [10:20:32.000] [10:20:33.500] [10:20:33.900]) "
        "(list [1000i] 1000 00:00:01.000 1000i [1000] [00:00:01.000]))",
        "(list [10:20:30.000] [10:20:30.000] [10:20:31.000] [10:20:32.000] [10:20:33.000] [10:20:33.000])");
    TEST_ASSERT_EQ("(xbar (list 2025.02.03D12:13:14.123456789 2025.02.03D12:13:14.123456789) (list [10000i] 10000i))",
                   "(list [2025.02.03D12:13:14.123450000] 2025.02.03D12:13:14.123450000)");
    TEST_ASSERT_EQ(
        "(xbar (list 2025.02.03D12:13:14.123456789 2025.02.03D12:13:14.123456789) (list [10000] 00:00:00.010))",
        "(list [2025.02.03D12:13:14.123450000] 2025.02.03D12:13:14.120000000)");
    TEST_ASSERT_EQ(
        "(xbar (list 2025.02.03D12:13:14.123456789 2025.02.03D12:13:14.123456789) (list [00:00:00.010] 10000))",
        "(list [2025.02.03D12:13:14.120000000] 2025.02.03D12:13:14.123450000)");
    TEST_ASSERT_EQ(
        "(xbar (list [2025.02.03D12:13:14.123456789] [2025.02.03D12:13:14.123456789]) (list [10000i] 10000i))",
        "(list [2025.02.03D12:13:14.123450000] [2025.02.03D12:13:14.123450000])");
    TEST_ASSERT_EQ(
        "(xbar (list [2025.02.03D12:13:14.123456789] [2025.02.03D12:13:14.123456789]) (list [10000] 00:00:00.010))",
        "(list [2025.02.03D12:13:14.123450000] [2025.02.03D12:13:14.120000000])");
    TEST_ASSERT_EQ(
        "(xbar (list [2025.02.03D12:13:14.123456789] [2025.02.03D12:13:14.123456789]) (list [00:00:00.010] 10000))",
        "(list [2025.02.03D12:13:14.120000000] [2025.02.03D12:13:14.123450000])");

    TEST_ASSERT_ER("(xbar 00:00:05.000 2.7)", "xbar: unsupported types: 'time, 'f64");

    TEST_ASSERT_EQ("(sum 5i)", "5i");
    TEST_ASSERT_EQ("(sum -1.7)", "-1.7");
    TEST_ASSERT_EQ("(sum [-24 12 3])", "-9");
    TEST_ASSERT_EQ("(sum -24)", "-24");
    TEST_ASSERT_EQ("(sum [1.0 2.0 3.0])", "6.0");
    TEST_ASSERT_EQ("(sum [1 2 3 0Nl 4])", "10");
    TEST_ASSERT_EQ("(sum [1i 2i -3i])", "0i");
    TEST_ASSERT_EQ("(sum [02:01:03.000 00:00:02.500])", "02:01:05.500");
    TEST_ASSERT_ER("(sum [2020.02.03 2025.02.03])", "sum: unsupported type: 'Date");

    TEST_ASSERT_EQ("(avg 5i)", "5.0");
    TEST_ASSERT_EQ("(avg -1.7)", "-1.7");
    TEST_ASSERT_EQ("(avg [-24 12 6])", "-2.0");
    TEST_ASSERT_EQ("(avg -24)", "-24.0");
    TEST_ASSERT_EQ("(avg [1.0 2.0 3.0])", "2.0");
    TEST_ASSERT_EQ("(avg [1i 2i -3i])", "0.0");
    TEST_ASSERT_EQ("(avg [-24 12 6 0Nl])", "-2.0");
    TEST_ASSERT_EQ("(avg [0Ni])", "0Nf");
    TEST_ASSERT_EQ("(avg 0Nf)", "0Nf");

    TEST_ASSERT_EQ("(min 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(min 5i)", "5i");
    TEST_ASSERT_EQ("(min -1.7)", "-1.7");
    TEST_ASSERT_EQ("(min -24)", "-24");
    TEST_ASSERT_EQ("(min 2020.03.05)", "2020.03.05");
    TEST_ASSERT_EQ("(min -00:00:05.000)", "-00:00:05.000");
    TEST_ASSERT_EQ("(min 1999.03.13D11:45:43.848458167)", "1999.03.13D11:45:43.848458167");
    TEST_ASSERT_EQ("(min [1i 2i -3i])", "-3i");
    TEST_ASSERT_EQ("(min [0Ni -24i 12i 6i])", "-24i");
    TEST_ASSERT_EQ("(min [1.0 2.0 3.0 0Nf])", "1.0");
    TEST_ASSERT_EQ("(min [-24 12 6 0Nl])", "-24");
    TEST_ASSERT_EQ("(min [0Ni])", "0Ni");
    TEST_ASSERT_EQ("(min [2020.03.05])", "2020.03.05");
    TEST_ASSERT_EQ("(min [-00:00:05.000])", "-00:00:05.000");
    TEST_ASSERT_EQ("(min [1999.03.13D11:45:43.848458167])", "1999.03.13D11:45:43.848458167");
    TEST_ASSERT_EQ("(min [])", "0Nl");

    TEST_ASSERT_EQ("(max 0Nt)", "0Nt");
    TEST_ASSERT_EQ("(max 5i)", "5i");
    TEST_ASSERT_EQ("(max -24)", "-24");
    TEST_ASSERT_EQ("(max -1.7)", "-1.7");
    TEST_ASSERT_EQ("(max [-24 12 6])", "12");
    TEST_ASSERT_EQ("(max 2020.03.05)", "2020.03.05");
    TEST_ASSERT_EQ("(max -00:00:05.000)", "-00:00:05.000");
    TEST_ASSERT_EQ("(max 1999.03.13D11:45:43.848458167)", "1999.03.13D11:45:43.848458167");
    TEST_ASSERT_EQ("(max [1i 2i -3i])", "2i");
    TEST_ASSERT_EQ("(max [0Ni -24i 12i 6i])", "12i");
    TEST_ASSERT_EQ("(max [1.0 2.0 3.0 0Nf])", "3.0");
    TEST_ASSERT_EQ("(max [-24 12 6 0Nl])", "12");
    TEST_ASSERT_EQ("(max [0Ni])", "0Ni");
    TEST_ASSERT_EQ("(max [2020.03.05])", "2020.03.05");
    TEST_ASSERT_EQ("(max [-00:00:05.000])", "-00:00:05.000");
    TEST_ASSERT_EQ("(max [1999.03.13D11:45:43.848458167])", "1999.03.13D11:45:43.848458167");
    TEST_ASSERT_EQ("(max [])", "0Nl");

    TEST_ASSERT_EQ("(round [])", "[]");
    TEST_ASSERT_EQ("(round -0.5)", "-1.0");
    TEST_ASSERT_EQ("(round [-1.5 -1.1 -0.0 0Nf 0.0 1.1 1.5])", "[-2.0 -1.0 0.0 0Nf 0.0 1.0 2.0]");
    TEST_ASSERT_EQ("(round 0Nf)", "0Nf");

    TEST_ASSERT_EQ("(floor [1.1 2.5 -1.1])", "[1.0 2.0 -2.0]");
    TEST_ASSERT_EQ("(floor -0.0)", "0.0");
    TEST_ASSERT_EQ("(floor [-1.0 3.0])", "[-1.0 3.0]");
    TEST_ASSERT_EQ("(floor 0.0)", "0.0");
    TEST_ASSERT_EQ("(floor 1.0)", "1.0");
    TEST_ASSERT_EQ("(floor -1.0)", "-1.0");
    TEST_ASSERT_EQ("(floor 1.5)", "1.0");
    TEST_ASSERT_EQ("(floor -1.5)", "-2.0");
    TEST_ASSERT_EQ("(floor 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(floor -5i)", "-5i");
    TEST_ASSERT_ER("(floor 'i64)", "floor: unsupported type: 'symbol");

    TEST_ASSERT_EQ("(ceil -5i)", "-5i");
    TEST_ASSERT_EQ("(ceil [1.1 2.5 -1.1])", "[2.0 3.0 -1.0]");
    TEST_ASSERT_EQ("(ceil -0.0)", "0.0");
    TEST_ASSERT_EQ("(ceil [-1.0 3.0])", "[-1.0 3.0]");
    TEST_ASSERT_EQ("(ceil 0.0)", "0.0");
    TEST_ASSERT_EQ("(ceil 1.0)", "1.0");
    TEST_ASSERT_EQ("(ceil -1.0)", "-1.0");
    TEST_ASSERT_EQ("(ceil 1.5)", "2.0");
    TEST_ASSERT_EQ("(ceil -1.5)", "-1.0");
    TEST_ASSERT_EQ("(ceil 0Nf)", "0Nf");
    TEST_ASSERT_ER("(ceil 'i64)", "ceil: unsupported type: 'symbol");

    TEST_ASSERT_EQ("(med 2i)", "2.0");
    TEST_ASSERT_EQ("(med -5)", "-5.0");
    TEST_ASSERT_EQ("(med 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(med [])", "0Nf");
    // TEST_ASSERT_EQ("(med [1i 2i 3i])", "2.0");
    TEST_ASSERT_EQ("(med [3 1 2])", "2.0");
    TEST_ASSERT_EQ("(med [3 1 2 4])", "2.5");
    // TEST_ASSERT_EQ("(med [0Nl 3 0Nl 1 2])", "2.0");
    // TEST_ASSERT_EQ("(med [0Nl 1 0Nl 2 3])", "2.0");
    // TEST_ASSERT_EQ("(med [0Ni 0Ni])", "0Nf");
    // TEST_ASSERT_EQ("(med [1.0 2.0 3.0 4.0 0Nf 0Nf])", "2.5");

    TEST_ASSERT_EQ("(dev 1)", "0.0");
    TEST_ASSERT_EQ("(dev [0Ni])", "0Nf");
    TEST_ASSERT_EQ("(dev [1i 2i])", "0.5");
    TEST_ASSERT_EQ("(dev [1 0Nl 2])", "0.5");
    TEST_ASSERT_EQ("(dev [1 2 3 4 50])", "19.0263");
    TEST_ASSERT_EQ("(dev [0Nl 1 2 3 4 50 0Nl])", "19.0263");
    TEST_ASSERT_EQ("(dev [0Nf -2.0 10.0 11.0 5.0 0Nf])", "5.147815");

    TEST_ASSERT_EQ("((fn [x y] (+ x y)) 1 [2.3 4])", "[3.3 5.0]");

    PASS();
}

test_result_t test_lang_take() {
    TEST_ASSERT_EQ("(take 0h false)", "[]");
    TEST_ASSERT_EQ("(type (take 0i false))", "'B8");
    TEST_ASSERT_EQ("(take 2 true)", "[true true]");
    TEST_ASSERT_EQ("(take 3 [false false true true])", "[false false true]");
    TEST_ASSERT_EQ("(take -3 [false false true true])", "[false true true]");
    TEST_ASSERT_EQ("(take 5 [false false true true])", "[false false true true false]");
    TEST_ASSERT_EQ("(take -5 [false false true true])", "[true false false true true]");

    TEST_ASSERT_EQ("(take 0h [0x00])", "[]");
    TEST_ASSERT_EQ("(type (take 0i 0x00))", "'U8");
    TEST_ASSERT_EQ("(take 2 0x01)", "[0x01 0x01]");
    TEST_ASSERT_EQ("(take 3 [0x00 0x01 0x02 0x03])", "[0x00 0x01 0x02]");
    TEST_ASSERT_EQ("(take -3 [0x00 0x01 0x02 0x03])", "[0x01 0x02 0x03]");
    TEST_ASSERT_EQ("(take 5 [0x00 0x01 0x02 0x03])", "[0x00 0x01 0x02 0x03 0x00]");
    TEST_ASSERT_EQ("(take -5 [0x00 0x01 0x02 0x03])", "[0x03 0x00 0x01 0x02 0x03]");

    TEST_ASSERT_EQ("(take 0h \"abcd\")", "\"\"");
    TEST_ASSERT_EQ("(type (take 0i \"abcd\"))", "'String");
    TEST_ASSERT_EQ("(take 2 'a')", "\"aa\"");
    TEST_ASSERT_EQ("(take 3 \"abcd\")", "\"abc\"");
    TEST_ASSERT_EQ("(take -3 \"abcd\")", "\"bcd\"");
    TEST_ASSERT_EQ("(take 5 \"abcd\")", "\"abcda\"");
    TEST_ASSERT_EQ("(take -5 \"abcd\")", "\"dabcd\"");

    TEST_ASSERT_EQ("(take 0h 1h)", "[]");
    TEST_ASSERT_EQ("(type (take 0i 1h))", "'I16");
    TEST_ASSERT_EQ("(take 2 1h)", "[1h 1h]");
    TEST_ASSERT_EQ("(take 3 [0h 1h 2h 3h])", "[0h 1h 2h]");
    TEST_ASSERT_EQ("(take -3 [0h 1h 2h 3h])", "[1h 2h 3h]");
    TEST_ASSERT_EQ("(take 5 [0h 1h 2h 3h])", "[0h 1h 2h 3h 0h]");
    TEST_ASSERT_EQ("(take -5 [0h 1h 2h 3h])", "[3h 0h 1h 2h 3h]");

    TEST_ASSERT_EQ("(take 0h 1i)", "[]");
    TEST_ASSERT_EQ("(type (take 0i 1i))", "'I32");
    TEST_ASSERT_EQ("(take 2 1i)", "[1i 1i]");
    TEST_ASSERT_EQ("(take 3 [0i 1i 2i 3i])", "[0i 1i 2i]");
    TEST_ASSERT_EQ("(take -3 [0i 1i 2i 3i])", "[1i 2i 3i]");
    TEST_ASSERT_EQ("(take 5 [0i 1i 2i 3i])", "[0i 1i 2i 3i 0i]");
    TEST_ASSERT_EQ("(take -5 [0i 1i 2i 3i])", "[3i 0i 1i 2i 3i]");

    TEST_ASSERT_EQ("(take 0 2025.05.01)", "[]");
    TEST_ASSERT_EQ("(type (take 0 2025.05.01))", "'Date");
    TEST_ASSERT_EQ("(take 2 2025.05.01)", "[2025.05.01 2025.05.01]");
    TEST_ASSERT_EQ("(take 3 [2025.05.01 2025.05.02 2025.05.03 2025.05.04])", "[2025.05.01 2025.05.02 2025.05.03]");
    TEST_ASSERT_EQ("(take -3 [2025.05.01 2025.05.02 2025.05.03 2025.05.04])", "[2025.05.02 2025.05.03 2025.05.04]");
    TEST_ASSERT_EQ("(take 5 [2025.05.01 2025.05.02 2025.05.03 2025.05.04])",
                   "[2025.05.01 2025.05.02 2025.05.03 2025.05.04 2025.05.01]");
    TEST_ASSERT_EQ("(take -5 [2025.05.01 2025.05.02 2025.05.03 2025.05.04])",
                   "[2025.05.04 2025.05.01 2025.05.02 2025.05.03 2025.05.04]");
    TEST_ASSERT_EQ("(take 0 00:00:01.000)", "[]");
    TEST_ASSERT_EQ("(type (take 0 00:00:01.000))", "'Time");
    TEST_ASSERT_EQ("(take 2 00:00:01.000)", "[00:00:01.000 00:00:01.000]");
    TEST_ASSERT_EQ("(take 3 [00:00:01.000 00:00:02.000 00:00:03.000 00:00:04.000])",
                   "[00:00:01.000 00:00:02.000 00:00:03.000]");
    TEST_ASSERT_EQ("(take -3 [00:00:01.000 00:00:02.000 00:00:03.000 00:00:04.000])",
                   "[00:00:02.000 00:00:03.000 00:00:04.000]");
    TEST_ASSERT_EQ("(take 5 [00:00:01.000 00:00:02.000 00:00:03.000 00:00:04.000])",
                   "[00:00:01.000 00:00:02.000 00:00:03.000 00:00:04.000 00:00:01.000]");
    TEST_ASSERT_EQ("(take -5 [00:00:01.000 00:00:02.000 00:00:03.000 00:00:04.000])",
                   "[00:00:04.000 00:00:01.000 00:00:02.000 00:00:03.000 00:00:04.000]");

    TEST_ASSERT_EQ("(take 0h 1)", "[]");
    TEST_ASSERT_EQ("(type (take 0i 1))", "'I64");
    TEST_ASSERT_EQ("(take 2 1)", "[1 1]");
    TEST_ASSERT_EQ("(take 3 [0 1 2 3])", "[0 1 2]");
    TEST_ASSERT_EQ("(take -3 [0 1 2 3])", "[1 2 3]");
    TEST_ASSERT_EQ("(take 5 [0 1 2 3])", "[0 1 2 3 0]");
    TEST_ASSERT_EQ("(take -5 [0 1 2 3])", "[3 0 1 2 3]");

    TEST_ASSERT_EQ("(take 0h 'a)", "[]");
    TEST_ASSERT_EQ("(type (take 0i 'a))", "'Symbol");
    TEST_ASSERT_EQ("(take 2 'a)", "['a 'a]");
    TEST_ASSERT_EQ("(take 3 ['a 'b 'c 'd])", "['a 'b 'c]");
    TEST_ASSERT_EQ("(take -3 ['a 'b 'c 'd])", "['b 'c 'd]");
    TEST_ASSERT_EQ("(take 5 ['a 'b 'c 'd])", "['a 'b 'c 'd 'a]");
    TEST_ASSERT_EQ("(take -5 ['a 'b 'c 'd])", "['d 'a 'b 'c 'd]");

    TEST_ASSERT_EQ("(take 0h 2025.05.01D00:00:01.000000000)", "[]");
    TEST_ASSERT_EQ("(type (take 0i 2025.05.01D00:00:01.000000000))", "'Timestamp");
    TEST_ASSERT_EQ("(take 2 2025.05.01D00:00:01.000000000)",
                   "[2025.05.01D00:00:01.000000000 2025.05.01D00:00:01.000000000]");
    TEST_ASSERT_EQ(
        "(take 3 [2025.05.01D01:02:03.040000000 2025.05.02D01:02:03.040000000 2025.05.03D01:02:03.040000000 "
        "2025.05.04D01:02:03.040000000])",
        "[2025.05.01D01:02:03.040000000 2025.05.02D01:02:03.040000000 2025.05.03D01:02:03.040000000]");
    TEST_ASSERT_EQ(
        "(take -3 [2025.05.01D01:02:03.040000000 2025.05.02D01:02:03.040000000 2025.05.03D01:02:03.040000000 "
        "2025.05.04D01:02:03.040000000])",
        "[2025.05.02D01:02:03.040000000 2025.05.03D01:02:03.040000000 2025.05.04D01:02:03.040000000]");
    TEST_ASSERT_EQ(
        "(take 5 [2025.05.01D01:02:03.040000000 2025.05.02D01:02:03.040000000 2025.05.03D01:02:03.040000000 "
        "2025.05.04D01:02:03.040000000])",
        "[2025.05.01D01:02:03.040000000 2025.05.02D01:02:03.040000000 2025.05.03D01:02:03.040000000 "
        "2025.05.04D01:02:03.040000000 2025.05.01D01:02:03.040000000]");
    TEST_ASSERT_EQ(
        "(take -5 [2025.05.01D01:02:03.040000000 2025.05.02D01:02:03.040000000 2025.05.03D01:02:03.040000000 "
        "2025.05.04D01:02:03.040000000])",
        "[2025.05.04D01:02:03.040000000 2025.05.01D01:02:03.040000000 2025.05.02D01:02:03.040000000 "
        "2025.05.03D01:02:03.040000000 2025.05.04D01:02:03.040000000]");

    TEST_ASSERT_EQ("(take 0h 1.0)", "[]");
    TEST_ASSERT_EQ("(type (take 0i 1.0))", "'F64");
    TEST_ASSERT_EQ("(take 2 1.0)", "[1.0 1.0]");
    TEST_ASSERT_EQ("(take 3 [0.0 1.0 2.0 3.0])", "[0.0 1.0 2.0]");
    TEST_ASSERT_EQ("(take -3 [0.0 1.0 2.0 3.0])", "[1.0 2.0 3.0]");
    TEST_ASSERT_EQ("(take 5 [0.0 1.0 2.0 3.0])", "[0.0 1.0 2.0 3.0 0.0]");
    TEST_ASSERT_EQ("(take -5 [0.0 1.0 2.0 3.0])", "[3.0 0.0 1.0 2.0 3.0]");

    TEST_ASSERT_EQ("(take 0h (guid 1))", "[]");
    TEST_ASSERT_EQ("(type (take 0i (guid 1)))", "'Guid");
    TEST_ASSERT_EQ("(take 2 (as 'guid \"d49f18a4-1969-49e8-9b8a-6bb9a4832eea\"))",
                   "[d49f18a4-1969-49e8-9b8a-6bb9a4832eea d49f18a4-1969-49e8-9b8a-6bb9a4832eea]");
    TEST_ASSERT_EQ("(set l (guid 4)) (take 4 l)", "l");
    TEST_ASSERT_EQ("(set l (guid 4)) (take -4 l)", "l");
    TEST_ASSERT_EQ("(set l (guid 4)) (first (take 5 l))", "(first l)");
    TEST_ASSERT_EQ("(set l (guid 4)) (first (take -5 l))", "(last l)");

    TEST_ASSERT_EQ("(set sym ['a 'b 'c 'd 'e]) (take 0h (enum 'sym ['a 'b 'c 'd]))", "(enum 'sym (take 0 'a))");
    TEST_ASSERT_EQ("(set sym ['a 'b 'c 'd 'e]) (type (take 0i (enum 'sym ['a 'b 'c 'd])))", "'Enum");
    TEST_ASSERT_EQ("(set sym ['a 'b 'c 'd 'e]) (take 3 (enum 'sym ['a 'b 'c 'd]))", "(enum 'sym ['a 'b 'c])");
    TEST_ASSERT_EQ("(set sym ['a 'b 'c 'd 'e]) (take -3 (enum 'sym ['a 'b 'c 'd]))", "(enum 'sym ['b 'c 'd])");
    TEST_ASSERT_EQ("(set sym ['a 'b 'c 'd 'e]) (take 5 (enum 'sym ['a 'b 'c 'd]))", "(enum 'sym ['a 'b 'c 'd 'a])");
    TEST_ASSERT_EQ("(set sym ['a 'b 'c 'd 'e]) (take -5 (enum 'sym ['a 'b 'c 'd]))", "(enum 'sym ['d 'a 'b 'c 'd])");

    TEST_ASSERT_EQ("(take 0h (list 1h 2i 3 \"4\"))", "(list )");
    TEST_ASSERT_EQ("(type (take 0i (list 1h 2i 3 \"4\")))", "'List");
    TEST_ASSERT_EQ("(take 3 (list 1h 2i 3 \"4\"))", "(list 1h 2i 3)");
    TEST_ASSERT_EQ("(take -3 (list 1h 2i 3 \"4\"))", "(list 2i 3 \"4\")");
    TEST_ASSERT_EQ("(take 5 (list 1h 2i 3 \"4\"))", "(list 1h 2i 3 \"4\" 1h)");
    TEST_ASSERT_EQ("(take -5 (list 1h 2i 3 \"4\"))", "(list \"4\" 1h 2i 3 \"4\")");

    TEST_ASSERT_EQ("(take 0h (dict ['a 'b 'c 'd] [1 2 3 4]))", "{}");
    TEST_ASSERT_EQ("(type (take 0i (dict ['a 'b 'c 'd] [1 2 3 4])))", "'Dict");
    TEST_ASSERT_EQ("(take 3 (dict ['a 'b 'c 'd] [1 2 3 4]))", "(dict ['a 'b 'c] [1 2 3])");
    TEST_ASSERT_EQ("(take -3 (dict ['a 'b 'c 'd] [1 2 3 4]))", "(dict ['b 'c 'd] [2 3 4])");
    TEST_ASSERT_EQ("(take 5 (dict ['a 'b 'c 'd] [1 2 3 4]))", "(dict ['a 'b 'c 'd 'a] [1 2 3 4 1])");
    TEST_ASSERT_EQ("(take -5 (dict ['a 'b 'c 'd] [1 2 3 4]))", "(dict ['d 'a 'b 'c 'd] [4 1 2 3 4])");

    TEST_ASSERT_EQ("(take 0h (table [a b] (list [1 2 3 4] ['a 'b 'c 'd])))", "(table [a b] (list [] []))");
    TEST_ASSERT_EQ("(type (take 0i (table [a b] (list [1 2 3 4] ['a 'b 'c 'd]))))", "'Table");
    TEST_ASSERT_EQ("(take 3 (table [a b] (list [1 2 3 4] ['a 'b 'c 'd])))", "(table [a b] (list [1 2 3] ['a 'b 'c]))");
    TEST_ASSERT_EQ("(take -3 (table [a b] (list [1 2 3 4] ['a 'b 'c 'd])))", "(table [a b] (list [2 3 4] ['b 'c 'd]))");
    TEST_ASSERT_EQ("(take 5 (table [a b] (list [1 2 3 4] ['a 'b 'c 'd])))",
                   "(table [a b] (list [1 2 3 4 1] ['a 'b 'c 'd 'a]))");
    TEST_ASSERT_EQ("(take -5 (table [a b] (list [1 2 3 4] ['a 'b 'c 'd])))",
                   "(table [a b] (list [4 1 2 3 4] ['d 'a 'b 'c 'd]))");

    TEST_ASSERT_ER("(take 2.0 1.0)", "take: unsupported types: 'f64, f64");
    TEST_ASSERT_ER("(take 2 take)", "take: unsupported types: 'i64, Binary");

    PASS();
}

test_result_t test_lang_split() {
    // Test split function
    TEST_ASSERT_EQ("(split \"hello,world\" \",\")", "(list \"hello\" \"world\")");
    TEST_ASSERT_EQ("(split \"a,b,c\" \",\")", "(list \"a\" \"b\" \"c\")");
    TEST_ASSERT_EQ("(split \"hello\" \",\")", "(list \"hello\")");
    TEST_ASSERT_EQ("(split \"\" \",\")", "(list \"\")");
    TEST_ASSERT_EQ("(split \",\" \",\")", "(list \"\" \"\")");
    TEST_ASSERT_EQ("(split \",a,\" \",\")", "(list \"\" \"a\" \"\")");

    // Test multi-character delimiters
    TEST_ASSERT_EQ("(split \"hello--world\" \"--\")", "(list \"hello\" \"world\")");
    TEST_ASSERT_EQ("(split \"a--b--c\" \"--\")", "(list \"a\" \"b\" \"c\")");
    TEST_ASSERT_EQ("(split \"hello\" \"--\")", "(list \"hello\")");
    TEST_ASSERT_EQ("(split \"\" \"--\")", "(list \"\")");
    TEST_ASSERT_EQ("(split \"--\" \"--\")", "(list \"\" \"\")");
    TEST_ASSERT_EQ("(split \"--a--\" \"--\")", "(list \"\" \"a\" \"\")");
    TEST_ASSERT_EQ("(split 'asasd \"d\")", "(list \"asas\" \"\")");
    TEST_ASSERT_EQ("(split 'asasd 'd')", "(list \"asas\" \"\")");

    // Basic vector splitting with I64 indices
    TEST_ASSERT_EQ("(split [1 2 3 4 5] [0 2 4])", "(list [1 2] [3 4] [5])");
    TEST_ASSERT_EQ("(split [1 2 3 4 5] [0 3])", "(list [1 2 3] [4 5])");

    // Different vector types with I64 indices
    TEST_ASSERT_EQ("(split [1i 2i 3i 4i 5i] [0 2 4])", "(list [1i 2i] [3i 4i] [5i])");
    TEST_ASSERT_EQ("(split [1.0 2.0 3.0 4.0 5.0] [0 2 4])", "(list [1.0 2.0] [3.0 4.0] [5.0])");
    TEST_ASSERT_EQ("(split [true false true false true] [0 2 4])", "(list [true false] [true false] [true])");
    TEST_ASSERT_EQ("(split \"hello\" [0 2 4])", "(list \"he\" \"ll\" \"o\")");

    // Different index types (I16, I32, I64)
    TEST_ASSERT_EQ("(split [1 2 3 4 5] [0h 2h 4h])", "(list [1 2] [3 4] [5])");
    TEST_ASSERT_EQ("(split [1 2 3 4 5] [0h 2h 4h])", "(list [1 2] [3 4] [5])");
    TEST_ASSERT_EQ("(split [1 2 3 4 5] [0h 2h 4h])", "(list [1 2] [3 4] [5])");

    // Error cases for vector splitting
    // TEST_ASSERT_ER("(split [1 2 3] [0 4])", "cut: invalid index or index vector is not sorted: 4");
    // TEST_ASSERT_ER("(split [1 2 3] [2 1])", "cut: invalid index or index vector is not sorted: 1");
    // TEST_ASSERT_ER("(split [1 2 3] [0 2 3])", "cut: invalid index or index vector is not sorted: 3");

    // Empty vectors and indices
    TEST_ASSERT_EQ(
        "(split (as 'Guid (list \"7e42b82b-51c7-4468-a477-375bd5006e60\" \"7e42b82b-51c7-4468-a477-375bd5006e60\")) [0 "
        "1])",
        "(list (as 'Guid (list \"7e42b82b-51c7-4468-a477-375bd5006e60\")) (as 'Guid (list "
        "\"7e42b82b-51c7-4468-a477-375bd5006e60\")))");

    TEST_ASSERT_EQ("(split (list 1 2 3 4 5 6 \"asdf\" 9.33) [0 2 4])", "(list [1 2] [3 4] (list 5 6 \"asdf\" 9.33))");

    // Empty vectors and indices
    TEST_ASSERT_EQ("(split [] [])", "null");
    TEST_ASSERT_EQ("(split [1 2 3] [])", "null");

    PASS();
}

test_result_t test_lang_query() {
    TEST_ASSERT_EQ(
        "(set t (table [sym price volume tape] (list [apl vod god] [102 99 203] [500 400 900] (list "
        "\"A\"\"B\"\"C\"))))",
        "(table [sym price volume tape] (list [apl vod god] [102 99 203] [500 400 900] (list \"A\"\"B\"\"C\")))");
    TEST_ASSERT_EQ("(at t 'sym)", "[apl vod god]");
    TEST_ASSERT_EQ("(at t 'price)", "[102 99 203]");
    TEST_ASSERT_EQ("(at t 'volume)", "[500 400 900]");
    TEST_ASSERT_EQ("(at t 'tape)", "(list \"A\"\"B\"\"C\")");
    TEST_ASSERT_EQ(
        "(set n 10)"
        "(set gds (take n (guid 3)))"
        "(set t (table [OrderId Symbol Price Size Tape Timestamp]"
        "(list gds"
        "(take n [apll good msfk ibmd amznt fbad baba])"
        "(as 'F64 (til n))"
        "(take n (+ 1 (til 3)))"
        "(map (fn [x] (as 'String x)) (take n (til 10)))"
        "(as 'Timestamp (til n)))))"
        "null",
        "null");
    TEST_ASSERT_EQ("(select {from: t by: Symbol})",
                   "(table [Symbol OrderId Price Size Tape Timestamp]"
                   "(list [apll good msfk ibmd amznt fbad baba]"
                   "(at gds (til 7)) [0 1 2 3 4 5 6.0] [1 2 3 1 2 3 1]"
                   "(list \"0\"\"1\"\"2\"\"3\"\"4\"\"5\"\"6\")"
                   "(at (at t 'Timestamp) (til 7))))");
    TEST_ASSERT_EQ("(select {from: t by: Symbol where: (== Price 3)})",
                   "(table [Symbol OrderId Price Size Tape Timestamp]"
                   "(list [ibmd] (at gds 3) [3.00] [1] (list \"3\") [2000.01.01D00:00:00.000000003]))");
    TEST_ASSERT_EQ("(select {from: t by: Symbol where: (== Price 99)})",
                   "(table [Symbol OrderId Price Size Tape Timestamp]"
                   "(list [] [] [] [] (list) []))");
    TEST_ASSERT_EQ("(select {s: (sum Price) from: t by: Symbol})",
                   "(table [Symbol s]"
                   "(list [apll good msfk ibmd amznt fbad baba]"
                   "[7.00 9.00 11.00 3.00 4.00 5.00 6.00]))");

    // Test and with select - this exposes the parallel processing bug
    TEST_ASSERT_EQ(
        "(set t (table ['a 'b 'c] (list (take 25001 [false true]) (take 25001 [true false]) (take 25001 1)))) (count "
        "(select {c: c from: t where: (and a b)}))",
        "0");

    PASS();
}

test_result_t test_lang_update() {
    // ========== INSERT TESTS ==========

    // Test 1: Insert single record via list of atoms (immediate - returns new table)
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1 2 3] [alice bob charlie] [10.0 20.0 30.0])))"
        "(insert t (list 4 'david 40.0))",
        "(table [ID Name Value] (list [1 2 3 4] [alice bob charlie david] [10.0 20.0 30.0 40.0]))");

    // Verify original table unchanged (immediate mode)
    TEST_ASSERT_EQ("t", "(table [ID Name Value] (list [1 2 3] [alice bob charlie] [10.0 20.0 30.0]))");

    // Test 2: Insert single record in-place (modifying variable)
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1 2] [alice bob] [10.0 20.0])))"
        "(insert 't (list 3 'charlie 30.0))"
        "t",
        "(table [ID Name Value] (list [1 2 3] [alice bob charlie] [10.0 20.0 30.0]))");

    // Test 3: Insert multiple records via list of vectors (immediate)
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1] [alice] [10.0])))"
        "(insert t (list [2 3] [bob charlie] [20.0 30.0]))",
        "(table [ID Name Value] (list [1 2 3] [alice bob charlie] [10.0 20.0 30.0]))");

    // Test 4: Insert multiple records via list of vectors in-place
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1] [alice] [10.0])))"
        "(insert 't (list [2 3] [bob charlie] [20.0 30.0]))"
        "t",
        "(table [ID Name Value] (list [1 2 3] [alice bob charlie] [10.0 20.0 30.0]))");

    // Test 5: Insert via dict (single record, immediate)
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1 2] [alice bob] [10.0 20.0])))"
        "(insert t (dict [ID Name Value] (list 3 'charlie 30.0)))",
        "(table [ID Name Value] (list [1 2 3] [alice bob charlie] [10.0 20.0 30.0]))");

    // Test 6: Insert via dict in-place
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1 2] [alice bob] [10.0 20.0])))"
        "(insert 't (dict [ID Name Value] (list 3 'charlie 30.0)))"
        "t",
        "(table [ID Name Value] (list [1 2 3] [alice bob charlie] [10.0 20.0 30.0]))");

    // Test 7: Insert via table (immediate)
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1] [alice] [10.0])))"
        "(insert t (table [ID Name Value] (list [2 3] [bob charlie] [20.0 30.0])))",
        "(table [ID Name Value] (list [1 2 3] [alice bob charlie] [10.0 20.0 30.0]))");

    // Test 8: Insert via table in-place
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1] [alice] [10.0])))"
        "(insert 't (table [ID Name Value] (list [2 3] [bob charlie] [20.0 30.0])))"
        "t",
        "(table [ID Name Value] (list [1 2 3] [alice bob charlie] [10.0 20.0 30.0]))");

    // ========== UPSERT TESTS ==========

    // Test 9: Upsert single record (new) via list of atoms (immediate)
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1 2] [alice bob] [10.0 20.0])))"
        "(upsert t 1 (list 3 'charlie 30.0))",
        "(table [ID Name Value] (list [1 2 3] [alice bob charlie] [10.0 20.0 30.0]))");

    // Verify original table unchanged (immediate mode)
    TEST_ASSERT_EQ("t", "(table [ID Name Value] (list [1 2] [alice bob] [10.0 20.0]))");

    // Test 10: Upsert single record (update existing) via list of atoms (immediate)
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1 2 3] [alice bob charlie] [10.0 20.0 30.0])))"
        "(upsert t 1 (list 2 'bobby 25.0))",
        "(table [ID Name Value] (list [1 2 3] [alice bobby charlie] [10.0 25.0 30.0]))");

    // Test 11: Upsert single record in-place (new record)
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1 2] [alice bob] [10.0 20.0])))"
        "(upsert 't 1 (list 3 'charlie 30.0))"
        "t",
        "(table [ID Name Value] (list [1 2 3] [alice bob charlie] [10.0 20.0 30.0]))");

    // Test 12: Upsert single record in-place (update existing)
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1 2 3] [alice bob charlie] [10.0 20.0 30.0])))"
        "(upsert 't 1 (list 2 'bobby 25.0))"
        "t",
        "(table [ID Name Value] (list [1 2 3] [alice bobby charlie] [10.0 25.0 30.0]))");

    // Test 13: Upsert multiple records via list of vectors (immediate)
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1 2] [alice bob] [10.0 20.0])))"
        "(upsert t 1 (list [3 4] [charlie david] [30.0 40.0]))",
        "(table [ID Name Value] (list [1 2 3 4] [alice bob charlie david] [10.0 20.0 30.0 40.0]))");

    // Test 14: Upsert multiple records with mixed insert/update (immediate)
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1 2] [alice bob] [10.0 20.0])))"
        "(upsert t 1 (list [2 3] [bobby charlie] [25.0 30.0]))",
        "(table [ID Name Value] (list [1 2 3] [alice bobby charlie] [10.0 25.0 30.0]))");

    // Test 15: Upsert multiple records in-place
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1 2] [alice bob] [10.0 20.0])))"
        "(upsert 't 1 (list [2 3] [bobby charlie] [25.0 30.0]))"
        "t",
        "(table [ID Name Value] (list [1 2 3] [alice bobby charlie] [10.0 25.0 30.0]))");

    // Test 16: Upsert via dict (immediate, new record)
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1 2] [alice bob] [10.0 20.0])))"
        "(upsert t 1 (dict [ID Name Value] (list 3 'charlie 30.0)))",
        "(table [ID Name Value] (list [1 2 3] [alice bob charlie] [10.0 20.0 30.0]))");

    // Test 17: Upsert via dict (immediate, update existing)
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1 2 3] [alice bob charlie] [10.0 20.0 30.0])))"
        "(upsert t 1 (dict [ID Name Value] (list 2 'bobby 25.0)))",
        "(table [ID Name Value] (list [1 2 3] [alice bobby charlie] [10.0 25.0 30.0]))");

    // Test 18: Upsert via dict in-place
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1 2] [alice bob] [10.0 20.0])))"
        "(upsert 't 1 (dict [ID Name Value] (list 3 'charlie 30.0)))"
        "t",
        "(table [ID Name Value] (list [1 2 3] [alice bob charlie] [10.0 20.0 30.0]))");

    // Test 19: Upsert via table (immediate)
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1 2] [alice bob] [10.0 20.0])))"
        "(upsert t 1 (table [ID Name Value] (list [3 4] [charlie david] [30.0 40.0])))",
        "(table [ID Name Value] (list [1 2 3 4] [alice bob charlie david] [10.0 20.0 30.0 40.0]))");

    // Test 20: Upsert via table in-place
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1 2] [alice bob] [10.0 20.0])))"
        "(upsert 't 1 (table [ID Name Value] (list [2 3] [bobby charlie] [25.0 30.0])))"
        "t",
        "(table [ID Name Value] (list [1 2 3] [alice bobby charlie] [10.0 25.0 30.0]))");

    // Test 21: Upsert with composite key (2 key columns)
    TEST_ASSERT_EQ(
        "(set t (table [K1 K2 Value] (list [1 1 2] [1 2 1] [10.0 20.0 30.0])))"
        "(upsert t 2 (list [1 3] [2 1] [25.0 40.0]))",
        "(table [K1 K2 Value] (list [1 1 2 3] [1 2 1 1] [10.0 25.0 30.0 40.0]))");

    // ========== UPDATE TESTS ==========

    // Test 22: Basic update with no filter
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1 2 3] [alice bob charlie] [10.0 20.0 30.0])))"
        "(update {from: 't NewCol: 100})"
        "t",
        "(table [ID Name Value NewCol] (list [1 2 3] [alice bob charlie] [10.0 20.0 30.0] [100 100 100]))");

    // Test 23: Update with filter
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1 2 3] [alice bob charlie] [10.0 20.0 30.0])))"
        "(update {from: 't Value: 99.0 where: (== ID 2)})"
        "t",
        "(table [ID Name Value] (list [1 2 3] [alice bob charlie] [10.0 99.0 30.0]))");

    // Test 24: Update with groupby
    TEST_ASSERT_EQ(
        "(set t (table [Group ID Value] (list [a a b b] [1 2 3 4] [10.0 20.0 30.0 40.0])))"
        "(update {from: 't Total: (sum Value) by: Group})"
        "t",
        "(table [Group ID Value Total] (list [a a b b] [1 2 3 4] [10.0 20.0 30.0 40.0] [30.0 30.0 70.0 70.0]))");

    // ========== FLEXIBLE COLUMN ORDERING TESTS ==========

    // Test 25: Insert via dict with columns in different order (single record)
    TEST_ASSERT_EQ(
        "(set t (table [a b c] (list [1 2] [10 20] [x y])))"
        "(insert t (dict [c a b] (list 'z 3 30)))",
        "(table [a b c] (list [1 2 3] [10 20 30] [x y z]))");

    // Test 26: Insert via dict with columns in reversed order (in-place)
    TEST_ASSERT_EQ(
        "(set t (table [a b c] (list [1 2] [10 20] [x y])))"
        "(insert 't (dict [c b a] (list 'z 30 3)))"
        "t",
        "(table [a b c] (list [1 2 3] [10 20 30] [x y z]))");

    // Test 27: Insert via dict with partial columns (missing column gets null)
    TEST_ASSERT_EQ(
        "(set t (table [a b c] (list [1 2] [10 20] [x y])))"
        "(insert t (dict [c b] (list 'z 30)))",
        "(table [a b c] (list [1 2 0Nl] [10 20 30] [x y z]))");

    // Test 28: Insert via dict with partial columns in different order
    TEST_ASSERT_EQ(
        "(set t (table [a b c] (list [1 2] [10 20] [x y])))"
        "(insert t (dict [b a] (list 30 3)))",
        "(table [a b c] (list [1 2 3] [10 20 30] [x y 0Ns]))");

    // Test 29: Insert multiple records via dict with different column order
    TEST_ASSERT_EQ(
        "(set t (table [a b c] (list [1] [10] [x])))"
        "(insert t (dict [c b a] (list [y z] [20 30] [2 3])))",
        "(table [a b c] (list [1 2 3] [10 20 30] [x y z]))");

    // Test 30: Insert via table with columns in different order
    TEST_ASSERT_EQ(
        "(set t (table [a b c] (list [1] [10] [x])))"
        "(insert t (table [c a b] (list [y z] [2 3] [20 30])))",
        "(table [a b c] (list [1 2 3] [10 20 30] [x y z]))");

    // Test 31: Upsert via dict with different column order (new record)
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1 2] [alice bob] [10.0 20.0])))"
        "(upsert t 1 (dict [Value Name ID] (list 30.0 'charlie 3)))",
        "(table [ID Name Value] (list [1 2 3] [alice bob charlie] [10.0 20.0 30.0]))");

    // Test 32: Upsert via dict with different column order (update existing)
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1 2 3] [alice bob charlie] [10.0 20.0 30.0])))"
        "(upsert t 1 (dict [Value ID Name] (list 25.0 2 'bobby)))",
        "(table [ID Name Value] (list [1 2 3] [alice bobby charlie] [10.0 25.0 30.0]))");

    // Test 33: Upsert via dict with different column order (in-place)
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1 2] [alice bob] [10.0 20.0])))"
        "(upsert 't 1 (dict [Name Value ID] (list 'charlie 30.0 3)))"
        "t",
        "(table [ID Name Value] (list [1 2 3] [alice bob charlie] [10.0 20.0 30.0]))");

    // Test 34: Upsert multiple records via dict with different column order
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1 2] [alice bob] [10.0 20.0])))"
        "(upsert t 1 (dict [Value ID Name] (list [25.0 30.0] [2 3] [bobby charlie])))",
        "(table [ID Name Value] (list [1 2 3] [alice bobby charlie] [10.0 25.0 30.0]))");

    // Test 35: Upsert via table with different column order
    TEST_ASSERT_EQ(
        "(set t (table [ID Name Value] (list [1 2] [alice bob] [10.0 20.0])))"
        "(upsert t 1 (table [Name ID Value] (list [charlie david] [3 4] [30.0 40.0])))",
        "(table [ID Name Value] (list [1 2 3 4] [alice bob charlie david] [10.0 20.0 30.0 40.0]))");

    // Test 36: Insert error - column not found in table
    TEST_ASSERT_ER(
        "(set t (table [a b c] (list [1] [10] [x])))"
        "(insert t (dict [a b d] (list 2 20 'w)))",
        "not found in table");

    // Test 37: Upsert error - column not found in table
    TEST_ASSERT_ER(
        "(set t (table [ID Name Value] (list [1] [alice] [10.0])))"
        "(upsert t 1 (dict [ID Unknown Value] (list 2 'x 20.0)))",
        "not found in table");

    PASS();
}

test_result_t test_lang_serde() {
    TEST_ASSERT_EQ("(de (ser null))", "null");

    PASS();
}

test_result_t test_lang_literals() {
    // Test basic character literals
    TEST_ASSERT_EQ("'a'", "'a'");
    TEST_ASSERT_EQ("'z'", "'z'");
    TEST_ASSERT_EQ("'0'", "'0'");
    TEST_ASSERT_EQ("'9'", "'9'");

    // Test standard escape sequences in character literals
    TEST_ASSERT_EQ("'\\n'", "'\\n'");
    TEST_ASSERT_EQ("'\\r'", "'\\r'");
    TEST_ASSERT_EQ("'\\t'", "'\\t'");
    TEST_ASSERT_EQ("'\\\\'", "'\\\\'");
    TEST_ASSERT_EQ("'\\''", "'\\''");

    // Test octal escape sequences in character literals
    TEST_ASSERT_EQ("'\\001'", "'\\001'");  // SOH (Start of Heading)
    TEST_ASSERT_EQ("'\\002'", "'\\002'");  // STX (Start of Text)
    TEST_ASSERT_EQ("'\\003'", "'\\003'");  // ETX (End of Text)
    TEST_ASSERT_EQ("'\\007'", "'\\007'");  // BEL (Bell)
    TEST_ASSERT_EQ("'\\010'", "'\\010'");  // BS (Backspace)
    TEST_ASSERT_EQ("'\\011'", "'\\011'");  // HT (Horizontal Tab) - same as '\t'
    TEST_ASSERT_EQ("'\\012'", "'\\012'");  // LF (Line Feed) - same as '\n'
    TEST_ASSERT_EQ("'\\015'", "'\\015'");  // CR (Carriage Return) - same as '\r'
    TEST_ASSERT_EQ("'\\032'", "'\\032'");  // SUB (Substitute)

    // Test empty quoted symbol (single quote)
    TEST_ASSERT_EQ("'", "0Ns");  // Empty quoted symbol should be parsed as a null symbol

    // Test basic string literals
    TEST_ASSERT_EQ("\"Hello, World!\"", "\"Hello, World!\"");
    TEST_ASSERT_EQ("\"\"", "\"\"");  // Empty string
    TEST_ASSERT_EQ("\"123\"", "\"123\"");

    // Test standard escape sequences in string literals
    TEST_ASSERT_EQ("\"Hello\\nWorld\"", "\"Hello\\nWorld\"");
    TEST_ASSERT_EQ("\"Hello\\rWorld\"", "\"Hello\\rWorld\"");
    TEST_ASSERT_EQ("\"Hello\\tWorld\"", "\"Hello\\tWorld\"");
    TEST_ASSERT_EQ("\"Hello\\\\World\"", "\"Hello\\\\World\"");
    TEST_ASSERT_EQ("\"Hello\\\"World\"", "\"Hello\\\"World\"");

    // Test octal escape sequences in string literals
    TEST_ASSERT_EQ("\"Hello\\001World\"", "\"Hello\\001World\"");  // SOH
    TEST_ASSERT_EQ("\"Hello\\002World\"", "\"Hello\\002World\"");  // STX
    TEST_ASSERT_EQ("\"Hello\\003World\"", "\"Hello\\003World\"");  // ETX
    TEST_ASSERT_EQ("\"Hello\\007World\"", "\"Hello\\007World\"");  // BEL
    TEST_ASSERT_EQ("\"Hello\\010World\"", "\"Hello\\010World\"");  // BS
    TEST_ASSERT_EQ("\"Hello\\011World\"", "\"Hello\\011World\"");  // HT
    TEST_ASSERT_EQ("\"Hello\\012World\"", "\"Hello\\012World\"");  // LF
    TEST_ASSERT_EQ("\"Hello\\015World\"", "\"Hello\\015World\"");  // CR

    // Test FIX protocol message with octal escapes
    TEST_ASSERT_EQ("\"8=FIX.4.2\\0019=006035=A49=CL156=TR34=152=20\"",
                   "\"8=FIX.4.2\\0019=006035=A49=CL156=TR34=152=20\"");

    // Test mixed escape sequences
    TEST_ASSERT_EQ("\"Mixed\\001\\n\\t\\015Escapes\"", "\"Mixed\\001\\n\\t\\015Escapes\"");

    PASS();
}

test_result_t test_lang_cmp() {
    // Test character-to-string comparisons
    TEST_ASSERT_EQ("(== 'a' \"a\")", "true");
    TEST_ASSERT_EQ("(== 'a' \"b\")", "false");
    TEST_ASSERT_EQ("(== 'a' \"ab\")", "false");
    TEST_ASSERT_EQ("(== \"a\" 'a')", "true");
    TEST_ASSERT_EQ("(== \"ab\" 'a')", "false");
    TEST_ASSERT_EQ("(!= 'a' \"b\")", "true");
    TEST_ASSERT_EQ("(!= 'a' \"a\")", "false");
    TEST_ASSERT_EQ("(!= \"a\" 'b')", "true");
    TEST_ASSERT_EQ("(!= \"a\" 'a')", "false");
    TEST_ASSERT_EQ("(< 'a' \"b\")", "true");
    TEST_ASSERT_EQ("(< 'b' \"a\")", "false");
    TEST_ASSERT_EQ("(< \"a\" 'b')", "true");
    TEST_ASSERT_EQ("(< \"b\" 'a')", "false");
    TEST_ASSERT_EQ("(> 'b' \"a\")", "true");
    TEST_ASSERT_EQ("(> 'a' \"b\")", "false");
    TEST_ASSERT_EQ("(> \"b\" 'a')", "true");
    TEST_ASSERT_EQ("(> \"a\" 'b')", "false");
    TEST_ASSERT_EQ("(<= 'a' \"a\")", "true");
    TEST_ASSERT_EQ("(<= 'a' \"b\")", "true");
    TEST_ASSERT_EQ("(<= 'b' \"a\")", "false");
    TEST_ASSERT_EQ("(<= \"a\" 'a')", "true");
    TEST_ASSERT_EQ("(<= \"a\" 'b')", "true");
    TEST_ASSERT_EQ("(<= \"b\" 'a')", "false");
    TEST_ASSERT_EQ("(>= 'a' \"a\")", "true");
    TEST_ASSERT_EQ("(>= 'b' \"a\")", "true");
    TEST_ASSERT_EQ("(>= 'a' \"b\")", "false");
    TEST_ASSERT_EQ("(>= \"a\" 'a')", "true");
    TEST_ASSERT_EQ("(>= \"b\" 'a')", "true");
    TEST_ASSERT_EQ("(>= \"a\" 'b')", "false");

    // Test character-to-character comparisons
    TEST_ASSERT_EQ("(== 'a' 'a')", "true");
    TEST_ASSERT_EQ("(== 'a' 'b')", "false");
    TEST_ASSERT_EQ("(!= 'a' 'b')", "true");
    TEST_ASSERT_EQ("(!= 'a' 'a')", "false");
    TEST_ASSERT_EQ("(< 'a' 'b')", "true");
    TEST_ASSERT_EQ("(< 'b' 'a')", "false");
    TEST_ASSERT_EQ("(> 'b' 'a')", "true");
    TEST_ASSERT_EQ("(> 'a' 'b')", "false");
    TEST_ASSERT_EQ("(<= 'a' 'a')", "true");
    TEST_ASSERT_EQ("(<= 'a' 'b')", "true");
    TEST_ASSERT_EQ("(<= 'b' 'a')", "false");
    TEST_ASSERT_EQ("(>= 'a' 'a')", "true");
    TEST_ASSERT_EQ("(>= 'b' 'a')", "true");
    TEST_ASSERT_EQ("(>= 'a' 'b')", "false");

    // Test string-to-string comparisons
    TEST_ASSERT_EQ("(== \"a\" \"a\")", "true");
    TEST_ASSERT_EQ("(== \"a\" \"b\")", "false");
    TEST_ASSERT_EQ("(== \"ab\" \"ab\")", "true");
    TEST_ASSERT_EQ("(== \"ab\" \"ac\")", "false");
    TEST_ASSERT_EQ("(!= \"a\" \"b\")", "true");
    TEST_ASSERT_EQ("(!= \"a\" \"a\")", "false");
    TEST_ASSERT_EQ("(< \"a\" \"b\")", "true");
    TEST_ASSERT_EQ("(< \"b\" \"a\")", "false");
    TEST_ASSERT_EQ("(> \"b\" \"a\")", "true");
    TEST_ASSERT_EQ("(> \"a\" \"b\")", "false");
    TEST_ASSERT_EQ("(<= \"a\" \"a\")", "true");
    TEST_ASSERT_EQ("(<= \"a\" \"b\")", "true");
    TEST_ASSERT_EQ("(<= \"b\" \"a\")", "false");
    TEST_ASSERT_EQ("(>= \"a\" \"a\")", "true");
    TEST_ASSERT_EQ("(>= \"b\" \"a\")", "true");
    TEST_ASSERT_EQ("(>= \"a\" \"b\")", "false");

    TEST_ASSERT_EQ(
        "(set l (list -2i 0i 0Ni 1i 2i -2 0 0Nl 1 2 -2.0 -0.0 0Nf 0.0 1.0  2.0)) (set f (fn [x y] (if (== x y) 1 0))) "
        "(map (fn[x] (map f x l)) l)",
        "(list [1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 0]"
        "[0 1 0 0 0 0 1 0 0 0 0 1 0 1 0 0]"
        "[0 0 1 0 0 0 0 1 0 0 0 0 1 0 0 0]"
        "[0 0 0 1 0 0 0 0 1 0 0 0 0 0 1 0]"
        "[0 0 0 0 1 0 0 0 0 1 0 0 0 0 0 1]"
        "[1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 0]"
        "[0 1 0 0 0 0 1 0 0 0 0 1 0 1 0 0]"
        "[0 0 1 0 0 0 0 1 0 0 0 0 1 0 0 0]"
        "[0 0 0 1 0 0 0 0 1 0 0 0 0 0 1 0]"
        "[0 0 0 0 1 0 0 0 0 1 0 0 0 0 0 1]"
        "[1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 0]"
        "[0 1 0 0 0 0 1 0 0 0 0 1 0 1 0 0]"
        "[0 0 1 0 0 0 0 1 0 0 0 0 1 0 0 0]"
        "[0 1 0 0 0 0 1 0 0 0 0 1 0 1 0 0]"
        "[0 0 0 1 0 0 0 0 1 0 0 0 0 0 1 0]"
        "[0 0 0 0 1 0 0 0 0 1 0 0 0 0 0 1])")
    TEST_ASSERT_EQ(
        "(set l (list -2i 0i 0Ni 1i 2i -2 0 0Nl 1 2 -2.0 -0.0 0Nf 0.0 1.0  2.0)) (set f (fn [x y] (if (< x y) 1 0))) "
        "(map (fn[x] (map f x l)) l)",
        "(list [0 1 0 1 1 0 1 0 1 1 0 1 0 1 1 1]"
        "[0 0 0 1 1 0 0 0 1 1 0 0 0 0 1 1]"
        "[1 1 0 1 1 1 1 0 1 1 1 1 0 1 1 1]"
        "[0 0 0 0 1 0 0 0 0 1 0 0 0 0 0 1]"
        "[0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0]"
        "[0 1 0 1 1 0 1 0 1 1 0 1 0 1 1 1]"
        "[0 0 0 1 1 0 0 0 1 1 0 0 0 0 1 1]"
        "[1 1 0 1 1 1 1 0 1 1 1 1 0 1 1 1]"
        "[0 0 0 0 1 0 0 0 0 1 0 0 0 0 0 1]"
        "[0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0]"
        "[0 1 0 1 1 0 1 0 1 1 0 1 0 1 1 1]"
        "[0 0 0 1 1 0 0 0 1 1 0 0 0 0 1 1]"
        "[1 1 0 1 1 1 1 0 1 1 1 1 0 1 1 1]"
        "[0 0 0 1 1 0 0 0 1 1 0 0 0 0 1 1]"
        "[0 0 0 0 1 0 0 0 0 1 0 0 0 0 0 1]"
        "[0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0])")
    TEST_ASSERT_EQ(
        "(set l (list -2i 0i 0Ni 1i 2i -2 0 0Nl 1 2 -2.0 -0.0 0Nf 0.0 1.0  2.0)) (set f (fn [x y] (if (> x y) 1 0))) "
        "(map (fn[x] (map f x l)) l)",
        "(list [0 0 1 0 0 0 0 1 0 0 0 0 1 0 0 0]"
        "[1 0 1 0 0 1 0 1 0 0 1 0 1 0 0 0]"
        "[0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0]"
        "[1 1 1 0 0 1 1 1 0 0 1 1 1 1 0 0]"
        "[1 1 1 1 0 1 1 1 1 0 1 1 1 1 1 0]"
        "[0 0 1 0 0 0 0 1 0 0 0 0 1 0 0 0]"
        "[1 0 1 0 0 1 0 1 0 0 1 0 1 0 0 0]"
        "[0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0]"
        "[1 1 1 0 0 1 1 1 0 0 1 1 1 1 0 0]"
        "[1 1 1 1 0 1 1 1 1 0 1 1 1 1 1 0]"
        "[0 0 1 0 0 0 0 1 0 0 0 0 1 0 0 0]"
        "[1 0 1 0 0 1 0 1 0 0 1 0 1 0 0 0]"
        "[0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0]"
        "[1 0 1 0 0 1 0 1 0 0 1 0 1 0 0 0]"
        "[1 1 1 0 0 1 1 1 0 0 1 1 1 1 0 0]"
        "[1 1 1 1 0 1 1 1 1 0 1 1 1 1 1 0])")
    TEST_ASSERT_EQ(
        "(set l (list -2i 0i 0Ni 1i 2i -2 0 0Nl 1 2 -2.0 -0.0 0Nf 0.0 1.0  2.0)) (set f (fn [x y] (sum (as 'I32 (== "
        "(enlist x) y))))) (map (fn[x] (map f x l)) l)",
        "(list [1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 0]"
        "[0 1 0 0 0 0 1 0 0 0 0 1 0 1 0 0]"
        "[0 0 1 0 0 0 0 1 0 0 0 0 1 0 0 0]"
        "[0 0 0 1 0 0 0 0 1 0 0 0 0 0 1 0]"
        "[0 0 0 0 1 0 0 0 0 1 0 0 0 0 0 1]"
        "[1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 0]"
        "[0 1 0 0 0 0 1 0 0 0 0 1 0 1 0 0]"
        "[0 0 1 0 0 0 0 1 0 0 0 0 1 0 0 0]"
        "[0 0 0 1 0 0 0 0 1 0 0 0 0 0 1 0]"
        "[0 0 0 0 1 0 0 0 0 1 0 0 0 0 0 1]"
        "[1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 0]"
        "[0 1 0 0 0 0 1 0 0 0 0 1 0 1 0 0]"
        "[0 0 1 0 0 0 0 1 0 0 0 0 1 0 0 0]"
        "[0 1 0 0 0 0 1 0 0 0 0 1 0 1 0 0]"
        "[0 0 0 1 0 0 0 0 1 0 0 0 0 0 1 0]"
        "[0 0 0 0 1 0 0 0 0 1 0 0 0 0 0 1])")
    TEST_ASSERT_EQ(
        "(set l (list -2i 0i 0Ni 1i 2i -2 0 0Nl 1 2 -2.0 -0.0 0Nf 0.0 1.0  2.0)) (set f (fn [x y] (sum (as 'I32 (< "
        "(enlist x) y))))) (map (fn[x] (map f x l)) l)",
        "(list [0 1 0 1 1 0 1 0 1 1 0 1 0 1 1 1]"
        "[0 0 0 1 1 0 0 0 1 1 0 0 0 0 1 1]"
        "[1 1 0 1 1 1 1 0 1 1 1 1 0 1 1 1]"
        "[0 0 0 0 1 0 0 0 0 1 0 0 0 0 0 1]"
        "[0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0]"
        "[0 1 0 1 1 0 1 0 1 1 0 1 0 1 1 1]"
        "[0 0 0 1 1 0 0 0 1 1 0 0 0 0 1 1]"
        "[1 1 0 1 1 1 1 0 1 1 1 1 0 1 1 1]"
        "[0 0 0 0 1 0 0 0 0 1 0 0 0 0 0 1]"
        "[0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0]"
        "[0 1 0 1 1 0 1 0 1 1 0 1 0 1 1 1]"
        "[0 0 0 1 1 0 0 0 1 1 0 0 0 0 1 1]"
        "[1 1 0 1 1 1 1 0 1 1 1 1 0 1 1 1]"
        "[0 0 0 1 1 0 0 0 1 1 0 0 0 0 1 1]"
        "[0 0 0 0 1 0 0 0 0 1 0 0 0 0 0 1]"
        "[0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0])")
    TEST_ASSERT_EQ(
        "(set l (list -2i 0i 0Ni 1i 2i -2 0 0Nl 1 2 -2.0 -0.0 0Nf 0.0 1.0  2.0)) (set f (fn [x y] (sum (as 'I32 (> "
        "(enlist x) y))))) (map (fn[x] (map f x l)) l)",
        "(list [0 0 1 0 0 0 0 1 0 0 0 0 1 0 0 0]"
        "[1 0 1 0 0 1 0 1 0 0 1 0 1 0 0 0]"
        "[0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0]"
        "[1 1 1 0 0 1 1 1 0 0 1 1 1 1 0 0]"
        "[1 1 1 1 0 1 1 1 1 0 1 1 1 1 1 0]"
        "[0 0 1 0 0 0 0 1 0 0 0 0 1 0 0 0]"
        "[1 0 1 0 0 1 0 1 0 0 1 0 1 0 0 0]"
        "[0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0]"
        "[1 1 1 0 0 1 1 1 0 0 1 1 1 1 0 0]"
        "[1 1 1 1 0 1 1 1 1 0 1 1 1 1 1 0]"
        "[0 0 1 0 0 0 0 1 0 0 0 0 1 0 0 0]"
        "[1 0 1 0 0 1 0 1 0 0 1 0 1 0 0 0]"
        "[0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0]"
        "[1 0 1 0 0 1 0 1 0 0 1 0 1 0 0 0]"
        "[1 1 1 0 0 1 1 1 0 0 1 1 1 1 0 0]"
        "[1 1 1 1 0 1 1 1 1 0 1 1 1 1 1 0])")
    TEST_ASSERT_EQ(
        "(set l (list -2i 0i 0Ni 1i 2i -2 0 0Nl 1 2 -2.0 -0.0 0Nf 0.0 1.0  2.0)) (set f (fn [x y] (sum (as 'I32 (!= x "
        "(enlist y)))))) (map (fn[x] (map f x l)) l)",
        "(list [0 1 1 1 1 0 1 1 1 1 0 1 1 1 1 1]"
        "[1 0 1 1 1 1 0 1 1 1 1 0 1 0 1 1]"
        "[1 1 0 1 1 1 1 0 1 1 1 1 0 1 1 1]"
        "[1 1 1 0 1 1 1 1 0 1 1 1 1 1 0 1]"
        "[1 1 1 1 0 1 1 1 1 0 1 1 1 1 1 0]"
        "[0 1 1 1 1 0 1 1 1 1 0 1 1 1 1 1]"
        "[1 0 1 1 1 1 0 1 1 1 1 0 1 0 1 1]"
        "[1 1 0 1 1 1 1 0 1 1 1 1 0 1 1 1]"
        "[1 1 1 0 1 1 1 1 0 1 1 1 1 1 0 1]"
        "[1 1 1 1 0 1 1 1 1 0 1 1 1 1 1 0]"
        "[0 1 1 1 1 0 1 1 1 1 0 1 1 1 1 1]"
        "[1 0 1 1 1 1 0 1 1 1 1 0 1 0 1 1]"
        "[1 1 0 1 1 1 1 0 1 1 1 1 0 1 1 1]"
        "[1 0 1 1 1 1 0 1 1 1 1 0 1 0 1 1]"
        "[1 1 1 0 1 1 1 1 0 1 1 1 1 1 0 1]"
        "[1 1 1 1 0 1 1 1 1 0 1 1 1 1 1 0])")
    TEST_ASSERT_EQ(
        "(set l (list -2i 0i 0Ni 1i 2i -2 0 0Nl 1 2 -2.0 -0.0 0Nf 0.0 1.0  2.0)) (set f (fn [x y] (sum (as 'I32 (<= x "
        "(enlist y)))))) (map (fn[x] (map f x l)) l)",
        "(list [1 1 0 1 1 1 1 0 1 1 1 1 0 1 1 1]"
        "[0 1 0 1 1 0 1 0 1 1 0 1 0 1 1 1]"
        "[1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1]"
        "[0 0 0 1 1 0 0 0 1 1 0 0 0 0 1 1]"
        "[0 0 0 0 1 0 0 0 0 1 0 0 0 0 0 1]"
        "[1 1 0 1 1 1 1 0 1 1 1 1 0 1 1 1]"
        "[0 1 0 1 1 0 1 0 1 1 0 1 0 1 1 1]"
        "[1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1]"
        "[0 0 0 1 1 0 0 0 1 1 0 0 0 0 1 1]"
        "[0 0 0 0 1 0 0 0 0 1 0 0 0 0 0 1]"
        "[1 1 0 1 1 1 1 0 1 1 1 1 0 1 1 1]"
        "[0 1 0 1 1 0 1 0 1 1 0 1 0 1 1 1]"
        "[1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1]"
        "[0 1 0 1 1 0 1 0 1 1 0 1 0 1 1 1]"
        "[0 0 0 1 1 0 0 0 1 1 0 0 0 0 1 1]"
        "[0 0 0 0 1 0 0 0 0 1 0 0 0 0 0 1])")
    TEST_ASSERT_EQ(
        "(set l (list -2i 0i 0Ni 1i 2i -2 0 0Nl 1 2 -2.0 -0.0 0Nf 0.0 1.0  2.0)) (set f (fn [x y] (sum (as 'I32 (>= x "
        "(enlist y)))))) (map (fn[x] (map f x l)) l)",
        "(list [1 0 1 0 0 1 0 1 0 0 1 0 1 0 0 0]"
        "[1 1 1 0 0 1 1 1 0 0 1 1 1 1 0 0]"
        "[0 0 1 0 0 0 0 1 0 0 0 0 1 0 0 0]"
        "[1 1 1 1 0 1 1 1 1 0 1 1 1 1 1 0]"
        "[1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1]"
        "[1 0 1 0 0 1 0 1 0 0 1 0 1 0 0 0]"
        "[1 1 1 0 0 1 1 1 0 0 1 1 1 1 0 0]"
        "[0 0 1 0 0 0 0 1 0 0 0 0 1 0 0 0]"
        "[1 1 1 1 0 1 1 1 1 0 1 1 1 1 1 0]"
        "[1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1]"
        "[1 0 1 0 0 1 0 1 0 0 1 0 1 0 0 0]"
        "[1 1 1 0 0 1 1 1 0 0 1 1 1 1 0 0]"
        "[0 0 1 0 0 0 0 1 0 0 0 0 1 0 0 0]"
        "[1 1 1 0 0 1 1 1 0 0 1 1 1 1 0 0]"
        "[1 1 1 1 0 1 1 1 1 0 1 1 1 1 1 0]"
        "[1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1])")
    TEST_ASSERT_EQ(
        "(set l (list -2i 0i 0Ni 1i 2i -2 0 0Nl 1 2 -2.0 -0.0 0Nf 0.0 1.0  2.0)) (set f (fn [x y] (sum (as 'I32 (!= "
        "(enlist x)(enlist y)))))) (map (fn[x] (map f x l)) l)",
        "(list [0 1 1 1 1 0 1 1 1 1 0 1 1 1 1 1]"
        "[1 0 1 1 1 1 0 1 1 1 1 0 1 0 1 1]"
        "[1 1 0 1 1 1 1 0 1 1 1 1 0 1 1 1]"
        "[1 1 1 0 1 1 1 1 0 1 1 1 1 1 0 1]"
        "[1 1 1 1 0 1 1 1 1 0 1 1 1 1 1 0]"
        "[0 1 1 1 1 0 1 1 1 1 0 1 1 1 1 1]"
        "[1 0 1 1 1 1 0 1 1 1 1 0 1 0 1 1]"
        "[1 1 0 1 1 1 1 0 1 1 1 1 0 1 1 1]"
        "[1 1 1 0 1 1 1 1 0 1 1 1 1 1 0 1]"
        "[1 1 1 1 0 1 1 1 1 0 1 1 1 1 1 0]"
        "[0 1 1 1 1 0 1 1 1 1 0 1 1 1 1 1]"
        "[1 0 1 1 1 1 0 1 1 1 1 0 1 0 1 1]"
        "[1 1 0 1 1 1 1 0 1 1 1 1 0 1 1 1]"
        "[1 0 1 1 1 1 0 1 1 1 1 0 1 0 1 1]"
        "[1 1 1 0 1 1 1 1 0 1 1 1 1 1 0 1]"
        "[1 1 1 1 0 1 1 1 1 0 1 1 1 1 1 0])")
    TEST_ASSERT_EQ(
        "(set l (list -2i 0i 0Ni 1i 2i -2 0 0Nl 1 2 -2.0 -0.0 0Nf 0.0 1.0  2.0)) (set f (fn [x y] (sum (as 'I32 (<= "
        "(enlist x) (enlist y)))))) (map (fn[x] (map f x l)) l)",
        "(list [1 1 0 1 1 1 1 0 1 1 1 1 0 1 1 1]"
        "[0 1 0 1 1 0 1 0 1 1 0 1 0 1 1 1]"
        "[1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1]"
        "[0 0 0 1 1 0 0 0 1 1 0 0 0 0 1 1]"
        "[0 0 0 0 1 0 0 0 0 1 0 0 0 0 0 1]"
        "[1 1 0 1 1 1 1 0 1 1 1 1 0 1 1 1]"
        "[0 1 0 1 1 0 1 0 1 1 0 1 0 1 1 1]"
        "[1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1]"
        "[0 0 0 1 1 0 0 0 1 1 0 0 0 0 1 1]"
        "[0 0 0 0 1 0 0 0 0 1 0 0 0 0 0 1]"
        "[1 1 0 1 1 1 1 0 1 1 1 1 0 1 1 1]"
        "[0 1 0 1 1 0 1 0 1 1 0 1 0 1 1 1]"
        "[1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1]"
        "[0 1 0 1 1 0 1 0 1 1 0 1 0 1 1 1]"
        "[0 0 0 1 1 0 0 0 1 1 0 0 0 0 1 1]"
        "[0 0 0 0 1 0 0 0 0 1 0 0 0 0 0 1])")
    TEST_ASSERT_EQ(
        "(set l (list -2i 0i 0Ni 1i 2i -2 0 0Nl 1 2 -2.0 -0.0 0Nf 0.0 1.0  2.0)) (set f (fn [x y] (sum (as 'I32 (>= "
        "(enlist x) (enlist y)))))) (map (fn[x] (map f x l)) l)",
        "(list [1 0 1 0 0 1 0 1 0 0 1 0 1 0 0 0]"
        "[1 1 1 0 0 1 1 1 0 0 1 1 1 1 0 0]"
        "[0 0 1 0 0 0 0 1 0 0 0 0 1 0 0 0]"
        "[1 1 1 1 0 1 1 1 1 0 1 1 1 1 1 0]"
        "[1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1]"
        "[1 0 1 0 0 1 0 1 0 0 1 0 1 0 0 0]"
        "[1 1 1 0 0 1 1 1 0 0 1 1 1 1 0 0]"
        "[0 0 1 0 0 0 0 1 0 0 0 0 1 0 0 0]"
        "[1 1 1 1 0 1 1 1 1 0 1 1 1 1 1 0]"
        "[1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1]"
        "[1 0 1 0 0 1 0 1 0 0 1 0 1 0 0 0]"
        "[1 1 1 0 0 1 1 1 0 0 1 1 1 1 0 0]"
        "[0 0 1 0 0 0 0 1 0 0 0 0 1 0 0 0]"
        "[1 1 1 0 0 1 1 1 0 0 1 1 1 1 0 0]"
        "[1 1 1 1 0 1 1 1 1 0 1 1 1 1 1 0]"
        "[1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1])")

    // Test DATE comparisons
    TEST_ASSERT_EQ("(== 2024.01.01 2024.01.01)", "true");
    TEST_ASSERT_EQ("(!= 2024.01.01 2024.01.02)", "true");
    TEST_ASSERT_EQ("(< 2024.01.01 2024.01.02)", "true");
    TEST_ASSERT_EQ("(> 2024.01.02 2024.01.01)", "true");
    TEST_ASSERT_EQ("(<= 2024.01.01 2024.01.02)", "true");
    TEST_ASSERT_EQ("(>= 2024.01.02 2024.01.01)", "true");
    TEST_ASSERT_EQ("(<= 2024.01.01 2024.01.01)", "true");
    TEST_ASSERT_EQ("(>= 2024.01.01 2024.01.01)", "true");

    // Test TIME comparisons
    TEST_ASSERT_EQ("(== 10:15:30.000 10:15:30.000)", "true");
    TEST_ASSERT_EQ("(!= 10:15:30.000 10:15:31.000)", "true");
    TEST_ASSERT_EQ("(< 10:15:30.000 10:15:31.000)", "true");
    TEST_ASSERT_EQ("(> 10:15:31.000 10:15:30.000)", "true");
    TEST_ASSERT_EQ("(<= 10:15:30.000 10:15:30.000)", "true");
    TEST_ASSERT_EQ("(>= 10:15:30.000 10:15:30.000)", "true");

    // Test TIMESTAMP comparisons
    TEST_ASSERT_EQ("(== 2024.01.01D10:15:30.000000000 2024.01.01D10:15:30.000000000)", "true");
    TEST_ASSERT_EQ("(!= 2024.01.01D10:15:30.000000000 2024.01.01D10:15:31.000000000)", "true");
    TEST_ASSERT_EQ("(< 2024.01.01D10:15:30.000000000 2024.01.01D10:15:31.000000000)", "true");
    TEST_ASSERT_EQ("(> 2024.01.01D10:15:31.000000000 2024.01.01D10:15:30.000000000)", "true");
    TEST_ASSERT_EQ("(<= 2024.01.01D10:15:30.000000000 2024.01.01D10:15:30.000000000)", "true");
    TEST_ASSERT_EQ("(>= 2024.01.01D10:15:30.000000000 2024.01.01D10:15:30.000000000)", "true");

    TEST_ASSERT_EQ("(== [1i 2i 3i] [1 2 4])", "[true true false]");
    TEST_ASSERT_EQ("(== [1 1 3] [1.0 2.0 3.0])", "[true false true]");
    TEST_ASSERT_EQ("(== [2i 2i 3i] [1.0 2.0 3.0])", "[false true true]");
    TEST_ASSERT_EQ("(!= [1i 2i 3i] [1 2 4])", "[false false true]");
    TEST_ASSERT_EQ("(< [1i 2i 3i] [1 3 3])", "[false true false]");
    TEST_ASSERT_EQ("(> [1 3 3] [1i 2i 3i])", "[false true false]");

    TEST_ASSERT_EQ("(== [0Ni 0Ni] [0Ni 0i])", "[true false]");
    TEST_ASSERT_EQ("(!= [0Ni 1i] [0Ni 2i])", "[false true]");
    TEST_ASSERT_EQ("(== [0Nf 1.0] [0Nf 1.0])", "[true true]");
    TEST_ASSERT_EQ("(!= [0Nf 1.0] [0Nf 2.0])", "[false true]");

    TEST_ASSERT_EQ("(== 5i [5i -5i 5i])", "[true false true]");
    TEST_ASSERT_EQ("(!= 5i [5i 6i 5i])", "[false true false]");
    TEST_ASSERT_EQ("(< 5i [6i 7i 8i])", "[true true true]");
    TEST_ASSERT_EQ("(> 5i [4i 8i 2i])", "[true false true]");
    TEST_ASSERT_EQ("(<= 5i [5i 6i 3i])", "[true true false]");
    TEST_ASSERT_EQ("(>= 5i [4i 5i 8i])", "[true true false]");

    TEST_ASSERT_EQ("(== [5.0 5.0 5.0] 5)", "[true true true]");
    TEST_ASSERT_EQ("(!= [5.0 6.0 5.0] 5)", "[false true false]");
    TEST_ASSERT_EQ("(< [4.0 3.0 2.0] 5)", "[true true true]");
    TEST_ASSERT_EQ("(> [6.0 7.0 3.0] 5)", "[true true false]");
    TEST_ASSERT_EQ("(<= [3.0 5.0 4.0] 5)", "[true true true]");
    TEST_ASSERT_EQ("(>= [6.0 5.0 3.0] 5)", "[true true false]");

    TEST_ASSERT_EQ("(== [1i 2i -3i] [1i 2i 3i])", "[true true false]");
    TEST_ASSERT_EQ("(!= [1i -2i] [1i 2i])", "[false true]");

    TEST_ASSERT_EQ("(< 2024.01.01 [2024.01.02 2023.12.31])", "[true false]");
    TEST_ASSERT_EQ("(> [2024.01.02 2024.01.05] 2024.01.03)", "[false true]");
    TEST_ASSERT_EQ("(< 10:00:01.100 [10:00:00.200 10:00:02.000])", "[false true]");
    TEST_ASSERT_EQ("(> [10:00:01.000 10:00:02.000] 10:00:01.500)", "[false true]");

    TEST_ASSERT_EQ("(== [2024.01.01D10:00:00.000000000] 2024.01.01D10:00:00.000000000)", "[true]");
    TEST_ASSERT_EQ("(!= 2024.01.01D10:00:00.000000000 [2024.01.01D10:00:01.000000000])", "[true]");
    TEST_ASSERT_EQ("(< [2024.01.01D10:00:00.000000000] [2024.01.01D10:00:01.000000000])", "[true]");
    TEST_ASSERT_EQ("(> [2024.01.01D10:00:01.000000000] [2024.01.01D10:00:00.000000000])", "[true]");

    TEST_ASSERT_EQ("(< 2024.01.01 2024.01.01D10:00:00.000000000)", "true");
    TEST_ASSERT_EQ("(> 2024.01.01D10:00:00.000000000 2024.01.01)", "true");
    TEST_ASSERT_EQ("(< [2024.01.01] 2024.01.01D10:00:00.000000000)", "[true]");
    TEST_ASSERT_EQ("(> [2024.01.01D10:00:00.000000000] 2024.01.01)", "[true]");
    TEST_ASSERT_EQ("(< 2024.01.01 [2024.01.01D10:00:00.000000000])", "[true]");
    TEST_ASSERT_EQ("(> 2024.01.01D10:00:00.000000000 [2024.01.01])", "[true]");
    TEST_ASSERT_EQ("(< [2024.01.01] [2024.01.01D10:00:00.000000000])", "[true]");
    TEST_ASSERT_EQ("(> [2024.01.01D10:00:00.000000000] [2024.01.01])", "[true]");

    TEST_ASSERT_EQ("(set t (guid 2)) (== (at t 0) (at t 0))", "true");
    TEST_ASSERT_EQ("(set t (guid 2)) (!= (at t 0) (at t 1))", "true");
    TEST_ASSERT_EQ("(set t (guid 2)) (== (at t 0) (enlist (at t 0)))", "[true]");
    TEST_ASSERT_EQ("(set t (guid 2)) (== (enlist (at t 0)) (enlist (at t 0)))", "[true]");
    TEST_ASSERT_EQ(
        "(set t1 (as 'guid \"d49f18a4-1969-49e8-9b8a-6bb9a4832eea\")) "
        "(set t2 (as 'guid \"d49f18a4-196a-49e8-9b8a-6bb9a4832eea\")) (< t1 t2)",
        "true");
    TEST_ASSERT_EQ(
        "(set t1 (as 'guid \"d49f18a4-1969-49e9-9b8a-6bb9a4832eea\")) "
        "(set t2 (as 'guid \"d49f18a4-1969-49e8-9b8a-6bb9a4832eea\")) (> t1 t2)",
        "true");
    TEST_ASSERT_EQ(
        "(set t1 (enlist (as 'guid \"d49f18a4-1969-49e8-9b8a-6bb9a4832eea\"))) "
        "(set t2 (as 'guid \"d49f18a4-196a-49e8-9b8a-6bb9a4832eea\")) (< t1 t2)",
        "[true]");
    TEST_ASSERT_EQ(
        "(set t1 (as 'guid \"d49f18a4-1969-49e9-9b8a-6bb9a4832eea\")) "
        "(set t2 (enlist (as 'guid \"d49f18a4-1969-49e8-9b8a-6bb9a4832eea\"))) (> t1 t2)",
        "[true]");
    TEST_ASSERT_EQ(
        "(set t1 (as 'guid \"d49f18a4-1969-49e8-9b8a-6bb9a4832eea\")) "
        "(set t2 (enlist (as 'guid \"d49f18a4-1969-49e8-9b8a-6bb9a4832eea\"))) (<= t1 t2)",
        "[true]");
    TEST_ASSERT_EQ(
        "(set t1 (enlist (as 'guid \"d49f18a4-1969-49e8-9b8a-6bb9a4832eea\"))) "
        "(set t2 (enlist (as 'guid \"d49f18a4-196a-49e8-9b8a-6bb9a4832eea\"))) (<= t1 t2)",
        "[true]");
    TEST_ASSERT_EQ(
        "(set t1 (enlist (as 'guid \"d49f18a4-1969-49e9-9b8a-6bb9a4832eea\"))) "
        "(set t2 (as 'guid \"d49f18a4-1969-49e8-9b8a-6bb9a4832eea\")) (>= t1 t2)",
        "[true]");
    TEST_ASSERT_EQ(
        "(set t1 (enlist (as 'guid \"d49f18a4-1969-49e9-9b8a-6bb9a4832eea\"))) "
        "(set t2 (enlist (as 'guid \"d49f18a4-1969-49e9-9b8a-6bb9a4832eea\"))) (>= t1 t2)",
        "[true]");

    PASS();
}

test_result_t test_lang_distinct() {
    TEST_ASSERT_EQ("(distinct [true true])", "[true]");
    TEST_ASSERT_EQ("(distinct [0x12 0x12 0x10])", "[0x10 0x12]");
    TEST_ASSERT_EQ("(distinct \"test\")", "\"est\"");
    TEST_ASSERT_EQ("(distinct \"\")", "\"\"");
    TEST_ASSERT_EQ("(distinct [1h 2h 2h])", "[1 2]");
    TEST_ASSERT_EQ("(distinct [1i 0Ni 1i])", "[1i]");
    TEST_ASSERT_EQ("(distinct [2012.12.12 2012.12.12])", "[2012.12.12]");
    TEST_ASSERT_EQ("(distinct [10:00:00.000 20:10:10.500 10:00:00.000])", "[10:00:00.000 20:10:10.500]");
    TEST_ASSERT_EQ("(distinct [1 1 1 2 3 4 2 3 4 2 3 4])", "[1 2 3 4]");
    TEST_ASSERT_EQ("(distinct [2024.01.01D10:00:00.000000000 2024.01.01D10:00:00.000000000])",
                   "[2024.01.01D10:00:00.000000000]");
    TEST_ASSERT_EQ("(distinct ['a 'b 'ab 'aa 'a 'aa])", "['a 'b 'ab 'aa]");
    TEST_ASSERT_EQ("(set l (guid 2)) (set l (concat l l)) (count (distinct l))", "2");
    TEST_ASSERT_EQ("(distinct (list [3i 3i] 2i [3i 3i] 2i))", "(list 2i [3i 3i])");

    PASS();
}

test_result_t test_lang_concat() {
    TEST_ASSERT_EQ("(concat true false)", "[true false]");
    TEST_ASSERT_EQ("(concat [true] false)", "[true false]");
    TEST_ASSERT_EQ("(concat true [false])", "[true false]");
    TEST_ASSERT_EQ("(concat [true] [false])", "[true false]");
    TEST_ASSERT_EQ("(concat 0x0d 0x0a)", "[0x0d 0x0a]");
    TEST_ASSERT_EQ("(concat [0x0d] 0x0a)", "[0x0d 0x0a]");
    TEST_ASSERT_EQ("(concat 0x0d [0x0a])", "[0x0d 0x0a]");
    TEST_ASSERT_EQ("(concat [0x0d] [0x0a])", "[0x0d 0x0a]");
    TEST_ASSERT_EQ("(concat 't' 's')", "\"ts\"");
    TEST_ASSERT_EQ("(concat 't' \"est\")", "\"test\"");
    TEST_ASSERT_EQ("(concat \"tes\" 't')", "\"test\"");
    TEST_ASSERT_EQ("(concat \"te\" \"st\")", "\"test\"");
    TEST_ASSERT_EQ("(concat 't' \"est\\000\")", "\"test\"");
    TEST_ASSERT_EQ("(concat \"tes\\000\" 't')", "\"test\"");
    TEST_ASSERT_EQ("(concat \"te\\000\" \"st\\000\")", "\"test\"");
    TEST_ASSERT_EQ("(concat 1h 2h)", "[1h 2h]");
    TEST_ASSERT_EQ("(concat [1h] 2h)", "[1h 2h]");
    TEST_ASSERT_EQ("(concat 1h [2h])", "[1h 2h]");
    TEST_ASSERT_EQ("(concat [1h] [2h])", "[1h 2h]");
    TEST_ASSERT_EQ("(concat 1i 2i)", "[1i 2i]");
    TEST_ASSERT_EQ("(concat [1i] 2i)", "[1i 2i]");
    TEST_ASSERT_EQ("(concat 1i [2i])", "[1i 2i]");
    TEST_ASSERT_EQ("(concat [1i] [2i])", "[1i 2i]");
    TEST_ASSERT_EQ("(concat 2020.10.10 2020.10.12)", "[2020.10.10 2020.10.12]");
    TEST_ASSERT_EQ("(concat [2020.10.10] 2020.10.12)", "[2020.10.10 2020.10.12]");
    TEST_ASSERT_EQ("(concat 2020.10.10 [2020.10.12])", "[2020.10.10 2020.10.12]");
    TEST_ASSERT_EQ("(concat [2020.10.10] [2020.10.12])", "[2020.10.10 2020.10.12]");
    TEST_ASSERT_EQ("(concat 10:00:00.000 10:00:00.001)", "[10:00:00.000 10:00:00.001]");
    TEST_ASSERT_EQ("(concat [10:00:00.000] 10:00:00.001)", "[10:00:00.000 10:00:00.001]");
    TEST_ASSERT_EQ("(concat 10:00:00.000 [10:00:00.001])", "[10:00:00.000 10:00:00.001]");
    TEST_ASSERT_EQ("(concat [10:00:00.000] [10:00:00.001])", "[10:00:00.000 10:00:00.001]");
    TEST_ASSERT_EQ("(concat 1 2)", "[1 2]");
    TEST_ASSERT_EQ("(concat [1] 2)", "[1 2]");
    TEST_ASSERT_EQ("(concat 1 [2])", "[1 2]");
    TEST_ASSERT_EQ("(concat [1] [2])", "[1 2]");
    TEST_ASSERT_EQ("(concat 'a 'b)", "['a 'b]");
    TEST_ASSERT_EQ("(concat 'a ['b])", "['a 'b]");
    TEST_ASSERT_EQ("(concat ['a] 'b)", "['a 'b]");
    TEST_ASSERT_EQ("(concat ['a] ['b])", "['a 'b]");
    TEST_ASSERT_EQ("(concat 2025.01.01D15:17:19.000000000 2025.01.02D15:17:19.000000002)",
                   "[2025.01.01D15:17:19.000000000 2025.01.02D15:17:19.000000002]");
    TEST_ASSERT_EQ("(concat [2025.01.01D15:17:19.000000000] 2025.01.02D15:17:19.000000002)",
                   "[2025.01.01D15:17:19.000000000 2025.01.02D15:17:19.000000002]");
    TEST_ASSERT_EQ("(concat 2025.01.01D15:17:19.000000000 [2025.01.02D15:17:19.000000002])",
                   "[2025.01.01D15:17:19.000000000 2025.01.02D15:17:19.000000002]");
    TEST_ASSERT_EQ("(concat [2025.01.01D15:17:19.000000000] [2025.01.02D15:17:19.000000002])",
                   "[2025.01.01D15:17:19.000000000 2025.01.02D15:17:19.000000002]");
    TEST_ASSERT_EQ("(concat 1.0 2.0)", "[1.0 2.0]");
    TEST_ASSERT_EQ("(concat [1.0] 2.0)", "[1.0 2.0]");
    TEST_ASSERT_EQ("(concat 1.0 [2.0])", "[1.0 2.0]");
    TEST_ASSERT_EQ("(concat [1.0] [2.0])", "[1.0 2.0]");

    TEST_ASSERT_EQ(
        "(concat (dict [A B] (list (+ (til 4) 10) [16 17 18 19])) "
        "(dict [C A] (list (list \"A\" \"B\" \"C\" \"D\" \"E\") (til 5))))",
        "(dict [A B C] (list (til 5) [16 17 18 19] (list \"A\" \"B\" \"C\" \"D\" \"E\")))");
    TEST_ASSERT_EQ(
        "(concat (table [A C] (list (+ (til 4) 10) (list \"a\" \"b\" \"c\" \"d\"))) (table [C B A] "
        "(list (list \"A\" \"B\" \"C\" \"D\" \"E\") [100 200 300 400 500] (til 5))))",
        "(table [A C] (list [10 11 12 13 0 1 2 3 4] (list \"a\" \"b\" \"c\" \"d\" \"A\" \"B\" \"C\" \"D\" \"E\")))");
    TEST_ASSERT_ER(
        "(concat (table [D C] (list (+ (til 4) 10) (list \"a\" \"b\" \"c\" \"d\"))) (table [C B A] "
        "(list (list \"A\" \"B\" \"C\" \"D\" \"E\") [100 200 300 400 500] (til 5))))",
        "concat: keys a not compatible");
    TEST_ASSERT_ER(
        "(concat (table [C A] (list (+ (til 4) 10) (list \"a\" \"b\" \"c\" \"d\"))) (table [C B A] "
        "(list (list \"A\" \"B\" \"C\" \"D\" \"E\") [100 200 300 400 500] (til 5))))",
        "concat: values a not compatible");

    TEST_ASSERT_EQ(
        "(concat (as 'guid \"d49f18a4-1969-49e8-9b8a-6bb9a4832eea\") "
        "(as 'guid \"a49f18a4-1969-49e8-9b8a-6bb9a4832eea\"))",
        "[d49f18a4-1969-49e8-9b8a-6bb9a4832eea a49f18a4-1969-49e8-9b8a-6bb9a4832eea]");
    TEST_ASSERT_EQ(
        "(concat (as 'guid \"d49f18a4-1969-49e8-9b8a-6bb9a4832eea\") "
        "(enlist (as 'guid \"a49f18a4-1969-49e8-9b8a-6bb9a4832eea\")))",
        "[d49f18a4-1969-49e8-9b8a-6bb9a4832eea a49f18a4-1969-49e8-9b8a-6bb9a4832eea]");
    TEST_ASSERT_EQ(
        "(concat (enlist (as 'guid \"d49f18a4-1969-49e8-9b8a-6bb9a4832eea\")) "
        "(as 'guid \"a49f18a4-1969-49e8-9b8a-6bb9a4832eea\"))",
        "[d49f18a4-1969-49e8-9b8a-6bb9a4832eea a49f18a4-1969-49e8-9b8a-6bb9a4832eea]");

    TEST_ASSERT_EQ("(concat (list 'a') 5)", "(list 'a' 5)");
    TEST_ASSERT_EQ("(concat 5 (list 'a'))", "(list 5 'a')");
    TEST_ASSERT_EQ("(concat 'a' 5)", "(list 'a' 5)");
    PASS();
}

test_result_t test_lang_raze() {
    TEST_ASSERT_EQ("(raze (list [1 2] [3 4]))", "[1 2 3 4]");
    TEST_ASSERT_EQ("(raze (list [1 2] [3.0 4.0]))", "(list 1 2 3.0 4.0)");
    TEST_ASSERT_EQ("(raze (list [1 2] (list 3 4)))", "[1 2 3 4]");
    TEST_ASSERT_EQ("(raze (list (list 1 2) (list 3 4)))", "[1 2 3 4]");
    TEST_ASSERT_EQ("(raze (list (list 1 2) (list 3.0 4)))", "(list 1 2 3.0 4)");
    TEST_ASSERT_EQ("(raze (list))", "()");
    TEST_ASSERT_EQ("(raze (list [1 2 3]))", "[1 2 3]");
    TEST_ASSERT_EQ("(raze 42)", "42");
    PASS();
}

test_result_t test_lang_filter() {
    TEST_ASSERT_EQ("(filter [true false true false] [true true false false])", "[true false]");
    TEST_ASSERT_EQ("(filter [0x12 0x11 0x10] [true false true])", "[0x12 0x10]");
    TEST_ASSERT_EQ("(filter \"test\" [true false true false])", "\"ts\"");
    TEST_ASSERT_EQ("(filter [1h 2h 3h] [false true false])", "[2h]");
    TEST_ASSERT_EQ("(filter [1i 0Ni 2i] [false true true])", "[0Ni 2i]");
    TEST_ASSERT_EQ("(filter [2012.12.12 2012.12.13] [true false])", "[2012.12.12]");
    TEST_ASSERT_EQ("(filter [10:00:00.000] [true])", "[10:00:00.000]");
    TEST_ASSERT_EQ("(filter [1 0Nl 2] [true true true])", "[1 0Nl 2]");
    TEST_ASSERT_EQ("(filter ['a 'b 'c 'dd] [true false false true])", "['a 'dd]");
    TEST_ASSERT_EQ("(filter [2024.01.01D10:00:00.000000000] [true])", "[2024.01.01D10:00:00.000000000]");
    TEST_ASSERT_EQ("(filter [1.0 2.0 3.0] [true false true])", "[1.0 3.0]");
    TEST_ASSERT_EQ("(filter (guid 2) [false false])", "[]");
    TEST_ASSERT_EQ("(filter (take 2 (as 'guid \"d49f18a4-1969-49e8-9b8a-6bb9a4832eea\")) [false true])",
                   "[d49f18a4-1969-49e8-9b8a-6bb9a4832eea]");
    TEST_ASSERT_EQ("(filter (list [3i 0Ni] 2i [3i 3i] 2i) [true true false false])", "(list [3i 0Ni] 2i)");
    TEST_ASSERT_EQ("(first (filter (table [a b] (list [1 2 3] (list 'a 'b 'c))) [false true true]))", "{a:2 b:'b}");

    TEST_ASSERT_ER("(filter [1i 0Ni 2i] [true true])", "filter: arguments must be the same length");
    TEST_ASSERT_ER("(filter [true false] [1 2])", "filter: unsupported types: 'B8, 'I64");

    PASS();
}

test_result_t test_lang_in() {
    TEST_ASSERT_EQ("(in 2 2)", "true");
    TEST_ASSERT_EQ("(in false [true false])", "true");
    TEST_ASSERT_EQ("(in 0x12 [0x12 0x11 0x10])", "true");
    TEST_ASSERT_EQ("(in 'e' \"test\")", "true");
    TEST_ASSERT_EQ("(in \"asd\" \"asd\")", "[true true true]");
    TEST_ASSERT_EQ("(in \"asd\" 'a')", "[true false false]");
    TEST_ASSERT_EQ("(in 1h [2h 3h])", "false");
    TEST_ASSERT_EQ("(in 2h [2 3])", "true");
    TEST_ASSERT_EQ("(in 1i [1i 0Ni 2i])", "true");
    TEST_ASSERT_EQ("(in 2012.12.12 [2012.12.12 2012.12.13])", "true");
    TEST_ASSERT_EQ("(in 10:00:00.000 [10:00:00.000 20:10:10.500])", "true");
    TEST_ASSERT_EQ("(in 1 [0Nl])", "false");
    TEST_ASSERT_EQ("(in 'a ['a 'b 'c 'dd])", "true");
    TEST_ASSERT_EQ("(in 2024.01.01D10:00:00.000000000 [])", "false");
    TEST_ASSERT_EQ("(in 1.0 [1.0 2.0 3.0])", "true");
    TEST_ASSERT_EQ("(in (guid 2) [])", "[false false]");
    TEST_ASSERT_EQ("(in 2 (list '3' 2))", "true");
    TEST_ASSERT_EQ("(in 3 [1i 0Ni 2i])", "false");
    TEST_ASSERT_EQ("(in [true false] [false])", "[false true]");
    TEST_ASSERT_EQ("(in [0x00 0x01] [0x02 0x00])", "[true false]");
    TEST_ASSERT_EQ("(in \"test\" \"post\")", "[true false true true]");
    TEST_ASSERT_EQ("(in [3h 2h 5h 0Nh] [1h 0Nh 2h 3h])", "[true true false true]");
    TEST_ASSERT_EQ("(in [3 2 5 0Nl -32768] [1h 0Nh 2h 3h])", "[true true false true false]");
    TEST_ASSERT_EQ("(in [3i 2i 5i 0Ni -32768i] [1h 0Nh 2h 3h])", "[true true false true false]");
    TEST_ASSERT_EQ("(in [3i 2i 5i 0Ni] [1i 0Ni 2i 3i])", "[true true false true]");
    TEST_ASSERT_EQ("(in [3h 2h 5h 0Nh] [1i 0Ni 2i 3i])", "[true true false true]");
    TEST_ASSERT_EQ("(in [3h 2h 5h 0Nh] [1 0Nl 2 3])", "[true true false true]");
    TEST_ASSERT_EQ("(in [3 2 5 0Nl -2147483648] [1i 0Ni 2i 3i])", "[true true false true false]");
    TEST_ASSERT_EQ("(in [3i 2i 0Ni] [1 0Nl 2 3])", "[true true true]");
    TEST_ASSERT_EQ("(in [3 2 5 0Nl] [1 0Nl 2 3])", "[true true false true]");
    TEST_ASSERT_EQ("(in (list 3h 2i 5 0Nl) [1 0Nl 2 3])", "(list true true false true)");
    TEST_ASSERT_EQ("(in [0 1 0Nl] 0Nl)", "[false false true]");
    TEST_ASSERT_EQ("(in [3] (list 3i 2 5))", "[true]");
    TEST_ASSERT_EQ("(in 'a' (list \"iu\" \"asd\"))", "false");
    TEST_ASSERT_EQ("(in 'a' (list \"iu\" \"asd\" 'a'))", "true");
    TEST_ASSERT_EQ("(in \"asd\" (list \"iu\" \"asd\"))", "[false false false]");
    TEST_ASSERT_EQ("(in \"asd\" (list 'd' \"asd\"))", "[false false true]");
    TEST_ASSERT_EQ("(in (list \"asd\") (list \"iu\" \"asd\"))", "(list [false false false])");      //(list true)
    TEST_ASSERT_EQ("(in (list \"asd\" \"iu\") \"asd\")", "(list [true true true] [false false])");  //(list false false)
    TEST_ASSERT_EQ("(set l (guid 2)) (in l l)", "[true true]");
    TEST_ASSERT_EQ("(set l (guid 2)) (in (first l) l)", "true");
    TEST_ASSERT_EQ("(set l (guid 2)) (in l (first l))", "[true false]");
    TEST_ASSERT_EQ("(set l (guid 2)) (in (first l) (list (first l)))", "true");
    TEST_ASSERT_EQ("(set l (guid 2)) (in (list (first l)) l)", "(list true)");
    TEST_ASSERT_EQ("(set l (guid 2)) (in (list (first l)) (list l))", "(list false)");
    TEST_ASSERT_EQ("(set l (guid 2)) (in (list (first l)) (list (first l)))", "(list true)");

    // ========== I16 x I8 "IN" TESTS (Bug fix: index_in_i16_i8) ==========
    // Test i16 values against i8 lookup set
    TEST_ASSERT_EQ("(in [1h 2h 3h] [1 2])", "[true true false]");             // i16 in i8 range
    TEST_ASSERT_EQ("(in [100h 50h 25h] [50 100])", "[true true false]");      // i16 values in i8 lookup
    TEST_ASSERT_EQ("(in [-128h 127h 0h] [-128 0 127])", "[true true true]");  // i8 boundary values
    TEST_ASSERT_EQ("(in [200h 10h] [10])", "[false true]");                   // value outside i8 range

    // ========== I16 x I16 "IN" TESTS (Bug fix: index_in_i16_i16 wrong size) ==========
    // Test when x length > y length (was causing buffer overflow)
    TEST_ASSERT_EQ("(in [1h 2h 3h 4h 5h] [2h 4h])", "[false true false true false]");
    TEST_ASSERT_EQ("(in [10h 20h 30h 40h 50h 60h] [20h])", "[false true false false false false]");
    // Test when x length < y length
    TEST_ASSERT_EQ("(in [1h 2h] [1h 2h 3h 4h 5h])", "[true true]");
    // Test with equal lengths
    TEST_ASSERT_EQ("(in [1h 2h 3h] [3h 2h 1h])", "[true true true]");
    // Test with empty arrays
    TEST_ASSERT_EQ("(in (list) [1h 2h])", "(list)");

    PASS();
}

test_result_t test_lang_except() {
    // Basic symbol except
    TEST_ASSERT_EQ("(except ['a 'b 'c] ['u 'o])", "[a b c]");
    TEST_ASSERT_EQ("(except ['a 'b 'c] ['a 'c])", "[b]");
    TEST_ASSERT_EQ("(except ['a 'b 'c] ['b])", "[a c]");
    TEST_ASSERT_EQ("(except ['a 'b 'c] ['a 'b 'c])", "[]");

    // Integer except
    TEST_ASSERT_EQ("(except [1 2 3 4 5] [2 4])", "[1 3 5]");
    TEST_ASSERT_EQ("(except [1 2 3] [5 6 7])", "[1 2 3]");
    TEST_ASSERT_EQ("(except [10 20 30] [10 30])", "[20]");
    TEST_ASSERT_EQ("(except [1 2 3] [1 2 3])", "[]");

    // Empty cases
    TEST_ASSERT_EQ("(except [] [1 2 3])", "[]");
    TEST_ASSERT_EQ("(except [1 2 3] [])", "[1 2 3]");
    TEST_ASSERT_EQ("(except [] [])", "[]");

    // Single element except
    TEST_ASSERT_EQ("(except ['x] ['x])", "[]");
    TEST_ASSERT_EQ("(except ['x] ['y])", "[x]");
    TEST_ASSERT_EQ("(except [42] [42])", "[]");
    TEST_ASSERT_EQ("(except [42] [99])", "[42]");

    // Scalar except vector
    TEST_ASSERT_EQ("(except [1 2 3 4 5] 3)", "[1 2 4 5]");
    TEST_ASSERT_EQ("(except ['a 'b 'c] 'b)", "[a c]");
    TEST_ASSERT_EQ("(except [1 2 3] 9)", "[1 2 3]");

    // Duplicates handling
    TEST_ASSERT_EQ("(except [1 1 2 2 3] [1 3])", "[2 2]");
    TEST_ASSERT_EQ("(except ['a 'a 'b 'c 'c] ['a 'c])", "[b]");

    PASS();
}

test_result_t test_lang_or() {
    TEST_ASSERT_EQ("(or true false)", "true");
    TEST_ASSERT_EQ("(or false true)", "true");
    TEST_ASSERT_EQ("(or false false)", "false");
    TEST_ASSERT_EQ("(or true true)", "true");
    TEST_ASSERT_EQ("(or [true false true] [false true false])", "[true true true]");
    TEST_ASSERT_EQ("(or [true false true] [false true false] [true false true])", "[true true true]");
    TEST_ASSERT_EQ("(or [true false true] true)", "[true true true]");

    PASS();
}

test_result_t test_lang_and() {
    TEST_ASSERT_EQ("(and true false)", "false");
    TEST_ASSERT_EQ("(and false true)", "false");
    TEST_ASSERT_EQ("(and false false)", "false");
    TEST_ASSERT_EQ("(and true true)", "true");
    TEST_ASSERT_EQ("(and [true false true] [false true false])", "[false false false]");
    TEST_ASSERT_EQ("(and [true false true] [false true false] [true false true])", "[false false false]");
    TEST_ASSERT_EQ("(and [true false true] true)", "[true false true]");

    PASS();
}

test_result_t test_lang_bin() {
    TEST_ASSERT_EQ("(bin [1 2 3 4 5] 3)", "2");
    TEST_ASSERT_EQ("(bin [0 2 4 6 8 10] 5)", "2");
    TEST_ASSERT_EQ("(bin [0 2 4 6 8 10] [-10 0 4 5 6 20])", "[-1 0 2 2 3 5]");
    TEST_ASSERT_EQ("(bin [0 1 1 2] [0 1 2])", "[0 2 3]");
    TEST_ASSERT_EQ("(binr [0 1 1 2] [0 1 2])", "[0 1 3]");

    PASS();
}

test_result_t test_lang_timestamp() {
    // Test ISO date-only format
    TEST_ASSERT_EQ("(as 'timestamp \"2004-10-21\")", "2004.10.21D00:00:00.000000000");
    TEST_ASSERT_EQ("(as 'timestamp \"2024-03-20\")", "2024.03.20D00:00:00.000000000");
    TEST_ASSERT_EQ("(as 'timestamp \"2023-12-31\")", "2023.12.31D00:00:00.000000000");
    TEST_ASSERT_EQ("(as 'timestamp \"2025-01-01\")", "2025.01.01D00:00:00.000000000");

    // Test ISO date-time with space separator
    TEST_ASSERT_EQ("(as 'timestamp \"2004-10-21 12:00:00\")", "2004.10.21D12:00:00.000000000");
    TEST_ASSERT_EQ("(as 'timestamp \"2025-03-04 15:41:47\")", "2025.03.04D15:41:47.000000000");
    TEST_ASSERT_EQ("(as 'timestamp \"2023-06-15 09:30:45\")", "2023.06.15D09:30:45.000000000");

    // Test ISO date-time with T separator
    TEST_ASSERT_EQ("(as 'timestamp \"2004-10-21T12:00:00\")", "2004.10.21D12:00:00.000000000");
    TEST_ASSERT_EQ("(as 'timestamp \"2025-03-04T15:41:47\")", "2025.03.04D15:41:47.000000000");

    // Test ISO with fractional seconds (milliseconds)
    TEST_ASSERT_EQ("(as 'timestamp \"2004-10-21 12:00:00.010\")", "2004.10.21D12:00:00.010000000");
    TEST_ASSERT_EQ("(as 'timestamp \"2025-03-04 15:41:47.123\")", "2025.03.04D15:41:47.123000000");

    // Test ISO with fractional seconds (microseconds)
    TEST_ASSERT_EQ("(as 'timestamp \"2004-10-21 12:00:00.010500\")", "2004.10.21D12:00:00.010500000");
    TEST_ASSERT_EQ("(as 'timestamp \"2025-03-04 15:41:47.123456\")", "2025.03.04D15:41:47.123456000");

    // Test ISO with fractional seconds (nanoseconds)
    TEST_ASSERT_EQ("(as 'timestamp \"2004-10-21 12:00:00.010000000\")", "2004.10.21D12:00:00.010000000");
    TEST_ASSERT_EQ("(as 'timestamp \"2025-03-04T15:41:47.087221025\")", "2025.03.04D15:41:47.087221025");

    // Test ISO with UTC timezone (Z)
    TEST_ASSERT_EQ("(as 'timestamp \"2004-10-21T12:00:00Z\")", "2004.10.21D12:00:00.000000000");
    TEST_ASSERT_EQ("(as 'timestamp \"2025-03-04T15:41:47.123456789Z\")", "2025.03.04D15:41:47.123456789");

    // Test ISO with positive timezone offset
    // 2004-10-21 12:00:00+02:00 should convert to 10:00:00 UTC
    TEST_ASSERT_EQ("(as 'timestamp \"2004-10-21 12:00:00+02:00\")", "2004.10.21D10:00:00.000000000");
    TEST_ASSERT_EQ("(as 'timestamp \"2004-10-21T12:00:00+02:00\")", "2004.10.21D10:00:00.000000000");
    TEST_ASSERT_EQ("(as 'timestamp \"2025-03-04 15:41:47+05:30\")", "2025.03.04D10:11:47.000000000");
    TEST_ASSERT_EQ("(as 'timestamp \"2023-06-15 18:30:00+01:00\")", "2023.06.15D17:30:00.000000000");

    // Test ISO with negative timezone offset
    // 2004-10-21 12:00:00-05:00 should convert to 17:00:00 UTC
    TEST_ASSERT_EQ("(as 'timestamp \"2004-10-21 12:00:00-05:00\")", "2004.10.21D17:00:00.000000000");
    TEST_ASSERT_EQ("(as 'timestamp \"2004-10-21 12:00:00.010-23:00\")", "2004.10.22D11:00:00.010000000");
    TEST_ASSERT_EQ("(as 'timestamp \"2025-03-04T08:00:00-08:00\")", "2025.03.04D16:00:00.000000000");

    // Test ISO with timezone without colon separator
    TEST_ASSERT_EQ("(as 'timestamp \"2004-10-21 12:00:00+0200\")", "2004.10.21D10:00:00.000000000");
    TEST_ASSERT_EQ("(as 'timestamp \"2004-10-21 12:00:00-0500\")", "2004.10.21D17:00:00.000000000");

    // Test ISO with fractional seconds and timezone
    TEST_ASSERT_EQ("(as 'timestamp \"2004-10-21 12:00:00.123456+02:00\")", "2004.10.21D10:00:00.123456000");
    TEST_ASSERT_EQ("(as 'timestamp \"2025-03-04T15:41:47.087-05:00\")", "2025.03.04D20:41:47.087000000");

    // Test that rayforce format still works
    TEST_ASSERT_EQ("(as 'timestamp \"2004.10.21D12:00:00.000000000\")", "2004.10.21D12:00:00.000000000");
    TEST_ASSERT_EQ("(as 'timestamp \"2025.03.04D15:41:47.087221025\")", "2025.03.04D15:41:47.087221025");

    PASS();
}

// ==================== AGGREGATION TESTS ====================
test_result_t test_lang_aggregations() {
    // ========== SUM TESTS ==========
    TEST_ASSERT_EQ("(sum [1 2 3 4 5])", "15");
    TEST_ASSERT_EQ("(sum [1.0 2.0 3.0])", "6.0");
    TEST_ASSERT_EQ("(sum [1i 2i 3i])", "6i");
    TEST_ASSERT_EQ("(sum [])", "0");
    TEST_ASSERT_EQ("(sum 5)", "5");  // scalar

    // ========== AVG TESTS ==========
    TEST_ASSERT_EQ("(avg [1 2 3 4 5])", "3.0");
    TEST_ASSERT_EQ("(avg [10.0 20.0 30.0])", "20.0");
    TEST_ASSERT_EQ("(avg [2 4 6 8])", "5.0");
    TEST_ASSERT_EQ("(avg 10)", "10.0");  // scalar

    // ========== MIN TESTS ==========
    TEST_ASSERT_EQ("(min [5 2 8 1 9])", "1");
    TEST_ASSERT_EQ("(min [5.0 2.0 8.0])", "2.0");
    TEST_ASSERT_EQ("(min [-5 -2 -8])", "-8");
    TEST_ASSERT_EQ("(min [5i 2i 8i])", "2i");
    TEST_ASSERT_EQ("(min 42)", "42");  // scalar

    // ========== MAX TESTS ==========
    TEST_ASSERT_EQ("(max [5 2 8 1 9])", "9");
    TEST_ASSERT_EQ("(max [5.0 2.0 8.0])", "8.0");
    TEST_ASSERT_EQ("(max [-5 -2 -8])", "-2");
    TEST_ASSERT_EQ("(max [5i 2i 8i])", "8i");
    TEST_ASSERT_EQ("(max 42)", "42");  // scalar

    // ========== COUNT TESTS ==========
    TEST_ASSERT_EQ("(count [1 2 3 4 5])", "5");
    TEST_ASSERT_EQ("(count [])", "0");
    TEST_ASSERT_EQ("(count \"hello\")", "5");
    TEST_ASSERT_EQ("(count (dict [a b c] [1 2 3]))", "3");
    TEST_ASSERT_EQ("(count (table [a b] (list [1 2 3] [4 5 6])))", "3");
    TEST_ASSERT_EQ("(count 5)", "1");  // scalar

    // ========== FIRST TESTS ==========
    TEST_ASSERT_EQ("(first [1 2 3 4 5])", "1");
    TEST_ASSERT_EQ("(first \"hello\")", "'h'");
    TEST_ASSERT_EQ("(first ['a 'b 'c])", "'a");
    TEST_ASSERT_EQ("(first (table [a b] (list [1 2 3] [4 5 6])))", "{a:1 b:4}");

    // ========== LAST TESTS ==========
    TEST_ASSERT_EQ("(last [1 2 3 4 5])", "5");
    TEST_ASSERT_EQ("(last \"hello\")", "'o'");
    TEST_ASSERT_EQ("(last ['a 'b 'c])", "'c");
    TEST_ASSERT_EQ("(last (table [a b] (list [1 2 3] [4 5 6])))", "{a:3 b:6}");

    // ========== MED (MEDIAN) TESTS ==========
    TEST_ASSERT_EQ("(med [1 2 3 4 5])", "3.0");
    TEST_ASSERT_EQ("(med [1 2 3 4])", "2.5");
    TEST_ASSERT_EQ("(med [5 1 3 2 4])", "3.0");  // unsorted input

    // ========== DEV (STANDARD DEVIATION) TESTS ==========
    TEST_ASSERT_EQ("(dev [1 1 1 1])", "0.0");
    TEST_ASSERT_EQ("(< (- (dev [1 2 3 4 5]) 1.4142) 0.001)", "true");  // approx sqrt(2)

    // ========== AGGREGATIONS ON TABLES WITH GROUPBY ==========
    TEST_ASSERT_EQ(
        "(set t (table [Group Value] (list [a a b b] [10 20 30 40])))"
        "(select {from: t Total: (sum Value) by: Group})",
        "(table [Group Total] (list [a b] [30 70]))");

    TEST_ASSERT_EQ(
        "(set t (table [Group Value] (list [a a b b] [10 20 30 40])))"
        "(select {from: t Avg: (avg Value) by: Group})",
        "(table [Group Avg] (list [a b] [15.0 35.0]))");

    TEST_ASSERT_EQ(
        "(set t (table [Group Value] (list [a a b b] [10 20 30 40])))"
        "(select {from: t Min: (min Value) Max: (max Value) by: Group})",
        "(table [Group Min Max] (list [a b] [10 30] [20 40]))");

    TEST_ASSERT_EQ(
        "(set t (table [Group Value] (list [a a a b b] [1 2 3 4 5])))"
        "(select {from: t Cnt: (count Value) by: Group})",
        "(table [Group Cnt] (list [a b] [3 2]))");

    PASS();
}

// ==================== JOIN TESTS ====================
test_result_t test_lang_joins() {
    // asof-join basic
    TEST_ASSERT_EQ(
        "(set trades (table [Sym Time Price] (list [x x] [10:00:01.000 10:00:03.000] [100.0 101.0])))"
        "(set quotes (table [Sym Time Bid] (list [x x x] [10:00:00.000 10:00:02.000 10:00:04.000] [99.0 100.5 101.5])))"
        "(asof-join [Sym Time] trades quotes)",
        "(table [Sym Time Price Bid] (list [x x] [10:00:01.000 10:00:03.000] [100.0 101.0] [99.0 100.5]))");

    // asof-join single matching symbol
    TEST_ASSERT_EQ(
        "(set trades (table [Sym Time Price] (list [a] [10:00:05.000] [50.0])))"
        "(set quotes (table [Sym Time Bid] (list [a a] [10:00:01.000 10:00:03.000] [48.0 49.0])))"
        "(asof-join [Sym Time] trades quotes)",
        "(table [Sym Time Price Bid] (list [a] [10:00:05.000] [50.0] [49.0]))");

    // asof-join with exact match on boundary
    TEST_ASSERT_EQ(
        "(set trades (table [Sym Time Price] (list [a] [10:00:01.000] [50.0])))"
        "(set quotes (table [Sym Time Bid] (list [a a] [10:00:01.000 10:00:03.000] [48.0 49.0])))"
        "(asof-join [Sym Time] trades quotes)",
        "(table [Sym Time Price Bid] (list [a] [10:00:01.000] [50.0] [48.0]))");

    // asof-join with I64 + Timestamp
    TEST_ASSERT_EQ(
        "(set aj1 (table [ID Ts Val] (list [1 1 2 2] "
        "[2024.01.01D10:00:01.000000000 2024.01.01D10:00:05.000000000 2024.01.01D10:00:03.000000000 2024.01.01D10:00:07.000000000] "
        "[100 200 300 400])))"
        "(set aj2 (table [ID Ts Ref] (list [1 1 2 2] "
        "[2024.01.01D10:00:00.000000000 2024.01.01D10:00:04.000000000 2024.01.01D10:00:02.000000000 2024.01.01D10:00:06.000000000] "
        "[10 20 30 40])))"
        "(at (asof-join [ID Ts] aj1 aj2) 'Ref)",
        "[10 20 30 40]");

    // asof-join with Symbol + Date
    TEST_ASSERT_EQ(
        "(set orders (table [Cust Date Amount] (list [A A B B] [2024.01.02 2024.01.05 2024.01.03 2024.01.06] [100 200 300 400])))"
        "(set rates (table [Cust Date Rate] (list [A A B B] [2024.01.01 2024.01.04 2024.01.01 2024.01.05] [0.1 0.15 0.2 0.25])))"
        "(at (asof-join [Cust Date] orders rates) 'Rate)",
        "[0.1 0.15 0.2 0.25]");

    // left-join all rows match
    TEST_ASSERT_EQ(
        "(set t1 (table [ID Name] (list [1 3] [a c])))"
        "(set t2 (table [ID Value] (list [1 3] [100 300])))"
        "(left-join [ID] t1 t2)",
        "(table [ID Name Value] (list [1 3] [a c] [100 300]))");

    // left-join partial matches
    TEST_ASSERT_EQ(
        "(set t1 (table [ID Name] (list [1 2 3] [a b c])))"
        "(set t2 (table [ID Value] (list [1 3] [100 300])))"
        "(count (left-join [ID] t1 t2))",
        "3");

    // left-join by Date
    TEST_ASSERT_EQ(
        "(set t1 (table [dt val1] (list [2024.01.01 2024.01.02 2024.01.03] [100 200 300])))"
        "(set t2 (table [dt val2] (list [2024.01.01 2024.01.03] [1000 3000])))"
        "(count (left-join [dt] t1 t2))",
        "3");

    // left-join by Time
    TEST_ASSERT_EQ(
        "(set t1 (table [tm val1] (list [10:00:00 10:00:01 10:00:02] [100 200 300])))"
        "(set t2 (table [tm val2] (list [10:00:00 10:00:02] [1000 3000])))"
        "(count (left-join [tm] t1 t2))",
        "3");

    // inner-join basic
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 2 3 4 5] [100 200 300 400 500])))"
        "(set t2 (table [id val2] (list [1 3 5 6 7] [1000 3000 5000 6000 7000])))"
        "(count (inner-join [id] t1 t2))",
        "3");

    // inner-join returns correct right values
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 2 3 4 5] [100 200 300 400 500])))"
        "(set t2 (table [id val2] (list [1 3 5 6 7] [1000 3000 5000 6000 7000])))"
        "(at (inner-join [id] t1 t2) 'val2)",
        "[1000 3000 5000]");

    // inner-join preserves correct left values
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 2 3 4 5] [100 200 300 400 500])))"
        "(set t2 (table [id val2] (list [1 3 5 6 7] [1000 3000 5000 6000 7000])))"
        "(at (inner-join [id] t1 t2) 'val1)",
        "[100 300 500]");

    // inner-join by Date
    TEST_ASSERT_EQ(
        "(set t1 (table [dt val1] (list [2024.01.01 2024.01.02 2024.01.03] [100 200 300])))"
        "(set t2 (table [dt val2] (list [2024.01.01 2024.01.03 2024.01.05] [1000 3000 5000])))"
        "(count (inner-join [dt] t1 t2))",
        "2");

    // inner-join by Time
    TEST_ASSERT_EQ(
        "(set t1 (table [tm val1] (list [10:00:00 10:00:01 10:00:02] [100 200 300])))"
        "(set t2 (table [tm val2] (list [10:00:00 10:00:02 10:00:05] [1000 3000 5000])))"
        "(count (inner-join [tm] t1 t2))",
        "2");

    // inner-join with no matches
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 2 3] [100 200 300])))"
        "(set t2 (table [id val2] (list [4 5 6] [400 500 600])))"
        "(count (inner-join [id] t1 t2))",
        "0");

    // inner-join with all matches
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 2 3] [100 200 300])))"
        "(set t2 (table [id val2] (list [1 2 3] [1000 2000 3000])))"
        "(count (inner-join [id] t1 t2))",
        "3");

    // inner-join by Symbol
    TEST_ASSERT_EQ(
        "(set t1 (table [sym val1] (list [AAPL GOOG MSFT] [100 200 300])))"
        "(set t2 (table [sym val2] (list [AAPL MSFT TSLA] [1000 3000 5000])))"
        "(count (inner-join [sym] t1 t2))",
        "2");

    // inner-join by F64
    TEST_ASSERT_EQ(
        "(set t1 (table [price val1] (list [1.0 2.0 3.0] [100 200 300])))"
        "(set t2 (table [price val2] (list [1.0 3.0 5.0] [1000 3000 5000])))"
        "(count (inner-join [price] t1 t2))",
        "2");

    // inner-join multi-key
    TEST_ASSERT_EQ(
        "(set t1 (table [id1 id2 val1] (list [1 1 2] [a b a] [100 200 300])))"
        "(set t2 (table [id1 id2 val2] (list [1 2] [a a] [1000 3000])))"
        "(count (inner-join [id1 id2] t1 t2))",
        "2");

    // window-join
    TEST_ASSERT_EQ(
        "(set trades (table [Sym Time Price] (list [a a] [10:00:01.000 10:00:05.000] [100 200])))"
        "(set quotes (table [Sym Time Bid] (list [a a a] [10:00:00.000 10:00:02.000 10:00:04.000] [99 100 101])))"
        "(set intervals (map-left + [-2000 2000] (at trades 'Time)))"
        "(at (window-join [Sym Time] intervals trades quotes {minBid: (min Bid)}) 'minBid)",
        "[99 100]");

    // window-join1
    TEST_ASSERT_EQ(
        "(set trades (table [Sym Time Price] (list [a a] [10:00:01.000 10:00:05.000] [100 200])))"
        "(set quotes (table [Sym Time Bid] (list [a a a] [10:00:00.000 10:00:02.000 10:00:04.000] [99 100 101])))"
        "(set intervals (map-left + [-2000 2000] (at trades 'Time)))"
        "(at (window-join1 [Sym Time] intervals trades quotes {minBid: (min Bid)}) 'minBid)",
        "[99 101]");

    // window-join with raw column (TYPE_MAPGROUP)
    TEST_ASSERT_EQ(
        "(set trades (table [Sym Time Price] (list [a a] [10:00:01.000 10:00:05.000] [100 200])))"
        "(set quotes (table [Sym Time Bid] (list [a a a] [10:00:00.000 10:00:02.000 10:00:04.000] [99 100 101])))"
        "(set intervals (map-left + [-2000 2000] (at trades 'Time)))"
        "(count (at (window-join [Sym Time] intervals trades quotes {bids: Bid}) 'bids))",
        "2");

    // window-join1 with raw column (TYPE_MAPGROUP)
    TEST_ASSERT_EQ(
        "(set trades (table [Sym Time Price] (list [a a] [10:00:01.000 10:00:05.000] [100 200])))"
        "(set quotes (table [Sym Time Bid] (list [a a a] [10:00:00.000 10:00:02.000 10:00:04.000] [99 100 101])))"
        "(set intervals (map-left + [-2000 2000] (at trades 'Time)))"
        "(count (at (window-join1 [Sym Time] intervals trades quotes {bids: Bid}) 'bids))",
        "2");

    // empty left table
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list (take 0 [1]) (take 0 [1]))))"
        "(set t2 (table [id val2] (list [1 2 3] [100 200 300])))"
        "(count (left-join [id] t1 t2))",
        "0");

    // empty right table
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list [1 2 3] [100 200 300])))"
        "(set t2 (table [id val2] (list (take 0 [1]) (take 0 [1]))))"
        "(count (left-join [id] t1 t2))",
        "3");

    // empty tables for inner-join
    TEST_ASSERT_EQ(
        "(set t1 (table [id val1] (list (take 0 [1]) (take 0 [1]))))"
        "(set t2 (table [id val2] (list [1 2 3] [100 200 300])))"
        "(count (inner-join [id] t1 t2))",
        "0");

    // left-join multi-key
    TEST_ASSERT_EQ(
        "(set t1 (table [id1 id2 val1] (list [1 1 2] [a b a] [100 200 300])))"
        "(set t2 (table [id1 id2 val2] (list [1 2] [a a] [1000 3000])))"
        "(count (left-join [id1 id2] t1 t2))",
        "3");

    // inner-join with Timestamp
    TEST_ASSERT_EQ(
        "(set t1 (table [ts val1] (list [2024.01.01D10:00:00.000000000 2024.01.01D10:00:01.000000000 2024.01.01D10:00:02.000000000] [100 200 300])))"
        "(set t2 (table [ts val2] (list [2024.01.01D10:00:00.000000000 2024.01.01D10:00:02.000000000] [1000 3000])))"
        "(count (inner-join [ts] t1 t2))",
        "2");

    // asof-join no match before returns Null
    TEST_ASSERT_EQ(
        "(set trades (table [Sym Time Price] (list [a] [10:00:00.000] [100.0])))"
        "(set quotes (table [Sym Time Bid] (list [a] [10:00:05.000] [99.0])))"
        "(count (asof-join [Sym Time] trades quotes))",
        "1");

    // error: left-join wrong type
    TEST_ASSERT_ER("(left-join 123 (table [a] (list [1])) (table [a] (list [1])))", "symbol");

    // error: inner-join wrong type
    TEST_ASSERT_ER("(inner-join [a] [1 2 3] (table [a] (list [1])))", "table");

    // error: asof-join wrong arity
    TEST_ASSERT_ER("(asof-join [a b])", "asof-join");

    PASS();
}

// ==================== TEMPORAL TESTS ====================
test_result_t test_lang_temporal() {
    // ========== DATE ARITHMETIC ==========
    TEST_ASSERT_EQ("(+ 2024.01.01 1)", "2024.01.02");
    TEST_ASSERT_EQ("(+ 2024.01.01 30)", "2024.01.31");
    TEST_ASSERT_EQ("(+ 2024.01.01 31)", "2024.02.01");
    TEST_ASSERT_EQ("(- 2024.01.10 5)", "2024.01.05");
    TEST_ASSERT_EQ("(- 2024.02.01 2024.01.01)", "31");

    // ========== TIME ARITHMETIC ==========
    TEST_ASSERT_EQ("(+ 10:00:00.000 1000)", "10:00:01.000");
    TEST_ASSERT_EQ("(+ 10:00:00.000 60000)", "10:01:00.000");
    TEST_ASSERT_EQ("(+ 10:00:00.000 3600000)", "11:00:00.000");
    TEST_ASSERT_EQ("(- 10:00:01.000 1000)", "10:00:00.000");
    TEST_ASSERT_EQ("(- 10:00:01.000 10:00:00.000)", "00:00:01.000");

    // ========== TIMESTAMP ARITHMETIC ==========
    TEST_ASSERT_EQ("(+ 2024.01.01D10:00:00.000000000 1000000000)", "2024.01.01D10:00:01.000000000");
    TEST_ASSERT_EQ("(- 2024.01.01D10:00:01.000000000 1000000000)", "2024.01.01D10:00:00.000000000");

    // ========== VECTOR OPERATIONS ==========
    TEST_ASSERT_EQ("(+ [2024.01.01 2024.01.02] 1)", "[2024.01.02 2024.01.03]");
    TEST_ASSERT_EQ("(- [2024.01.10 2024.01.20] [2024.01.01 2024.01.10])", "[9 10]");

    PASS();
}

// ==================== ITERATION TESTS ====================
test_result_t test_lang_iteration() {
    // ========== MAP-LEFT TESTS ==========
    TEST_ASSERT_EQ("(map-left - 10 [1 2 3])", "[9 8 7]");
    TEST_ASSERT_EQ("(map-left / 100 [2 4 5])", "[50 25 20]");

    // ========== MAP-RIGHT TESTS ==========
    TEST_ASSERT_EQ("(map-right - [10 20 30] 5)", "[5 15 25]");
    TEST_ASSERT_EQ("(map-right / [10 20 30] 2)", "[5 10 15]");

    PASS();
}

// ==================== CONDITIONAL TESTS ====================
test_result_t test_lang_conditionals() {
    // ========== BASIC IF TESTS ==========
    TEST_ASSERT_EQ("(if true 1 2)", "1");
    TEST_ASSERT_EQ("(if false 1 2)", "2");

    // ========== NESTED IF ==========
    TEST_ASSERT_EQ("(if true (if true 1 2) 3)", "1");
    TEST_ASSERT_EQ("(if true (if false 1 2) 3)", "2");
    TEST_ASSERT_EQ("(if false 1 (if true 2 3))", "2");

    // ========== IF WITH EXPRESSIONS ==========
    TEST_ASSERT_EQ("(if (> 5 3) (+ 1 1) (- 1 1))", "2");
    TEST_ASSERT_EQ("(if (< 5 3) (+ 1 1) (- 5 3))", "2");
    TEST_ASSERT_EQ("(if (== 1 1) \"yes\" \"no\")", "\"yes\"");

    // ========== IF WITH SIDE EFFECTS ==========
    TEST_ASSERT_EQ("(set y 0) (if true (set y 10) (set y 20)) y", "10");
    TEST_ASSERT_EQ("(set y 0) (if false (set y 10) (set y 20)) y", "20");

    // ========== CHAINED CONDITIONS ==========
    TEST_ASSERT_EQ("(set x 5) (if (< x 0) 'neg (if (== x 0) 'zero 'pos))", "'pos");
    TEST_ASSERT_EQ("(set x -3) (if (< x 0) 'neg (if (== x 0) 'zero 'pos))", "'neg");
    TEST_ASSERT_EQ("(set x 0) (if (< x 0) 'neg (if (== x 0) 'zero 'pos))", "'zero");

    PASS();
}

// ==================== DICT TESTS ====================
test_result_t test_lang_dict() {
    // ========== DICT CREATION ==========
    TEST_ASSERT_EQ("(dict [a b c] [1 2 3])", "{a:1 b:2 c:3}");
    TEST_ASSERT_EQ("(dict [x y] [10.0 20.0])", "{x:10.0 y:20.0}");
    TEST_ASSERT_EQ("(dict [] [])", "{}");

    // ========== DICT ACCESS ==========
    TEST_ASSERT_EQ("(set d (dict [a b c] [1 2 3])) (at d 'a)", "1");
    TEST_ASSERT_EQ("(set d (dict [a b c] [1 2 3])) (at d 'b)", "2");
    TEST_ASSERT_EQ("(set d (dict [a b c] [1 2 3])) (at d 'c)", "3");
    TEST_ASSERT_EQ("(set d (dict [a b c] [1 2 3])) (at d 'd)", "0Nl");  // missing key

    // ========== KEY FUNCTION ==========
    TEST_ASSERT_EQ("(key (dict [a b c] [1 2 3]))", "[a b c]");
    TEST_ASSERT_EQ("(key (dict [x] [10]))", "[x]");
    TEST_ASSERT_EQ("(key (table [a b] (list [1 2] [3 4])))", "[a b]");

    // ========== VALUE FUNCTION ==========
    TEST_ASSERT_EQ("(value (dict [a b c] [1 2 3]))", "[1 2 3]");
    TEST_ASSERT_EQ("(value (dict [x] [10]))", "[10]");
    TEST_ASSERT_EQ("(value (table [a b] (list [1 2] [3 4])))", "(list [1 2] [3 4])");

    // ========== NESTED DICT ==========
    TEST_ASSERT_EQ("(set d (dict [a b] (list 1 (dict [x y] [10 20])))) (at (at d 'b) 'x)", "10");

    // ========== DICT IN TABLE ROW ==========
    TEST_ASSERT_EQ("(first (table [a b] (list [1 2] [3 4])))", "{a:1 b:3}");
    TEST_ASSERT_EQ("(at (first (table [a b] (list [1 2] [3 4]))) 'a)", "1");

    PASS();
}

// ==================== LIST TESTS ====================
test_result_t test_lang_list() {
    // ========== LIST CREATION ==========
    TEST_ASSERT_EQ("(list 1 2 3)", "(list 1 2 3)");
    TEST_ASSERT_EQ("(list [1 2] [3 4])", "(list [1 2] [3 4])");
    TEST_ASSERT_EQ("(list 1 \"hello\" 'a)", "(list 1 \"hello\" 'a)");

    // ========== ENLIST ==========
    TEST_ASSERT_EQ("(enlist 5)", "[5]");
    TEST_ASSERT_EQ("(enlist 'a)", "['a]");
    TEST_ASSERT_EQ("(enlist \"hello\")", "(list \"hello\")");
    TEST_ASSERT_EQ("(enlist [1 2 3])", "(list [1 2 3])");

    // ========== AT (INDEXING) ==========
    TEST_ASSERT_EQ("(at [10 20 30 40] 0)", "10");
    TEST_ASSERT_EQ("(at [10 20 30 40] 2)", "30");
    TEST_ASSERT_EQ("(at [10 20 30 40] 3)", "40");
    TEST_ASSERT_EQ("(at [10 20 30 40] [0 2])", "[10 30]");  // multiple indices
    TEST_ASSERT_EQ("(at \"hello\" 1)", "'e'");
    TEST_ASSERT_EQ("(at \"hello\" [0 4])", "\"ho\"");

    // ========== AT ON TABLES ==========
    TEST_ASSERT_EQ("(at (table [a b] (list [1 2 3] [4 5 6])) 0)", "{a:1 b:4}");
    TEST_ASSERT_EQ("(at (table [a b] (list [1 2 3] [4 5 6])) 'a)", "[1 2 3]");

    // ========== TIL ==========
    TEST_ASSERT_EQ("(til 5)", "[0 1 2 3 4]");
    TEST_ASSERT_EQ("(til 0)", "[]");
    TEST_ASSERT_EQ("(til 1)", "[0]");

    // ========== TAKE ==========
    TEST_ASSERT_EQ("(take 3 [1 2 3 4 5])", "[1 2 3]");
    TEST_ASSERT_EQ("(take -3 [1 2 3 4 5])", "[3 4 5]");
    TEST_ASSERT_EQ("(take 7 [1 2 3])", "[1 2 3 1 2 3 1]");  // cyclic
    TEST_ASSERT_EQ("(take 3 \"hello\")", "\"hel\"");

    PASS();
}

// ==================== ALTER TESTS ====================
test_result_t test_lang_alter() {
    // ========== ALTER SET ON VECTORS ==========
    TEST_ASSERT_EQ("(set v [1 2 3 4 5]) (alter 'v set 0 100) v", "[100 2 3 4 5]");

    // ========== ALTER CONCAT ON VECTORS ==========
    TEST_ASSERT_EQ("(set v [1 2 3]) (alter 'v concat 4) v", "[1 2 3 4]");

    PASS();
}

// ==================== NULL HANDLING TESTS ====================
test_result_t test_lang_null() {
    // ========== NIL? TESTS ==========
    TEST_ASSERT_EQ("(nil? null)", "true");
    TEST_ASSERT_EQ("(nil? 0Nl)", "true");
    TEST_ASSERT_EQ("(nil? 0)", "false");
    TEST_ASSERT_EQ("(nil? 1)", "false");
    TEST_ASSERT_EQ("(nil? \"\")", "false");

    // ========== NULL PROPAGATION ==========
    TEST_ASSERT_EQ("(+ 1 0Nl)", "0Nl");
    TEST_ASSERT_EQ("(* 5 0Nl)", "0Nl");
    TEST_ASSERT_EQ("(+ [1 2 3] [0Nl 2 3])", "[0Nl 4 6]");

    // ========== NULL COMPARISONS ==========
    TEST_ASSERT_EQ("(== 0Nl 0Nl)", "true");
    TEST_ASSERT_EQ("(== null null)", "true");

    // ========== NULL IN TABLES ==========
    TEST_ASSERT_EQ(
        "(set t (table [a b] (list [1 0Nl 3] [4 5 6])))"
        "(at (at t 'a) 1)",
        "0Nl");

    PASS();
}

// ==================== SET OPERATIONS TESTS ====================
test_result_t test_lang_set_ops() {
    // ========== UNION TESTS ==========
    TEST_ASSERT_EQ("(union [1 2 3] [3 4 5])", "[1 2 3 4 5]");
    TEST_ASSERT_EQ("(union [1 2 3] [1 2 3])", "[1 2 3]");
    TEST_ASSERT_EQ("(union [] [1 2 3])", "[1 2 3]");
    TEST_ASSERT_EQ("(union [1 2 3] [])", "[1 2 3]");
    TEST_ASSERT_EQ("(union ['a 'b] ['b 'c])", "[a b c]");

    // ========== SECT (INTERSECTION) TESTS ==========
    TEST_ASSERT_EQ("(sect [1 2 3 4] [2 4 6])", "[2 4]");
    TEST_ASSERT_EQ("(sect [1 2 3] [4 5 6])", "[]");
    TEST_ASSERT_EQ("(sect [1 2 3] [1 2 3])", "[1 2 3]");
    TEST_ASSERT_EQ("(sect ['a 'b 'c] ['b 'c 'd])", "[b c]");

    // ========== WITHIN TESTS ==========
    TEST_ASSERT_EQ("(within [5] [1 10])", "[true]");
    TEST_ASSERT_EQ("(within [0] [1 10])", "[false]");
    TEST_ASSERT_EQ("(within [11] [1 10])", "[false]");
    TEST_ASSERT_EQ("(within [5 0 15] [1 10])", "[true false false]");

    PASS();
}

// ==================== TYPE CASTING TESTS ====================
test_result_t test_lang_cast() {
    // ========== STRING/SYMBOL CASTS ==========
    TEST_ASSERT_EQ("(as 'String 'hello)", "\"hello\"");
    TEST_ASSERT_EQ("(as 'String 123)", "\"123\"");

    // ========== GUID CASTS ==========
    TEST_ASSERT_EQ("(type (as 'guid \"d49f18a4-1969-49e8-9b8a-6bb9a4832eea\"))", "'guid");

    PASS();
}

// ==================== LAMBDA TESTS ====================
test_result_t test_lang_lambda() {
    // ========== BASIC LAMBDA ==========
    TEST_ASSERT_EQ("((fn [x] (+ x 1)) 5)", "6");
    TEST_ASSERT_EQ("((fn [x y] (+ x y)) 3 4)", "7");
    TEST_ASSERT_EQ("((fn [] 42))", "42");

    // ========== LAMBDA WITH MULTIPLE ARGS ==========
    TEST_ASSERT_EQ("((fn [a b c] (+ a (+ b c))) 1 2 3)", "6");
    TEST_ASSERT_EQ("((fn [x y z] (* x (* y z))) 2 3 4)", "24");

    // ========== STORED LAMBDA ==========
    TEST_ASSERT_EQ("(set f (fn [x] (* x x))) (f 5)", "25");
    TEST_ASSERT_EQ("(set add (fn [a b] (+ a b))) (add 10 20)", "30");

    // ========== LAMBDA WITH MAP ==========
    TEST_ASSERT_EQ("(map (fn [x] (* x 2)) [1 2 3 4 5])", "[2 4 6 8 10]");
    TEST_ASSERT_EQ("(map (fn [x] (+ x 10)) [0 1 2])", "[10 11 12]");

    // ========== LAMBDA WITH FILTER/WHERE ==========
    TEST_ASSERT_EQ("(filter [1 2 3 4 5 6] (map (fn [x] (> x 3)) [1 2 3 4 5 6]))", "[4 5 6]");

    // ========== LAMBDA WITH CONDITIONALS ==========
    TEST_ASSERT_EQ("((fn [x] (if (> x 0) 'pos 'neg)) 5)", "'pos");
    TEST_ASSERT_EQ("((fn [x] (if (> x 0) 'pos 'neg)) -5)", "'neg");

    // ========== RECURSIVE-STYLE (SELF) ==========
    TEST_ASSERT_EQ("(set factorial (fn [n] (if (<= n 1) 1 (* n (factorial (- n 1)))))) (factorial 5)", "120");

    PASS();
}

// ==================== GROUP TESTS ====================
test_result_t test_lang_group() {
    // ========== BASIC GROUP ==========
    TEST_ASSERT_EQ("(group [a a b b c])", "{a:[0 1] b:[2 3] c:[4]}");
    TEST_ASSERT_EQ("(group [1 1 2 2 3])", "{1:[0 1] 2:[2 3] 3:[4]}");
    TEST_ASSERT_EQ("(group [true false true false])", "{true:[0 2] false:[1 3]}");

    // ========== GROUP ON EMPTY ==========
    TEST_ASSERT_EQ("(group [])", "{}");

    // ========== GROUP WITH UNIQUE VALUES ==========
    TEST_ASSERT_EQ("(group [a b c])", "{a:[0] b:[1] c:[2]}");

    // ========== SELECT WITH BY (GROUPBY) ==========
    TEST_ASSERT_EQ(
        "(set t (table [Category Value] (list [a a b b] [10 20 30 40])))"
        "(select {from: t Sum: (sum Value) by: Category})",
        "(table [Category Sum] (list [a b] [30 70]))");

    TEST_ASSERT_EQ(
        "(set t (table [Category Value] (list [a a b b] [10 20 30 40])))"
        "(select {from: t Count: (count Value) by: Category})",
        "(table [Category Count] (list [a b] [2 2]))");

    // ========== UPDATE WITH BY ==========
    TEST_ASSERT_EQ(
        "(set t (table [Group Value] (list [a a b b] [10 20 30 40])))"
        "(update {from: 't GroupSum: (sum Value) by: Group})"
        "t",
        "(table [Group Value GroupSum] (list [a a b b] [10 20 30 40] [30 30 70 70]))");

    // ========== GROUP BY BOOL ==========
    TEST_ASSERT_EQ(
        "(set t (table [Flag Value] (list [true false true false] [10 20 30 40])))"
        "(select {from: t Sum: (sum Value) by: Flag})",
        "(table [Flag Sum] (list [true false] [40 60]))");

    // ========== GROUP BY STRING COLUMN ==========
    // Tests for grouping by list of strings (TYPE_LIST of TYPE_C8)
    // This tests the fix for parallel radix partitioning bug where identical strings
    // at different memory locations were incorrectly counted as different groups

    // Basic group by string list
    TEST_ASSERT_EQ("(group (list \"apple\" \"banana\" \"apple\" \"cherry\" \"banana\"))",
                   "{\"apple\":[0 2] \"banana\":[1 4] \"cherry\":[3]}");

    // Select with group by string column
    TEST_ASSERT_EQ(
        "(set t (table [Name Value] (list (list \"alice\" \"bob\" \"alice\" \"bob\") [10 20 30 40])))"
        "(select {from: t Sum: (sum Value) by: Name})",
        "(table [Name Sum] (list (list \"alice\" \"bob\") [40 60]))");

    // Count by string column
    TEST_ASSERT_EQ(
        "(set t (table [City Population] (list (list \"NYC\" \"LA\" \"NYC\" \"LA\" \"NYC\") [100 200 150 250 120])))"
        "(select {from: t Total: (sum Population) Count: (count Population) by: City})",
        "(table [City Total Count] (list (list \"NYC\" \"LA\") [370 450] [3 2]))");

    // Group by string with more unique values (to test parallel partitioning)
    TEST_ASSERT_EQ(
        "(set t (table [Category Amount] (list "
        "(list \"cat1\" \"cat2\" \"cat3\" \"cat1\" \"cat2\" \"cat3\" \"cat1\" \"cat2\") "
        "[10 20 30 40 50 60 70 80])))"
        "(select {from: t Sum: (sum Amount) by: Category})",
        "(table [Category Sum] (list (list \"cat1\" \"cat2\" \"cat3\") [120 150 90]))");

    // Update with group by string
    TEST_ASSERT_EQ(
        "(set t (table [Type Value] (list (list \"A\" \"B\" \"A\" \"B\") [10 20 30 40])))"
        "(update {from: 't TypeSum: (sum Value) by: Type})"
        "t",
        "(table [Type Value TypeSum] (list (list \"A\" \"B\" \"A\" \"B\") [10 20 30 40] [40 60 40 60]))");

    PASS();
}

// ==================== FIND TESTS ====================
test_result_t test_lang_find() {
    // ========== FIND IN VECTOR ==========
    TEST_ASSERT_EQ("(find [10 20 30 40] 30)", "2");
    TEST_ASSERT_EQ("(find [10 20 30 40] 50)", "0Nl");  // not found
    TEST_ASSERT_EQ("(find [10 20 30 40] 10)", "0");
    TEST_ASSERT_EQ("(find ['a 'b 'c] 'b)", "1");
    TEST_ASSERT_EQ("(find \"hello\" 'l')", "2");

    // ========== FIND MULTIPLE ==========
    TEST_ASSERT_EQ("(find [10 20 30 40] [20 40])", "[1 3]");
    TEST_ASSERT_EQ("(find ['a 'b 'c 'd] ['c 'a])", "[2 0]");
    TEST_ASSERT_EQ("(find [1 2 3] [4 2 5])", "[0Nl 1 0Nl]");

    // ========== FIND IN EMPTY ARRAY ==========
    TEST_ASSERT_EQ("(find [] 1)", "0Nl");
    TEST_ASSERT_EQ("(find [] [1 2 3])", "[]");  // Returns empty when source is empty

    // ========== FIND WITH DIFFERENT TYPE WIDTHS (index_find_i8) ==========
    TEST_ASSERT_EQ("(find \"abc\" 'b')", "1");
    TEST_ASSERT_EQ("(find \"abc\" 'z')", "0Nl");
    TEST_ASSERT_EQ("(find \"\" 'a')", "0Nl");

    // ========== FIND WITH i64 (index_find_i64) ==========
    TEST_ASSERT_EQ("(find [1000000000 2000000000 3000000000] 2000000000)", "1");
    TEST_ASSERT_EQ("(find [1000000000 2000000000] 9999999999)", "0Nl");

    // ========== FIND WITH SYMBOLS ==========
    TEST_ASSERT_EQ("(find ['apple 'banana 'cherry] 'banana)", "1");
    TEST_ASSERT_EQ("(find ['a] 'a)", "0");
    TEST_ASSERT_EQ("(find ['a] 'b)", "0Nl");

    PASS();
}

// ==================== RANDOM TESTS ====================
test_result_t test_lang_rand() {
    // ========== BASIC RAND ==========
    TEST_ASSERT_EQ("(count (rand 10 100))", "10");  // 10 random values in [0,100)
    TEST_ASSERT_EQ("(type (rand 5 100))", "'I64");  // int random

    // ========== VERIFY RANGE ==========
    TEST_ASSERT_EQ("(and (>= (min (rand 100 10)) 0) (< (max (rand 100 10)) 10))", "true");

    PASS();
}

// ==================== NEG/NOT/WHERE TESTS ====================
test_result_t test_lang_unary_ops() {
    // ========== NEG TESTS ==========
    TEST_ASSERT_EQ("(neg 5)", "-5");
    TEST_ASSERT_EQ("(neg -5)", "5");
    TEST_ASSERT_EQ("(neg [1 -2 3 -4])", "[-1 2 -3 4]");
    TEST_ASSERT_EQ("(neg 5.0)", "-5.0");

    // ========== NOT TESTS ==========
    TEST_ASSERT_EQ("(not true)", "false");
    TEST_ASSERT_EQ("(not false)", "true");
    TEST_ASSERT_EQ("(not [true false true])", "[false true false]");

    // ========== WHERE TESTS ==========
    TEST_ASSERT_EQ("(where [true false true false true])", "[0 2 4]");
    TEST_ASSERT_EQ("(where [false false false])", "[]");
    TEST_ASSERT_EQ("(where [true true true])", "[0 1 2]");
    TEST_ASSERT_EQ("(where (> [1 2 3 4 5] 3))", "[3 4]");

    PASS();
}

// ==================== STRING OPERATIONS TESTS ====================
test_result_t test_lang_string_ops() {
    // ========== STRING CONCAT ==========
    TEST_ASSERT_EQ("(concat \"hel\" \"lo\")", "\"hello\"");
    TEST_ASSERT_EQ("(concat \"\" \"test\")", "\"test\"");
    TEST_ASSERT_EQ("(concat \"test\" \"\")", "\"test\"");

    // ========== STRING COUNT ==========
    TEST_ASSERT_EQ("(count \"hello\")", "5");
    TEST_ASSERT_EQ("(count \"\")", "0");

    // ========== STRING INDEXING ==========
    TEST_ASSERT_EQ("(at \"hello\" 0)", "'h'");
    TEST_ASSERT_EQ("(at \"hello\" 4)", "'o'");

    // ========== STRING TAKE ==========
    TEST_ASSERT_EQ("(take 3 \"hello\")", "\"hel\"");
    TEST_ASSERT_EQ("(take -2 \"hello\")", "\"lo\"");

    // ========== STRING FIRST/LAST ==========
    TEST_ASSERT_EQ("(first \"hello\")", "'h'");
    TEST_ASSERT_EQ("(last \"hello\")", "'o'");

    PASS();
}

// ==================== DO/LET TESTS ====================
test_result_t test_lang_do_let() {
    // ========== DO TESTS ==========
    TEST_ASSERT_EQ("(do (set x 1) (set y 2) (+ x y))", "3");
    TEST_ASSERT_EQ("(do 1 2 3)", "3");  // returns last

    PASS();
}

// ==================== TRY/RAISE TESTS ====================
test_result_t test_lang_error() {
    // ========== TRY WITH SUCCESS ==========
    TEST_ASSERT_EQ("(try (+ 1 2) (fn [e] 0))", "3");

    // ========== TRY WITH ERROR ==========
    TEST_ASSERT_EQ("(try (raise \"error\") (fn [e] 99))", "99");

    // ========== NESTED TRY ==========
    TEST_ASSERT_EQ("(try (try (raise \"inner\") (fn [e] (raise \"outer\"))) (fn [e] 42))", "42");

    PASS();
}

// ==================== SAFETY/EDGE CASE TESTS ====================
// These tests verify that invalid inputs return proper errors instead of segfaulting
test_result_t test_lang_safety() {
    // ========== TIL NEGATIVE LENGTH ==========
    TEST_ASSERT_ER("(til -1)", "non-negative length");
    TEST_ASSERT_ER("(til -100)", "non-negative length");

    // ========== RAND NEGATIVE/ZERO ==========
    TEST_ASSERT_ER("(rand -1 10)", "non-negative count");
    TEST_ASSERT_ER("(rand 5 0)", "positive upper bound");
    TEST_ASSERT_ER("(rand 5 -1)", "positive upper bound");

    // ========== MODIFY WRONG ARITY ==========
    TEST_ASSERT_ER("(do (set v [1 2]) (modify 'v * 2))", "expected at least 4 arguments");

    // ========== OUT OF BOUNDS ACCESS ==========
    TEST_ASSERT_ER("(do (set v [1 2 3]) (alter 'v set -10 0))", "out of range");

    // ========== NULL OPERATIONS ==========
    TEST_ASSERT_ER("(+ null 1)", "unsupported type");
    TEST_ASSERT_ER("(sum null)", "unsupported type");

    // ========== VALID EDGE CASES (should not crash) ==========
    TEST_ASSERT_EQ("(til 0)", "[]");
    TEST_ASSERT_EQ("(rand 0 10)", "[]");
    TEST_ASSERT_EQ("(at [] 0)", "0Nl");
    TEST_ASSERT_EQ("(first [])", "0Nl");
    TEST_ASSERT_EQ("(last [])", "0Nl");
    TEST_ASSERT_EQ("(/ 1 0)", "0Nl");
    TEST_ASSERT_EQ("(group [])", "{}");

    PASS();
}
