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
bool_t string_match(str_t str, str_t pat)
{
    str_t end;
    i8_t inv = 0, match = 0;

    while (*str != '\0')
    {
        if (*pat == '*' && *(pat + 1) == '\0')
            return 1;

        // Any sequence of characters
        else if (*pat == '*')
        {
            pat++;

            while (*str != '\0')
            {
                if (string_match(pat, str))
                    return 1;

                str++;
            }
        }

        // Set of characters
        else if (*pat == '[')
        {
            pat++;

            // Character '[' itself
            if (*pat == '[')
            {
                if (*pat != *str)
                    return 0;

                pat++;
                str++;
                continue;
            }

            // Inverse characters set
            if (*pat == '^')
            {
                inv = 1;
                pat++;
            }

            end = strchr(pat, ']');

            if (end == NULL || end == pat)
                return 0;

            while (pat < end)
            {
                if (inv && *pat == *str)
                    return 0;
                else if (*pat == *str)
                {
                    match = 1;
                    break;
                }

                pat++;
            }

            if (!(match | inv))
                return 0;

            pat = end + 1;
            str++;
            inv = match = 0;
        }

        // exact match
        else if (*pat == '?' || *pat == *str)
        {
            pat++;
            str++;
        }

        else
            return 0;
    }

    return (*pat == '\0');
}

str_t str_dup(str_t str)
{
    u64_t len = strlen(str);
    str_t dup = heap_alloc(len + 1);
    strncpy(dup, str, len);
    dup[len] = '\0';
    return dup;
}

u64_t string_len(str_t s, u64_t n)
{
    u64_t i;
    for (i = 0; i < n && s[i] != '\0'; ++i)
        ;
    return i;
}