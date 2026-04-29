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

#ifndef RAY_NFO_H
#define RAY_NFO_H

#include <rayforce.h>

/* ===== Source Span ===== */

/* 8-byte source location: packs start/end line+col into a single int64.
 * id == 0 means "no span information available". */
typedef union ray_span_t {
    int64_t id;
    struct {
        uint16_t start_line;
        uint16_t end_line;
        uint16_t start_col;
        uint16_t end_col;
    };
} ray_span_t;

/* ===== Nfo Object ===== */

/* An nfo is a RAY_LIST with 4 elements:
 *   [0] filename  (RAY_STR atom)
 *   [1] source    (RAY_STR atom)
 *   [2] keys      (RAY_I64 vector — intptr_t node pointers)
 *   [3] vals      (RAY_I64 vector — span ids)
 */

#define NFO_FILENAME(nfo)  ray_list_get((nfo), 0)
#define NFO_SOURCE(nfo)    ray_list_get((nfo), 1)
#define NFO_KEYS(nfo)      ray_list_get((nfo), 2)
#define NFO_VALS(nfo)      ray_list_get((nfo), 3)

/* Create a new nfo object for the given source file.
 * Returns a RAY_LIST or ray_error() on failure. */
ray_t* ray_nfo_create(const char* filename, size_t fname_len,
                      const char* source,   size_t src_len);

/* Record the source span for an AST node. */
void ray_nfo_insert(ray_t* nfo, ray_t* node, ray_span_t span);

/* Look up the source span for an AST node.
 * Returns a span with id==0 if not found. */
ray_span_t ray_nfo_get(ray_t* nfo, ray_t* node);

#endif /* RAY_NFO_H */
