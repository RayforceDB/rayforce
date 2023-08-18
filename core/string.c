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

/*
 * Creates new obj_tstring from a C string.
 */
obj_t string_from_str(str_t str, i32_t len)
{
    obj_t s = string(len);
    if (len > 0)
        strncpy(as_string(s), str, len);

    return s;
}

/*
 * match() lambda takes in two pointers to character arrays: pattern and text.
 * It returns a i8_t obj_t indicating whether the text string matches the pattern string.
 * Note that this implementation assumes that the pattern and text strings do not contain any null characters ('\0').
 * If this is not the case, a more sophisticated implementation may be required.
 */
bool_t str_match(str_t str, str_t pat)
{
    str_t last_str = NULL, last_pat = NULL;
    bool_t inv;

    while (*str)
    {
        if (*pat == '?' || *str == *pat)
        {
            str++;
            pat++;
        }
        else if (*pat == '*')
        {
            // Skip consecutive '*'
            while (*pat == '*')
                pat++;

            last_str = str;
            last_pat = pat;
        }
        else if (*pat == '[')
        {
            char_t char_class[256] = {0}; // Assuming ASCII, adjust for other character sets
            inv = false;
            pat++;

            if (*pat == '^')
            {
                inv = true;
                pat++;
            }

            while (*pat != ']' && *pat)
            {
                char_class[(i32_t)*pat] = 1;
                pat++;
            }

            if (!*pat)
                return false; // unmatched '['

            if (char_class[(i32_t)*str] == inv)
            {
                if (last_pat)
                {
                    str = ++last_str;
                    pat = last_pat;
                }
                else
                    return false;
            }
            else
            {
                str++;
                pat++;
            }
        }
        else if (last_pat)
        {
            str = ++last_str;
            pat = last_pat;
        }
        else
            return false;
    }

    // Check for trailing patterns
    while (*pat)
    {
        if (*pat == '?')
            return false;
        if (*pat++ != '*')
            return false;
        while (*pat == '*')
            pat++;
    }

    return true;
}

str_t str_dup(str_t str)
{
    u64_t len = strlen(str) + 1;
    str_t dup = heap_alloc(len);
    strncpy(dup, str, len);
    return dup;
}

u64_t str_len(str_t s, u64_t n)
{
    u64_t i;
    for (i = 0; i < n && s[i] != '\0'; ++i)
        ;
    return i;
}

u64_t str_cpy(str_t dst, str_t src)
{
    u64_t i;
    for (i = 0; src[i] != '\0'; ++i)
        dst[i] = src[i];
    dst[i] = '\0';

    return i;
}