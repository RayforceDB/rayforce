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
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>    /* the lazily read page size and read-ahead step */
#if defined(RAY_OS_MACOS)
#include <sys/sysctl.h>   /* sysctlbyname — hw.physicalcpu */
#endif
#if defined(RAY_OS_LINUX)
#include <dirent.h>       /* /sys/block — the read-ahead caps */
#include <sys/resource.h> /* getrusage: ray_io_counters */
#if !defined(RUSAGE_THREAD)
#define RUSAGE_THREAD 1
#endif
#endif
#include "mem/sys.h"

/* --------------------------------------------------------------------------
 * Virtual memory
 * -------------------------------------------------------------------------- */
void* ray_vm_alloc(size_t size) {
    void* p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    ray_sys_track_add((int64_t)size);   /* committed RAM */
    return p;
}

void ray_vm_free(void* ptr, size_t size) {
    if (!ptr) return;
    munmap(ptr, size);
    ray_sys_track_sub((int64_t)size);
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

    ray_sys_track_file_add((int64_t)len);   /* file-backed mapping */
    if (out_size) *out_size = len;
    return p;
}

void ray_vm_unmap_file(void* ptr, size_t size) {
    if (!ptr) return;
    munmap(ptr, size);
    ray_sys_track_file_sub((int64_t)size);
}

/* Read-only, private map of an already-open fd — the zero-copy parse buffer
 * for CSV / script loads.  Counted in the file-mapping total; the caller
 * still owns the fd (close as before).  Free with ray_vm_unmap_file.
 * Returns NULL on failure (not MAP_FAILED). */
void* ray_vm_map_fd_ro(int fd, size_t size) {
    if (size == 0) return NULL;
    void* p = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) return NULL;
    ray_sys_track_file_add((int64_t)size);
    return p;
}

void ray_vm_advise_seq(void* ptr, size_t size) {
    if (ptr) madvise(ptr, size, MADV_SEQUENTIAL);
}

/* Read once, from whichever thread asks first: the threads that race on it
 * all store the same value, relaxed. */
static size_t vm_page_size(void) {
    static _Atomic(size_t) cached = 0;
    size_t pg = atomic_load_explicit(&cached, memory_order_relaxed);
    if (pg == 0) {
        long ps = sysconf(_SC_PAGESIZE);
        pg = (ps > 0) ? (size_t)ps : 4096;
        atomic_store_explicit(&cached, pg, memory_order_relaxed);
    }
    return pg;
}

#if defined(RAY_OS_LINUX)
/* A block device's queue attribute in KiB, -1 when it cannot be read. */
static long vm_queue_kb(const char* dev, const char* attr) {
    char path[320], buf[32];
    snprintf(path, sizeof(path), "/sys/block/%s/queue/%s", dev, attr);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    return strtol(buf, NULL, 10);
}
#endif

/* Bytes one WILLNEED is asked for at a time.  Linux reads at most
 * max(read_ahead_kb, max_sectors_kb) of the file's device per call, and a
 * longer step would leave the rest of it unread: so the smallest such cap
 * of the block devices, within [32 KiB, 256 KiB]; 128 KiB, the kernel's
 * default read-ahead, when none can be read or elsewhere. */
static size_t vm_willneed_step(void) {
#if defined(RAY_OS_LINUX)
    static _Atomic(size_t) cached = 0;
    size_t step = atomic_load_explicit(&cached, memory_order_relaxed);
    if (step) return step;
    DIR* d = opendir("/sys/block");
    if (d) {
        struct dirent* e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            long ra = vm_queue_kb(e->d_name, "read_ahead_kb");
            long mx = vm_queue_kb(e->d_name, "max_sectors_kb");
            long cap = ra > mx ? ra : mx;
            if (cap > 0 && (!step || ((size_t)cap << 10) < step)) step = (size_t)cap << 10;
        }
        closedir(d);
    }
    if (!step) step = (size_t)128 << 10;
    if (step < ((size_t)32 << 10)) step = (size_t)32 << 10;
    if (step > ((size_t)256 << 10)) step = (size_t)256 << 10;
    step &= ~(vm_page_size() - 1);
    if (step < vm_page_size()) step = vm_page_size();   /* pages above 32 KiB */
    atomic_store_explicit(&cached, step, memory_order_relaxed);
    return step;
#else
    return (size_t)128 << 10;
#endif
}

void ray_vm_advise_willneed(void* ptr, size_t size) {
    if (!ptr || !size) return;
    /* madvise wants a page-aligned start.  One WILLNEED reads at most what
     * the device allows per call (vm_willneed_step), so a large range is
     * requested step by step: each call only queues its reads, so the steps
     * are all in flight together. */
    const size_t chunk = vm_willneed_step();
    uintptr_t a = (uintptr_t)ptr & ~(uintptr_t)(vm_page_size() - 1);
    uintptr_t end = (uintptr_t)ptr + size;
    for (; a < end; a += chunk)
        madvise((void*)a, end - a < chunk ? end - a : chunk, MADV_WILLNEED);
}

void ray_file_writeback_start(int fd, int64_t off, int64_t len) {
#if defined(__linux__)
    if (fd >= 0 && off >= 0 && len > 0)
        (void)sync_file_range(fd, (off64_t)off, (off64_t)len, SYNC_FILE_RANGE_WRITE);
#else
    (void)fd; (void)off; (void)len;
#endif
}

void ray_sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

bool ray_vm_resident(const void* ptr, size_t size) {
    if (!ptr || !size) return true;
    size_t pg = vm_page_size();
    uintptr_t a = (uintptr_t)ptr & ~(uintptr_t)(pg - 1);
    size_t pages = ((uintptr_t)ptr + size - a + pg - 1) / pg;
    size_t samples = pages < 16 ? pages : 16;
    for (size_t i = 0; i < samples; i++) {
        size_t at = samples > 1 ? (pages - 1) * i / (samples - 1) : 0;
#if defined(RAY_OS_MACOS)
        char v = 0;
#else
        unsigned char v = 0;
#endif
        if (mincore((void*)(a + at * pg), pg, &v) != 0 || !(v & 1)) return false;
    }
    return true;
}

void ray_vm_release(void* ptr, size_t size) {
    if (!ptr) return;
#if defined(RAY_OS_MACOS)
    madvise(ptr, size, MADV_FREE);
#else
    madvise(ptr, size, MADV_DONTNEED);
#endif
}

void ray_vm_release_block(void* blk, size_t bsize, bool hugepage) {
    if (!hugepage) {
        /* Release only whole pages strictly INSIDE the block, keeping its
         * first page resident: the buddy free-list links (fl_prev/fl_next
         * at offset 0-15) live there and the block stays linked on the
         * freelist after this call.
         *
         * The page size is not always 4096 — macOS arm64 and some aarch64
         * Linux kernels use 16K (or 64K) pages.  Worse, Darwin's madvise
         * rounds an unaligned range OUTWARD (trunc_page(addr),
         * round_page(addr+len)), so the old `blk + 4096` start was rounded
         * back to `blk` on 16K pages and MADV_FREE hit the header page of
         * a linked freelist block; once the kernel reclaimed it, fl_next
         * read back as zero and the next GC freelist walk crashed on a
         * NULL link (issue #240).  Rounding inward to page-aligned bounds
         * makes the kernel's own rounding a no-op on every platform. */
        size_t pg = vm_page_size();
        uintptr_t s = ((uintptr_t)blk + 32 + (pg - 1)) & ~(uintptr_t)(pg - 1);
        uintptr_t e = ((uintptr_t)blk + bsize) & ~(uintptr_t)(pg - 1);
        if (e > s) ray_vm_release((void*)s, e - s);
        return;
    }
    /* THP pool: release only the whole 2MB-aligned interior so a partial
     * MADV_DONTNEED does not shatter the transparent huge page.
     *
     * Crucially, never discard the block's FIRST page: the free-list links
     * (fl_prev/fl_next at offset 0-15) live there and the block stays linked
     * after release, so dropping that page zeroes the links and corrupts the
     * free-list for every later traversal (alloc, GC).  When blk is itself
     * 2MB-aligned the rounded-up start equals blk and would MADV_DONTNEED the
     * header; advance past the first 2MB region so the header survives — we
     * still release only whole 2MB regions, so no THP shatter. */
    uintptr_t s = ((uintptr_t)blk + (2u << 20) - 1) & ~((uintptr_t)(2u << 20) - 1);
    if (s == (uintptr_t)blk) s += (2u << 20);
    uintptr_t e = ((uintptr_t)blk + bsize) & ~((uintptr_t)(2u << 20) - 1);
    if (e > s) ray_vm_release((void*)s, e - s);
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

    /* Count only the KEPT region, so the two trim munmaps above never touch
     * the counter — ray_vm_free(ptr, size) later subtracts the same `size`. */
    ray_sys_track_add((int64_t)size);
    return (void*)aligned;
}

bool ray_vm_hugepage(void* ptr, size_t size) {
#if defined(MADV_HUGEPAGE)
    if (!ptr) return false;
    return madvise(ptr, size, MADV_HUGEPAGE) == 0;
#else
    (void)ptr; (void)size; return false;
#endif
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
    /* Keep SIGURG (the IPC out-of-band cancel signal) off spawned threads so it
     * is only ever delivered to the main thread.  A worker's stack can be
     * nearly full mid-kernel (large per-morsel VLAs in the elementwise ops), and
     * running the signal handler on top of it would overflow the guard page.
     * Workers observe cancellation via the pool's cancel flag, not the signal. */
    sigset_t urg;
    sigemptyset(&urg);
    sigaddset(&urg, SIGURG);
    pthread_sigmask(SIG_BLOCK, &urg, NULL);
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

/* Physical cores (SMT siblings collapsed).  The worker pool's kernels are
 * memory-bound; two hyperthreads sharing one core's load/store machinery
 * only add contention (measured: the full ClickBench suite runs ~11%
 * SLOWER with 32 SMT threads than with the 16 physical cores on a 5950X).
 * Counts unique (package, core) pairs from sysfs; any read failure falls
 * back to the logical count so exotic systems keep the old behavior. */
uint32_t ray_physical_core_count(void) {
#if defined(RAY_OS_MACOS)
    int phys = 0;
    size_t len = sizeof(phys);
    if (sysctlbyname("hw.physicalcpu", &phys, &len, NULL, 0) == 0 && phys > 0)
        return (uint32_t)phys;
    return ray_thread_count();
#else
    uint32_t logical = ray_thread_count();
    /* (package_id << 16) | core_id per cpu; count distinct values. */
    enum { MAX_IDS = 4096 };
    uint32_t seen[MAX_IDS];
    uint32_t n_seen = 0;
    for (uint32_t cpu = 0; cpu < logical && cpu < MAX_IDS; cpu++) {
        char path[128];
        long core = -1, pkg = 0;
        FILE* f;
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%u/topology/core_id", cpu);
        f = fopen(path, "r");
        if (!f) return logical;              /* no topology → fall back */
        if (fscanf(f, "%ld", &core) != 1) { fclose(f); return logical; }
        fclose(f);
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%u/topology/physical_package_id",
                 cpu);
        f = fopen(path, "r");
        if (f) { if (fscanf(f, "%ld", &pkg) != 1) pkg = 0; fclose(f); }
        uint32_t id = ((uint32_t)pkg << 16) | ((uint32_t)core & 0xFFFF);
        uint32_t j = 0;
        while (j < n_seen && seen[j] != id) j++;
        if (j == n_seen) seen[n_seen++] = id;
    }
    return n_seen > 0 ? n_seen : logical;
#endif
}

/* Total last-level cache capacity across every LLC instance, in bytes.
 * Replicated per-task state (dense group slabs) stops scaling the moment
 * its total footprint leaves the LLC: a 100k-group sum measured 6 ms with
 * 8 slabs (26 MB, inside a 33 MB L3) and 21 ms with 28 slabs (92 MB) on the
 * same 28-thread pool.  Callers bound such replication by this figure.
 *
 * Linux reads sysfs: the highest-level unified cache of cpu0 gives the
 * per-instance size, and its shared_cpu_list gives the instance width, so
 * multi-die parts (one LLC per die) report the sum of every die's LLC.
 * Falls back to sysconf's L3 (then L2) size; 0 when nothing is known. */
#if defined(RAY_OS_LINUX)
static uint32_t cache_cpu_list_count(const char* list) {
    /* "0-27" / "0-3,8-11" / "5" → number of CPUs named. */
    uint32_t n = 0;
    const char* p = list;
    while (*p) {
        char* end;
        long lo = strtol(p, &end, 10);
        if (end == p) break;
        long hi = lo;
        p = end;
        if (*p == '-') { hi = strtol(p + 1, &end, 10); if (end == p + 1) break; p = end; }
        if (hi >= lo) n += (uint32_t)(hi - lo + 1);
        while (*p == ',' || *p == ' ' || *p == '\n') p++;
    }
    return n;
}
static uint64_t cache_sysfs_llc_bytes(void) {
    uint64_t best = 0; long best_level = 0;
    uint32_t logical = ray_thread_count();
    for (int index = 0; index < 8; index++) {
        char path[128], buf[256];
        FILE* f;
        long level = 0;
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%d/level", index);
        f = fopen(path, "r");
        if (!f) break;
        if (fscanf(f, "%ld", &level) != 1) level = 0;
        fclose(f);
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%d/type", index);
        f = fopen(path, "r");
        if (!f) continue;
        buf[0] = 0;
        if (!fgets(buf, sizeof(buf), f)) buf[0] = 0;
        fclose(f);
        if (strncmp(buf, "Unified", 7) != 0 || level <= best_level) continue;
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%d/size", index);
        f = fopen(path, "r");
        if (!f) continue;
        buf[0] = 0;
        if (!fgets(buf, sizeof(buf), f)) buf[0] = 0;
        fclose(f);
        char* unit = NULL;
        unsigned long long size = strtoull(buf, &unit, 10);
        if (unit && (*unit == 'K' || *unit == 'k')) size <<= 10;
        else if (unit && (*unit == 'M' || *unit == 'm')) size <<= 20;
        if (size == 0) continue;
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%d/shared_cpu_list", index);
        f = fopen(path, "r");
        uint32_t width = 0;
        if (f) {
            buf[0] = 0;
            if (fgets(buf, sizeof(buf), f)) width = cache_cpu_list_count(buf);
            fclose(f);
        }
        uint32_t instances = width > 0 && logical > width ? (logical + width - 1) / width : 1;
        best = (uint64_t)size * instances;
        best_level = level;
    }
    return best;
}
#endif

/* --------------------------------------------------------------------------
 * Container memory limit (Linux cgroups)
 *
 * A process limited below host RAM (`docker run --memory`, `systemd-run -p
 * MemoryMax=`) is OOM-killed by its cgroup long before sysconf's physical
 * RAM figure is reached, so the heap's spill threshold must see that limit
 * (issue #688).  cgroup v2 publishes it as memory.max ("max" = unlimited)
 * under the cgroup named by /proc/self/cgroup; v1 as memory.limit_in_bytes
 * under the memory controller mount, where "unlimited" is LLONG_MAX rounded
 * down to a page.  A limit may sit on an ancestor (a slice above the scope),
 * so every directory up to the mount root is read and the smallest wins.
 * Inside a cgroup namespace the reported path is "/" and the root files are
 * the container's own, which the same walk covers.
 * -------------------------------------------------------------------------- */
#if defined(RAY_OS_LINUX)
/* One cgroup limit file → bytes, or 0 when missing, unreadable or unlimited. */
static int64_t cgroup_read_limit(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char buf[64];
    buf[0] = 0;
    if (!fgets(buf, sizeof(buf), f)) buf[0] = 0;
    fclose(f);
    if (strncmp(buf, "max", 3) == 0) return 0;
    char* end = NULL;
    unsigned long long v = strtoull(buf, &end, 10);
    if (end == buf || v == 0 || v >= (1ULL << 62)) return 0;   /* v1 "unlimited" ~ 2^63 */
    return (int64_t)v;
}

/* Smallest limit found in `root`/`cgpath`/`file` and every ancestor. */
static int64_t cgroup_walk_limit(const char* root, const char* cgpath, const char* file) {
    char dir[512];
    if (snprintf(dir, sizeof(dir), "%s%s", root, cgpath) >= (int)sizeof(dir)) return 0;
    size_t rootlen = strlen(root);
    int64_t limit = 0;
    for (;;) {
        char path[640];
        snprintf(path, sizeof(path), "%s/%s", dir, file);
        int64_t v = cgroup_read_limit(path);
        if (v > 0 && (limit == 0 || v < limit)) limit = v;
        if (strlen(dir) <= rootlen) break;              /* mount root checked */
        char* slash = strrchr(dir, '/');
        if (!slash || (size_t)(slash - dir) < rootlen) break;
        *slash = 0;                                      /* parent directory */
    }
    return limit;
}

/* True when the comma-separated controller list names "memory". */
static bool cgroup_has_memory(const char* ctrls) {
    while (*ctrls) {
        size_t n = strcspn(ctrls, ",");
        if (n == 6 && strncmp(ctrls, "memory", 6) == 0) return true;
        ctrls += n;
        if (*ctrls == ',') ctrls++;
    }
    return false;
}

int64_t ray_os_cgroup_mem_limit(void) {
    FILE* f = fopen("/proc/self/cgroup", "r");
    if (!f) return 0;
    int64_t limit = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        /* "<hierarchy>:<controllers>:<path>" — v2 is "0::/path". */
        char* c1 = strchr(line, ':');
        char* c2 = c1 ? strchr(c1 + 1, ':') : NULL;
        if (!c2) continue;
        *c2 = 0;
        const char* ctrls = c1 + 1;
        char* cgpath = c2 + 1;
        cgpath[strcspn(cgpath, "\n")] = 0;
        if (strcmp(cgpath, "/") == 0) cgpath[0] = 0;
        int64_t v;
        if (*ctrls == 0)
            v = cgroup_walk_limit("/sys/fs/cgroup", cgpath, "memory.max");
        else if (cgroup_has_memory(ctrls))
            v = cgroup_walk_limit("/sys/fs/cgroup/memory", cgpath, "memory.limit_in_bytes");
        else
            continue;
        if (v > 0 && (limit == 0 || v < limit)) limit = v;
    }
    fclose(f);
    return limit;
}
#else
int64_t ray_os_cgroup_mem_limit(void) { return 0; }
#endif

static uint64_t cache_llc_probe(void) {
    static uint64_t cached = UINT64_MAX;
    if (cached != UINT64_MAX) return cached;
    uint64_t bytes = 0;
#if defined(RAY_OS_MACOS)
    uint64_t v = 0; size_t len = sizeof(v);
    if (sysctlbyname("hw.l3cachesize", &v, &len, NULL, 0) == 0 && v > 0) bytes = v;
    else {
        /* No L3 (recent ARM desktop parts): the per-cluster L2 is the last level.
         * Sum every cluster's L2 from the perflevel topology. */
        for (int level = 0; level < 2 && bytes < UINT64_MAX / 2; level++) {
            char name[64];
            uint64_t l2 = 0; int cpus = 0, per_l2 = 0;
            size_t l2_len = sizeof(l2), cpus_len = sizeof(cpus), per_len = sizeof(per_l2);
            snprintf(name, sizeof(name), "hw.perflevel%d.l2cachesize", level);
            if (sysctlbyname(name, &l2, &l2_len, NULL, 0) != 0 || l2 == 0) break;
            snprintf(name, sizeof(name), "hw.perflevel%d.physicalcpu", level);
            if (sysctlbyname(name, &cpus, &cpus_len, NULL, 0) != 0 || cpus <= 0) cpus = 1;
            snprintf(name, sizeof(name), "hw.perflevel%d.cpusperl2", level);
            if (sysctlbyname(name, &per_l2, &per_len, NULL, 0) != 0 || per_l2 <= 0) per_l2 = cpus;
            bytes += l2 * (uint64_t)((cpus + per_l2 - 1) / per_l2);
        }
        if (bytes == 0) {
            len = sizeof(v);
            if (sysctlbyname("hw.l2cachesize", &v, &len, NULL, 0) == 0) bytes = v;
        }
    }
#elif defined(RAY_OS_LINUX)
    bytes = cache_sysfs_llc_bytes();
    if (bytes == 0) {
#if defined(_SC_LEVEL3_CACHE_SIZE)
        long l3 = sysconf(_SC_LEVEL3_CACHE_SIZE);
        if (l3 > 0) bytes = (uint64_t)l3;
#endif
    }
    if (bytes == 0) {
#if defined(_SC_LEVEL2_CACHE_SIZE)
        long l2 = sysconf(_SC_LEVEL2_CACHE_SIZE);
        if (l2 > 0) bytes = (uint64_t)l2 * ray_physical_core_count();
#endif
    }
#endif
    cached = bytes;
    return bytes;
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
#include <io.h>      /* _get_osfhandle */
#include "mem/sys.h"

/* --------------------------------------------------------------------------
 * Virtual memory
 * -------------------------------------------------------------------------- */
void* ray_vm_alloc(size_t size) {
    void* p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p) ray_sys_track_add((int64_t)size);
    return p;
}

void ray_vm_free(void* ptr, size_t size) {
    if (!ptr) return;
    VirtualFree(ptr, 0, MEM_RELEASE);
    ray_sys_track_sub((int64_t)size);
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

    ray_sys_track_file_add((int64_t)file_size.QuadPart);
    if (out_size) *out_size = (size_t)file_size.QuadPart;
    return p;
}

void ray_vm_unmap_file(void* ptr, size_t size) {
    if (!ptr) return;
    /* munmap releases exactly [ptr, ptr+size) and is a no-op (EINVAL) for an
     * unaligned ptr — the heap relies on that when it frees a block living
     * inside a column's mapping (a passenger index).  UnmapViewOfFile instead
     * drops the WHOLE view containing ptr, which would pull the column out
     * from under its live references.  So only unmap at the view's own base;
     * an interior block goes away with the view.  The byte accounting still
     * follows the call, exactly as on POSIX. */
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) && mbi.AllocationBase == ptr)
        UnmapViewOfFile(ptr);
    ray_sys_track_file_sub((int64_t)size);
}

/* Read-only view of an open CRT descriptor (the CSV reader maps the file this
 * way, then closes the fd).  The view keeps the file alive on its own, so both
 * the mapping handle and the caller's fd may be closed afterwards. */
void* ray_vm_map_fd_ro(int fd, size_t size) {
    if (size == 0) return NULL;
    HANDLE hFile = (HANDLE)_get_osfhandle(fd);
    if (hFile == INVALID_HANDLE_VALUE) return NULL;
    HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMap) return NULL;
    void* p = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, size);
    CloseHandle(hMap);
    if (!p) return NULL;
    ray_sys_track_file_add((int64_t)size);
    return p;
}

void ray_vm_advise_seq(void* ptr, size_t size) {
    /* PrefetchVirtualMemory is Win8.1+. Best-effort; ignore failure. */
    WIN32_MEMORY_RANGE_ENTRY entry;
    entry.VirtualAddress = ptr;
    entry.NumberOfBytes  = size;
    PrefetchVirtualMemory(GetCurrentProcess(), 1, &entry, 0);
}

void ray_vm_advise_willneed(void* ptr, size_t size) {
    if (!ptr || !size) return;
    WIN32_MEMORY_RANGE_ENTRY entry;
    entry.VirtualAddress = ptr;
    entry.NumberOfBytes  = size;
    PrefetchVirtualMemory(GetCurrentProcess(), 1, &entry, 0);
}

void ray_file_writeback_start(int fd, int64_t off, int64_t len) { (void)fd; (void)off; (void)len; }

void ray_sleep_ms(int ms) { Sleep((DWORD)ms); }

/* No cheap residency query here: report "not resident" so a read-ahead is
 * still requested (PrefetchVirtualMemory skips resident pages itself). */
bool ray_vm_resident(const void* ptr, size_t size) { (void)ptr; (void)size; return false; }

void ray_vm_release(void* ptr, size_t size) {
    if (!ptr) return;
    /* DiscardVirtualMemory (Win8.1+) or fallback to decommit+recommit */
    DiscardVirtualMemory(ptr, size);
}

void ray_vm_release_block(void* blk, size_t bsize, bool hugepage) {
    (void)hugepage;
    if (bsize > 4096) ray_vm_release((char*)blk + 4096, bsize - 4096);
}

void* ray_vm_alloc_aligned(size_t size, size_t alignment) {
    /* VirtualFree(MEM_RELEASE) only accepts the exact base VirtualAlloc
     * returned, and a reservation cannot be trimmed.  So find an aligned
     * hole by reserving size+alignment, release it, and allocate exactly
     * `size` at the aligned address inside it.  Another thread may take the
     * hole in between; retry a few times.  The result is its own allocation
     * base, so ray_vm_free(ptr, size) releases it like any other block. */
    for (int attempt = 0; attempt < 16; attempt++) {
        void* probe = VirtualAlloc(NULL, size + alignment, MEM_RESERVE, PAGE_NOACCESS);
        if (!probe) return NULL;
        uintptr_t aligned = ((uintptr_t)probe + alignment - 1) & ~(alignment - 1);
        VirtualFree(probe, 0, MEM_RELEASE);
        void* p = VirtualAlloc((void*)aligned, size,
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p) {
            ray_sys_track_add((int64_t)size);
            return p;
        }
    }
    return NULL;
}

bool ray_vm_hugepage(void* ptr, size_t size) { (void)ptr; (void)size; return false; }

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

/* Windows: no cheap topology read here — fall back to the logical count
 * (the SMT-aware default sizing is a POSIX-side optimization). */
uint32_t ray_physical_core_count(void) {
    return ray_thread_count();
}

int64_t ray_os_cgroup_mem_limit(void) { return 0; }

/* Sum of every level-3 cache instance reported by the processor topology
 * (each SYSTEM_LOGICAL_PROCESSOR_INFORMATION cache record is one instance).
 * 0 when the query fails. */
static uint64_t cache_llc_probe(void) {
    static uint64_t cached = UINT64_MAX;
    if (cached != UINT64_MAX) return cached;
    uint64_t bytes = 0;
    DWORD len = 0;
    GetLogicalProcessorInformation(NULL, &len);
    if (len > 0) {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION* info = ray_sys_alloc(len);
        if (info) {
            if (GetLogicalProcessorInformation(info, &len)) {
                DWORD n = len / sizeof(*info);
                BYTE best_level = 0;
                for (DWORD i = 0; i < n; i++) {
                    if (info[i].Relationship != RelationCache) continue;
                    BYTE level = info[i].Cache.Level;
                    if (info[i].Cache.Type != CacheUnified && info[i].Cache.Type != CacheData) continue;
                    if (level > best_level) { best_level = level; bytes = 0; }
                    if (level == best_level) bytes += info[i].Cache.Size;
                }
            }
            ray_sys_free(info);
        }
    }
    cached = bytes;
    return bytes;
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
        /* zero-filled, as a fresh mapping is (ray_sys_alloc's contract) */
        if (p) memset(p, 0, aligned);
    }
    if (p) ray_sys_track_add((int64_t)size);
    return p;
}

void ray_vm_free(void* ptr, size_t size) {
    if (!ptr) return;
    if (munmap(ptr, size) != 0) ray_free_raw(ptr);
    ray_sys_track_sub((int64_t)size);
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
    ray_sys_track_file_add((int64_t)len);
    if (out_size) *out_size = len;
    return p;
}

void ray_vm_unmap_file(void* ptr, size_t size) {
    if (!ptr) return;
    munmap(ptr, size);
    ray_sys_track_file_sub((int64_t)size);
}

void* ray_vm_map_fd_ro(int fd, size_t size) {
    if (size == 0) return NULL;
    void* p = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) return NULL;
    ray_sys_track_file_add((int64_t)size);
    return p;
}

/* madvise hints are advisory and have no analog on WASM — no-ops. */
void ray_vm_advise_seq(void* ptr, size_t size)      { (void)ptr; (void)size; }
void ray_vm_advise_willneed(void* ptr, size_t size) { (void)ptr; (void)size; }
void ray_file_writeback_start(int fd, int64_t off, int64_t len) { (void)fd; (void)off; (void)len; }
void ray_sleep_ms(int ms)                          { (void)ms; }
bool ray_vm_resident(const void* ptr, size_t size)  { (void)ptr; (void)size; return true; }
void ray_vm_release(void* ptr, size_t size)         { (void)ptr; (void)size; }

void ray_vm_release_block(void* blk, size_t bsize, bool hugepage) {
    (void)hugepage;
    if (bsize > 4096) ray_vm_release((char*)blk + 4096, bsize - 4096);
}

void* ray_vm_alloc_aligned(size_t size, size_t alignment) {
    /* aligned_alloc requires size to be a multiple of alignment per C17. */
    size_t aligned_size = (size + alignment - 1) & ~(alignment - 1);
    void* p = aligned_alloc(alignment, aligned_size);
    /* Count the requested `size` to balance ray_vm_free(ptr, size). */
    if (p) ray_sys_track_add((int64_t)size);
    return p;
}

bool ray_vm_hugepage(void* ptr, size_t size) { (void)ptr; (void)size; return false; }

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
int64_t ray_os_cgroup_mem_limit(void) { return 0; }
static uint64_t cache_llc_probe(void) { return 0; }

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

#ifdef DEBUG
/* Test pin for the probed LLC size: routing that bounds replicated state by
 * the cache (group dense slabs) otherwise picks a different strategy on
 * every CI runner.  0 restores the platform probe. */
static uint64_t g_llc_for_test = 0;

void ray_cache_llc_set_for_test(uint64_t bytes) {
    g_llc_for_test = bytes;
}
#endif

uint64_t ray_cache_llc_bytes(void) {
#ifdef DEBUG
    if (g_llc_for_test) return g_llc_for_test;
#endif
    return cache_llc_probe();
}

/* ---- I/O counters for the traces ----------------------------------------- */
void ray_io_counters(bool thread, ray_io_counters_t* out) {
    *out = (ray_io_counters_t){0};
#if defined(RAY_OS_LINUX)
    struct rusage ru;
    if (getrusage(thread ? RUSAGE_THREAD : RUSAGE_SELF, &ru) == 0) out->majflt = ru.ru_majflt;
    FILE* f = fopen(thread ? "/proc/thread-self/io" : "/proc/self/io", "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            long long v;
            if (sscanf(line, "read_bytes: %lld", &v) == 1) out->rd = v;
            else if (sscanf(line, "write_bytes: %lld", &v) == 1) out->wr = v;
            else if (sscanf(line, "cancelled_write_bytes: %lld", &v) == 1) out->cwr = v;
            else if (sscanf(line, "syscr: %lld", &v) == 1) out->syscr = v;
            else if (sscanf(line, "syscw: %lld", &v) == 1) out->syscw = v;
        }
        fclose(f);
    }
#else
    (void)thread;
#endif
}
