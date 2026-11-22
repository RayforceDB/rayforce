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

#ifndef RAY_JSON_H
#define RAY_JSON_H

/*
 * json.h -- JSON text in and out (docs/docs/namespaces/json.md).
 *
 *   (.json.parse s [limits])  JSON text to a value; element-wise over a
 *                             string vector.  Strict (RFC 8259): malformed
 *                             text is a `parse` error naming the byte, never
 *                             a partial value.  Never evaluates anything and
 *                             interns nothing: object keys are strings.
 *   (.json.get s path [limits]) one field of each document, by a dotted path
 *                             ("data.ids.0"), building only that field; a
 *                             missing path is null.
 *   (.json.format v)          a value to JSON text.
 *
 * The input, depth, node and allocation caps are read-value's
 * (ray_read_limits_default), and `limits` overrides them by the same names.
 */
#include <rayforce.h>

ray_t* ray_json_parse_fn(ray_t** args, int64_t n);
ray_t* ray_json_get_fn(ray_t** args, int64_t n);
ray_t* ray_json_format_fn(ray_t* v);

#endif /* RAY_JSON_H */
