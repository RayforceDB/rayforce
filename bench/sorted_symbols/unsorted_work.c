/* Default: elapsed sort time. COUNT_RESOLUTIONS: GNU ld --wrap work probe. */
#define _POSIX_C_SOURCE 200809L
#include <rayforce.h>
#include "mem/heap.h"
#include "table/sym.h"
#include "table/domain.h"
#include "ops/internal.h"
#include "lang/internal.h"
#include "core/pool.h"
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
static _Atomic unsigned long long resolutions;
#ifdef COUNT_RESOLUTIONS
ray_t* __real_ray_sym_domain_str(ray_sym_domain_t*, int64_t);
ray_t* __wrap_ray_sym_domain_str(ray_sym_domain_t* dom, int64_t id) {
    atomic_fetch_add_explicit(&resolutions, 1, memory_order_relaxed);
    return __real_ray_sym_domain_str(dom, id);
}
#endif
static int probe(const char* label, int n, int late, int desc, int64_t a, int64_t z) {
    ray_t* col = ray_vec_new(RAY_SYM, n); col->len = n;
    int64_t* data = ray_data(col);
    for (int i = 0; i < n; i++) data[i] = desc ? z : a;
    /* Exactly one out-of-order value; early or beyond the leading probe. */
    data[late ? n - 2 : 0] = desc ? a : z;
    int64_t name = ray_sym_intern("key", 3);
    ray_t* keys = ray_alloc(0); keys->type = -RAY_SYM; keys->i64 = name;
    ray_t* table = ray_table_new(1);
    table = ray_table_add_col(table, name, col);
    atomic_store(&resolutions, 0);
    struct timespec start, finish;
    clock_gettime(CLOCK_MONOTONIC, &start);
    ray_t* result = desc ? ray_xdesc_fn(table, keys) : ray_xasc_fn(table, keys);
    clock_gettime(CLOCK_MONOTONIC, &finish);
    double ms = (finish.tv_sec - start.tv_sec) * 1000.0 +
                (finish.tv_nsec - start.tv_nsec) / 1000000.0;
    unsigned long long calls = atomic_load(&resolutions);
    int valid = result && !RAY_IS_ERR(result) && ray_table_nrows(result) == n;
    if (valid) {
        ray_t* output = ray_table_get_col(result, name);
        for (int i = 0; i < n; i++) {
            int64_t expected = i == n - 1 ? (desc ? a : z) : (desc ? z : a);
            if (ray_read_sym(ray_data(output), i, output->type, output->attrs) != expected) valid = 0;
        }
    }
    printf("%s|desc=%d|valid=%d|resolutions=%llu|ms=%.6f\n", label, desc, valid, calls, ms);
    ray_release(result); ray_release(table); ray_release(keys); ray_release(col);
    return valid;
}
int main(void) {
    ray_heap_init(); ray_sym_init();
    for (int i = 0; i < 60000; i++) {
        char name[40]; int n = snprintf(name, sizeof(name), "unused-%d", i);
        ray_sym_intern(name, n);
    }
    int64_t z = ray_sym_intern("used-z", 6), a = ray_sym_intern("used-a", 6);
    int valid = 1;
    ray_pool_init_total(5);
    for (int desc = 0; desc < 2; desc++) {
        valid &= probe("small", 8, 0, desc, a, z);
        valid &= probe("early", 200000, 0, desc, a, z);
        valid &= probe("late", 200000, 1, desc, a, z);
    }
    ray_pool_destroy(); ray_pool_init_total(1);
    valid &= probe("serial", 200000, 0, 0, a, z);
    ray_pool_destroy(); ray_sym_destroy(); ray_heap_destroy();
    return valid ? 0 : 1;
}
