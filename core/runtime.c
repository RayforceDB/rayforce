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

#include "runtime.h"
#include "mmap.h"
#include "alloc.h"
#include "util.h"

// Global runtime reference
runtime_t _RUNTIME = NULL;

null_t runtime_init(u16_t slaves)
{
    rf_alloc_init();

    _RUNTIME = mmap_malloc(ALIGNUP(sizeof(struct runtime_t), PAGE_SIZE));

    _RUNTIME->slaves = slaves;
    _RUNTIME->symbols = symbols_new();
    _RUNTIME->env = create_env();
}

null_t runtime_cleanup()
{
    symbols_free(_RUNTIME->symbols);
    mmap_free(_RUNTIME->symbols, sizeof(symbols_t));
    free_env(&_RUNTIME->env);
    mmap_free(_RUNTIME, ALIGNUP(sizeof(struct runtime_t), PAGE_SIZE));
    rf_alloc_cleanup();
}

runtime_t runtime_get()
{
    return _RUNTIME;
}
