/* Default: sort time. COUNT_RESOLUTIONS: assert GNU ld --wrap work counts. */
#define _POSIX_C_SOURCE 200809L
#include <rayforce.h>
#include "mem/heap.h"
#include "table/sym.h"
#include "table/domain.h"
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
static int probe(int kind, int desc, int64_t a, int64_t z) {
    const int n = 65537;
    ray_t* sym = ray_vec_new(RAY_SYM, n);
    ray_t* time = ray_vec_new(RAY_I64, n);
    ray_t* row = ray_vec_new(RAY_I64, n);
    sym->len = time->len = row->len = n;
    for (int i = 0; i < n; i++) {
        int first = kind == 2 ? i % 2 == 0 : i < n / 2;
        ((int64_t*)ray_data(sym))[i] = kind == 0 ? a : (first ? (desc ? z : a) : (desc ? a : z));
        ((int64_t*)ray_data(time))[i] = desc ? n - 1 - i : i;
        ((int64_t*)ray_data(row))[i] = i;
    }
    int64_t sn = ray_sym_intern("s", 1), tn = ray_sym_intern("t", 1);
    int64_t rn = ray_sym_intern("row", 3);
    int64_t names[] = {tn, sn};
    ray_t* keys = ray_vec_from_raw(RAY_SYM, names + (kind == 2 ? 0 : 1), kind == 2 ? 2 : 1);
    ray_t* table = ray_table_new(3);
    table = ray_table_add_col(table, sn, sym);
    table = ray_table_add_col(table, tn, time);
    table = ray_table_add_col(table, rn, row);
    atomic_store(&resolutions, 0);
    struct timespec start, finish;
    clock_gettime(CLOCK_MONOTONIC, &start);
    ray_t* result = desc ? ray_xdesc_fn(table, keys) : ray_xasc_fn(table, keys);
    clock_gettime(CLOCK_MONOTONIC, &finish);
    unsigned long long calls = atomic_load(&resolutions);
    double ms = (finish.tv_sec - start.tv_sec) * 1000.0 + (finish.tv_nsec - start.tv_nsec) / 1000000.0;
    int valid = result && !RAY_IS_ERR(result) && ray_table_nrows(result) == n;
    if (valid) {
        ray_t* out_row = ray_table_get_col(result, rn);
        ray_t* out_sym = ray_table_get_col(result, sn);
        ray_t* out_time = ray_table_get_col(result, tn);
        for (int i = 0; i < n; i++) {
            valid &= ((int64_t*)ray_data(out_row))[i] == i;
            valid &= ((int64_t*)ray_data(out_time))[i] == ((int64_t*)ray_data(time))[i];
            valid &= ray_read_sym(ray_data(out_sym), i, out_sym->type, out_sym->attrs) == ((int64_t*)ray_data(sym))[i];
        }
    }
    printf("case=%d|desc=%d|valid=%d|resolutions=%llu|ms=%.6f\n", kind, desc, valid, calls, ms);
    ray_release(result); ray_release(table); ray_release(keys);
    ray_release(sym); ray_release(time); ray_release(row);
#ifdef COUNT_RESOLUTIONS
    valid &= calls == (kind == 1 ? 2u : 0u);
#endif
    return valid;
}
int main(void) {
    ray_heap_init(); ray_sym_init();
    for (int i = 0; i < 70000; i++) {
        char name[40]; int n = snprintf(name, sizeof(name), "unused-constant-%d", i);
        ray_sym_intern(name, n);
    }
    int64_t z = ray_sym_intern("used-z", 6), a = ray_sym_intern("used-a", 6);
    ray_pool_init_total(3);
    int valid = 1;
    for (int desc = 0; desc < 2; desc++)
        for (int kind = 0; kind < 3; kind++) valid &= probe(kind, desc, a, z);
    ray_pool_destroy(); ray_sym_destroy(); ray_heap_destroy();
    return valid ? 0 : 1;
}
