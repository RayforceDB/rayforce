#include <rayforce.h>
#include "mem/heap.h"
#include "table/sym.h"
#include "table/domain.h"
#include "ops/internal.h"
#include <stdio.h>
static unsigned long long resolutions;
ray_t* __real_ray_sym_domain_str(ray_sym_domain_t*, int64_t);
ray_t* __wrap_ray_sym_domain_str(ray_sym_domain_t* dom, int64_t id) {
    resolutions++;
    return __real_ray_sym_domain_str(dom, id);
}
int main(void) {
    ray_heap_init(); ray_sym_init();
    for (int i = 0; i < 70000; i++) {
        char name[40]; int n = snprintf(name, sizeof(name), "padding-%d", i);
        ray_sym_intern(name, n);
    }
    int64_t id = ray_sym_intern("late-used-symbol", 16);
    for (int n = 8; n <= 10000; n += 9992) {
        ray_t* col = ray_vec_new(RAY_SYM, n); col->len = n;
        for (int i = 0; i < n; i++) ((int64_t*)ray_data(col))[i] = id;
        for (int repeat = 0; repeat < 2; repeat++) {
            resolutions = 0;
            int sorted = ray_key_cols_sorted(&col, 1, 0, n);
            printf("rows=%d|repeat=%d|sorted=%d|resolutions=%llu\n", n, repeat, sorted, resolutions);
        }
        ray_release(col);
    }
    ray_sym_destroy(); ray_heap_destroy();
    return 0;
}
