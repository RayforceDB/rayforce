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
#include "string.h"
#include "util.h"

#if defined(OS_WINDOWS)

raw_p mmap_stack(u64_t size) { return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }

raw_p mmap_alloc(u64_t size) { return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }

raw_p mmap_file(i64_t fd, raw_p addr, u64_t size, i64_t offset) {
    HANDLE hMapping;
    raw_p ptr;

    hMapping = CreateFileMapping((HANDLE)fd, NULL, PAGE_READWRITE, 0, size, NULL);

    if (hMapping == NULL) {
        return NULL;
    }

    ptr = MapViewOfFile(hMapping, FILE_MAP_ALL_ACCESS, (DWORD)(offset >> 32), (DWORD)(offset & 0xFFFFFFFF), size);
    CloseHandle(hMapping);

    return ptr;
}

i64_t mmap_free(raw_p addr, u64_t size) {
    UNUSED(size);
    return VirtualFree(addr, 0, MEM_RELEASE);
}

i64_t mmap_sync(raw_p addr, u64_t size) { return FlushViewOfFile(addr, size); }

raw_p mmap_reserve(raw_p addr, u64_t size) {
    raw_p ptr = VirtualAlloc(addr, size, MEM_RESERVE, PAGE_NOACCESS);

    if (ptr == NULL)
        return NULL;

    return ptr;
}

i64_t mmap_commit(raw_p addr, u64_t size) {
    if (VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE) == NULL)
        return -1;

    return 0;
}

#elif defined(OS_LINUX)

raw_p mmap_stack(u64_t size) {
    return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE | MAP_STACK, -1, 0);
}

raw_p mmap_alloc(u64_t size) {
    raw_p ptr;

    ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED | MAP_NONBLOCK | MAP_POPULATE, -1, 0);

    if (ptr == MAP_FAILED)
        return NULL;

    return ptr;
}

raw_p mmap_file(i64_t fd, raw_p addr, u64_t size, i64_t offset) {
    raw_p ptr = mmap(addr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NONBLOCK | MAP_POPULATE, fd, offset);

    if (ptr == MAP_FAILED)
        return NULL;

    return ptr;
}

i64_t mmap_free(raw_p addr, u64_t size) { return munmap(addr, size); }

i64_t mmap_sync(raw_p addr, u64_t size) { return msync(addr, size, MS_SYNC); }

raw_p mmap_reserve(raw_p addr, u64_t size) {
    raw_p ptr;

    ptr = mmap(addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (ptr == MAP_FAILED)
        return NULL;

    return ptr;
}

i64_t mmap_commit(raw_p addr, u64_t size) { return mprotect(addr, size, PROT_READ | PROT_WRITE); }

#elif defined(OS_MACOS)

#define MAP_ANON 0x1000
#define MAP_ANONYMOUS MAP_ANON

raw_p mmap_stack(u64_t size) {
    return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
}

raw_p mmap_alloc(u64_t size) {
    raw_p ptr;

    ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);

    if (ptr == MAP_FAILED)
        return NULL;

    return ptr;
}

raw_p mmap_file(i64_t fd, raw_p addr, u64_t size, i64_t offset) {
    raw_p ptr;

    ptr = mmap(addr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);

    if (ptr == MAP_FAILED)
        return NULL;

    return ptr;
}

i64_t mmap_free(raw_p addr, u64_t size) { return munmap(addr, size); }

i64_t mmap_sync(raw_p addr, u64_t size) { return msync(addr, size, MS_SYNC); }

raw_p mmap_reserve(raw_p addr, u64_t size) {
    raw_p ptr;

    ptr = mmap(addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (ptr == MAP_FAILED)
        return NULL;

    return ptr;
}

i64_t mmap_commit(raw_p addr, u64_t size) { return mprotect(addr, size, PROT_READ | PROT_WRITE); }

#endif
