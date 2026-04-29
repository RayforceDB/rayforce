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
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void generate_csv(const char* path, int64_t n_rows, int64_t n_unique) {
    FILE* f = fopen(path, "w");
    fprintf(f, "id,val,sym\n");
    for (int64_t i = 0; i < n_rows; i++) {
        fprintf(f, "%lld,%.2f,sym_%lld\n",
                (long long)i, (double)i * 0.1, (long long)(i % n_unique));
    }
    fclose(f);
}

static void bench(const char* label, const char* path, int64_t n_rows, int reps) {
    double best = 1e18;
    for (int r = 0; r < reps; r++) {
        ray_heap_init();
        { ray_err_t _e = ray_sym_init(); (void)_e; };

        double start = now_ns();
        ray_t* t = ray_read_csv(path);
        double elapsed = now_ns() - start;

        if (t && !RAY_IS_ERR(t)) ray_release(t);
        ray_sym_destroy();
        ray_heap_destroy();

        if (elapsed < best) best = elapsed;
    }
    printf("%-36s  %8lld rows  %8.1f ms  %12.0f rows/sec\n",
           label, (long long)n_rows, best / 1e6,
           (double)n_rows / (best / 1e9));
}

int main(void) {
    const char* csv_path = "/tmp/rayforce_bench_csv_detail.csv";

    printf("%-36s  %8s       %8s  %12s\n",
           "Benchmark", "Rows", "Time", "Throughput");
    printf("%-36s  %8s       %8s  %12s\n",
           "------------------------------------", "--------", "--------", "------------");

    /* Run each 5 times, take best */

    generate_csv(csv_path, 100000, 100);
    bench("100K rows, 100 unique syms", csv_path, 100000, 5);

    generate_csv(csv_path, 1000000, 100);
    bench("1M rows, 100 unique syms", csv_path, 1000000, 5);

    generate_csv(csv_path, 100000, 100000);
    bench("100K rows, 100K unique syms", csv_path, 100000, 5);

    generate_csv(csv_path, 1000000, 100000);
    bench("1M rows, 100K unique syms", csv_path, 1000000, 5);

    generate_csv(csv_path, 1000000, 1000000);
    bench("1M rows, 1M unique syms", csv_path, 1000000, 5);

    generate_csv(csv_path, 1000000, 1);  /* all same symbol */
    bench("1M rows, 1 unique sym", csv_path, 1000000, 5);

    /* Numeric only baseline */
    {
        FILE* f = fopen(csv_path, "w");
        fprintf(f, "id,val\n");
        for (int64_t i = 0; i < 1000000; i++)
            fprintf(f, "%lld,%.2f\n", (long long)i, (double)i * 0.1);
        fclose(f);
        bench("1M rows, numeric only", csv_path, 1000000, 5);
    }

    unlink(csv_path);
    return 0;
}
