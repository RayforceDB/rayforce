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

#define _POSIX_C_SOURCE 199309L
#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include <mem/sys.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/utsname.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#else
#include <unistd.h>
#endif

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void report(const char* name, int64_t nrows, double elapsed_ns) {
    double rows_per_sec = (double)nrows / (elapsed_ns / 1e9);
    printf("%-30s  %10lld rows  %10.1f ms  %12.0f rows/sec\n",
           name, (long long)nrows, elapsed_ns / 1e6, rows_per_sec);
}

/* Q1: scan + filter + group + sum (analytics) */
static void bench_q1_analytics(int64_t n) {
    ray_heap_init();
    { ray_err_t _e = ray_sym_init(); (void)_e; };

    int64_t* region_data = ray_sys_alloc((size_t)n * sizeof(int64_t));
    int64_t* amount_data = ray_sys_alloc((size_t)n * sizeof(int64_t));
    int64_t* flag_data   = ray_sys_alloc((size_t)n * sizeof(int64_t));
    for (int64_t i = 0; i < n; i++) {
        region_data[i] = i % 5;
        amount_data[i] = (i * 7 + 13) % 1000;
        flag_data[i]   = i % 3;
    }

    ray_t* r_v = ray_vec_from_raw(RAY_I64, region_data, n);
    ray_t* a_v = ray_vec_from_raw(RAY_I64, amount_data, n);
    ray_t* f_v = ray_vec_from_raw(RAY_I64, flag_data, n);

    int64_t n_r = ray_sym_intern("region", 6);
    int64_t n_a = ray_sym_intern("amount", 6);
    int64_t n_f = ray_sym_intern("flag", 4);

    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, n_r, r_v);
    tbl = ray_table_add_col(tbl, n_a, a_v);
    tbl = ray_table_add_col(tbl, n_f, f_v);
    ray_release(r_v); ray_release(a_v); ray_release(f_v);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* sf = ray_scan(g, "flag");
    ray_op_t* zero = ray_const_i64(g, 0);
    ray_op_t* pred = ray_eq(g, sf, zero);
    ray_op_t* sr = ray_scan(g, "region");
    ray_op_t* sa = ray_scan(g, "amount");
    ray_op_t* flt_r = ray_filter(g, sr, pred);
    ray_op_t* flt_a = ray_filter(g, sa, pred);
    ray_op_t* keys[] = { flt_r };
    uint16_t agg_ops[] = { OP_SUM };
    ray_op_t* agg_ins[] = { flt_a };
    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);
    ray_op_t* cnt = ray_count(g, grp);

    double t0 = now_ns();
    ray_t* result = ray_execute(g, cnt);
    double elapsed = now_ns() - t0;

    report("Q1: filter+group+sum", n, elapsed);

    if (result && !RAY_IS_ERR(result)) ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sys_free(region_data);
    ray_sys_free(amount_data);
    ray_sys_free(flag_data);
    ray_sym_destroy();
    ray_heap_destroy();
}

/* Q2: join + count (relational) */
static void bench_q2_relational(int64_t n) {
    ray_heap_init();
    { ray_err_t _e = ray_sym_init(); (void)_e; };

    int64_t* oid_data = ray_sys_alloc((size_t)n * sizeof(int64_t));
    int64_t* cid_data = ray_sys_alloc((size_t)n * sizeof(int64_t));
    int64_t* amt_data = ray_sys_alloc((size_t)n * sizeof(int64_t));
    for (int64_t i = 0; i < n; i++) {
        oid_data[i] = i;
        cid_data[i] = i % 1000;
        amt_data[i] = (i * 13 + 7) % 10000;
    }

    ray_t* o_v = ray_vec_from_raw(RAY_I64, oid_data, n);
    ray_t* c_v = ray_vec_from_raw(RAY_I64, cid_data, n);
    ray_t* a_v = ray_vec_from_raw(RAY_I64, amt_data, n);

    int64_t n_oid = ray_sym_intern("oid", 3);
    int64_t n_cid = ray_sym_intern("cid", 3);
    int64_t n_amt = ray_sym_intern("amt", 3);

    ray_t* orders = ray_table_new(3);
    orders = ray_table_add_col(orders, n_oid, o_v);
    orders = ray_table_add_col(orders, n_cid, c_v);
    orders = ray_table_add_col(orders, n_amt, a_v);
    ray_release(o_v); ray_release(c_v); ray_release(a_v);

    int64_t n_cust = 1000;
    int64_t* c2_data = ray_sys_alloc((size_t)n_cust * sizeof(int64_t));
    int64_t* sc_data = ray_sys_alloc((size_t)n_cust * sizeof(int64_t));
    for (int64_t i = 0; i < n_cust; i++) { c2_data[i] = i; sc_data[i] = i * 100; }

    ray_t* c2_v = ray_vec_from_raw(RAY_I64, c2_data, n_cust);
    ray_t* sc_v = ray_vec_from_raw(RAY_I64, sc_data, n_cust);

    int64_t n_score = ray_sym_intern("score", 5);

    ray_t* custs = ray_table_new(2);
    custs = ray_table_add_col(custs, n_cid, c2_v);
    custs = ray_table_add_col(custs, n_score, sc_v);
    ray_release(c2_v); ray_release(sc_v);

    ray_graph_t* g = ray_graph_new(orders);
    ray_op_t* lo = ray_const_table(g, orders);
    ray_op_t* ro = ray_const_table(g, custs);
    ray_op_t* lk = ray_scan(g, "cid");
    ray_op_t* lk_arr[] = { lk };
    ray_op_t* rk_arr[] = { lk };
    ray_op_t* join_op = ray_join(g, lo, lk_arr, ro, rk_arr, 1, 0);
    ray_op_t* cnt = ray_count(g, join_op);

    double t0 = now_ns();
    ray_t* result = ray_execute(g, cnt);
    double elapsed = now_ns() - t0;

    report("Q2: join+count", n, elapsed);

    if (result && !RAY_IS_ERR(result)) ray_release(result);
    ray_graph_free(g);
    ray_release(orders);
    ray_release(custs);
    ray_sys_free(oid_data);
    ray_sys_free(cid_data);
    ray_sys_free(amt_data);
    ray_sys_free(c2_data);
    ray_sys_free(sc_data);
    ray_sym_destroy();
    ray_heap_destroy();
}

static void print_machine_info(void) {
    struct utsname u;
    uname(&u);
    printf("System:  %s %s %s\n", u.sysname, u.release, u.machine);

#ifdef __APPLE__
    /* CPU brand string */
    char cpu[128] = "unknown";
    size_t cpu_len = sizeof(cpu);
    if (sysctlbyname("machdep.cpu.brand_string", cpu, &cpu_len, NULL, 0) != 0)
        sysctlbyname("hw.model", cpu, &cpu_len, NULL, 0);
    printf("CPU:     %s\n", cpu);

    /* Physical + logical cores */
    int pcores = 0, lcores = 0;
    size_t sz = sizeof(int);
    sysctlbyname("hw.physicalcpu", &pcores, &sz, NULL, 0);
    sysctlbyname("hw.logicalcpu", &lcores, &sz, NULL, 0);
    printf("Cores:   %d physical, %d logical\n", pcores, lcores);

    /* RAM */
    uint64_t mem = 0;
    size_t msz = sizeof(mem);
    sysctlbyname("hw.memsize", &mem, &msz, NULL, 0);
    printf("Memory:  %llu GB\n", (unsigned long long)(mem / (1024ULL * 1024 * 1024)));

    /* L1/L2/L3 cache sizes */
    uint64_t l1d = 0, l2 = 0, l3 = 0;
    size_t csz = sizeof(uint64_t);
    sysctlbyname("hw.l1dcachesize", &l1d, &csz, NULL, 0);
    sysctlbyname("hw.l2cachesize", &l2, &csz, NULL, 0);
    sysctlbyname("hw.l3cachesize", &l3, &csz, NULL, 0);
    if (l1d) printf("Cache:   L1d %llu KB", (unsigned long long)(l1d / 1024));
    if (l2)  printf(", L2 %llu KB", (unsigned long long)(l2 / 1024));
    if (l3)  printf(", L3 %llu MB", (unsigned long long)(l3 / (1024 * 1024)));
    printf("\n");
#else
    /* Linux: read /proc */
    FILE* f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "model name", 10) == 0) {
                char* p = strchr(line, ':');
                if (p) printf("CPU:     %s", p + 2);
                break;
            }
        }
        fclose(f);
    }

    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc > 0) printf("Cores:   %ld\n", nproc);

    long pages = sysconf(_SC_PHYS_PAGES);
    long page_sz = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_sz > 0) {
        uint64_t mem = (uint64_t)pages * (uint64_t)page_sz;
        printf("Memory:  %llu GB\n", (unsigned long long)(mem / (1024ULL * 1024 * 1024)));
    }
#endif

    /* Build type */
#ifdef NDEBUG
    printf("Build:   Release\n");
#else
    printf("Build:   Debug\n");
#endif
    printf("\n");
}

int main(void) {
    int64_t sizes[] = { 10000, 1000000 };
    int n_sizes = 2;

    print_machine_info();

    printf("%-30s  %10s  %10s  %12s\n", "Query", "Rows", "Time", "Throughput");
    printf("%-30s  %10s  %10s  %12s\n",
           "------------------------------", "----------", "----------", "------------");

    for (int s = 0; s < n_sizes; s++) {
        bench_q1_analytics(sizes[s]);
        bench_q2_relational(sizes[s]);
        printf("\n");
    }

    return 0;
}
