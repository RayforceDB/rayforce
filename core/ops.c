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
#include "runtime.h"
#include "pool.h"

__thread i64_t __RND_SEED__ = 0;

// Initialize the Global null object
struct obj_t __NULL_OBJECT = {.mmod = MMOD_INTERNAL, .order = 0, .type = TYPE_NULL, .attrs = 0, .rc = 0, .len = 0};

/*
 * Treat obj as a b8
 */
b8_t ops_as_b8(obj_p x) {
    switch (x->type) {
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

b8_t ops_is_prime(i64_t x) {
    i64_t i;

    if (x <= 1)
        return B8_FALSE;
    if (x <= 3)
        return B8_TRUE;
    if (x % 2 == 0 || x % 3 == 0)
        return B8_FALSE;
    for (i = 5; i * i <= x; i += 6) {
        if (x % i == 0 || x % (i + 2) == 0)
            return B8_FALSE;
    }

    return B8_TRUE;
}

i64_t ops_next_prime(i64_t x) {
    while (!ops_is_prime(x))
        x++;

    return x;
}

i64_t ops_rand_u64(nil_t) {
    if (__RND_SEED__ == 0) {
        // Use a more robust seeding strategy
        __RND_SEED__ = time(NULL) ^ (i64_t)&ops_rand_u64 ^ (i64_t)&time;
    }

    // XORShift algorithm for better randomness
    __RND_SEED__ ^= __RND_SEED__ << 13;
    __RND_SEED__ ^= __RND_SEED__ >> 7;
    __RND_SEED__ ^= __RND_SEED__ << 17;

    return __RND_SEED__;
}

b8_t ops_eq_idx(obj_p a, i64_t ai, obj_p b, i64_t bi) {
    obj_p lv, rv;
    b8_t eq;

    switch (MTYPE2(a->type, b->type)) {
        case MTYPE2(TYPE_U8, -TYPE_U8):
        case MTYPE2(TYPE_C8, -TYPE_C8):
        case MTYPE2(TYPE_B8, -TYPE_B8):
            return AS_U8(a)[ai] == b->u8;
        case MTYPE2(TYPE_I64, -TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, -TYPE_SYMBOL):
        case MTYPE2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
            return AS_I64(a)[ai] == b->i64;
        case MTYPE2(TYPE_U8, TYPE_U8):
        case MTYPE2(TYPE_B8, TYPE_B8):
        case MTYPE2(TYPE_C8, TYPE_C8):
            return AS_U8(a)[ai] == AS_U8(b)[bi];
        case MTYPE2(TYPE_I64, TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, TYPE_SYMBOL):
        case MTYPE2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
            return AS_I64(a)[ai] == AS_I64(b)[bi];
        case MTYPE2(TYPE_F64, -TYPE_F64):
            return AS_F64(a)[ai] == b->f64;
        case MTYPE2(TYPE_F64, TYPE_F64):
            return AS_F64(a)[ai] == AS_F64(b)[bi];
        case MTYPE2(TYPE_GUID, -TYPE_GUID):
            return memcmp(AS_GUID(a) + ai, AS_GUID(b), sizeof(guid_t)) == 0;
        case MTYPE2(TYPE_GUID, TYPE_GUID):
            return memcmp(AS_GUID(a) + ai, AS_GUID(b) + bi, sizeof(guid_t)) == 0;
            // TODO: figure out how to distinguish between list as column and a list as a value
        case MTYPE2(TYPE_LIST, TYPE_LIST):
            return cmp_obj(AS_LIST(a)[ai], AS_LIST(b)[bi]) == 0;
        case MTYPE2(TYPE_ENUM, TYPE_ENUM):
            lv = at_idx(a, ai);
            rv = at_idx(b, bi);
            eq = lv->i64 == rv->i64;
            drop_obj(lv);
            drop_obj(rv);
            return eq;
        case MTYPE2(TYPE_MAPLIST, TYPE_MAPLIST):
            lv = at_idx(a, ai);
            rv = at_idx(b, bi);
            eq = cmp_obj(lv, rv) == 0;
            drop_obj(lv);
            drop_obj(rv);
            return eq;
        default:
            PANIC("hash: unsupported type: %d", a->type);
    }
}

i64_t ops_count(obj_p x) {
    i64_t i, l, c;

    switch (x->type) {
        case TYPE_NULL:
            return 0;
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_C8:
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
        case TYPE_I64:
        case TYPE_F64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
        case TYPE_GUID:
        case TYPE_LIST:
        case TYPE_MAPFD:
            return x->len;
        case TYPE_TABLE:
            return AS_LIST(x)[1]->len ? ops_count(AS_LIST(AS_LIST(x)[1])[0]) : 0;
        case TYPE_DICT:
            return AS_LIST(x)[0]->len;
        case TYPE_ENUM:
            return ENUM_VAL(x)->len;
        case TYPE_MAPLIST:
            return MAPLIST_VAL(x)->len;
        case TYPE_PARTEDLIST:
        case TYPE_PARTEDB8:
        case TYPE_PARTEDU8:
        case TYPE_PARTEDI64:
        case TYPE_PARTEDENUM:
        case TYPE_PARTEDTIMESTAMP:
        case TYPE_PARTEDF64:
        case TYPE_PARTEDGUID:
            l = x->len;
            for (i = 0, c = 0; i < l; i++)
                c += ops_count(AS_LIST(x)[i]);

            return c;
        case TYPE_MAPFILTER:
            return AS_LIST(x)[1]->len;
        case TYPE_MAPGROUP:
            return AS_LIST(AS_LIST(x)[1])[0]->i64;
        case TYPE_MAPCOMMON:
            l = AS_LIST(x)[0]->len;
            for (i = 0, c = 0; i < l; i++)
                c += AS_I64(AS_LIST(x)[1])[i];

            return c;
        default:
            return 1;
    }
}

/*
 * Returns the rank of an arguments, i.e.
 * if there are at least one vector - it's length, otherwise - 1.
 * In case if there are vectors with different lengths - returns -1.
 */
i64_t ops_rank(obj_p *x, i64_t n) {
    i64_t i, l = NULL_I64;
    obj_p *b;

    for (i = 0; i < n; i++) {
        b = x + i;
        if (IS_VECTOR(*b) && l == NULL_I64)
            l = ops_count(*b);
        else if (IS_VECTOR(*b) && ops_count(*b) != l)
            return NULL_I64;
    }

    // all are atoms
    if (l == NULL_I64)
        l = 1;

    return l;
}

// TODO: optimize this via parallel processing
obj_p ops_where(b8_t *mask, i64_t len) {
    i64_t i, j, count;
    i64_t *ids;
    obj_p res;

    count = 0;
    for (i = 0; i < len; i++)
        count += mask[i];

    res = I64(count);
    ids = AS_I64(res);

    for (i = 0, j = 0; i < len; i++) {
        if (mask[i])
            ids[j++] = i;
    }

    return res;
}

#if defined(OS_WINDOWS)

obj_p sys_error(os_ray_error_type_t tp, lit_p msg) {
    obj_p err, emsg;
    DWORD dw;
    LPVOID lpMsgBuf;

    switch (tp) {
        case ERROR_TYPE_OS:
            emsg = str_fmt(-1, "%s: %s", msg, strerror(errno));
            err = error_obj(ERR_IO, emsg);
            return err;

        case ERROR_TYPE_SOCK:
            dw = WSAGetLastError();
            break;
        default:
            dw = GetLastError();
    }

    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                   dw, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&lpMsgBuf, 0, NULL);

    emsg = str_fmt(-1, "%s: %s", msg, lpMsgBuf);
    err = error_obj(ERR_IO, emsg);

    LocalFree(lpMsgBuf);

    return err;
}

#else

obj_p sys_error(os_ray_error_type_t tp, lit_p msg) {
    UNUSED(tp);
    return error(ERR_SYS, "'%s': %s", msg, strerror(errno));
}

#endif
