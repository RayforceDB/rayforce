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

/*
 * Creates new value_t string from a C string.
 * Guarantees that the string is null-terminated.
 */
extern value_t string(i64_t len)
{
    str_t ptr = rayforce_malloc(len + 1);
    ptr[len] = '\0';

    value_t string = {
        .type = TYPE_STRING,
        .list = {
            .len = len,
            .ptr = ptr,
        },
    };

    return string;
}

/*
 * Creates new value_t string from a C string (aka slice).
 * Not guaranteed to be null-terminated.
 */
extern value_t str(str_t ptr, i64_t len)
{
    value_t string = {
        .type = TYPE_STRING,
        .list = {
            .len = len,
            .ptr = ptr,
        },
    };

    return string;
}

/*
 * match() function takes in two pointers to character arrays: pattern and text.
 * It returns a u8_t value indicating whether the text string matches the pattern string.
 * Note that this implementation assumes that the pattern and text strings do not contain any null characters ('\0').
 * If this is not the case, a more sophisticated implementation may be required.
 */
u8_t match(str_t pat, str_t str)
{
    // u8_t start, end;

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
                if (match(pat, str))
                    return 1;

                str++;
            }
        }

        // exact match
        else if (*pat == '?' || *pat == *str)
        {
            pat++;
            str++;
        }

        // Set of characters
        // else if (*pat == '[')
        // {
        //     start = pat++;
        //     end = strchr(pat, ']');

        //     if (end == NULL || end == pat)
        //         return 0;

        //     u8_t match = 0;

        //     while (*pat != ']')
        //     {
        //         if (*pat == *str)
        //             match = 1;

        //         pat++;
        //     }

        //     if (!match)
        //         return 0;

        //     pat++;
        //     str++;
        // }

        else
            return 0;
    }

    return (*pat == '\0');
}
