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

#include "mmap.h"

#if defined(_WIN32) || defined(__CYGWIN__)
#include <windows.h>
#define MAP_FAILED (nil_t *)-1
#else
#include <sys/mman.h>
#endif

#include <stdlib.h>
#include "string.h"
#include "util.h"

#if defined(_WIN32) || defined(__CYGWIN__)

nil_t *mmap_stack(u64_t size)
{
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

nil_t *mmap_malloc(u64_t size)
{
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

nil_t *mmap_file(i64_t fd, u64_t size)
{
    unused(attrs);
    return MapViewOfFile(CreateFileMapping((HANDLE)fd, NULL, PAGE_READWRITE, 0, size, NULL), FILE_MAP_ALL_ACCESS, 0, 0, size);
}

i64_t mmap_free(nil_t *addr, u64_t size)
{
    unused(size);
    return VirtualFree(addr, 0, MEM_RELEASE);
}

i64_t mmap_sync(nil_t *addr, u64_t size)
{
    return FlushViewOfFile(addr, size);
}

#elif defined(__linux__)

nil_t *mmap_stack(u64_t size)
{
    return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_STACK | MAP_GROWSDOWN, -1, 0);
}

nil_t *mmap_malloc(u64_t size)
{
    return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
}

nil_t *mmap_file(i64_t fd, u64_t size)
{
    return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
}

i64_t mmap_free(nil_t *addr, u64_t size)
{
    return munmap(addr, size);
}

i64_t mmap_sync(nil_t *addr, u64_t size)
{
    return msync(addr, size, MS_SYNC);
}

#elif defined(__APPLE__) && defined(__MACH__)

#define MAP_ANON 0x1000
#define MAP_ANONYMOUS MAP_ANON

nil_t *mmap_stack(u64_t size)
{
    return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
}

nil_t *mmap_malloc(u64_t size)
{
    return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
}

nil_t *mmap_file(i64_t fd, u64_t size)
{
    return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
}

i64_t mmap_free(nil_t *addr, u64_t size)
{
    return munmap(addr, size);
}

i64_t mmap_sync(nil_t *addr, u64_t size)
{
    return msync(addr, size, MS_SYNC);
}

#else
#error Unknown environment!
#endif