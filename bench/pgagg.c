/*
 * Standalone parallel grouped aggregation benchmark (DuckDB-inspired).
 * Focus: grouped SUM on v3 by id1..id6 for the G1_1e7_1e2_0_0 dataset.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "../core/rayforce.h"
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

#define PGAGG_KEY_COUNT 6
#define PGAGG_VALUE_NAME "v3"
#define PGAGG_RADIX_BITS 8
#define PGAGG_PARTITIONS (1u << PGAGG_RADIX_BITS)
#define PGAGG_LOAD_FACTOR 1.5
#define PGAGG_HT_MIN_CAP 1024
#define PGAGG_SPLIT_THRESHOLD (RAY_PAGE_SIZE * 4)

#define PGAGG_SALT_MASK 0xFFFF000000000000ULL
#define PGAGG_POINTER_MASK 0x0000FFFFFFFFFFFFULL

#define PGAGG_RADIX_SHIFT (64 - 16 - PGAGG_RADIX_BITS)

#define PGAGG_CSV_PATH "../rayforce-bench/datasets/G1_1e7_1e2_0_0/G1_1e7_1e2_0_0.csv"

typedef union {
    i64_t i64;
    f64_t f64;
} pgagg_sum_t;

typedef struct {
    i64_t row_start;
    i64_t row_count;
    u64_t *entries;
    i64_t capacity;
    u64_t bitmask;
    u8_t *rows;
    i64_t row_size;
    i64_t group_count;
} pgagg_partition_t;

typedef struct {
    i64_t row_count;
    i64_t *row_ids;
    u64_t *hashes;
    i64_t *key_data[PGAGG_KEY_COUNT];
    i8_t key_types[PGAGG_KEY_COUNT];
    i8_t value_type;
    i64_t *value_i64;
    f64_t *value_f64;
} pgagg_context_t;

typedef struct {
    pgagg_context_t *ctx;
    pgagg_partition_t *parts;
} pgagg_task_ctx_t;

typedef struct {
    double hash_ms;
    double prefix_ms;
    double scatter_ms;
    double ht_alloc_ms;
    double ht_build_ms;
    double output_ms;
} pgagg_timings_t;

typedef struct {
    pgagg_partition_t *parts;
    i64_t *group_offsets;
    i64_t *out_keys[PGAGG_KEY_COUNT];
    i64_t *out_sum_i64;
    f64_t *out_sum_f64;
    i8_t value_type;
} pgagg_output_ctx_t;

obj_p pgagg_partition_task(raw_p arg1, raw_p arg2, raw_p arg3);
obj_p pgagg_hash_task(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4);
obj_p pgagg_scatter_task(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4);
obj_p pgagg_output_task(raw_p arg1, raw_p arg2, raw_p arg3);

static inline double pgagg_elapsed_ms(struct timeval start, struct timeval end) {
    double ms = (end.tv_sec - start.tv_sec) * 1000.0;
    ms += (end.tv_usec - start.tv_usec) / 1000.0;
    return ms;
}

static inline u64_t pgagg_hash_row(const pgagg_context_t *ctx, i64_t row) {
    u64_t h = U64_HASH_SEED;
    for (i64_t k = 0; k < PGAGG_KEY_COUNT; k++) {
        u64_t v = (u64_t)ctx->key_data[k][row];
        h = hash_index_u64(h, v);
    }
    return h;
}

static inline u64_t pgagg_radix_partition(u64_t hash) {
    return (hash >> PGAGG_RADIX_SHIFT) & (PGAGG_PARTITIONS - 1);
}

static inline u64_t pgagg_salt(u64_t hash) { return hash & PGAGG_SALT_MASK; }

static inline u64_t pgagg_probe_next(u64_t offset, u64_t hash, u64_t bitmask) {
    static const u64_t increment_bits = 5;
    u64_t increment = (hash >> (64 - increment_bits)) | 1ULL;
    return (offset + increment) & bitmask;
}

static inline b8_t pgagg_keys_match(const pgagg_context_t *ctx, const u8_t *row_ptr, i64_t row_idx) {
    const i64_t *keys = (const i64_t *)row_ptr;

    return keys[0] == ctx->key_data[0][row_idx] && keys[1] == ctx->key_data[1][row_idx]
           && keys[2] == ctx->key_data[2][row_idx] && keys[3] == ctx->key_data[3][row_idx]
           && keys[4] == ctx->key_data[4][row_idx] && keys[5] == ctx->key_data[5][row_idx];
}

static i64_t pgagg_find_symbol_idx(obj_p symbols, i64_t sym) {
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

static obj_p pgagg_read_csv(void) {
    static const char *type_names[] = {"SYMBOL", "SYMBOL", "SYMBOL", "I64", "I64", "I64", "I64", "I64", "F64"};
    const i64_t type_count = (i64_t)(sizeof(type_names) / sizeof(type_names[0]));
    obj_p types = SYMBOL(type_count);
    obj_p path = cstring_from_str(PGAGG_CSV_PATH, (i64_t)strlen(PGAGG_CSV_PATH));
    obj_p args[2];
    obj_p res;

    for (i64_t i = 0; i < type_count; i++) {
        AS_I64(types)[i] = symbols_intern(type_names[i], (i64_t)strlen(type_names[i]));
    }

    args[0] = types;
    args[1] = path;
    res = ray_read_csv(args, 2);

    drop_obj(types);
    drop_obj(path);

    return res;
}

static obj_p pgagg_group_sum(obj_p input_table) {
    static const char *key_names[PGAGG_KEY_COUNT] = {"id1", "id2", "id3", "id4", "id5", "id6"};
    obj_p colnames, cols;
    obj_p value_col;
    pgagg_context_t ctx = {0};
    pgagg_partition_t *parts = NULL;
    pgagg_task_ctx_t task_ctx;
    i64_t value_idx;
    i64_t rows;
    i64_t total_groups = 0;
    u64_t *partition_counts = NULL;
    i64_t *partition_offsets = NULL;
    u64_t *chunk_counts = NULL;
    i64_t *chunk_offsets = NULL;
    i64_t *scatter_positions = NULL;
    i64_t *group_offsets = NULL;
    obj_p out_names = NULL_OBJ;
    obj_p out_vals = NULL_OBJ;
    obj_p out_vecs[PGAGG_KEY_COUNT];
    obj_p out_sum = NULL_OBJ;
    obj_p result = NULL_OBJ;
    obj_p err = NULL_OBJ;
    pgagg_timings_t timings = {0};
    struct timeval t0, t1;
    pgagg_output_ctx_t out_ctx;

    if (input_table->type != TYPE_TABLE)
        return err_type(TYPE_TABLE, input_table->type, 0, 0);

    colnames = AS_LIST(input_table)[0];
    cols = AS_LIST(input_table)[1];
    rows = ops_count(input_table);

    for (i64_t k = 0; k < PGAGG_KEY_COUNT; k++) {
        i64_t sym = symbols_intern(key_names[k], (i64_t)strlen(key_names[k]));
        i64_t idx = pgagg_find_symbol_idx(colnames, sym);
        obj_p col;

        if (idx == NULL_I64)
            return err_value(sym);

        col = AS_LIST(cols)[idx];
        if (col->type != TYPE_SYMBOL && col->type != TYPE_I64)
            return err_type(TYPE_SYMBOL, col->type, 0, 0);

        ctx.key_types[k] = col->type;
        ctx.key_data[k] = AS_I64(col);
    }

    value_idx = pgagg_find_symbol_idx(colnames, symbols_intern(PGAGG_VALUE_NAME, 2));
    if (value_idx == NULL_I64)
        return err_value(symbols_intern(PGAGG_VALUE_NAME, 2));

    value_col = AS_LIST(cols)[value_idx];
    if (value_col->type != TYPE_F64 && value_col->type != TYPE_I64)
        return err_type(TYPE_F64, value_col->type, 0, 0);

    ctx.value_type = value_col->type;
    ctx.value_f64 = (value_col->type == TYPE_F64) ? AS_F64(value_col) : NULL;
    ctx.value_i64 = (value_col->type == TYPE_I64) ? AS_I64(value_col) : NULL;
    ctx.row_count = rows;

    if (rows == 0) {
        out_names = SYMBOL(PGAGG_KEY_COUNT + 1);
        out_vals = LIST(PGAGG_KEY_COUNT + 1);
        for (i64_t k = 0; k < PGAGG_KEY_COUNT; k++) {
            AS_I64(out_names)[k] = symbols_intern(key_names[k], (i64_t)strlen(key_names[k]));
            out_vecs[k] = vector(ctx.key_types[k], 0);
            AS_LIST(out_vals)[k] = out_vecs[k];
        }
        AS_I64(out_names)[PGAGG_KEY_COUNT] = symbols_intern(PGAGG_VALUE_NAME, 2);
        out_sum = vector(ctx.value_type, 0);
        AS_LIST(out_vals)[PGAGG_KEY_COUNT] = out_sum;
        return table(out_names, out_vals);
    }

    ctx.hashes = (u64_t *)heap_alloc(rows * sizeof(u64_t));
    ctx.row_ids = (i64_t *)heap_alloc(rows * sizeof(i64_t));
    partition_counts = (u64_t *)heap_alloc(PGAGG_PARTITIONS * sizeof(u64_t));
    partition_offsets = (i64_t *)heap_alloc((PGAGG_PARTITIONS + 1) * sizeof(i64_t));
    parts = (pgagg_partition_t *)heap_alloc(PGAGG_PARTITIONS * sizeof(pgagg_partition_t));

    if (!ctx.hashes || !ctx.row_ids || !partition_counts || !partition_offsets || !parts) {
        err = err_user("pgagg: allocation failed");
        goto cleanup;
    }

    {
        pool_p pool = pool_get();
        i64_t executors = pool ? pool_get_executors_count(pool) : 1;
        i64_t chunk_size = rows;
        i64_t chunks = 1;

        if (pool && rows >= PGAGG_SPLIT_THRESHOLD && executors > 1) {
            chunk_size = pool_chunk_aligned(rows, executors, sizeof(u64_t));
            if (chunk_size <= 0)
                chunk_size = rows;
            chunks = (rows + chunk_size - 1) / chunk_size;
        }

        chunk_counts = (u64_t *)heap_alloc(chunks * PGAGG_PARTITIONS * sizeof(u64_t));
        chunk_offsets = (i64_t *)heap_alloc(chunks * PGAGG_PARTITIONS * sizeof(i64_t));
        scatter_positions = (i64_t *)heap_alloc(chunks * PGAGG_PARTITIONS * sizeof(i64_t));

        if (!chunk_counts || !chunk_offsets || !scatter_positions) {
            err = err_user("pgagg: allocation failed");
            goto cleanup;
        }

        memset(chunk_counts, 0, chunks * PGAGG_PARTITIONS * sizeof(u64_t));

        gettimeofday(&t0, NULL);
        if (chunks > 1) {
            pool_prepare(pool);
            for (i64_t c = 0; c < chunks; c++) {
                i64_t offset = c * chunk_size;
                i64_t len = chunk_size;
                if (offset + len > rows)
                    len = rows - offset;
                pool_add_task(pool, (raw_p)pgagg_hash_task, 4, (raw_p)len, (raw_p)offset, &ctx,
                              (raw_p)(chunk_counts + c * PGAGG_PARTITIONS));
            }
            obj_p res = pool_run(pool);
            drop_obj(res);
        } else {
            pgagg_hash_task((raw_p)rows, (raw_p)0, &ctx, (raw_p)chunk_counts);
        }
        gettimeofday(&t1, NULL);
        timings.hash_ms = pgagg_elapsed_ms(t0, t1);

        gettimeofday(&t0, NULL);
        memset(partition_counts, 0, PGAGG_PARTITIONS * sizeof(u64_t));
        for (i64_t p = 0; p < PGAGG_PARTITIONS; p++) {
            u64_t total = 0;
            for (i64_t c = 0; c < chunks; c++) {
                total += chunk_counts[c * PGAGG_PARTITIONS + p];
            }
            partition_counts[p] = total;
        }

        partition_offsets[0] = 0;
        for (i64_t p = 0; p < PGAGG_PARTITIONS; p++)
            partition_offsets[p + 1] = partition_offsets[p] + (i64_t)partition_counts[p];

        for (i64_t p = 0; p < PGAGG_PARTITIONS; p++) {
            i64_t pos = partition_offsets[p];
            for (i64_t c = 0; c < chunks; c++) {
                i64_t idx = c * PGAGG_PARTITIONS + p;
                chunk_offsets[idx] = pos;
                pos += (i64_t)chunk_counts[idx];
            }
        }

        memcpy(scatter_positions, chunk_offsets, chunks * PGAGG_PARTITIONS * sizeof(i64_t));
        gettimeofday(&t1, NULL);
        timings.prefix_ms = pgagg_elapsed_ms(t0, t1);

        gettimeofday(&t0, NULL);
        if (chunks > 1) {
            pool_prepare(pool);
            for (i64_t c = 0; c < chunks; c++) {
                i64_t offset = c * chunk_size;
                i64_t len = chunk_size;
                if (offset + len > rows)
                    len = rows - offset;
                pool_add_task(pool, (raw_p)pgagg_scatter_task, 4, (raw_p)len, (raw_p)offset, &ctx,
                              (raw_p)(scatter_positions + c * PGAGG_PARTITIONS));
            }
            obj_p res = pool_run(pool);
            drop_obj(res);
        } else {
            pgagg_scatter_task((raw_p)rows, (raw_p)0, &ctx, (raw_p)scatter_positions);
        }
        gettimeofday(&t1, NULL);
        timings.scatter_ms = pgagg_elapsed_ms(t0, t1);
    }

    gettimeofday(&t0, NULL);
    for (i64_t p = 0; p < PGAGG_PARTITIONS; p++) {
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

        capacity = next_power_of_two_u64((i64_t)(count * PGAGG_LOAD_FACTOR));
        if (capacity < PGAGG_HT_MIN_CAP)
            capacity = PGAGG_HT_MIN_CAP;

        row_size = (i64_t)(PGAGG_KEY_COUNT * sizeof(i64_t));
        row_size += (ctx.value_type == TYPE_F64) ? (i64_t)sizeof(f64_t) : (i64_t)sizeof(i64_t);

        parts[p].capacity = capacity;
        parts[p].bitmask = (u64_t)(capacity - 1);
        parts[p].row_size = row_size;
        parts[p].entries = (u64_t *)heap_alloc(capacity * sizeof(u64_t));
        parts[p].rows = (u8_t *)heap_alloc(count * row_size);

        if (!parts[p].entries || !parts[p].rows) {
            err = err_user("pgagg: allocation failed");
            goto cleanup;
        }

        memset(parts[p].entries, 0, capacity * sizeof(u64_t));
    }
    gettimeofday(&t1, NULL);
    timings.ht_alloc_ms = pgagg_elapsed_ms(t0, t1);

    task_ctx.ctx = &ctx;
    task_ctx.parts = parts;

    {
        pool_p pool = pool_get();
        gettimeofday(&t0, NULL);
        if (pool && rows >= PGAGG_SPLIT_THRESHOLD && pool_get_executors_count(pool) > 1) {
            pool_prepare(pool);
            for (i64_t p = 0; p < PGAGG_PARTITIONS; p++) {
                pool_add_task(pool, (raw_p)pgagg_partition_task, 3, (raw_p)p, (raw_p)(p + 1), &task_ctx);
            }
            obj_p res = pool_run(pool);
            drop_obj(res);
        } else {
            for (i64_t p = 0; p < PGAGG_PARTITIONS; p++) {
                pgagg_partition_task((raw_p)p, (raw_p)(p + 1), &task_ctx);
            }
        }
        gettimeofday(&t1, NULL);
        timings.ht_build_ms = pgagg_elapsed_ms(t0, t1);
    }

    for (i64_t p = 0; p < PGAGG_PARTITIONS; p++)
        total_groups += parts[p].group_count;

    gettimeofday(&t0, NULL);
    group_offsets = (i64_t *)heap_alloc((PGAGG_PARTITIONS + 1) * sizeof(i64_t));
    if (!group_offsets) {
        err = err_user("pgagg: allocation failed");
        goto cleanup;
    }
    group_offsets[0] = 0;
    for (i64_t p = 0; p < PGAGG_PARTITIONS; p++)
        group_offsets[p + 1] = group_offsets[p] + parts[p].group_count;

    out_names = SYMBOL(PGAGG_KEY_COUNT + 1);
    out_vals = LIST(PGAGG_KEY_COUNT + 1);

    for (i64_t k = 0; k < PGAGG_KEY_COUNT; k++) {
        AS_I64(out_names)[k] = symbols_intern(key_names[k], (i64_t)strlen(key_names[k]));
        out_vecs[k] = vector(ctx.key_types[k], total_groups);
        AS_LIST(out_vals)[k] = out_vecs[k];
    }

    AS_I64(out_names)[PGAGG_KEY_COUNT] = symbols_intern(PGAGG_VALUE_NAME, 2);
    out_sum = vector(ctx.value_type, total_groups);
    AS_LIST(out_vals)[PGAGG_KEY_COUNT] = out_sum;

    for (i64_t k = 0; k < PGAGG_KEY_COUNT; k++)
        out_ctx.out_keys[k] = AS_I64(out_vecs[k]);
    out_ctx.out_sum_i64 = (ctx.value_type == TYPE_I64) ? AS_I64(out_sum) : NULL;
    out_ctx.out_sum_f64 = (ctx.value_type == TYPE_F64) ? AS_F64(out_sum) : NULL;
    out_ctx.parts = parts;
    out_ctx.group_offsets = group_offsets;
    out_ctx.value_type = ctx.value_type;

    {
        pool_p pool = pool_get();
        if (pool && total_groups >= PGAGG_SPLIT_THRESHOLD && pool_get_executors_count(pool) > 1) {
            pool_prepare(pool);
            for (i64_t p = 0; p < PGAGG_PARTITIONS; p++) {
                pool_add_task(pool, (raw_p)pgagg_output_task, 3, (raw_p)p, (raw_p)(p + 1), &out_ctx);
            }
            obj_p res = pool_run(pool);
            drop_obj(res);
        } else {
            for (i64_t p = 0; p < PGAGG_PARTITIONS; p++) {
                pgagg_output_task((raw_p)p, (raw_p)(p + 1), &out_ctx);
            }
        }
    }
    gettimeofday(&t1, NULL);
    timings.output_ms = pgagg_elapsed_ms(t0, t1);

    result = table(out_names, out_vals);
    {
        double total = timings.hash_ms + timings.prefix_ms + timings.scatter_ms + timings.ht_alloc_ms +
                       timings.ht_build_ms + timings.output_ms;
        printf("pgagg phases ms: hash=%.3f prefix=%.3f scatter=%.3f ht_alloc=%.3f ht_build=%.3f output=%.3f total=%.3f\n",
               timings.hash_ms, timings.prefix_ms, timings.scatter_ms, timings.ht_alloc_ms, timings.ht_build_ms,
               timings.output_ms, total);
    }

cleanup:
    if (parts) {
        for (i64_t p = 0; p < PGAGG_PARTITIONS; p++) {
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

obj_p pgagg_hash_task(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4) {
    i64_t len = (i64_t)arg1;
    i64_t offset = (i64_t)arg2;
    pgagg_context_t *ctx = (pgagg_context_t *)arg3;
    u64_t *hist = (u64_t *)arg4;

    for (i64_t i = 0; i < len; i++) {
        i64_t row = offset + i;
        u64_t hash = pgagg_hash_row(ctx, row);
        u64_t part = pgagg_radix_partition(hash);

        ctx->hashes[row] = hash;
        hist[part]++;
    }

    return NULL_OBJ;
}

obj_p pgagg_scatter_task(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4) {
    i64_t len = (i64_t)arg1;
    i64_t offset = (i64_t)arg2;
    pgagg_context_t *ctx = (pgagg_context_t *)arg3;
    i64_t *scatter = (i64_t *)arg4;

    for (i64_t i = 0; i < len; i++) {
        i64_t row = offset + i;
        u64_t part = pgagg_radix_partition(ctx->hashes[row]);
        i64_t pos = scatter[part]++;
        ctx->row_ids[pos] = row;
    }

    return NULL_OBJ;
}

obj_p pgagg_output_task(raw_p arg1, raw_p arg2, raw_p arg3) {
    i64_t start = (i64_t)arg1;
    i64_t end = (i64_t)arg2;
    pgagg_output_ctx_t *ctx = (pgagg_output_ctx_t *)arg3;

    for (i64_t p = start; p < end; p++) {
        pgagg_partition_t *part = &ctx->parts[p];
        i64_t base = ctx->group_offsets[p];
        const i64_t key_bytes = (i64_t)(PGAGG_KEY_COUNT * sizeof(i64_t));

        for (i64_t g = 0; g < part->group_count; g++) {
            u8_t *row_ptr = part->rows + g * part->row_size;
            i64_t *keys = (i64_t *)row_ptr;
            u8_t *sum_ptr = row_ptr + key_bytes;
            i64_t out_idx = base + g;

            for (i64_t k = 0; k < PGAGG_KEY_COUNT; k++) {
                ctx->out_keys[k][out_idx] = keys[k];
            }

            if (ctx->value_type == TYPE_F64)
                ctx->out_sum_f64[out_idx] = *(f64_t *)sum_ptr;
            else
                ctx->out_sum_i64[out_idx] = *(i64_t *)sum_ptr;
        }
    }

    return NULL_OBJ;
}

obj_p pgagg_partition_task(raw_p arg1, raw_p arg2, raw_p arg3) {
    i64_t start = (i64_t)arg1;
    i64_t end = (i64_t)arg2;
    pgagg_task_ctx_t *task_ctx = (pgagg_task_ctx_t *)arg3;
    pgagg_context_t *ctx = task_ctx->ctx;

    for (i64_t p = start; p < end; p++) {
        pgagg_partition_t *part = &task_ctx->parts[p];
        i64_t offset = part->row_start;
        i64_t limit = part->row_start + part->row_count;
        const i64_t key_bytes = (i64_t)(PGAGG_KEY_COUNT * sizeof(i64_t));

        if (part->row_count == 0)
            continue;

        for (i64_t i = offset; i < limit; i++) {
            i64_t row_idx = ctx->row_ids[i];
            u64_t hash = ctx->hashes[row_idx];
            u64_t salt = pgagg_salt(hash);
            u64_t ht_offset = hash & part->bitmask;

            for (;;) {
                u64_t entry = part->entries[ht_offset];

                if (entry == 0) {
                    i64_t group_idx = part->group_count++;
                    u8_t *row_ptr = part->rows + group_idx * part->row_size;
                    i64_t *keys = (i64_t *)row_ptr;
                    u8_t *sum_ptr = row_ptr + key_bytes;

                    for (i64_t k = 0; k < PGAGG_KEY_COUNT; k++) {
                        keys[k] = ctx->key_data[k][row_idx];
                    }

                    if (ctx->value_type == TYPE_F64) {
                        *(f64_t *)sum_ptr = 0.0;
                        *(f64_t *)sum_ptr = ADDF64(*(f64_t *)sum_ptr, ctx->value_f64[row_idx]);
                    } else {
                        *(i64_t *)sum_ptr = 0;
                        *(i64_t *)sum_ptr = ADDI64(*(i64_t *)sum_ptr, ctx->value_i64[row_idx]);
                    }

                    part->entries[ht_offset] = (salt & PGAGG_SALT_MASK) | (((u64_t)row_ptr) & PGAGG_POINTER_MASK);
                    break;
                }

                if ((entry & PGAGG_SALT_MASK) == (salt & PGAGG_SALT_MASK)) {
                    u8_t *row_ptr = (u8_t *)(entry & PGAGG_POINTER_MASK);
                    u8_t *sum_ptr = row_ptr + key_bytes;

                    if (pgagg_keys_match(ctx, row_ptr, row_idx)) {
                        if (ctx->value_type == TYPE_F64)
                            *(f64_t *)sum_ptr = ADDF64(*(f64_t *)sum_ptr, ctx->value_f64[row_idx]);
                        else
                            *(i64_t *)sum_ptr = ADDI64(*(i64_t *)sum_ptr, ctx->value_i64[row_idx]);
                        break;
                    }
                }

                ht_offset = pgagg_probe_next(ht_offset, hash, part->bitmask);
            }
        }
    }

    return NULL_OBJ;
}

int main(void) {
    obj_p table;
    obj_p result;
    struct timeval start, end;
    double elapsed_ms;
    str_p argv[] = {"pgagg", "-c", "0"};

    runtime_create(3, argv);

    table = pgagg_read_csv();
    if (IS_ERR(table)) {
        obj_p formatted = obj_fmt(table, 1);
        fprintf(stderr, "pgagg read-csv error: %s\n", AS_C8(formatted));
        drop_obj(formatted);
        drop_obj(table);
        runtime_destroy();
        return 1;
    }

    gettimeofday(&start, NULL);
    result = pgagg_group_sum(table);
    gettimeofday(&end, NULL);

    if (IS_ERR(result)) {
        obj_p formatted = obj_fmt(result, 1);
        fprintf(stderr, "pgagg group error: %s\n", AS_C8(formatted));
        drop_obj(formatted);
        drop_obj(result);
        drop_obj(table);
        runtime_destroy();
        return 1;
    }

    elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0;
    elapsed_ms += (end.tv_usec - start.tv_usec) / 1000.0;

    printf("pgagg sum v3 by id1..id6: %.3f ms (%lld groups)\n", elapsed_ms, ops_count(result));

    drop_obj(result);
    drop_obj(table);
    runtime_destroy();
    return 0;
}
