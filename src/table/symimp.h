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

#ifndef RAY_SYMIMP_H
#define RAY_SYMIMP_H

/* The import dictionary: a symbol file one converter builds alone.
 *
 * Interned strings are appended straight into the mapped file as its STRL
 * records ([u32 len | bytes], position i = i-th record), so the file is the
 * dictionary's storage: nothing is copied out at the end and its pages are
 * the page cache's to write back and evict.  In memory there are only the
 * record offsets and a hash index split into shards, each with its own
 * lock; a hit is found without any lock.  Position 0 is "".  POSIX only:
 * ray_symimp_create returns NULL elsewhere, and on any failure to set up,
 * and the caller keeps the ordinary domain. */

#include <rayforce.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ray_symimp_s ray_symimp_t;

/* Create (truncating) the symbol file at `path`.  `count` receives the
 * number of entries as they are added. */
ray_symimp_t* ray_symimp_create(const char* path, _Atomic(int64_t)* count);
/* Position of each string, adding the new ones; thread-safe.  False on
 * allocation or file growth failure. */
bool ray_symimp_intern_batch(ray_symimp_t* m, int64_t n, const char* const* strs,
                             const size_t* lens, const uint32_t* hashes,
                             int64_t* out_pos);
/* Write the header count and start writing back what was added since the
 * last call; `durable` cuts the file to its records and syncs it. */
ray_err_t ray_symimp_sync(ray_symimp_t* m, bool durable);
void ray_symimp_free(ray_symimp_t* m);

#endif
