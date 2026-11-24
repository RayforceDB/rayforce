/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
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

/* A list of three errors, each with a code and a message of its own, and a
 * check of its wire form.  Shared by the ser/de, journal and IPC tests that
 * pin that every error record carries its own error's message (#1214). */

#ifndef RAY_TEST_ERR_LIST_H
#define RAY_TEST_ERR_LIST_H

#include <rayforce.h>
#include <stdint.h>
#include <string.h>

/* Raised in this order: the last one raised is the third, so a writer that
 * took the thread's last message would write "third" into every record. */
static const char* const ERR_LIST[3][2] = {
    { "type",   "first: a type error" },
    { "length", "second: a length error with a longer message" },
    { "domain", "third" },
};

static inline ray_t* err_list_new(void) {
    ray_t* l = ray_list_new(3);
    if (!l || RAY_IS_ERR(l)) return l;
    ray_t** cells = (ray_t**)ray_data(l);
    for (int i = 0; i < 3; i++) cells[i] = ray_error(ERR_LIST[i][0], "%s", ERR_LIST[i][1]);
    l->len = 3;
    return l;
}

/* Errors are not reference counted: the list's release leaves its cells. */
static inline void err_list_free(ray_t* l) {
    if (!l || RAY_IS_ERR(l)) return;
    ray_t** cells = (ray_t**)ray_data(l);
    for (int64_t i = 0; i < l->len; i++) { ray_error_free(cells[i]); cells[i] = RAY_NULL_OBJ; }
    ray_release(l);
}

/* The wire payload of err_list_new(): a LIST record (type, attrs, length),
 * then one error record per cell (type, 8-byte code, length byte, message).
 * -1 when all three records are the expected ones; the index of the first
 * that is not; -2 when the list record itself is wrong; 3 on trailing bytes. */
static inline int err_list_check(const uint8_t* p, int64_t n) {
    if (n < 10 || p[0] != RAY_LIST) return -2;
    int64_t len;
    memcpy(&len, p + 2, 8);
    if (len != 3) return -2;
    int64_t at = 10;
    for (int i = 0; i < 3; i++) {
        const char* code = ERR_LIST[i][0];
        const char* msg  = ERR_LIST[i][1];
        size_t cl = strlen(code), ml = strlen(msg);
        if (at + 10 > n || p[at] != (uint8_t)RAY_ERROR) return i;
        if (memcmp(p + at + 1, code, cl) != 0 || p[at + 1 + cl] != 0) return i;
        if (p[at + 9] != ml || at + 10 + (int64_t)ml > n) return i;
        if (memcmp(p + at + 10, msg, ml) != 0) return i;
        at += 10 + (int64_t)ml;
    }
    return at == n ? -1 : 3;
}

#endif /* RAY_TEST_ERR_LIST_H */
