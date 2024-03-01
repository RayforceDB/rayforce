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

#ifndef SERDE_H
#define SERDE_H

#include "rayforce.h"
#include "util.h"

#define SERDE_PREFIX 0xcefadefa

typedef struct header_t
{
    u32_t prefix; // marker
    u8_t version; // version of the app
    u8_t flags;   // 0 - no flags
    u8_t endian;  // 0 - little, 1 - big
    u8_t msgtype; // used for ipc: 0 - async, 1 - sync, 2 - response
    u64_t size;   // size of the payload (in bytes)
} header_t;

CASSERT(sizeof(header_t) == 16, header_t)

obj_p de_raw(u8_t *buf, u64_t len);
i64_t ser_raw(u8_t **buf, obj_p obj);
u64_t size_of_type(i8_t type);
u64_t size_of(obj_p obj);
u64_t size_obj(obj_p obj);
u64_t save_obj(u8_t *buf, u64_t len, obj_p obj);
obj_p load_obj(u8_t **buf, u64_t len);

#endif // SERDE_H
