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

#include <time.h>
#include "ops.h"
#include "string.h"
#include "util.h"
#include "hash.h"
#include "heap.h"
#include "serde.h"
#include "items.h"
#include "unary.h"
#include "eval.h"
#include "error.h"

#if defined(_WIN32) || defined(__CYGWIN__)
#include <windows.h>
#else
#include <errno.h>
#endif

__thread u64_t __RND_SEED__ = 0;

/*
 * Treat obj as a bool
 */
bool_t ops_as_bool(obj_t x)
{
    switch (x->type)
    {
    case -TYPE_BOOL:
        return x->bool;
    case -TYPE_BYTE:
    case -TYPE_CHAR:
        return x->u8 != 0;
    case -TYPE_I64:
    case -TYPE_SYMBOL:
    case -TYPE_TIMESTAMP:
        return x->i64 != 0;
    case TYPE_BOOL:
    case TYPE_BYTE:
    case TYPE_CHAR:
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
    case TYPE_LIST:
        return x->len != 0;
    default:
        return true;
    }
}
/*
 * In case of using -Ofast compiler flag, we can not just use x != x due to
 * compiler optimizations. So we need to use memcpy to get the bits of the x
 * and then separate check mantissa and exponent.
 */
bool_t ops_is_nan(f64_t x)
{
    u64_t bits;
    memcpy(&bits, &x, sizeof(x));
    return (bits & 0x7ff0000000000000ull) == 0x7ff0000000000000ull && (bits & 0x000fffffffffffffull) != 0;
}

u64_t ops_rand_u64()
{
    if (__RND_SEED__ == 0)
    {
        // Use a more robust seeding strategy
        __RND_SEED__ = time(NULL) ^ (u64_t)&ops_rand_u64 ^ (u64_t)&time;
    }

    // XORShift algorithm for better randomness
    __RND_SEED__ ^= __RND_SEED__ << 13;
    __RND_SEED__ ^= __RND_SEED__ >> 7;
    __RND_SEED__ ^= __RND_SEED__ << 17;

    return __RND_SEED__;
}

bool_t ops_eq_idx(obj_t a, i64_t ai, obj_t b, i64_t bi)
{
    obj_t lv, rv;
    bool_t eq;

    switch (a->type)
    {
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        return as_i64(a)[ai] == as_i64(b)[bi];
    case TYPE_F64:
        return as_f64(a)[ai] == as_f64(b)[bi];
    case TYPE_CHAR:
        return as_string(a)[ai] == as_string(b)[bi];
    case TYPE_GUID:
        return memcmp(as_guid(a) + ai, as_guid(b) + bi, sizeof(guid_t)) == 0;
    case TYPE_LIST:
        return objcmp(as_list(a)[ai], as_list(b)[bi]) == 0;
    case TYPE_ENUM:
        lv = at_idx(a, ai);
        rv = at_idx(b, bi);
        eq = lv->i64 == rv->i64;
        drop(lv);
        drop(rv);
        return eq;
    case TYPE_ANYMAP:
        lv = at_idx(a, ai);
        rv = at_idx(b, bi);
        eq = objcmp(lv, rv) == 0;
        drop(lv);
        drop(rv);
        return eq;
    default:
        panic("hash: unsupported type: %d", a->type);
    }
}

obj_t ops_find(i64_t x[], u64_t xl, i64_t y[], u64_t yl)
{
    u64_t i, range;
    i64_t max = 0, min = 0, n, p, *r, *f;
    obj_t vec, ht;

    if (xl == 0)
        return vector_i64(0);

    max = min = x[0];

    for (i = 0; i < xl; i++)
    {
        max = (x[i] > max) ? x[i] : max;
        min = (x[i] < min) ? x[i] : min;
    }

    range = max - min + 1;

    if (range <= yl)
    {
        vec = vector_i64(yl + range);
        r = as_i64(vec);
        f = r + yl;

        for (i = 0; i < range; i++)
            f[i] = NULL_I64;

        for (i = 0; i < xl; i++)
        {
            n = x[i] - min;
            f[n] = (f[n] == NULL_I64) ? (i64_t)i : NULL_I64;
        }

        for (i = 0; i < yl; i++)
        {
            n = y[i] - min;
            r[i] = (y[i] < min || y[i] > max) ? NULL_I64 : f[n];
        }

        resize(&vec, yl);

        return vec;
    }

    vec = vector_i64(yl);
    r = as_i64(vec);

    // otherwise, use a hash table
    ht = ht_tab(xl, TYPE_I64);

    for (i = 0; i < xl; i++)
    {
        p = ht_tab_next(&ht, x[i] - min);
        if (as_i64(as_list(ht)[0])[p] == NULL_I64)
        {
            as_i64(as_list(ht)[0])[p] = x[i] - min;
            as_i64(as_list(ht)[1])[p] = i;
        }
    }

    for (i = 0; i < yl; i++)
    {
        p = ht_tab_get(ht, y[i] - min);
        r[i] = p == NULL_I64 ? NULL_I64 : as_i64(as_list(ht)[1])[p];
    }

    drop(ht);

    return vec;
}

u64_t ops_count(obj_t x)
{
    if (!x)
        return 0;

    switch (x->type)
    {
    case TYPE_CHAR:
        return x->len ? x->len - 1 : x->len;
    case TYPE_TABLE:
        return as_list(x)[1]->len ? ops_count(as_list(as_list(x)[1])[0]) : 0;
    case TYPE_DICT:
        return as_list(x)[0]->len;
    case TYPE_ENUM:
        return enum_val(x)->len;
    case TYPE_ANYMAP:
        return anymap_val(x)->len;
    case TYPE_FILTERMAP:
        return as_list(x)[1]->len;
    case TYPE_GROUPMAP:
        return as_list(as_list(x)[1])[0]->i64;
    default:
        return x->len;
    }
}

#if defined(_WIN32) || defined(__CYGWIN__)

obj_t sys_error(os_error_type_t type, str_t msg)
{
    str_t emsg;
    obj_t err;
    DWORD dw;
    LPVOID lpMsgBuf;

    switch (type)
    {
    case ERROR_TYPE_OS:
        emsg = str_fmt(0, "%s: %s", msg, strerror(errno));
        err = error_str(ERR_IO, emsg);
        heap_free(emsg);
        return err;

    case ERROR_TYPE_SOCK:
        dw = WSAGetLastError();
        break;
    default:
        dw = GetLastError();
    }

    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        dw,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&lpMsgBuf,
        0, NULL);

    emsg = str_fmt(0, "%s: %s", msg, lpMsgBuf);
    err = error_str(ERR_IO, emsg);
    heap_free(emsg);

    LocalFree(lpMsgBuf);

    return err;
}

#else

obj_t sys_error(os_error_type_t type, str_t msg)
{
    unused(type);
    return error(ERR_IO, "%s: %s", msg, strerror(errno));
}

#endif
