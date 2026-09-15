/* Direct indexed-consumer benchmark. The query language separately caps K at
 * 1024; the internal kernel accepts larger K. Verify every result using an
 * independent histogram, outside the timed interval. */
#define _POSIX_C_SOURCE 200809L
#include <rayforce.h>
#include "ops/internal.h"
#include "core/pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/resource.h>

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}
static bool verify(ray_t* out, const int64_t* histogram, int64_t expected, bool desc) {
    if (!out || RAY_IS_ERR(out) || out->type != RAY_LIST || out->len != 1) return false;
    ray_t* cell = ray_list_get(out, 0);
    if (!cell || cell->type != RAY_I64 || cell->len != expected) return false;
    const int64_t* values = ray_data(cell);
    int64_t at = 0;
    for (int code = 0; code < 4096 && at < expected; code++) {
        int value = desc ? 4095 - code : code;
        for (int64_t i = 0; i < histogram[value] && at < expected; i++)
            if (values[at++] != value) return false;
    }
    return at == expected;
}
int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "usage: topk-consumer ROWS K\n"); return 2; }
    char *end_n, *end_k;
    int64_t n = strtoll(argv[1], &end_n, 10), k = strtoll(argv[2], &end_k, 10);
    if (*end_n || *end_k || n < 1 || n > INT32_MAX || k < 1 || k > INT32_MAX) return 2;
    ray_runtime_t* runtime = ray_runtime_create(0, NULL);
    if (!runtime) return 1;
    const char* setting = getenv("RAYFORCE_CORES");
    ray_pool_destroy();
    if (ray_pool_init_total(setting ? (uint32_t)strtoul(setting, NULL, 10) : 0) != RAY_OK) return 1;
    ray_t* src = ray_vec_new(RAY_I64, n);
    int64_t* rows = ray_alloc_raw((size_t)n * sizeof(int64_t));
    if (!src || RAY_IS_ERR(src) || !rows) return 1;
    src->len = n; src->attrs |= RAY_ATTR_HAS_NULLS;
    int64_t histogram[4096] = {0}, valid = 0, offset = 0;
    for (int64_t i = 0; i < n; i++) {
        int64_t value = i * 37 % 4096;
        bool missing = i % 97 == 0;
        ((int64_t*)ray_data(src))[i] = missing ? NULL_I64 : value;
        if (!missing) { histogram[value]++; valid++; }
        rows[i] = n - 1 - i;
    }
    double ms[6];
    for (unsigned run = 0; run < 6; run++) {
        double begin = now_ms();
        ray_t* top = ray_topk_per_group_buf(src, k, 1, rows, &offset, &n, 1);
        ray_t* bot = ray_topk_per_group_buf(src, k, 0, rows, &offset, &n, 1);
        ms[run] = now_ms() - begin;
        int64_t expected = k < valid ? k : valid;
        if (!verify(top, histogram, expected, true) || !verify(bot, histogram, expected, false)) {
            fprintf(stderr, "top/bottom K differs from histogram oracle\n"); return 1;
        }
        ray_release(top); ray_release(bot);
    }
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    printf("{\"actual_workers\":%u,\"cold_ms\":%.6f,\"warm_ms\":[%.6f,%.6f,%.6f,%.6f,%.6f],\"peak_rss_kib\":%ld}\n",
        ray_pool_total_workers(ray_pool_get()), ms[0], ms[1], ms[2], ms[3], ms[4], ms[5], usage.ru_maxrss);
    ray_free_raw(rows); ray_release(src); ray_runtime_destroy(runtime);
    return 0;
}
