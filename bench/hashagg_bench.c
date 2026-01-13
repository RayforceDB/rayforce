/*
 * Bench driver for hashagg implementation with multiple group-by configs.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "hashagg.h"
#include "../core/runtime.h"
#include "../core/format.h"
#include "../core/ops.h"

static double elapsed_ms(struct timeval start, struct timeval end) {
    double ms = (end.tv_sec - start.tv_sec) * 1000.0;
    ms += (end.tv_usec - start.tv_usec) / 1000.0;
    return ms;
}

static void run_bench(obj_p table, const hashagg_config_t *cfg) {
    struct timeval start, end;
    hashagg_timings_t timings = {0};
    obj_p result;
    double total_ms;

    gettimeofday(&start, NULL);
    result = hashagg_group_sum(table, cfg, &timings);
    gettimeofday(&end, NULL);
    total_ms = elapsed_ms(start, end);

    if (IS_ERR(result)) {
        obj_p formatted = obj_fmt(result, 1);
        fprintf(stderr, "hashagg %s error: %s\n", cfg->label, AS_C8(formatted));
        drop_obj(formatted);
        drop_obj(result);
        return;
    }

    printf("hashagg %s: %.3f ms (%lld groups)\n", cfg->label, total_ms, ops_count(result));
    printf("  phases ms: hash=%.3f prefix=%.3f scatter=%.3f ht_alloc=%.3f ht_build=%.3f output=%.3f\n",
           timings.hash_ms, timings.prefix_ms, timings.scatter_ms, timings.ht_alloc_ms, timings.ht_build_ms,
           timings.output_ms);

    drop_obj(result);
}

int main(void) {
    obj_p table;
    str_p argv[] = {"hashagg", "-c", "0"};

    runtime_create(3, argv);

    table = hashagg_read_csv(HASHAGG_DEFAULT_CSV_PATH);
    if (IS_ERR(table)) {
        obj_p formatted = obj_fmt(table, 1);
        fprintf(stderr, "hashagg read-csv error: %s\n", AS_C8(formatted));
        drop_obj(formatted);
        drop_obj(table);
        runtime_destroy();
        return 1;
    }

    {
        hashagg_config_t cfg1 = {.label = "id1", .key_count = 1, .value_name = "v3"};
        cfg1.key_names[0] = "id1";

        hashagg_config_t cfg2 = {.label = "id1,id2", .key_count = 2, .value_name = "v3"};
        cfg2.key_names[0] = "id1";
        cfg2.key_names[1] = "id2";

        hashagg_config_t cfg4 = {.label = "id1..id4", .key_count = 4, .value_name = "v3"};
        cfg4.key_names[0] = "id1";
        cfg4.key_names[1] = "id2";
        cfg4.key_names[2] = "id3";
        cfg4.key_names[3] = "id4";

        hashagg_config_t cfg6 = {.label = "id1..id6", .key_count = 6, .value_name = "v3"};
        cfg6.key_names[0] = "id1";
        cfg6.key_names[1] = "id2";
        cfg6.key_names[2] = "id3";
        cfg6.key_names[3] = "id4";
        cfg6.key_names[4] = "id5";
        cfg6.key_names[5] = "id6";

        run_bench(table, &cfg1);
        run_bench(table, &cfg2);
        run_bench(table, &cfg4);
        run_bench(table, &cfg6);
    }

    drop_obj(table);
    runtime_destroy();
    return 0;
}
