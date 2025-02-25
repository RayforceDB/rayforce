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

test_result_t test_str_match() {
    TEST_ASSERT(str_match("brown", 5, "br?*wn", 6), "brown\", \"br?*wn");
    TEST_ASSERT(str_match("broasdfasdfwn", 13, "br?*wn", 6), "broasdfasdfwn\", \"br?*wn");
    TEST_ASSERT(str_match("browmwn", 7, "br?*wn", 6), "browmwn\", \"br?*wn");
    TEST_ASSERT(str_match("brown", 5, "[wertfb]rown", 12), "brown\", \"[wertfb]rown");
    TEST_ASSERT(str_match("brown", 5, "[^wertf]rown", 12), "brown\", \"[^wertf]rown");
    TEST_ASSERT(!str_match("bro[wn", 6, "[^wertf]ro[[wn", 14), "bro[wn\", \"[^wertf]ro[[wn");
    TEST_ASSERT(str_match("bro^wn", 6, "[^wertf]ro^wn", 13), "bro^wn\", \"[^wertf]ro^wn");
    TEST_ASSERT(!str_match("brown", 5, "br[?*]wn", 8), "brown\", \"br[?*]wn");

    // FIX protocol message tests - previously caused infinite loop
    TEST_ASSERT(!str_match("8=FIX.4.29=004835=049=TR56=CL134=252=20250225-16:15:36.31410=157", 67, "*35=A*", 6),
                "FIX message with 35=0 tag should not match *35=A* pattern");
    TEST_ASSERT(str_match("8=FIX.4.29=004835=049=TR56=CL134=252=20250225-16:15:36.31410=157", 67, "*35=0*", 6),
                "FIX message with 35=0 tag should match *35=0* pattern");

    // Additional edge cases for backtracking
    TEST_ASSERT(!str_match("abc", 3, "*d*", 3), "abc should not match *d*");
    TEST_ASSERT(str_match("abcdefg", 7, "*c*g", 4), "abcdefg should match *c*g");
    TEST_ASSERT(str_match("abcdefg", 7, "a*d*g", 5), "abcdefg should match a*d*g");
    TEST_ASSERT(!str_match("abcdefg", 7, "a*x*g", 5), "abcdefg should not match a*x*g");

    PASS();
}
