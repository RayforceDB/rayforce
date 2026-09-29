/* Focused concat benchmark: build with release library, run before/after. */
#define _POSIX_C_SOURCE 200809L
#include <rayforce.h>
#include "mem/heap.h"
#include "vec/vec.h"
#include <stdio.h>
#include <time.h>

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(void) {
    ray_heap_init();
    const int64_t n = 1000000;
    const int reps = 40;
    for (int kind = 0; kind < 2; kind++) {
        for (int mode = 0; mode < 3; mode++) {
            ray_t* a = ray_vec_new(kind ? RAY_SYM : RAY_STR, n);
            if (!a || RAY_IS_ERR(a)) return 1;
            if (kind) {
                a->len = n;
                for (int64_t i = 0; i < n; i++) ((int64_t*)ray_data(a))[i] = 1;
            } else {
                for (int64_t i = 0; i < n; i++) a = ray_str_vec_append(a, "abc", 3);
            }
            if (mode == 1) ray_vec_set_null(a, 0, true);
            if (mode == 2) ray_vec_set_null(a, n - 1, true);
            double start = now();
            for (int r = 0; r < reps; r++) {
                ray_t* out = ray_vec_concat(a, a);
                if (!out || RAY_IS_ERR(out) || out->len != 2 * n) return 2;
                ray_release(out);
            }
            printf("%s %s rows=%lld reps=%d ms_per_concat=%.6f\n",
                   kind ? "SYM64" : "STR-inline", mode == 0 ? "no-null" : mode == 1 ? "first-null" : "last-null",
                   (long long)n, reps, (now() - start) * 1000 / reps);
            ray_release(a);
        }
    }
    ray_heap_destroy();
    return 0;
}
