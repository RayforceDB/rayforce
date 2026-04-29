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
#if defined(__linux__)
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
void  ray_vm_unmap_file(void* ptr, size_t size);
void  ray_vm_advise_seq(void* ptr, size_t size);
void  ray_vm_advise_willneed(void* ptr, size_t size);
void  ray_vm_release(void* ptr, size_t size);
void* ray_vm_alloc_aligned(size_t size, size_t alignment);

/* --------------------------------------------------------------------------
 * Threading API
 * -------------------------------------------------------------------------- */
ray_err_t ray_thread_create(ray_thread_t* t, ray_thread_fn fn, void* arg);
ray_err_t ray_thread_join(ray_thread_t t);
uint32_t ray_thread_count(void);

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
#else
  #include <semaphore.h>
  typedef sem_t ray_sem_t;
#endif

ray_err_t ray_sem_init(ray_sem_t* s, uint32_t initial_value);
void     ray_sem_destroy(ray_sem_t* s);
void     ray_sem_wait(ray_sem_t* s);
void     ray_sem_signal(ray_sem_t* s);

#endif /* RAY_PLATFORM_H */
