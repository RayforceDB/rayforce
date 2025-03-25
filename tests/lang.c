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

#define TEST_ASSERT_EQ(lhs, rhs)                                                                                       \
    {                                                                                                                  \
        obj_p le = eval_str(lhs);                                                                                      \
        obj_p lns = obj_fmt(le, B8_TRUE);                                                                              \
        if (IS_ERROR(le)) {                                                                                            \
            obj_p fmt = str_fmt(-1, "Input error: %s\n -- at: %s:%d", AS_C8(lns), __FILE__, __LINE__);                 \
            TEST_ASSERT(0, AS_C8(lns));                                                                                \
            drop_obj(lns);                                                                                             \
            drop_obj(fmt);                                                                                             \
        } else {                                                                                                       \
            obj_p re = eval_str(rhs);                                                                                  \
            obj_p rns = obj_fmt(re, B8_TRUE);                                                                          \
            obj_p fmt = str_fmt(-1, "Expected %s, got %s\n -- at: %s:%d", AS_C8(rns), AS_C8(lns), __FILE__, __LINE__); \
            TEST_ASSERT(str_cmp(AS_C8(lns), lns->len, AS_C8(rns), rns->len) == 0, AS_C8(fmt));                         \
            drop_obj(fmt);                                                                                             \
            drop_obj(re);                                                                                              \
            drop_obj(le);                                                                                              \
            drop_obj(lns);                                                                                             \
            drop_obj(rns);                                                                                             \
        }                                                                                                              \
    }

#define TEST_ASSERT_ER(lhs, rhs)                                                                                \
    {                                                                                                           \
        obj_p le = eval_str(lhs);                                                                               \
        obj_p lns = obj_fmt(le, B8_TRUE);                                                                       \
        if (!IS_ERROR(le)) {                                                                                    \
            obj_p fmt = str_fmt(-1, "Expected error: %s\n -- at: %s:%d", AS_C8(lns), __FILE__, __LINE__);       \
            TEST_ASSERT(0, AS_C8(lns));                                                                         \
            drop_obj(lns);                                                                                      \
            drop_obj(fmt);                                                                                      \
            drop_obj(le);                                                                                       \
        } else {                                                                                                \
            lit_p err_text = AS_C8(lns);                                                                        \
            if (err_text == NULL || strstr(err_text, rhs) == NULL) {                                            \
                obj_p fmt =                                                                                     \
                    str_fmt(-1, "Expect \"%s\", in: \"%s\"\n -- at: %s:%d", rhs, err_text, __FILE__, __LINE__); \
                TEST_ASSERT(0, AS_C8(fmt));                                                                     \
                drop_obj(fmt);                                                                                  \
            }                                                                                                   \
            drop_obj(le);                                                                                       \
            drop_obj(lns);                                                                                      \
        }                                                                                                       \
    }

test_result_t test_lang_basic() {
    TEST_ASSERT_EQ("null", "null");
    TEST_ASSERT_EQ("0x1a", "0x1a");
    TEST_ASSERT_EQ("[0x1a 0x1b]", "[0x1a 0x1b]");
    TEST_ASSERT_EQ("true", "true");
    TEST_ASSERT_EQ("false", "false");
    TEST_ASSERT_EQ("1", "1");
    TEST_ASSERT_EQ("1.1", "1.10");
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
    TEST_ASSERT_EQ("(+ 0i 0Ni)", "0i");
    TEST_ASSERT_EQ("(+ 0Ni -1i)", "-1i");
    TEST_ASSERT_EQ("(+ 0Nl 0Nl)", "0Nl");
    TEST_ASSERT_EQ("(+ 0 0Nl)", "0i");
    TEST_ASSERT_EQ("(+ 0Ni -1i)", "-1i");
    TEST_ASSERT_EQ("(+ 0Ni -10.00)", "-10.00")
    TEST_ASSERT_EQ("(+ 0Ni 0Nl)", "0Nl");
    TEST_ASSERT_EQ("(+ 0Nf 0Ni)", "0Nf");
    TEST_ASSERT_EQ("(+ 0Nf 5)", "5.0");
    TEST_ASSERT_EQ("(+ 0.00 0Ni)", "0.0")
    TEST_ASSERT_EQ("(+ 0Ni -0.00)", "0.00")
    TEST_ASSERT_EQ("(+ -0.00 0Nl)", "0.00")
    TEST_ASSERT_EQ("(+ 0Nf [-0.00])", "[0.00]")
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
    TEST_ASSERT_EQ("(- -0.00 0Nf)", "0.00")

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
    TEST_ASSERT_EQ("(/ -5 6)", "0");
    TEST_ASSERT_EQ("(/ -5 -5i)", "1");
    TEST_ASSERT_EQ("(/ -5 0Ni)", "0Nl");
    TEST_ASSERT_EQ("(/ -5 0i)", "0Nl");
    TEST_ASSERT_EQ("(/ -5 6i)", "0");
    TEST_ASSERT_EQ("(/ -5 -2.00)", "2");
    TEST_ASSERT_EQ("(/ -5 -1.00)", "5");
    TEST_ASSERT_EQ("(/ -5 -0.60)", "8");
    TEST_ASSERT_EQ("(/ -5 -0.00)", "0Nl");
    TEST_ASSERT_EQ("(/ -5 0Nf)", "0Nl");
    TEST_ASSERT_EQ("(/ -5 0.00)", "0Nl");
    TEST_ASSERT_EQ("(/ -5 0.60)", "-8");
    TEST_ASSERT_EQ("(/ -5 1.00)", "-5");
    TEST_ASSERT_EQ("(/ -5 3.00)", "-1");
    TEST_ASSERT_EQ("(/ -2 -5)", "0");
    TEST_ASSERT_EQ("(/ -2 -2)", "1");
    TEST_ASSERT_EQ("(/ -2 0Nl)", "0Nl");
    TEST_ASSERT_EQ("(/ -2 0)", "0Nl");
    TEST_ASSERT_EQ("(/ -2 6)", "0");
    TEST_ASSERT_EQ("(/ -2 -5i)", "0");
    TEST_ASSERT_EQ("(/ -2 0Ni)", "0Nl");
    TEST_ASSERT_EQ("(/ -2 0i)", "0Nl");
    TEST_ASSERT_EQ("(/ -2 6i)", "0");
    TEST_ASSERT_EQ("(/ -2 -2.00)", "1");
    TEST_ASSERT_EQ("(/ -2 -1.00)", "2");
    TEST_ASSERT_EQ("(/ -2 -0.60)", "3");
    TEST_ASSERT_EQ("(/ -2 -0.00)", "0Nl");
    TEST_ASSERT_EQ("(/ -2 0Nf)", "0Nl");
    TEST_ASSERT_EQ("(/ -2 0.00)", "0Nl");
    TEST_ASSERT_EQ("(/ -2 0.60)", "-3");
    TEST_ASSERT_EQ("(/ -2 1.00)", "-2");
    TEST_ASSERT_EQ("(/ -2 3.00)", "0");
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
    TEST_ASSERT_EQ("(/ 6 -5)", "-1");
    TEST_ASSERT_EQ("(/ 6 -2)", "-3");
    TEST_ASSERT_EQ("(/ 6 0Nl)", "0Nl");
    TEST_ASSERT_EQ("(/ 6 0)", "0Nl");
    TEST_ASSERT_EQ("(/ 6 6)", "1");
    TEST_ASSERT_EQ("(/ 6 -5i)", "-1");
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
    TEST_ASSERT_EQ("(/ -5i 6)", "0");
    TEST_ASSERT_EQ("(/ -5i -5i)", "1");
    TEST_ASSERT_EQ("(/ -5i 0Ni)", "0Ni");
    TEST_ASSERT_EQ("(/ -5i 0i)", "0Ni");
    TEST_ASSERT_EQ("(/ -5i 6i)", "0");
    TEST_ASSERT_EQ("(/ -5i -2.00)", "2");
    TEST_ASSERT_EQ("(/ -5i -1.00)", "5");
    TEST_ASSERT_EQ("(/ -5i -0.60)", "8");
    TEST_ASSERT_EQ("(/ -5i -0.00)", "0Ni");
    TEST_ASSERT_EQ("(/ -5i 0Nf)", "0Ni");
    TEST_ASSERT_EQ("(/ -5i 0.00)", "0Ni");
    TEST_ASSERT_EQ("(/ -5i 0.60)", "-8");
    TEST_ASSERT_EQ("(/ -5i 1.00)", "-5");
    TEST_ASSERT_EQ("(/ -5i 3.00)", "-1");
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
    TEST_ASSERT_EQ("(/ 6i -5)", "-1");
    TEST_ASSERT_EQ("(/ 6i -2)", "-3");
    TEST_ASSERT_EQ("(/ 6i 0Nl)", "0Ni");
    TEST_ASSERT_EQ("(/ 6i 0)", "0Ni");
    TEST_ASSERT_EQ("(/ 6i 6)", "1");
    TEST_ASSERT_EQ("(/ 6i -5i)", "-1");
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
    TEST_ASSERT_EQ("(/ -2.00 6)", "0.00");
    TEST_ASSERT_EQ("(/ -2.00 -5i)", "0.00");
    TEST_ASSERT_EQ("(/ -2.00 0Ni)", "0Nf");
    TEST_ASSERT_EQ("(/ -2.00 0i)", "0Nf");
    TEST_ASSERT_EQ("(/ -2.00 6i)", "0.00");
    TEST_ASSERT_EQ("(/ -2.00 -2.00)", "1.00");
    TEST_ASSERT_EQ("(/ -2.00 -1.00)", "2.00");
    TEST_ASSERT_EQ("(/ -2.00 -0.60)", "3.00");
    TEST_ASSERT_EQ("(/ -2.00 -0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ -2.00 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(/ -2.00 0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ -2.00 0.60)", "-3.00");
    TEST_ASSERT_EQ("(/ -2.00 1.00)", "-2.00");
    TEST_ASSERT_EQ("(/ -2.00 3.00)", "0.00");
    TEST_ASSERT_EQ("(/ -1.00 -5)", "0.00");
    TEST_ASSERT_EQ("(/ -1.00 -2)", "0.00");
    TEST_ASSERT_EQ("(/ -1.00 0Nl)", "0Nf");
    TEST_ASSERT_EQ("(/ -1.00 0)", "0Nf");
    TEST_ASSERT_EQ("(/ -1.00 6)", "0.00");
    TEST_ASSERT_EQ("(/ -1.00 -5i)", "0.00");
    TEST_ASSERT_EQ("(/ -1.00 0Ni)", "0Nf");
    TEST_ASSERT_EQ("(/ -1.00 0i)", "0Nf");
    TEST_ASSERT_EQ("(/ -1.00 6i)", "0.00");
    TEST_ASSERT_EQ("(/ -1.00 -2.00)", "0.00");
    TEST_ASSERT_EQ("(/ -1.00 -1.00)", "1.00");
    TEST_ASSERT_EQ("(/ -1.00 -0.60)", "1.00");
    TEST_ASSERT_EQ("(/ -1.00 -0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ -1.00 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(/ -1.00 0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ -1.00 0.60)", "-1.00");
    TEST_ASSERT_EQ("(/ -1.00 1.00)", "-1.00");
    TEST_ASSERT_EQ("(/ -1.00 3.00)", "0.00");
    TEST_ASSERT_EQ("(/ -0.60 -5)", "0.00");
    TEST_ASSERT_EQ("(/ -0.60 -2)", "0.00");
    TEST_ASSERT_EQ("(/ -0.60 0Nl)", "0Nf");
    TEST_ASSERT_EQ("(/ -0.60 0)", "0Nf");
    TEST_ASSERT_EQ("(/ -0.60 6)", "0.00");
    TEST_ASSERT_EQ("(/ -0.60 -5i)", "0.00");
    TEST_ASSERT_EQ("(/ -0.60 0Ni)", "0Nf");
    TEST_ASSERT_EQ("(/ -0.60 0i)", "0Nf");
    TEST_ASSERT_EQ("(/ -0.60 6i)", "0.00");
    TEST_ASSERT_EQ("(/ -0.60 -2.00)", "0.00");
    TEST_ASSERT_EQ("(/ -0.60 -1.00)", "0.00");
    TEST_ASSERT_EQ("(/ -0.60 -0.60)", "1.00");
    TEST_ASSERT_EQ("(/ -0.60 -0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ -0.60 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(/ -0.60 0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ -0.60 0.60)", "-1.00");
    TEST_ASSERT_EQ("(/ -0.60 1.00)", "0.00");
    TEST_ASSERT_EQ("(/ -0.60 3.00)", "0.00");
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
    TEST_ASSERT_EQ("(/ 0.60 -5)", "0.00");
    TEST_ASSERT_EQ("(/ 0.60 -2)", "0.00");
    TEST_ASSERT_EQ("(/ 0.60 0Nl)", "0Nf");
    TEST_ASSERT_EQ("(/ 0.60 0)", "0Nf");
    TEST_ASSERT_EQ("(/ 0.60 6)", "0.00");
    TEST_ASSERT_EQ("(/ 0.60 -5i)", "0.00");
    TEST_ASSERT_EQ("(/ 0.60 0Ni)", "0Nf");
    TEST_ASSERT_EQ("(/ 0.60 0i)", "0Nf");
    TEST_ASSERT_EQ("(/ 0.60 6i)", "0.00");
    TEST_ASSERT_EQ("(/ 0.60 -2.00)", "0.00");
    TEST_ASSERT_EQ("(/ 0.60 -1.00)", "0.00");
    TEST_ASSERT_EQ("(/ 0.60 -0.60)", "-1.00");
    TEST_ASSERT_EQ("(/ 0.60 -0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ 0.60 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(/ 0.60 0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ 0.60 0.60)", "1.00");
    TEST_ASSERT_EQ("(/ 0.60 1.00)", "0.00");
    TEST_ASSERT_EQ("(/ 0.60 3.00)", "0.00");
    TEST_ASSERT_EQ("(/ 1.00 -5)", "0.00");
    TEST_ASSERT_EQ("(/ 1.00 -2)", "0.00");
    TEST_ASSERT_EQ("(/ 1.00 0Nl)", "0Nf");
    TEST_ASSERT_EQ("(/ 1.00 0)", "0Nf");
    TEST_ASSERT_EQ("(/ 1.00 6)", "0.00");
    TEST_ASSERT_EQ("(/ 1.00 -5i)", "0.00");
    TEST_ASSERT_EQ("(/ 1.00 0Ni)", "0Nf");
    TEST_ASSERT_EQ("(/ 1.00 0i)", "0Nf");
    TEST_ASSERT_EQ("(/ 1.00 6i)", "0.00");
    TEST_ASSERT_EQ("(/ 1.00 -2.00)", "0.00");
    TEST_ASSERT_EQ("(/ 1.00 -1.00)", "-1.00");
    TEST_ASSERT_EQ("(/ 1.00 -0.60)", "-1.00");
    TEST_ASSERT_EQ("(/ 1.00 -0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ 1.00 0Nf)", "0Nf");
    TEST_ASSERT_EQ("(/ 1.00 0.00)", "0Nf");
    TEST_ASSERT_EQ("(/ 1.00 0.60)", "1.00");
    TEST_ASSERT_EQ("(/ 1.00 1.00)", "1.00");
    TEST_ASSERT_EQ("(/ 1.00 3.00)", "0.00");
    TEST_ASSERT_EQ("(/ 3.00 -5)", "0.00");
    TEST_ASSERT_EQ("(/ 3.00 -2)", "-1.00");
    TEST_ASSERT_EQ("(/ 3.00 0Nl)", "0Nf");
    TEST_ASSERT_EQ("(/ 3.00 0)", "0Nf");
    TEST_ASSERT_EQ("(/ 3.00 6)", "0.00");
    TEST_ASSERT_EQ("(/ 3.00 -5i)", "0.00");
    TEST_ASSERT_EQ("(/ 3.00 0Ni)", "0Nf");
    TEST_ASSERT_EQ("(/ 3.00 0i)", "0Nf");
    TEST_ASSERT_EQ("(/ 3.00 6i)", "0.00");
    TEST_ASSERT_EQ("(/ 3.00 -2.00)", "-1.00");
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
    TEST_ASSERT_EQ("(/ [-5] 6)", "[0]");
    TEST_ASSERT_EQ("(/ [-5] -5i)", "[1]");
    TEST_ASSERT_EQ("(/ [-5] 0Ni)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] 0i)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] 6i)", "[0]");
    TEST_ASSERT_EQ("(/ [-5] -2.00)", "[2]");
    TEST_ASSERT_EQ("(/ [-5] -1.00)", "[5]");
    TEST_ASSERT_EQ("(/ [-5] -0.60)", "[8]");
    TEST_ASSERT_EQ("(/ [-5] -0.00)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] 0Nf)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] 0.00)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] 0.60)", "[-8]");
    TEST_ASSERT_EQ("(/ [-5] 1.00)", "[-5]");
    TEST_ASSERT_EQ("(/ [-5] 3.00)", "[-1]");
    TEST_ASSERT_EQ("(/ [-2] -5)", "[0]");
    TEST_ASSERT_EQ("(/ [-2] -2)", "[1]");
    TEST_ASSERT_EQ("(/ [-2] 0Nl)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] 0)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] 6)", "[0]");
    TEST_ASSERT_EQ("(/ [-2] -5i)", "[0]");
    TEST_ASSERT_EQ("(/ [-2] 0Ni)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] 0i)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] 6i)", "[0]");
    TEST_ASSERT_EQ("(/ [-2] -2.00)", "[1]");
    TEST_ASSERT_EQ("(/ [-2] -1.00)", "[2]");
    TEST_ASSERT_EQ("(/ [-2] -0.60)", "[3]");
    TEST_ASSERT_EQ("(/ [-2] -0.00)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] 0Nf)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] 0.00)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] 0.60)", "[-3]");
    TEST_ASSERT_EQ("(/ [-2] 1.00)", "[-2]");
    TEST_ASSERT_EQ("(/ [-2] 3.00)", "[0]");
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
    TEST_ASSERT_EQ("(/ [6] -5)", "[-1]");
    TEST_ASSERT_EQ("(/ [6] -2)", "[-3]");
    TEST_ASSERT_EQ("(/ [6] 0Nl)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [6] 0)", "[0Nl]");
    TEST_ASSERT_EQ("(/ [6] 6)", "[1]");
    TEST_ASSERT_EQ("(/ [6] -5i)", "[-1]");
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
    TEST_ASSERT_EQ("(/ [-5i] 6)", "[0]");
    TEST_ASSERT_EQ("(/ [-5i] -5i)", "[1]");
    TEST_ASSERT_EQ("(/ [-5i] 0Ni)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] 0i)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] 6i)", "[0]");
    TEST_ASSERT_EQ("(/ [-5i] -2.00)", "[2]");
    TEST_ASSERT_EQ("(/ [-5i] -1.00)", "[5]");
    TEST_ASSERT_EQ("(/ [-5i] -0.60)", "[8]");
    TEST_ASSERT_EQ("(/ [-5i] -0.00)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] 0Nf)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] 0.00)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] 0.60)", "[-8]");
    TEST_ASSERT_EQ("(/ [-5i] 1.00)", "[-5]");
    TEST_ASSERT_EQ("(/ [-5i] 3.00)", "[-1]");
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
    TEST_ASSERT_EQ("(/ [6i] -5)", "[-1]");
    TEST_ASSERT_EQ("(/ [6i] -2)", "[-3]");
    TEST_ASSERT_EQ("(/ [6i] 0Nl)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [6i] 0)", "[0Ni]");
    TEST_ASSERT_EQ("(/ [6i] 6)", "[1]");
    TEST_ASSERT_EQ("(/ [6i] -5i)", "[-1]");
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
    TEST_ASSERT_EQ("(/ [-2.00] 6)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-2.00] -5i)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-2.00] 0Ni)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] 0i)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] 6i)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-2.00] -2.00)", "[1.00]");
    TEST_ASSERT_EQ("(/ [-2.00] -1.00)", "[2.00]");
    TEST_ASSERT_EQ("(/ [-2.00] -0.60)", "[3.00]");
    TEST_ASSERT_EQ("(/ [-2.00] -0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] 0Nf)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] 0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] 0.60)", "[-3.00]");
    TEST_ASSERT_EQ("(/ [-2.00] 1.00)", "[-2.00]");
    TEST_ASSERT_EQ("(/ [-2.00] 3.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-1.00] -5)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-1.00] -2)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-1.00] 0Nl)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] 0)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] 6)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-1.00] -5i)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-1.00] 0Ni)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] 0i)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] 6i)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-1.00] -2.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-1.00] -1.00)", "[1.00]");
    TEST_ASSERT_EQ("(/ [-1.00] -0.60)", "[1.00]");
    TEST_ASSERT_EQ("(/ [-1.00] -0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] 0Nf)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] 0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] 0.60)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-1.00] 1.00)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-1.00] 3.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] -5)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] -2)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] 0Nl)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] 0)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] 6)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] -5i)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] 0Ni)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] 0i)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] 6i)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] -2.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] -1.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] -0.60)", "[1.00]");
    TEST_ASSERT_EQ("(/ [-0.60] -0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] 0Nf)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] 0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] 0.60)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-0.60] 1.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] 3.00)", "[0.00]");
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
    TEST_ASSERT_EQ("(/ [0.60] -5)", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] -2)", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] 0Nl)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] 0)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] 6)", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] -5i)", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] 0Ni)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] 0i)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] 6i)", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] -2.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] -1.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] -0.60)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [0.60] -0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] 0Nf)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] 0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] 0.60)", "[1.00]");
    TEST_ASSERT_EQ("(/ [0.60] 1.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] 3.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [1.00] -5)", "[0.00]");
    TEST_ASSERT_EQ("(/ [1.00] -2)", "[0.00]");
    TEST_ASSERT_EQ("(/ [1.00] 0Nl)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] 0)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] 6)", "[0.00]");
    TEST_ASSERT_EQ("(/ [1.00] -5i)", "[0.00]");
    TEST_ASSERT_EQ("(/ [1.00] 0Ni)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] 0i)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] 6i)", "[0.00]");
    TEST_ASSERT_EQ("(/ [1.00] -2.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [1.00] -1.00)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [1.00] -0.60)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [1.00] -0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] 0Nf)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] 0.00)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] 0.60)", "[1.00]");
    TEST_ASSERT_EQ("(/ [1.00] 1.00)", "[1.00]");
    TEST_ASSERT_EQ("(/ [1.00] 3.00)", "[0.00]");
    TEST_ASSERT_EQ("(/ [3.00] -5)", "[0.00]");
    TEST_ASSERT_EQ("(/ [3.00] -2)", "[-1.00]");
    TEST_ASSERT_EQ("(/ [3.00] 0Nl)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [3.00] 0)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [3.00] 6)", "[0.00]");
    TEST_ASSERT_EQ("(/ [3.00] -5i)", "[0.00]");
    TEST_ASSERT_EQ("(/ [3.00] 0Ni)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [3.00] 0i)", "[0Nf]");
    TEST_ASSERT_EQ("(/ [3.00] 6i)", "[0.00]");
    TEST_ASSERT_EQ("(/ [3.00] -2.00)", "[-1.00]");
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
    TEST_ASSERT_EQ("(/ -5 [6])", "[0]");
    TEST_ASSERT_EQ("(/ -5 [-5i])", "[1]");
    TEST_ASSERT_EQ("(/ -5 [0Ni])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -5 [0i])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -5 [6i])", "[0]");
    TEST_ASSERT_EQ("(/ -5 [-2.00])", "[2]");
    TEST_ASSERT_EQ("(/ -5 [-1.00])", "[5]");
    TEST_ASSERT_EQ("(/ -5 [-0.60])", "[8]");
    TEST_ASSERT_EQ("(/ -5 [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -5 [0Nf])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -5 [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -5 [0.60])", "[-8]");
    TEST_ASSERT_EQ("(/ -5 [1.00])", "[-5]");
    TEST_ASSERT_EQ("(/ -5 [3.00])", "[-1]");
    TEST_ASSERT_EQ("(/ -2 [-5])", "[0]");
    TEST_ASSERT_EQ("(/ -2 [-2])", "[1]");
    TEST_ASSERT_EQ("(/ -2 [0Nl])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -2 [0])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -2 [6])", "[0]");
    TEST_ASSERT_EQ("(/ -2 [-5i])", "[0]");
    TEST_ASSERT_EQ("(/ -2 [0Ni])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -2 [0i])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -2 [6i])", "[0]");
    TEST_ASSERT_EQ("(/ -2 [-2.00])", "[1]");
    TEST_ASSERT_EQ("(/ -2 [-1.00])", "[2]");
    TEST_ASSERT_EQ("(/ -2 [-0.60])", "[3]");
    TEST_ASSERT_EQ("(/ -2 [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -2 [0Nf])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -2 [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ -2 [0.60])", "[-3]");
    TEST_ASSERT_EQ("(/ -2 [1.00])", "[-2]");
    TEST_ASSERT_EQ("(/ -2 [3.00])", "[0]");
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
    TEST_ASSERT_EQ("(/ 6 [-5])", "[-1]");
    TEST_ASSERT_EQ("(/ 6 [-2])", "[-3]");
    TEST_ASSERT_EQ("(/ 6 [0Nl])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 6 [0])", "[0Nl]");
    TEST_ASSERT_EQ("(/ 6 [6])", "[1]");
    TEST_ASSERT_EQ("(/ 6 [-5i])", "[-1]");
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
    TEST_ASSERT_EQ("(/ -5i [6])", "[0]");
    TEST_ASSERT_EQ("(/ -5i [-5i])", "[1]");
    TEST_ASSERT_EQ("(/ -5i [0Ni])", "[0Ni]");
    TEST_ASSERT_EQ("(/ -5i [0i])", "[0Ni]");
    TEST_ASSERT_EQ("(/ -5i [6i])", "[0]");
    TEST_ASSERT_EQ("(/ -5i [-2.00])", "[2]");
    TEST_ASSERT_EQ("(/ -5i [-1.00])", "[5]");
    TEST_ASSERT_EQ("(/ -5i [-0.60])", "[8]");
    TEST_ASSERT_EQ("(/ -5i [0.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ -5i [0Nf])", "[0Ni]");
    TEST_ASSERT_EQ("(/ -5i [0.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ -5i [0.60])", "[-8]");
    TEST_ASSERT_EQ("(/ -5i [1.00])", "[-5]");
    TEST_ASSERT_EQ("(/ -5i [3.00])", "[-1]");
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
    TEST_ASSERT_EQ("(/ 6i [-5])", "[-1]");
    TEST_ASSERT_EQ("(/ 6i [-2])", "[-3]");
    TEST_ASSERT_EQ("(/ 6i [0Nl])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 6i [0])", "[0Ni]");
    TEST_ASSERT_EQ("(/ 6i [6])", "[1]");
    TEST_ASSERT_EQ("(/ 6i [-5i])", "[-1]");
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
    TEST_ASSERT_EQ("(/ -2.00 [6])", "[0.00]");
    TEST_ASSERT_EQ("(/ -2.00 [-5i])", "[0.00]");
    TEST_ASSERT_EQ("(/ -2.00 [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -2.00 [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -2.00 [6i])", "[0.00]");
    TEST_ASSERT_EQ("(/ -2.00 [-2.00])", "[1.00]");
    TEST_ASSERT_EQ("(/ -2.00 [-1.00])", "[2.00]");
    TEST_ASSERT_EQ("(/ -2.00 [-0.60])", "[3.00]");
    TEST_ASSERT_EQ("(/ -2.00 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -2.00 [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -2.00 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -2.00 [0.60])", "[-3.00]");
    TEST_ASSERT_EQ("(/ -2.00 [1.00])", "[-2.00]");
    TEST_ASSERT_EQ("(/ -2.00 [3.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ -1.00 [-5])", "[0.00]");
    TEST_ASSERT_EQ("(/ -1.00 [-2])", "[0.00]");
    TEST_ASSERT_EQ("(/ -1.00 [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -1.00 [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -1.00 [6])", "[0.00]");
    TEST_ASSERT_EQ("(/ -1.00 [-5i])", "[0.00]");
    TEST_ASSERT_EQ("(/ -1.00 [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -1.00 [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -1.00 [6i])", "[0.00]");
    TEST_ASSERT_EQ("(/ -1.00 [-2.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ -1.00 [-1.00])", "[1.00]");
    TEST_ASSERT_EQ("(/ -1.00 [-0.60])", "[1.00]");
    TEST_ASSERT_EQ("(/ -1.00 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -1.00 [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -1.00 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -1.00 [0.60])", "[-1.00]");
    TEST_ASSERT_EQ("(/ -1.00 [1.00])", "[-1.00]");
    TEST_ASSERT_EQ("(/ -1.00 [3.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ -0.60 [-5])", "[0.00]");
    TEST_ASSERT_EQ("(/ -0.60 [-2])", "[0.00]");
    TEST_ASSERT_EQ("(/ -0.60 [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -0.60 [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -0.60 [6])", "[0.00]");
    TEST_ASSERT_EQ("(/ -0.60 [-5i])", "[0.00]");
    TEST_ASSERT_EQ("(/ -0.60 [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -0.60 [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -0.60 [6i])", "[0.00]");
    TEST_ASSERT_EQ("(/ -0.60 [-2.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ -0.60 [-1.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ -0.60 [-0.60])", "[1.00]");
    TEST_ASSERT_EQ("(/ -0.60 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -0.60 [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -0.60 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ -0.60 [0.60])", "[-1.00]");
    TEST_ASSERT_EQ("(/ -0.60 [1.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ -0.60 [3.00])", "[0.00]");
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
    TEST_ASSERT_EQ("(/ 0.60 [-5])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.60 [-2])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.60 [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.60 [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.60 [6])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.60 [-5i])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.60 [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.60 [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.60 [6i])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.60 [-2.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.60 [-1.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.60 [-0.60])", "[-1.00]");
    TEST_ASSERT_EQ("(/ 0.60 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.60 [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.60 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 0.60 [0.60])", "[1.00]");
    TEST_ASSERT_EQ("(/ 0.60 [1.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ 0.60 [3.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ 1.00 [-5])", "[0.00]");
    TEST_ASSERT_EQ("(/ 1.00 [-2])", "[0.00]");
    TEST_ASSERT_EQ("(/ 1.00 [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 1.00 [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 1.00 [6])", "[0.00]");
    TEST_ASSERT_EQ("(/ 1.00 [-5i])", "[0.00]");
    TEST_ASSERT_EQ("(/ 1.00 [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 1.00 [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 1.00 [6i])", "[0.00]");
    TEST_ASSERT_EQ("(/ 1.00 [-2.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ 1.00 [-1.00])", "[-1.00]");
    TEST_ASSERT_EQ("(/ 1.00 [-0.60])", "[-1.00]");
    TEST_ASSERT_EQ("(/ 1.00 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 1.00 [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 1.00 [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 1.00 [0.60])", "[1.00]");
    TEST_ASSERT_EQ("(/ 1.00 [1.00])", "[1.00]");
    TEST_ASSERT_EQ("(/ 1.00 [3.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ 3.00 [-5])", "[0.00]");
    TEST_ASSERT_EQ("(/ 3.00 [-2])", "[-1.00]");
    TEST_ASSERT_EQ("(/ 3.00 [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 3.00 [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 3.00 [6])", "[0.00]");
    TEST_ASSERT_EQ("(/ 3.00 [-5i])", "[0.00]");
    TEST_ASSERT_EQ("(/ 3.00 [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 3.00 [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ 3.00 [6i])", "[0.00]");
    TEST_ASSERT_EQ("(/ 3.00 [-2.00])", "[-1.00]");
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
    TEST_ASSERT_EQ("(/ [-5] [6])", "[0]");
    TEST_ASSERT_EQ("(/ [-5] [-5i])", "[1]");
    TEST_ASSERT_EQ("(/ [-5] [0Ni])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] [0i])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] [6i])", "[0]");
    TEST_ASSERT_EQ("(/ [-5] [-2.00])", "[2]");
    TEST_ASSERT_EQ("(/ [-5] [-1.00])", "[5]");
    TEST_ASSERT_EQ("(/ [-5] [-0.60])", "[8]");
    TEST_ASSERT_EQ("(/ [-5] [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] [0Nf])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-5] [0.60])", "[-8]");
    TEST_ASSERT_EQ("(/ [-5] [1.00])", "[-5]");
    TEST_ASSERT_EQ("(/ [-5] [3.00])", "[-1]");
    TEST_ASSERT_EQ("(/ [-2] [-5])", "[0]");
    TEST_ASSERT_EQ("(/ [-2] [-2])", "[1]");
    TEST_ASSERT_EQ("(/ [-2] [0Nl])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] [0])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] [6])", "[0]");
    TEST_ASSERT_EQ("(/ [-2] [-5i])", "[0]");
    TEST_ASSERT_EQ("(/ [-2] [0Ni])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] [0i])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] [6i])", "[0]");
    TEST_ASSERT_EQ("(/ [-2] [-2.00])", "[1]");
    TEST_ASSERT_EQ("(/ [-2] [-1.00])", "[2]");
    TEST_ASSERT_EQ("(/ [-2] [-0.60])", "[3]");
    TEST_ASSERT_EQ("(/ [-2] [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] [0Nf])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] [0.00])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [-2] [0.60])", "[-3]");
    TEST_ASSERT_EQ("(/ [-2] [1.00])", "[-2]");
    TEST_ASSERT_EQ("(/ [-2] [3.00])", "[0]");
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
    TEST_ASSERT_EQ("(/ [6] [-5])", "[-1]");
    TEST_ASSERT_EQ("(/ [6] [-2])", "[-3]");
    TEST_ASSERT_EQ("(/ [6] [0Nl])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [6] [0])", "[0Nl]");
    TEST_ASSERT_EQ("(/ [6] [6])", "[1]");
    TEST_ASSERT_EQ("(/ [6] [-5i])", "[-1]");
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
    TEST_ASSERT_EQ("(/ [-5i] [6])", "[0]");
    TEST_ASSERT_EQ("(/ [-5i] [-5i])", "[1]");
    TEST_ASSERT_EQ("(/ [-5i] [0Ni])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] [0i])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] [6i])", "[0]");
    TEST_ASSERT_EQ("(/ [-5i] [-2.00])", "[2]");
    TEST_ASSERT_EQ("(/ [-5i] [-1.00])", "[5]");
    TEST_ASSERT_EQ("(/ [-5i] [-0.60])", "[8]");
    TEST_ASSERT_EQ("(/ [-5i] [0.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] [0Nf])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] [0.00])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [-5i] [0.60])", "[-8]");
    TEST_ASSERT_EQ("(/ [-5i] [1.00])", "[-5]");
    TEST_ASSERT_EQ("(/ [-5i] [3.00])", "[-1]");
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
    TEST_ASSERT_EQ("(/ [6i] [-5])", "[-1]");
    TEST_ASSERT_EQ("(/ [6i] [-2])", "[-3]");
    TEST_ASSERT_EQ("(/ [6i] [0Nl])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [6i] [0])", "[0Ni]");
    TEST_ASSERT_EQ("(/ [6i] [6])", "[1]");
    TEST_ASSERT_EQ("(/ [6i] [-5i])", "[-1]");
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
    TEST_ASSERT_EQ("(/ [-2.00] [6])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-2.00] [-5i])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-2.00] [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] [6i])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-2.00] [-2.00])", "[1.00]");
    TEST_ASSERT_EQ("(/ [-2.00] [-1.00])", "[2.00]");
    TEST_ASSERT_EQ("(/ [-2.00] [-0.60])", "[3.00]");
    TEST_ASSERT_EQ("(/ [-2.00] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-2.00] [0.60])", "[-3.00]");
    TEST_ASSERT_EQ("(/ [-2.00] [1.00])", "[-2.00]");
    TEST_ASSERT_EQ("(/ [-2.00] [3.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-1.00] [-5])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-1.00] [-2])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-1.00] [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] [6])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-1.00] [-5i])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-1.00] [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] [6i])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-1.00] [-2.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-1.00] [-1.00])", "[1.00]");
    TEST_ASSERT_EQ("(/ [-1.00] [-0.60])", "[1.00]");
    TEST_ASSERT_EQ("(/ [-1.00] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-1.00] [0.60])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-1.00] [1.00])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-1.00] [3.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] [-5])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] [-2])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] [6])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] [-5i])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] [6i])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] [-2.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] [-1.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] [-0.60])", "[1.00]");
    TEST_ASSERT_EQ("(/ [-0.60] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [-0.60] [0.60])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [-0.60] [1.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [-0.60] [3.00])", "[0.00]");
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
    TEST_ASSERT_EQ("(/ [0.60] [-5])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] [-2])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] [6])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] [-5i])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] [6i])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] [-2.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] [-1.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] [-0.60])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [0.60] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [0.60] [0.60])", "[1.00]");
    TEST_ASSERT_EQ("(/ [0.60] [1.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [0.60] [3.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [1.00] [-5])", "[0.00]");
    TEST_ASSERT_EQ("(/ [1.00] [-2])", "[0.00]");
    TEST_ASSERT_EQ("(/ [1.00] [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] [6])", "[0.00]");
    TEST_ASSERT_EQ("(/ [1.00] [-5i])", "[0.00]");
    TEST_ASSERT_EQ("(/ [1.00] [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] [6i])", "[0.00]");
    TEST_ASSERT_EQ("(/ [1.00] [-2.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [1.00] [-1.00])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [1.00] [-0.60])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [1.00] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] [0Nf])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] [0.00])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [1.00] [0.60])", "[1.00]");
    TEST_ASSERT_EQ("(/ [1.00] [1.00])", "[1.00]");
    TEST_ASSERT_EQ("(/ [1.00] [3.00])", "[0.00]");
    TEST_ASSERT_EQ("(/ [3.00] [-5])", "[0.00]");
    TEST_ASSERT_EQ("(/ [3.00] [-2])", "[-1.00]");
    TEST_ASSERT_EQ("(/ [3.00] [0Nl])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [3.00] [0])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [3.00] [6])", "[0.00]");
    TEST_ASSERT_EQ("(/ [3.00] [-5i])", "[0.00]");
    TEST_ASSERT_EQ("(/ [3.00] [0Ni])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [3.00] [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(/ [3.00] [6i])", "[0.00]");
    TEST_ASSERT_EQ("(/ [3.00] [-2.00])", "[-1.00]");
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
    TEST_ASSERT_EQ("(% -11i 5i)", "-1i");
    TEST_ASSERT_EQ("(% -11i 5)", "-1");
    TEST_ASSERT_EQ("(% -11i 5.0)", "-1.0");
    TEST_ASSERT_EQ("(% -10i [0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% -10i [0])", "[0Nl]");
    TEST_ASSERT_EQ("(% -10i [0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% -10i [5i])", "[0i]");
    TEST_ASSERT_EQ("(% -11i [5i])", "[-1i]");
    TEST_ASSERT_EQ("(% -11i [5])", "[-1]");
    TEST_ASSERT_EQ("(% -11i [5.0])", "[-1.0]");
    TEST_ASSERT_EQ("(% 10i -0i)", "0Ni");
    TEST_ASSERT_EQ("(% 10i -5i)", "0i");
    TEST_ASSERT_EQ("(% 11i -5i)", "1i");
    TEST_ASSERT_EQ("(% 11i -5)", "1");
    TEST_ASSERT_EQ("(% 11i -5.0)", "1.0");
    TEST_ASSERT_EQ("(% 10i [-0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% 10i [-0])", "[0Nl]");
    TEST_ASSERT_EQ("(% 10i [-0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% 10i [-5i])", "[0i]");
    TEST_ASSERT_EQ("(% 11i [-5i])", "[1i]");
    TEST_ASSERT_EQ("(% 11i [-5])", "[1]");
    TEST_ASSERT_EQ("(% 11i [-5.0])", "[1.0]");
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
    TEST_ASSERT_EQ("(% -11 5i)", "-1i");
    TEST_ASSERT_EQ("(% -11 5)", "-1");
    TEST_ASSERT_EQ("(% -11 5.0)", "-1.0");
    TEST_ASSERT_EQ("(% -10 [0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% -10 [0])", "[0Nl]");
    TEST_ASSERT_EQ("(% -10 [0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% -10 [5i])", "[0i]");
    TEST_ASSERT_EQ("(% -11 [5i])", "[-1i]");
    TEST_ASSERT_EQ("(% -11 [5])", "[-1]");
    TEST_ASSERT_EQ("(% -11 [5.0])", "[-1.0]");
    TEST_ASSERT_EQ("(% 10 -0i)", "0Ni");
    TEST_ASSERT_EQ("(% 10 -5i)", "0i");
    TEST_ASSERT_EQ("(% 11 -5i)", "1i");
    TEST_ASSERT_EQ("(% 11 -5)", "1");
    TEST_ASSERT_EQ("(% 11 -5.0)", "1.0");
    TEST_ASSERT_EQ("(% 10 [-0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% 10 [-0])", "[0Nl]");
    TEST_ASSERT_EQ("(% 10 [-0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% 10 [-5i])", "[0i]");
    TEST_ASSERT_EQ("(% 11 [-5i])", "[1i]");
    TEST_ASSERT_EQ("(% 11 [-5])", "[1]");
    TEST_ASSERT_EQ("(% 11 [-5.0])", "[1.0]");
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
    TEST_ASSERT_EQ("(% -11.0 5i)", "-1.0");
    TEST_ASSERT_EQ("(% -11.0 5)", "-1.0");
    TEST_ASSERT_EQ("(% -11.0 5.0)", "-1.0");
    TEST_ASSERT_EQ("(% -10.0 [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(% -10.0 [0])", "[0Nf]");
    TEST_ASSERT_EQ("(% -10.0 [0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% -10.0 [5i])", "[0.0]");
    TEST_ASSERT_EQ("(% -11.0 [5i])", "[-1.0]");
    TEST_ASSERT_EQ("(% -11.0 [5])", "[-1.0]");
    TEST_ASSERT_EQ("(% -11.0 [5.0])", "[-1.0]");
    TEST_ASSERT_EQ("(% 10.0 -0i)", "0Nf");
    TEST_ASSERT_EQ("(% 10.0 -5i)", "0.0");
    TEST_ASSERT_EQ("(% 11.0 -5i)", "1.0");
    TEST_ASSERT_EQ("(% 11.0 -5)", "1.0");
    TEST_ASSERT_EQ("(% 11.0 -5.0)", "1.0");
    TEST_ASSERT_EQ("(% 10.0 [-0i])", "[0Nf]");
    TEST_ASSERT_EQ("(% 10.0 [-0])", "[0Nf]");
    TEST_ASSERT_EQ("(% 10.0 [-0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% 10.0 [-5i])", "[0.0]");
    TEST_ASSERT_EQ("(% 11.0 [-5i])", "[1.0]");
    TEST_ASSERT_EQ("(% 11.0 [-5])", "[1.0]");
    TEST_ASSERT_EQ("(% 11.0 [-5.0])", "[1.0]");
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
    TEST_ASSERT_EQ("(% [-11i] 5i)", "[-1i]");
    TEST_ASSERT_EQ("(% [-11i] 5)", "[-1]");
    TEST_ASSERT_EQ("(% [-11i] 5.0)", "[-1.0]");
    TEST_ASSERT_EQ("(% [-10i] [0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% [-10i] [0])", "[0Nl]");
    TEST_ASSERT_EQ("(% [-10i] [0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% [-10i] [5i])", "[0i]");
    TEST_ASSERT_EQ("(% [-11i] [5i])", "[-1i]");
    TEST_ASSERT_EQ("(% [-11i] [5])", "[-1]");
    TEST_ASSERT_EQ("(% [-11i] [5.0])", "[-1.0]");
    TEST_ASSERT_EQ("(% [10i] -0i)", "[0Ni]");
    TEST_ASSERT_EQ("(% [10i] -5i)", "[0i]");
    TEST_ASSERT_EQ("(% [11i] -5i)", "[1i]");
    TEST_ASSERT_EQ("(% [11i] -5)", "[1]");
    TEST_ASSERT_EQ("(% [11i] -5.0)", "[1.0]");
    TEST_ASSERT_EQ("(% [10i] [-0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% [10i] [-0])", "[0Nl]");
    TEST_ASSERT_EQ("(% [10i] [-0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% [10i] [-5i])", "[0i]");
    TEST_ASSERT_EQ("(% [11i] [-5i])", "[1i]");
    TEST_ASSERT_EQ("(% [11i] [-5])", "[1]");
    TEST_ASSERT_EQ("(% [11i] [-5.0])", "[1.0]");
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
    TEST_ASSERT_EQ("(% [-11] 5i)", "[-1i]");
    TEST_ASSERT_EQ("(% [-11] 5)", "[-1]");
    TEST_ASSERT_EQ("(% [-11] 5.0)", "[-1.0]");
    TEST_ASSERT_EQ("(% [-10] [0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% [-10] [0])", "[0Nl]");
    TEST_ASSERT_EQ("(% [-10] [0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% [-10] [5i])", "[0i]");
    TEST_ASSERT_EQ("(% [-11] [5i])", "[-1i]");
    TEST_ASSERT_EQ("(% [-11] [5])", "[-1]");
    TEST_ASSERT_EQ("(% [-11] [5.0])", "[-1.0]");
    TEST_ASSERT_EQ("(% [10] -0i)", "[0Ni]");
    TEST_ASSERT_EQ("(% [10] -5i)", "[0i]");
    TEST_ASSERT_EQ("(% [11] -5i)", "[1i]");
    TEST_ASSERT_EQ("(% [11] -5)", "[1]");
    TEST_ASSERT_EQ("(% [11] -5.0)", "[1.0]");
    TEST_ASSERT_EQ("(% [10] [-0i])", "[0Ni]");
    TEST_ASSERT_EQ("(% [10] [-0])", "[0Nl]");
    TEST_ASSERT_EQ("(% [10] [-0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% [10] [-5i])", "[0i]");
    TEST_ASSERT_EQ("(% [11] [-5i])", "[1i]");
    TEST_ASSERT_EQ("(% [11] [-5])", "[1]");
    TEST_ASSERT_EQ("(% [11] [-5.0])", "[1.0]");
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
    TEST_ASSERT_EQ("(% [-11.0] 5i)", "[-1.0]");
    TEST_ASSERT_EQ("(% [-11.0] 5)", "[-1.0]");
    TEST_ASSERT_EQ("(% [-11.0] 5.0)", "[-1.0]");
    TEST_ASSERT_EQ("(% [-10.0] [0i])", "[0Nf]");
    TEST_ASSERT_EQ("(% [-10.0] [0])", "[0Nf]");
    TEST_ASSERT_EQ("(% [-10.0] [0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% [-10.0] [5i])", "[0.0]");
    TEST_ASSERT_EQ("(% [-11.0] [5i])", "[-1.0]");
    TEST_ASSERT_EQ("(% [-11.0] [5])", "[-1.0]");
    TEST_ASSERT_EQ("(% [-11.0] [5.0])", "[-1.0]");
    TEST_ASSERT_EQ("(% [10.0] -0i)", "[0Nf]");
    TEST_ASSERT_EQ("(% [10.0] -5i)", "[0.0]");
    TEST_ASSERT_EQ("(% [11.0] -5i)", "[1.0]");
    TEST_ASSERT_EQ("(% [11.0] -5)", "[1.0]");
    TEST_ASSERT_EQ("(% [11.0] -5.0)", "[1.0]");
    TEST_ASSERT_EQ("(% [10.0] [-0i])", "[0Nf]");
    TEST_ASSERT_EQ("(% [10.0] [-0])", "[0Nf]");
    TEST_ASSERT_EQ("(% [10.0] [-0.0])", "[0Nf]");
    TEST_ASSERT_EQ("(% [10.0] [-5i])", "[0.0]");
    TEST_ASSERT_EQ("(% [11.0] [-5i])", "[1.0]");
    TEST_ASSERT_EQ("(% [11.0] [-5])", "[1.0]");
    TEST_ASSERT_EQ("(% [11.0] [-5.0])", "[1.0]");
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
    TEST_ASSERT_EQ("(map count (list (list \"aaa\" \"bbb\")))", "[2]");

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
    PASS();
}

test_result_t test_lang_update() { PASS(); }

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
