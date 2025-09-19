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

#define INDEX_SCOPE_LIMIT RAY_PAGE_SIZE * 128

typedef enum index_type_t {
    INDEX_TYPE_IDS = 0,
    INDEX_TYPE_SHIFT,
    INDEX_TYPE_PARTEDCOMMON,
} index_type_t;

typedef struct __index_list_ctx_t {
    obj_p lcols;
    obj_p rcols;
    i64_t *hashes;
    i64_t *filter;
} __index_list_ctx_t;

typedef struct __index_find_ctx_t {
    raw_p lobj;
    raw_p robj;
    i64_t *hashes;
} __index_find_ctx_t;

typedef struct index_scope_t {
    i64_t min;
    i64_t max;
    i64_t range;
} index_scope_t;

i64_t index_group_count(obj_p index);
i64_t index_group_len(obj_p index);
index_type_t index_group_type(obj_p index);
i64_t *index_group_source(obj_p index);
i64_t *index_group_ids(obj_p index);
i64_t *index_group_filter_ids(obj_p index);
obj_p index_group_filter(obj_p index);
i64_t index_group_shift(obj_p index);
obj_p index_distinct_i8(i8_t values[], i64_t len);
obj_p index_distinct_i16(i16_t values[], i64_t len);
obj_p index_distinct_i32(i32_t values[], i64_t len);
obj_p index_distinct_i64(i64_t values[], i64_t len);
obj_p index_distinct_guid(guid_t values[], i64_t len);
obj_p index_distinct_obj(obj_p values[], i64_t len);
obj_p index_in_i8_i8(i8_t x[], i64_t xl, i8_t y[], i64_t yl);
obj_p index_in_i8_i16(i8_t x[], i64_t xl, i16_t y[], i64_t yl);
obj_p index_in_i8_i32(i8_t x[], i64_t xl, i32_t y[], i64_t yl);
obj_p index_in_i8_i64(i8_t x[], i64_t xl, i64_t y[], i64_t yl);
obj_p index_in_i16_i8(i16_t x[], i64_t xl, i8_t y[], i64_t yl);
obj_p index_in_i16_i16(i16_t x[], i64_t xl, i16_t y[], i64_t yl);
obj_p index_in_i16_i32(i16_t x[], i64_t xl, i32_t y[], i64_t yl);
obj_p index_in_i16_i64(i16_t x[], i64_t xl, i64_t y[], i64_t yl);
obj_p index_in_i32_i8(i32_t x[], i64_t xl, i8_t y[], i64_t yl);
obj_p index_in_i32_i16(i32_t x[], i64_t xl, i16_t y[], i64_t yl);
obj_p index_in_i32_i32(i32_t x[], i64_t xl, i32_t y[], i64_t yl);
obj_p index_in_i32_i64(i32_t x[], i64_t xl, i64_t y[], i64_t yl);
obj_p index_in_i64_i8(i64_t x[], i64_t xl, i8_t y[], i64_t yl);
obj_p index_in_i64_i16(i64_t x[], i64_t xl, i16_t y[], i64_t yl);
obj_p index_in_i64_i32(i64_t x[], i64_t xl, i32_t y[], i64_t yl);
obj_p index_in_i64_i64(i64_t x[], i64_t xl, i64_t y[], i64_t yl);
obj_p index_in_guid_guid(guid_t x[], i64_t xl, guid_t y[], i64_t yl);
obj_p index_find_i8(i8_t x[], i64_t xl, i8_t y[], i64_t yl);
obj_p index_find_i64(i64_t x[], i64_t xl, i64_t y[], i64_t yl);
obj_p index_find_guid(guid_t x[], i64_t xl, guid_t y[], i64_t yl);
obj_p index_find_obj(obj_p x[], i64_t xl, obj_p y[], i64_t yl);
obj_p index_group(obj_p val, obj_p filter);
obj_p index_group_list(obj_p obj, obj_p filter);
obj_p index_join_obj(obj_p lcols, obj_p rcols, i64_t len);
obj_p index_upsert_obj(obj_p lcols, obj_p rcols, i64_t len);
nil_t index_hash_obj(obj_p obj, i64_t out[], i64_t filter[], i64_t len, b8_t resolve);
obj_p index_xwin(obj_p val, obj_p n, obj_p filter);

#endif  // INDEX_H
