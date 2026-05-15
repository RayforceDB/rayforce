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

/* Feature test macros must come before any includes */
#if defined(__linux__)
  #define _GNU_SOURCE
#endif

#include "platform.h"

/* ==========================================================================
 * Linux / macOS (POSIX)
 * ========================================================================== */
#if defined(RAY_OS_LINUX) || defined(RAY_OS_MACOS)

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include "mem/sys.h"

/* --------------------------------------------------------------------------
 * Virtual memory
 * -------------------------------------------------------------------------- */
void* ray_vm_alloc(size_t size) {
    void* p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? NULL : p;
}

void ray_vm_free(void* ptr, size_t size) {
    if (ptr) munmap(ptr, size);
}

void* ray_vm_map_file(const char* path, size_t* out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return NULL;
    }

    if (st.st_size <= 0) {
        close(fd);
        if (out_size) *out_size = 0;
        return NULL;
    }

    size_t len = (size_t)st.st_size;
    void* p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);

    if (p == MAP_FAILED) return NULL;

    if (out_size) *out_size = len;
    return p;
}

void ray_vm_unmap_file(void* ptr, size_t size) {
    if (ptr) munmap(ptr, size);
}

void ray_vm_advise_seq(void* ptr, size_t size) {
    if (ptr) madvise(ptr, size, MADV_SEQUENTIAL);
}

void ray_vm_release(void* ptr, size_t size) {
    if (!ptr) return;
#if defined(RAY_OS_MACOS)
    madvise(ptr, size, MADV_FREE);
#else
    madvise(ptr, size, MADV_DONTNEED);
#endif
}

void* ray_vm_alloc_aligned(size_t size, size_t alignment) {
    size_t total = size + alignment;
    void* mem = mmap(NULL, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return NULL;

    uintptr_t addr = (uintptr_t)mem;
    uintptr_t aligned = (addr + alignment - 1) & ~(alignment - 1);

    /* Trim leading excess */
    if (aligned > addr)
        munmap(mem, aligned - addr);

    /* Trim trailing excess */
    uintptr_t end = addr + total;
    uintptr_t aligned_end = aligned + size;
    if (end > aligned_end)
        munmap((void*)aligned_end, end - aligned_end);

    return (void*)aligned;
}

/* --------------------------------------------------------------------------
 * Threading
 * -------------------------------------------------------------------------- */

/* pthread entry expects void*(*)(void*), but ray_thread_fn is void(*)(void*).
 * Use a small trampoline to bridge the signatures.                          */
typedef struct {
    ray_thread_fn fn;
    void*        arg;
} ray_thread_trampoline_t;

static void* thread_trampoline(void* raw) {
    ray_thread_trampoline_t ctx = *(ray_thread_trampoline_t*)raw;
    /* Free the trampoline struct allocated on the heap. We copied it first
     * so the creating thread can proceed freely.                            */
    ray_sys_free(raw);
    ctx.fn(ctx.arg);
    return NULL;
}

ray_err_t ray_thread_create(ray_thread_t* t, ray_thread_fn fn, void* arg) {
    ray_thread_trampoline_t* ctx = (ray_thread_trampoline_t*)ray_sys_alloc(sizeof(*ctx));
    if (!ctx) return RAY_ERR_OOM;
    ctx->fn  = fn;
    ctx->arg = arg;

    pthread_t pt;
    int rc = pthread_create(&pt, NULL, thread_trampoline, ctx);
    if (rc != 0) {
        ray_sys_free(ctx);
        return RAY_ERR_OOM;
    }
    *t = (ray_thread_t)pt;
    return RAY_OK;
}

ray_err_t ray_thread_join(ray_thread_t t) {
    int rc = pthread_join((pthread_t)t, NULL);
    return (rc == 0) ? RAY_OK : RAY_ERR_IO;
}

uint32_t ray_thread_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (uint32_t)n : 1;
}

/* --------------------------------------------------------------------------
 * Semaphore
 * -------------------------------------------------------------------------- */
#if defined(RAY_OS_MACOS)

ray_err_t ray_sem_init(ray_sem_t* s, uint32_t initial_value) {
    *s = dispatch_semaphore_create((long)initial_value);
    return (*s) ? RAY_OK : RAY_ERR_OOM;
}

void ray_sem_destroy(ray_sem_t* s) {
    /* dispatch_semaphore is ARC-managed on modern macOS; explicit release for
     * non-ARC builds (our C code).                                           */
    if (*s) dispatch_release(*s);
    *s = NULL;
}

void ray_sem_wait(ray_sem_t* s) {
    dispatch_semaphore_wait(*s, DISPATCH_TIME_FOREVER);
}

void ray_sem_signal(ray_sem_t* s) {
    dispatch_semaphore_signal(*s);
}

#else /* Linux */

ray_err_t ray_sem_init(ray_sem_t* s, uint32_t initial_value) {
    return (sem_init(s, 0, initial_value) == 0) ? RAY_OK : RAY_ERR_OOM;
}

void ray_sem_destroy(ray_sem_t* s) {
    sem_destroy(s);
}

void ray_sem_wait(ray_sem_t* s) {
    while (sem_wait(s) != 0) { /* retry on EINTR */ }
}

void ray_sem_signal(ray_sem_t* s) {
    sem_post(s);
}

#endif /* macOS vs Linux semaphore */

/* ==========================================================================
 * Windows
 * ========================================================================== */
#elif defined(RAY_OS_WINDOWS)

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* --------------------------------------------------------------------------
 * Virtual memory
 * -------------------------------------------------------------------------- */
void* ray_vm_alloc(size_t size) {
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

void ray_vm_free(void* ptr, size_t size) {
    (void)size;
    if (ptr) VirtualFree(ptr, 0, MEM_RELEASE);
}

void* ray_vm_map_file(const char* path, size_t* out_size) {
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return NULL;

    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(hFile, &file_size)) {
        CloseHandle(hFile);
        return NULL;
    }

    HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_WRITECOPY, 0, 0, NULL);
    if (!hMap) {
        CloseHandle(hFile);
        return NULL;
    }

    void* p = MapViewOfFile(hMap, FILE_MAP_COPY, 0, 0, 0);

    /* We can close both handles; the mapping keeps the file open internally. */
    CloseHandle(hMap);
    CloseHandle(hFile);

    if (!p) return NULL;

    if (out_size) *out_size = (size_t)file_size.QuadPart;
    return p;
}

void ray_vm_unmap_file(void* ptr, size_t size) {
    (void)size;
    if (ptr) UnmapViewOfFile(ptr);
}

void ray_vm_advise_seq(void* ptr, size_t size) {
    /* PrefetchVirtualMemory is Win8.1+. Best-effort; ignore failure. */
    WIN32_MEMORY_RANGE_ENTRY entry;
    entry.VirtualAddress = ptr;
    entry.NumberOfBytes  = size;
    PrefetchVirtualMemory(GetCurrentProcess(), 1, &entry, 0);
}

void ray_vm_release(void* ptr, size_t size) {
    if (!ptr) return;
    /* DiscardVirtualMemory (Win8.1+) or fallback to decommit+recommit */
    DiscardVirtualMemory(ptr, size);
}

void* ray_vm_alloc_aligned(size_t size, size_t alignment) {
    /* Over-allocate, find aligned offset. Can't trim on Windows, so the
     * pool header's vm_base field stores the original base for VirtualFree. */
    void* mem = VirtualAlloc(NULL, size + alignment,
                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) return NULL;
    uintptr_t aligned = ((uintptr_t)mem + alignment - 1) & ~(alignment - 1);
    return (void*)aligned;
}

/* --------------------------------------------------------------------------
 * Threading
 * -------------------------------------------------------------------------- */
typedef struct {
    ray_thread_fn fn;
    void*        arg;
} ray_thread_trampoline_t;

static DWORD WINAPI thread_trampoline(LPVOID raw) {
    ray_thread_trampoline_t ctx = *(ray_thread_trampoline_t*)raw;
    HeapFree(GetProcessHeap(), 0, raw);
    ctx.fn(ctx.arg);
    return 0;
}

ray_err_t ray_thread_create(ray_thread_t* t, ray_thread_fn fn, void* arg) {
    ray_thread_trampoline_t* ctx = HeapAlloc(GetProcessHeap(), 0, sizeof(*ctx));
    if (!ctx) return RAY_ERR_OOM;
    ctx->fn  = fn;
    ctx->arg = arg;

    HANDLE h = CreateThread(NULL, 0, thread_trampoline, ctx, 0, NULL);
    if (!h) {
        HeapFree(GetProcessHeap(), 0, ctx);
        return RAY_ERR_OOM;
    }
    *t = (ray_thread_t)h;
    return RAY_OK;
}

ray_err_t ray_thread_join(ray_thread_t t) {
    DWORD rc = WaitForSingleObject((HANDLE)t, INFINITE);
    CloseHandle((HANDLE)t);
    return (rc == WAIT_OBJECT_0) ? RAY_OK : RAY_ERR_IO;
}

uint32_t ray_thread_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (uint32_t)si.dwNumberOfProcessors;
}

/* --------------------------------------------------------------------------
 * Semaphore
 * -------------------------------------------------------------------------- */
ray_err_t ray_sem_init(ray_sem_t* s, uint32_t initial_value) {
    *s = CreateSemaphoreA(NULL, (LONG)initial_value, LONG_MAX, NULL);
    return (*s) ? RAY_OK : RAY_ERR_OOM;
}

void ray_sem_destroy(ray_sem_t* s) {
    if (*s) CloseHandle(*s);
    *s = NULL;
}

void ray_sem_wait(ray_sem_t* s) {
    WaitForSingleObject(*s, INFINITE);
}

void ray_sem_signal(ray_sem_t* s) {
    ReleaseSemaphore(*s, 1, NULL);
}

#endif /* RAY_OS_WINDOWS */

/* ==========================================================================
 * WASM (Emscripten)
 *
 * Single-threaded by construction.  VM allocs are plain malloc; mmap of
 * files goes through MEMFS via mmap()/munmap() (still works in emscripten
 * for files written into the in-memory FS).  Thread/semaphore ops are
 * stubs — pool.c will see thread_count() == 1 and skip worker creation.
 * ========================================================================== */
#if defined(RAY_OS_WASM)

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "mem/sys.h"

void* ray_vm_alloc(size_t size) {
    /* Emscripten provides MAP_ANONYMOUS; this is the cleanest way to get a
     * page-aligned region the heap can hand out.  Falls back to aligned
     * malloc if mmap is somehow refused (shouldn't happen on MEMFS). */
    void* p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        /* aligned_alloc requires size to be a multiple of alignment.
         * Round up to a 64KB WASM page. */
        size_t aligned = (size + 65535u) & ~(size_t)65535u;
        p = aligned_alloc(65536, aligned);
        return p;
    }
    return p;
}

void ray_vm_free(void* ptr, size_t size) {
    if (!ptr) return;
    if (munmap(ptr, size) != 0) free(ptr);
}

void* ray_vm_map_file(const char* path, size_t* out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        if (out_size) *out_size = 0;
        return NULL;
    }

    size_t len = (size_t)st.st_size;
    void* p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);

    if (p == MAP_FAILED) return NULL;
    if (out_size) *out_size = len;
    return p;
}

void ray_vm_unmap_file(void* ptr, size_t size) {
    if (ptr) munmap(ptr, size);
}

/* madvise hints are advisory and have no analog on WASM — no-ops. */
void ray_vm_advise_seq(void* ptr, size_t size)      { (void)ptr; (void)size; }
void ray_vm_release(void* ptr, size_t size)         { (void)ptr; (void)size; }

void* ray_vm_alloc_aligned(size_t size, size_t alignment) {
    /* aligned_alloc requires size to be a multiple of alignment per C17. */
    size_t aligned_size = (size + alignment - 1) & ~(alignment - 1);
    return aligned_alloc(alignment, aligned_size);
}

/* Threading — return errors / 1.  pool.c with n_workers==0 (the result of
 * thread_count==1 ⇒ ncpu-1 == 0) never invokes thread_create. */
ray_err_t ray_thread_create(ray_thread_t* t, ray_thread_fn fn, void* arg) {
    (void)t; (void)fn; (void)arg;
    return RAY_ERR_NYI;
}

ray_err_t ray_thread_join(ray_thread_t t) {
    (void)t;
    return RAY_OK;
}

uint32_t ray_thread_count(void) { return 1; }

/* Semaphore — counter-only.  Single-threaded so wait never blocks (the
 * counter must already be positive when wait fires). */
ray_err_t ray_sem_init(ray_sem_t* s, uint32_t initial_value) {
    *s = (int32_t)initial_value;
    return RAY_OK;
}

void ray_sem_destroy(ray_sem_t* s) { (void)s; }

void ray_sem_wait(ray_sem_t* s) {
    if (*s > 0) (*s)--;
}

void ray_sem_signal(ray_sem_t* s) { (*s)++; }

#endif /* RAY_OS_WASM */
