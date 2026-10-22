/* Whole-xasc typed-key control; construction/checks are outside timing. */
#define _POSIX_C_SOURCE 200809L
#include <rayforce.h>
#include "mem/heap.h"
#include "table/sym.h"
#include "lang/internal.h"
#include "core/pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int main(int argc, char** argv) {
    int participants = argc > 1 ? atoi(argv[1]) : 4;
    int stamp = argc > 2 ? atoi(argv[2]) : 0;
    if (participants < 1 || participants > 64 || (stamp != 0 && stamp != 1)) return 2;
    ray_heap_init(); ray_sym_init();
    int64_t ids[100];
    for (int i = 99; i >= 0; i--) {
        char name[32]; int n = snprintf(name, sizeof(name), "typed-%03d", i);
        ids[i] = ray_sym_intern(name, (size_t)n);
    }
    const int64_t n = 2000000, run_rows = 20000;
    ray_t* sym = ray_vec_new(RAY_SYM, n);
    ray_t* time = ray_vec_new(stamp ? RAY_TIMESTAMP : RAY_I64, n);
    sym->len = time->len = n;
    for (int64_t i = 0; i < n; i++) {
        ((int64_t*)ray_data(sym))[i] = ids[i / run_rows];
        ((int64_t*)ray_data(time))[i] = (i % run_rows - 10000) * 1000000;
    }
    int64_t sn = ray_sym_intern("s", 1), tn = ray_sym_intern("t", 1);
    int64_t names[] = {sn, tn};
    ray_t* keys = ray_vec_from_raw(RAY_SYM, names, 2);
    ray_t* table = ray_table_new(2);
    table = ray_table_add_col(table, sn, sym);
    table = ray_table_add_col(table, tn, time);
    if (ray_pool_init_total((uint32_t)participants) != RAY_OK) return 2;
    struct timespec start, finish;
    clock_gettime(CLOCK_MONOTONIC, &start);
    ray_t* result = ray_xasc_fn(table, keys);
    clock_gettime(CLOCK_MONOTONIC, &finish);
    double ms = (finish.tv_sec - start.tv_sec) * 1000.0 + (finish.tv_nsec - start.tv_nsec) / 1000000.0;
    int valid = result && !RAY_IS_ERR(result) && ray_table_nrows(result) == n;
    if (valid) {
        ray_t* os = ray_table_get_col(result, sn);
        ray_t* ot = ray_table_get_col(result, tn);
        for (int64_t i = 0; i < n; i++) {
            int64_t expected = (i % run_rows - 10000) * 1000000;
            valid &= ray_read_sym(ray_data(os), i, os->type, os->attrs) == ids[i / run_rows];
            valid &= ((int64_t*)ray_data(ot))[i] == expected;
            valid &= ((int64_t*)ray_data(sym))[i] == ids[i / run_rows];
            valid &= ((int64_t*)ray_data(time))[i] == expected;
        }
    }
    printf("typed|participants=%d|timestamp=%d|valid=%d|ms=%.6f\n", participants, stamp, valid, ms);
    ray_release(result); ray_release(table); ray_release(keys);
    ray_release(sym); ray_release(time);
    ray_pool_destroy(); ray_sym_destroy(); ray_heap_destroy();
    return valid ? 0 : 1;
}
