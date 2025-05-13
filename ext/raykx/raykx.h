/*
 *   Copyright (c) 2025 Anton Kundenko <singaraiona@gmail.com>
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

#ifndef RAYKX_H
#define RAYKX_H

#include "../../core/rayforce.h"

// KDB+ IPC Protocol Constants
#define KDB_MSG_ASYN 0
#define KDB_MSG_SYNC 1
#define KDB_MSG_RESP 2
#define KDB_MSG_ERR 128

// KDB+ IPC Context
typedef struct raykx_ctx_t {
    obj_p name;
    u8_t msgtype;
} *raykx_ctx_p;

// KDB+ IPC Header
typedef struct raykx_header_t {
    u8_t endianness;
    u8_t msgtype;
    u8_t compressed;
    u8_t reserved;
    u32_t size;
} *raykx_header_p;

obj_p raykx_hopen(obj_p addr);
obj_p raykx_hclose(obj_p fd);
obj_p raykx_send(obj_p fd, obj_p msg);

#endif  // RAYKX_H
