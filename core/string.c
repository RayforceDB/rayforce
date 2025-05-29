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
#include "error.h"

#define is_digit(c) ((c) >= '0' && (c) <= '9')
#define IS_SPACE(c) ((c) == ' ' || (c) == '\t' || (c) == '\n' || (c) == '\r')

// Creates new obj_p string from a C string.
obj_p string_from_str(lit_p str, i64_t len) {
    obj_p s;

    s = C8(len);
    memcpy(AS_C8(s), str, len);

    return s;
}

// Null terminated string
obj_p cstring_from_str(lit_p str, i64_t len) {
    obj_p s;

    if (len <= 0)
        return C8(0);

    if (str[len - 1] == '\0') {
        s = C8(len);
        strncpy(AS_C8(s), str, len);
    } else {
        s = C8(len + 1);
        memcpy(AS_C8(s), str, len);
        AS_C8(s)[len] = '\0';
        return s;
    }

    return s;
}

obj_p cstring_from_obj(obj_p obj) { return cstring_from_str(AS_C8(obj), obj->len); }

i32_t i32_from_str(lit_p str, i64_t len) {
    i32_t result = 0, sign = 1;

    if (len == 0)
        return NULL_I32;

    // Skip leading whitespace
    while (IS_SPACE(*str))
        str++;

    // Handle optional sign
    if (*str == '-') {
        sign = -1;
        str++;
    }

    // Parse the digits
    while (len > 0 && is_digit(*str)) {
        result = result * 10 + (*str - '0');
        str++;
        len--;
    }

    if (len)
        return NULL_I32;

    return result * sign;
}

i64_t i64_from_str(lit_p str, i64_t len) {
    i64_t result = 0, sign = 1;

    if (len == 0)
        return NULL_I64;

    // Skip leading whitespace
    while (IS_SPACE(*str))
        str++;

    // Handle optional sign
    if (*str == '-') {
        sign = -1;
        str++;
    }

    // Parse the digits
    while (len > 0 && is_digit(*str)) {
        result = result * 10 + (*str - '0');
        str++;
        len--;
    }

    if (len)
        return NULL_I64;

    return result * sign;
}

f64_t f64_from_str(lit_p str, i64_t len) {
    f64_t result = 0.0, factor = 1.0, fraction = 0.1, exp_factor = 1.0;
    i64_t exp;
    b8_t has_digits = B8_FALSE, exp_negative = B8_FALSE;

    if (len == 0 || str == NULL)
        return NULL_F64;

    // Skip leading whitespace
    while (len > 0 && IS_SPACE(*str)) {
        str++;
        len--;
    }

    // Handle optional sign
    if (len > 0 && *str == '-') {
        factor = -1.0;
        str++;
        len--;
    } else if (len > 0 && *str == '+') {
        str++;
        len--;
    }

    // Parse the integer part
    while (len > 0 && is_digit(*str)) {
        result = result * 10.0 + (*str - '0');
        str++;
        len--;
        has_digits = B8_TRUE;
    }

    // Parse the fractional part
    if (len > 0 && *str == '.') {
        str++;
        len--;
        while (len > 0 && is_digit(*str)) {
            result += (*str - '0') * fraction;
            fraction *= 0.1;
            str++;
            len--;
            has_digits = B8_TRUE;
        }
    }

    // Handle exponent
    if (len > 0 && (*str == 'e' || *str == 'E')) {
        str++;
        len--;
        exp = 0;
        exp_negative = B8_FALSE;

        if (len > 0 && *str == '-') {
            exp_negative = B8_TRUE;
            str++;
            len--;
        } else if (len > 0 && *str == '+') {
            str++;
            len--;
        }

        while (len > 0 && is_digit(*str)) {
            exp = exp * 10 + (*str - '0');
            str++;
            len--;
        }

        // Apply exponent
        exp_factor = 1.0;
        while (exp > 0) {
            exp_factor *= exp_negative ? 0.1 : 10.0;
            exp--;
        }
        result *= exp_factor;
    }

    if (has_digits == B8_FALSE)
        return NULL_F64;

    return result * factor;
}

i64_t guid_from_str(lit_p str, i64_t len, guid_t dst) {
    i64_t i, j, k;
    i64_t n;
    u8_t c;

    if (len != 36)
        return -1;

    for (i = 0, j = 0, k = 0; i < len; i++) {
        c = str[i];
        if (c == '-')
            continue;

        if (c >= '0' && c <= '9')
            n = c - '0';
        else if (c >= 'a' && c <= 'f')
            n = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            n = c - 'A' + 10;
        else
            return -1;

        if (j % 2 == 0) {
            dst[k] = n << 4;
        } else {
            dst[k] |= n;
            k++;
        }

        j++;
    }

    return 0;
}

/*
 * Checks if pattern is like *?**literal.
 */
str_p str_chk_from_end(str_p pat) {
    i64_t l;
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
b8_t str_starts_with(str_p str, str_p pat) {
    i64_t str_len = strlen(str);
    i64_t pat_len = strlen(pat);

    // If the pattern is longer than the string, it can't be a suffix.
    if (pat_len > str_len)
        return B8_FALSE;

    // Compare the tail of the string with the pattern.
    return strcmp(pat, str) == 0;
}

/*
 * Checks if string ends with a literal.
 */
b8_t str_ends_with(str_p str, str_p pat) {
    // Pointers to the ends of the strings
    const char *s = str, *p = pat;

    // Traverse to the end of both strings
    while (*s)
        ++s;
    while (*p)
        ++p;

    // Check character by character from the end
    while (s >= str && p >= pat) {
        if (*s != *p) {
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
b8_t str_match(str_p str, i64_t str_len, str_p pat, i64_t pat_len) {
    b8_t inv = B8_FALSE, match = B8_FALSE;
    i64_t str_pos = 0, pat_pos = 0, s_pos;
    i64_t last_star_pat_pos = NULL_I64;  // Track position of last '*' in pattern
    i64_t last_star_str_pos = 0;         // Track corresponding string position

    while (str_pos < str_len) {
        if (pat_pos >= pat_len)
            return B8_FALSE;

        switch (pat[pat_pos]) {
            case '*':
                // Save the position of this star for backtracking
                last_star_pat_pos = pat_pos;
                last_star_str_pos = str_pos;

                // Skip consecutive stars
                while (pat_pos < pat_len && pat[pat_pos] == '*')
                    pat_pos++;

                if (pat_pos == pat_len)
                    return B8_TRUE;

                if (pat[pat_pos] != '[' && pat[pat_pos] != '?') {
                    s_pos = str_pos;
                    while (s_pos < str_len && str[s_pos] != pat[pat_pos])
                        s_pos++;
                    if (s_pos == str_len)
                        return B8_FALSE;
                    str_pos = s_pos;
                }
                break;

            case '?':
                str_pos++;
                pat_pos++;
                break;

            case '[':
                pat_pos++;
                if (pat_pos >= pat_len)
                    return B8_FALSE;
                inv = (pat[pat_pos] == '^') ? B8_TRUE : B8_FALSE;
                if (inv)
                    pat_pos++;
                match = B8_FALSE;
                while (pat_pos < pat_len && pat[pat_pos] != ']') {
                    if (str[str_pos] == pat[pat_pos])
                        match = B8_TRUE;
                    pat_pos++;
                }
                if (pat_pos == pat_len)
                    return B8_FALSE;  // unmatched '['
                if ((match && inv) || (!match && !inv)) {
                    // Mismatch - backtrack to last '*' if possible
                    if (last_star_pat_pos != NULL_I64) {
                        pat_pos = last_star_pat_pos + 1;
                        str_pos = ++last_star_str_pos;
                    } else {
                        return B8_FALSE;
                    }
                } else {
                    str_pos++;
                    pat_pos++;
                }
                break;

            default:
                if (str[str_pos] != pat[pat_pos]) {
                    // Mismatch - backtrack to last '*' if possible
                    if (last_star_pat_pos != NULL_I64) {
                        pat_pos = last_star_pat_pos + 1;
                        str_pos = ++last_star_str_pos;
                    } else {
                        return B8_FALSE;
                    }
                } else {
                    str_pos++;
                    pat_pos++;
                }
                break;
        }
    }

    // Check if remaining pattern consists only of '*'
    while (pat_pos < pat_len && pat[pat_pos] == '*')
        pat_pos++;

    return (pat_pos == pat_len);
}

i64_t str_len(str_p s, i64_t n) {
    i64_t i;
    for (i = 0; i < n && s[i] != '\0'; ++i)
        ;
    return i;
}

i64_t str_cpy(str_p dst, str_p src) {
    i64_t i;
    for (i = 0; src[i] != '\0'; ++i)
        dst[i] = src[i];
    dst[i] = '\0';

    return i;
}

obj_p vn_vc8(lit_p fmt, va_list args) {
    obj_p res = NULL_OBJ;

    str_vfmt_into(&res, -1, fmt, args);

    return res;
}

obj_p vn_c8(lit_p fmt, ...) {
    obj_p res;
    va_list args;

    va_start(args, fmt);
    res = vn_vc8(fmt, args);
    va_end(args);

    return res;
}

i64_t str_cmp(lit_p lhs, i64_t m, lit_p rhs, i64_t n) {
    i64_t len = (m < n) ? m : n;
    i64_t res = __builtin_memcmp(lhs, rhs, len);

    if (res == 0) {
        if (m < n)
            return -1;
        else if (m > n)
            return 1;
        else
            return 0;
    }

    return res;
}

str_p str_rchr(lit_p s, i32_t c, i64_t n) {
    lit_p ptr, last_ptr, found_ptr;

    ptr = s;
    last_ptr = NULL;

    while ((found_ptr = (lit_p)memchr(ptr, c, n - (ptr - s))) != NULL) {
        last_ptr = found_ptr;
        ptr = found_ptr + 1;
    }

    return (str_p)last_ptr;
}

/*
 * Simplefied version of murmurhash
 */
u64_t str_hash(lit_p str, i64_t len) {
    i64_t i;
    u64_t k, k1;
    u64_t hash = 0x1234ABCD1234ABCD;
    u64_t c1 = 0x87c37b91114253d5ULL;
    u64_t c2 = 0x4cf5ad432745937fULL;
    const i32_t r1 = 31;
    const i32_t r2 = 27;
    const u64_t m = 5ULL;
    const u64_t n = 0x52dce729ULL;

    // Process each 8-byte block of the key
    for (i = 0; i + 7 < len; i += 8) {
        k = (u64_t)str[i] | ((u64_t)str[i + 1] << 8) | ((u64_t)str[i + 2] << 16) | ((u64_t)str[i + 3] << 24) |
            ((u64_t)str[i + 4] << 32) | ((u64_t)str[i + 5] << 40) | ((u64_t)str[i + 6] << 48) |
            ((u64_t)str[i + 7] << 56);

        k *= c1;
        k = (k << r1) | (k >> (64 - r1));
        k *= c2;

        hash ^= k;
        hash = ((hash << r2) | (hash >> (64 - r2))) * m + n;
    }

    // Process the tail of the data
    k1 = 0;
    switch (len & 7) {
        case 7:
            k1 ^= ((u64_t)str[i + 6]) << 48;  // fall through
        case 6:
            k1 ^= ((u64_t)str[i + 5]) << 40;  // fall through
        case 5:
            k1 ^= ((u64_t)str[i + 4]) << 32;  // fall through
        case 4:
            k1 ^= ((u64_t)str[i + 3]) << 24;  // fall through
        case 3:
            k1 ^= ((u64_t)str[i + 2]) << 16;  // fall through
        case 2:
            k1 ^= ((u64_t)str[i + 1]) << 8;  // fall through
        case 1:
            k1 ^= ((u64_t)str[i]);
            k1 *= c1;
            k1 = (k1 << r1) | (k1 >> (64 - r1));
            k1 *= c2;
            hash ^= k1;
    }

    // Finalize the hash
    hash ^= len;
    hash ^= (hash >> 33);
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= (hash >> 33);
    hash *= 0xc4ceb9fe1a85ec53ULL;
    hash ^= (hash >> 33);

    return hash;
}

obj_p str_split(lit_p str, i64_t str_len, lit_p delim, i64_t delim_len) {
    i64_t i;
    lit_p start, end;
    obj_p part, last_part, result = NULL_OBJ;

    // Input validation
    if (str == NULL || delim == NULL)
        THROW(ERR_TYPE, "str_split: null pointer");

    if (delim_len == 0)
        THROW(ERR_LENGTH, "str_split: empty delimiter");

    // Create empty list
    result = LIST(0);
    if (IS_ERR(result))
        return result;

    // Handle case where input string is shorter than delimiter
    if (str_len < delim_len) {
        part = string_from_str(str, str_len);
        if (IS_ERR(part)) {
            drop_obj(result);
            return part;
        }
        push_obj(&result, part);
        return result;
    }

    // Special case for single character delimiter
    if (delim_len == 1) {
        start = str;
        while ((end = (str_p)memchr(start, delim[0], str + str_len - start)) != NULL) {
            part = string_from_str(start, end - start);
            if (IS_ERR(part)) {
                drop_obj(result);
                return part;
            }
            push_obj(&result, part);
            start = end + 1;
        }

        // Add the last part
        last_part = string_from_str(start, str + str_len - start);
        if (IS_ERR(last_part)) {
            drop_obj(result);
            return last_part;
        }
        push_obj(&result, last_part);
        return result;
    }

    // For multi-character delimiters
    start = str;
    for (i = 0; i <= str_len - delim_len; i++) {
        if (memcmp(str + i, delim, delim_len) == 0) {
            end = str + i;
            part = string_from_str(start, end - start);
            if (IS_ERR(part)) {
                drop_obj(result);
                return part;
            }
            push_obj(&result, part);
            start = end + delim_len;
            i += delim_len - 1;  // Skip the rest of the delimiter
        }
    }

    // Add the last part
    last_part = string_from_str(start, str + str_len - start);
    if (IS_ERR(last_part)) {
        drop_obj(result);
        return last_part;
    }
    push_obj(&result, last_part);
    return result;
}
