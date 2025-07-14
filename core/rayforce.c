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

#include "rayforce.h"
#include <stdio.h>
#include "atomic.h"
#include "binary.h"
#include "error.h"
#include "eval.h"
#include "filter.h"
#include "format.h"
#include "fs.h"
#include "heap.h"
#include "items.h"
#include "lambda.h"
#include "mmap.h"
#include "ops.h"
#include "runtime.h"
#include "serde.h"
#include "sock.h"
#include "string.h"
#include "sys.h"
#include "unary.h"
#include "util.h"
#include "fdmap.h"
#include "date.h"
#include "time.h"
#include "timestamp.h"
#include "cmp.h"

RAYASSERT(sizeof(struct obj_t) == 16, rayforce_h)

// Synchronization flag (use atomics on rc operations).
__thread i64_t __RC_SYNC = 0;

i32_t ray_init(nil_t) {
    runtime_p runtime;

    runtime = runtime_create(0, NULL);

    if (runtime == NULL)
        return -1;

    return 0;
}

nil_t ray_clean(nil_t) { runtime_destroy(); }

u8_t version(nil_t) { return RAYFORCE_VERSION; }

nil_t zero_obj(obj_p obj) {
    i64_t size;

    size = size_of(obj) - sizeof(struct obj_t);
    memset(obj->raw, 0, size);
}

obj_p atom(i8_t type) {
    obj_p a;

    a = (obj_p)heap_alloc(sizeof(struct obj_t));

    if (a == NULL)
        PANIC("oom");

    a->mmod = MMOD_INTERNAL;
    a->type = -type;
    a->rc = 1;
    a->attrs = 0;

    return a;
}

obj_p null(i8_t type) {
    switch (type) {
        case TYPE_B8:
            return b8(B8_FALSE);
        case TYPE_I64:
            return i64(NULL_I64);
        case TYPE_F64:
            return f64(NULL_F64);
        case TYPE_C8:
            return c8('\0');
        case TYPE_SYMBOL:
            return symboli64(NULL_I64);
        case TYPE_TIMESTAMP:
            return timestamp(NULL_I64);
        default:
            return NULL_OBJ;
    }
}

obj_p nullv(i8_t type, i64_t len) {
    i64_t i;
    obj_p vec;
    i8_t t;

    if (type == TYPE_C8)
        t = TYPE_LIST;
    else if (type < 0)
        t = -type;
    else
        t = type;

    vec = vector(t, len);

    switch (t) {
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_C8:
            memset(vec->raw, 0, len);
            break;
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
            for (i = 0; i < len; i++)
                AS_I64(vec)[i] = NULL_I64;
            break;
        case TYPE_F64:
            for (i = 0; i < len; i++)
                AS_F64(vec)[i] = NULL_F64;
            break;
        case TYPE_GUID:
            for (i = 0; i < len; i++)
                memset(AS_GUID(vec)[i], 0, sizeof(guid_t));
            break;
        case TYPE_LIST:
            for (i = 0; i < len; i++)
                AS_LIST(vec)[i] = NULL_OBJ;
            break;
        default:
            memset(vec->raw, 0, len * size_of_type(t));
            break;
    }

    return vec;
}

obj_p b8(b8_t val) {
    obj_p b = atom(TYPE_B8);
    b->b8 = val;
    return b;
}

obj_p u8(u8_t val) {
    obj_p b = atom(TYPE_U8);
    b->u8 = val;
    return b;
}

obj_p i16(i16_t val) {
    obj_p i = atom(TYPE_I16);
    i->i16 = val;
    return i;
}

obj_p i32(i32_t val) {
    obj_p i = atom(TYPE_I32);
    i->i32 = val;
    return i;
}

obj_p i64(i64_t val) {
    obj_p i = atom(TYPE_I64);
    i->i64 = val;
    return i;
}

obj_p f64(f64_t val) {
    obj_p f = atom(TYPE_F64);
    f->f64 = val;
    return f;
}

obj_p symbol(lit_p s, i64_t n) {
    i64_t id = symbols_intern(s, n);
    obj_p a = atom(TYPE_SYMBOL);
    a->i64 = id;

    return a;
}

obj_p symboli64(i64_t id) {
    obj_p a = atom(TYPE_SYMBOL);
    a->i64 = id;

    return a;
}

obj_p guid(const guid_t buf) {
    obj_p guid = vector(TYPE_I64, 2);
    guid->type = -TYPE_GUID;

    if (buf == NULL)
        memset(*AS_GUID(guid), 0, sizeof(guid_t));
    else
        memcpy(AS_GUID(guid)[0], buf, sizeof(guid_t));

    return guid;
}

obj_p c8(c8_t c) {
    obj_p s = atom(TYPE_C8);
    s->c8 = c;
    return s;
}

obj_p adate(i32_t val) {
    obj_p t = atom(TYPE_DATE);
    t->i32 = val;
    return t;
}

obj_p atime(i32_t val) {
    obj_p t = atom(TYPE_TIME);
    t->i32 = val;
    return t;
}

obj_p timestamp(i64_t val) {
    obj_p t = atom(TYPE_TIMESTAMP);
    t->i64 = val;
    return t;
}

obj_p vector(i8_t type, i64_t len) {
    i8_t t;
    obj_p vec;

    if (type < 0)
        t = -type;
    else if (type > 0 && type < TYPE_ENUM)
        t = type;
    else if (type == TYPE_ENUM)
        t = TYPE_SYMBOL;
    else
        t = TYPE_LIST;

    vec = (obj_p)heap_alloc(sizeof(struct obj_t) + len * size_of_type(t));

    if (vec == NULL)
        THROW(ERR_HEAP, "oom");

    vec->mmod = MMOD_INTERNAL;
    vec->type = t;
    vec->rc = 1;
    vec->len = len;
    vec->attrs = 0;

    return vec;
}

obj_p vn_symbol(i64_t len, ...) {
    i64_t i;
    i64_t sym, *syms;
    obj_p res;
    str_p s;
    va_list args;

    res = SYMBOL(len);
    syms = AS_SYMBOL(res);

    va_start(args, len);

    for (i = 0; i < len; i++) {
        s = va_arg(args, str_p);
        sym = symbols_intern(s, strlen(s));
        syms[i] = sym;
    }

    va_end(args);

    return res;
}

obj_p vn_list(i64_t len, ...) {
    i64_t i;
    obj_p l;
    va_list args;

    l = (obj_p)heap_alloc(sizeof(struct obj_t) + sizeof(obj_p) * len);
    l->mmod = MMOD_INTERNAL;
    l->type = TYPE_LIST;
    l->rc = 1;
    l->len = len;
    l->attrs = 0;

    va_start(args, len);

    for (i = 0; i < len; i++)
        AS_LIST(l)[i] = va_arg(args, obj_p);

    va_end(args);

    return l;
}

obj_p dict(obj_p keys, obj_p vals) {
    obj_p d;

    d = vn_list(2, keys, vals);
    d->type = TYPE_DICT;

    return d;
}

obj_p table(obj_p keys, obj_p vals) {
    obj_p t;

    t = vn_list(2, keys, vals);
    t->type = TYPE_TABLE;

    return t;
}

obj_p enumerate(obj_p sym, obj_p vec) {
    obj_p e;

    e = vn_list(2, sym, vec);
    e->type = TYPE_ENUM;

    return e;
}

obj_p anymap(obj_p sym, obj_p vec) {
    obj_p e;

    e = vn_list(2, sym, vec);
    e->type = TYPE_MAPLIST;

    return e;
}

obj_p resize_obj(obj_p *obj, i64_t len) {
    i64_t elem_size, obj_size;
    obj_p new_obj;

    DEBUG_ASSERT(IS_VECTOR(*obj), "resize: invalid type: %d", (*obj)->type);

    if ((*obj)->len == len)
        return *obj;

    elem_size = size_of_type((*obj)->type);

    // calculate size of vector with new length
    obj_size = sizeof(struct obj_t) + len * elem_size;

    // printf("LEN: %lld ELEM SIZE: %lld OBJ SIZE: %lld NEW OBJ SIZE: %lld\n", len, elem_size,
    //        sizeof(struct obj_t) + (*obj)->len * elem_size, obj_size);

    if (IS_INTERNAL(*obj))
        *obj = (obj_p)heap_realloc(*obj, obj_size);
    else {
        new_obj = (obj_p)heap_alloc(obj_size);
        memcpy(new_obj, *obj, sizeof(struct obj_t) + len * elem_size);
        heap_free(*obj);
        *obj = new_obj;
    }

    (*obj)->len = len;

    return *obj;
}

obj_p push_raw(obj_p *obj, raw_p val) {
    i64_t off, req, size;
    obj_p new_obj;

    size = size_of_type((*obj)->type);
    off = (*obj)->len * size;
    req = sizeof(struct obj_t) + off + size;

    if (IS_INTERNAL(*obj))
        *obj = (obj_p)heap_realloc(*obj, req);
    else {
        new_obj = (obj_p)heap_alloc(req);
        memcpy(new_obj->raw, (*obj)->raw, off);
        new_obj->mmod = MMOD_INTERNAL;
        new_obj->type = (*obj)->type;
        new_obj->rc = 1;
        drop_obj(*obj);
        *obj = new_obj;
    }

    memcpy((*obj)->raw + off, val, size);
    (*obj)->len++;

    return *obj;
}

obj_p push_obj(obj_p *obj, obj_p val) {
    i64_t i, l;
    obj_p res, lst = NULL_OBJ;

    // change object type to a coresponding list
    if ((*obj)->type != TYPE_LIST && (*obj)->type != -val->type) {
        l = ops_count(*obj);

        if (val->type < 0 && val->type == (*obj)->type)
            lst = vector(val->type, l + 1);
        else
            lst = LIST(l + 1);

        for (i = 0; i < l; i++)
            ins_obj(&lst, i, at_idx(*obj, i));

        ins_obj(&lst, i, val);

        drop_obj(*obj);
        *obj = lst;

        return lst;
    }

    switch (MTYPE2((*obj)->type, val->type)) {
        case MTYPE2(TYPE_I64, -TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, -TYPE_SYMBOL):
        case MTYPE2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
            res = push_raw(obj, &val->i64);
            drop_obj(val);
            return res;
        case MTYPE2(TYPE_F64, -TYPE_F64):
            res = push_raw(obj, &val->f64);
            drop_obj(val);
            return res;
        case MTYPE2(TYPE_C8, -TYPE_C8):
            res = push_raw(obj, &val->c8);
            drop_obj(val);
            return res;
        case MTYPE2(TYPE_GUID, -TYPE_GUID):
            res = push_raw(obj, AS_GUID(val));
            drop_obj(val);
            return res;
        default:
            if ((*obj)->type == TYPE_LIST)
                return push_raw(obj, &val);

            THROW(ERR_TYPE, "push_obj: invalid types: '%s, '%s", type_name((*obj)->type), type_name(val->type));
    }
}

obj_p append_list(obj_p *obj, obj_p vals) {
    i64_t i, c, l, size1, size2;
    obj_p res;

    switch (MTYPE2((*obj)->type, vals->type)) {
        case MTYPE2(TYPE_I64, TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, TYPE_SYMBOL):
        case MTYPE2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
            size1 = size_of(*obj) - sizeof(struct obj_t);
            size2 = size_of(vals) - sizeof(struct obj_t);
            res = resize_obj(obj, (*obj)->len + vals->len);
            memcpy((*obj)->raw + size1, AS_I64(vals), size2);
            return res;
        case MTYPE2(TYPE_F64, TYPE_F64):
            size1 = size_of(*obj) - sizeof(struct obj_t);
            size2 = size_of(vals) - sizeof(struct obj_t);
            res = resize_obj(obj, (*obj)->len + vals->len);
            memcpy((*obj)->raw + size1, AS_F64(vals), size2);
            return res;
        case MTYPE2(TYPE_C8, TYPE_C8):
            size1 = size_of(*obj) - sizeof(struct obj_t);
            size2 = size_of(vals) - sizeof(struct obj_t);
            res = resize_obj(obj, (*obj)->len + vals->len);
            memcpy((*obj)->raw + size1, AS_C8(vals), size2);
            return res;
        case MTYPE2(TYPE_GUID, TYPE_GUID):
            size1 = size_of(*obj) - sizeof(struct obj_t);
            size2 = size_of(vals) - sizeof(struct obj_t);
            res = resize_obj(obj, (*obj)->len + vals->len);
            memcpy((*obj)->raw + size1, AS_GUID(vals), size2);
            return res;
        default:
            if ((*obj)->type == TYPE_LIST) {
                l = (*obj)->len;
                c = ops_count(vals);
                res = resize_obj(obj, l + c);
                for (i = 0; i < c; i++)
                    AS_LIST(res)[l + i] = at_idx(vals, i);

                return res;
            }

            THROW(ERR_TYPE, "push_obj: invalid types: '%s, '%s", type_name((*obj)->type), type_name(vals->type));
    }
}

obj_p unify_list(obj_p *obj) {
    i64_t i, l;
    obj_p res;
    i8_t type;

    switch ((*obj)->type) {
        case TYPE_LIST:
            l = (*obj)->len;
            if (l == 0)
                return *obj;

            type = AS_LIST(*obj)[0]->type;
            if (type >= 0)
                return *obj;

            res = vector(type, l);
            ins_obj(&res, 0, clone_obj(AS_LIST(*obj)[0]));

            for (i = 1; i < l; i++) {
                if (AS_LIST(*obj)[i]->type != type) {
                    drop_obj(res);
                    return *obj;
                }

                ins_obj(&res, i, clone_obj(AS_LIST(*obj)[i]));
            }

            drop_obj(*obj);
            *obj = res;

            return *obj;
        default:
            return *obj;
    }
}

obj_p diverse_obj(obj_p *obj) {
    i64_t i, l;
    obj_p res;

    switch ((*obj)->type) {
        case TYPE_U8:
        case TYPE_B8:
        case TYPE_I16:
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
        case TYPE_I64:
        case TYPE_TIMESTAMP:
        case TYPE_SYMBOL:
        case TYPE_F64:
        case TYPE_C8:
        case TYPE_GUID:
            l = (*obj)->len;
            res = LIST(l);
            for (i = 0; i < l; i++)
                AS_LIST(res)[i] = at_idx(*obj, i);

            drop_obj(*obj);
            *obj = res;
            return res;
        default:
            res = LIST(1);
            AS_LIST(res)[0] = *obj;
            *obj = res;
            return res;
    }
}

obj_p push_sym(obj_p *obj, lit_p str) {
    i64_t sym;

    sym = symbols_intern(str, strlen(str));

    return push_raw(obj, &sym);
}

obj_p ins_raw(obj_p *obj, i64_t idx, raw_p val) {
    i32_t size;

    size = size_of_type((*obj)->type);
    memcpy((*obj)->raw + idx * size, val, size);

    return *obj;
}

obj_p ins_obj(obj_p *obj, i64_t idx, obj_p val) {
    i64_t i;
    i64_t l;
    obj_p ret;

    if (!IS_VECTOR(*obj))
        return *obj;

    // we need to convert vector to list
    if (((*obj)->type - (-val->type) != 0) && (*obj)->type != TYPE_LIST) {
        l = ops_count(*obj);
        ret = LIST(l);

        for (i = 0; i < idx; i++)
            AS_LIST(ret)[i] = at_idx(*obj, i);

        AS_LIST(ret)[idx] = val;

        drop_obj(*obj);

        *obj = ret;

        return ret;
    }

    switch ((*obj)->type) {
        case TYPE_B8:
            ret = ins_raw(obj, idx, &val->b8);
            drop_obj(val);
            break;
        case TYPE_U8:
            ret = ins_raw(obj, idx, &val->u8);
            drop_obj(val);
            break;
        case TYPE_I16:
            ret = ins_raw(obj, idx, &val->i16);
            drop_obj(val);
            break;
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            ret = ins_raw(obj, idx, &val->i32);
            drop_obj(val);
            break;
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
            ret = ins_raw(obj, idx, &val->i64);
            drop_obj(val);
            break;
        case TYPE_F64:
            ret = ins_raw(obj, idx, &val->f64);
            drop_obj(val);
            break;
        case TYPE_C8:
            ret = ins_raw(obj, idx, &val->c8);
            drop_obj(val);
            break;
        case TYPE_GUID:
            ret = ins_raw(obj, idx, AS_GUID(val));
            drop_obj(val);
            break;
        case TYPE_LIST:
            ret = ins_raw(obj, idx, &val);
            break;
        default:
            PANIC("ins_obj: invalid type: '%s", type_name((*obj)->type));
    }

    return ret;
}

obj_p ins_sym(obj_p *obj, i64_t idx, lit_p str) {
    i64_t sym;

    sym = symbols_intern(str, strlen(str));

    return ins_raw(obj, idx, &sym);
}

obj_p at_idx(obj_p obj, i64_t idx) {
    i64_t size;
    i64_t i, m, n, l;
    obj_p k, v, res;
    u8_t *buf;

    if (idx == NULL_I64)
        return null(obj->type);

    switch (obj->type) {
        case TYPE_I16:
            if (idx < 0)
                idx = obj->len + idx;
            if (idx >= 0 && idx < (i64_t)obj->len)
                return i16(AS_I16(obj)[idx]);
            return i16(NULL_I16);
        case TYPE_I32:
            if (idx < 0)
                idx = obj->len + idx;
            if (idx >= 0 && idx < (i64_t)obj->len)
                return i32(AS_I32(obj)[idx]);
            return i32(NULL_I32);
        case TYPE_DATE:
            if (idx < 0)
                idx = obj->len + idx;
            if (idx >= 0 && idx < (i64_t)obj->len)
                return adate(AS_I32(obj)[idx]);
            return adate(NULL_I32);
        case TYPE_TIME:
            if (idx < 0)
                idx = obj->len + idx;
            if (idx >= 0 && idx < (i64_t)obj->len)
                return atime(AS_I32(obj)[idx]);
            return atime(NULL_I32);
        case TYPE_I64:
            if (idx < 0)
                idx = obj->len + idx;
            if (idx >= 0 && idx < (i64_t)obj->len)
                return i64(AS_I64(obj)[idx]);
            return i64(NULL_I64);
        case TYPE_SYMBOL:
            if (idx < 0)
                idx = obj->len + idx;
            if (idx >= 0 && idx < (i64_t)obj->len)
                return symboli64(AS_SYMBOL(obj)[idx]);
            return symboli64(NULL_I64);
        case TYPE_TIMESTAMP:
            if (idx < 0)
                idx = obj->len + idx;
            if (idx >= 0 && idx < (i64_t)obj->len)
                return timestamp(AS_TIMESTAMP(obj)[idx]);
            return timestamp(NULL_I64);
        case TYPE_F64:
            if (idx < 0)
                idx = obj->len + idx;
            if (idx >= 0 && idx < (i64_t)obj->len)
                return f64(AS_F64(obj)[idx]);
            return f64(NULL_F64);
        case TYPE_C8:
            l = ops_count(obj);
            if (idx < 0)
                idx = l + idx;
            if (idx >= 0 && idx < (i64_t)l)
                return c8(AS_C8(obj)[idx]);
            return c8('\0');
        case TYPE_LIST:
            if (idx < 0)
                idx = obj->len + idx;
            if (idx >= 0 && idx < (i64_t)obj->len)
                return clone_obj(AS_LIST(obj)[idx]);
            return NULL_OBJ;
        case TYPE_GUID:
            if (idx < 0)
                idx = obj->len + idx + 1;
            if (idx >= 0 && idx < (i64_t)obj->len)
                return guid(AS_GUID(obj)[idx]);
            return NULL_OBJ;
        case TYPE_ENUM:
            if (idx < 0)
                idx = obj->len + idx;
            if (idx >= 0 && idx < (i64_t)ENUM_VAL(obj)->len) {
                k = ray_key(obj);
                if (IS_ERR(k))
                    return k;
                v = ray_get(k);
                drop_obj(k);
                if (IS_ERR(v))
                    return v;
                idx = AS_I64(ENUM_VAL(obj))[idx];
                res = at_idx(v, idx);
                drop_obj(v);
                return res;
            }
            return symboli64(NULL_I64);

        case TYPE_MAPLIST:
            k = MAPLIST_KEY(obj);
            v = MAPLIST_VAL(obj);
            size = k->len;
            if (idx < 0)
                idx = obj->len + idx;
            if (idx >= 0 && idx < (i64_t)v->len) {
                buf = AS_U8(k) + AS_I64(v)[idx];
                return de_raw(buf, &size);
            }

            return NULL_OBJ;

        case TYPE_TABLE:
            n = AS_LIST(obj)[0]->len;
            v = LIST(n);
            l = ops_count(obj);
            if (idx < 0)
                idx = l + idx;
            if (idx >= 0 && idx < (i64_t)l) {
                for (i = 0; i < n; i++) {
                    k = at_idx(AS_LIST(AS_LIST(obj)[1])[i], idx);
                    if (IS_ERR(k)) {
                        v->len = i;
                        drop_obj(v);
                        return k;
                    }
                    ins_obj(&v, i, k);
                }
                return dict(clone_obj(AS_LIST(obj)[0]), v);
            }

            for (i = 0; i < n; i++) {
                k = null(AS_LIST(AS_LIST(obj)[1])[i]->type);
                ins_obj(&v, i, k);
            }

            return dict(clone_obj(AS_LIST(obj)[0]), v);

        case TYPE_PARTEDLIST:
            l = obj->len;
            for (i = 0, n = 0; i < l; i++) {
                m = AS_LIST(obj)[i]->len;
                n += m;
                if (idx < n)
                    return at_idx(AS_LIST(obj)[i], m - (n - idx));
            }

            return NULL_OBJ;

        case TYPE_PARTEDB8:
        case TYPE_PARTEDU8:
            l = obj->len;
            for (i = 0, n = 0; i < l; i++) {
                m = AS_LIST(obj)[i]->len;
                n += m;
                if (idx < n)
                    return b8(AS_U8(AS_LIST(obj)[i])[m - (n - idx)]);
            }

            return b8(B8_FALSE);

        case TYPE_PARTEDI64:
        case TYPE_PARTEDTIMESTAMP:
            l = obj->len;
            for (i = 0, n = 0; i < l; i++) {
                m = AS_LIST(obj)[i]->len;
                n += m;
                if (idx < n) {
                    res = atom(obj->type - TYPE_PARTEDLIST);
                    res->i64 = AS_I64(AS_LIST(obj)[i])[m - (n - idx)];
                    return res;
                }
            }

            return null(obj->type - TYPE_PARTEDLIST);

        case TYPE_PARTEDENUM:
            l = obj->len;
            for (i = 0, n = 0; i < l; i++) {
                m = AS_LIST(obj)[i]->len;
                n += m;
                if (idx < n) {
                    k = ray_key(AS_LIST(obj)[i]);
                    if (IS_ERR(k))
                        return k;
                    v = ray_get(k);
                    drop_obj(k);
                    if (IS_ERR(v))
                        return v;
                    idx = AS_I64(ENUM_VAL(AS_LIST(obj)[i]))[m - (n - idx)];
                    res = at_idx(v, idx);
                    drop_obj(v);
                    return res;
                }
            }

            return symboli64(NULL_I64);

        case TYPE_PARTEDF64:
            l = obj->len;
            for (i = 0, n = 0; i < l; i++) {
                m = AS_LIST(obj)[i]->len;
                n += m;
                if (idx < n)
                    return f64(AS_F64(AS_LIST(obj)[i])[m - (n - idx)]);
            }

            return f64(NULL_F64);

        case TYPE_PARTEDGUID:
            l = obj->len;
            for (i = 0, n = 0; i < l; i++) {
                m = AS_LIST(obj)[i]->len;
                n += m;
                if (idx < n)
                    return guid(AS_GUID(AS_LIST(obj)[i])[m - (n - idx)]);
            }

            return guid(NULL);

        case TYPE_MAPCOMMON:
            l = AS_LIST(obj)[0]->len;
            for (i = 0, n = 0; i < l; i++) {
                m = AS_I64(AS_LIST(obj)[1])[i];
                n += m;
                if (idx < n)
                    return at_idx(AS_LIST(obj)[0], i);
            }

            return null(AS_LIST(obj)[0]->type);

        default:
            return clone_obj(obj);
    }
}

obj_p at_ids(obj_p obj, i64_t ids[], i64_t len) {
    i64_t i, xl;
    i64_t mapid, m, n;
    u8_t *u8inp, *u8out;
    i16_t *i16inp, *i16out;
    i32_t *i32inp, *i32out;
    i64_t *i64inp, *i64out;
    f64_t *f64inp, *f64out;
    obj_p k, v, cols, res;

    switch (obj->type) {
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_C8:
            res = vector(obj->type, len);
            u8inp = AS_U8(obj);
            u8out = AS_U8(res);
            for (i = 0; i < len; i++)
                u8out[i] = u8inp[ids[i]];

            return res;
        case TYPE_I16:
            res = I16(len);
            i16inp = AS_I16(obj);
            i16out = AS_I16(res);
            for (i = 0; i < len; i++)
                i16out[i] = i16inp[ids[i]];

            return res;
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            res = vector(obj->type, len);
            i32inp = AS_I32(obj);
            i32out = AS_I32(res);
            for (i = 0; i < len; i++)
                i32out[i] = i32inp[ids[i]];

            return res;
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
            res = vector(obj->type, len);
            i64inp = AS_I64(obj);
            i64out = AS_I64(res);
            for (i = 0; i < len; i++)
                i64out[i] = i64inp[ids[i]];

            return res;
        case TYPE_F64:
            res = F64(len);
            f64inp = AS_F64(obj);
            f64out = AS_F64(res);
            for (i = 0; i < len; i++)
                f64out[i] = f64inp[ids[i]];

            return res;
        case TYPE_GUID:
            res = vector(TYPE_GUID, len);
            for (i = 0; i < len; i++)
                memcpy(AS_GUID(res)[i], AS_GUID(obj)[ids[i]], sizeof(guid_t));

            return res;
        case TYPE_LIST:
            res = LIST(len);
            for (i = 0; i < len; i++)
                AS_LIST(res)[i] = clone_obj(AS_LIST(obj)[ids[i]]);

            return res;
        case TYPE_ENUM:
            k = ray_key(obj);
            if (IS_ERR(k))
                return k;

            v = ray_get(k);
            drop_obj(k);

            if (IS_ERR(v))
                return v;

            if (v->type != TYPE_SYMBOL)
                return error(ERR_TYPE, "enum: '%s' is not a 'Symbol'", type_name(v->type));

            res = SYMBOL(len);
            for (i = 0; i < len; i++)
                AS_I64(res)[i] = AS_I64(v)[AS_I64(ENUM_VAL(obj))[ids[i]]];

            drop_obj(v);

            return res;
        case TYPE_TABLE:
            xl = AS_LIST(obj)[0]->len;
            cols = LIST(xl);
            for (i = 0; i < xl; i++) {
                k = at_ids(AS_LIST(AS_LIST(obj)[1])[i], ids, len);

                // if (IS_ATOM(c))
                //     c = ray_enlist(&c, 1);

                if (IS_ERR(k)) {
                    cols->len = i;
                    drop_obj(cols);
                    return k;
                }

                ins_obj(&cols, i, k);
            }

            return table(clone_obj(AS_LIST(obj)[0]), cols);
        case TYPE_PARTEDB8:
        case TYPE_PARTEDU8:
            res = vector(obj->type - TYPE_MAPLIST, len);
            n = AS_LIST(obj)[0]->len;
            for (i = 0, mapid = 0, m = 0; i < len; i++) {
                while (ids[i] >= n) {
                    m = n;
                    n += ops_count(AS_LIST(obj)[++mapid]);
                }
                AS_U8(res)[i] = AS_U8(AS_LIST(obj)[mapid])[ids[i] - m];
            }

            return res;
        case TYPE_PARTEDI64:
        case TYPE_PARTEDTIMESTAMP:
            res = vector(obj->type - TYPE_MAPLIST, len);
            n = AS_LIST(obj)[0]->len;
            for (i = 0, mapid = 0, m = 0; i < len; i++) {
                while (ids[i] >= n) {
                    m = n;
                    n += AS_LIST(obj)[++mapid]->len;
                }
                AS_I64(res)[i] = AS_I64(AS_LIST(obj)[mapid])[ids[i] - m];
            }

            return res;
        case TYPE_PARTEDF64:
            res = F64(len);
            n = AS_LIST(obj)[0]->len;
            for (i = 0, mapid = 0, m = 0; i < len; i++) {
                while (ids[i] >= n) {
                    m = n;
                    n += AS_LIST(obj)[++mapid]->len;
                }
                AS_F64(res)[i] = AS_F64(AS_LIST(obj)[mapid])[ids[i] - m];
            }

            return res;
        case TYPE_PARTEDENUM:
            k = ray_key(AS_LIST(obj)[0]);
            if (IS_ERR(k))
                return k;

            v = ray_get(k);
            drop_obj(k);

            if (IS_ERR(v))
                return v;

            if (v->type != TYPE_SYMBOL)
                return error(ERR_TYPE, "enum: '%s' is not a 'Symbol'", type_name(v->type));

            res = SYMBOL(len);
            n = AS_LIST(obj)[0]->len;
            for (i = 0, mapid = 0, m = 0; i < len; i++) {
                while (ids[i] >= n) {
                    m = n;
                    n += AS_LIST(obj)[++mapid]->len;
                }
                AS_I64(res)[i] = AS_I64(v)[AS_I64(ENUM_VAL(AS_LIST(obj)[mapid]))[ids[i] - m]];
            }

            drop_obj(v);
            return res;
        case TYPE_PARTEDGUID:
            res = GUID(len);
            n = AS_LIST(obj)[0]->len;
            for (i = 0, mapid = 0, m = 0; i < len; i++) {
                while (ids[i] >= n) {
                    m = n;
                    n += AS_LIST(obj)[++mapid]->len;
                }
                memcpy(AS_GUID(res)[i], AS_GUID(AS_LIST(obj)[mapid])[ids[i] - m], sizeof(guid_t));
            }

            return res;

        default:
            res = vector(TYPE_LIST, len);
            for (i = 0; i < len; i++)
                ins_obj(&res, i, at_idx(obj, ids[i]));

            return res;
    }
}

obj_p at_obj(obj_p obj, obj_p idx) {
    i64_t i, n, l;
    i64_t j, *ids;
    obj_p v;

    switch (MTYPE2(obj->type, idx->type)) {
        case MTYPE2(TYPE_I64, -TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, -TYPE_I64):
        case MTYPE2(TYPE_TIMESTAMP, -TYPE_I64):
        case MTYPE2(TYPE_F64, -TYPE_I64):
        case MTYPE2(TYPE_C8, -TYPE_I64):
        case MTYPE2(TYPE_LIST, -TYPE_I64):
        case MTYPE2(TYPE_ENUM, -TYPE_I64):
        case MTYPE2(TYPE_MAPLIST, -TYPE_I64):
        case MTYPE2(TYPE_TABLE, -TYPE_I64):
            return at_idx(obj, idx->i64);
        case MTYPE2(TYPE_TABLE, -TYPE_SYMBOL):
            j = find_raw(AS_LIST(obj)[0], &idx->i64);
            if (j == NULL_I64)
                return null(AS_LIST(obj)[1]->type);
            return at_idx(AS_LIST(obj)[1], j);
        case MTYPE2(TYPE_I64, TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, TYPE_I64):
        case MTYPE2(TYPE_TIMESTAMP, TYPE_I64):
        case MTYPE2(TYPE_F64, TYPE_I64):
        case MTYPE2(TYPE_GUID, TYPE_I64):
        case MTYPE2(TYPE_LIST, TYPE_I64):
        case MTYPE2(TYPE_ENUM, TYPE_I64):
        case MTYPE2(TYPE_TABLE, TYPE_I64):
            ids = AS_I64(idx);
            n = idx->len;
            l = ops_count(obj);
            for (i = 0; i < n; i++)
                if (ids[i] < 0 || ids[i] >= (i64_t)l)
                    THROW(ERR_TYPE, "at_obj: '%lld' is out of range '0..%lld'", ids[i], l - 1);
            return at_ids(obj, AS_I64(idx), idx->len);
        case MTYPE2(TYPE_TABLE, TYPE_SYMBOL):
            l = ops_count(idx);
            v = LIST(l);

            for (i = 0; i < l; i++) {
                j = find_raw(AS_LIST(obj)[0], &AS_SYMBOL(idx)[i]);
                if (j == NULL_I64)
                    AS_LIST(v)[i] = null(0);
                else
                    AS_LIST(v)[i] = at_idx(AS_LIST(obj)[1], j);
            }
            return v;
        default:
            if (obj->type == TYPE_DICT) {
                j = find_obj_idx(AS_LIST(obj)[0], idx);
                return (j == NULL_I64) ? null(AS_LIST(obj)[1]->type) : at_idx(AS_LIST(obj)[1], j);
            }

            THROW(ERR_TYPE, "at_obj: unable to index: '%s by '%s", type_name(obj->type), type_name(idx->type));
    }
}

obj_p at_sym(obj_p obj, lit_p str, i64_t n) {
    obj_p sym, res;

    sym = symbol(str, n);
    res = at_obj(obj, sym);
    drop_obj(sym);

    return res;
}

obj_p set_idx(obj_p *obj, i64_t idx, obj_p val) {
    if (idx < 0 || idx >= (i64_t)(*obj)->len) {
        drop_obj(val);
        THROW(ERR_INDEX, "set_idx: '%lld' is out of range '0..%lld'", idx, (*obj)->len - 1);
    }

    switch (MTYPE2((*obj)->type, val->type)) {
        case MTYPE2(TYPE_I64, -TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, -TYPE_SYMBOL):
        case MTYPE2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
            AS_I64(*obj)[idx] = val->i64;
            drop_obj(val);
            return *obj;
        case MTYPE2(TYPE_F64, -TYPE_F64):
            AS_F64(*obj)[idx] = val->f64;
            drop_obj(val);
            return *obj;
        case MTYPE2(TYPE_C8, -TYPE_C8):
            AS_C8(*obj)[idx] = val->c8;
            drop_obj(val);
            return *obj;
        case MTYPE2(TYPE_GUID, -TYPE_GUID):
            memcpy(AS_GUID(*obj)[idx], AS_GUID(val), sizeof(guid_t));
            drop_obj(val);
            return *obj;
        default:
            if ((*obj)->type == TYPE_LIST) {
                drop_obj(AS_LIST(*obj)[idx]);
                AS_LIST(*obj)[idx] = val;
                return *obj;
            }

            // Diverse to a list then
            diverse_obj(obj);
            return set_idx(obj, idx, val);
    }
}

obj_p set_ids(obj_p *obj, i64_t ids[], i64_t len, obj_p vals) {
    i64_t i;

    switch (MTYPE2((*obj)->type, vals->type)) {
        case MTYPE2(TYPE_I64, -TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, -TYPE_I64):
        case MTYPE2(TYPE_TIMESTAMP, -TYPE_I64):
            for (i = 0; i < len; i++)
                AS_I64(*obj)[ids[i]] = vals->i64;
            drop_obj(vals);
            return *obj;
        case MTYPE2(TYPE_F64, -TYPE_F64):
            for (i = 0; i < len; i++)
                AS_F64(*obj)[ids[i]] = vals->f64;
            drop_obj(vals);
            return *obj;
        case MTYPE2(TYPE_C8, -TYPE_C8):
            for (i = 0; i < len; i++)
                AS_C8(*obj)[ids[i]] = vals->c8;
            drop_obj(vals);
            return *obj;
        case MTYPE2(TYPE_GUID, -TYPE_GUID):
            for (i = 0; i < len; i++)
                memcpy(AS_GUID(*obj)[ids[i]], AS_GUID(vals), sizeof(guid_t));
            drop_obj(vals);
            return *obj;
        case MTYPE2(TYPE_I64, TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, TYPE_I64):
        case MTYPE2(TYPE_TIMESTAMP, TYPE_I64):
            for (i = 0; i < len; i++)
                AS_I64(*obj)[ids[i]] = AS_I64(vals)[i];
            drop_obj(vals);
            return *obj;
        case MTYPE2(TYPE_F64, TYPE_F64):
            for (i = 0; i < len; i++)
                AS_F64(*obj)[ids[i]] = AS_F64(vals)[i];
            drop_obj(vals);
            return *obj;
        case MTYPE2(TYPE_C8, TYPE_C8):
            for (i = 0; i < len; i++)
                AS_C8(*obj)[ids[i]] = AS_C8(vals)[i];
            drop_obj(vals);
            return *obj;
        case MTYPE2(TYPE_GUID, TYPE_GUID):
            for (i = 0; i < len; i++)
                memcpy(AS_GUID(*obj)[ids[i]], AS_GUID(vals)[i], sizeof(guid_t));
            drop_obj(vals);
            return *obj;
        case MTYPE2(TYPE_LIST, TYPE_C8):
            for (i = 0; i < len; i++) {
                drop_obj(AS_LIST(*obj)[ids[i]]);
                AS_LIST(*obj)[ids[i]] = clone_obj(vals);
            }
            drop_obj(vals);
            return *obj;
        default:
            if ((*obj)->type == TYPE_LIST) {
                if (IS_VECTOR(vals) && len != (*obj)->len) {
                    for (i = 0; i < len; i++) {
                        drop_obj(AS_LIST(*obj)[ids[i]]);
                        AS_LIST(*obj)[ids[i]] = clone_obj(vals);
                    }
                } else {
                    for (i = 0; i < len; i++) {
                        drop_obj(AS_LIST(*obj)[ids[i]]);
                        AS_LIST(*obj)[ids[i]] = at_idx(vals, i);
                    }
                }

                drop_obj(vals);

                return *obj;
            }

            THROW(ERR_TYPE, "set_ids: types mismatch/unsupported: '%s, '%s", type_name((*obj)->type),
                  type_name(vals->type));
    }
}

obj_p __expand(obj_p obj, i64_t len) {
    i64_t i;
    obj_p res;

    switch (obj->type) {
        case -TYPE_B8:
        case -TYPE_U8:
        case -TYPE_C8:
            res = vector(obj->type, len);
            for (i = 0; i < len; i++)
                AS_U8(res)[i] = obj->u8;

            drop_obj(obj);

            return res;
        case -TYPE_I64:
        case -TYPE_SYMBOL:
        case -TYPE_TIMESTAMP:
            res = vector(obj->type, len);
            for (i = 0; i < len; i++)
                AS_I64(res)[i] = obj->i64;

            drop_obj(obj);

            return res;
        case -TYPE_F64:
            res = F64(len);
            for (i = 0; i < len; i++)
                AS_F64(res)[i] = obj->f64;

            drop_obj(obj);

            return res;
        case -TYPE_GUID:
            res = vector(TYPE_GUID, len);
            for (i = 0; i < len; i++)
                memcpy(AS_GUID(res)[i], *AS_GUID(obj), sizeof(guid_t));

            drop_obj(obj);

            return res;
        default:
            if (ops_count(obj) != len) {
                drop_obj(obj);
                THROW(ERR_LENGTH, "set: invalid length: '%lld' != '%lld'", ops_count(obj), len);
            }

            return obj;
    }
}

i64_t find_sym(obj_p obj, lit_p str) {
    i64_t n;

    n = symbols_intern(str, strlen(str));
    return find_raw(obj, &n);
}

i64_t find_obj_idx(obj_p obj, obj_p val) {
    i16_t vi16;
    i32_t vi32;
    i64_t vi64;
    i64_t i;
    obj_p eq;

    switch (MTYPE2(obj->type, val->type)) {
        case MTYPE2(TYPE_B8, -TYPE_B8):
        case MTYPE2(TYPE_U8, -TYPE_U8):
        case MTYPE2(TYPE_C8, -TYPE_C8):
            return find_raw(obj, &val->u8);
        case MTYPE2(TYPE_I16, -TYPE_I16):
            return find_raw(obj, &val->i16);
        case MTYPE2(TYPE_I16, -TYPE_I32):
            vi32 = val->i32;
            vi16 = (i16_t)vi32;
            return find_raw(obj, &vi16);
        case MTYPE2(TYPE_I16, -TYPE_I64):
            vi64 = val->i64;
            vi16 = (i16_t)vi64;
            return find_raw(obj, &vi16);
        case MTYPE2(TYPE_I32, -TYPE_I16):
            vi16 = val->i16;
            vi32 = (i32_t)vi16;
            return find_raw(obj, &vi32);
        case MTYPE2(TYPE_I32, -TYPE_I32):
        case MTYPE2(TYPE_DATE, -TYPE_DATE):
        case MTYPE2(TYPE_TIME, -TYPE_TIME):
            return find_raw(obj, &val->i32);
        case MTYPE2(TYPE_I32, -TYPE_I64):
            vi64 = val->i64;
            vi32 = (i32_t)vi64;
            return find_raw(obj, &vi32);
        case MTYPE2(TYPE_I64, -TYPE_I16):
            vi16 = val->i16;
            vi64 = (i64_t)vi16;
            return find_raw(obj, &vi64);
        case MTYPE2(TYPE_I64, -TYPE_I32):
            vi32 = val->i32;
            vi64 = (i64_t)vi32;
            return find_raw(obj, &vi64);
        case MTYPE2(TYPE_I64, -TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, -TYPE_SYMBOL):
        case MTYPE2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
            return find_raw(obj, &val->i64);
        case MTYPE2(TYPE_F64, -TYPE_F64):
            return find_raw(obj, &val->f64);
        case MTYPE2(TYPE_GUID, -TYPE_GUID):
            return find_raw(obj, AS_GUID(val));
        default:
            if (!IS_VECTOR(obj) && !IS_VECTOR(val))
                return (cmp_obj(obj, val) == 0) ? 0 : NULL_I64;

            if (obj->type == TYPE_LIST) {
                for (i = 0; i < obj->len; i++) {
                    eq = ray_eq(AS_LIST(obj)[i], val);
                    if (eq->type == -TYPE_B8 && eq->b8) {
                        drop_obj(eq);
                        return i;
                    }
                    drop_obj(eq);
                }
            }

            return NULL_I64;
    }
}

obj_p find_obj_ids(obj_p obj, obj_p val) {
    i64_t i, j, l;
    obj_p ids;

    switch (MTYPE2(obj->type, val->type)) {
        case MTYPE2(TYPE_I64, TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, TYPE_SYMBOL):
        case MTYPE2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
            l = val->len;
            ids = I64(l);

            for (i = 0; i < l; i++) {
                j = find_raw(obj, &AS_I64(val)[i]);
                if (j == NULL_I64) {
                    drop_obj(ids);
                    return NULL_OBJ;
                }

                AS_I64(ids)[i] = j;
            }

            return ids;
        default:
            THROW(ERR_TYPE, "find_obj_ids: invalid types: '%s, '%s'", type_name(obj->type), type_name(val->type));
    }
}

obj_p set_dict_obj(obj_p *obj, obj_p idx, obj_p val) {
    i64_t i;
    obj_p ids, res;

    switch (idx->type) {
        case -TYPE_I64:
        case -TYPE_SYMBOL:
        case -TYPE_TIMESTAMP:
            i = find_obj_idx(AS_LIST(*obj)[0], idx);
            // Key not found, add it
            if (i == NULL_I64) {
                res = push_obj(&AS_LIST(*obj)[0], clone_obj(idx));
                if (IS_ERR(res))
                    return res;

                res = push_obj(&AS_LIST(*obj)[1], val);
                if (IS_ERR(res))
                    return res;
            }
            // Key found, update it
            else {
                res = set_idx(&AS_LIST(*obj)[1], i, val);
                if (IS_ERR(res))
                    return res;
            }

            return *obj;
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
            ids = find_obj_ids(AS_LIST(*obj)[0], idx);
            if (IS_ERR(ids))
                return ids;

            res = set_ids(&AS_LIST(*obj)[1], AS_I64(ids), ids->len, val);
            drop_obj(ids);

            return res;
        default:
            THROW(ERR_TYPE, "set_dict_obj: invalid types: 'keys: %s, 'idx: %s, 'val: %s",
                  type_name(AS_LIST(*obj)[0]->type), type_name(idx->type), type_name(val->type));
    }

    return *obj;
}

obj_p set_obj(obj_p *obj, obj_p idx, obj_p val) {
    obj_p k, v, res;
    i64_t i, n, l;
    i64_t j, id = NULL_I64, *ids = NULL;

    // dispatch:
    switch (MTYPE2((*obj)->type, idx->type)) {
        case MTYPE2(TYPE_I64, -TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, -TYPE_I64):
        case MTYPE2(TYPE_TIMESTAMP, -TYPE_I64):
        case MTYPE2(TYPE_F64, -TYPE_I64):
        case MTYPE2(TYPE_C8, -TYPE_I64):
        case MTYPE2(TYPE_LIST, -TYPE_I64):
        case MTYPE2(TYPE_GUID, -TYPE_I64):
            if (idx->i64 < 0 || idx->i64 >= (i64_t)(*obj)->len) {
                drop_obj(val);
                THROW(ERR_TYPE, "set_obj: '%lld' is out of range '0..%lld'", idx->i64, (*obj)->len - 1);
            }
            return set_idx(obj, idx->i64, val);
        case MTYPE2(TYPE_I64, TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, TYPE_I64):
        case MTYPE2(TYPE_TIMESTAMP, TYPE_I64):
        case MTYPE2(TYPE_F64, TYPE_I64):
        case MTYPE2(TYPE_C8, TYPE_I64):
        case MTYPE2(TYPE_GUID, TYPE_I64):
        case MTYPE2(TYPE_LIST, TYPE_I64):
            if (IS_VECTOR(val) && idx->len != val->len) {
                drop_obj(val);
                THROW(ERR_LENGTH, "set_obj: idx and vals length mismatch: '%lld' != '%lld'", idx->len, val->len);
            }
            ids = AS_I64(idx);
            n = idx->len;
            l = ops_count(*obj);
            for (i = 0; i < n; i++) {
                if (ids[i] < 0 || ids[i] >= (i64_t)l) {
                    drop_obj(val);
                    THROW(ERR_TYPE, "set_obj: '%lld' is out of range '0..%lld'", ids[i], l - 1);
                }
            }
            return set_ids(obj, ids, n, val);
        case MTYPE2(TYPE_TABLE, -TYPE_SYMBOL):
            val = __expand(val, ops_count(*obj));
            if (IS_ERR(val))
                return val;
            j = find_obj_idx(AS_LIST(*obj)[0], idx);
            if (j == NULL_I64) {
                res = push_obj(&AS_LIST(*obj)[0], clone_obj(idx));
                if (IS_ERR(res))
                    return res;

                res = push_obj(&AS_LIST(*obj)[1], val);
                if (IS_ERR(res))
                    PANIC("set_obj: inconsistent update");

                return *obj;
            }

            set_idx(&AS_LIST(*obj)[1], j, val);

            return *obj;
        case MTYPE2(TYPE_TABLE, TYPE_SYMBOL):
            if (val->type != TYPE_LIST) {
                drop_obj(val);
                THROW(ERR_TYPE,
                      "set_obj: 'Table indexed via vector expects 'List in a "
                      "values, found: '%s",
                      type_name(val->type));
            }

            l = ops_count(idx);
            if (l != ops_count(val)) {
                drop_obj(val);
                THROW(ERR_LENGTH, "set_obj: idx and vals length mismatch: '%lld' != '%lld'", ops_count(*obj),
                      ops_count(val));
            }

            n = ops_count(*obj);
            v = LIST(l);
            for (i = 0; i < l; i++) {
                k = __expand(clone_obj(AS_LIST(val)[i]), n);

                if (IS_ERR(k)) {
                    v->len = i;
                    drop_obj(v);
                    drop_obj(val);
                    return k;
                }

                AS_LIST(v)[i] = k;
            }

            drop_obj(val);
            val = v;

            for (i = 0; i < l; i++) {
                id = find_raw(AS_LIST(*obj)[0], &AS_SYMBOL(idx)[i]);
                if (id == NULL_I64) {
                    push_raw(&AS_LIST(*obj)[0], &AS_SYMBOL(idx)[i]);
                    push_obj(&AS_LIST(*obj)[1], clone_obj(AS_LIST(val)[i]));
                } else {
                    set_idx(&AS_LIST(*obj)[1], id, clone_obj(AS_LIST(val)[i]));
                }
            }

            drop_obj(val);

            return *obj;
        default:
            if ((*obj)->type == TYPE_DICT) {
                res = set_dict_obj(obj, idx, val);
                if (IS_ERR(res)) {
                    drop_obj(val);
                    return res;
                }

                return res;
            }

            // Diverse to a list then
            diverse_obj(obj);
            return set_obj(obj, idx, val);
    }
}

obj_p pop_obj(obj_p *obj) {
    if ((*obj)->len == 0)
        return NULL_OBJ;

    switch ((*obj)->type) {
        case TYPE_I64:
            return i64(AS_I64(*obj)[--(*obj)->len]);
        case TYPE_SYMBOL:
            return symboli64(AS_SYMBOL(*obj)[--(*obj)->len]);
        case TYPE_TIMESTAMP:
            return timestamp(AS_TIMESTAMP(*obj)[--(*obj)->len]);
        case TYPE_F64:
            return f64(AS_F64(*obj)[--(*obj)->len]);
        case TYPE_C8:
            return c8(AS_C8(*obj)[--(*obj)->len]);
        case TYPE_LIST:
            return AS_LIST(*obj)[--(*obj)->len];
        default:
            PANIC("pop_obj: invalid type: %d", (*obj)->type);
    }
}

obj_p remove_idx(obj_p *obj, i64_t idx) {
    if (idx < 0 || idx >= (i64_t)(*obj)->len)
        return *obj;

    switch ((*obj)->type) {
        case TYPE_U8:
        case TYPE_B8:
        case TYPE_C8:
            memmove(AS_U8(*obj) + idx, AS_U8(*obj) + idx + 1, ((*obj)->len - idx - 1) * sizeof(u8_t));
            return resize_obj(obj, (*obj)->len - 1);
        case TYPE_I16:
            memmove(AS_I16(*obj) + idx, AS_I16(*obj) + idx + 1, ((*obj)->len - idx - 1) * sizeof(i16_t));
            return resize_obj(obj, (*obj)->len - 1);
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            memmove(AS_I32(*obj) + idx, AS_I32(*obj) + idx + 1, ((*obj)->len - idx - 1) * sizeof(i32_t));
            return resize_obj(obj, (*obj)->len - 1);
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
        case TYPE_F64:
            memmove(AS_I64(*obj) + idx, AS_I64(*obj) + idx + 1, ((*obj)->len - idx - 1) * sizeof(i64_t));
            return resize_obj(obj, (*obj)->len - 1);
        case TYPE_GUID:
            memmove(AS_GUID(*obj) + idx, AS_GUID(*obj) + idx + 1, ((*obj)->len - idx - 1) * sizeof(guid_t));
            return resize_obj(obj, (*obj)->len - 1);
        case TYPE_LIST:
            drop_obj(AS_LIST(*obj)[idx]);
            memmove(AS_LIST(*obj) + idx, AS_LIST(*obj) + idx + 1, ((*obj)->len - idx - 1) * sizeof(obj_p));
            return resize_obj(obj, (*obj)->len - 1);
        default:
            THROW(ERR_TYPE, "remove_idx: invalid type: %d", (*obj)->type);
    }
}

obj_p remove_ids(obj_p *obj, i64_t ids[], i64_t len) {
    i64_t i, j;

    if (len == 0 || (*obj)->len == 0 || len > (*obj)->len)
        return *obj;

    // Check ids are in range
    for (i = 0; i < len; i++)
        if (ids[i] < 0 || ids[i] >= (i64_t)(*obj)->len)
            return *obj;

    switch ((*obj)->type) {
        case TYPE_U8:
        case TYPE_B8:
        case TYPE_C8:
            for (i = 0; i < len; i++) {
                j = ids[i] - i;
                memmove(AS_U8(*obj) + j, AS_U8(*obj) + j + 1, ((*obj)->len - j - 1) * sizeof(u8_t));
            }

            return resize_obj(obj, (*obj)->len - len);
        case TYPE_I16:
            for (i = 0; i < len; i++) {
                j = ids[i] - i;
                memmove(AS_I16(*obj) + j, AS_I16(*obj) + j + 1, ((*obj)->len - j - 1) * sizeof(i16_t));
            }

            return resize_obj(obj, (*obj)->len - len);

        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            for (i = 0; i < len; i++) {
                j = ids[i] - i;
                memmove(AS_I32(*obj) + j, AS_I32(*obj) + j + 1, ((*obj)->len - j - 1) * sizeof(i32_t));
            }

            return resize_obj(obj, (*obj)->len - len);

        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
        case TYPE_F64:
            for (i = 0; i < len; i++) {
                j = ids[i] - i;
                memmove(AS_I64(*obj) + j, AS_I64(*obj) + j + 1, ((*obj)->len - j - 1) * sizeof(i64_t));
            }

            return resize_obj(obj, (*obj)->len - len);

        case TYPE_GUID:
            for (i = 0; i < len; i++) {
                j = ids[i] - i;
                memmove(AS_GUID(*obj) + j, AS_GUID(*obj) + j + 1, ((*obj)->len - j - 1) * sizeof(guid_t));
            }

            return resize_obj(obj, (*obj)->len - len);

        case TYPE_LIST:
            for (i = 0; i < len; i++) {
                j = ids[i] - i;
                drop_obj(AS_LIST(*obj)[j]);
                memmove(AS_I64(*obj) + j, AS_I64(*obj) + j + 1, ((*obj)->len - j - 1) * sizeof(i64_t));
            }

            return resize_obj(obj, (*obj)->len - len);

        default:
            PANIC("remove_ids: invalid type: %d", (*obj)->type);
    }
}

obj_p remove_obj(obj_p *obj, obj_p idx) {
    i64_t i, j;
    obj_p v;

    switch (MTYPE2((*obj)->type, idx->type)) {
        case MTYPE2(TYPE_U8, -TYPE_I64):
        case MTYPE2(TYPE_B8, -TYPE_I64):
        case MTYPE2(TYPE_C8, -TYPE_I64):
        case MTYPE2(TYPE_I16, -TYPE_I64):
        case MTYPE2(TYPE_I32, -TYPE_I64):
        case MTYPE2(TYPE_DATE, -TYPE_I64):
        case MTYPE2(TYPE_TIME, -TYPE_I64):
        case MTYPE2(TYPE_I64, -TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, -TYPE_I64):
        case MTYPE2(TYPE_TIMESTAMP, -TYPE_I64):
        case MTYPE2(TYPE_F64, -TYPE_I64):
        case MTYPE2(TYPE_LIST, -TYPE_I64):
            return remove_idx(obj, idx->i64);
        case MTYPE2(TYPE_U8, TYPE_I64):
        case MTYPE2(TYPE_B8, TYPE_I64):
        case MTYPE2(TYPE_C8, TYPE_I64):
        case MTYPE2(TYPE_I16, TYPE_I64):
        case MTYPE2(TYPE_I32, TYPE_I64):
        case MTYPE2(TYPE_DATE, TYPE_I64):
        case MTYPE2(TYPE_TIME, TYPE_I64):
        case MTYPE2(TYPE_I64, TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, TYPE_I64):
        case MTYPE2(TYPE_TIMESTAMP, TYPE_I64):
        case MTYPE2(TYPE_F64, TYPE_I64):
        case MTYPE2(TYPE_LIST, TYPE_I64):
            return remove_ids(obj, AS_I64(idx), idx->len);
        case MTYPE2(TYPE_DICT, -TYPE_I64):
            i = find_obj_idx(AS_LIST(*obj)[0], idx);

            // not found ?
            if (i == NULL_I64)
                return *obj;

            v = remove_idx(&AS_LIST(*obj)[0], i);

            if (IS_ERR(v))
                return v;

            v = remove_idx(&AS_LIST(*obj)[1], i);
            if (IS_ERR(v))
                return v;

            return *obj;
        default:
            if ((*obj)->type == TYPE_DICT) {
                j = find_obj_idx(AS_LIST(*obj)[0], idx);
                if (j == NULL_I64)
                    return *obj;

                v = remove_idx(&AS_LIST(*obj)[0], j);
                if (IS_ERR(v))
                    return v;

                v = remove_idx(&AS_LIST(*obj)[1], j);
                if (IS_ERR(v))
                    return v;

                return *obj;
            }

            THROW(ERR_TYPE, "remove_obj: invalid types: '%s' '%s'", type_name((*obj)->type), type_name(idx->type));
    }
}

b8_t is_null(obj_p obj) {
    return (obj->type == TYPE_NULL) || (obj->type == -TYPE_I64 && obj->i64 == NULL_I64) ||
           (obj->type == -TYPE_SYMBOL && obj->i64 == NULL_I64) || (obj->type == -TYPE_F64 && obj->f64 == NULL_F64) ||
           (obj->type == -TYPE_TIMESTAMP && obj->i64 == NULL_I64) || (obj->type == -TYPE_C8 && obj->c8 == '\0');
}

i64_t cmp_obj(obj_p a, obj_p b) {
    i64_t i, l, d;

    if (a == b)
        return 0;

    if (a->type != b->type)
        // if ((a->type == -TYPE_I16 || a->type == -TYPE_I32 || a->type == -TYPE_I64) &&
        //     (b->type == -TYPE_I16 || b->type == -TYPE_I32 || b->type != -TYPE_I64)) {
        //     switch (a->type) {
        //         case -TYPE_I16:
        //             switch (b->type) {
        //                 case -TYPE_I32:
        //                     return (i64_t)a->i16 - (i64_t)b->i32;
        //                 case -TYPE_I64:
        //                     return (i64_t)a->i16 < b->i64 ? -1 : (i64_t)a->i16 > b->i64 ? 1 : 0;
        //             }
        //         case -TYPE_I32:
        //             switch (b->type) {
        //                 case -TYPE_I16:
        //                     return (i64_t)a->i32 - (i64_t)b->i16;
        //                 case -TYPE_I64:
        //                     return (i64_t)a->i32 < b->i64 ? -1 : (i64_t)a->i32 > b->i64 ? 1 : 0;
        //             }
        //         case -TYPE_I64:
        //             switch (b->type) {
        //                 case -TYPE_I16:
        //                     return a->i64<(i64_t)b->i16 ? -1 : a->i64>(i64_t) b->i16 ? 1 : 0;
        //                 case -TYPE_I32:
        //                     return a->i64<(i64_t)b->i32 ? -1 : a->i64>(i64_t) b->i32 ? 1 : 0;
        //             }
        //     }
        // } else {
        return a->type - b->type;
    // }

    switch (a->type) {
        case -TYPE_B8:
            return (i64_t)a->b8 - (i64_t)b->b8;
        case -TYPE_U8:
            return (i64_t)a->u8 - (i64_t)b->u8;
        case -TYPE_I16:
            return (i64_t)a->i16 - (i64_t)b->i16;
        case -TYPE_C8:
            return (i64_t)a->u8 - (i64_t)b->u8;
        case -TYPE_I32:
        case -TYPE_DATE:
        case -TYPE_TIME:
            return (i64_t)a->i32 - (i64_t)b->i32;
        case -TYPE_I64:
        case -TYPE_TIMESTAMP:
            return a->i64 > b->i64 ? 1 : a->i64 < b->i64 ? -1 : 0;
        case TYPE_UNARY:
        case TYPE_BINARY:
        case TYPE_VARY:
            return a->i64 - b->i64;
        case -TYPE_SYMBOL: {
            str_p str_a = str_from_symbol(a->i64);
            str_p str_b = str_from_symbol(b->i64);
            return strcmp(str_a, str_b);
        }
        case -TYPE_F64:
            return (ISNANF64(a->f64) && ISNANF64(b->f64))   ? 0
                   : (ISNANF64(a->f64) || ISNANF64(b->f64)) ? 1
                   : (ABSF64(a->f64 - b->f64) < 1e-16)      ? 0
                                                            : 1;
        case -TYPE_GUID:
            return memcmp(AS_GUID(a), AS_GUID(b), sizeof(guid_t));
        case TYPE_B8:
        case TYPE_C8:
        case TYPE_U8: {
            l = a->len < b->len ? a->len : b->len;
            d = memcmp(AS_C8(a), AS_C8(b), l);
            if (d != 0)
                return d;
            return a->len - b->len;
        }
        case TYPE_I16: {
            l = a->len < b->len ? a->len : b->len;
            for (i = 0; i < l; i++) {
                d = (i64_t)(AS_I16(a)[i] - AS_I16(b)[i]);
                if (d != 0)
                    return d;
            }
            return a->len - b->len;
        }
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME: {
            l = a->len < b->len ? a->len : b->len;
            for (i = 0; i < l; i++) {
                d = (i64_t)(AS_I32(a)[i] - AS_I32(b)[i]);
                if (d != 0)
                    return d;
            }
            return a->len - b->len;
        }
        case TYPE_I64:
        case TYPE_TIMESTAMP: {
            l = a->len < b->len ? a->len : b->len;
            for (i = 0; i < l; i++) {
                d = AS_I64(a)[i] > AS_I64(b)[i] ? 1 : AS_I64(a)[i] < AS_I64(b)[i] ? -1 : 0;
                if (d != 0)
                    return d;
            }
            return a->len - b->len;
        }
        case TYPE_SYMBOL: {
            l = a->len < b->len ? a->len : b->len;
            for (i = 0; i < l; i++) {
                str_p str_a = str_from_symbol(AS_I64(a)[i]);
                str_p str_b = str_from_symbol(AS_I64(b)[i]);
                d = strcmp(str_a, str_b);
                if (d != 0)
                    return d;
            }
            return a->len - b->len;
        }
        case TYPE_F64: {
            l = a->len < b->len ? a->len : b->len;
            for (i = 0; i < l; i++) {
                d = (ISNANF64(AS_F64(a)[i]) && ISNANF64(AS_F64(b)[i]))   ? 0
                    : (ISNANF64(AS_F64(a)[i]) || ISNANF64(AS_F64(b)[i])) ? 1
                    : (ABSF64(AS_F64(a)[i] - AS_F64(b)[i]) < 1e-16)      ? 0
                                                                         : 1;
                if (d != 0)
                    return d;
            }
            return a->len - b->len;
        }
        case TYPE_LIST: {
            l = a->len < b->len ? a->len : b->len;
            for (i = 0; i < l; i++) {
                obj_p elem_a = AS_LIST(a)[i];
                obj_p elem_b = AS_LIST(b)[i];
                d = cmp_obj(elem_a, elem_b);
                if (d != 0)
                    return d;
            }
            return a->len - b->len;
        }

        default:
            return -1;
    }
}

i64_t find_raw(obj_p obj, raw_p val) {
    i64_t i, l;

    if (!IS_VECTOR(obj))
        return NULL_I64;

    l = obj->len;

    switch (obj->type) {
        case TYPE_U8:
        case TYPE_B8:
        case TYPE_C8:
            for (i = 0; i < l; i++)
                if (AS_U8(obj)[i] == *(u8_t *)val)
                    return i;
            return NULL_I64;
        case TYPE_I16:
            for (i = 0; i < l; i++)
                if (AS_I16(obj)[i] == *(i16_t *)val)
                    return i;
            return NULL_I64;
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            for (i = 0; i < l; i++)
                if (AS_I32(obj)[i] == *(i32_t *)val)
                    return i;
            return NULL_I64;
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
            for (i = 0; i < l; i++)
                if (AS_I64(obj)[i] == *(i64_t *)val)
                    return i;
            return NULL_I64;
        case TYPE_F64:
            for (i = 0; i < l; i++)
                if (AS_F64(obj)[i] == *(f64_t *)val)
                    return i;
            return NULL_I64;
        case TYPE_GUID:
            for (i = 0; i < l; i++)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
                if (memcmp(AS_GUID(obj)[i], *(guid_t *)val, sizeof(guid_t)) == 0)
#pragma GCC diagnostic pop
                    return i;
            return NULL_I64;
        case TYPE_LIST:
            for (i = 0; i < l; i++)
                if (cmp_obj(AS_LIST(obj)[i], *(obj_p *)val) == 0)
                    return i;
            return NULL_I64;
        default:
            return NULL_I64;
    }
}

obj_p cast_obj(i8_t type, obj_p obj) {
    obj_p v, res, err, msg;
    u8_t *g;
    i32_t num_i32;
    i64_t i, l, num_i64;
    f64_t num_f64;
    lit_p str;

    // Do nothing if the type is the same
    if (type == obj->type)
        return clone_obj(obj);

    switch (MTYPE2(type, obj->type)) {
        case MTYPE2(-TYPE_B8, -TYPE_I16):
            return b8(obj->i16 != 0);
        case MTYPE2(-TYPE_B8, -TYPE_I32):
            return b8(obj->i32 != 0);
        case MTYPE2(-TYPE_B8, -TYPE_I64):
            return b8(obj->i64 != 0);
        case MTYPE2(-TYPE_U8, -TYPE_I16):
            return u8(obj->i16);
        case MTYPE2(-TYPE_U8, -TYPE_I32):
            return u8(obj->i32);
        case MTYPE2(-TYPE_U8, -TYPE_I64):
            return u8(obj->i64);
        case MTYPE2(-TYPE_I16, -TYPE_I32):
            return i16(obj->i32);
        case MTYPE2(-TYPE_I16, -TYPE_I64):
            return i16(obj->i64);
        case MTYPE2(-TYPE_I32, -TYPE_U8):
            return i32(obj->u8);
        case MTYPE2(-TYPE_I32, -TYPE_I16):
            return i32(obj->i16);
        case MTYPE2(-TYPE_I32, -TYPE_I64):
            return i32(obj->i64);
        case MTYPE2(-TYPE_I32, -TYPE_TIME):
        case MTYPE2(-TYPE_I32, -TYPE_DATE):
            return i32(obj->i32);
        case MTYPE2(-TYPE_I64, -TYPE_U8):
        case MTYPE2(-TYPE_I64, -TYPE_I16):
        case MTYPE2(-TYPE_I64, -TYPE_I32):
        case MTYPE2(-TYPE_I64, -TYPE_TIME):
        case MTYPE2(-TYPE_I64, -TYPE_DATE):
        case MTYPE2(-TYPE_I64, -TYPE_TIMESTAMP):
            return i64(obj->i64);
        case MTYPE2(-TYPE_I64, -TYPE_F64):
            return i64((i64_t)obj->f64);
        case MTYPE2(-TYPE_F64, -TYPE_U8):
        case MTYPE2(-TYPE_F64, -TYPE_I16):
        case MTYPE2(-TYPE_F64, -TYPE_I32):
        case MTYPE2(-TYPE_F64, -TYPE_DATE):
        case MTYPE2(-TYPE_F64, -TYPE_TIME):
        case MTYPE2(-TYPE_F64, -TYPE_I64):
        case MTYPE2(-TYPE_F64, -TYPE_TIMESTAMP):
            return f64((f64_t)obj->i64);
        case MTYPE2(-TYPE_DATE, -TYPE_U8):
        case MTYPE2(-TYPE_DATE, -TYPE_I16):
        case MTYPE2(-TYPE_DATE, -TYPE_I32):
        case MTYPE2(-TYPE_DATE, -TYPE_I64):
            return adate(obj->i32);
        case MTYPE2(-TYPE_TIME, -TYPE_U8):
        case MTYPE2(-TYPE_TIME, -TYPE_I16):
        case MTYPE2(-TYPE_TIME, -TYPE_I32):
        case MTYPE2(-TYPE_TIME, -TYPE_I64):
            return atime(obj->i32);
        case MTYPE2(-TYPE_TIMESTAMP, -TYPE_U8):
        case MTYPE2(-TYPE_TIMESTAMP, -TYPE_I16):
        case MTYPE2(-TYPE_TIMESTAMP, -TYPE_I32):
        case MTYPE2(-TYPE_TIMESTAMP, -TYPE_I64):
            return timestamp(obj->i64);
        case MTYPE2(-TYPE_SYMBOL, -TYPE_U8):
        case MTYPE2(-TYPE_SYMBOL, -TYPE_I16):
        case MTYPE2(-TYPE_SYMBOL, -TYPE_I32):
        case MTYPE2(-TYPE_SYMBOL, -TYPE_I64):
            v = str_fmt(-1, "%lld", obj->i64);
            res = symbol(AS_C8(v), strlen(AS_C8(v)));
            drop_obj(v);
            return res;
        case MTYPE2(-TYPE_SYMBOL, -TYPE_F64):
            v = str_fmt(-1, "%f", obj->f64);
            res = symbol(AS_C8(v), strlen(AS_C8(v)));
            drop_obj(v);
            return res;
        case MTYPE2(-TYPE_SYMBOL, TYPE_C8):
            return symbol(AS_C8(obj), obj->len);
        case MTYPE2(-TYPE_B8, TYPE_C8):
            return b8(obj->len > 0);
        case MTYPE2(-TYPE_I32, TYPE_C8):
            i32_from_str(AS_C8(obj), obj->len, &num_i32);
            return i32(num_i32);
        case MTYPE2(-TYPE_I64, TYPE_C8):
            i64_from_str(AS_C8(obj), obj->len, &num_i64);
            return i64(num_i64);
        case MTYPE2(-TYPE_DATE, TYPE_C8):
            return adate(date_into_i32(date_from_str(AS_C8(obj), obj->len)));
        case MTYPE2(-TYPE_TIME, TYPE_C8):
            return atime(time_into_i32(time_from_str(AS_C8(obj), obj->len)));
        case MTYPE2(-TYPE_TIMESTAMP, TYPE_C8):
            return timestamp(timestamp_into_i64(timestamp_from_str(AS_C8(obj), obj->len)));
        case MTYPE2(-TYPE_F64, TYPE_C8):
            f64_from_str(AS_C8(obj), obj->len, &num_f64);
            return f64(num_f64);
        case MTYPE2(TYPE_TABLE, TYPE_DICT):
            return table(clone_obj(AS_LIST(obj)[0]), clone_obj(AS_LIST(obj)[1]));
        case MTYPE2(TYPE_DICT, TYPE_TABLE):
            return dict(clone_obj(AS_LIST(obj)[0]), clone_obj(AS_LIST(obj)[1]));
        case MTYPE2(TYPE_B8, TYPE_I16):
        case MTYPE2(TYPE_U8, TYPE_I16):
            l = obj->len;
            res = U8(l);
            res->type = type;
            for (i = 0; i < l; i++)
                AS_U8(res)[i] = (u8_t)AS_I16(obj)[i];
            return res;
        case MTYPE2(TYPE_B8, TYPE_I32):
        case MTYPE2(TYPE_U8, TYPE_I32):
        case MTYPE2(TYPE_U8, TYPE_DATE):
        case MTYPE2(TYPE_U8, TYPE_TIME):
            l = obj->len;
            res = U8(l);
            res->type = type;
            for (i = 0; i < l; i++)
                AS_U8(res)[i] = (u8_t)AS_I32(obj)[i];
            return res;
        case MTYPE2(TYPE_B8, TYPE_I64):
        case MTYPE2(TYPE_U8, TYPE_TIMESTAMP):
            l = obj->len;
            res = U8(l);
            res->type = type;
            for (i = 0; i < l; i++)
                AS_U8(res)[i] = (u8_t)AS_I64(obj)[i];
            return res;
        case MTYPE2(TYPE_B8, TYPE_F64):
        case MTYPE2(TYPE_U8, TYPE_F64):
            l = obj->len;
            res = U8(l);
            res->type = type;
            for (i = 0; i < l; i++)
                AS_U8(res)[i] = (u8_t)AS_F64(obj)[i];
            return res;
        case MTYPE2(TYPE_I16, TYPE_U8):
        case MTYPE2(TYPE_I16, TYPE_B8):
            l = obj->len;
            res = I16(l);
            for (i = 0; i < l; i++)
                AS_I16(res)[i] = (i16_t)AS_U8(obj)[i];
            return res;
        case MTYPE2(TYPE_I16, TYPE_I32):
            l = obj->len;
            res = I16(l);
            for (i = 0; i < l; i++)
                AS_I16(res)[i] = (i16_t)AS_I32(obj)[i];
            return res;
        case MTYPE2(TYPE_I16, TYPE_I64):
            l = obj->len;
            res = I16(l);
            for (i = 0; i < l; i++)
                AS_I16(res)[i] = (i16_t)AS_I64(obj)[i];
            return res;
        case MTYPE2(TYPE_I32, TYPE_U8):
        case MTYPE2(TYPE_I32, TYPE_B8):
            l = obj->len;
            res = I32(l);
            for (i = 0; i < l; i++)
                AS_I32(res)[i] = (i32_t)AS_U8(obj)[i];
            return res;
        case MTYPE2(TYPE_I32, TYPE_I16):
            l = obj->len;
            res = I32(l);
            for (i = 0; i < l; i++)
                AS_I32(res)[i] = (i32_t)AS_I16(obj)[i];
            return res;
        case MTYPE2(TYPE_I32, TYPE_I64):
            l = obj->len;
            res = I32(l);
            for (i = 0; i < l; i++)
                AS_I32(res)[i] = (i32_t)AS_I64(obj)[i];
            return res;
        case MTYPE2(TYPE_I64, TYPE_U8):
            l = obj->len;
            res = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(res)[i] = (i64_t)AS_U8(obj)[i];
            return res;
        case MTYPE2(TYPE_I64, TYPE_I16):
            l = obj->len;
            res = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(res)[i] = (i64_t)AS_I16(obj)[i];
            return res;
        case MTYPE2(TYPE_I64, TYPE_I32):
        case MTYPE2(TYPE_I64, TYPE_DATE):
        case MTYPE2(TYPE_I64, TYPE_TIME):
            l = obj->len;
            res = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(res)[i] = (i64_t)AS_I32(obj)[i];
            return res;
        case MTYPE2(TYPE_I64, TYPE_F64):
            l = obj->len;
            res = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(res)[i] = (i64_t)AS_F64(obj)[i];
            return res;
        case MTYPE2(TYPE_I64, TYPE_TIMESTAMP):
            l = obj->len;
            res = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(res)[i] = AS_I64(obj)[i];
            return res;
        case MTYPE2(TYPE_TIMESTAMP, TYPE_I32):
            l = obj->len;
            res = TIMESTAMP(l);
            for (i = 0; i < l; i++)
                AS_TIMESTAMP(res)[i] = (i64_t)AS_I32(obj)[i];
            return res;
        case MTYPE2(TYPE_TIMESTAMP, TYPE_I64):
            l = obj->len;
            res = TIMESTAMP(l);
            for (i = 0; i < l; i++)
                AS_TIMESTAMP(res)[i] = AS_I64(obj)[i];
            return res;
        case MTYPE2(TYPE_F64, TYPE_U8):
        case MTYPE2(TYPE_F64, TYPE_B8):
            l = obj->len;
            res = F64(l);
            res->type = type;
            for (i = 0; i < l; i++)
                AS_F64(res)[i] = (f64_t)AS_U8(obj)[i];
            return res;
        case MTYPE2(TYPE_F64, TYPE_I16):
            l = obj->len;
            res = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(res)[i] = (f64_t)AS_I16(obj)[i];
            return res;
        case MTYPE2(TYPE_F64, TYPE_I32):
        case MTYPE2(TYPE_F64, TYPE_DATE):
        case MTYPE2(TYPE_F64, TYPE_TIME):
            l = obj->len;
            res = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(res)[i] = (f64_t)AS_I32(obj)[i];
            return res;
        case MTYPE2(TYPE_F64, TYPE_I64):
        case MTYPE2(TYPE_F64, TYPE_TIMESTAMP):
            l = obj->len;
            res = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(res)[i] = (f64_t)AS_I64(obj)[i];
            return res;
        case MTYPE2(-TYPE_GUID, TYPE_C8):
            res = guid(NULL);

            // Validate input length and format
            if (obj->len != 36) {
                memset(AS_GUID(res)[0], 0, sizeof(guid_t));
                return res;
            }

            // Validate format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
            str = AS_C8(obj);
            if (str[8] != '-' || str[13] != '-' || str[18] != '-' || str[23] != '-') {
                memset(AS_GUID(res)[0], 0, sizeof(guid_t));
                return res;
            }

            // Parse hex values safely
            g = AS_GUID(res)[0];
            for (int i = 0; i < 16; i++) {
                int pos = i < 4    ? i * 2
                          : i < 6  ? 9 + (i - 4) * 2
                          : i < 8  ? 14 + (i - 6) * 2
                          : i < 10 ? 19 + (i - 8) * 2
                                   : 24 + (i - 10) * 2;

                c8_t hex[3] = {str[pos], str[pos + 1], '\0'};
                c8_t *end;
                u64_t val = strtoul(hex, &end, 16);
                if (end != hex + 2) {
                    memset(AS_GUID(res)[0], 0, sizeof(guid_t));
                    return res;
                }
                g[i] = (u8_t)val;
            }

            return res;
        default:
            if (type == TYPE_C8)
                return obj_fmt(obj, B8_FALSE);

            if (obj->type == TYPE_LIST) {
                l = obj->len;
                if (l == 0)
                    return vector(type, 0);

                v = cast_obj(-type, AS_LIST(obj)[0]);
                if (IS_ERR(v))
                    return v;

                res = vector(type, l);
                ins_obj(&res, 0, v);

                for (i = 1; i < l; i++) {
                    v = cast_obj(-type, AS_LIST(obj)[i]);
                    if (IS_ERR(v)) {
                        res->len = i;
                        drop_obj(res);
                        return v;
                    }

                    ins_obj(&res, i, v);
                }

                return res;
            }

            msg = str_fmt(-1, "invalid conversion from '%s to '%s", type_name(obj->type), type_name(type));
            err = error_obj(ERR_TYPE, msg);
            return err;
    }

    return res;
}

obj_p __attribute__((hot)) clone_obj(obj_p obj) {
    DEBUG_ASSERT(is_valid(obj), "invalid object type: %d", obj->type);

    if (!__RC_SYNC)
        (obj)->rc += 1;
    else
        __atomic_fetch_add(&obj->rc, 1, __ATOMIC_RELAXED);

    return obj;
}

nil_t __attribute__((hot)) drop_obj(obj_p obj) {
    DEBUG_ASSERT(is_valid(obj), "invalid object type: %d", obj->type);

    u32_t rc;
    i64_t i, l;

    if (!__RC_SYNC) {
        (obj)->rc -= 1;
        rc = (obj)->rc;
    } else
        rc = __atomic_sub_fetch(&obj->rc, 1, __ATOMIC_RELAXED);

    if (rc > 0)
        return;

    switch (obj->type) {
        case TYPE_LIST:
        case TYPE_MAPFILTER:
        case TYPE_MAPGROUP:
        case TYPE_MAPCOMMON:
        case TYPE_PARTEDLIST:
        case TYPE_PARTEDB8:
        case TYPE_PARTEDU8:
        case TYPE_PARTEDI64:
        case TYPE_PARTEDF64:
        case TYPE_PARTEDGUID:
        case TYPE_PARTEDTIMESTAMP:
        case TYPE_PARTEDENUM:
            l = obj->len;
            for (i = 0; i < l; i++)
                drop_obj(AS_LIST(obj)[i]);

            if (IS_EXTERNAL_SIMPLE(obj))
                mmap_free(obj, size_of(obj));
            else
                heap_free(obj);
            return;
        case TYPE_MAPFD:
            fdmap_destroy(obj);
            heap_free(obj);
            return;
        case TYPE_ENUM:
            if (IS_EXTERNAL_COMPOUND(obj)) {
                runtime_fdmap_pop(runtime_get(), obj);
                // mmap_free((str_p)obj - RAY_PAGE_SIZE, size_of(obj) + RAY_PAGE_SIZE);
            } else {
                drop_obj(AS_LIST(obj)[0]);
                drop_obj(AS_LIST(obj)[1]);
                heap_free(obj);
            }
            return;
        case TYPE_MAPLIST:
            runtime_fdmap_pop(runtime_get(), MAPLIST_KEY(obj));
            runtime_fdmap_pop(runtime_get(), obj);
            // mmap_free(MAPLIST_KEY(obj), size_of(obj));
            // mmap_free((str_p)obj - RAY_PAGE_SIZE, size_of(obj) + RAY_PAGE_SIZE);
            return;
        case TYPE_TABLE:
        case TYPE_DICT:
            drop_obj(AS_LIST(obj)[0]);
            drop_obj(AS_LIST(obj)[1]);
            heap_free(obj);
            return;
        case TYPE_LAMBDA:
            drop_obj(AS_LAMBDA(obj)->name);
            drop_obj(AS_LAMBDA(obj)->args);
            drop_obj(AS_LAMBDA(obj)->body);
            drop_obj(AS_LAMBDA(obj)->nfo);
            heap_free(obj);
            return;
        case TYPE_NULL:
            return;
        case TYPE_ERR:
            drop_obj(AS_ERROR(obj)->msg);
            drop_obj(AS_ERROR(obj)->locs);
            heap_free(obj);
            return;
        default:
            if (IS_EXTERNAL_SIMPLE(obj))
                runtime_fdmap_pop(runtime_get(), obj);
            else if (IS_EXTERNAL_COMPOUND(obj)) {
                runtime_fdmap_pop(runtime_get(), MAPLIST_KEY(obj));
                runtime_fdmap_pop(runtime_get(), obj);
            } else
                heap_free(obj);

            return;
    }
}

nil_t drop_raw(raw_p ptr) { heap_free(ptr); }

obj_p copy_obj(obj_p obj) {
    i64_t i, l;
    obj_p res;

    switch (obj->type) {
        case -TYPE_B8:
            return b8(obj->b8);
        case -TYPE_C8:
            return c8(obj->c8);
        case -TYPE_U8:
            return u8(obj->u8);
        case -TYPE_I16:
            return i16(obj->i16);
        case -TYPE_I32:
            return i32(obj->i32);
        case -TYPE_DATE:
            return adate(obj->i32);
        case -TYPE_TIME:
            return atime(obj->i32);
        case -TYPE_I64:
            return i64(obj->i64);
        case -TYPE_SYMBOL:
            return symboli64(obj->i64);
        case -TYPE_TIMESTAMP:
            return timestamp(obj->i64);
        case -TYPE_F64:
            return f64(obj->f64);
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_C8:
        case TYPE_I16:
        case TYPE_I32:
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
        case TYPE_F64:
        case TYPE_GUID:
            res = vector(obj->type, obj->len);
            memcpy(res->raw, obj->raw, size_of(obj) - sizeof(struct obj_t));
            return res;
        case TYPE_LIST:
            l = obj->len;
            res = LIST(l);
            res->rc = 1;
            for (i = 0; i < l; i++)
                AS_LIST(res)[i] = clone_obj(AS_LIST(obj)[i]);
            return res;
        case TYPE_ENUM:
        case TYPE_MAPLIST:
            return ray_value(obj);
        case TYPE_TABLE:
            return table(copy_obj(AS_LIST(obj)[0]), copy_obj(AS_LIST(obj)[1]));
        case TYPE_DICT:
            return dict(copy_obj(AS_LIST(obj)[0]), copy_obj(AS_LIST(obj)[1]));
        default:
            THROW(ERR_NOT_IMPLEMENTED, "cow: not implemented for type: '%s", type_name(obj->type));
    }
}

obj_p cow_obj(obj_p obj) {
    u32_t rc;

    // Complex types like enumerations or anymap may not be modified inplace
    if (obj->type == TYPE_ENUM || obj->type == TYPE_MAPLIST)
        return copy_obj(obj);

    /*
    Since it is forbidden to modify globals from several threads simultenously,
    we can just check for rc == 1 and if it is the case, we can just return the
    object
    */
    if (!__RC_SYNC)
        rc = (obj)->rc;
    else
        rc = __atomic_load_n(&obj->rc, __ATOMIC_RELAXED);

    // we only owns the reference, so we can freely modify it
    if (rc == 1)
        return obj;

    // we don't own the reference, so we need to copy object
    return copy_obj(obj);
}

u32_t rc_obj(obj_p obj) {
    u32_t rc;

    if (!__RC_SYNC)
        rc = (obj)->rc;
    else
        rc = __atomic_load_n(&obj->rc, __ATOMIC_RELAXED);

    return rc;
}

str_p type_name(i8_t type) { return str_from_symbol(env_get_typename_by_type(&runtime_get()->env, type)); }

obj_p parse_str(lit_p str) { return parse(str, strlen(str), NULL_OBJ); }

b8_t rc_sync_get() { return __RC_SYNC; }

nil_t rc_sync_set(b8_t on) { __RC_SYNC = on; }