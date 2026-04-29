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
#include <time.h>
#include <string.h>

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void report(const char* name, int64_t nrows, double elapsed_ns) {
    double rows_per_sec = (double)nrows / (elapsed_ns / 1e9);
    printf("%-24s  %10lld rows  %10.1f ms  %12.0f rows/sec\n",
           name, (long long)nrows, elapsed_ns / 1e6, rows_per_sec);
}

static void bench_vec_add(int64_t n) {
    int64_t* a_data = ray_sys_alloc((size_t)n * sizeof(int64_t));
    int64_t* b_data = ray_sys_alloc((size_t)n * sizeof(int64_t));
    for (int64_t i = 0; i < n; i++) { a_data[i] = i; b_data[i] = i * 2; }

    ray_t* a = ray_vec_from_raw(RAY_I64, a_data, n);
    ray_t* b = ray_vec_from_raw(RAY_I64, b_data, n);

    int64_t n_a = ray_sym_intern("a", 1);
    int64_t n_b = ray_sym_intern("b", 1);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, n_a, a);
    tbl = ray_table_add_col(tbl, n_b, b);
    ray_release(a); ray_release(b);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* sa = ray_scan(g, "a");
    ray_op_t* sb = ray_scan(g, "b");
    ray_op_t* add = ray_add(g, sa, sb);
    ray_op_t* s = ray_sum(g, add);

    double t0 = now_ns();
    ray_t* result = ray_execute(g, s);
    double elapsed = now_ns() - t0;

    report("vec_add", n, elapsed);

    if (result && !RAY_IS_ERR(result)) ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sys_free(a_data);
    ray_sys_free(b_data);
}

static void bench_filter(int64_t n) {
    int64_t* v_data = ray_sys_alloc((size_t)n * sizeof(int64_t));
    for (int64_t i = 0; i < n; i++) v_data[i] = i;

    ray_t* v = ray_vec_from_raw(RAY_I64, v_data, n);
    int64_t n_v = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, n_v, v);
    ray_release(v);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* sv = ray_scan(g, "v");
    ray_op_t* thresh = ray_const_i64(g, n / 2);
    ray_op_t* pred = ray_gt(g, sv, thresh);
    ray_op_t* flt = ray_filter(g, sv, pred);
    ray_op_t* s = ray_sum(g, flt);

    double t0 = now_ns();
    ray_t* result = ray_execute(g, s);
    double elapsed = now_ns() - t0;

    report("filter", n, elapsed);

    if (result && !RAY_IS_ERR(result)) ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sys_free(v_data);
}

/* Simple xorshift64 PRNG for reproducible random data */
static uint64_t bench_rng_state = 0x123456789ABCDEF0ULL;
static int64_t bench_rand(void) {
    bench_rng_state ^= bench_rng_state << 13;
    bench_rng_state ^= bench_rng_state >> 7;
    bench_rng_state ^= bench_rng_state << 17;
    return (int64_t)(bench_rng_state & 0x7FFFFFFFFFFFFFFFULL);
}

static void bench_sort_pattern(const char* name, int64_t* v_data, int64_t n) {
    ray_t* v = ray_vec_from_raw(RAY_I64, v_data, n);
    int64_t n_v = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, n_v, v);
    ray_release(v);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* sv = ray_scan(g, "v");
    ray_op_t* keys[] = { sv };
    uint8_t descs[] = { 0 };
    uint8_t nf[] = { 0 };
    ray_op_t* sort_op = ray_sort_op(g, sv, keys, descs, nf, 1);
    ray_op_t* s = ray_sum(g, sort_op);

    double t0 = now_ns();
    ray_t* result = ray_execute(g, s);
    double elapsed = now_ns() - t0;

    report(name, n, elapsed);

    if (result && !RAY_IS_ERR(result)) ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
}

static void bench_sort(int64_t n) {
    int64_t* v_data = ray_sys_alloc((size_t)n * sizeof(int64_t));

    /* Pattern 1: reverse-ordered */
    for (int64_t i = 0; i < n; i++) v_data[i] = n - i;
    bench_sort_pattern("sort_reverse", v_data, n);

    /* Pattern 2: random */
    bench_rng_state = 0x123456789ABCDEF0ULL;
    for (int64_t i = 0; i < n; i++) v_data[i] = bench_rand() % (n * 10);
    bench_sort_pattern("sort_random", v_data, n);

    /* Pattern 3: already sorted */
    for (int64_t i = 0; i < n; i++) v_data[i] = i;
    bench_sort_pattern("sort_sorted", v_data, n);

    /* Pattern 4: nearly sorted (1% random swaps) */
    for (int64_t i = 0; i < n; i++) v_data[i] = i;
    bench_rng_state = 0xDEADBEEFCAFEBABEULL;
    for (int64_t i = 0; i < n / 100; i++) {
        int64_t a = bench_rand() % n;
        int64_t b = bench_rand() % n;
        int64_t tmp = v_data[a]; v_data[a] = v_data[b]; v_data[b] = tmp;
    }
    bench_sort_pattern("sort_nearly", v_data, n);

    ray_sys_free(v_data);
}

static void bench_group(int64_t n) {
    int64_t* id_data = ray_sys_alloc((size_t)n * sizeof(int64_t));
    int64_t* v_data = ray_sys_alloc((size_t)n * sizeof(int64_t));
    for (int64_t i = 0; i < n; i++) { id_data[i] = i % 100; v_data[i] = i; }

    ray_t* id_v = ray_vec_from_raw(RAY_I64, id_data, n);
    ray_t* v_v = ray_vec_from_raw(RAY_I64, v_data, n);

    int64_t n_id = ray_sym_intern("id", 2);
    int64_t n_v = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, n_id, id_v);
    tbl = ray_table_add_col(tbl, n_v, v_v);
    ray_release(id_v); ray_release(v_v);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* sid = ray_scan(g, "id");
    ray_op_t* sv = ray_scan(g, "v");
    ray_op_t* keys[] = { sid };
    uint16_t agg_ops[] = { OP_SUM };
    ray_op_t* agg_ins[] = { sv };
    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);
    ray_op_t* cnt = ray_count(g, grp);

    double t0 = now_ns();
    ray_t* result = ray_execute(g, cnt);
    double elapsed = now_ns() - t0;

    report("group", n, elapsed);

    if (result && !RAY_IS_ERR(result)) ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sys_free(id_data);
    ray_sys_free(v_data);
}

/* ---- asof_join: ASOF join on time-series ---- */
static void bench_asof_join(int64_t n) {
    /* Left: trades with time, sym, price */
    int64_t* lt_data = ray_sys_alloc((size_t)n * sizeof(int64_t));
    int64_t* ls_data = ray_sys_alloc((size_t)n * sizeof(int64_t));
    int64_t* lp_data = ray_sys_alloc((size_t)n * sizeof(int64_t));
    for (int64_t i = 0; i < n; i++) {
        lt_data[i] = i * 10;
        ls_data[i] = i % 100;
        lp_data[i] = (i * 7 + 3) % 1000;
    }

    ray_t* lt_v = ray_vec_from_raw(RAY_I64, lt_data, n);
    ray_t* ls_v = ray_vec_from_raw(RAY_I64, ls_data, n);
    ray_t* lp_v = ray_vec_from_raw(RAY_I64, lp_data, n);

    int64_t n_time  = ray_sym_intern("time", 4);
    int64_t n_sym   = ray_sym_intern("sym", 3);
    int64_t n_price = ray_sym_intern("price", 5);

    ray_t* left = ray_table_new(3);
    left = ray_table_add_col(left, n_time, lt_v);
    left = ray_table_add_col(left, n_sym, ls_v);
    left = ray_table_add_col(left, n_price, lp_v);
    ray_release(lt_v); ray_release(ls_v); ray_release(lp_v);

    /* Right: quotes with time, sym, bid */
    int64_t* rt_data = ray_sys_alloc((size_t)n * sizeof(int64_t));
    int64_t* rs_data = ray_sys_alloc((size_t)n * sizeof(int64_t));
    int64_t* rb_data = ray_sys_alloc((size_t)n * sizeof(int64_t));
    for (int64_t i = 0; i < n; i++) {
        rt_data[i] = i * 10 + 5;  /* offset by 5 from trades */
        rs_data[i] = i % 100;
        rb_data[i] = (i * 13 + 7) % 1000;
    }

    ray_t* rt_v = ray_vec_from_raw(RAY_I64, rt_data, n);
    ray_t* rs_v = ray_vec_from_raw(RAY_I64, rs_data, n);
    ray_t* rb_v = ray_vec_from_raw(RAY_I64, rb_data, n);

    int64_t n_bid = ray_sym_intern("bid", 3);

    ray_t* right = ray_table_new(3);
    right = ray_table_add_col(right, n_time, rt_v);
    right = ray_table_add_col(right, n_sym, rs_v);
    right = ray_table_add_col(right, n_bid, rb_v);
    ray_release(rt_v); ray_release(rs_v); ray_release(rb_v);

    ray_graph_t* g = ray_graph_new(left);
    ray_op_t* left_op  = ray_const_table(g, left);
    ray_op_t* right_op = ray_const_table(g, right);
    ray_op_t* tkey = ray_scan(g, "time");
    ray_op_t* skey = ray_scan(g, "sym");
    ray_op_t* eq_keys[] = { skey };

    ray_op_t* aj = ray_asof_join(g, left_op, right_op, tkey, eq_keys, 1, 0);
    ray_op_t* cnt = ray_count(g, aj);

    double t0 = now_ns();
    ray_t* result = ray_execute(g, cnt);
    double elapsed = now_ns() - t0;

    report("asof_join", n, elapsed);

    if (result && !RAY_IS_ERR(result)) ray_release(result);
    ray_graph_free(g);
    ray_release(left);
    ray_release(right);
    ray_sys_free(lt_data); ray_sys_free(ls_data); ray_sys_free(lp_data);
    ray_sys_free(rt_data); ray_sys_free(rs_data); ray_sys_free(rb_data);
}

int main(void) {
    int64_t sizes[] = { 1000, 100000, 10000000 };
    int n_sizes = 3;

    printf("%-24s  %10s  %10s  %12s\n", "Benchmark", "Rows", "Time", "Throughput");
    printf("%-24s  %10s  %10s  %12s\n",
           "------------------------", "----------", "----------", "------------");

    for (int s = 0; s < n_sizes; s++) {
        ray_heap_init();
        { ray_err_t _e = ray_sym_init(); (void)_e; };

        bench_vec_add(sizes[s]);
        bench_filter(sizes[s]);
        bench_sort(sizes[s]);
        bench_group(sizes[s]);
        bench_asof_join(sizes[s]);

        ray_sym_destroy();
        ray_heap_destroy();

        printf("\n");
    }

    return 0;
}
