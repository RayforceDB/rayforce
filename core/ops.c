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
// Initialize the Global null object
struct obj_t __NULL_OBJECT = {.mmod = 0, .type = TYPE_NULL, .attrs = 0, .rc = 0, .len = 0};

/*
 * Treat obj as a b8
 */
b8_t ops_as_b8(obj_p x)
{
    switch (x->type)
    {
    case -TYPE_B8:
        return x->b8;
    case -TYPE_U8:
    case -TYPE_C8:
        return x->u8 != 0;
    case -TYPE_I64:
    case -TYPE_SYMBOL:
    case -TYPE_TIMESTAMP:
        return x->i64 != 0;
    case TYPE_B8:
    case TYPE_U8:
    case TYPE_C8:
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
    case TYPE_LIST:
        return x->len != 0;
    default:
        return B8_TRUE;
    }
}
/*
 * In case of using -Ofast compiler flag, we can not just use x != x due to
 * compiler optimizations. So we need to use memcpy to get the bits of the x
 * and then separate check mantissa and exponent.
 */
b8_t ops_is_nan(f64_t x)
{
    u64_t bits;
    memcpy(&bits, &x, sizeof(x));
    return (bits & 0x7ff0000000000000ull) == 0x7ff0000000000000ull && (bits & 0x000fffffffffffffull) != 0;
}

u64_t ops_rand_u64(nil_t)
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

b8_t ops_eq_idx(obj_p a, i64_t ai, obj_p b, i64_t bi)
{
    obj_p lv, rv;
    b8_t eq;

    switch (mtype2(a->type, b->type))
    {
    case mtype2(TYPE_U8, -TYPE_U8):
    case mtype2(TYPE_C8, -TYPE_C8):
    case mtype2(TYPE_B8, -TYPE_B8):
        return as_u8(a)[ai] == b->u8;
    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_SYMBOL, -TYPE_SYMBOL):
    case mtype2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        return as_i64(a)[ai] == b->i64;
    case mtype2(TYPE_U8, TYPE_U8):
    case mtype2(TYPE_B8, TYPE_B8):
    case mtype2(TYPE_C8, TYPE_C8):
        return as_u8(a)[ai] == as_u8(b)[bi];
    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_SYMBOL, TYPE_SYMBOL):
    case mtype2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
        return as_i64(a)[ai] == as_i64(b)[bi];
    case mtype2(TYPE_F64, -TYPE_F64):
        return as_f64(a)[ai] == b->f64;
    case mtype2(TYPE_F64, TYPE_F64):
        return as_f64(a)[ai] == as_f64(b)[bi];
    case mtype2(TYPE_GUID, -TYPE_GUID):
        return memcmp(as_guid(a) + ai, as_guid(b), sizeof(guid_t)) == 0;
    case mtype2(TYPE_GUID, TYPE_GUID):
        return memcmp(as_guid(a) + ai, as_guid(b) + bi, sizeof(guid_t)) == 0;
        // TODO: figure out how to distinguish between list as column and a list as a value
    case mtype2(TYPE_LIST, TYPE_LIST):
        return cmp_obj(as_list(a)[ai], as_list(b)[bi]) == 0;
    case mtype2(TYPE_ENUM, TYPE_ENUM):
        lv = at_idx(a, ai);
        rv = at_idx(b, bi);
        eq = lv->i64 == rv->i64;
        drop_obj(lv);
        drop_obj(rv);
        return eq;
    case mtype2(TYPE_ANYMAP, TYPE_ANYMAP):
        lv = at_idx(a, ai);
        rv = at_idx(b, bi);
        eq = cmp_obj(lv, rv) == 0;
        drop_obj(lv);
        drop_obj(rv);
        return eq;
    default:
        panic("hash: unsupported type: %d", a->type);
    }
}

u64_t ops_count(obj_p x)
{
    switch (x->type)
    {
    case TYPE_NULL:
        return 0;
    case TYPE_B8:
    case TYPE_U8:
    case TYPE_C8:
    case TYPE_I64:
    case TYPE_F64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
    case TYPE_GUID:
    case TYPE_LIST:
        return x->len;
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
        return 1;
    }
}

/*
 * Returns the rank of an arguments, i.e.
 * if there are at least one vector - it's length, otherwise - 1.
 * In case if there are vectors with different lengths - returns -1.
 */
u64_t ops_rank(obj_p *x, u64_t n)
{
    u64_t i, l = 0xfffffffffffffffful;
    obj_p *b;

    for (i = 0; i < n; i++)
    {
        b = x + i;
        if (is_vector(*b) && l == 0xfffffffffffffffful)
            l = ops_count(*b);
        else if (is_vector(*b) && ops_count(*b) != l)
            return 0xfffffffffffffffful;
    }

    // all are atoms
    if (l == 0xfffffffffffffffful)
        l = 1;

    return l;
}

#if defined(_WIN32) || defined(__CYGWIN__)

obj_p sys_error(os_ray_error_type_t tp, str_p msg)
{
    str_p emsg;
    obj_p err;
    DWORD dw;
    LPVOID lpMsgBuf;

    switch (tp)
    {
    case ERROR_TYPE_OS:
        emsg = str_fmt(-1, "%s: %s", msg, strerror(errno));
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

    emsg = str_fmt(-1, "%s: %s", msg, lpMsgBuf);
    err = error_str(ERR_IO, emsg);
    heap_free(emsg);

    LocalFree(lpMsgBuf);

    return err;
}

#else

obj_p sys_error(os_ray_error_type_t tp, str_p msg)
{
    return error(tp, "'%s': %s", msg, strerror(errno));
}

#endif
