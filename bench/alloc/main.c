/* Allocator micro-benchmark. */
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#else
#  define _POSIX_C_SOURCE 200809L
#endif
#include <rayforce.h>
#include "mem/heap.h"
#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>
#include <pthread.h>
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <psapi.h>
#else
#  include <sys/resource.h>
#endif

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* (a) single-thread alloc/free throughput at one size class */
static void bench_size(const char* label, size_t data_size, long iters) {
    double t0 = now_s();
    for (long i = 0; i < iters; i++) {
        ray_t* v = ray_alloc(data_size);
        ray_free(v);
    }
    double dt = now_s() - t0;
    printf("  %-14s %8.1f Mops/s  (%ld iters, %.3fs)\n",
           label, (double)iters / dt / 1e6, iters, dt);
}

/* (b) producer-consumer: producer allocs into a ring, consumer frees */
#define RING 4096
static _Atomic(ray_t*) g_ring[RING];
static _Atomic long g_produced, g_consumed;
static long g_pc_iters;
static size_t g_pc_size;

static void* producer(void* _) {
    (void)_;
    ray_heap_init();
    for (long i = 0; i < g_pc_iters; i++) {
        ray_t* v = ray_alloc(g_pc_size);
        long slot = i & (RING - 1);
        ray_t* expect = NULL;
        while (!atomic_compare_exchange_weak(&g_ring[slot], &expect, v)) expect = NULL;
        atomic_fetch_add(&g_produced, 1);
    }
    /* Do not destroy here: consumer still holds references to our pool
     * pages and will ray_free them.  Heap is cleaned up when process exits. */
    return NULL;
}
static void* consumer(void* _) {
    (void)_;
    ray_heap_init();
    for (long i = 0; i < g_pc_iters; i++) {
        long slot = i & (RING - 1);
        ray_t* v;
        while ((v = atomic_exchange(&g_ring[slot], NULL)) == NULL) ;
        ray_free(v);
        atomic_fetch_add(&g_consumed, 1);
    }
    return NULL;
}

static long max_rss_kb(void) {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return (long)(pmc.PeakWorkingSetSize / 1024);
#else
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
    /* Linux: ru_maxrss is KB; macOS: bytes.  Normalize to KB. */
#if defined(__APPLE__)
    return ru.ru_maxrss / 1024;
#else
    return ru.ru_maxrss;
#endif
#endif
}

int main(void) {
    ray_heap_init();
    printf("(a) single-thread alloc/free:\n");
    bench_size("atom-64B",   0,             20000000);
    bench_size("vec-256B",   256,           20000000);
    bench_size("morsel-8K",  8192,           5000000);
    bench_size("morsel-16K", 16384,          5000000);
    bench_size("large-1M",   1u << 20,        200000);
    ray_heap_destroy();

    printf("(b) producer-consumer (morsel-8K):\n");
    /* RAY_MAX_POOLS=512 pools × ~2048 blocks/pool at order-14 (morsel-8K) ≈ 1M
     * blocks total capacity.  Cross-thread free parks blocks in the consumer's
     * foreign list (not returned to the producer during the run), so the
     * producer exhausts its pool budget at ~1M allocs; use 1M iterations. */
    g_pc_iters = 1000000; g_pc_size = 8192;
    double t0 = now_s();
    pthread_t p, c;
    pthread_create(&p, NULL, producer, NULL);
    pthread_create(&c, NULL, consumer, NULL);
    pthread_join(p, NULL); pthread_join(c, NULL);
    double dt = now_s() - t0;
    printf("  throughput   %8.1f Mops/s  peak RSS %ld KB\n",
           (double)g_pc_iters / dt / 1e6, max_rss_kb());
    return 0;
}
