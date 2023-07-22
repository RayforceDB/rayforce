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

#define PAGE_SIZE 4096

#if defined(_WIN32) || defined(__CYGWIN__)
#include <windows.h>
#define MAP_FAILED (null_t *)-1
#else
#include <sys/mman.h>
#endif

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(__CYGWIN__)
#define mmap_stack(size) MapViewOfFile(CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, size, NULL), FILE_MAP_ALL_ACCESS, 0, 0, size);
#define mmap_malloc(size) MapViewOfFile(CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, size, NULL), FILE_MAP_ALL_ACCESS, 0, 0, size);
#define mmap_file(size, flags, fd) MapViewOfFile(CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, size, NULL), FILE_MAP_ALL_ACCESS, 0, 0, size);
#define mmap_free(addr, size) UnmapViewOfFile(addr);
#elif defined(__linux__)
#define mmap_stack(size) mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_STACK | MAP_GROWSDOWN, -1, 0);
#define mmap_malloc(size) mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
#define mmap_file(size, flags, fd) mmap(NULL, size, flags, MAP_PRIVATE, fd, 0);
#define mmap_free(addr, size) munmap(addr, size);
#define mmap_sync(addr, size) msync(addr, size, MS_SYNC);
#elif defined(__APPLE__) && defined(__MACH__)
#define MAP_ANON 0x1000
#define MAP_ANONYMOUS MAP_ANON
#define mmap_stack(size) mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0)
#define mmap_malloc(size) mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
#define mmap_file(size, flags, fd) mmap(NULL, size, flags, MAP_PRIVATE, fd, 0);
#define mmap_free(addr, size) munmap(addr, size);
#define mmap_sync(addr, size) msync(addr, size, MS_SYNC);
#else
#error Unknown environment!
#endif

#endif
