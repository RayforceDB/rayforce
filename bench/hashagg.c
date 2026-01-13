/*
 * Standalone hash aggregation implementation for benchmarks.
 * Shared engine for multiple group-by configurations.
 */

#define _POSIX_C_SOURCE 200809L
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "hashagg.h"
#include "../core/runtime.h"
#include "../core/io.h"
#include "../core/symbols.h"
#include "../core/hash.h"
#include "../core/ops.h"
#include "../core/heap.h"
#include "../core/pool.h"
#include "../core/util.h"
#include "../core/error.h"
#include "../core/format.h"

#define HASHAGG_RADIX_BITS 8
#define HASHAGG_PARTITIONS (1u << HASHAGG_RADIX_BITS)
#define HASHAGG_LOAD_FACTOR 1.5
#define HASHAGG_HT_MIN_CAP 1024
#define HASHAGG_SPLIT_THRESHOLD (RAY_PAGE_SIZE * 4)
#define HASHAGG_PERFECT_MAX_BYTES (256 * 1024 * 1024)

#define HASHAGG_SALT_MASK 0xFFFF000000000000ULL
#define HASHAGG_POINTER_MASK 0x0000FFFFFFFFFFFFULL
#define HASHAGG_RADIX_SHIFT (64 - 16 - HASHAGG_RADIX_BITS)

typedef union {
    i64_t i64;
    f64_t f64;
} hashagg_sum_t;

typedef struct {
    i64_t row_start;
    i64_t row_count;
    u64_t *entries;
    i64_t capacity;
    u64_t bitmask;
    u8_t *rows;
    i64_t row_size;
    i64_t group_count;
} hashagg_partition_t;

typedef struct {
    i64_t key_count;
    i64_t row_count;
    i64_t *row_ids;
    u64_t *hashes;
    i64_t *key_data[HASHAGG_MAX_KEYS];
    i8_t key_types[HASHAGG_MAX_KEYS];
    i8_t value_type;
    i64_t *value_i64;
    f64_t *value_f64;
} hashagg_context_t;

typedef struct {
    hashagg_context_t *ctx;
    hashagg_partition_t *parts;
} hashagg_task_ctx_t;

typedef struct {
    hashagg_partition_t *parts;
    i64_t *group_offsets;
    i64_t *out_keys[HASHAGG_MAX_KEYS];
    i64_t *out_sum_i64;
    f64_t *out_sum_f64;
    i8_t value_type;
    i64_t key_count;
} hashagg_output_ctx_t;

static inline double hashagg_elapsed_ms(struct timeval start, struct timeval end) {
    double ms = (end.tv_sec - start.tv_sec) * 1000.0;
    ms += (end.tv_usec - start.tv_usec) / 1000.0;
    return ms;
}

static inline u64_t hashagg_radix_partition(u64_t hash) {
    return (hash >> HASHAGG_RADIX_SHIFT) & (HASHAGG_PARTITIONS - 1);
}

static inline u64_t hashagg_salt(u64_t hash) { return hash & HASHAGG_SALT_MASK; }

static inline u64_t hashagg_probe_next(u64_t offset, u64_t hash, u64_t bitmask) {
    static const u64_t increment_bits = 5;
    u64_t increment = (hash >> (64 - increment_bits)) | 1ULL;
    return (offset + increment) & bitmask;
}

static inline u64_t hashagg_hash_row(const hashagg_context_t *ctx, i64_t row) {
    u64_t h = U64_HASH_SEED;

    switch (ctx->key_count) {
        case 1:
            return hash_index_u64(h, (u64_t)ctx->key_data[0][row]);
        case 2:
            h = hash_index_u64(h, (u64_t)ctx->key_data[0][row]);
            return hash_index_u64(h, (u64_t)ctx->key_data[1][row]);
        case 4:
            h = hash_index_u64(h, (u64_t)ctx->key_data[0][row]);
            h = hash_index_u64(h, (u64_t)ctx->key_data[1][row]);
            h = hash_index_u64(h, (u64_t)ctx->key_data[2][row]);
            return hash_index_u64(h, (u64_t)ctx->key_data[3][row]);
        case 6:
            h = hash_index_u64(h, (u64_t)ctx->key_data[0][row]);
            h = hash_index_u64(h, (u64_t)ctx->key_data[1][row]);
            h = hash_index_u64(h, (u64_t)ctx->key_data[2][row]);
            h = hash_index_u64(h, (u64_t)ctx->key_data[3][row]);
            h = hash_index_u64(h, (u64_t)ctx->key_data[4][row]);
            return hash_index_u64(h, (u64_t)ctx->key_data[5][row]);
        default:
            for (i64_t k = 0; k < ctx->key_count; k++) {
                h = hash_index_u64(h, (u64_t)ctx->key_data[k][row]);
            }
            return h;
    }
}

static inline b8_t hashagg_keys_match(const hashagg_context_t *ctx, const u8_t *row_ptr, i64_t row_idx) {
    const i64_t *keys = (const i64_t *)row_ptr;

    switch (ctx->key_count) {
        case 1:
            return keys[0] == ctx->key_data[0][row_idx];
        case 2:
            return keys[0] == ctx->key_data[0][row_idx] && keys[1] == ctx->key_data[1][row_idx];
        case 4:
            return keys[0] == ctx->key_data[0][row_idx] && keys[1] == ctx->key_data[1][row_idx]
                   && keys[2] == ctx->key_data[2][row_idx] && keys[3] == ctx->key_data[3][row_idx];
        case 6:
            return keys[0] == ctx->key_data[0][row_idx] && keys[1] == ctx->key_data[1][row_idx]
                   && keys[2] == ctx->key_data[2][row_idx] && keys[3] == ctx->key_data[3][row_idx]
                   && keys[4] == ctx->key_data[4][row_idx] && keys[5] == ctx->key_data[5][row_idx];
        default:
            for (i64_t k = 0; k < ctx->key_count; k++) {
                if (keys[k] != ctx->key_data[k][row_idx])
                    return B8_FALSE;
            }
            return B8_TRUE;
    }
}

static inline void hashagg_copy_keys(const hashagg_context_t *ctx, i64_t row_idx, i64_t *dst_keys) {
    for (i64_t k = 0; k < ctx->key_count; k++)
        dst_keys[k] = ctx->key_data[k][row_idx];
}

static b8_t hashagg_build_perfect(obj_p *out_result, const hashagg_config_t *cfg, const hashagg_context_t *ctx,
                                  const i64_t *mins, const u64_t *ranges, u64_t cardinality,
                                  hashagg_timings_t *timings) {
    u64_t bytes_per = (ctx->value_type == TYPE_F64) ? (u64_t)sizeof(f64_t) : (u64_t)sizeof(i64_t);
    u64_t needed = cardinality * (bytes_per + 1);
    obj_p out_names = NULL_OBJ;
    obj_p out_vals = NULL_OBJ;
    obj_p out_vecs[HASHAGG_MAX_KEYS];
    obj_p out_sum = NULL_OBJ;
    b8_t *present = NULL;
    i64_t group_count = 0;
    struct timeval t0, t1;

    if (needed > HASHAGG_PERFECT_MAX_BYTES)
        return B8_FALSE;

    present = (b8_t *)heap_alloc(cardinality * sizeof(b8_t));
    if (!present)
        return B8_FALSE;
    memset(present, 0, cardinality * sizeof(b8_t));

    if (ctx->value_type == TYPE_F64) {
        f64_t *sums = (f64_t *)heap_alloc(cardinality * sizeof(f64_t));
        if (!sums) {
            heap_free(present);
            return B8_FALSE;
        }
        for (u64_t i = 0; i < cardinality; i++)
            sums[i] = 0.0;

        gettimeofday(&t0, NULL);
        for (i64_t row = 0; row < ctx->row_count; row++) {
            u64_t gid = 0;
            for (i64_t k = 0; k < ctx->key_count; k++) {
                u64_t offs = (u64_t)(ctx->key_data[k][row] - mins[k]);
                gid = gid * ranges[k] + offs;
            }
            present[gid] = 1;
            sums[gid] = ADDF64(sums[gid], ctx->value_f64[row]);
        }
        gettimeofday(&t1, NULL);
        timings->ht_build_ms = hashagg_elapsed_ms(t0, t1);

        group_count = 0;
        for (u64_t i = 0; i < cardinality; i++)
            group_count += present[i] ? 1 : 0;

        gettimeofday(&t0, NULL);
        out_names = SYMBOL(cfg->key_count + 1);
        out_vals = LIST(cfg->key_count + 1);
        for (i64_t k = 0; k < cfg->key_count; k++) {
            AS_I64(out_names)[k] = symbols_intern(cfg->key_names[k], (i64_t)strlen(cfg->key_names[k]));
            out_vecs[k] = vector(ctx->key_types[k], group_count);
            AS_LIST(out_vals)[k] = out_vecs[k];
        }
        AS_I64(out_names)[cfg->key_count] = symbols_intern(cfg->value_name, (i64_t)strlen(cfg->value_name));
        out_sum = vector(ctx->value_type, group_count);
        AS_LIST(out_vals)[cfg->key_count] = out_sum;

        {
            i64_t out_idx = 0;
            for (u64_t gid = 0; gid < cardinality; gid++) {
                if (!present[gid])
                    continue;

                u64_t tmp = gid;
                for (i64_t k = ctx->key_count - 1; k >= 0; k--) {
                    u64_t rem = tmp % ranges[k];
                    tmp /= ranges[k];
                    AS_I64(out_vecs[k])[out_idx] = mins[k] + (i64_t)rem;
                    if (k == 0)
                        break;
                }
                AS_F64(out_sum)[out_idx] = sums[gid];
                out_idx++;
            }
        }
        gettimeofday(&t1, NULL);
        timings->output_ms = hashagg_elapsed_ms(t0, t1);

        heap_free(sums);
    } else {
        i64_t *sums = (i64_t *)heap_alloc(cardinality * sizeof(i64_t));
        if (!sums) {
            heap_free(present);
            return B8_FALSE;
        }
        for (u64_t i = 0; i < cardinality; i++)
            sums[i] = 0;

        gettimeofday(&t0, NULL);
        for (i64_t row = 0; row < ctx->row_count; row++) {
            u64_t gid = 0;
            for (i64_t k = 0; k < ctx->key_count; k++) {
                u64_t offs = (u64_t)(ctx->key_data[k][row] - mins[k]);
                gid = gid * ranges[k] + offs;
            }
            present[gid] = 1;
            sums[gid] = ADDI64(sums[gid], ctx->value_i64[row]);
        }
        gettimeofday(&t1, NULL);
        timings->ht_build_ms = hashagg_elapsed_ms(t0, t1);

        group_count = 0;
        for (u64_t i = 0; i < cardinality; i++)
            group_count += present[i] ? 1 : 0;

        gettimeofday(&t0, NULL);
        out_names = SYMBOL(cfg->key_count + 1);
        out_vals = LIST(cfg->key_count + 1);
        for (i64_t k = 0; k < cfg->key_count; k++) {
            AS_I64(out_names)[k] = symbols_intern(cfg->key_names[k], (i64_t)strlen(cfg->key_names[k]));
            out_vecs[k] = vector(ctx->key_types[k], group_count);
            AS_LIST(out_vals)[k] = out_vecs[k];
        }
        AS_I64(out_names)[cfg->key_count] = symbols_intern(cfg->value_name, (i64_t)strlen(cfg->value_name));
        out_sum = vector(ctx->value_type, group_count);
        AS_LIST(out_vals)[cfg->key_count] = out_sum;

        {
            i64_t out_idx = 0;
            for (u64_t gid = 0; gid < cardinality; gid++) {
                if (!present[gid])
                    continue;

                u64_t tmp = gid;
                for (i64_t k = ctx->key_count - 1; k >= 0; k--) {
                    u64_t rem = tmp % ranges[k];
                    tmp /= ranges[k];
                    AS_I64(out_vecs[k])[out_idx] = mins[k] + (i64_t)rem;
                    if (k == 0)
                        break;
                }
                AS_I64(out_sum)[out_idx] = sums[gid];
                out_idx++;
            }
        }
        gettimeofday(&t1, NULL);
        timings->output_ms = hashagg_elapsed_ms(t0, t1);

        heap_free(sums);
    }

    heap_free(present);
    *out_result = table(out_names, out_vals);
    return B8_TRUE;
}

static i64_t hashagg_find_symbol_idx(obj_p symbols, i64_t sym) {
    i64_t i, l;
    i64_t *syms;

    l = symbols->len;
    syms = AS_I64(symbols);

    for (i = 0; i < l; i++) {
        if (syms[i] == sym)
            return i;
    }

    return NULL_I64;
}

obj_p hashagg_read_csv(const char *path) {
    static const char *type_names[] = {"SYMBOL", "SYMBOL", "SYMBOL", "I64", "I64", "I64", "I64", "I64", "F64"};
    const i64_t type_count = (i64_t)(sizeof(type_names) / sizeof(type_names[0]));
    obj_p types = SYMBOL(type_count);
    obj_p csv_path = cstring_from_str(path, (i64_t)strlen(path));
    obj_p args[2];
    obj_p res;

    for (i64_t i = 0; i < type_count; i++) {
        AS_I64(types)[i] = symbols_intern(type_names[i], (i64_t)strlen(type_names[i]));
    }

    args[0] = types;
    args[1] = csv_path;
    res = ray_read_csv(args, 2);

    drop_obj(types);
    drop_obj(csv_path);

    return res;
}

static obj_p hashagg_hash_task(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4, raw_p arg5, raw_p arg6, raw_p arg7) {
    i64_t len = (i64_t)arg1;
    i64_t offset = (i64_t)arg2;
    hashagg_context_t *ctx = (hashagg_context_t *)arg3;
    u64_t *hist = (u64_t *)arg4;
    i64_t *mins = (i64_t *)arg5;
    i64_t *maxs = (i64_t *)arg6;
    b8_t *has_null = (b8_t *)arg7;

    for (i64_t i = 0; i < len; i++) {
        i64_t row = offset + i;
        u64_t hash = hashagg_hash_row(ctx, row);
        u64_t part = hashagg_radix_partition(hash);

        ctx->hashes[row] = hash;
        hist[part]++;

        for (i64_t k = 0; k < ctx->key_count; k++) {
            i64_t val = ctx->key_data[k][row];
            if (val == NULL_I64) {
                *has_null = B8_TRUE;
                continue;
            }
            if (val < mins[k])
                mins[k] = val;
            if (val > maxs[k])
                maxs[k] = val;
        }
    }

    return NULL_OBJ;
}

static obj_p hashagg_scatter_task(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4) {
    i64_t len = (i64_t)arg1;
    i64_t offset = (i64_t)arg2;
    hashagg_context_t *ctx = (hashagg_context_t *)arg3;
    i64_t *scatter = (i64_t *)arg4;

    for (i64_t i = 0; i < len; i++) {
        i64_t row = offset + i;
        u64_t part = hashagg_radix_partition(ctx->hashes[row]);
        i64_t pos = scatter[part]++;
        ctx->row_ids[pos] = row;
    }

    return NULL_OBJ;
}

static obj_p hashagg_partition_task(raw_p arg1, raw_p arg2, raw_p arg3) {
    i64_t start = (i64_t)arg1;
    i64_t end = (i64_t)arg2;
    hashagg_task_ctx_t *task_ctx = (hashagg_task_ctx_t *)arg3;
    hashagg_context_t *ctx = task_ctx->ctx;
    const i64_t key_bytes = (i64_t)(ctx->key_count * sizeof(i64_t));

    for (i64_t p = start; p < end; p++) {
        hashagg_partition_t *part = &task_ctx->parts[p];
        i64_t offset = part->row_start;
        i64_t limit = part->row_start + part->row_count;

        if (part->row_count == 0)
            continue;

        for (i64_t i = offset; i < limit; i++) {
            i64_t row_idx = ctx->row_ids[i];
            u64_t hash = ctx->hashes[row_idx];
            u64_t salt = hashagg_salt(hash);
            u64_t ht_offset = hash & part->bitmask;

            for (;;) {
                u64_t entry = part->entries[ht_offset];

                if (entry == 0) {
                    i64_t group_idx = part->group_count++;
                    u8_t *row_ptr = part->rows + group_idx * part->row_size;
                    i64_t *keys = (i64_t *)row_ptr;
                    u8_t *sum_ptr = row_ptr + key_bytes;

                    hashagg_copy_keys(ctx, row_idx, keys);

                    if (ctx->value_type == TYPE_F64) {
                        *(f64_t *)sum_ptr = 0.0;
                        *(f64_t *)sum_ptr = ADDF64(*(f64_t *)sum_ptr, ctx->value_f64[row_idx]);
                    } else {
                        *(i64_t *)sum_ptr = 0;
                        *(i64_t *)sum_ptr = ADDI64(*(i64_t *)sum_ptr, ctx->value_i64[row_idx]);
                    }

                    part->entries[ht_offset] = (salt & HASHAGG_SALT_MASK) | (((u64_t)row_ptr) & HASHAGG_POINTER_MASK);
                    break;
                }

                if ((entry & HASHAGG_SALT_MASK) == (salt & HASHAGG_SALT_MASK)) {
                    u8_t *row_ptr = (u8_t *)(entry & HASHAGG_POINTER_MASK);
                    u8_t *sum_ptr = row_ptr + key_bytes;

                    if (hashagg_keys_match(ctx, row_ptr, row_idx)) {
                        if (ctx->value_type == TYPE_F64)
                            *(f64_t *)sum_ptr = ADDF64(*(f64_t *)sum_ptr, ctx->value_f64[row_idx]);
                        else
                            *(i64_t *)sum_ptr = ADDI64(*(i64_t *)sum_ptr, ctx->value_i64[row_idx]);
                        break;
                    }
                }

                ht_offset = hashagg_probe_next(ht_offset, hash, part->bitmask);
            }
        }
    }

    return NULL_OBJ;
}

static obj_p hashagg_output_task(raw_p arg1, raw_p arg2, raw_p arg3) {
    i64_t start = (i64_t)arg1;
    i64_t end = (i64_t)arg2;
    hashagg_output_ctx_t *ctx = (hashagg_output_ctx_t *)arg3;
    const i64_t key_bytes = (i64_t)(ctx->key_count * sizeof(i64_t));

    for (i64_t p = start; p < end; p++) {
        hashagg_partition_t *part = &ctx->parts[p];
        i64_t base = ctx->group_offsets[p];

        for (i64_t g = 0; g < part->group_count; g++) {
            u8_t *row_ptr = part->rows + g * part->row_size;
            i64_t *keys = (i64_t *)row_ptr;
            u8_t *sum_ptr = row_ptr + key_bytes;
            i64_t out_idx = base + g;

            for (i64_t k = 0; k < ctx->key_count; k++)
                ctx->out_keys[k][out_idx] = keys[k];

            if (ctx->value_type == TYPE_F64)
                ctx->out_sum_f64[out_idx] = *(f64_t *)sum_ptr;
            else
                ctx->out_sum_i64[out_idx] = *(i64_t *)sum_ptr;
        }
    }

    return NULL_OBJ;
}

obj_p hashagg_group_sum(obj_p input_table, const hashagg_config_t *cfg, hashagg_timings_t *timings) {
    obj_p colnames, cols;
    obj_p value_col;
    hashagg_context_t ctx = {0};
    hashagg_partition_t *parts = NULL;
    hashagg_task_ctx_t task_ctx;
    i64_t value_idx;
    i64_t rows;
    i64_t total_groups = 0;
    u64_t *partition_counts = NULL;
    i64_t *partition_offsets = NULL;
    u64_t *chunk_counts = NULL;
    i64_t *chunk_offsets = NULL;
    i64_t *scatter_positions = NULL;
    i64_t *group_offsets = NULL;
    i64_t *chunk_mins = NULL;
    i64_t *chunk_maxs = NULL;
    b8_t *chunk_has_null = NULL;
    i64_t mins[HASHAGG_MAX_KEYS];
    i64_t maxs[HASHAGG_MAX_KEYS];
    u64_t ranges[HASHAGG_MAX_KEYS];
    u64_t cardinality = 0;
    b8_t perfect_ok = B8_FALSE;
    obj_p out_names = NULL_OBJ;
    obj_p out_vals = NULL_OBJ;
    obj_p out_vecs[HASHAGG_MAX_KEYS];
    obj_p out_sum = NULL_OBJ;
    obj_p result = NULL_OBJ;
    obj_p err = NULL_OBJ;
    hashagg_timings_t local_timings = {0};
    struct timeval t0, t1;
    hashagg_output_ctx_t out_ctx;

    if (!cfg || cfg->key_count < 1 || cfg->key_count > HASHAGG_MAX_KEYS)
        return err_user("hashagg: invalid key count");

    if (input_table->type != TYPE_TABLE)
        return err_type(TYPE_TABLE, input_table->type, 0, 0);

    if (timings == NULL)
        timings = &local_timings;
    *timings = (hashagg_timings_t){0};

    colnames = AS_LIST(input_table)[0];
    cols = AS_LIST(input_table)[1];
    rows = ops_count(input_table);

    ctx.key_count = cfg->key_count;

    for (i64_t k = 0; k < cfg->key_count; k++) {
        const char *name = cfg->key_names[k];
        i64_t sym = symbols_intern(name, (i64_t)strlen(name));
        i64_t idx = hashagg_find_symbol_idx(colnames, sym);
        obj_p col;

        if (idx == NULL_I64)
            return err_value(sym);

        col = AS_LIST(cols)[idx];
        if (col->type != TYPE_SYMBOL && col->type != TYPE_I64)
            return err_type(TYPE_SYMBOL, col->type, 0, 0);

        ctx.key_types[k] = col->type;
        ctx.key_data[k] = AS_I64(col);
    }

    value_idx = hashagg_find_symbol_idx(colnames, symbols_intern(cfg->value_name, (i64_t)strlen(cfg->value_name)));
    if (value_idx == NULL_I64)
        return err_value(symbols_intern(cfg->value_name, (i64_t)strlen(cfg->value_name)));

    value_col = AS_LIST(cols)[value_idx];
    if (value_col->type != TYPE_F64 && value_col->type != TYPE_I64)
        return err_type(TYPE_F64, value_col->type, 0, 0);

    ctx.value_type = value_col->type;
    ctx.value_f64 = (value_col->type == TYPE_F64) ? AS_F64(value_col) : NULL;
    ctx.value_i64 = (value_col->type == TYPE_I64) ? AS_I64(value_col) : NULL;
    ctx.row_count = rows;

    if (rows == 0) {
        out_names = SYMBOL(cfg->key_count + 1);
        out_vals = LIST(cfg->key_count + 1);
        for (i64_t k = 0; k < cfg->key_count; k++) {
            AS_I64(out_names)[k] = symbols_intern(cfg->key_names[k], (i64_t)strlen(cfg->key_names[k]));
            out_vecs[k] = vector(ctx.key_types[k], 0);
            AS_LIST(out_vals)[k] = out_vecs[k];
        }
        AS_I64(out_names)[cfg->key_count] = symbols_intern(cfg->value_name, (i64_t)strlen(cfg->value_name));
        out_sum = vector(ctx.value_type, 0);
        AS_LIST(out_vals)[cfg->key_count] = out_sum;
        return table(out_names, out_vals);
    }

    ctx.hashes = (u64_t *)heap_alloc(rows * sizeof(u64_t));
    ctx.row_ids = (i64_t *)heap_alloc(rows * sizeof(i64_t));
    partition_counts = (u64_t *)heap_alloc(HASHAGG_PARTITIONS * sizeof(u64_t));
    partition_offsets = (i64_t *)heap_alloc((HASHAGG_PARTITIONS + 1) * sizeof(i64_t));
    parts = (hashagg_partition_t *)heap_alloc(HASHAGG_PARTITIONS * sizeof(hashagg_partition_t));

    if (!ctx.hashes || !ctx.row_ids || !partition_counts || !partition_offsets || !parts) {
        err = err_user("hashagg: allocation failed");
        goto cleanup;
    }

    {
        pool_p pool = pool_get();
        i64_t executors = pool ? pool_get_executors_count(pool) : 1;
        i64_t chunk_size = rows;
        i64_t chunks = 1;

        if (pool && rows >= HASHAGG_SPLIT_THRESHOLD && executors > 1) {
            chunk_size = pool_chunk_aligned(rows, executors, sizeof(u64_t));
            if (chunk_size <= 0)
                chunk_size = rows;
            chunks = (rows + chunk_size - 1) / chunk_size;
        }

        chunk_counts = (u64_t *)heap_alloc(chunks * HASHAGG_PARTITIONS * sizeof(u64_t));
        chunk_offsets = (i64_t *)heap_alloc(chunks * HASHAGG_PARTITIONS * sizeof(i64_t));
        scatter_positions = (i64_t *)heap_alloc(chunks * HASHAGG_PARTITIONS * sizeof(i64_t));
        chunk_mins = (i64_t *)heap_alloc(chunks * ctx.key_count * sizeof(i64_t));
        chunk_maxs = (i64_t *)heap_alloc(chunks * ctx.key_count * sizeof(i64_t));
        chunk_has_null = (b8_t *)heap_alloc(chunks * sizeof(b8_t));

        if (!chunk_counts || !chunk_offsets || !scatter_positions || !chunk_mins || !chunk_maxs || !chunk_has_null) {
            err = err_user("hashagg: allocation failed");
            goto cleanup;
        }

        memset(chunk_counts, 0, chunks * HASHAGG_PARTITIONS * sizeof(u64_t));
        for (i64_t c = 0; c < chunks; c++) {
            i64_t *cmins = chunk_mins + c * ctx.key_count;
            i64_t *cmaxs = chunk_maxs + c * ctx.key_count;

            for (i64_t k = 0; k < ctx.key_count; k++) {
                cmins[k] = LLONG_MAX;
                cmaxs[k] = LLONG_MIN;
            }
            chunk_has_null[c] = B8_FALSE;
        }

        gettimeofday(&t0, NULL);
        if (chunks > 1) {
            pool_prepare(pool);
            for (i64_t c = 0; c < chunks; c++) {
                i64_t offset = c * chunk_size;
                i64_t len = chunk_size;
                if (offset + len > rows)
                    len = rows - offset;
                pool_add_task(pool, (raw_p)hashagg_hash_task, 7, (raw_p)len, (raw_p)offset, &ctx,
                              (raw_p)(chunk_counts + c * HASHAGG_PARTITIONS),
                              (raw_p)(chunk_mins + c * ctx.key_count),
                              (raw_p)(chunk_maxs + c * ctx.key_count), (raw_p)(chunk_has_null + c));
            }
            obj_p res = pool_run(pool);
            drop_obj(res);
        } else {
            hashagg_hash_task((raw_p)rows, (raw_p)0, &ctx, (raw_p)chunk_counts, (raw_p)chunk_mins,
                              (raw_p)chunk_maxs, (raw_p)chunk_has_null);
        }
        gettimeofday(&t1, NULL);
        timings->hash_ms = hashagg_elapsed_ms(t0, t1);

        for (i64_t k = 0; k < ctx.key_count; k++) {
            mins[k] = LLONG_MAX;
            maxs[k] = LLONG_MIN;
        }
        for (i64_t c = 0; c < chunks; c++) {
            if (chunk_has_null[c])
                perfect_ok = B8_FALSE;
        }
        for (i64_t k = 0; k < ctx.key_count; k++) {
            for (i64_t c = 0; c < chunks; c++) {
                i64_t vmin = chunk_mins[c * ctx.key_count + k];
                i64_t vmax = chunk_maxs[c * ctx.key_count + k];
                if (vmin < mins[k])
                    mins[k] = vmin;
                if (vmax > maxs[k])
                    maxs[k] = vmax;
            }
        }

        perfect_ok = B8_TRUE;
        for (i64_t c = 0; c < chunks; c++) {
            if (chunk_has_null[c]) {
                perfect_ok = B8_FALSE;
                break;
            }
        }

        if (perfect_ok) {
            __int128 prod = 1;
            for (i64_t k = 0; k < ctx.key_count; k++) {
                if (mins[k] == LLONG_MAX || maxs[k] == LLONG_MIN || maxs[k] < mins[k]) {
                    perfect_ok = B8_FALSE;
                    break;
                }
                __int128 diff = (__int128)maxs[k] - (__int128)mins[k] + 1;
                if (diff <= 0 || diff > UINT64_MAX) {
                    perfect_ok = B8_FALSE;
                    break;
                }
                ranges[k] = (u64_t)diff;
                prod *= diff;
                if (prod > UINT64_MAX) {
                    perfect_ok = B8_FALSE;
                    break;
                }
            }
            if (perfect_ok)
                cardinality = (u64_t)prod;
        }

        if (perfect_ok && cardinality > 0) {
            obj_p perfect_res = NULL_OBJ;
            if (hashagg_build_perfect(&perfect_res, cfg, &ctx, mins, ranges, cardinality, timings)) {
                result = perfect_res;
                goto cleanup;
            }
        }

        gettimeofday(&t0, NULL);
        memset(partition_counts, 0, HASHAGG_PARTITIONS * sizeof(u64_t));
        for (i64_t p = 0; p < HASHAGG_PARTITIONS; p++) {
            u64_t total = 0;
            for (i64_t c = 0; c < chunks; c++) {
                total += chunk_counts[c * HASHAGG_PARTITIONS + p];
            }
            partition_counts[p] = total;
        }

        partition_offsets[0] = 0;
        for (i64_t p = 0; p < HASHAGG_PARTITIONS; p++)
            partition_offsets[p + 1] = partition_offsets[p] + (i64_t)partition_counts[p];

        for (i64_t p = 0; p < HASHAGG_PARTITIONS; p++) {
            i64_t pos = partition_offsets[p];
            for (i64_t c = 0; c < chunks; c++) {
                i64_t idx = c * HASHAGG_PARTITIONS + p;
                chunk_offsets[idx] = pos;
                pos += (i64_t)chunk_counts[idx];
            }
        }

        memcpy(scatter_positions, chunk_offsets, chunks * HASHAGG_PARTITIONS * sizeof(i64_t));
        gettimeofday(&t1, NULL);
        timings->prefix_ms = hashagg_elapsed_ms(t0, t1);

        gettimeofday(&t0, NULL);
        if (chunks > 1) {
            pool_prepare(pool);
            for (i64_t c = 0; c < chunks; c++) {
                i64_t offset = c * chunk_size;
                i64_t len = chunk_size;
                if (offset + len > rows)
                    len = rows - offset;
                pool_add_task(pool, (raw_p)hashagg_scatter_task, 4, (raw_p)len, (raw_p)offset, &ctx,
                              (raw_p)(scatter_positions + c * HASHAGG_PARTITIONS));
            }
            obj_p res = pool_run(pool);
            drop_obj(res);
        } else {
            hashagg_scatter_task((raw_p)rows, (raw_p)0, &ctx, (raw_p)scatter_positions);
        }
        gettimeofday(&t1, NULL);
        timings->scatter_ms = hashagg_elapsed_ms(t0, t1);
    }

    gettimeofday(&t0, NULL);
    for (i64_t p = 0; p < HASHAGG_PARTITIONS; p++) {
        i64_t count = (i64_t)partition_counts[p];
        i64_t capacity;
        i64_t row_size;

        parts[p].row_start = partition_offsets[p];
        parts[p].row_count = count;
        parts[p].group_count = 0;
        parts[p].entries = NULL;
        parts[p].rows = NULL;
        parts[p].capacity = 0;
        parts[p].bitmask = 0;
        parts[p].row_size = 0;

        if (count == 0)
            continue;

        capacity = next_power_of_two_u64((i64_t)(count * HASHAGG_LOAD_FACTOR));
        if (capacity < HASHAGG_HT_MIN_CAP)
            capacity = HASHAGG_HT_MIN_CAP;

        row_size = (i64_t)(ctx.key_count * sizeof(i64_t));
        row_size += (ctx.value_type == TYPE_F64) ? (i64_t)sizeof(f64_t) : (i64_t)sizeof(i64_t);

        parts[p].capacity = capacity;
        parts[p].bitmask = (u64_t)(capacity - 1);
        parts[p].row_size = row_size;
        parts[p].entries = (u64_t *)heap_alloc(capacity * sizeof(u64_t));
        parts[p].rows = (u8_t *)heap_alloc(count * row_size);

        if (!parts[p].entries || !parts[p].rows) {
            err = err_user("hashagg: allocation failed");
            goto cleanup;
        }

        memset(parts[p].entries, 0, capacity * sizeof(u64_t));
    }
    gettimeofday(&t1, NULL);
    timings->ht_alloc_ms = hashagg_elapsed_ms(t0, t1);

    task_ctx.ctx = &ctx;
    task_ctx.parts = parts;

    {
        pool_p pool = pool_get();
        gettimeofday(&t0, NULL);
        if (pool && rows >= HASHAGG_SPLIT_THRESHOLD && pool_get_executors_count(pool) > 1) {
            pool_prepare(pool);
            for (i64_t p = 0; p < HASHAGG_PARTITIONS; p++) {
                pool_add_task(pool, (raw_p)hashagg_partition_task, 3, (raw_p)p, (raw_p)(p + 1), &task_ctx);
            }
            obj_p res = pool_run(pool);
            drop_obj(res);
        } else {
            for (i64_t p = 0; p < HASHAGG_PARTITIONS; p++) {
                hashagg_partition_task((raw_p)p, (raw_p)(p + 1), &task_ctx);
            }
        }
        gettimeofday(&t1, NULL);
        timings->ht_build_ms = hashagg_elapsed_ms(t0, t1);
    }

    for (i64_t p = 0; p < HASHAGG_PARTITIONS; p++)
        total_groups += parts[p].group_count;

    gettimeofday(&t0, NULL);
    group_offsets = (i64_t *)heap_alloc((HASHAGG_PARTITIONS + 1) * sizeof(i64_t));
    if (!group_offsets) {
        err = err_user("hashagg: allocation failed");
        goto cleanup;
    }
    group_offsets[0] = 0;
    for (i64_t p = 0; p < HASHAGG_PARTITIONS; p++)
        group_offsets[p + 1] = group_offsets[p] + parts[p].group_count;

    out_names = SYMBOL(cfg->key_count + 1);
    out_vals = LIST(cfg->key_count + 1);

    for (i64_t k = 0; k < cfg->key_count; k++) {
        AS_I64(out_names)[k] = symbols_intern(cfg->key_names[k], (i64_t)strlen(cfg->key_names[k]));
        out_vecs[k] = vector(ctx.key_types[k], total_groups);
        AS_LIST(out_vals)[k] = out_vecs[k];
    }

    AS_I64(out_names)[cfg->key_count] = symbols_intern(cfg->value_name, (i64_t)strlen(cfg->value_name));
    out_sum = vector(ctx.value_type, total_groups);
    AS_LIST(out_vals)[cfg->key_count] = out_sum;

    for (i64_t k = 0; k < cfg->key_count; k++)
        out_ctx.out_keys[k] = AS_I64(out_vecs[k]);
    out_ctx.out_sum_i64 = (ctx.value_type == TYPE_I64) ? AS_I64(out_sum) : NULL;
    out_ctx.out_sum_f64 = (ctx.value_type == TYPE_F64) ? AS_F64(out_sum) : NULL;
    out_ctx.parts = parts;
    out_ctx.group_offsets = group_offsets;
    out_ctx.value_type = ctx.value_type;
    out_ctx.key_count = ctx.key_count;

    {
        pool_p pool = pool_get();
        if (pool && total_groups >= HASHAGG_SPLIT_THRESHOLD && pool_get_executors_count(pool) > 1) {
            pool_prepare(pool);
            for (i64_t p = 0; p < HASHAGG_PARTITIONS; p++) {
                pool_add_task(pool, (raw_p)hashagg_output_task, 3, (raw_p)p, (raw_p)(p + 1), &out_ctx);
            }
            obj_p res = pool_run(pool);
            drop_obj(res);
        } else {
            for (i64_t p = 0; p < HASHAGG_PARTITIONS; p++) {
                hashagg_output_task((raw_p)p, (raw_p)(p + 1), &out_ctx);
            }
        }
    }
    gettimeofday(&t1, NULL);
    timings->output_ms = hashagg_elapsed_ms(t0, t1);

    result = table(out_names, out_vals);

cleanup:
    if (parts) {
        for (i64_t p = 0; p < HASHAGG_PARTITIONS; p++) {
            if (parts[p].entries)
                heap_free(parts[p].entries);
            if (parts[p].rows)
                heap_free(parts[p].rows);
        }
    }
    if (parts)
        heap_free(parts);
    if (scatter_positions)
        heap_free(scatter_positions);
    if (group_offsets)
        heap_free(group_offsets);
    if (chunk_has_null)
        heap_free(chunk_has_null);
    if (chunk_maxs)
        heap_free(chunk_maxs);
    if (chunk_mins)
        heap_free(chunk_mins);
    if (chunk_offsets)
        heap_free(chunk_offsets);
    if (chunk_counts)
        heap_free(chunk_counts);
    if (partition_offsets)
        heap_free(partition_offsets);
    if (partition_counts)
        heap_free(partition_counts);
    if (ctx.row_ids)
        heap_free(ctx.row_ids);
    if (ctx.hashes)
        heap_free(ctx.hashes);

    if (err != NULL_OBJ) {
        if (out_vals)
            drop_obj(out_vals);
        if (out_names)
            drop_obj(out_names);
        return err;
    }

    return result;
}
