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

#ifndef RAY_PLATFORM_H
#define RAY_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/* --------------------------------------------------------------------------
 * OS detection
 * -------------------------------------------------------------------------- */
/* Detect WASM/Emscripten *before* Linux so we don't pull in Linux-only
 * headers (madvise, sem_*, pthread) that emscripten's sysroot stubs out.
 * platform.c provides a dedicated WASM arm with malloc/MEMFS shims. */
#if defined(__EMSCRIPTEN__)
  #define RAY_OS_WASM    1
#elif defined(__linux__)
  #define RAY_OS_LINUX   1
#elif defined(__APPLE__) && defined(__MACH__)
  #define RAY_OS_MACOS   1
#elif defined(_WIN32)
  #define RAY_OS_WINDOWS 1
#else
  #error "Unsupported platform"
#endif

/* --------------------------------------------------------------------------
 * Compiler hints
 * -------------------------------------------------------------------------- */
#if !defined(RAY_LIKELY)
#if defined(__GNUC__) || defined(__clang__)
  #define RAY_LIKELY(x)   __builtin_expect(!!(x), 1)
  #define RAY_UNLIKELY(x) __builtin_expect(!!(x), 0)
  #define RAY_ALIGN(n)    __attribute__((aligned(n)))
  #define RAY_INLINE      static inline __attribute__((always_inline))
#elif defined(_MSC_VER)
  #define RAY_LIKELY(x)   (x)
  #define RAY_UNLIKELY(x) (x)
  #define RAY_ALIGN(n)    __declspec(align(n))
  #define RAY_INLINE      static __forceinline
#else
  #define RAY_LIKELY(x)   (x)
  #define RAY_UNLIKELY(x) (x)
  #define RAY_ALIGN(n)
  #define RAY_INLINE      static inline
#endif
#endif /* !RAY_LIKELY */

/* --------------------------------------------------------------------------
 * Thread-local storage
 * -------------------------------------------------------------------------- */
#if !defined(RAY_TLS)
#if defined(_MSC_VER)
  #define RAY_TLS __declspec(thread)
#else
  #define RAY_TLS _Thread_local
#endif
#endif /* !RAY_TLS */

/* --------------------------------------------------------------------------
 * Atomics
 * -------------------------------------------------------------------------- */
#if !defined(ray_atomic_inc)
#if defined(_MSC_VER)
  #include <intrin.h>
  /* MSVC Interlocked* return the NEW value; adjust to match fetch_add/
   * fetch_sub semantics (return OLD value).
   * _InterlockedIncrement returns new, subtract 1 to get pre-increment.
   * _InterlockedDecrement returns new, add 1 to get pre-decrement.
   * On ARM use _nf (no fence) / _rel variants for relaxed/release semantics. */
  #if defined(_M_ARM) || defined(_M_ARM64)
    #define ray_atomic_inc(p)   (_InterlockedIncrement_nf((volatile long*)(p)) - 1)
    #define ray_atomic_dec(p)   (_InterlockedDecrement_rel((volatile long*)(p)) + 1)
    #define ray_atomic_fence_acquire()  __dmb(_ARM_BARRIER_ISH)
  #else
    #define ray_atomic_inc(p)   (_InterlockedIncrement((volatile long*)(p)) - 1)
    #define ray_atomic_dec(p)   (_InterlockedDecrement((volatile long*)(p)) + 1)
    #define ray_atomic_fence_acquire()  _ReadWriteBarrier()
  #endif
  #define ray_atomic_load(p)  _InterlockedOr((volatile long*)(p), 0)
  #define ray_atomic_store(p, v) _InterlockedExchange((volatile long*)(p), (long)(v))
  #define ray_atomic_cas(p, expected, desired) \
      (_InterlockedCompareExchange((volatile long*)(p), (long)(desired), (long)(*(expected))) == (long)(*(expected)))
#else
  #include <stdatomic.h>
  #define ray_atomic_inc(p)   __atomic_fetch_add(p, 1, __ATOMIC_RELAXED)
  #define ray_atomic_dec(p)   __atomic_fetch_sub(p, 1, __ATOMIC_RELEASE)
  #define ray_atomic_load(p)  __atomic_load_n(p, __ATOMIC_ACQUIRE)
  #define ray_atomic_store(p, v) __atomic_store_n(p, v, __ATOMIC_RELEASE)
  #define ray_atomic_cas(p, expected, desired) \
      __atomic_compare_exchange_n(p, expected, desired, 0, \
          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
  #define ray_atomic_fence_acquire()  __atomic_thread_fence(__ATOMIC_ACQUIRE)
#endif
#endif /* !ray_atomic_inc */

/* --------------------------------------------------------------------------
 * CPU spin-loop relax hint
 *
 * Emit inside busy-wait spin loops (spinlocks, pending-task waits) to cut
 * contention and power draw on SMT cores: `pause` on x86, `yield` on
 * aarch64, a no-op elsewhere.  Callers layer their own sched_yield()
 * escalation on top when a spin may run long.
 * -------------------------------------------------------------------------- */
#if defined(__x86_64__) || defined(__i386__)
  #define RAY_CPU_RELAX() __builtin_ia32_pause()
#elif defined(__aarch64__)
  #define RAY_CPU_RELAX() __asm__ volatile("yield" ::: "memory")
#else
  #define RAY_CPU_RELAX() ((void)0)
#endif

/* --------------------------------------------------------------------------
 * Pull in the public header for ray_err_t, ray_t, etc.
 * -------------------------------------------------------------------------- */
#include <rayforce.h>

/* --------------------------------------------------------------------------
 * Thread types
 * -------------------------------------------------------------------------- */
#if defined(_WIN32)
  typedef void* ray_thread_t;
#else
  typedef unsigned long ray_thread_t;
#endif

typedef void (*ray_thread_fn)(void* arg);

/* --------------------------------------------------------------------------
 * Platform VM API
 * -------------------------------------------------------------------------- */
void* ray_vm_alloc(size_t size);
void  ray_vm_free(void* ptr, size_t size);
void* ray_vm_map_file(const char* path, size_t* out_size);
/* The identity of a mapped file, read from the descriptor the mapping was
 * made from (not the path, which may name another file by now): inode,
 * size, modification and status-change times in ns.  valid is false where
 * the platform gives none (Windows, WASM). */
typedef struct { bool valid; uint64_t ino, size; int64_t mtime_ns, ctime_ns; } ray_file_id_t;
void* ray_vm_map_file_id(const char* path, size_t* out_size, ray_file_id_t* id);
void* ray_vm_map_fd_ro(int fd, size_t size);
void  ray_vm_unmap_file(void* ptr, size_t size);
/* Zeroed writable memory released by ray_vm_unmap_file (a generated file image). */
void* ray_vm_map_anon(size_t size);
void  ray_vm_advise_seq(void* ptr, size_t size);
/* The host page size (sysconf / GetSystemInfo, read once; 4096 when the host
 * does not say): what a madvise start must be aligned to. */
size_t ray_vm_page_size(void);
/* Ask the kernel to start reading [ptr, ptr+size) of a file mapping now,
 * asynchronously, so a later access finds it resident.  Best effort. */
void  ray_vm_advise_willneed(void* ptr, size_t size);
/* Bytes asked for so far by ray_vm_advise_willneed, process-wide: the
 * pool's trace summary, and tests of readers that must ask before they
 * read a mapped column they sample. */
uint64_t ray_vm_willneed_bytes(void);
/* Start writing back bytes [off, off+len) of open file `fd` without waiting
 * (Linux sync_file_range WRITE; a no-op elsewhere): a later fsync then
 * finds them on disk. */
void  ray_file_writeback_start(int fd, int64_t off, int64_t len);
/* The I/O counters the converters' traces read: bytes read from and
 * written to storage, written bytes whose writeback was cancelled, read and
 * write calls, and major faults; of the process, or of the calling thread
 * (thread true).  Linux reads /proc/self/io (/proc/thread-self/io) and
 * getrusage; every field reads zero elsewhere, or where they cannot be read. */
typedef struct { int64_t rd, wr, cwr, syscr, syscw, majflt; } ray_io_counters_t;
void  ray_io_counters(bool thread, ray_io_counters_t* out);
void  ray_sleep_ms(int ms);
/* True when a sample of the pages of [ptr, ptr+size) is resident: a cheap
 * check (a handful of page probes) that a read-ahead would find nothing to
 * read.  Where residency cannot be queried it reports false. */
bool  ray_vm_resident(const void* ptr, size_t size);
/* How far into [ptr, ptr+size) pages are in RAM: to the end of the last
 * resident page (mincore), 0 if none; size where residency cannot be
 * queried. */
size_t ray_vm_resident_end(const void* ptr, size_t size);
void  ray_vm_release(void* ptr, size_t size);
/* Release physical pages for a free block.  When hugepage is true, only the
 * 2MB-aligned interior is released so a partial MADV_DONTNEED does not
 * shatter the transparent huge page. */
void ray_vm_release_block(void* blk, size_t bsize, bool hugepage);
void* ray_vm_alloc_aligned(size_t size, size_t alignment);
/* Hint the kernel to back [ptr, ptr+size) with huge pages.  Returns true
 * if the hint was issued (Linux THP); false where unsupported (macOS, WASM,
 * Windows). */
bool ray_vm_hugepage(void* ptr, size_t size);
/* Lock [ptr, ptr+size) of an anonymous mapping against swap-out, page by
 * page as each is faulted in (Linux: mlock2 MLOCK_ONFAULT); munmap unlocks
 * it.  False where it cannot be — RLIMIT_MEMLOCK, a kernel before 4.4, a
 * platform other than Linux — and nothing is locked then (errno says why). */
bool    ray_vm_lock(void* ptr, size_t size);
/* Unlock a range ray_vm_lock locked; its pages stay. */
void    ray_vm_unlock(void* ptr, size_t size);
/* ray_vm_release / ray_vm_release_block for a locked range, which
 * MADV_DONTNEED refuses: MADV_DONTNEED_LOCKED (Linux 5.18), else unlock,
 * release and lock again.  0: released, still locked; 1: released, left
 * unlocked (the lock again failed); -1: nothing released. */
int     ray_vm_release_locked(void* ptr, size_t size);
int     ray_vm_release_block_locked(void* blk, size_t bsize, bool hugepage);
/* For a block whose lock is not known: MADV_DONTNEED, and where that is
 * refused for a lock, as ray_vm_release_block_locked.  Same returns. */
int     ray_vm_release_block_any(void* blk, size_t bsize, bool hugepage);
/* Whether this process holds CAP_IPC_LOCK, past RLIMIT_MEMLOCK (Linux). */
bool    ray_os_has_ipc_lock(void);
/* Bytes of this process's locked mappings (VmLck); 0 where unknown. */
int64_t ray_vm_locked_bytes(void);
/* Raise RLIMIT_MEMLOCK's soft limit to its hard limit (Linux; a no-op
 * elsewhere, and where it already is). */
void    ray_os_raise_memlock(void);

/* --------------------------------------------------------------------------
 * Threading API
 * -------------------------------------------------------------------------- */
ray_err_t ray_thread_create(ray_thread_t* t, ray_thread_fn fn, void* arg);
ray_err_t ray_thread_join(ray_thread_t t);
uint32_t ray_thread_count(void);
/* The CPUs this process may run on: the affinity mask on Linux (taskset, a
 * cpuset or docker --cpuset-cpus narrow it), ray_thread_count elsewhere. */
uint32_t ray_cpu_set_count(void);
/* Test hook: a mask buffer below n bytes fails as on a kernel with a wider
 * mask (EINVAL), so the retry with a larger buffer is exercised; 0 = none. */
void     ray_cpu_set_test_min_bytes(size_t n);
/* Physical cores (SMT siblings collapsed); falls back to the logical
 * count when topology is unavailable. */
uint32_t ray_physical_core_count(void);
/* Total last-level cache capacity summed over every LLC instance (bytes);
 * 0 when the platform cannot report it.  Bounds replicated per-task state
 * whose random-access working set must stay cache-resident to scale. */
uint64_t ray_cache_llc_bytes(void);
/* Memory limit imposed on this process by its container / cgroup (bytes):
 * the smallest cgroup v2 memory.max or v1 memory.limit_in_bytes found on
 * the process's cgroup and its ancestors.  0 when none is set, or on
 * platforms without cgroups. */
int64_t ray_os_cgroup_mem_limit(void);
/* CPU quota of this process's cgroup (docker --cpus, systemd CPUQuota=), in
 * millicpus: the smallest v2 cpu.max or v1 cfs quota over its period on the
 * cgroup and its ancestors.  0 when none is set, or without cgroups. */
int64_t ray_os_cgroup_cpu_quota(void);
/* Test hook: read /proc/self/{cgroup,mountinfo} and the cgroup files of the
 * quota from under `dir` (NULL: the real ones). */
void    ray_os_cgroup_test_root(const char* dir);
/* The directory of the cgroup that sets that limit, into buf[n]; false when
 * no limit is set, or on platforms without cgroups. */
bool    ray_os_cgroup_mem_dir(char* buf, size_t n);
/* Read the memory.stat of cgroup `cg` (a directory from
 * ray_os_cgroup_mem_dir), for what reading does: the kernel updates a
 * cgroup's statistics lazily and flushes them when the file is read.  With
 * open/read, no stdio.  false when there is no such file (and on platforms
 * without cgroups). */
bool    ray_os_cgroup_stat_flush(const char* cg);
#ifdef DEBUG
/* Pin ray_cache_llc_bytes to `bytes` (0 = probe again). */
void ray_cache_llc_set_for_test(uint64_t bytes);
#endif

void ray_parallel_begin(void);
void ray_parallel_end(void);
extern _Atomic(uint32_t) ray_parallel_flag;

/* --------------------------------------------------------------------------
 * Semaphore (platform-specific, not in the public header)
 * -------------------------------------------------------------------------- */
#if defined(RAY_OS_WINDOWS)
  typedef void* ray_sem_t;  /* HANDLE */
#elif defined(RAY_OS_MACOS)
  #include <dispatch/dispatch.h>
  typedef dispatch_semaphore_t ray_sem_t;
#elif defined(RAY_OS_WASM)
  /* WASM is single-threaded by construction; semaphores are no-op stubs. */
  typedef int32_t ray_sem_t;
#else
  #include <semaphore.h>
  typedef sem_t ray_sem_t;
#endif

ray_err_t ray_sem_init(ray_sem_t* s, uint32_t initial_value);
void     ray_sem_destroy(ray_sem_t* s);
void     ray_sem_wait(ray_sem_t* s);
void     ray_sem_signal(ray_sem_t* s);

#endif /* RAY_PLATFORM_H */
