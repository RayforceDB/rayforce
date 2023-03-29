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

#include <string.h>
#include "string.h"
#include "rayforce.h"
#include "alloc.h"
#include "util.h"

/*
 * Creates new rf_object_t string from a C string.
 * Guarantees that the string is null-terminated.
 */
extern rf_object_t string(i64_t len)
{
    rf_object_t string = {
        .type = TYPE_STRING,
        .adt = {
            .len = len,
            .ptr = NULL,
        },
    };

    if (len == 0)
        return string;

    string.adt.ptr = rayforce_malloc(len);
    ((str_t)string.adt.ptr)[len] = '\0';

    return string;
}

/*
 * Creates new rf_object_t string from a C string (aka slice).
 * Not guaranteed to be null-terminated.
 */
extern rf_object_t str(str_t ptr, i64_t len)
{
    rf_object_t string = {
        .type = TYPE_STRING,
        .adt = {
            .len = len,
            .ptr = ptr,
        },
    };

    return string;
}

/*
 * Creates new rf_object_t string from a C string.
 */
rf_object_t string_from_str(str_t str)
{
    i32_t len = strlen(str);
    rf_object_t s = string(len);
    memcpy(s.adt.ptr, str, len);
    return s;
}

/*
 * Creates new rf_object_t string from a constant C string.
 * Does not copy the string, but instead uses the pointer directly.
 */
rf_object_t string_from_const_str(str_t str)
{
    i32_t len = strlen(str);
    rf_object_t string = {
        .type = TYPE_STRING,
        .adt = {
            .len = len,
            .ptr = str,
        },
    };

    return string;
}

/*
 * match() function takes in two pointers to character arrays: pattern and text.
 * It returns a i8_t object indicating whether the text string matches the pattern string.
 * Note that this implementation assumes that the pattern and text strings do not contain any null characters ('\0').
 * If this is not the case, a more sophisticated implementation may be required.
 */
i8_t string_match(str_t str, str_t pat)
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
