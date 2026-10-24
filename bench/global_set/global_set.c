/* #951: isolate binding lookup from allocation and Rayfall dispatch.
 * Build against a release librayforce.a; see README.md. */
#include <rayforce.h>
#include "lang/env.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double ns_per_op(clock_t begin, uint64_t n) {
    return 1e9 * (double)(clock() - begin) / (double)CLOCKS_PER_SEC / (double)n;
}

int main(int argc, char** argv) {
    uint64_t n = argc > 1 ? strtoull(argv[1], NULL, 10) : UINT64_C(10000000);
    if (!n) return 2;
    ray_runtime_t* runtime = ray_runtime_create(0, NULL);
    if (!runtime) return 2;
    enum { NAMES = 512 };
    int64_t ids[NAMES];
    ray_t* value = ray_i64(7);
    for (int i = 0; i < NAMES; i++) {
        char name[48];
        int len = snprintf(name, sizeof(name), "global_set_bench_%d", i);
        ids[i] = ray_sym_intern(name, (size_t)len);
        if (ray_env_set(ids[i], value) != RAY_OK) return 2;
    }
    printf("bindings=%d iterations=%llu\n", ray_env_global_count(), (unsigned long long)n);
    clock_t begin = clock();
    for (uint64_t i = 0; i < n; i++)
        if (ray_env_set(ids[NAMES - 1], value) != RAY_OK) return 2;
    printf("same-symbol-ns %.3f\n", ns_per_op(begin, n));
    begin = clock();
    for (uint64_t i = 0; i < n; i++)
        if (ray_env_set(ids[i % NAMES], value) != RAY_OK) return 2;
    printf("rotating-symbols-ns %.3f\n", ns_per_op(begin, n));
    volatile int64_t sum = 0;
    begin = clock();
    for (uint64_t i = 0; i < n; i++) sum += ray_env_get_global(ids[i % NAMES])->i64;
    printf("read-control-ns %.3f\n", ns_per_op(begin, n));
    int64_t dotted = ray_sym_intern("global_set_state.value", 22);
    if (ray_env_set(dotted, value) != RAY_OK) return 2;
    uint64_t ndot = n / 10 + 1;
    begin = clock();
    for (uint64_t i = 0; i < ndot; i++)
        if (ray_env_set(dotted, value) != RAY_OK) return 2;
    printf("dotted-control-ns %.3f\n", ns_per_op(begin, ndot));
    printf("checksum=%lld\n", (long long)sum);
    ray_release(value);
    ray_runtime_destroy(runtime);
    return sum == (int64_t)(7 * n) ? 0 : 2;
}
