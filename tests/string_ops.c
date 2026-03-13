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

// ==================== STRING CONCAT EDGE CASES ====================
test_result_t test_string_concat_edge() {
    // Empty + empty
    TEST_ASSERT_EQ("(concat \"\" \"\")", "\"\"");
    // Multi concat via do
    TEST_ASSERT_EQ("(concat (concat \"ab\" \"cd\") \"ef\")", "\"abcdef\"");
    // Concat char atoms to string
    TEST_ASSERT_EQ("(concat 'a' 'b')", "\"ab\"");
    TEST_ASSERT_EQ("(concat 'a' \"bc\")", "\"abc\"");
    TEST_ASSERT_EQ("(concat \"ab\" 'c')", "\"abc\"");
    // Single char strings
    TEST_ASSERT_EQ("(concat \"a\" \"b\")", "\"ab\"");
    // Longer strings
    TEST_ASSERT_EQ("(concat \"hello \" \"world\")", "\"hello world\"");
    // Concat preserves length
    TEST_ASSERT_EQ("(count (concat \"ab\" \"cd\"))", "4");
    TEST_ASSERT_EQ("(count (concat \"\" \"\"))", "0");
    // Concat string with list produces list
    TEST_ASSERT_EQ("(concat (list \"hello\") \"world\")", "(list \"hello\" \"world\")");
    // Symbol concat
    TEST_ASSERT_EQ("(concat ['a 'b] ['c])", "['a 'b 'c]");
    TEST_ASSERT_EQ("(concat 'a 'b)", "['a 'b]");
    // Concat with null-terminated strings (existing tests verify this)
    TEST_ASSERT_EQ("(concat 't' \"est\\000\")", "\"test\"");
    TEST_ASSERT_EQ("(concat \"tes\\000\" 't')", "\"test\"");
    TEST_ASSERT_EQ("(concat \"te\\000\" \"st\\000\")", "\"test\"");
    // Mixed type concat produces list
    TEST_ASSERT_EQ("(concat 'a' 5)", "(list 'a' 5)");
    TEST_ASSERT_EQ("(concat 5 (list 'a'))", "(list 5 'a')");

    PASS();
}

// ==================== STRING COUNT & LENGTH ====================
test_result_t test_string_count() {
    TEST_ASSERT_EQ("(count \"\")", "0");
    TEST_ASSERT_EQ("(count \"a\")", "1");
    TEST_ASSERT_EQ("(count \"hello\")", "5");
    TEST_ASSERT_EQ("(count \"hello world\")", "11");
    // Count on char atom (should be 1)
    TEST_ASSERT_EQ("(count 'a')", "1");
    // Count on symbol vectors
    TEST_ASSERT_EQ("(count ['a 'b 'c])", "3");
    TEST_ASSERT_EQ("(count ['hello])", "1");
    // Count on list of strings
    TEST_ASSERT_EQ("(count (list \"a\" \"bb\" \"ccc\"))", "3");
    // Count on empty list
    TEST_ASSERT_EQ("(count (list))", "0");

    PASS();
}

// ==================== STRING INDEXING ====================
test_result_t test_string_indexing() {
    // Basic scalar indexing
    TEST_ASSERT_EQ("(at \"hello\" 0)", "'h'");
    TEST_ASSERT_EQ("(at \"hello\" 1)", "'e'");
    TEST_ASSERT_EQ("(at \"hello\" 4)", "'o'");
    // Vector indexing
    TEST_ASSERT_EQ("(at \"hello\" [0 1 2 3 4])", "\"hello\"");
    TEST_ASSERT_EQ("(at \"hello\" [4 3 2 1 0])", "\"olleh\"");
    TEST_ASSERT_EQ("(at \"hello\" [0 0 0])", "\"hhh\"");
    TEST_ASSERT_EQ("(at \"abcde\" [0 2 4])", "\"ace\"");
    // Single element vector index
    TEST_ASSERT_EQ("(at \"hello\" [0])", "\"h\"");
    // Empty vector index returns null
    TEST_ASSERT_EQ("(at \"hello\" [])", "0Nl");
    // Out of bounds returns null char
    TEST_ASSERT_EQ("(at \"hello\" 10)", "(as 'c8 0)");
    // Indexing empty string
    TEST_ASSERT_EQ("(at \"\" 0)", "(as 'c8 0)");

    PASS();
}

// ==================== STRING TAKE ====================
test_result_t test_string_take() {
    // Positive take
    TEST_ASSERT_EQ("(take \"hello\" 0)", "\"\"");
    TEST_ASSERT_EQ("(take \"hello\" 1)", "\"h\"");
    TEST_ASSERT_EQ("(take \"hello\" 3)", "\"hel\"");
    TEST_ASSERT_EQ("(take \"hello\" 5)", "\"hello\"");
    // Negative take (from end)
    TEST_ASSERT_EQ("(take \"hello\" -1)", "\"o\"");
    TEST_ASSERT_EQ("(take \"hello\" -2)", "\"lo\"");
    TEST_ASSERT_EQ("(take \"hello\" -5)", "\"hello\"");
    // Over-take (cyclic wrapping)
    TEST_ASSERT_EQ("(take \"ab\" 5)", "\"ababa\"");
    TEST_ASSERT_EQ("(take \"abc\" 7)", "\"abcabca\"");
    // Take from single char string
    TEST_ASSERT_EQ("(take \"x\" 3)", "\"xxx\"");
    // Take empty string
    TEST_ASSERT_EQ("(take \"\" 0)", "\"\"");

    PASS();
}

// ==================== STRING FIRST/LAST ====================
test_result_t test_string_first_last() {
    TEST_ASSERT_EQ("(first \"hello\")", "'h'");
    TEST_ASSERT_EQ("(first \"a\")", "'a'");
    TEST_ASSERT_EQ("(last \"hello\")", "'o'");
    TEST_ASSERT_EQ("(last \"a\")", "'a'");
    // First/last on empty string return null char
    TEST_ASSERT_EQ("(first \"\")", "(as 'c8 0)");
    TEST_ASSERT_EQ("(last \"\")", "(as 'c8 0)");
    // First/last on list of strings
    TEST_ASSERT_EQ("(first (list \"hello\" \"world\"))", "\"hello\"");
    TEST_ASSERT_EQ("(last (list \"hello\" \"world\"))", "\"world\"");
    // First/last on symbol vector
    TEST_ASSERT_EQ("(first ['abc 'def 'ghi])", "'abc");
    TEST_ASSERT_EQ("(last ['abc 'def 'ghi])", "'ghi");

    PASS();
}

// ==================== STRING LIKE (PATTERN MATCHING) ====================
test_result_t test_string_like() {
    // Exact match
    TEST_ASSERT_EQ("(like \"hello\" \"hello\")", "true");
    TEST_ASSERT_EQ("(like \"hello\" \"world\")", "false");
    // Wildcard * (matches any sequence)
    TEST_ASSERT_EQ("(like \"hello\" \"h*\")", "true");
    TEST_ASSERT_EQ("(like \"hello\" \"*o\")", "true");
    TEST_ASSERT_EQ("(like \"hello\" \"*ll*\")", "true");
    TEST_ASSERT_EQ("(like \"hello\" \"*xyz*\")", "false");
    TEST_ASSERT_EQ("(like \"hello\" \"*\")", "true");
    // Single char wildcard ?
    TEST_ASSERT_EQ("(like \"hello\" \"h?llo\")", "true");
    TEST_ASSERT_EQ("(like \"hello\" \"?ello\")", "true");
    TEST_ASSERT_EQ("(like \"hello\" \"hell?\")", "true");
    TEST_ASSERT_EQ("(like \"hello\" \"?????\")", "true");
    TEST_ASSERT_EQ("(like \"hello\" \"??????\")", "false");
    TEST_ASSERT_EQ("(like \"hello\" \"????\")", "false");
    // Combined wildcards
    TEST_ASSERT_EQ("(like \"hello\" \"h*o\")", "true");
    TEST_ASSERT_EQ("(like \"hello\" \"?*o\")", "true");
    TEST_ASSERT_EQ("(like \"abc\" \"a?c\")", "true");
    TEST_ASSERT_EQ("(like \"ac\" \"a?c\")", "false");
    // Empty pattern
    TEST_ASSERT_EQ("(like \"\" \"\")", "true");
    TEST_ASSERT_EQ("(like \"hello\" \"\")", "false");
    TEST_ASSERT_EQ("(like \"\" \"*\")", "true");
    TEST_ASSERT_EQ("(like \"\" \"?\")", "false");
    // Like on list of strings
    TEST_ASSERT_EQ("(like (list \"hello\" \"world\" \"help\") \"hel*\")", "[true false true]");
    TEST_ASSERT_EQ("(like (list \"abc\" \"def\" \"abz\") \"a?c\")", "[true false false]");
    TEST_ASSERT_EQ("(like (list \"foo\" \"bar\" \"baz\") \"b*\")", "[false true true]");
    // Character class [...]
    TEST_ASSERT_EQ("(like \"brown\" \"[wertfb]rown\")", "true");
    TEST_ASSERT_EQ("(like \"brown\" \"[^wertf]rown\")", "true");
    // Negated char class
    TEST_ASSERT_EQ("(like \"brown\" \"[^b]rown\")", "false");
    // No match
    TEST_ASSERT_EQ("(like \"abc\" \"*d*\")", "false");
    TEST_ASSERT_EQ("(like \"abcdefg\" \"a*x*g\")", "false");
    // Complex patterns
    TEST_ASSERT_EQ("(like \"abcdefg\" \"*c*g\")", "true");
    TEST_ASSERT_EQ("(like \"abcdefg\" \"a*d*g\")", "true");

    PASS();
}

// ==================== STRING SPLIT ====================
test_result_t test_string_split() {
    // Basic single-char delimiter
    TEST_ASSERT_EQ("(split \"a,b,c\" \",\")", "(list \"a\" \"b\" \"c\")");
    TEST_ASSERT_EQ("(split \"hello\" \",\")", "(list \"hello\")");
    // Leading/trailing delimiter
    TEST_ASSERT_EQ("(split \",a,b,\" \",\")", "(list \"\" \"a\" \"b\" \"\")");
    TEST_ASSERT_EQ("(split \",\" \",\")", "(list \"\" \"\")");
    // Consecutive delimiters
    TEST_ASSERT_EQ("(split \"a,,b\" \",\")", "(list \"a\" \"\" \"b\")");
    TEST_ASSERT_EQ("(split \",,\" \",\")", "(list \"\" \"\" \"\")");
    // Multi-char delimiter
    TEST_ASSERT_EQ("(split \"a--b--c\" \"--\")", "(list \"a\" \"b\" \"c\")");
    TEST_ASSERT_EQ("(split \"hello\" \"--\")", "(list \"hello\")");
    TEST_ASSERT_EQ("(split \"--a--\" \"--\")", "(list \"\" \"a\" \"\")");
    // Empty string split
    TEST_ASSERT_EQ("(split \"\" \",\")", "(list \"\")");
    TEST_ASSERT_EQ("(split \"\" \"--\")", "(list \"\")");
    // Symbol split
    TEST_ASSERT_EQ("(split 'asasd \"d\")", "(list \"asas\" \"\")");
    TEST_ASSERT_EQ("(split 'asasd 'd')", "(list \"asas\" \"\")");
    // Split with count verification
    TEST_ASSERT_EQ("(count (split \"a,b,c,d,e\" \",\"))", "5");
    TEST_ASSERT_EQ("(count (split \"hello\" \",\"))", "1");
    // First/last of split result
    TEST_ASSERT_EQ("(first (split \"hello,world\" \",\"))", "\"hello\"");
    TEST_ASSERT_EQ("(last (split \"hello,world\" \",\"))", "\"world\"");
    // Split vector by indices
    TEST_ASSERT_EQ("(split [1 2 3 4 5] [0 2 4])", "(list [1 2] [3 4] [5])");
    TEST_ASSERT_EQ("(split \"hello\" [0 2 4])", "(list \"he\" \"ll\" \"o\")");

    PASS();
}

// ==================== STRING COMPARISONS ====================
test_result_t test_string_comparison() {
    // String equality (whole string comparison, returns atom)
    TEST_ASSERT_EQ("(== \"hello\" \"hello\")", "true");
    TEST_ASSERT_EQ("(== \"hello\" \"world\")", "false");
    TEST_ASSERT_EQ("(== \"\" \"\")", "true");
    TEST_ASSERT_EQ("(== \"a\" \"a\")", "true");
    TEST_ASSERT_EQ("(== \"a\" \"b\")", "false");
    // String inequality
    TEST_ASSERT_EQ("(!= \"hello\" \"world\")", "true");
    TEST_ASSERT_EQ("(!= \"hello\" \"hello\")", "false");
    // Char comparisons
    TEST_ASSERT_EQ("(== 'a' 'a')", "true");
    TEST_ASSERT_EQ("(== 'a' 'b')", "false");
    TEST_ASSERT_EQ("(< 'a' 'b')", "true");
    TEST_ASSERT_EQ("(> 'b' 'a')", "true");
    TEST_ASSERT_EQ("(<= 'a' 'a')", "true");
    TEST_ASSERT_EQ("(>= 'z' 'a')", "true");
    // Symbol comparisons
    TEST_ASSERT_EQ("(== 'hello 'hello)", "true");
    TEST_ASSERT_EQ("(== 'hello 'world)", "false");
    TEST_ASSERT_EQ("(!= 'hello 'world)", "true");
    // Char in string
    TEST_ASSERT_EQ("(in 'h' \"hello\")", "true");
    TEST_ASSERT_EQ("(in 'z' \"hello\")", "false");
    // String in string (element-wise per char)
    TEST_ASSERT_EQ("(in \"abc\" \"axy\")", "[true false false]");

    PASS();
}

// ==================== STRING REVERSE ====================
test_result_t test_string_reverse() {
    TEST_ASSERT_EQ("(reverse \"hello\")", "\"olleh\"");
    TEST_ASSERT_EQ("(reverse \"a\")", "\"a\"");
    TEST_ASSERT_EQ("(reverse \"\")", "\"\"");
    TEST_ASSERT_EQ("(reverse \"ab\")", "\"ba\"");
    TEST_ASSERT_EQ("(reverse \"abcde\")", "\"edcba\"");
    // Double reverse = identity
    TEST_ASSERT_EQ("(reverse (reverse \"hello\"))", "\"hello\"");
    // Reverse palindrome gives same string (compare as whole)
    TEST_ASSERT_EQ("(== (reverse \"racecar\") \"racecar\")", "true");
    // Reverse + indexing
    TEST_ASSERT_EQ("(first (reverse \"hello\"))", "'o'");
    TEST_ASSERT_EQ("(last (reverse \"hello\"))", "'h'");
    // Reverse + count
    TEST_ASSERT_EQ("(count (reverse \"hello\"))", "5");
    TEST_ASSERT_EQ("(count (reverse \"\"))", "0");

    PASS();
}

// ==================== STRING FIND ====================
test_result_t test_string_find() {
    // Find char in string
    TEST_ASSERT_EQ("(find \"hello\" 'h')", "0");
    TEST_ASSERT_EQ("(find \"hello\" 'e')", "1");
    TEST_ASSERT_EQ("(find \"hello\" 'l')", "2");
    TEST_ASSERT_EQ("(find \"hello\" 'o')", "4");
    TEST_ASSERT_EQ("(find \"hello\" 'z')", "0Nl");
    // Find in empty string
    TEST_ASSERT_EQ("(find \"\" 'a')", "0Nl");
    // Find multiple chars
    TEST_ASSERT_EQ("(find \"abcde\" \"ace\")", "[0 2 4]");
    TEST_ASSERT_EQ("(find \"abcde\" \"xyz\")", "[0Nl 0Nl 0Nl]");
    TEST_ASSERT_EQ("(find \"abcde\" \"axe\")", "[0 0Nl 4]");
    // Find returns first occurrence
    TEST_ASSERT_EQ("(find \"aabaa\" 'a')", "0");
    TEST_ASSERT_EQ("(find \"aabaa\" 'b')", "2");

    PASS();
}

// ==================== STRING DISTINCT ====================
test_result_t test_string_distinct() {
    TEST_ASSERT_EQ("(distinct \"test\")", "\"est\"");
    TEST_ASSERT_EQ("(distinct \"\")", "\"\"");
    TEST_ASSERT_EQ("(distinct \"aaa\")", "\"a\"");
    TEST_ASSERT_EQ("(distinct \"abcabc\")", "\"abc\"");
    TEST_ASSERT_EQ("(distinct \"abcde\")", "\"abcde\"");
    // Distinct on symbol vectors
    TEST_ASSERT_EQ("(distinct ['a 'b 'a 'c 'b])", "['a 'b 'c]");
    TEST_ASSERT_EQ("(distinct ['x])", "['x]");
    // Distinct on list of strings
    TEST_ASSERT_EQ("(distinct (list \"hello\" \"world\" \"hello\"))", "(list \"hello\" \"world\")");
    // Count of distinct
    TEST_ASSERT_EQ("(count (distinct \"aabbcc\"))", "3");
    TEST_ASSERT_EQ("(count (distinct ['a 'a 'a]))", "1");

    PASS();
}

// ==================== STRING FILTER ====================
test_result_t test_string_filter() {
    // Filter chars from string with boolean mask
    TEST_ASSERT_EQ("(filter \"hello\" [true false true false true])", "\"hlo\"");
    TEST_ASSERT_EQ("(filter \"hello\" [true true true true true])", "\"hello\"");
    TEST_ASSERT_EQ("(filter \"hello\" [false false false false false])", "\"\"");
    // Filter symbol vectors
    TEST_ASSERT_EQ("(filter ['a 'b 'c 'd] [true false true false])", "['a 'c]");
    TEST_ASSERT_EQ("(filter ['a 'b 'c] [false false false])", "[]");
    // Filter list of strings
    TEST_ASSERT_EQ("(filter (list \"hello\" \"world\" \"foo\") [true false true])", "(list \"hello\" \"foo\")");
    // Length mismatch error
    TEST_ASSERT_ER("(filter \"hello\" [true false])", "length");
    // Filter with like result
    TEST_ASSERT_EQ("(filter (list \"apple\" \"banana\" \"apricot\") (like (list \"apple\" \"banana\" \"apricot\") \"ap*\"))",
                   "(list \"apple\" \"apricot\")");

    PASS();
}

// ==================== STRING TYPE & CAST ====================
test_result_t test_string_type_cast() {
    // Type checking
    TEST_ASSERT_EQ("(type \"hello\")", "'C8");
    TEST_ASSERT_EQ("(type 'a')", "'c8");
    TEST_ASSERT_EQ("(type 'hello)", "'symbol");
    TEST_ASSERT_EQ("(type ['a 'b 'c])", "'SYMBOL");
    // Cast integer to char (ASCII)
    TEST_ASSERT_EQ("(as 'c8 42)", "'*'");
    TEST_ASSERT_EQ("(as 'c8 65)", "'A'");
    // Cast symbol to char (first char)
    TEST_ASSERT_EQ("(as 'c8 'hello)", "'h'");
    // Format produces string representation
    TEST_ASSERT_EQ("(type (format 42))", "'C8");
    // Cast from string to numbers
    TEST_ASSERT_EQ("(as 'i64 \"42\")", "42");
    TEST_ASSERT_EQ("(as 'f64 \"3.14\")", "3.14");
    TEST_ASSERT_EQ("(as 'i16 \"100\")", "100h");
    TEST_ASSERT_EQ("(as 'i32 \"42\")", "42i");
    // Symbol cast
    TEST_ASSERT_EQ("(as 'symbol \"hello\")", "'hello");

    PASS();
}

// ==================== STRING WITH ENLIST/LIST ====================
test_result_t test_string_list_ops() {
    // Enlist string
    TEST_ASSERT_EQ("(enlist \"hello\")", "(list \"hello\")");
    TEST_ASSERT_EQ("(count (enlist \"hello\"))", "1");
    // List of strings
    TEST_ASSERT_EQ("(list \"a\" \"b\" \"c\")", "(list \"a\" \"b\" \"c\")");
    TEST_ASSERT_EQ("(count (list \"a\" \"b\" \"c\"))", "3");
    // Indexing into list of strings
    TEST_ASSERT_EQ("(at (list \"hello\" \"world\") 0)", "\"hello\"");
    TEST_ASSERT_EQ("(at (list \"hello\" \"world\") 1)", "\"world\"");
    // Raze list of strings
    TEST_ASSERT_EQ("(raze (list \"hel\" \"lo\"))", "\"hello\"");
    TEST_ASSERT_EQ("(raze (list \"\" \"\"))", "\"\"");
    TEST_ASSERT_EQ("(raze (list \"a\"))", "\"a\"");
    // Concat of string lists
    TEST_ASSERT_EQ("(concat (list \"a\" \"b\") \"c\")", "(list \"a\" \"b\" \"c\")");
    // Take from list of strings
    TEST_ASSERT_EQ("(take (list \"a\" \"b\" \"c\") 2)", "(list \"a\" \"b\")");
    TEST_ASSERT_EQ("(take (list \"a\" \"b\" \"c\") -1)", "(list \"c\")");
    // Reverse list of strings
    TEST_ASSERT_EQ("(reverse (list \"a\" \"b\" \"c\"))", "(list \"c\" \"b\" \"a\")");

    PASS();
}

// ==================== STRING WITH WHERE/IN ====================
test_result_t test_string_where_in() {
    // In with chars and strings
    TEST_ASSERT_EQ("(in 'a' \"abc\")", "true");
    TEST_ASSERT_EQ("(in 'z' \"abc\")", "false");
    TEST_ASSERT_EQ("(in \"abc\" \"axy\")", "[true false false]");
    TEST_ASSERT_EQ("(in \"abc\" \"abc\")", "[true true true]");
    // In with symbols
    TEST_ASSERT_EQ("(in 'x ['x 'y 'z])", "true");
    TEST_ASSERT_EQ("(in 'w ['x 'y 'z])", "false");
    TEST_ASSERT_EQ("(in ['a 'b] ['a 'c 'd])", "[true false]");
    // Except on symbols
    TEST_ASSERT_EQ("(except ['a 'b 'c 'd] ['b 'd])", "[a c]");
    TEST_ASSERT_EQ("(except ['a 'b] ['x 'y])", "[a b]");
    // Union on symbols
    TEST_ASSERT_EQ("(union ['a 'b] ['b 'c])", "[a b c]");
    // Sect on symbols
    TEST_ASSERT_EQ("(sect ['a 'b 'c] ['b 'c 'd])", "[b c]");
    TEST_ASSERT_EQ("(sect ['a 'b] ['c 'd])", "[]");
    // Where on boolean vector from in (h=0, e=1, o=4)
    TEST_ASSERT_EQ("(where (in \"hello\" \"heo\"))", "[0 1 4]");
    // Where on like results
    TEST_ASSERT_EQ("(where (like (list \"ax\" \"bx\" \"cx\") \"a*\"))", "[0]");

    PASS();
}

// ==================== STRING COMBINED OPERATIONS ====================
test_result_t test_string_combined() {
    // Chain: split then count each
    TEST_ASSERT_EQ("(map count (split \"hello,world,foo\" \",\"))", "[5 5 3]");
    // Chain: split then first
    TEST_ASSERT_EQ("(first (split \"hello,world\" \",\"))", "\"hello\"");
    // Chain: reverse each in a list
    TEST_ASSERT_EQ("(map reverse (list \"abc\" \"def\"))", "(list \"cba\" \"fed\")");
    // Chain: filter + like
    TEST_ASSERT_EQ("(filter (list \"apple\" \"banana\" \"apricot\") (like (list \"apple\" \"banana\" \"apricot\") \"ap*\"))",
                   "(list \"apple\" \"apricot\")");
    // String arithmetic (count is numeric)
    TEST_ASSERT_EQ("(+ (count \"hello\") (count \"world\"))", "10");
    // Concat then take
    TEST_ASSERT_EQ("(take (concat \"hello\" \" world\") 5)", "\"hello\"");
    // Distinct then count
    TEST_ASSERT_EQ("(count (distinct \"mississippi\"))", "4");
    // Group string chars
    TEST_ASSERT_EQ("(count (group \"hello\"))", "4");
    // Find then at
    TEST_ASSERT_EQ("(at \"hello\" (find \"hello\" 'l'))", "'l'");

    PASS();
}

// ==================== SYMBOL INTERN LARGE ====================
test_result_t test_symbols_intern_large() {
    symbols_p symbols = runtime_get()->symbols;
    i64_t i, id;
    char buf[32];

    for (i = 0; i < 20000; i++) {
        i64_t len = snprintf(buf, sizeof(buf), "sym_%lld", (long long)i);
        id = symbols_intern(buf, len);
        TEST_ASSERT(id != NULL_I64, "symbols_intern returned NULL_I64");
    }

    TEST_ASSERT(symbols_count(symbols) >= 20000, "expected >= 20000 symbols");

    for (i = 0; i < 20000; i++) {
        i64_t len = snprintf(buf, sizeof(buf), "sym_%lld", (long long)i);
        id = symbols_intern(buf, len);
        str_p s = str_from_symbol(id);
        TEST_ASSERT(s != NULL, "str_from_symbol returned NULL");
        TEST_ASSERT(str_cmp(s, len, buf, len) == 0, "symbol string mismatch");
    }

    PASS();
}
