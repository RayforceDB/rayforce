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

#ifndef MMAP_H
#define MMAP_H

#ifndef __USE_MISC
#define __USE_MISC
#define _DEFAULT_SOURCE
#endif

#ifdef OS_WASM
#ifndef PAGE_SIZE
#define PAGE_SIZE 65536
#endif
#else
#define PAGE_SIZE 4096
#endif

#include "rayforce.h"

raw_p mmap_stack(u64_t size);
raw_p mmap_alloc(u64_t size);
raw_p mmap_file(i64_t fd, u64_t size, i32_t shared);
i64_t mmap_free(raw_p addr, u64_t size);
i64_t mmap_sync(raw_p addr, u64_t size);
raw_p mmap_reserve(raw_p addr, u64_t size);
i64_t mmap_commit(raw_p addr, u64_t size);

#endif // MMAP_H
