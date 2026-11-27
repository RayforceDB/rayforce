/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#ifndef RAY_ERRPATH_H
#define RAY_ERRPATH_H

#include <string.h>

/* A path as an error message shows it (#1883).  An error message holds 255
 * bytes; a path is the caller's and unbounded, so one written whole ahead of
 * the reason could leave no room for it.  A path up to RAY_ERR_PATH_MAX
 * bytes is shown as it is; a longer one as "..." and its last bytes, cut at
 * a character boundary, so the operation and the reason always fit.  `buf`
 * holds RAY_ERR_PATH_MAX + 1 bytes; the answer is `path` or `buf`. */
#define RAY_ERR_PATH_MAX 120

/* As ray_err_path, at most `max` (<= RAY_ERR_PATH_MAX) bytes: for a
 * message that names two paths. */
static inline const char* ray_err_path_n(const char* path, char* buf, size_t max) {
    if (!path) return "";
    if (max > RAY_ERR_PATH_MAX) max = RAY_ERR_PATH_MAX;
    if (max < 8) max = 8;
    size_t n = strlen(path);
    if (n <= max) return path;
    size_t keep = max - 3;
    const char* tail = path + (n - keep);
    /* never start inside a UTF-8 sequence */
    while (*tail && ((unsigned char)*tail & 0xC0) == 0x80) { tail++; keep--; }
    memcpy(buf, "...", 3);
    memcpy(buf + 3, tail, keep);
    buf[3 + keep] = '\0';
    return buf;
}

static inline const char* ray_err_path(const char* path, char* buf) {
    return ray_err_path_n(path, buf, RAY_ERR_PATH_MAX);
}

#endif /* RAY_ERRPATH_H */
