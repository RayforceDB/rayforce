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

#include "string.h"
#include "rayforce.h"
#include "heap.h"
#include "util.h"
#include "ops.h"

#define is_digit(c) ((c) >= '0' && (c) <= '9')
#define is_space(c) ((c) == ' ' || (c) == '\t' || (c) == '\n' || (c) == '\r')

// Creates new obj_p string from a C string.
obj_p string_from_str(lit_p str, u64_t len)
{
    obj_p s;

    s = string(len);
    strncpy(as_string(s), str, len);

    return s;
}

// Null terminated string
obj_p cstring_from_str(lit_p str, u64_t len)
{
    obj_p s;

    if (len <= 0)
        return string(0);

    if (str[len - 1] == '\0')
    {
        s = string(len);
        strncpy(as_string(s), str, len);
    }
    else
    {
        s = string(len + 1);
        memcpy(as_string(s), str, len);
        as_string(s)[len] = '\0';
        return s;
    }

    return s;
}

obj_p cstring_from_obj(obj_p obj)
{
    return cstring_from_str(as_string(obj), obj->len);
}

i64_t i64_from_str(lit_p str, u64_t len)
{
    i64_t result = 0, sign = 1;

    if (len == 0)
        return NULL_I64;

    // Skip leading whitespace
    while (is_space(*str))
        str++;

    // Handle optional sign
    if (*str == '-')
    {
        sign = -1;
        str++;
    }

    // Parse the digits
    while (is_digit(*str))
    {
        result = result * 10 + (*str - '0');
        str++;
    }

    return result * sign;
}

f64_t f64_from_str(lit_p str, u64_t len)
{
    f64_t result = 0.0, factor = 1.0, fraction;

    if (len == 0)
        return NULL_F64;

    // Skip leading whitespace
    while (is_space(*str))
        str++;

    // Handle optional sign
    if (*str == '-')
    {
        factor = -1.0;
        str++;
    }

    // Parse the integer part
    while (is_digit(*str))
    {
        result = result * 10.0 + (*str - '0');
        str++;
    }

    // Parse the fractional part
    if (*str == '.')
    {
        str++;
        fraction = 1.0;
        while (is_digit(*str))
        {
            fraction /= 10.0;
            result += (*str - '0') * fraction;
            str++;
        }
    }

    return result * factor;
}

/*
 * Checks if pattern is like *?**literal.
 */
str_p str_chk_from_end(str_p pat)
{
    u64_t l;
    str_p p, s;

    l = strlen(pat);

    if (l == 0)
        return NULL;

    p = pat;

    while (*p == '*' || *p == '?')
        p++;

    if (*p == '\0')
        return p;

    s = p;
    while (*s != '*' && *s != '?' && *s != '[' && *s != '\0')
        s++;

    if (*s == '\0')
        return p;

    return pat;
}
/*
 * Checks if string starts with a literal.
 */
b8_t str_starts_with(str_p str, str_p pat)
{
    u64_t str_len = strlen(str);
    u64_t pat_len = strlen(pat);

    // If the pattern is longer than the string, it can't be a suffix.
    if (pat_len > str_len)
        return B8_FALSE;

    // Compare the tail of the string with the pattern.
    return strcmp(pat, str) == 0;
}

/*
 * Checks if string ends with a literal.
 */
b8_t str_ends_with(str_p str, str_p pat)
{
    // Pointers to the ends of the strings
    const char *s = str, *p = pat;

    // Traverse to the end of both strings
    while (*s)
        ++s;
    while (*p)
        ++p;

    // Check character by character from the end
    while (s >= str && p >= pat)
    {
        if (*s != *p)
        {
            return B8_FALSE;
        }
        s--;
        p--;
    }

    // If we've traversed all of the pattern, but not all of the string
    // This means the string ends with the pattern
    return p < pat;
}

/*
 * match() lambda takes in two pointers to character arrays: pattern and text.
 * It returns a i8_t obj_p indicating whether the text string matches the pattern string.
 * Note that this implementation assumes that the pattern and text strings do not contain any null characters ('\0').
 * If this is not the case, a more sophisticated implementation may be required.
 */
b8_t str_match(str_p str, str_p pat)
{
    str_p s = NULL;
    b8_t inv = B8_FALSE, match = B8_FALSE;

    while (*str)
    {
        switch (*pat)
        {
        case '*':
            while (*pat == '*')
                pat++;

            if (*pat == '\0')
                return B8_TRUE;

            if (*pat != '[' && *pat != '?')
            {
                s = str;
                while (*s && *s != *pat)
                    s++;

                // If character is not found in the rest of the string
                if (!*s)
                    return B8_FALSE;

                str = s; // Move str to the position of the found character
            }
            else
            {
                // If followed by wildcard or set, we'll need to consider every possibility
                s = str;
            }
            break;

        case '?':
            str++;
            pat++;
            break;

        case '[':
            pat++;
            inv = (*pat == '^') ? B8_TRUE : B8_FALSE;
            if (inv)
                pat++;

            match = B8_FALSE;
            while (*pat != ']' && *pat != '\0')
            {
                if (*str == *pat)
                    match = B8_TRUE;
                pat++;
            }

            if (*pat == '\0')
                return B8_FALSE; // unmatched '['

            if ((match && inv) || (!match && !inv))
            {
                // Failed to match with the current character set
                if (!s)
                    return B8_FALSE;

                // Revert back to the position right after the '*'
                str = s + 1;
                pat -= (inv ? 2 : 1); // Go back to '['
            }
            else
            {
                str++;
                pat++;
            }
            break;

        default:
            if (*str != *pat)
            {
                if (!s)
                    return B8_FALSE;

                str = ++s;
                pat -= 2; // To account for the next iteration's increment
            }
            else
            {
                str++;
                pat++;
            }
            break;
        }
    }

    // Check for trailing patterns
    while (*pat)
    {
        if (*pat == '?')
            return B8_FALSE;
        if (*pat++ != '*')
            return B8_FALSE;
        while (*pat == '*')
            pat++;
    }

    return B8_TRUE;
}

u64_t str_len(str_p s, u64_t n)
{
    u64_t i;
    for (i = 0; i < n && s[i] != '\0'; ++i)
        ;
    return i;
}

u64_t str_cpy(str_p dst, str_p src)
{
    u64_t i;
    for (i = 0; src[i] != '\0'; ++i)
        dst[i] = src[i];
    dst[i] = '\0';

    return i;
}

obj_p vn_vstring(lit_p fmt, va_list args)
{
    obj_p res = NULL_OBJ;

    str_vfmt_into(&res, -1, fmt, args);

    return res;
}

obj_p vn_string(lit_p fmt, ...)
{
    obj_p res;
    va_list args;

    va_start(args, fmt);
    res = vn_vstring(fmt, args);
    va_end(args);

    return res;
}

i64_t str_cmp(lit_p lhs, u64_t m, lit_p rhs, u64_t n)
{
    u64_t len = (m < n) ? m : n;
    i64_t res = __builtin_memcmp(lhs, rhs, len);

    if (res == 0)
    {
        if (m < n)
            return -1;
        else if (m > n)
            return 1;
        else
            return 0;
    }

    return res;
}