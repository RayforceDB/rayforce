#ifndef HASHAGG_H
#define HASHAGG_H

#include "../core/rayforce.h"

#define HASHAGG_MAX_KEYS 6
#define HASHAGG_DEFAULT_CSV_PATH "../rayforce-bench/datasets/G1_1e7_1e2_0_0/G1_1e7_1e2_0_0.csv"

typedef struct {
    double hash_ms;
    double prefix_ms;
    double scatter_ms;
    double ht_alloc_ms;
    double ht_build_ms;
    double output_ms;
} hashagg_timings_t;

typedef struct {
    const char *label;
    i64_t key_count;
    const char *key_names[HASHAGG_MAX_KEYS];
    const char *value_name;
} hashagg_config_t;

obj_p hashagg_read_csv(const char *path);
obj_p hashagg_group_sum(obj_p table, const hashagg_config_t *cfg, hashagg_timings_t *timings);

#endif  // HASHAGG_H
