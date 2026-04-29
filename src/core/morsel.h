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

#ifndef RAY_MORSEL_H
#define RAY_MORSEL_H

/*
 * morsel.h -- Morsel iterator infrastructure.
 *
 * A morsel is a chunk of up to RAY_MORSEL_ELEMS (1024) elements from a vector.
 * The iterator advances through the vector one morsel at a time, providing
 * direct data pointers and null bitmap pointers for each chunk.
 */

#include "ops/ops.h"

/* Initialize a morsel iterator over a sub-range [start, end) of vec.
 * Used by parallel dispatch to partition work across workers. */
void ray_morsel_init_range(ray_morsel_t* m, ray_t* vec, int64_t start, int64_t end);

#endif /* RAY_MORSEL_H */
