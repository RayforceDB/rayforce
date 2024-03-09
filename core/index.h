/*
 *   Copyright (c) 2024 Anton Kundenko <singaraiona@gmail.com>
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

#ifndef INDEX_H
#define INDEX_H

#include "rayforce.h"

typedef struct __index_list_ctx_t
{
    obj_p lcols;
    obj_p rcols;
    u64_t *hashes;
} __index_list_ctx_t;

typedef struct __find_ctx_t
{
    raw_p lobj;
    raw_p robj;
    u64_t *hashes;
} __find_ctx_t;

typedef struct index_scope_t
{
    i64_t min;
    i64_t max;
    u64_t range;
} index_scope_t;

index_scope_t index_scope(i64_t values[], i64_t filter[], u64_t len);
obj_p index_distinct_i8(i8_t values[], u64_t len, b8_t term);
obj_p index_distinct_i64(i64_t values[], u64_t len);
obj_p index_distinct_guid(guid_t values[], u64_t len);
obj_p index_distinct_obj(obj_p values[], u64_t len);
obj_p index_find_i8(i8_t x[], u64_t xl, i8_t y[], u64_t yl);
obj_p index_find_i64(i64_t x[], u64_t xl, i64_t y[], u64_t yl);
obj_p index_find_guid(guid_t x[], u64_t xl, guid_t y[], u64_t yl);
obj_p index_find_obj(obj_p x[], u64_t xl, obj_p y[], u64_t yl);
obj_p index_group_i8(i8_t values[], i64_t filter[], u64_t len);
obj_p index_group_i64(i64_t values[], i64_t filter[], u64_t len);
obj_p index_group_guid(guid_t values[], i64_t filter[], u64_t len);
obj_p index_group_obj(obj_p values[], i64_t filter[], u64_t len);
obj_p index_group_list(obj_p lst, i64_t filter[], u64_t len);
obj_p index_group_cnts(obj_p grp);
obj_p index_join_obj(obj_p lcols, obj_p rcols, u64_t len);
nil_t index_hash_obj(obj_p obj, u64_t out[], i64_t filter[], u64_t len, b8_t deref);

#endif // INDEX_H
