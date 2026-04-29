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

#include "meta.h"
#include "store/col.h"
#include <string.h>
#include <stdio.h>

/* --------------------------------------------------------------------------
 * .d file: serialized I64 vector of column name symbol IDs
 *
 * ray_meta_save_d: write schema vector to .d file
 * ray_meta_load_d: read .d file back as I64 vector
 * -------------------------------------------------------------------------- */

ray_err_t ray_meta_save_d(ray_t* schema, const char* path) {
    if (!schema || RAY_IS_ERR(schema)) return RAY_ERR_TYPE;
    return ray_col_save(schema, path);
}

ray_t* ray_meta_load_d(const char* path) {
    return ray_col_load(path);
}
