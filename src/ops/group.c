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

#include "ops/internal.h"
#include "ops/rowsel.h"

/* ============================================================================
 * Reduction execution
 * ============================================================================ */

typedef struct {
    double sum_f, min_f, max_f, prod_f, first_f, last_f, sum_sq_f;
    int64_t sum_i, min_i, max_i, prod_i, first_i, last_i, sum_sq_i;
    int64_t cnt;
    int64_t null_count;
    bool has_first;
} reduce_acc_t;

static void reduce_acc_init(reduce_acc_t* acc) {
    acc->sum_f = 0; acc->min_f = DBL_MAX; acc->max_f = -DBL_MAX;
    acc->prod_f = 1.0; acc->first_f = 0; acc->last_f = 0; acc->sum_sq_f = 0;
    acc->sum_i = 0; acc->min_i = INT64_MAX; acc->max_i = INT64_MIN;
    acc->prod_i = 1; acc->first_i = 0; acc->last_i = 0; acc->sum_sq_i = 0;
    acc->cnt = 0; acc->null_count = 0; acc->has_first = false;
}

/* Integer reduction loop — reads native type T, accumulates as i64 */
#define REDUCE_LOOP_I(T, base, start, end, acc, has_nulls, null_bm) \
    do { \
        const T* d = (const T*)(base); \
        for (int64_t row = start; row < end; row++) { \
            if (has_nulls && (null_bm[row/8] >> (row%8)) & 1) { (acc)->null_count++; continue; } \
            int64_t v = (int64_t)d[row]; \
            /* sum/sum_sq may overflow on signed arithmetic — use defined \
             * unsigned wrap (same semantic, no UBSan whine). */ \
            (acc)->sum_i    = (int64_t)((uint64_t)(acc)->sum_i    + (uint64_t)v); \
            (acc)->sum_sq_i = (int64_t)((uint64_t)(acc)->sum_sq_i + (uint64_t)v * (uint64_t)v); \
            (acc)->prod_i   = (int64_t)((uint64_t)(acc)->prod_i   * (uint64_t)v); \
            if (v < (acc)->min_i) (acc)->min_i = v; \
            if (v > (acc)->max_i) (acc)->max_i = v; \
            if (!(acc)->has_first) { (acc)->first_i = v; (acc)->has_first = true; } \
            (acc)->last_i = v; (acc)->cnt++; \
        } \
    } while (0)

/* Float reduction loop */
#define REDUCE_LOOP_F(base, start, end, acc, has_nulls, null_bm) \
    do { \
        const double* d = (const double*)(base); \
        for (int64_t row = start; row < end; row++) { \
            if (has_nulls && (null_bm[row/8] >> (row%8)) & 1) { (acc)->null_count++; continue; } \
            double v = d[row]; \
            (acc)->sum_f += v; (acc)->sum_sq_f += v * v; (acc)->prod_f *= v; \
            if (v < (acc)->min_f) (acc)->min_f = v; \
            if (v > (acc)->max_f) (acc)->max_f = v; \
            if (!(acc)->has_first) { (acc)->first_f = v; (acc)->has_first = true; } \
            (acc)->last_f = v; (acc)->cnt++; \
        } \
    } while (0)

static void reduce_range(ray_t* input, int64_t start, int64_t end,
                         reduce_acc_t* acc, bool has_nulls,
                         const uint8_t* null_bm) {
    void* base = ray_data(input);
    switch (input->type) {
    case RAY_BOOL: case RAY_U8:
        REDUCE_LOOP_I(uint8_t, base, start, end, acc, has_nulls, null_bm); break;
    case RAY_I16:
        REDUCE_LOOP_I(int16_t, base, start, end, acc, has_nulls, null_bm); break;
    case RAY_I32: case RAY_DATE: case RAY_TIME:
        REDUCE_LOOP_I(int32_t, base, start, end, acc, has_nulls, null_bm); break;
    case RAY_I64: case RAY_TIMESTAMP:
        REDUCE_LOOP_I(int64_t, base, start, end, acc, has_nulls, null_bm); break;
    case RAY_F64:
        REDUCE_LOOP_F(base, start, end, acc, has_nulls, null_bm); break;
    case RAY_SYM: {
        /* Adaptive-width SYM columns — use read_col_i64 */
        for (int64_t row = start; row < end; row++) {
            if (has_nulls && (null_bm[row/8] >> (row%8)) & 1) { acc->null_count++; continue; }
            int64_t v = read_col_i64(base, row, input->type, input->attrs);
            acc->sum_i += v; acc->sum_sq_i += v * v;
            acc->prod_i = (int64_t)((uint64_t)acc->prod_i * (uint64_t)v);
            if (v < acc->min_i) acc->min_i = v;
            if (v > acc->max_i) acc->max_i = v;
            if (!acc->has_first) { acc->first_i = v; acc->has_first = true; }
            acc->last_i = v; acc->cnt++;
        }
        break;
    }
    default: break;
    }
}

/* Context for parallel reduction */
typedef struct {
    ray_t*         input;
    reduce_acc_t*  accs;   /* one per worker */
    bool           has_nulls;
    const uint8_t* null_bm;
} par_reduce_ctx_t;

static void par_reduce_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    par_reduce_ctx_t* c = (par_reduce_ctx_t*)ctx;
    reduce_range(c->input, start, end, &c->accs[worker_id],
                 c->has_nulls, c->null_bm);
}

static void reduce_merge(reduce_acc_t* dst, const reduce_acc_t* src, int8_t in_type) {
    if (in_type == RAY_F64) {
        dst->sum_f += src->sum_f;
        dst->sum_sq_f += src->sum_sq_f;
        dst->prod_f *= src->prod_f;
        if (src->min_f < dst->min_f) dst->min_f = src->min_f;
        if (src->max_f > dst->max_f) dst->max_f = src->max_f;
    } else {
        /* Defined unsigned wrap — matches REDUCE_LOOP_I's per-row path. */
        dst->sum_i    = (int64_t)((uint64_t)dst->sum_i    + (uint64_t)src->sum_i);
        dst->sum_sq_i = (int64_t)((uint64_t)dst->sum_sq_i + (uint64_t)src->sum_sq_i);
        dst->prod_i   = (int64_t)((uint64_t)dst->prod_i   * (uint64_t)src->prod_i);
        if (src->min_i < dst->min_i) dst->min_i = src->min_i;
        if (src->max_i > dst->max_i) dst->max_i = src->max_i;
    }
    dst->cnt += src->cnt;
    dst->null_count += src->null_count;
    /* reduce_merge does not merge first/last; caller handles these separately.
     * Since workers process sequential ranges, worker 0's first is the global first,
     * and the last worker's last is the global last. */
}

/* Hash-based count distinct for integer/float columns */
ray_t* exec_count_distinct(ray_graph_t* g, ray_op_t* op, ray_t* input) {
    (void)g; (void)op;
    if (!input || RAY_IS_ERR(input)) return input;

    int8_t in_type = input->type;
    int64_t len = input->len;

    if (len == 0) return ray_i64(0);

    /* Only numeric/ordinal/sym column types are supported */
    switch (in_type) {
    case RAY_BOOL: case RAY_U8:
    case RAY_I16: case RAY_I32: case RAY_I64:
    case RAY_F64: case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP:
    case RAY_SYM:
        break;
    default:
        return ray_error("type", NULL);
    }

    /* Use a simple open-addressing hash set for int64 values */
    uint64_t cap = (uint64_t)(len < 16 ? 32 : len) * 2;
    /* Round up to power of 2 */
    uint64_t c = 1;
    while (c && c < cap) c <<= 1;
    if (!c) return ray_error("oom", NULL); /* overflow: cap too large */
    cap = c;

    ray_t* set_hdr;
    int64_t* set = (int64_t*)scratch_calloc(&set_hdr,
                                             (size_t)cap * sizeof(int64_t));
    ray_t* used_hdr;
    uint8_t* used = (uint8_t*)scratch_calloc(&used_hdr,
                                              (size_t)cap * sizeof(uint8_t));
    if (!set || !used) {
        if (set_hdr) scratch_free(set_hdr);
        if (used_hdr) scratch_free(used_hdr);
        return ray_error("oom", NULL);
    }

    int64_t count = 0;
    uint64_t mask = cap - 1;
    void* base = ray_data(input);

    for (int64_t i = 0; i < len; i++) {
        int64_t val;
        if (in_type == RAY_F64) {
            double fv = ((double*)base)[i];
            /* Normalize: NaN → canonical NaN, -0.0 → +0.0 */
            if (fv != fv) fv = (double)NAN;        /* canonical NaN */
            else if (fv == 0.0) fv = 0.0;          /* +0.0 */
            memcpy(&val, &fv, sizeof(int64_t));
        } else {
            val = read_col_i64(base, i, in_type, input->attrs);
        }

        /* Open-addressing linear probe */
        uint64_t h = (uint64_t)val * 0x9E3779B97F4A7C15ULL;
        uint64_t slot = h & mask;
        while (used[slot]) {
            if (set[slot] == val) goto next_val;
            slot = (slot + 1) & mask;
        }
        /* New distinct value */
        set[slot] = val;
        used[slot] = 1;
        count++;
        next_val:;
    }

    scratch_free(set_hdr);
    scratch_free(used_hdr);
    return ray_i64(count);
}

ray_t* exec_reduction(ray_graph_t* g, ray_op_t* op, ray_t* input) {
    (void)g;
    if (!input || RAY_IS_ERR(input)) return input;

    /* TABLE input: COUNT returns row count, others need a column */
    if (input->type == RAY_TABLE) {
        if (op->opcode == OP_COUNT)
            return ray_i64(ray_table_nrows(input));
        return ray_error("type", NULL);
    }

    int8_t in_type = input->type;
    int64_t len = input->len;

    /* Resolve null bitmap once before dispatching.  ray_vec_nullmap_bytes
     * handles slice / ext / inline / HAS_INDEX uniformly so this works on
     * vectors that carry an attached accelerator index. */
    bool has_nulls = (input->attrs & RAY_ATTR_HAS_NULLS) != 0;
    const uint8_t* null_bm = ray_vec_nullmap_bytes(input, NULL, NULL);

    /* O(1) short-circuit: first/last on numeric columns don't need a
     * full reduction pass.  Non-numeric types (STR, GUID) fall through
     * to the serial reduction path below. */
    if ((op->opcode == OP_FIRST || op->opcode == OP_LAST) &&
        (in_type == RAY_I64 || in_type == RAY_F64 || in_type == RAY_I32 ||
         in_type == RAY_I16 || in_type == RAY_BOOL || in_type == RAY_U8 ||
         in_type == RAY_TIMESTAMP || in_type == RAY_DATE || in_type == RAY_TIME ||
         in_type == RAY_SYM)) {
        int64_t row;
        if (op->opcode == OP_FIRST) {
            for (row = 0; row < len; row++)
                if (!has_nulls || !((null_bm[row/8] >> (row%8)) & 1)) break;
        } else {
            for (row = len - 1; row >= 0; row--)
                if (!has_nulls || !((null_bm[row/8] >> (row%8)) & 1)) break;
        }
        if (row < 0 || row >= len)
            return ray_typed_null(-in_type);
        void* base = ray_data(input);
        if (in_type == RAY_F64) return ray_f64(((const double*)base)[row]);
        return ray_i64(read_col_i64(base, row, in_type, input->attrs));
    }

    ray_pool_t* pool = ray_pool_get();
    if (pool && len >= RAY_PARALLEL_THRESHOLD) {
        uint32_t nw = ray_pool_total_workers(pool);
        ray_t* accs_hdr;
        reduce_acc_t* accs = (reduce_acc_t*)scratch_calloc(&accs_hdr, nw * sizeof(reduce_acc_t));
        if (!accs) return ray_error("oom", NULL);
        for (uint32_t i = 0; i < nw; i++) reduce_acc_init(&accs[i]);

        par_reduce_ctx_t ctx = { .input = input, .accs = accs,
                                 .has_nulls = has_nulls, .null_bm = null_bm };
        ray_pool_dispatch(pool, par_reduce_fn, &ctx, len);

        /* Merge: worker 0 is the base, merge the rest in order */
        reduce_acc_t merged;
        reduce_acc_init(&merged);
        merged = accs[0];
        for (uint32_t i = 1; i < nw; i++) {
            if (!accs[i].has_first) continue;
            reduce_merge(&merged, &accs[i], in_type);
        }
        /* first = accs[first worker with data], last = accs[last worker with data] */
        for (uint32_t i = 0; i < nw; i++) {
            if (accs[i].has_first) {
                if (in_type == RAY_F64) merged.first_f = accs[i].first_f;
                else merged.first_i = accs[i].first_i;
                break;
            }
        }
        for (int32_t i = (int32_t)nw - 1; i >= 0; i--) {
            if (accs[i].has_first) {
                if (in_type == RAY_F64) merged.last_f = accs[i].last_f;
                else merged.last_i = accs[i].last_i;
                break;
            }
        }

        ray_t* result;
        switch (op->opcode) {
            case OP_SUM:   result = in_type == RAY_F64 ? ray_f64(merged.sum_f) : ray_i64(merged.sum_i); break;
            case OP_PROD:  result = in_type == RAY_F64 ? ray_f64(merged.prod_f) : ray_i64(merged.prod_i); break;
            case OP_MIN:   result = merged.cnt > 0 ? (in_type == RAY_F64 ? ray_f64(merged.min_f) : ray_i64(merged.min_i)) : ray_typed_null(-in_type); break;
            case OP_MAX:   result = merged.cnt > 0 ? (in_type == RAY_F64 ? ray_f64(merged.max_f) : ray_i64(merged.max_i)) : ray_typed_null(-in_type); break;
            case OP_COUNT: result = ray_i64(merged.cnt); break;
            case OP_AVG:   result = merged.cnt > 0 ? ray_f64(in_type == RAY_F64 ? merged.sum_f / merged.cnt : (double)merged.sum_i / merged.cnt) : ray_typed_null(-RAY_F64); break;
            case OP_FIRST: result = merged.has_first ? (in_type == RAY_F64 ? ray_f64(merged.first_f) : ray_i64(merged.first_i)) : ray_typed_null(-in_type); break;
            case OP_LAST:  result = merged.has_first ? (in_type == RAY_F64 ? ray_f64(merged.last_f) : ray_i64(merged.last_i)) : ray_typed_null(-in_type); break;
            case OP_VAR: case OP_VAR_POP:
            case OP_STDDEV: case OP_STDDEV_POP: {
                bool insufficient = (op->opcode == OP_VAR || op->opcode == OP_STDDEV) ? merged.cnt <= 1 : merged.cnt <= 0;
                if (insufficient) { result = ray_typed_null(-RAY_F64); break; }
                double mean, var_pop;
                if (in_type == RAY_F64) { mean = merged.sum_f / merged.cnt; var_pop = merged.sum_sq_f / merged.cnt - mean * mean; }
                else { mean = (double)merged.sum_i / merged.cnt; var_pop = (double)merged.sum_sq_i / merged.cnt - mean * mean; }
                if (var_pop < 0) var_pop = 0;
                double val;
                if (op->opcode == OP_VAR_POP) val = var_pop;
                else if (op->opcode == OP_VAR) val = var_pop * merged.cnt / (merged.cnt - 1);
                else if (op->opcode == OP_STDDEV_POP) val = sqrt(var_pop);
                else val = sqrt(var_pop * merged.cnt / (merged.cnt - 1));
                result = ray_f64(val);
                break;
            }
            default:       result = ray_error("nyi", NULL); break;
        }
        scratch_free(accs_hdr);
        return result;
    }

    reduce_acc_t acc;
    reduce_acc_init(&acc);
    reduce_range(input, 0, len, &acc, has_nulls, null_bm);

    switch (op->opcode) {
        case OP_SUM:   return in_type == RAY_F64 ? ray_f64(acc.sum_f) : ray_i64(acc.sum_i);
        case OP_PROD:  return in_type == RAY_F64 ? ray_f64(acc.prod_f) : ray_i64(acc.prod_i);
        case OP_MIN:   return acc.cnt > 0 ? (in_type == RAY_F64 ? ray_f64(acc.min_f) : ray_i64(acc.min_i)) : ray_typed_null(-in_type);
        case OP_MAX:   return acc.cnt > 0 ? (in_type == RAY_F64 ? ray_f64(acc.max_f) : ray_i64(acc.max_i)) : ray_typed_null(-in_type);
        case OP_COUNT: return ray_i64(acc.cnt);
        case OP_AVG:   return acc.cnt > 0 ? ray_f64(in_type == RAY_F64 ? acc.sum_f / acc.cnt : (double)acc.sum_i / acc.cnt) : ray_typed_null(-RAY_F64);
        case OP_FIRST: return acc.has_first ? (in_type == RAY_F64 ? ray_f64(acc.first_f) : ray_i64(acc.first_i)) : ray_typed_null(-in_type);
        case OP_LAST:  return acc.has_first ? (in_type == RAY_F64 ? ray_f64(acc.last_f) : ray_i64(acc.last_i)) : ray_typed_null(-in_type);
        case OP_VAR: case OP_VAR_POP:
        case OP_STDDEV: case OP_STDDEV_POP: {
            bool insufficient = (op->opcode == OP_VAR || op->opcode == OP_STDDEV) ? acc.cnt <= 1 : acc.cnt <= 0;
            if (insufficient) return ray_typed_null(-RAY_F64);
            double mean, var_pop;
            if (in_type == RAY_F64) { mean = acc.sum_f / acc.cnt; var_pop = acc.sum_sq_f / acc.cnt - mean * mean; }
            else { mean = (double)acc.sum_i / acc.cnt; var_pop = (double)acc.sum_sq_i / acc.cnt - mean * mean; }
            if (var_pop < 0) var_pop = 0;
            double val;
            if (op->opcode == OP_VAR_POP) val = var_pop;
            else if (op->opcode == OP_VAR) val = var_pop * acc.cnt / (acc.cnt - 1);
            else if (op->opcode == OP_STDDEV_POP) val = sqrt(var_pop);
            else val = sqrt(var_pop * acc.cnt / (acc.cnt - 1));
            return ray_f64(val);
        }
        default:       return ray_error("nyi", NULL);
    }
}

/* ============================================================================
 * Group-by execution — with parallel local hash tables + merge
 * ============================================================================ */


/* Flags controlling which accumulator arrays are allocated */
/* GHT_NEED_* defined in exec_internal.h */

/* ── Row-layout HT ──────────────────────────────────────────────────────
 * Keys + accumulators stored inline in both radix entries and group rows.
 * After phase1 copies data from original columns, phase2 and phase3 never
 * touch column data again — all access is sequential/local.
 * ────────────────────────────────────────────────────────────────────── */

/* ght_layout_t defined in exec_internal.h */

ght_layout_t ght_compute_layout(uint8_t n_keys, uint8_t n_aggs,
                                        ray_t** agg_vecs, uint8_t need_flags,
                                        const uint16_t* agg_ops,
                                        const int8_t* key_types) {
    ght_layout_t ly;
    memset(&ly, 0, sizeof(ly));
    ly.n_keys = n_keys;
    ly.n_aggs = n_aggs;
    ly.need_flags = need_flags;

    /* Mark wide keys (those that don't fit in 8 bytes).  For each
     * wide key, the fat-entry and HT-row key slot stores a source
     * row index; probe/rehash/scatter resolve the actual bytes via
     * group_ht_t.key_data[k].  Currently only RAY_GUID is supported. */
    if (key_types) {
        for (uint8_t k = 0; k < n_keys && k < 8; k++) {
            if (key_types[k] == RAY_GUID) {
                ly.wide_key_mask |= (uint8_t)(1u << k);
                ly.wide_key_esz[k] = 16;
            }
        }
    }

    uint8_t nv = 0;
    for (uint8_t a = 0; a < n_aggs && a < 8; a++) {
        if (agg_vecs[a]) {
            ly.agg_val_slot[a] = (int8_t)nv;
            if (agg_vecs[a]->type == RAY_F64)
                ly.agg_is_f64 |= (1u << a);
            nv++;
        } else {
            ly.agg_val_slot[a] = -1;
        }
        if (agg_ops) {
            if (agg_ops[a] == OP_FIRST) ly.agg_is_first |= (1u << a);
            if (agg_ops[a] == OP_LAST)  ly.agg_is_last  |= (1u << a);
        }
    }
    ly.n_agg_vals = nv;
    /* Key region = n_keys*8 + 8-byte null mask slot (stored after last key).
     * The null mask slot holds a bitmap of which keys were null in the source
     * row (bit k = key k is null). Folding this slot into hash/memcmp lets
     * null and 0 form distinct groups. */
    uint16_t key_region = (uint16_t)((uint16_t)n_keys * 8 + 8);
    ly.entry_stride = (uint16_t)(8 + key_region + (uint16_t)nv * 8);

    uint16_t off = (uint16_t)(8 + key_region);
    uint16_t block = (uint16_t)nv * 8;
    if (need_flags & GHT_NEED_SUM)   { ly.off_sum   = off; off += block; }
    if (need_flags & GHT_NEED_MIN)   { ly.off_min   = off; off += block; }
    if (need_flags & GHT_NEED_MAX)   { ly.off_max   = off; off += block; }
    if (need_flags & GHT_NEED_SUMSQ) { ly.off_sumsq = off; off += block; }
    ly.row_stride = off;
    return ly;
}

/* Packed HT slots: [salt:8 | gid:24] in 4 bytes.
 * Max groups per HT = 16M (24 bits) — ample for partitioned probes.
 * 4B slots halve cache footprint vs 8B, fitting HT in L2. */
#define HT_EMPTY    UINT32_MAX
#define HT_PACK(salt, gid)  (((uint32_t)(uint8_t)(salt) << 24) | ((gid) & 0xFFFFFF))
#define HT_GID(s)   ((s) & 0xFFFFFF)
#define HT_SALT_V(s) ((uint8_t)((s) >> 24))

/* group_ht_t defined in exec_internal.h */

static bool group_ht_init_sized(group_ht_t* ht, uint32_t cap,
                                 const ght_layout_t* ly, uint32_t init_grp_cap) {
    ht->ht_cap = cap;
    ht->oom = 0;
    ht->layout = *ly;
    /* key_data must be populated by the caller via group_ht_set_key_data
     * whenever wide_key_mask != 0. */
    memset(ht->key_data, 0, sizeof(ht->key_data));
    ht->slots = (uint32_t*)scratch_alloc(&ht->_h_slots, (size_t)cap * sizeof(uint32_t));
    if (!ht->slots) return false;
    memset(ht->slots, 0xFF, (size_t)cap * sizeof(uint32_t)); /* HT_EMPTY = all-1s */
    ht->grp_cap = init_grp_cap;
    ht->grp_count = 0;
    ht->rows = (char*)scratch_alloc(&ht->_h_rows,
        (size_t)init_grp_cap * ly->row_stride);
    if (!ht->rows) return false;
    return true;
}

bool group_ht_init(group_ht_t* ht, uint32_t cap, const ght_layout_t* ly) {
    return group_ht_init_sized(ht, cap, ly, 256);
}

/* Populate key_data[k] for wide-key resolution. Called by the HT path
 * right after group_ht_init / group_ht_init_sized when any key is wide. */
static inline void group_ht_set_key_data(group_ht_t* ht, void** kd) {
    uint8_t mask = ht->layout.wide_key_mask;
    if (!mask || !kd) return;
    for (uint8_t k = 0; k < ht->layout.n_keys && k < 8; k++) {
        if (mask & (1u << k)) ht->key_data[k] = kd[k];
    }
}

void group_ht_free(group_ht_t* ht) {
    scratch_free(ht->_h_slots);
    scratch_free(ht->_h_rows);
}

static bool group_ht_grow(group_ht_t* ht) {
    uint32_t old_cap = ht->grp_cap;
    uint32_t new_cap = old_cap * 2;
    uint16_t rs = ht->layout.row_stride;
    char* new_rows = (char*)scratch_realloc(
        &ht->_h_rows, (size_t)old_cap * rs, (size_t)new_cap * rs);
    if (!new_rows) return false;
    ht->rows = new_rows;
    ht->grp_cap = new_cap;
    return true;
}

/* Hash inline int64_t keys (for rehash — resolves wide keys via
 * the HT's key_data pointers). */
static inline uint64_t hash_keys_inline(const int64_t* keys, const int8_t* key_types,
                                         uint8_t n_keys, uint8_t wide_mask,
                                         const uint8_t* wide_esz, void* const* key_data) {
    uint64_t h = 0;
    for (uint8_t k = 0; k < n_keys; k++) {
        uint64_t kh;
        if (wide_mask & (1u << k)) {
            /* Wide key: keys[k] is the source row index. Hash the
             * actual bytes from key_data[k]. */
            int64_t row_idx = keys[k];
            uint8_t esz = wide_esz[k];
            const void* src = (const char*)key_data[k] + (size_t)row_idx * esz;
            kh = ray_hash_bytes(src, esz);
        } else if (key_types[k] == RAY_F64) {
            double dv;
            memcpy(&dv, &keys[k], 8);
            kh = ray_hash_f64(dv);
        } else {
            kh = ray_hash_i64(keys[k]);
        }
        h = (k == 0) ? kh : ray_hash_combine(h, kh);
    }
    /* Fold null mask (slot n_keys) into hash so null/0 form distinct groups */
    int64_t null_mask = keys[n_keys];
    if (null_mask)
        h = ray_hash_combine(h, ray_hash_i64(null_mask));
    return h;
}

static void group_ht_rehash(group_ht_t* ht, const int8_t* key_types) {
    uint32_t new_cap = ht->ht_cap * 2;
    ray_t* new_h = NULL;
    uint32_t* new_slots = (uint32_t*)scratch_alloc(&new_h, (size_t)new_cap * sizeof(uint32_t));
    if (!new_slots) return; /* OOM: keep old HT, it still works (just slower) */
    scratch_free(ht->_h_slots);
    ht->_h_slots = new_h;
    ht->slots = new_slots;
    memset(ht->slots, 0xFF, (size_t)new_cap * sizeof(uint32_t));
    ht->ht_cap = new_cap;
    uint32_t mask = new_cap - 1;
    uint16_t rs = ht->layout.row_stride;
    uint8_t nk = ht->layout.n_keys;
    uint8_t wide = ht->layout.wide_key_mask;
    for (uint32_t gi = 0; gi < ht->grp_count; gi++) {
        const int64_t* row_keys = (const int64_t*)(ht->rows + (size_t)gi * rs + 8);
        uint64_t h = hash_keys_inline(row_keys, key_types, nk, wide,
                                       ht->layout.wide_key_esz, ht->key_data);
        uint32_t slot = (uint32_t)(h & mask);
        while (ht->slots[slot] != HT_EMPTY)
            slot = (slot + 1) & mask;
        ht->slots[slot] = HT_PACK(HT_SALT(h), gi);
    }
}

/* Initialize accumulators for a new group from entry's inline agg values.
 * Each unified block has n_agg_vals slots of 8 bytes, typed by agg_is_f64. */
static inline void init_accum_from_entry(char* row, const char* entry,
                                          const ght_layout_t* ly) {
    uint16_t accum_start = (uint16_t)(8 + ((uint16_t)ly->n_keys + 1) * 8);
    if (ly->row_stride > accum_start)
        memset(row + accum_start, 0, ly->row_stride - accum_start);

    const char* agg_data = entry + 8 + ((size_t)ly->n_keys + 1) * 8;
    uint8_t na = ly->n_aggs;
    uint8_t nf = ly->need_flags;

    for (uint8_t a = 0; a < na; a++) {
        int8_t s = ly->agg_val_slot[a];
        if (s < 0) continue;
        /* Copy raw 8 bytes from entry into each enabled accumulator block */
        if (nf & GHT_NEED_SUM) memcpy(row + ly->off_sum + s * 8, agg_data + s * 8, 8);
        if (nf & GHT_NEED_MIN) memcpy(row + ly->off_min + s * 8, agg_data + s * 8, 8);
        if (nf & GHT_NEED_MAX) memcpy(row + ly->off_max + s * 8, agg_data + s * 8, 8);
        if (nf & GHT_NEED_SUMSQ) {
            /* sumsq = v * v for the first entry */
            if (ly->agg_is_f64 & (1u << a)) {
                double v; memcpy(&v, agg_data + s * 8, 8);
                double sq = v * v;
                memcpy(row + ly->off_sumsq + s * 8, &sq, 8);
            } else {
                int64_t v; memcpy(&v, agg_data + s * 8, 8);
                double sq = (double)v * (double)v;
                memcpy(row + ly->off_sumsq + s * 8, &sq, 8);
            }
        }
    }
}

/* Row-layout accessors: cast through void* for strict-aliasing safety.
 * All row offsets are 8-byte aligned by construction. */
/* ROW_RD/WR macros defined in exec_internal.h */

/* Accumulate into existing group from entry's inline agg values */
static inline void accum_from_entry(char* row, const char* entry,
                                     const ght_layout_t* ly) {
    const char* agg_data = entry + 8 + ((size_t)ly->n_keys + 1) * 8;
    uint8_t na = ly->n_aggs;
    uint8_t nf = ly->need_flags;

    for (uint8_t a = 0; a < na; a++) {
        int8_t s = ly->agg_val_slot[a];
        if (s < 0) continue;
        const char* val = agg_data + s * 8;

        uint8_t amask = (1u << a);
        if (ly->agg_is_f64 & amask) {
            double v;
            memcpy(&v, val, 8);
            if (nf & GHT_NEED_SUM) {
                if (ly->agg_is_first & amask) { /* keep init value */ }
                else if (ly->agg_is_last & amask) { memcpy(row + ly->off_sum + s * 8, val, 8); }
                else { ROW_WR_F64(row, ly->off_sum, s) += v; }
            }
            if (nf & GHT_NEED_MIN) { double* p = &ROW_WR_F64(row, ly->off_min, s); if (v < *p) *p = v; }
            if (nf & GHT_NEED_MAX) { double* p = &ROW_WR_F64(row, ly->off_max, s); if (v > *p) *p = v; }
            if (nf & GHT_NEED_SUMSQ) { ROW_WR_F64(row, ly->off_sumsq, s) += v * v; }
        } else {
            int64_t v;
            memcpy(&v, val, 8);
            if (nf & GHT_NEED_SUM) {
                if (ly->agg_is_first & amask) { /* keep init value */ }
                else if (ly->agg_is_last & amask) { memcpy(row + ly->off_sum + s * 8, val, 8); }
                else { ROW_WR_I64(row, ly->off_sum, s) += v; }
            }
            if (nf & GHT_NEED_MIN) { int64_t* p = &ROW_WR_I64(row, ly->off_min, s); if (v < *p) *p = v; }
            if (nf & GHT_NEED_MAX) { int64_t* p = &ROW_WR_I64(row, ly->off_max, s); if (v > *p) *p = v; }
            if (nf & GHT_NEED_SUMSQ) { ROW_WR_F64(row, ly->off_sumsq, s) += (double)v * (double)v; }
        }
    }
}

/* Compare the n_keys key slots of two rows, handling wide keys via
 * key_data[] resolution.  Returns true if all keys are bytewise equal.
 * Hot path: when wide_mask == 0, reduces to a single memcmp over the
 * packed 8-byte-per-key region. */
static inline bool group_keys_equal(const int64_t* a_keys, const int64_t* b_keys,
                                      const ght_layout_t* ly, void* const* key_data) {
    uint8_t wide = ly->wide_key_mask;
    uint8_t nk = ly->n_keys;
    if (wide == 0) {
        /* memcmp covers nk values + trailing 8-byte null mask slot */
        return memcmp(a_keys, b_keys, (size_t)(nk + 1) * 8) == 0;
    }
    for (uint8_t k = 0; k < nk; k++) {
        if (wide & (1u << k)) {
            int64_t ra = a_keys[k];
            int64_t rb = b_keys[k];
            if (ra == rb) continue;  /* same source row - trivially equal */
            uint8_t esz = ly->wide_key_esz[k];
            const char* base = (const char*)key_data[k];
            if (memcmp(base + (size_t)ra * esz,
                       base + (size_t)rb * esz, esz) != 0) return false;
        } else {
            if (a_keys[k] != b_keys[k]) return false;
        }
    }
    /* Null mask slot must match too */
    if (a_keys[nk] != b_keys[nk]) return false;
    return true;
}

/* Probe + accumulate a single fat entry into the HT. Returns updated mask. */
static inline uint32_t group_probe_entry(group_ht_t* ht,
    const char* entry, const int8_t* key_types, uint32_t mask) {
    const ght_layout_t* ly = &ht->layout;
    uint64_t hash = *(const uint64_t*)entry;
    const char* ekeys = entry + 8;
    uint8_t salt = HT_SALT(hash);
    uint32_t slot = (uint32_t)(hash & mask);
    uint16_t key_bytes = (uint16_t)((ly->n_keys + 1) * 8);

    for (;;) {
        uint32_t sv = ht->slots[slot];
        if (sv == HT_EMPTY) {
            /* New group */
            if (ht->grp_count >= ht->grp_cap) {
                if (!group_ht_grow(ht)) { ht->oom = 1; return mask; }
            }
            uint32_t gid = ht->grp_count++;
            char* row = ht->rows + (size_t)gid * ly->row_stride;
            *(int64_t*)row = 1;   /* count = 1 */
            memcpy(row + 8, ekeys, key_bytes);
            init_accum_from_entry(row, entry, ly);
            ht->slots[slot] = HT_PACK(salt, gid);
            if (ht->grp_count * 2 > ht->ht_cap) {
                group_ht_rehash(ht, key_types);
                mask = ht->ht_cap - 1;
            }
            return mask;
        }
        if (HT_SALT_V(sv) == salt) {
            uint32_t gid = HT_GID(sv);
            char* row = ht->rows + (size_t)gid * ly->row_stride;
            if (group_keys_equal((const int64_t*)(row + 8),
                                  (const int64_t*)ekeys, ly, ht->key_data)) {
                (*(int64_t*)row)++;   /* count++ */
                accum_from_entry(row, entry, ly);
                return mask;
            }
        }
        slot = (slot + 1) & mask;
    }
}

/* Process rows [start, end) from original columns into a local hash table.
 * Converts each row to a fat entry on the stack, then probes. */
#define GROUP_PREFETCH_BATCH 16

void group_rows_range(group_ht_t* ht, void** key_data, int8_t* key_types,
                              uint8_t* key_attrs, ray_t** key_vecs, ray_t** agg_vecs,
                              int64_t start, int64_t end,
                              const int64_t* match_idx) {
    const ght_layout_t* ly = &ht->layout;
    uint8_t nk = ly->n_keys;
    uint8_t na = ly->n_aggs;
    uint8_t wide = ly->wide_key_mask;
    uint32_t mask = ht->ht_cap - 1;
    /* Stack buffer for one entry: hash + (nk+1) key slots + nv agg_vals.
     * Max size: 8 + 9*8 + 8*8 = 144 bytes. */
    char ebuf[8 + 9 * 8 + 8 * 8];

    /* Check which key columns can produce nulls (parent vec's HAS_NULLS
     * attr for slices) — skips per-row null checks on the fast path. */
    uint8_t nullable_mask = 0;
    for (uint8_t k = 0; k < nk; k++) {
        if (!key_vecs || !key_vecs[k]) continue;
        ray_t* kv = key_vecs[k];
        ray_t* src = (kv->attrs & RAY_ATTR_SLICE) ? kv->slice_parent : kv;
        if (src && (src->attrs & RAY_ATTR_HAS_NULLS))
            nullable_mask |= (uint8_t)(1u << k);
    }

    /* Wire the HT's key_data pointer table so probe/rehash can
     * resolve wide keys via the source columns. */
    if (wide) group_ht_set_key_data(ht, key_data);

    for (int64_t i = start; i < end; i++) {
        /* Cancellation checkpoint every 65536 rows — ~150 polls on a
         * 10M-row ingest, imperceptible in the inner loop and still
         * sub-100ms response time on Ctrl-C. */
        if (((i - start) & 65535) == 0 && ray_interrupted()) break;
        int64_t row = match_idx ? match_idx[i] : i;
        uint64_t h = 0;
        int64_t* ek = (int64_t*)(ebuf + 8);
        int64_t null_mask = 0;
        for (uint8_t k = 0; k < nk; k++) {
            int8_t t = key_types[k];
            uint64_t kh;
            bool is_null = (nullable_mask & (1u << k))
                           && ray_vec_is_null(key_vecs[k], row);
            if (is_null) {
                null_mask |= (int64_t)(1u << k);
                ek[k] = 0;  /* canonical null value — real 0 differs via null_mask */
                kh = ray_hash_i64(0);
            } else if (wide & (1u << k)) {
                /* Wide key: store source row index, hash the actual bytes. */
                uint8_t esz = ly->wide_key_esz[k];
                const void* src = (const char*)key_data[k] + (size_t)row * esz;
                ek[k] = row;
                kh = ray_hash_bytes(src, esz);
            } else if (t == RAY_F64) {
                int64_t kv;
                memcpy(&kv, &((double*)key_data[k])[row], 8);
                ek[k] = kv;
                kh = ray_hash_f64(((double*)key_data[k])[row]);
            } else {
                int64_t kv = read_col_i64(key_data[k], row, t, key_attrs[k]);
                ek[k] = kv;
                kh = ray_hash_i64(kv);
            }
            h = (k == 0) ? kh : ray_hash_combine(h, kh);
        }
        ek[nk] = null_mask;
        if (null_mask) h = ray_hash_combine(h, ray_hash_i64(null_mask));
        *(uint64_t*)ebuf = h;

        int64_t* ev = (int64_t*)(ebuf + 8 + ((size_t)nk + 1) * 8);
        uint8_t vi = 0;
        for (uint8_t a = 0; a < na; a++) {
            ray_t* ac = agg_vecs[a];
            if (!ac) continue;
            if (ac->type == RAY_F64)
                memcpy(&ev[vi], &((double*)ray_data(ac))[row], 8);
            else
                ev[vi] = read_col_i64(ray_data(ac), row, ac->type, ac->attrs);
            vi++;
        }

        mask = group_probe_entry(ht, ebuf, key_types, mask);
    }
}

/* ============================================================================
 * Radix-partitioned parallel group-by
 *
 * Phase 1 (parallel): Each worker reads keys+agg values from original columns,
 *         packs into fat entries (hash, keys, agg_vals), scatters into
 *         thread-local per-partition buffers.
 * Phase 2 (parallel): Each partition is aggregated independently using
 *         inline data — no original column access needed.
 * Phase 3: Build result columns from inline group rows.
 * ============================================================================ */

#define RADIX_BITS  8
#define RADIX_P     (1u << RADIX_BITS)   /* 256 partitions */
#define RADIX_MASK  (RADIX_P - 1)
#define RADIX_PART(h) (((uint32_t)((h) >> 16)) & RADIX_MASK)

/* Per-worker, per-partition buffer of fat entries */
typedef struct {
    char*    data;           /* flat buffer: data[i * entry_stride] */
    uint32_t count;
    uint32_t cap;
    bool     oom;            /* set on realloc failure */
    ray_t*    _hdr;
} radix_buf_t;

static inline void radix_buf_push(radix_buf_t* buf, uint16_t entry_stride,
                                   uint64_t hash, const int64_t* keys, uint8_t n_keys,
                                   int64_t null_mask,
                                   const int64_t* agg_vals, uint8_t n_agg_vals) {
    if (__builtin_expect(buf->count >= buf->cap, 0)) {
        uint32_t old_cap = buf->cap;
        uint32_t new_cap = old_cap * 2;
        char* new_data = (char*)scratch_realloc(
            &buf->_hdr, (size_t)old_cap * entry_stride,
            (size_t)new_cap * entry_stride);
        if (!new_data) { buf->oom = true; return; }
        buf->data = new_data;
        buf->cap = new_cap;
    }
    char* dst = buf->data + (size_t)buf->count * entry_stride;
    *(uint64_t*)dst = hash;
    memcpy(dst + 8, keys, (size_t)n_keys * 8);
    /* Null mask slot sits right after the keys */
    memcpy(dst + 8 + (size_t)n_keys * 8, &null_mask, 8);
    if (n_agg_vals)
        memcpy(dst + 8 + ((size_t)n_keys + 1) * 8, agg_vals, (size_t)n_agg_vals * 8);
    buf->count++;
}

typedef struct {
    void**       key_data;
    int8_t*      key_types;
    uint8_t*     key_attrs;
    ray_t**      key_vecs;
    uint8_t      nullable_mask;   /* bit k = key k column may contain nulls */
    ray_t**       agg_vecs;
    uint32_t     n_workers;
    radix_buf_t* bufs;        /* [n_workers * RADIX_P] */
    ght_layout_t layout;
    /* When non-NULL, workers iterate match_idx[start..end) and
     * read row=match_idx[i].  When NULL, row=i. */
    const int64_t* match_idx;
} radix_phase1_ctx_t;

static void radix_phase1_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    radix_phase1_ctx_t* c = (radix_phase1_ctx_t*)ctx;
    const ght_layout_t* ly = &c->layout;
    radix_buf_t* my_bufs = &c->bufs[(size_t)worker_id * RADIX_P];
    uint8_t nk = ly->n_keys;
    uint8_t na = ly->n_aggs;
    uint8_t nv = ly->n_agg_vals;
    uint8_t wide = ly->wide_key_mask;
    uint16_t estride = ly->entry_stride;
    const int64_t* match_idx = c->match_idx;

    int64_t keys[8];
    int64_t agg_vals[8];

    uint8_t nullable = c->nullable_mask;
    for (int64_t i = start; i < end; i++) {
        /* Cancellation checkpoint every 65536 rows — ~150 polls on a
         * 10M-row ingest, imperceptible in the inner loop and still
         * sub-100ms response time on Ctrl-C. */
        if (((i - start) & 65535) == 0 && ray_interrupted()) break;
        int64_t row = match_idx ? match_idx[i] : i;
        uint64_t h = 0;
        int64_t null_mask = 0;
        for (uint8_t k = 0; k < nk; k++) {
            int8_t t = c->key_types[k];
            uint64_t kh;
            bool is_null = (nullable & (1u << k))
                           && ray_vec_is_null(c->key_vecs[k], row);
            if (is_null) {
                null_mask |= (int64_t)(1u << k);
                keys[k] = 0;
                kh = ray_hash_i64(0);
            } else if (wide & (1u << k)) {
                uint8_t esz = ly->wide_key_esz[k];
                const void* src = (const char*)c->key_data[k] + (size_t)row * esz;
                keys[k] = row;
                kh = ray_hash_bytes(src, esz);
            } else if (t == RAY_F64) {
                int64_t kv;
                memcpy(&kv, &((double*)c->key_data[k])[row], 8);
                keys[k] = kv;
                kh = ray_hash_f64(((double*)c->key_data[k])[row]);
            } else {
                int64_t kv = read_col_i64(c->key_data[k], row, t, c->key_attrs[k]);
                keys[k] = kv;
                kh = ray_hash_i64(kv);
            }
            h = (k == 0) ? kh : ray_hash_combine(h, kh);
        }
        if (null_mask) h = ray_hash_combine(h, ray_hash_i64(null_mask));

        uint8_t vi = 0;
        for (uint8_t a = 0; a < na; a++) {
            ray_t* ac = c->agg_vecs[a];
            if (!ac) continue;
            if (ac->type == RAY_F64)
                memcpy(&agg_vals[vi], &((double*)ray_data(ac))[row], 8);
            else
                agg_vals[vi] = read_col_i64(ray_data(ac), row, ac->type, ac->attrs);
            vi++;
        }

        uint32_t part = RADIX_PART(h);
        radix_buf_push(&my_bufs[part], estride, h, keys, nk, null_mask, agg_vals, nv);
    }
}

/* Process pre-partitioned fat entries into an HT with prefetch batching.
 * Two-phase prefetch: (1) prefetch HT slots, (2) prefetch group rows. */
static void group_rows_indirect(group_ht_t* ht, const int8_t* key_types,
                                 const char* entries, uint32_t n_entries,
                                 uint16_t entry_stride) {
    uint32_t mask = ht->ht_cap - 1;
    /* Stride-ahead prefetch: prefetch HT slot for entry i+D while processing i.
     * D=8 covers ~200ns L2/L3 latency at ~25ns per probe iteration. */
    enum { PF_DIST = 8 };
    /* Prime the prefetch pipeline */
    uint32_t pf_end = (n_entries < PF_DIST) ? n_entries : PF_DIST;
    for (uint32_t j = 0; j < pf_end; j++) {
        uint64_t h = *(const uint64_t*)(entries + (size_t)j * entry_stride);
        __builtin_prefetch(&ht->slots[(uint32_t)(h & mask)], 0, 1);
    }
    for (uint32_t i = 0; i < n_entries; i++) {
        /* Prefetch PF_DIST entries ahead */
        if (i + PF_DIST < n_entries) {
            uint64_t h = *(const uint64_t*)(entries + (size_t)(i + PF_DIST) * entry_stride);
            __builtin_prefetch(&ht->slots[(uint32_t)(h & mask)], 0, 1);
        }
        const char* e = entries + (size_t)i * entry_stride;
        mask = group_probe_entry(ht, e, key_types, mask);
    }
}

/* Phase 3: build result columns from inline group rows */
typedef struct {
    int8_t  out_type;
    bool    src_f64;
    uint16_t agg_op;
    bool    affine;
    double  bias_f64;
    int64_t bias_i64;
    void*   dst;
    ray_t*  vec;
} agg_out_t;

/* Aliases for shared parallel null helpers from internal.h */
#define grp_set_null       par_set_null
#define grp_prepare_nullmap par_prepare_nullmap
#define grp_finalize_nulls par_finalize_nulls

typedef struct {
    group_ht_t*   part_hts;
    uint32_t*     part_offsets;
    char**        key_dsts;
    int8_t*       key_types;
    uint8_t*      key_attrs;
    uint8_t*      key_esizes;
    ray_t**       key_cols;       /* [n_keys] output key vecs (for null bit writes) */
    uint8_t       n_keys;
    agg_out_t*    agg_outs;
    uint8_t       n_aggs;
    /* For wide-key columns (RAY_GUID), the stored key slot is a
     * source row index and we copy the actual bytes from the source
     * column here during the result scatter. */
    void**        key_src_data;   /* [n_keys]; NULL entry if not wide */
} radix_phase3_ctx_t;

static void radix_phase3_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    radix_phase3_ctx_t* c = (radix_phase3_ctx_t*)ctx;
    uint8_t nk = c->n_keys;
    uint8_t na = c->n_aggs;

    for (int64_t p = start; p < end; p++) {
        group_ht_t* ph = &c->part_hts[p];
        uint32_t gc = ph->grp_count;
        if (gc == 0) continue;
        uint32_t off = c->part_offsets[p];
        const ght_layout_t* ly = &ph->layout;
        uint16_t rs = ly->row_stride;

        /* Single pass over group rows: read each row once, scatter keys + aggs.
         * Reduces memory traffic from nk+na passes over group data to 1 pass. */
        for (uint32_t gi = 0; gi < gc; gi++) {
            const char* row = ph->rows + (size_t)gi * rs;
            const int64_t* rkeys = (const int64_t*)(const void*)(row + 8);
            int64_t cnt = *(const int64_t*)(const void*)row;
            int64_t null_mask = rkeys[nk];
            uint32_t di = off + gi;

            /* Scatter keys to result columns */
            for (uint8_t k = 0; k < nk; k++) {
                if (null_mask & (int64_t)(1u << k)) {
                    if (c->key_cols && c->key_cols[k])
                        grp_set_null(c->key_cols[k], di);
                    continue;
                }
                int64_t kv = rkeys[k];
                int8_t kt = c->key_types[k];
                char* dst = c->key_dsts[k];
                uint8_t esz = c->key_esizes[k];
                size_t doff = (size_t)di * esz;
                if (ly->wide_key_mask & (1u << k)) {
                    /* Wide key: kv is the source row index; copy the
                     * bytes from the source column into the output. */
                    const char* src = (const char*)c->key_src_data[k];
                    memcpy(dst + doff, src + (size_t)kv * esz, esz);
                } else if (kt == RAY_F64) {
                    memcpy(dst + doff, &kv, 8);
                } else {
                    write_col_i64(dst, di, kv, kt, c->key_attrs[k]);
                }
            }

            /* Scatter agg results to result columns */
            for (uint8_t a = 0; a < na; a++) {
                agg_out_t* ao = &c->agg_outs[a];
                if (!ao->dst) continue; /* allocation failed (OOM) */
                uint16_t op = ao->agg_op;
                bool sf = ao->src_f64;
                int8_t s = ly->agg_val_slot[a];
                if (ao->out_type == RAY_F64) {
                    double v;
                    switch (op) {
                        case OP_SUM:
                            v = sf ? ROW_RD_F64(row, ly->off_sum, s)
                                   : (double)ROW_RD_I64(row, ly->off_sum, s);
                            if (ao->affine) v += ao->bias_f64 * cnt;
                            break;
                        case OP_AVG:
                            v = sf ? ROW_RD_F64(row, ly->off_sum, s) / cnt
                                   : (double)ROW_RD_I64(row, ly->off_sum, s) / cnt;
                            if (ao->affine) v += ao->bias_f64;
                            break;
                        case OP_MIN:
                            v = sf ? ROW_RD_F64(row, ly->off_min, s)
                                   : (double)ROW_RD_I64(row, ly->off_min, s);
                            break;
                        case OP_MAX:
                            v = sf ? ROW_RD_F64(row, ly->off_max, s)
                                   : (double)ROW_RD_I64(row, ly->off_max, s);
                            break;
                        case OP_FIRST: case OP_LAST:
                            v = sf ? ROW_RD_F64(row, ly->off_sum, s)
                                   : (double)ROW_RD_I64(row, ly->off_sum, s);
                            break;
                        case OP_VAR: case OP_VAR_POP:
                        case OP_STDDEV: case OP_STDDEV_POP: {
                            bool insuf = (op == OP_VAR || op == OP_STDDEV) ? cnt <= 1 : cnt <= 0;
                            if (insuf) { v = 0.0; grp_set_null(ao->vec, di); break; }
                            double sum_val = sf ? ROW_RD_F64(row, ly->off_sum, s)
                                                : (double)ROW_RD_I64(row, ly->off_sum, s);
                            double sq_val = ly->off_sumsq ? ROW_RD_F64(row, ly->off_sumsq, s) : 0.0;
                            double mean = sum_val / cnt;
                            double var_pop = sq_val / cnt - mean * mean;
                            if (var_pop < 0) var_pop = 0;
                            if (op == OP_VAR_POP) v = var_pop;
                            else if (op == OP_VAR) v = var_pop * cnt / (cnt - 1);
                            else if (op == OP_STDDEV_POP) v = sqrt(var_pop);
                            else v = sqrt(var_pop * cnt / (cnt - 1));
                            break;
                        }
                        default: v = 0.0; break;
                    }
                    ((double*)(void*)ao->dst)[di] = v;
                } else {
                    int64_t v;
                    switch (op) {
                        case OP_SUM:
                            v = ROW_RD_I64(row, ly->off_sum, s);
                            if (ao->affine) v += ao->bias_i64 * cnt;
                            break;
                        case OP_COUNT: v = cnt; break;
                        case OP_MIN:   v = ROW_RD_I64(row, ly->off_min, s); break;
                        case OP_MAX:   v = ROW_RD_I64(row, ly->off_max, s); break;
                        case OP_FIRST: case OP_LAST: v = ROW_RD_I64(row, ly->off_sum, s); break;
                        default:       v = 0; break;
                    }
                    ((int64_t*)(void*)ao->dst)[di] = v;
                }
            }
        }
    }
}

/* Phase 2: aggregate each partition independently using inline data */
typedef struct {
    int8_t*      key_types;
    uint8_t      n_keys;
    uint32_t     n_workers;
    radix_buf_t* bufs;
    group_ht_t*  part_hts;
    ght_layout_t layout;
    /* Shared (read-only) source column bases for wide-key resolution.
     * Each partition HT stashes the ones matching wide_key_mask. */
    void**       key_data;
} radix_phase2_ctx_t;

static void radix_phase2_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    radix_phase2_ctx_t* c = (radix_phase2_ctx_t*)ctx;
    uint16_t estride = c->layout.entry_stride;

    for (int64_t p = start; p < end; p++) {
        uint32_t total = 0;
        for (uint32_t w = 0; w < c->n_workers; w++)
            total += c->bufs[(size_t)w * RADIX_P + p].count;
        if (total == 0) continue;

        uint32_t part_ht_cap = 256;
        {
            uint64_t target = (uint64_t)total * 2;
            if (target < 256) target = 256;
            while (part_ht_cap < target) part_ht_cap *= 2;
        }
        /* Pre-size group store to avoid grows. Use next_pow2(total) as upper
         * bound on groups. Over-allocation is bounded: worst case total >> groups,
         * but total * row_stride is already committed via HT capacity anyway. */
        uint32_t init_grp = 256;
        while (init_grp < total && init_grp < 65536) init_grp *= 2;
        if (!group_ht_init_sized(&c->part_hts[p], part_ht_cap, &c->layout, init_grp))
            continue;
        /* Wide keys need source-column resolution during probe/rehash. */
        if (c->layout.wide_key_mask && c->key_data)
            group_ht_set_key_data(&c->part_hts[p], c->key_data);

        for (uint32_t w = 0; w < c->n_workers; w++) {
            radix_buf_t* buf = &c->bufs[(size_t)w * RADIX_P + p];
            if (buf->count == 0) continue;
            group_rows_indirect(&c->part_hts[p], c->key_types,
                                buf->data, buf->count, estride);
        }
    }
}

/* ============================================================================
 * Parallel direct-array accumulation for low-cardinality single integer key
 * ============================================================================ */

/* Parallel min/max scan for direct-array key range detection */
typedef struct {
    const void* key_data;
    int8_t      key_type;
    uint8_t     key_attrs;
    int64_t*    per_worker_min;  /* [n_workers] */
    int64_t*    per_worker_max;  /* [n_workers] */
    uint32_t    n_workers;
    const int64_t* match_idx;    /* NULL = no selection */
} minmax_ctx_t;

static void minmax_scan_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    minmax_ctx_t* c = (minmax_ctx_t*)ctx;
    uint32_t wid = worker_id % c->n_workers;
    const int64_t* match_idx = c->match_idx;
    int64_t kmin = INT64_MAX, kmax = INT64_MIN;
    int8_t t = c->key_type;

    #define MINMAX_SEG_LOOP(TYPE, CAST) \
        do { \
            const TYPE* kd = (const TYPE*)c->key_data; \
            for (int64_t i = start; i < end; i++) { \
                int64_t r = match_idx ? match_idx[i] : i; \
                int64_t v = (int64_t)CAST kd[r]; \
                if (v < kmin) kmin = v; \
                if (v > kmax) kmax = v; \
            } \
        } while (0)

    if (t == RAY_I64 || t == RAY_TIMESTAMP)
        MINMAX_SEG_LOOP(int64_t, );
    else if (RAY_IS_SYM(t)) {
        uint8_t w = c->key_attrs & RAY_SYM_W_MASK;
        if (w == RAY_SYM_W64) MINMAX_SEG_LOOP(int64_t, );
        else if (w == RAY_SYM_W32) MINMAX_SEG_LOOP(uint32_t, );
        else if (w == RAY_SYM_W16) MINMAX_SEG_LOOP(uint16_t, );
        else MINMAX_SEG_LOOP(uint8_t, );
    }
    else if (t == RAY_BOOL || t == RAY_U8)
        MINMAX_SEG_LOOP(uint8_t, );
    else if (t == RAY_I16)
        MINMAX_SEG_LOOP(int16_t, );
    else /* RAY_I32, RAY_DATE, RAY_TIME */
        MINMAX_SEG_LOOP(int32_t, );

    #undef MINMAX_SEG_LOOP

    /* Merge with existing per-worker values (a worker may process multiple morsels) */
    if (kmin < c->per_worker_min[wid]) c->per_worker_min[wid] = kmin;
    if (kmax > c->per_worker_max[wid]) c->per_worker_max[wid] = kmax;
}

typedef union { double f; int64_t i; } da_val_t;

typedef struct {
    da_val_t* sum;       /* SUM/AVG/FIRST/LAST [n_slots * n_aggs] */
    da_val_t* min_val;   /* MIN [n_slots * n_aggs] */
    da_val_t* max_val;   /* MAX [n_slots * n_aggs] */
    double*   sumsq_f64; /* sum-of-squares for STDDEV/VAR */
    int64_t*  count;     /* group counts [n_slots] */
    /* Arena headers */
    ray_t* _h_sum;
    ray_t* _h_min;
    ray_t* _h_max;
    ray_t* _h_sumsq;
    ray_t* _h_count;
} da_accum_t;

static inline void da_accum_free(da_accum_t* a) {
    scratch_free(a->_h_sum);
    scratch_free(a->_h_min);
    scratch_free(a->_h_max);
    scratch_free(a->_h_sumsq);
    scratch_free(a->_h_count);
}

/* Unified agg result emitter — used by both DA and HT paths.
 * Arrays indexed by [gi * n_aggs + a], counts by [gi]. */
static void emit_agg_columns(ray_t** result, ray_graph_t* g, const ray_op_ext_t* ext,
                              ray_t* const* agg_vecs, uint32_t grp_count,
                              uint8_t n_aggs,
                              const double*  sum_f64,  const int64_t* sum_i64,
                              const double*  min_f64,  const double*  max_f64,
                              const int64_t* min_i64,  const int64_t* max_i64,
                              const int64_t* counts,
                              const agg_affine_t* affine,
                              const double*  sumsq_f64) {
    for (uint8_t a = 0; a < n_aggs; a++) {
        uint16_t agg_op = ext->agg_ops[a];
        ray_t* agg_col = agg_vecs[a];
        bool is_f64 = agg_col && agg_col->type == RAY_F64;
        int8_t out_type;
        switch (agg_op) {
            case OP_AVG:
            case OP_STDDEV: case OP_STDDEV_POP:
            case OP_VAR: case OP_VAR_POP:
                out_type = RAY_F64; break;
            case OP_COUNT: out_type = RAY_I64; break;
            case OP_SUM: case OP_PROD:
                out_type = is_f64 ? RAY_F64 : RAY_I64; break;
            default:
                out_type = agg_col ? agg_col->type : RAY_I64; break;
        }
        ray_t* new_col = ray_vec_new(out_type, (int64_t)grp_count);
        if (!new_col || RAY_IS_ERR(new_col)) continue;
        new_col->len = (int64_t)grp_count;
        for (uint32_t gi = 0; gi < grp_count; gi++) {
            size_t idx = (size_t)gi * n_aggs + a;
            if (out_type == RAY_F64) {
                double v;
                switch (agg_op) {
                    case OP_SUM:
                        v = is_f64 ? sum_f64[idx] : (double)sum_i64[idx];
                        if (affine && affine[a].enabled)
                            v += affine[a].bias_f64 * counts[gi];
                        break;
                    case OP_AVG:
                        v = is_f64 ? sum_f64[idx] / counts[gi] : (double)sum_i64[idx] / counts[gi];
                        if (affine && affine[a].enabled)
                            v += affine[a].bias_f64;
                        break;
                    case OP_MIN: v = is_f64 ? min_f64[idx] : (double)min_i64[idx]; break;
                    case OP_MAX: v = is_f64 ? max_f64[idx] : (double)max_i64[idx]; break;
                    case OP_FIRST: case OP_LAST:
                        v = is_f64 ? sum_f64[idx] : (double)sum_i64[idx]; break;
                    case OP_VAR: case OP_VAR_POP:
                    case OP_STDDEV: case OP_STDDEV_POP: {
                        int64_t cnt = counts[gi];
                        bool insuf = (agg_op == OP_VAR || agg_op == OP_STDDEV) ? cnt <= 1 : cnt <= 0;
                        if (insuf) { v = 0.0; ray_vec_set_null(new_col, gi, true); break; }
                        double sum_val = is_f64 ? sum_f64[idx] : (double)sum_i64[idx];
                        double sq_val = sumsq_f64 ? sumsq_f64[idx] : 0.0;
                        double mean = sum_val / cnt;
                        double var_pop = sq_val / cnt - mean * mean;
                        if (var_pop < 0) var_pop = 0;
                        if (agg_op == OP_VAR_POP) v = var_pop;
                        else if (agg_op == OP_VAR) v = var_pop * cnt / (cnt - 1);
                        else if (agg_op == OP_STDDEV_POP) v = sqrt(var_pop);
                        else v = sqrt(var_pop * cnt / (cnt - 1));
                        break;
                    }
                    default:     v = 0.0; break;
                }
                ((double*)ray_data(new_col))[gi] = v;
            } else {
                int64_t v;
                switch (agg_op) {
                    case OP_SUM:
                        v = sum_i64[idx];
                        if (affine && affine[a].enabled)
                            v += affine[a].bias_i64 * counts[gi];
                        break;
                    case OP_COUNT: v = counts[gi]; break;
                    case OP_MIN:   v = min_i64[idx]; break;
                    case OP_MAX:   v = max_i64[idx]; break;
                    case OP_FIRST: case OP_LAST: v = sum_i64[idx]; break;
                    default:       v = 0; break;
                }
                ((int64_t*)ray_data(new_col))[gi] = v;
            }
        }
        /* Generate unique column name: base_name + agg suffix (e.g. "v1_sum") */
        ray_op_ext_t* agg_ext = find_ext(g, ext->agg_ins[a]->id);
        int64_t name_id;
        if (agg_ext && agg_ext->base.opcode == OP_SCAN) {
            ray_t* name_atom = ray_sym_str(agg_ext->sym);
            const char* base = name_atom ? ray_str_ptr(name_atom) : NULL;
            size_t blen = base ? ray_str_len(name_atom) : 0;
            const char* sfx = "";
            size_t slen = 0;
            switch (agg_op) {
                case OP_SUM:   sfx = "_sum";   slen = 4; break;
                case OP_COUNT: sfx = "_count"; slen = 6; break;
                case OP_AVG:   sfx = "_mean";  slen = 5; break;
                case OP_MIN:   sfx = "_min";   slen = 4; break;
                case OP_MAX:   sfx = "_max";   slen = 4; break;
                case OP_FIRST: sfx = "_first"; slen = 6; break;
                case OP_LAST:  sfx = "_last";  slen = 5; break;
                case OP_STDDEV:     sfx = "_stddev";     slen = 7; break;
                case OP_STDDEV_POP: sfx = "_stddev_pop"; slen = 11; break;
                case OP_VAR:        sfx = "_var";        slen = 4; break;
                case OP_VAR_POP:    sfx = "_var_pop";    slen = 8; break;
            }
            char buf[256];
            if (base && blen + slen < sizeof(buf)) {
                memcpy(buf, base, blen);
                memcpy(buf + blen, sfx, slen);
                name_id = ray_sym_intern(buf, blen + slen);
            } else {
                name_id = agg_ext->sym;
            }
        } else {
            /* Expression agg input — synthetic name like "_e0_sum" */
            char nbuf[32];
            int np = 0;
            nbuf[np++] = '_'; nbuf[np++] = 'e';
            /* Multi-digit agg index */
            { uint8_t v = a; char dig[3]; int nd = 0;
              do { dig[nd++] = (char)('0' + v % 10); v /= 10; } while (v);
              while (nd--) nbuf[np++] = dig[nd]; }
            const char* nsfx = "";
            size_t nslen = 0;
            switch (agg_op) {
                case OP_SUM:   nsfx = "_sum";   nslen = 4; break;
                case OP_COUNT: nsfx = "_count"; nslen = 6; break;
                case OP_AVG:   nsfx = "_mean";  nslen = 5; break;
                case OP_MIN:   nsfx = "_min";   nslen = 4; break;
                case OP_MAX:   nsfx = "_max";   nslen = 4; break;
                case OP_FIRST: nsfx = "_first"; nslen = 6; break;
                case OP_LAST:  nsfx = "_last";  nslen = 5; break;
                case OP_STDDEV:     nsfx = "_stddev";     nslen = 7; break;
                case OP_STDDEV_POP: nsfx = "_stddev_pop"; nslen = 11; break;
                case OP_VAR:        nsfx = "_var";        nslen = 4; break;
                case OP_VAR_POP:    nsfx = "_var_pop";    nslen = 8; break;
            }
            memcpy(nbuf + np, nsfx, nslen);
            name_id = ray_sym_intern(nbuf, (size_t)np + nslen);
        }
        *result = ray_table_add_col(*result, name_id, new_col);
        ray_release(new_col);
    }
}

/* Bitmask for which accumulator arrays are actually needed */
#define DA_NEED_SUM   0x01  /* da_val_t sum array */
#define DA_NEED_MIN   0x02  /* da_val_t min_val array */
#define DA_NEED_MAX   0x04  /* da_val_t max_val array */
#define DA_NEED_COUNT 0x08  /* count array */
#define DA_NEED_SUMSQ 0x10  /* sumsq_f64 array (for STDDEV/VAR) */

typedef struct {
    da_accum_t*    accums;
    uint32_t       n_accums;     /* number of accumulator sets (may < pool workers) */
    void**         key_ptrs;     /* key data pointers [n_keys] */
    int8_t*        key_types;    /* key type codes [n_keys] */
    uint8_t*       key_attrs;    /* key attrs for RAY_SYM width [n_keys] */
    uint8_t*       key_esz;      /* pre-computed per-key elem size [n_keys] */
    int64_t*       key_mins;     /* per-key minimum [n_keys] */
    int64_t*       key_strides;  /* per-key stride [n_keys] */
    uint8_t        n_keys;
    void**         agg_ptrs;
    int8_t*        agg_types;
    uint16_t*      agg_ops;      /* per-agg operation code */
    uint8_t        n_aggs;
    uint8_t        need_flags;   /* DA_NEED_* bitmask */
    uint32_t       agg_f64_mask; /* bitmask: bit a set if agg[a] is RAY_F64 */
    bool           all_sum;      /* true when all ops are SUM/AVG/COUNT (no MIN/MAX/FIRST/LAST) */
    uint32_t       n_slots;
    const int64_t* match_idx;    /* NULL = no selection */
} da_ctx_t;

/* Composite GID from multi-key.  Arithmetic overflow is prevented in practice
 * by the DA budget check (DA_PER_WORKER_MAX) which limits total_slots to 262K. */
static inline int32_t da_composite_gid(da_ctx_t* c, int64_t r) {
    int32_t gid = 0;
    for (uint8_t k = 0; k < c->n_keys; k++) {
        int64_t val = read_by_esz(c->key_ptrs[k], r, c->key_esz[k]);
        gid += (int32_t)((val - c->key_mins[k]) * c->key_strides[k]);
    }
    return gid;
}

/* Typed composite GID: eliminates per-element switch when all keys share width */
#define DEFINE_DA_COMPOSITE_GID_TYPED(SUFFIX, KTYPE) \
static inline int32_t da_composite_gid_##SUFFIX(da_ctx_t* c, int64_t r) { \
    int32_t gid = 0; \
    for (uint8_t k = 0; k < c->n_keys; k++) { \
        int64_t val = (int64_t)((const KTYPE*)c->key_ptrs[k])[r]; \
        gid += (int32_t)((val - c->key_mins[k]) * c->key_strides[k]); \
    } \
    return gid; \
}
DEFINE_DA_COMPOSITE_GID_TYPED(u8,  uint8_t)
DEFINE_DA_COMPOSITE_GID_TYPED(u16, uint16_t)
DEFINE_DA_COMPOSITE_GID_TYPED(u32, uint32_t)
DEFINE_DA_COMPOSITE_GID_TYPED(i64, int64_t)
#undef DEFINE_DA_COMPOSITE_GID_TYPED

static inline void da_read_val(const void* ptr, int8_t type, uint8_t attrs,
                               int64_t r, double* out_f64, int64_t* out_i64) {
    if (type == RAY_F64) {
        *out_f64 = ((const double*)ptr)[r];
        *out_i64 = (int64_t)*out_f64;
    } else {
        *out_i64 = read_col_i64(ptr, r, type, attrs);
        *out_f64 = (double)*out_i64;
    }
}

/* Materialize a scalar (atom or len-1 vector) into a full-length vector so
 * group-aggregation loops can read row-wise without out-of-bounds access. */
static ray_t* materialize_broadcast_input(ray_t* src, int64_t nrows) {
    if (!src || RAY_IS_ERR(src) || nrows < 0) return NULL;

    int8_t out_type = ray_is_atom(src) ? (int8_t)-src->type : src->type;
    if (out_type <= 0 || out_type >= RAY_TYPE_COUNT) return NULL;

    ray_t* out = ray_vec_new(out_type, nrows);
    if (!out || RAY_IS_ERR(out)) return out;
    out->len = nrows;
    if (nrows == 0) return out;

    if (!ray_is_atom(src)) {
        uint8_t esz = col_esz(src);
        const char* s = (const char*)ray_data(src);
        char* d = (char*)ray_data(out);
        for (int64_t i = 0; i < nrows; i++)
            memcpy(d + (size_t)i * esz, s, esz);
        return out;
    }

    switch (src->type) {
        case -RAY_F64: {
            double v = src->f64;
            for (int64_t i = 0; i < nrows; i++) ((double*)ray_data(out))[i] = v;
            return out;
        }
        case -RAY_I64:
        case -RAY_SYM:
        case -RAY_TIMESTAMP: {
            int64_t v = src->i64;
            for (int64_t i = 0; i < nrows; i++) ((int64_t*)ray_data(out))[i] = v;
            return out;
        }
        case -RAY_DATE:
        case -RAY_TIME: {
            int32_t v = (int32_t)src->i64;
            for (int64_t i = 0; i < nrows; i++) ((int32_t*)ray_data(out))[i] = v;
            return out;
        }
        case -RAY_I32: {
            int32_t v = src->i32;
            for (int64_t i = 0; i < nrows; i++) ((int32_t*)ray_data(out))[i] = v;
            return out;
        }
        case -RAY_I16: {
            int16_t v = src->i16;
            for (int64_t i = 0; i < nrows; i++) ((int16_t*)ray_data(out))[i] = v;
            return out;
        }
        case -RAY_U8:
        case -RAY_BOOL: {
            uint8_t v = src->u8;
            for (int64_t i = 0; i < nrows; i++) ((uint8_t*)ray_data(out))[i] = v;
            return out;
        }
        default:
            ray_release(out);
            return NULL;
    }
}

/* ---- Scalar aggregate (n_keys==0): one flat scan, no GID, no hash ---- */
typedef struct {
    void**         agg_ptrs;
    int8_t*        agg_types;
    uint16_t*      agg_ops;
    agg_linear_t*  agg_linear;
    uint8_t        n_aggs;
    uint8_t        need_flags;
    const int64_t* match_idx;    /* NULL = no selection */
    /* per-worker accumulators (1 slot each) */
    da_accum_t*    accums;
    uint32_t       n_accums;
} scalar_ctx_t;

static inline int64_t scalar_i64_at(const void* ptr, int8_t type, int64_t r) {
    return read_col_i64(ptr, r, type, 0);  /* attrs=0: agg columns are numeric, never SYM */
}

/* Tight SIMD-friendly loop for single SUM/AVG on i64 (no mask).
 * Note: int64 sum can overflow; caller responsibility to use appropriate types. */
static void scalar_sum_i64_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    scalar_ctx_t* c = (scalar_ctx_t*)ctx;
    da_accum_t* acc = &c->accums[worker_id];
    const int64_t* restrict data = (const int64_t*)c->agg_ptrs[0];
    int64_t sum = 0;
    for (int64_t r = start; r < end; r++)
        sum += data[r];
    acc->sum[0].i += sum;
    acc->count[0] += end - start;
}

/* Tight SIMD-friendly loop for single SUM/AVG on f64 (no mask) */
static void scalar_sum_f64_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    scalar_ctx_t* c = (scalar_ctx_t*)ctx;
    da_accum_t* acc = &c->accums[worker_id];
    const double* restrict data = (const double*)c->agg_ptrs[0];
    double sum = 0.0;
    for (int64_t r = start; r < end; r++)
        sum += data[r];
    acc->sum[0].f += sum;
    acc->count[0] += end - start;
}

/* Tight loop for single SUM/AVG on integer linear expression (no mask). */
static void scalar_sum_linear_i64_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    scalar_ctx_t* c = (scalar_ctx_t*)ctx;
    da_accum_t* acc = &c->accums[worker_id];
    const agg_linear_t* lin = &c->agg_linear[0];
    int64_t n = end - start;

    int64_t sum = lin->bias_i64 * n;
    for (uint8_t t = 0; t < lin->n_terms; t++) {
        int64_t coeff = lin->coeff_i64[t];
        if (coeff == 0) continue;
        const void* ptr = lin->term_ptrs[t];
        int8_t type = lin->term_types[t];
        int64_t term_sum = 0;
        for (int64_t r = start; r < end; r++)
            term_sum += scalar_i64_at(ptr, type, r);
        sum += coeff * term_sum;
    }

    acc->sum[0].i += sum;
    acc->count[0] += n;
}

/* Generic scalar accumulation: handles all ops, all types, mask */
/* Inner scalar accumulation for a single row */
static inline void scalar_accum_row(scalar_ctx_t* c, da_accum_t* acc, int64_t r) {
    uint8_t n_aggs = c->n_aggs;
    acc->count[0]++;
    for (uint8_t a = 0; a < n_aggs; a++) {
        double fv; int64_t iv;
        if (c->agg_linear && c->agg_linear[a].enabled) {
            const agg_linear_t* lin = &c->agg_linear[a];
            iv = lin->bias_i64;
            for (uint8_t t = 0; t < lin->n_terms; t++) {
                iv += lin->coeff_i64[t] *
                      scalar_i64_at(lin->term_ptrs[t], lin->term_types[t], r);
            }
            fv = (double)iv;
        } else {
            if (!c->agg_ptrs[a]) continue;
            da_read_val(c->agg_ptrs[a], c->agg_types[a], 0, r, &fv, &iv);
        }
        uint16_t op = c->agg_ops[a];
        bool is_f = (c->agg_types[a] == RAY_F64);
        if (op == OP_SUM || op == OP_AVG || op == OP_STDDEV || op == OP_STDDEV_POP || op == OP_VAR || op == OP_VAR_POP) {
            if (is_f) acc->sum[a].f += fv;
            else acc->sum[a].i += iv;
            if (acc->sumsq_f64) acc->sumsq_f64[a] += fv * fv;
        } else if (op == OP_FIRST) {
            if (acc->count[0] == 1) {
                if (is_f) acc->sum[a].f = fv; else acc->sum[a].i = iv;
            }
        } else if (op == OP_LAST) {
            if (is_f) acc->sum[a].f = fv; else acc->sum[a].i = iv;
        } else if (op == OP_MIN) {
            if (is_f) { if (fv < acc->min_val[a].f) acc->min_val[a].f = fv; }
            else      { if (iv < acc->min_val[a].i) acc->min_val[a].i = iv; }
        } else if (op == OP_MAX) {
            if (is_f) { if (fv > acc->max_val[a].f) acc->max_val[a].f = fv; }
            else      { if (iv > acc->max_val[a].i) acc->max_val[a].i = iv; }
        }
    }
}

static void scalar_accum_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    scalar_ctx_t* c = (scalar_ctx_t*)ctx;
    da_accum_t* acc = &c->accums[worker_id];
    const int64_t* match_idx = c->match_idx;

    for (int64_t i = start; i < end; i++) {
        int64_t r = match_idx ? match_idx[i] : i;
        scalar_accum_row(c, acc, r);
    }
}

/* Inner DA accumulation for a single row — shared by single-key and multi-key paths.
 * Fast path for SUM/AVG-only queries: eliminates op-code dispatch and da_read_val
 * dual-write overhead.  The branch on c->all_sum is perfectly predicted (invariant
 * across all rows). */
static inline void da_accum_row(da_ctx_t* c, da_accum_t* acc, int32_t gid, int64_t r) {
    uint8_t n_aggs = c->n_aggs;
    acc->count[gid]++;
    size_t base = (size_t)gid * n_aggs;

    if (RAY_LIKELY(c->all_sum)) {
        /* SUM/AVG/COUNT fast path — no op-code dispatch, typed read only.
         * COUNT-only queries have acc->sum==NULL; count[gid]++ above suffices. */
        if (!acc->sum) return;
        uint32_t f64m = c->agg_f64_mask;
        for (uint8_t a = 0; a < n_aggs; a++) {
            if (!c->agg_ptrs[a]) continue;
            size_t idx = base + a;
            if (f64m & (1u << a))
                acc->sum[idx].f += ((const double*)c->agg_ptrs[a])[r];
            else
                acc->sum[idx].i += read_col_i64(c->agg_ptrs[a], r,
                                                c->agg_types[a], 0);
        }
        return;
    }

    for (uint8_t a = 0; a < n_aggs; a++) {
        if (!c->agg_ptrs[a]) continue;
        size_t idx = base + a;
        double fv; int64_t iv;
        da_read_val(c->agg_ptrs[a], c->agg_types[a], 0, r, &fv, &iv);
        uint16_t op = c->agg_ops[a];
        if (op == OP_SUM || op == OP_AVG || op == OP_STDDEV || op == OP_STDDEV_POP || op == OP_VAR || op == OP_VAR_POP) {
            if (c->agg_types[a] == RAY_F64) acc->sum[idx].f += fv;
            else acc->sum[idx].i = (int64_t)((uint64_t)acc->sum[idx].i + (uint64_t)iv);
            if (acc->sumsq_f64) acc->sumsq_f64[idx] += fv * fv;
        } else if (op == OP_FIRST) {
            if (acc->count[gid] == 1) {
                if (c->agg_types[a] == RAY_F64) acc->sum[idx].f = fv;
                else acc->sum[idx].i = iv;
            }
        } else if (op == OP_LAST) {
            if (c->agg_types[a] == RAY_F64) acc->sum[idx].f = fv;
            else acc->sum[idx].i = iv;
        } else if (op == OP_MIN) {
            if (c->agg_types[a] == RAY_F64) {
                if (fv < acc->min_val[idx].f) acc->min_val[idx].f = fv;
            } else {
                if (iv < acc->min_val[idx].i) acc->min_val[idx].i = iv;
            }
        } else if (op == OP_MAX) {
            if (c->agg_types[a] == RAY_F64) {
                if (fv > acc->max_val[idx].f) acc->max_val[idx].f = fv;
            } else {
                if (iv > acc->max_val[idx].i) acc->max_val[idx].i = iv;
            }
        }
    }
}

static void da_accum_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    da_ctx_t* c = (da_ctx_t*)ctx;
    da_accum_t* acc = &c->accums[worker_id];
    uint8_t n_aggs = c->n_aggs;
    uint8_t n_keys = c->n_keys;
    const int64_t* match_idx = c->match_idx;

    /* Fast path: single key — avoid composite GID loop overhead.
     * Templated by key element size: the entire loop is stamped out per width
     * so the compiler generates direct movzbl/movzwl/movl/movq — zero dispatch. */
    #define DA_PF_DIST 8
    #define DA_SINGLE_KEY_LOOP(KTYPE, KCAST) \
    do { \
        const KTYPE* kp = (const KTYPE*)c->key_ptrs[0]; \
        int64_t kmin = c->key_mins[0]; \
        bool da_pf = c->n_slots >= 4096; \
        for (int64_t i = start; i < end; i++) { \
            int64_t r = match_idx ? match_idx[i] : i; \
            if (da_pf && RAY_LIKELY(i + DA_PF_DIST < end)) { \
                int64_t pf_r = match_idx ? match_idx[i + DA_PF_DIST] : (i + DA_PF_DIST); \
                int64_t pfk = (int64_t)KCAST kp[pf_r]; \
                __builtin_prefetch(&acc->count[(int32_t)(pfk - kmin)], 1, 1); \
                if (acc->sum) __builtin_prefetch( \
                    &acc->sum[(size_t)(int32_t)(pfk - kmin) * n_aggs], 1, 1); \
            } \
            int64_t kv = (int64_t)KCAST kp[r]; \
            da_accum_row(c, acc, (int32_t)(kv - kmin), r); \
        } \
    } while (0)

    if (n_keys == 1) {
        switch (c->key_esz[0]) {
        case 1: DA_SINGLE_KEY_LOOP(uint8_t, ); break;
        case 2: DA_SINGLE_KEY_LOOP(uint16_t, ); break;
        case 4: DA_SINGLE_KEY_LOOP(uint32_t, (int64_t)); break;
        default: DA_SINGLE_KEY_LOOP(int64_t, ); break;
        }
        #undef DA_SINGLE_KEY_LOOP
        return;
    }

    /* Multi-key composite GID — typed inner loop eliminates read_by_esz switch.
     * When all keys share the same element size, use da_composite_gid_XX(). */
    #define DA_MULTI_KEY_LOOP(GID_FN) \
    do { \
        bool _da_pf = c->n_slots >= 4096; \
        for (int64_t i = start; i < end; i++) { \
            int64_t r = match_idx ? match_idx[i] : i; \
            if (_da_pf && RAY_LIKELY(i + DA_PF_DIST < end)) { \
                int64_t pf_r = match_idx ? match_idx[i + DA_PF_DIST] : (i + DA_PF_DIST); \
                int32_t pf_gid = GID_FN(pf_r); \
                __builtin_prefetch(&acc->count[pf_gid], 1, 1); \
                if (acc->sum) __builtin_prefetch(&acc->sum[(size_t)pf_gid * n_aggs], 1, 1); \
            } \
            da_accum_row(c, acc, GID_FN(r), r); \
        } \
    } while (0)

    /* Check if all keys share the same element size */
    bool uniform_esz = true;
    for (uint8_t k = 1; k < n_keys; k++)
        if (c->key_esz[k] != c->key_esz[0]) { uniform_esz = false; break; }

    if (uniform_esz) {
        switch (c->key_esz[0]) {
        case 1:
#define GID_FN(R) da_composite_gid_u8(c, (R))
            DA_MULTI_KEY_LOOP(GID_FN);
#undef GID_FN
            break;
        case 2:
#define GID_FN(R) da_composite_gid_u16(c, (R))
            DA_MULTI_KEY_LOOP(GID_FN);
#undef GID_FN
            break;
        case 4:
#define GID_FN(R) da_composite_gid_u32(c, (R))
            DA_MULTI_KEY_LOOP(GID_FN);
#undef GID_FN
            break;
        default:
#define GID_FN(R) da_composite_gid_i64(c, (R))
            DA_MULTI_KEY_LOOP(GID_FN);
#undef GID_FN
            break;
        }
    } else {
#define GID_FN(R) da_composite_gid(c, (R))
        DA_MULTI_KEY_LOOP(GID_FN);
#undef GID_FN
    }
    #undef DA_MULTI_KEY_LOOP
    #undef DA_PF_DIST
}

/* Parallel DA merge: merge per-worker accumulators into accums[0] by
 * dispatching disjoint slot ranges across pool workers. */
typedef struct {
    da_accum_t* accums;
    uint32_t    n_src_workers; /* number of source workers to merge (1..n) */
    uint8_t     need_flags;
    uint8_t     n_aggs;
    const int8_t* agg_types;  /* per-agg value type (for typed merge) */
    const uint16_t* agg_ops;  /* per-agg opcode (for FIRST/LAST merge) */
} da_merge_ctx_t;

static void da_merge_fn(void* ctx, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    da_merge_ctx_t* c = (da_merge_ctx_t*)ctx;
    da_accum_t* merged = &c->accums[0];
    uint8_t n_aggs = c->n_aggs;
    const int8_t* agg_types = c->agg_types;
    for (uint32_t w = 1; w < c->n_src_workers; w++) {
        da_accum_t* wa = &c->accums[w];
        for (int64_t s = start; s < end; s++) {
            size_t base = (size_t)s * n_aggs;
            if (c->need_flags & DA_NEED_SUMSQ) {
                for (uint8_t a = 0; a < n_aggs; a++)
                    merged->sumsq_f64[base + a] += wa->sumsq_f64[base + a];
            }
            if (c->need_flags & DA_NEED_SUM) {
                for (uint8_t a = 0; a < n_aggs; a++) {
                    size_t idx = base + a;
                    uint16_t aop = c->agg_ops ? c->agg_ops[a] : OP_SUM;
                    if (aop == OP_FIRST) {
                        /* Keep worker 0 value; take from w only if merged has no data */
                        if (merged->count[s] == 0 && wa->count[s] > 0)
                            merged->sum[idx] = wa->sum[idx];
                    } else if (aop == OP_LAST) {
                        /* Overwrite with last worker that has data */
                        if (wa->count[s] > 0)
                            merged->sum[idx] = wa->sum[idx];
                    } else if (agg_types[a] == RAY_F64)
                        merged->sum[idx].f += wa->sum[idx].f;
                    else
                        merged->sum[idx].i += wa->sum[idx].i;
                }
            }
            if (c->need_flags & DA_NEED_MIN) {
                for (uint8_t a = 0; a < n_aggs; a++) {
                    size_t idx = base + a;
                    if (agg_types[a] == RAY_F64) {
                        if (wa->min_val[idx].f < merged->min_val[idx].f)
                            merged->min_val[idx].f = wa->min_val[idx].f;
                    } else {
                        if (wa->min_val[idx].i < merged->min_val[idx].i)
                            merged->min_val[idx].i = wa->min_val[idx].i;
                    }
                }
            }
            if (c->need_flags & DA_NEED_MAX) {
                for (uint8_t a = 0; a < n_aggs; a++) {
                    size_t idx = base + a;
                    if (agg_types[a] == RAY_F64) {
                        if (wa->max_val[idx].f > merged->max_val[idx].f)
                            merged->max_val[idx].f = wa->max_val[idx].f;
                    } else {
                        if (wa->max_val[idx].i > merged->max_val[idx].i)
                            merged->max_val[idx].i = wa->max_val[idx].i;
                    }
                }
            }
            merged->count[s] += wa->count[s];
        }
    }
}

/* ============================================================================
 * Partition-aware group-by: detect parted columns, concatenate segments into
 * a flat table, then run standard exec_group once.
 * ============================================================================ */
ray_t* exec_group(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                  int64_t group_limit); /* forward decl */

/* Forward declaration — defined below exec_group */
static ray_t* exec_group_per_partition(ray_t* parted_tbl, ray_op_ext_t* ext,
                                       int32_t n_parts, const int64_t* key_syms,
                                       const int64_t* agg_syms, int has_avg,
                                       int has_stddev, int64_t group_limit);

/* --------------------------------------------------------------------------
 * exec_group_parted — dispatch per-partition or concat-fallback
 * -------------------------------------------------------------------------- */
static ray_t* exec_group_parted(ray_graph_t* g, ray_op_t* op, ray_t* parted_tbl,
                               int64_t group_limit) {
    int64_t ncols = ray_table_ncols(parted_tbl);
    if (ncols <= 0) return ray_error("nyi", NULL);

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    uint8_t n_keys = ext->n_keys;
    uint8_t n_aggs = ext->n_aggs;

    /* Find partition count and total rows from first parted column */
    int32_t n_parts = 0;
    int64_t total_rows = 0;
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(parted_tbl, c);
        if (col && RAY_IS_PARTED(col->type)) {
            n_parts = (int32_t)col->len;
            total_rows = ray_parted_nrows(col);
            break;
        }
    }
    if (n_parts <= 0 || total_rows <= 0) return ray_error("nyi", NULL);

    /* Check eligibility for per-partition exec + merge:
     * - All keys and agg inputs must be simple SCANs
     * - Supported agg ops: SUM, COUNT, MIN, MAX, AVG, FIRST, LAST,
     *   STDDEV, STDDEV_POP, VAR, VAR_POP */
    int can_partition = 1;
    int has_avg = 0;
    int has_stddev = 0;
    int64_t key_syms[8];
    for (uint8_t k = 0; k < n_keys && can_partition; k++) {
        ray_op_ext_t* ke = find_ext(g, ext->keys[k]->id);
        if (!ke || ke->base.opcode != OP_SCAN) { can_partition = 0; break; }
        key_syms[k] = ke->sym;
    }
    int64_t agg_syms[8];
    for (uint8_t a = 0; a < n_aggs && can_partition; a++) {
        uint16_t aop = ext->agg_ops[a];
        if (aop != OP_SUM && aop != OP_COUNT && aop != OP_MIN &&
            aop != OP_MAX && aop != OP_AVG && aop != OP_FIRST &&
            aop != OP_LAST && aop != OP_STDDEV && aop != OP_STDDEV_POP &&
            aop != OP_VAR && aop != OP_VAR_POP) { can_partition = 0; break; }
        if (aop == OP_AVG) has_avg = 1;
        if (aop == OP_STDDEV || aop == OP_STDDEV_POP ||
            aop == OP_VAR || aop == OP_VAR_POP) has_stddev = 1;
        ray_op_ext_t* ae = find_ext(g, ext->agg_ins[a]->id);
        if (!ae || ae->base.opcode != OP_SCAN) { can_partition = 0; break; }
        agg_syms[a] = ae->sym;
    }

    /* Cardinality gate: estimate groups from first partition.
     * Per-partition only wins when #groups << partition_size. */
    if (can_partition) {
        int64_t rows_per_part = total_rows / n_parts;
        int64_t est_groups = 1;
        for (uint8_t k = 0; k < n_keys; k++) {
            ray_t* pcol = ray_table_get_col(parted_tbl, key_syms[k]);
            if (!pcol) { est_groups = rows_per_part; break; }
            /* MAPCOMMON key: constant per partition — excluded from
             * per-partition sub-GROUP-BY, contributes 0 to cardinality. */
            if (pcol->type == RAY_MAPCOMMON) { continue; }
            if (!RAY_IS_PARTED(pcol->type)) { est_groups = rows_per_part; break; }
            ray_t* seg0 = ((ray_t**)ray_data(pcol))[0];
            if (!seg0 || seg0->len <= 0) { est_groups = rows_per_part; break; }
            int8_t bt = RAY_PARTED_BASETYPE(pcol->type);
            int64_t card;
            if (RAY_IS_SYM(bt)) {
                uint32_t sym_n = ray_sym_count();
                if (sym_n == 0 || sym_n > 4194304) { est_groups = rows_per_part; break; }
                size_t bwords = ((size_t)sym_n + 63) / 64;
                ray_t* bits_hdr = NULL;
                uint64_t* bits = (uint64_t*)scratch_calloc(&bits_hdr, bwords * 8);
                if (!bits) { est_groups = rows_per_part; break; }
                for (int64_t r = 0; r < seg0->len; r++) {
                    uint32_t id = (uint32_t)ray_read_sym(ray_data(seg0), r, seg0->type, seg0->attrs);
                    bits[id / 64] |= 1ULL << (id % 64);
                }
                card = 0;
                for (size_t i = 0; i < bwords; i++)
                    card += __builtin_popcountll(bits[i]);
                scratch_free(bits_hdr);
            } else if (bt == RAY_I64) {
                const int64_t* v = (const int64_t*)ray_data(seg0);
                int64_t lo = v[0], hi = v[0];
                for (int64_t r = 1; r < seg0->len; r++) {
                    if (v[r] < lo) lo = v[r];
                    if (v[r] > hi) hi = v[r];
                }
                card = hi - lo + 1;
            } else if (bt == RAY_I32) {
                const int32_t* v = (const int32_t*)ray_data(seg0);
                int32_t lo = v[0], hi = v[0];
                for (int64_t r = 1; r < seg0->len; r++) {
                    if (v[r] < lo) lo = v[r];
                    if (v[r] > hi) hi = v[r];
                }
                card = (int64_t)(hi - lo + 1);
            } else {
                card = seg0->len;
            }
            est_groups *= card;
            if (est_groups > rows_per_part) { est_groups = rows_per_part; break; }
        }
        /* Block per-partition when cardinality is high AND the concat
         * fallback would fit in memory (< 4 GB estimated).  When concat is
         * too large, per-partition with batched merge is the only option. */
        int64_t concat_bytes = total_rows * 8LL * (int64_t)(n_keys + n_aggs);
        if (est_groups * 100 > rows_per_part &&
            concat_bytes < 4LL * 1024 * 1024 * 1024)
            can_partition = 0;
    }

    /* Try per-partition path (separate noinline function to avoid I-cache pressure) */
    if (can_partition) {
        ray_t* result = exec_group_per_partition(parted_tbl, ext, n_parts,
                                                 key_syms, agg_syms, has_avg,
                                                 has_stddev, group_limit);
        if (result) return result;
        /* NULL = per-partition failed, fall through to concat */
    }

    /* ---- Concat fallback ---- */
    /* ---- Concat-only-needed-columns fallback ----
     * Used when query has AVG or expression keys/aggs.
     * Only concatenates the columns actually referenced by the GROUP BY. */
    {
        /* Collect needed column sym IDs (keys + agg inputs) */
        int64_t needed[16];
        int n_needed = 0;
        for (uint8_t k = 0; k < n_keys; k++) {
            ray_op_ext_t* ke = find_ext(g, ext->keys[k]->id);
            if (ke && ke->base.opcode == OP_SCAN) {
                int dup = 0;
                for (int i = 0; i < n_needed; i++)
                    if (needed[i] == ke->sym) { dup = 1; break; }
                if (!dup) needed[n_needed++] = ke->sym;
            }
        }
        for (uint8_t a = 0; a < n_aggs; a++) {
            ray_op_ext_t* ae = find_ext(g, ext->agg_ins[a]->id);
            if (ae && ae->base.opcode == OP_SCAN) {
                int dup = 0;
                for (int i = 0; i < n_needed; i++)
                    if (needed[i] == ae->sym) { dup = 1; break; }
                if (!dup) needed[n_needed++] = ae->sym;
            } else {
                /* Expression agg input — need all columns for evaluation.
                 * Fall back to copying everything. */
                n_needed = 0;
                break;
            }
        }

        /* Build flat table with only needed columns (or all if n_needed==0) */
        ray_t* flat_tbl = ray_table_new(n_needed > 0 ? (int64_t)n_needed : ncols);
        if (!flat_tbl || RAY_IS_ERR(flat_tbl)) return flat_tbl;

        int64_t cols_to_iter = n_needed > 0 ? (int64_t)n_needed : ncols;
        for (int64_t ci = 0; ci < cols_to_iter; ci++) {
            ray_t* col;
            int64_t name_id;
            if (n_needed > 0) {
                col = ray_table_get_col(parted_tbl, needed[ci]);
                name_id = needed[ci];
            } else {
                col = ray_table_get_col_idx(parted_tbl, ci);
                name_id = ray_table_col_name(parted_tbl, ci);
            }
            if (!col) continue;
            if (col->type == RAY_MAPCOMMON) {
                ray_t* mc_flat = materialize_mapcommon(col);
                if (mc_flat && !RAY_IS_ERR(mc_flat)) {
                    flat_tbl = ray_table_add_col(flat_tbl, name_id, mc_flat);
                    ray_release(mc_flat);
                }
                continue;
            }

            if (!RAY_IS_PARTED(col->type)) {
                ray_retain(col);
                flat_tbl = ray_table_add_col(flat_tbl, name_id, col);
                ray_release(col);
                continue;
            }

            int8_t base_type = (int8_t)RAY_PARTED_BASETYPE(col->type);
            ray_t** segs = (ray_t**)ray_data(col);
            ray_t* flat;

            if (base_type == RAY_STR) {
                flat = parted_flatten_str(segs, col->len, total_rows);
            } else {
                uint8_t base_attrs = (base_type == RAY_SYM)
                                   ? parted_first_attrs(segs, col->len) : 0;
                flat = typed_vec_new(base_type, base_attrs, total_rows);
                if (!flat || RAY_IS_ERR(flat)) {
                    ray_release(flat_tbl);
                    return ray_error("oom", NULL);
                }
                flat->len = total_rows;

                size_t elem_size = (size_t)ray_sym_elem_size(base_type, base_attrs);
                int64_t offset = 0;
                for (int32_t p = 0; p < n_parts; p++) {
                    ray_t* seg = segs[p];
                    if (!seg || seg->len <= 0) continue;
                    if (parted_seg_esz_ok(seg, base_type, (uint8_t)elem_size)) {
                        memcpy((char*)ray_data(flat) + (size_t)offset * elem_size,
                               ray_data(seg), (size_t)seg->len * elem_size);
                    } else {
                        memset((char*)ray_data(flat) + (size_t)offset * elem_size,
                               0, (size_t)seg->len * elem_size);
                    }
                    offset += seg->len;
                }
            }
            if (!flat || RAY_IS_ERR(flat)) {
                ray_release(flat_tbl);
                return ray_error("oom", NULL);
            }

            flat_tbl = ray_table_add_col(flat_tbl, name_id, flat);
            ray_release(flat);
        }

        ray_t* saved = g->table;
        g->table = flat_tbl;
        ray_t* result = exec_group(g, op, flat_tbl, 0);
        g->table = saved;
        ray_release(flat_tbl);
        return result;
    }
}

ray_t* exec_group(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                  int64_t group_limit) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;

    /* Selection-shape guard — runs BEFORE any fast path (parted
     * dispatch, factorized shortcut) so every exec_group code path
     * sees the same validated selection state.  A mismatch here
     * indicates a graph-construction bug: the caller installed a
     * selection that was built for a different table shape, and
     * silently ignoring it would return unfiltered results. */
    if (g->selection) {
        ray_rowsel_t* sm = ray_rowsel_meta(g->selection);
        int64_t tbl_nrows = ray_table_nrows(tbl);
        if (sm->nrows != tbl_nrows)
            return ray_error("domain",
                "exec_group: selection nrows mismatch (sel=%lld tbl=%lld)",
                (long long)sm->nrows, (long long)tbl_nrows);
    }

    /* Parted dispatch: detect parted input columns */
    {
        int64_t nc = ray_table_ncols(tbl);
        for (int64_t c = 0; c < nc; c++) {
            ray_t* col = ray_table_get_col_idx(tbl, c);
            if (col && (RAY_IS_PARTED(col->type) || col->type == RAY_MAPCOMMON)) {
                /* exec_group_parted has no rowsel plumbing — a
                 * selection in flight would be silently ignored.
                 * Reject rather than produce unfiltered results. */
                if (g->selection)
                    return ray_error("nyi",
                        "GROUP BY with selection on parted table");
                return exec_group_parted(g, op, tbl, group_limit);
            }
        }
    }

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    int64_t nrows = ray_table_nrows(tbl);
    uint8_t n_keys = ext->n_keys;
    uint8_t n_aggs = ext->n_aggs;

    /* Factorized shortcut: if input is a factorized expand result with
     * (_src, _count) columns, and GROUP BY _src with COUNT/SUM(_count),
     * return the pre-aggregated table directly without re-scanning.
     *
     * Interaction with g->selection: the factorized _count column
     * encodes weighted counts, so COUNT(*) must SUM _count to get
     * the true row count and SUM(_count) is the same thing.
     * Neither the shortcut (returns verbatim, no filter) nor the
     * main path (counts rows of the _src table, ignoring _count)
     * knows how to apply a row filter while preserving those
     * semantics.
     *
     * Other agg shapes — SUM/AVG/MIN/MAX of a non-_count column,
     * etc. — don't rely on the factorized weighting; the main
     * path handles them correctly with the selection installed.
     * So the rejection must mirror the shortcut's exact
     * compatibility check (all aggs are COUNT or SUM(_count)),
     * not just the presence of a _count column. */
    if (g->selection && n_keys == 1 && n_aggs > 0 && nrows > 0) {
        int64_t cnt_sym_probe = ray_sym_intern("_count", 6);
        ray_t*  cnt_col_probe = ray_table_get_col(tbl, cnt_sym_probe);
        ray_op_ext_t* key_ext_probe = find_ext(g, ext->keys[0]->id);
        int64_t src_sym_probe = ray_sym_intern("_src", 4);
        if (cnt_col_probe && cnt_col_probe->type == RAY_I64 &&
            key_ext_probe && key_ext_probe->base.opcode == OP_SCAN &&
            key_ext_probe->sym == src_sym_probe) {
            /* Reject on ANY agg whose semantics depend on the
             * factorized _count weighting: COUNT(*) counts
             * underlying source rows (not _src table rows) and
             * SUM(_count) is equivalent.  Even if only one agg in
             * a mixed query needs weighting, the main path can't
             * handle it correctly, so fail the whole query rather
             * than return a mix of right and wrong columns.
             *
             * Special case: an empty selection (total_pass == 0)
             * means every row was filtered out, so the result is
             * an empty group set regardless of which aggs are
             * involved.  The main path handles this correctly
             * even for count-weighted aggs because n_scan == 0
             * produces no group rows at all.  Let it fall
             * through. */
            ray_rowsel_t* sm = ray_rowsel_meta(g->selection);
            if (sm->total_pass > 0) {
                bool needs_weighting = false;
                for (uint8_t a = 0; a < n_aggs; a++) {
                    uint16_t aop = ext->agg_ops[a];
                    ray_op_ext_t* agg_ext = find_ext(g, ext->agg_ins[a]->id);
                    if (aop == OP_COUNT) { needs_weighting = true; break; }
                    if (aop == OP_SUM && agg_ext &&
                        agg_ext->base.opcode == OP_SCAN &&
                        agg_ext->sym == cnt_sym_probe) {
                        needs_weighting = true; break;
                    }
                }
                if (needs_weighting)
                    return ray_error("nyi",
                        "GROUP BY with selection on factorized expand result "
                        "(COUNT/SUM(_count) semantics)");
            }
        }
    }
    if (!g->selection && n_keys == 1 && n_aggs > 0 && nrows > 0) {
        int64_t cnt_sym = ray_sym_intern("_count", 6);
        ray_t* cnt_col = ray_table_get_col(tbl, cnt_sym);
        if (cnt_col && cnt_col->type == RAY_I64) {
            ray_op_ext_t* key_ext = find_ext(g, ext->keys[0]->id);
            int64_t src_sym = ray_sym_intern("_src", 4);
            if (key_ext && key_ext->base.opcode == OP_SCAN &&
                key_ext->sym == src_sym) {
                /* Verify all aggs are compatible with factorized data:
                 * COUNT(*) → use _count directly
                 * SUM(_count) → use _count directly */
                bool all_compat = true;
                for (uint8_t a = 0; a < n_aggs; a++) {
                    uint16_t aop = ext->agg_ops[a];
                    ray_op_ext_t* agg_ext = find_ext(g, ext->agg_ins[a]->id);
                    if (aop == OP_COUNT) continue;
                    if (aop == OP_SUM && agg_ext &&
                        agg_ext->base.opcode == OP_SCAN &&
                        agg_ext->sym == cnt_sym) continue;
                    all_compat = false;
                    break;
                }
                if (all_compat) {
                    /* The factorized table already has one row per group.
                     * Build result with _src key + agg columns from _count. */
                    ray_t* src_col = ray_table_get_col(tbl, src_sym);
                    if (src_col) {
                        int64_t out_nkeys = 1;
                        int64_t out_ncols = out_nkeys + n_aggs;
                        ray_t* result = ray_table_new((int64_t)out_ncols);
                        if (!result || RAY_IS_ERR(result))
                            return ray_error("oom", NULL);
                        ray_retain(src_col);
                        ray_t* tmp_r = ray_table_add_col(result, src_sym, src_col);
                        ray_release(src_col);
                        if (!tmp_r || RAY_IS_ERR(tmp_r)) {
                            ray_release(result);
                            return ray_error("oom", NULL);
                        }
                        result = tmp_r;
                        for (uint8_t a = 0; a < n_aggs; a++) {
                            ray_retain(cnt_col);
                            int64_t agg_name = ray_sym_intern("_agg", 4);
                            if (n_aggs > 1) {
                                char buf[16];
                                int n = snprintf(buf, sizeof(buf), "_agg%d", a);
                                agg_name = ray_sym_intern(buf, (size_t)n);
                            }
                            tmp_r = ray_table_add_col(result, agg_name, cnt_col);
                            ray_release(cnt_col);
                            if (!tmp_r || RAY_IS_ERR(tmp_r)) {
                                ray_release(result);
                                return ray_error("oom", NULL);
                            }
                            result = tmp_r;
                        }
                        return result;
                    }
                }
            }
        }
    }

    if (n_keys > 8 || n_aggs > 8) return ray_error("nyi", NULL);

    /* Extract selection (rowsel) for pushdown.  Workers iterate over
     * [0, n_scan) and read row=match_idx[i].  When no selection is
     * present, match_idx is NULL and n_scan equals nrows.  The
     * match_idx_block must be released on every exec_group exit
     * path — see the various `goto cleanup` and early returns below.
     *
     * The top-of-function guard already rejected nrows mismatches,
     * so if we reach here with a selection it's guaranteed valid
     * for `tbl`. */
    ray_t* match_idx_block = NULL;
    const int64_t* match_idx = NULL;
    int64_t n_scan = nrows;
    if (g->selection) {
        match_idx_block = ray_rowsel_to_indices(g->selection);
        if (!match_idx_block) return ray_error("oom", NULL);
        match_idx = (const int64_t*)ray_data(match_idx_block);
        n_scan = ray_rowsel_meta(g->selection)->total_pass;
    }

    /* Resolve key columns (VLA — n_keys ≤ 8; use ≥1 to avoid zero-size VLA UB) */
    uint8_t vla_keys = n_keys > 0 ? n_keys : 1;
    ray_t* key_vecs[vla_keys];
    memset(key_vecs, 0, vla_keys * sizeof(ray_t*));

    uint8_t key_owned[vla_keys]; /* 1 = we allocated via exec_node, must free */
    memset(key_owned, 0, vla_keys * sizeof(uint8_t));
    for (uint8_t k = 0; k < n_keys; k++) {
        ray_op_t* key_op = ext->keys[k];
        ray_op_ext_t* key_ext = find_ext(g, key_op->id);
        if (key_ext && key_ext->base.opcode == OP_SCAN) {
            key_vecs[k] = ray_table_get_col(tbl, key_ext->sym);
        } else {
            /* Expression key (CASE WHEN etc) — evaluate against current tbl */
            ray_t* saved_table = g->table;
            g->table = tbl;
            ray_t* vec = exec_node(g, key_op);
            g->table = saved_table;
            if (vec && !RAY_IS_ERR(vec)) {
                key_vecs[k] = vec;
                key_owned[k] = 1;
            }
        }
    }

    /* Resolve agg input columns (VLA — n_aggs ≤ 8; use ≥1 to avoid zero-size VLA UB) */
    uint8_t vla_aggs = n_aggs > 0 ? n_aggs : 1;
    ray_t* agg_vecs[vla_aggs];
    uint8_t agg_owned[vla_aggs]; /* 1 = we allocated via exec_node, must free */
    agg_affine_t agg_affine[vla_aggs];
    agg_linear_t agg_linear[vla_aggs];
    memset(agg_vecs, 0, vla_aggs * sizeof(ray_t*));
    memset(agg_owned, 0, vla_aggs * sizeof(uint8_t));
    memset(agg_affine, 0, vla_aggs * sizeof(agg_affine_t));
    memset(agg_linear, 0, vla_aggs * sizeof(agg_linear_t));

    for (uint8_t a = 0; a < n_aggs; a++) {
        ray_op_t* agg_input_op = ext->agg_ins[a];
        ray_op_ext_t* agg_ext = find_ext(g, agg_input_op->id);

        /* SUM/AVG(scan +/- const): aggregate base scan and apply bias at emit. */
        uint16_t agg_kind = ext->agg_ops[a];
        if ((agg_kind == OP_SUM || agg_kind == OP_AVG) &&
            try_affine_sumavg_input(g, tbl, agg_input_op, &agg_vecs[a], &agg_affine[a])) {
            continue;
        }

        /* SUM/AVG(integer-linear expr): scalar path can aggregate directly
         * without materializing the expression vector. */
        if (n_keys == 0 && nrows > 0 &&
            (agg_kind == OP_SUM || agg_kind == OP_AVG) &&
            try_linear_sumavg_input_i64(g, tbl, agg_input_op, &agg_linear[a])) {
            continue;
        }

        if (agg_ext && agg_ext->base.opcode == OP_SCAN) {
            agg_vecs[a] = ray_table_get_col(tbl, agg_ext->sym);
        } else if (agg_ext && agg_ext->base.opcode == OP_CONST && agg_ext->literal) {
            agg_vecs[a] = agg_ext->literal;
        } else {
            /* Expression node (ADD/MUL etc) — try compiled expression first */
            ray_expr_t agg_expr;
            if (expr_compile(g, tbl, agg_input_op, &agg_expr)) {
                ray_t* vec = expr_eval_full(&agg_expr, nrows);
                if (vec && !RAY_IS_ERR(vec)) {
                    agg_vecs[a] = vec;
                    agg_owned[a] = 1;
                    continue;
                }
            }
            /* Fallback: full recursive evaluation */
            ray_t* saved_table = g->table;
            g->table = tbl;
            ray_t* vec = exec_node(g, agg_input_op);
            g->table = saved_table;
            if (vec && !RAY_IS_ERR(vec)) {
                agg_vecs[a] = vec;
                agg_owned[a] = 1;
            }
        }
    }

    /* Normalize scalar agg inputs to full-length vectors.
     * Constants and scalar sub-expressions (len=1) must be broadcast to nrows
     * before row-wise aggregation loops. */
    for (uint8_t a = 0; a < n_aggs; a++) {
        if (!agg_vecs[a] || RAY_IS_ERR(agg_vecs[a])) continue;
        if (ext->agg_ops[a] == OP_COUNT) continue; /* value is ignored for COUNT */

        bool needs_broadcast = ray_is_atom(agg_vecs[a]) ||
                               (agg_vecs[a]->type > 0 && agg_vecs[a]->len == 1 && nrows > 1);
        if (!needs_broadcast) continue;

        ray_t* bcast = materialize_broadcast_input(agg_vecs[a], nrows);
        if (!bcast || RAY_IS_ERR(bcast)) {
            for (uint8_t i = 0; i < n_aggs; i++) {
                if (agg_owned[i] && agg_vecs[i]) ray_release(agg_vecs[i]);
            }
            for (uint8_t k = 0; k < n_keys; k++) {
                if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
            }
            return bcast && RAY_IS_ERR(bcast) ? bcast : ray_error("oom", NULL);
        }

        if (agg_owned[a]) ray_release(agg_vecs[a]);
        agg_vecs[a] = bcast;
        agg_owned[a] = 1;
    }

    /* Pre-compute key metadata (VLA — n_keys ≤ 8; vla_keys ≥ 1) */
    void* key_data[vla_keys];
    int8_t key_types[vla_keys];
    uint8_t key_attrs[vla_keys];
    for (uint8_t k = 0; k < n_keys; k++) {
        if (key_vecs[k]) {
            key_data[k]  = ray_data(key_vecs[k]);
            key_types[k] = key_vecs[k]->type;
            key_attrs[k] = key_vecs[k]->attrs;
        } else {
            key_data[k]  = NULL;
            key_types[k] = 0;
            key_attrs[k] = 0;
        }
    }

    /* ---- Scalar aggregate fast path (n_keys == 0): flat vector scan ---- */
    if (n_keys == 0 && nrows > 0) {
        uint8_t need_flags = DA_NEED_COUNT;
        for (uint8_t a = 0; a < n_aggs; a++) {
            uint16_t aop = ext->agg_ops[a];
            if (aop == OP_SUM || aop == OP_AVG || aop == OP_FIRST || aop == OP_LAST)
                need_flags |= DA_NEED_SUM;
            else if (aop == OP_STDDEV || aop == OP_STDDEV_POP || aop == OP_VAR || aop == OP_VAR_POP)
                { need_flags |= DA_NEED_SUM; need_flags |= DA_NEED_SUMSQ; }
            else if (aop == OP_MIN) need_flags |= DA_NEED_MIN;
            else if (aop == OP_MAX) need_flags |= DA_NEED_MAX;
        }

        void* agg_ptrs[vla_aggs];
        int8_t agg_types[vla_aggs];
        for (uint8_t a = 0; a < n_aggs; a++) {
            if (agg_vecs[a]) {
                agg_ptrs[a]  = ray_data(agg_vecs[a]);
                agg_types[a] = agg_vecs[a]->type;
            } else {
                agg_ptrs[a]  = NULL;
                agg_types[a] = 0;
            }
        }

        ray_pool_t* sc_pool = ray_pool_get();
        uint32_t sc_n = (sc_pool && nrows >= RAY_PARALLEL_THRESHOLD)
                        ? ray_pool_total_workers(sc_pool) : 1;

        ray_t* sc_hdr;
        da_accum_t* sc_acc = (da_accum_t*)scratch_calloc(&sc_hdr,
            sc_n * sizeof(da_accum_t));
        if (!sc_acc) goto da_path;

        /* Allocate 1-slot accumulators per worker (n_aggs entries) */
        bool alloc_ok = true;
        for (uint32_t w = 0; w < sc_n; w++) {
            if (need_flags & DA_NEED_SUM) {
                sc_acc[w].sum = (da_val_t*)scratch_calloc(&sc_acc[w]._h_sum,
                    n_aggs * sizeof(da_val_t));
                if (!sc_acc[w].sum) { alloc_ok = false; break; }
            }
            if (need_flags & DA_NEED_MIN) {
                sc_acc[w].min_val = (da_val_t*)scratch_alloc(&sc_acc[w]._h_min,
                    n_aggs * sizeof(da_val_t));
                if (!sc_acc[w].min_val) { alloc_ok = false; break; }
                for (uint8_t a = 0; a < n_aggs; a++) {
                    if (agg_types[a] == RAY_F64) sc_acc[w].min_val[a].f = DBL_MAX;
                    else sc_acc[w].min_val[a].i = INT64_MAX;
                }
            }
            if (need_flags & DA_NEED_MAX) {
                sc_acc[w].max_val = (da_val_t*)scratch_alloc(&sc_acc[w]._h_max,
                    n_aggs * sizeof(da_val_t));
                if (!sc_acc[w].max_val) { alloc_ok = false; break; }
                for (uint8_t a = 0; a < n_aggs; a++) {
                    if (agg_types[a] == RAY_F64) sc_acc[w].max_val[a].f = -DBL_MAX;
                    else sc_acc[w].max_val[a].i = INT64_MIN;
                }
            }
            if (need_flags & DA_NEED_SUMSQ) {
                sc_acc[w].sumsq_f64 = (double*)scratch_calloc(&sc_acc[w]._h_sumsq,
                    n_aggs * sizeof(double));
                if (!sc_acc[w].sumsq_f64) { alloc_ok = false; break; }
            }
            sc_acc[w].count = (int64_t*)scratch_calloc(&sc_acc[w]._h_count,
                1 * sizeof(int64_t));
            if (!sc_acc[w].count) { alloc_ok = false; break; }
        }
        if (!alloc_ok) {
            for (uint32_t w = 0; w < sc_n; w++) da_accum_free(&sc_acc[w]);
            scratch_free(sc_hdr);
            goto da_path;
        }

        scalar_ctx_t sc_ctx = {
            .agg_ptrs   = agg_ptrs,
            .agg_types  = agg_types,
            .agg_ops    = ext->agg_ops,
            .agg_linear = agg_linear,
            .n_aggs     = n_aggs,
            .need_flags = need_flags,
            .match_idx  = match_idx,
            .accums     = sc_acc,
            .n_accums   = sc_n,
        };

        /* Pick specialized tight loop when possible, else generic.
         * The specialized scalar_sum_*_fn variants don't honour
         * match_idx — they read data[r] directly — so they're only
         * safe when no selection is in flight. */
        typedef void (*scalar_fn_t)(void*, uint32_t, int64_t, int64_t);
        scalar_fn_t sc_fn = scalar_accum_fn;
        if (n_aggs == 1 && !match_idx && agg_ptrs[0] != NULL) {
            uint16_t op0 = ext->agg_ops[0];
            int8_t   t0  = agg_types[0];
            if ((op0 == OP_SUM || op0 == OP_AVG) &&
                (t0 == RAY_I64 || t0 == RAY_SYM || t0 == RAY_TIMESTAMP))
                sc_fn = scalar_sum_i64_fn;
            else if ((op0 == OP_SUM || op0 == OP_AVG) && t0 == RAY_F64)
                sc_fn = scalar_sum_f64_fn;
        } else if (n_aggs == 1 && !match_idx && agg_linear[0].enabled) {
            uint16_t op0 = ext->agg_ops[0];
            if (op0 == OP_SUM || op0 == OP_AVG)
                sc_fn = scalar_sum_linear_i64_fn;
        }

        if (sc_n > 1)
            ray_pool_dispatch(sc_pool, sc_fn, &sc_ctx, n_scan);
        else
            sc_fn(&sc_ctx, 0, 0, n_scan);

        /* Merge per-worker accumulators into sc_acc[0] */
        da_accum_t* m = &sc_acc[0];
        for (uint32_t w = 1; w < sc_n; w++) {
            da_accum_t* wa = &sc_acc[w];
            if (need_flags & DA_NEED_SUM) {
                for (uint8_t a = 0; a < n_aggs; a++) {
                    uint16_t merge_op = ext->agg_ops[a];
                    if (merge_op == OP_FIRST) {
                        if (m->count[0] == 0 && wa->count[0] > 0)
                            m->sum[a] = wa->sum[a];
                    } else if (merge_op == OP_LAST) {
                        if (wa->count[0] > 0)
                            m->sum[a] = wa->sum[a];
                    } else {
                        if (agg_types[a] == RAY_F64)
                            m->sum[a].f += wa->sum[a].f;
                        else
                            m->sum[a].i += wa->sum[a].i;
                    }
                }
            }
            if (need_flags & DA_NEED_SUMSQ) {
                for (uint8_t a = 0; a < n_aggs; a++)
                    m->sumsq_f64[a] += wa->sumsq_f64[a];
            }
            if (need_flags & DA_NEED_MIN) {
                for (uint8_t a = 0; a < n_aggs; a++) {
                    if (agg_types[a] == RAY_F64) {
                        if (wa->min_val[a].f < m->min_val[a].f)
                            m->min_val[a].f = wa->min_val[a].f;
                    } else {
                        if (wa->min_val[a].i < m->min_val[a].i)
                            m->min_val[a].i = wa->min_val[a].i;
                    }
                }
            }
            if (need_flags & DA_NEED_MAX) {
                for (uint8_t a = 0; a < n_aggs; a++) {
                    if (agg_types[a] == RAY_F64) {
                        if (wa->max_val[a].f > m->max_val[a].f)
                            m->max_val[a].f = wa->max_val[a].f;
                    } else {
                        if (wa->max_val[a].i > m->max_val[a].i)
                            m->max_val[a].i = wa->max_val[a].i;
                    }
                }
            }
            m->count[0] += wa->count[0];
        }
        for (uint32_t w = 1; w < sc_n; w++) da_accum_free(&sc_acc[w]);

        /* Emit 1-row result with no key columns */
        ray_t* result = ray_table_new(n_aggs);
        if (!result || RAY_IS_ERR(result)) {
            da_accum_free(&sc_acc[0]); scratch_free(sc_hdr);
            for (uint8_t a = 0; a < n_aggs; a++)
                if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
            for (uint8_t k = 0; k < n_keys; k++)
                if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
            if (match_idx_block) ray_release(match_idx_block);
            return result ? result : ray_error("oom", NULL);
        }

        emit_agg_columns(&result, g, ext, agg_vecs, 1, n_aggs,
                         (double*)m->sum, (int64_t*)m->sum,
                         (double*)m->min_val, (double*)m->max_val,
                         (int64_t*)m->min_val, (int64_t*)m->max_val,
                         m->count, agg_affine, m->sumsq_f64);

        da_accum_free(&sc_acc[0]); scratch_free(sc_hdr);
        for (uint8_t a = 0; a < n_aggs; a++)
            if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
        for (uint8_t k = 0; k < n_keys; k++)
            if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
        if (match_idx_block) ray_release(match_idx_block);
        return result;
    }

da_path:;
    /* ---- Direct-array fast path for low-cardinality integer keys ---- */
    /* Supports multi-key via composite index: product of ranges <= MAX */
    #define DA_MAX_COMPOSITE_SLOTS 262144  /* 256K slots max */
    #define DA_MEM_BUDGET      (256ULL << 20)  /* 256 MB total across all workers */
    #define DA_PER_WORKER_MAX  (6ULL << 20)    /* 6 MB per-worker max */
    {
        bool da_eligible = (nrows > 0 && n_keys > 0 && n_keys <= 8);
        for (uint8_t k = 0; k < n_keys && da_eligible; k++) {
            if (!key_data[k]) { da_eligible = false; break; }
            int8_t t = key_types[k];
            if (t != RAY_I64 && t != RAY_SYM && t != RAY_I32
                && t != RAY_TIMESTAMP && t != RAY_DATE && t != RAY_TIME
                && t != RAY_BOOL && t != RAY_U8 && t != RAY_I16) {
                da_eligible = false;
            }
            /* DA path cannot represent nulls — fall back to HT path. */
            if (key_vecs[k]) {
                ray_t* src = (key_vecs[k]->attrs & RAY_ATTR_SLICE)
                             ? key_vecs[k]->slice_parent : key_vecs[k];
                if (src && (src->attrs & RAY_ATTR_HAS_NULLS))
                    da_eligible = false;
            }
        }

        int64_t da_key_min[8], da_key_range[8], da_key_stride[8];
        uint64_t total_slots = 1;
        bool da_fits = false;


        if (da_eligible) {
            da_fits = true;
            ray_pool_t* mm_pool = ray_pool_get();
            uint32_t mm_n = (mm_pool && nrows >= RAY_PARALLEL_THRESHOLD)
                            ? ray_pool_total_workers(mm_pool) : 1;
            /* VLA bounded by worker count — max ~2KB per key even on 256-core systems. */
            int64_t mm_mins[mm_n], mm_maxs[mm_n];
            for (uint8_t k = 0; k < n_keys && da_fits; k++) {
                int64_t kmin, kmax;
                for (uint32_t w = 0; w < mm_n; w++) {
                    mm_mins[w] = INT64_MAX;
                    mm_maxs[w] = INT64_MIN;
                }
                minmax_ctx_t mm_ctx = {
                    .key_data       = key_data[k],
                    .key_type       = key_types[k],
                    .key_attrs      = key_attrs[k],
                    .per_worker_min = mm_mins,
                    .per_worker_max = mm_maxs,
                    .n_workers      = mm_n,
                    .match_idx      = match_idx,
                };
                if (mm_n > 1) {
                    ray_pool_dispatch(mm_pool, minmax_scan_fn, &mm_ctx, n_scan);
                } else {
                    minmax_scan_fn(&mm_ctx, 0, 0, n_scan);
                }
                kmin = INT64_MAX; kmax = INT64_MIN;
                for (uint32_t w = 0; w < mm_n; w++) {
                    if (mm_mins[w] < kmin) kmin = mm_mins[w];
                    if (mm_maxs[w] > kmax) kmax = mm_maxs[w];
                }
                da_key_min[k]   = kmin;
                /* kmax - kmin may overflow i64 when keys span full range.
                 * Compute in uint64_t and reject if the span exceeds i64. */
                uint64_t span = (uint64_t)kmax - (uint64_t)kmin + 1;
                if (span > (uint64_t)INT64_MAX) { da_fits = false; break; }
                da_key_range[k] = (int64_t)span;
                if (da_key_range[k] <= 0) { da_fits = false; break; }
                total_slots *= (uint64_t)da_key_range[k];
                if (total_slots > DA_MAX_COMPOSITE_SLOTS) da_fits = false;
            }
        }

        if (da_fits) {
            /* Compute which accumulator arrays we actually need */
            uint8_t need_flags = DA_NEED_COUNT; /* always need count */
            for (uint8_t a = 0; a < n_aggs; a++) {
                uint16_t aop = ext->agg_ops[a];
                if (aop == OP_SUM || aop == OP_AVG || aop == OP_FIRST || aop == OP_LAST) need_flags |= DA_NEED_SUM;
                else if (aop == OP_STDDEV || aop == OP_STDDEV_POP || aop == OP_VAR || aop == OP_VAR_POP)
                    { need_flags |= DA_NEED_SUM; need_flags |= DA_NEED_SUMSQ; }
                else if (aop == OP_MIN) need_flags |= DA_NEED_MIN;
                else if (aop == OP_MAX) need_flags |= DA_NEED_MAX;
            }

            /* Compute per-worker memory budget.  Actual allocation is 1 union
             * array per type, but MIN/MAX use conditional random writes that
             * perform worse than radix-partitioned HT at high group counts.
             * Weight MIN/MAX at 2x to keep those queries on the HT path. */
            uint32_t arrays_per_agg = 0;
            if (need_flags & DA_NEED_SUM) arrays_per_agg += 1;
            if (need_flags & DA_NEED_MIN) arrays_per_agg += 2; /* 2x: DA MIN slow at high cardinality */
            if (need_flags & DA_NEED_MAX) arrays_per_agg += 2; /* 2x: DA MAX slow at high cardinality */
            if (need_flags & DA_NEED_SUMSQ) arrays_per_agg += 1;
            uint64_t per_worker = total_slots * (arrays_per_agg * n_aggs + 1u) * 8u;
            if (per_worker > DA_PER_WORKER_MAX)
                da_fits = false;
        }

        if (da_fits) {
            /* Recompute need_flags (da_fits may have changed scope) */
            uint8_t need_flags = DA_NEED_COUNT;
            bool all_sum = true;
            for (uint8_t a = 0; a < n_aggs; a++) {
                uint16_t aop = ext->agg_ops[a];
                if (aop == OP_SUM || aop == OP_AVG || aop == OP_FIRST || aop == OP_LAST) need_flags |= DA_NEED_SUM;
                else if (aop == OP_STDDEV || aop == OP_STDDEV_POP || aop == OP_VAR || aop == OP_VAR_POP)
                    { need_flags |= DA_NEED_SUM; need_flags |= DA_NEED_SUMSQ; }
                else if (aop == OP_MIN) need_flags |= DA_NEED_MIN;
                else if (aop == OP_MAX) need_flags |= DA_NEED_MAX;
                if (aop != OP_SUM && aop != OP_AVG && aop != OP_COUNT)
                    all_sum = false;
            }

            /* Compute strides: stride[k] = product of ranges[k+1..n_keys-1]
             * Guard against overflow: if any product exceeds INT64_MAX,
             * fall through to HT path. */
            bool stride_overflow = false;
            for (uint8_t k = 0; k < n_keys; k++) {
                int64_t s = 1;
                for (uint8_t j = k + 1; j < n_keys; j++) {
                    if (da_key_range[j] != 0 && s > INT64_MAX / da_key_range[j]) {
                        stride_overflow = true; break;
                    }
                    s *= da_key_range[j];
                }
                if (stride_overflow) break;
                da_key_stride[k] = s;
            }
            if (stride_overflow) da_fits = false;

            uint32_t n_slots = (uint32_t)total_slots;
            size_t total = (size_t)n_slots * n_aggs;

            void* agg_ptrs[vla_aggs];
            int8_t agg_types[vla_aggs];
            uint32_t agg_f64_mask = 0;
            for (uint8_t a = 0; a < n_aggs; a++) {
                if (agg_vecs[a]) {
                    agg_ptrs[a]  = ray_data(agg_vecs[a]);
                    agg_types[a] = agg_vecs[a]->type;
                    if (agg_vecs[a]->type == RAY_F64)
                        agg_f64_mask |= (1u << a);
                } else {
                    agg_ptrs[a]  = NULL;
                    agg_types[a] = 0;
                }
            }

            ray_pool_t* da_pool = ray_pool_get();
            uint32_t da_n_workers = (da_pool && nrows >= RAY_PARALLEL_THRESHOLD)
                                    ? ray_pool_total_workers(da_pool) : 1;

            /* Check memory budget — need one accumulator set per worker.
             * Weight MIN/MAX at 2x in budget (same as eligibility check) to
             * keep MIN/MAX-heavy queries on the faster radix-HT path. */
            uint32_t arrays_per_agg = 0;
            if (need_flags & DA_NEED_SUM) arrays_per_agg += 1;
            if (need_flags & DA_NEED_MIN) arrays_per_agg += 2;
            if (need_flags & DA_NEED_MAX) arrays_per_agg += 2;
            if (need_flags & DA_NEED_SUMSQ) arrays_per_agg += 1;
            uint64_t per_worker_bytes = (uint64_t)n_slots * (arrays_per_agg * n_aggs + 1u) * 8u;
            if ((uint64_t)da_n_workers * per_worker_bytes > DA_MEM_BUDGET)
                da_n_workers = 1;

            ray_t* accums_hdr;
            da_accum_t* accums = (da_accum_t*)scratch_calloc(&accums_hdr,
                da_n_workers * sizeof(da_accum_t));
            if (!accums) goto ht_path;

            bool alloc_ok = true;
            for (uint32_t w = 0; w < da_n_workers; w++) {
                if (need_flags & DA_NEED_SUM) {
                    accums[w].sum = (da_val_t*)scratch_calloc(&accums[w]._h_sum,
                        total * sizeof(da_val_t));
                    if (!accums[w].sum) { alloc_ok = false; break; }
                }
                if (need_flags & DA_NEED_SUMSQ) {
                    accums[w].sumsq_f64 = (double*)scratch_calloc(&accums[w]._h_sumsq,
                        total * sizeof(double));
                    if (!accums[w].sumsq_f64) { alloc_ok = false; break; }
                }
                if (need_flags & DA_NEED_MIN) {
                    accums[w].min_val = (da_val_t*)scratch_alloc(&accums[w]._h_min,
                        total * sizeof(da_val_t));
                    if (!accums[w].min_val) { alloc_ok = false; break; }
                    for (size_t i = 0; i < total; i++) {
                        uint8_t a = (uint8_t)(i % n_aggs);
                        if (agg_types[a] == RAY_F64) accums[w].min_val[i].f = DBL_MAX;
                        else accums[w].min_val[i].i = INT64_MAX;
                    }
                }
                if (need_flags & DA_NEED_MAX) {
                    accums[w].max_val = (da_val_t*)scratch_alloc(&accums[w]._h_max,
                        total * sizeof(da_val_t));
                    if (!accums[w].max_val) { alloc_ok = false; break; }
                    for (size_t i = 0; i < total; i++) {
                        uint8_t a = (uint8_t)(i % n_aggs);
                        if (agg_types[a] == RAY_F64) accums[w].max_val[i].f = -DBL_MAX;
                        else accums[w].max_val[i].i = INT64_MIN;
                    }
                }
                accums[w].count = (int64_t*)scratch_calloc(&accums[w]._h_count,
                    n_slots * sizeof(int64_t));
                if (!accums[w].count) { alloc_ok = false; break; }
            }
            if (!alloc_ok) {
                for (uint32_t w = 0; w < da_n_workers; w++)
                    da_accum_free(&accums[w]);
                scratch_free(accums_hdr);
                goto ht_path;
            }


            /* Pre-compute per-key element sizes for fast DA reads */
            uint8_t da_key_esz[n_keys];
            for (uint8_t k = 0; k < n_keys; k++)
                da_key_esz[k] = ray_sym_elem_size(key_types[k], key_attrs[k]);

            da_ctx_t da_ctx = {
                .accums      = accums,
                .n_accums    = da_n_workers,
                .key_ptrs    = key_data,
                .key_types   = key_types,
                .key_attrs   = key_attrs,
                .key_esz     = da_key_esz,
                .key_mins    = da_key_min,
                .key_strides = da_key_stride,
                .n_keys      = n_keys,
                .agg_ptrs    = agg_ptrs,
                .agg_types   = agg_types,
                .agg_ops     = ext->agg_ops,
                .n_aggs      = n_aggs,
                .need_flags  = need_flags,
                .agg_f64_mask = agg_f64_mask,
                .all_sum     = all_sum,
                .n_slots     = n_slots,
                .match_idx   = match_idx,
            };

            if (da_n_workers > 1)
                ray_pool_dispatch(da_pool, da_accum_fn, &da_ctx, n_scan);
            else
                da_accum_fn(&da_ctx, 0, 0, n_scan);

            /* Merge target is always accums[0] */
            da_accum_t* merged = &accums[0];

            /* Check if any agg is FIRST/LAST (needs ordered per-worker merge) */
            bool has_first_last = false;
            for (uint8_t a = 0; a < n_aggs; a++) {
                uint16_t aop = ext->agg_ops[a];
                if (aop == OP_FIRST || aop == OP_LAST) { has_first_last = true; break; }
            }

            /* Merge per-worker accumulators into accums[0].
             * FIRST/LAST require worker-order-dependent merge (sequential).
             * All other ops are commutative — dispatch over disjoint slot
             * ranges for parallel merge. */
            if (has_first_last) {
                for (uint32_t w = 1; w < da_n_workers; w++) {
                    da_accum_t* wa = &accums[w];
                    if (need_flags & DA_NEED_SUMSQ) {
                        for (size_t i = 0; i < total; i++)
                            merged->sumsq_f64[i] += wa->sumsq_f64[i];
                    }
                    if (need_flags & DA_NEED_SUM) {
                        for (uint32_t s = 0; s < n_slots; s++) {
                            size_t base = (size_t)s * n_aggs;
                            for (uint8_t a = 0; a < n_aggs; a++) {
                                size_t idx = base + a;
                                uint16_t aop = ext->agg_ops[a];
                                if (aop == OP_SUM || aop == OP_AVG || aop == OP_STDDEV || aop == OP_STDDEV_POP || aop == OP_VAR || aop == OP_VAR_POP) {
                                    if (agg_types[a] == RAY_F64) merged->sum[idx].f += wa->sum[idx].f;
                                    else merged->sum[idx].i += wa->sum[idx].i;
                                } else if (aop == OP_FIRST) {
                                    if (merged->count[s] == 0 && wa->count[s] > 0)
                                        merged->sum[idx] = wa->sum[idx];
                                } else if (aop == OP_LAST) {
                                    if (wa->count[s] > 0)
                                        merged->sum[idx] = wa->sum[idx];
                                }
                            }
                        }
                    }
                    if (need_flags & DA_NEED_MIN) {
                        for (size_t i = 0; i < total; i++) {
                            uint8_t a = (uint8_t)(i % n_aggs);
                            if (agg_types[a] == RAY_F64) {
                                if (wa->min_val[i].f < merged->min_val[i].f)
                                    merged->min_val[i].f = wa->min_val[i].f;
                            } else {
                                if (wa->min_val[i].i < merged->min_val[i].i)
                                    merged->min_val[i].i = wa->min_val[i].i;
                            }
                        }
                    }
                    if (need_flags & DA_NEED_MAX) {
                        for (size_t i = 0; i < total; i++) {
                            uint8_t a = (uint8_t)(i % n_aggs);
                            if (agg_types[a] == RAY_F64) {
                                if (wa->max_val[i].f > merged->max_val[i].f)
                                    merged->max_val[i].f = wa->max_val[i].f;
                            } else {
                                if (wa->max_val[i].i > merged->max_val[i].i)
                                    merged->max_val[i].i = wa->max_val[i].i;
                            }
                        }
                    }
                    for (uint32_t s = 0; s < n_slots; s++)
                        merged->count[s] += wa->count[s];
                }
            } else if (da_n_workers > 1 && n_slots >= 1024 && da_pool) {
                /* Parallel merge: dispatch over disjoint slot ranges */
                da_merge_ctx_t merge_ctx = {
                    .accums        = accums,
                    .n_src_workers = da_n_workers,
                    .need_flags    = need_flags,
                    .n_aggs        = n_aggs,
                    .agg_types     = agg_types,
                    .agg_ops       = ext->agg_ops,
                };
                ray_pool_dispatch(da_pool, da_merge_fn, &merge_ctx, (int64_t)n_slots);
            } else {
                /* Sequential merge for small slot counts */
                for (uint32_t w = 1; w < da_n_workers; w++) {
                    da_accum_t* wa = &accums[w];
                    if (need_flags & DA_NEED_SUMSQ) {
                        for (size_t i = 0; i < total; i++)
                            merged->sumsq_f64[i] += wa->sumsq_f64[i];
                    }
                    if (need_flags & DA_NEED_SUM) {
                        for (uint32_t s = 0; s < n_slots; s++) {
                            size_t base = (size_t)s * n_aggs;
                            for (uint8_t a = 0; a < n_aggs; a++) {
                                size_t idx = base + a;
                                uint16_t aop = ext->agg_ops[a];
                                if (aop == OP_FIRST) {
                                    if (merged->count[s] == 0 && wa->count[s] > 0)
                                        merged->sum[idx] = wa->sum[idx];
                                } else if (aop == OP_LAST) {
                                    if (wa->count[s] > 0)
                                        merged->sum[idx] = wa->sum[idx];
                                } else if (agg_types[a] == RAY_F64)
                                    merged->sum[idx].f += wa->sum[idx].f;
                                else
                                    merged->sum[idx].i += wa->sum[idx].i;
                            }
                        }
                    }
                    if (need_flags & DA_NEED_MIN) {
                        for (size_t i = 0; i < total; i++) {
                            uint8_t a = (uint8_t)(i % n_aggs);
                            if (agg_types[a] == RAY_F64) {
                                if (wa->min_val[i].f < merged->min_val[i].f)
                                    merged->min_val[i].f = wa->min_val[i].f;
                            } else {
                                if (wa->min_val[i].i < merged->min_val[i].i)
                                    merged->min_val[i].i = wa->min_val[i].i;
                            }
                        }
                    }
                    if (need_flags & DA_NEED_MAX) {
                        for (size_t i = 0; i < total; i++) {
                            uint8_t a = (uint8_t)(i % n_aggs);
                            if (agg_types[a] == RAY_F64) {
                                if (wa->max_val[i].f > merged->max_val[i].f)
                                    merged->max_val[i].f = wa->max_val[i].f;
                            } else {
                                if (wa->max_val[i].i > merged->max_val[i].i)
                                    merged->max_val[i].i = wa->max_val[i].i;
                            }
                        }
                    }
                    for (uint32_t s = 0; s < n_slots; s++)
                        merged->count[s] += wa->count[s];
                }
            }



            for (uint32_t w = 1; w < da_n_workers; w++)
                da_accum_free(&accums[w]);

            da_val_t* da_sum      = merged->sum;      /* may be NULL if !DA_NEED_SUM */
            da_val_t* da_min_val  = merged->min_val;  /* may be NULL if !DA_NEED_MIN */
            da_val_t* da_max_val  = merged->max_val;  /* may be NULL if !DA_NEED_MAX */
            double*   da_sumsq   = merged->sumsq_f64; /* may be NULL if !DA_NEED_SUMSQ */
            int64_t*  da_count   = merged->count;

            uint32_t grp_count = 0;
            for (uint32_t s = 0; s < n_slots; s++)
                if (da_count[s] > 0) grp_count++;

            int64_t total_cols = n_keys + n_aggs;
            ray_t* result = ray_table_new(total_cols);
            if (!result || RAY_IS_ERR(result)) {
                da_accum_free(&accums[0]); scratch_free(accums_hdr);
                for (uint8_t a = 0; a < n_aggs; a++)
                    if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                for (uint8_t k = 0; k < n_keys; k++)
                    if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                if (match_idx_block) ray_release(match_idx_block);
                return result ? result : ray_error("oom", NULL);
            }

            /* Key columns — decompose composite slot back to per-key values */
            for (uint8_t k = 0; k < n_keys; k++) {
                ray_t* src_col = key_vecs[k];
                if (!src_col) continue;
                ray_t* key_col = col_vec_new(src_col, (int64_t)grp_count);
                if (!key_col || RAY_IS_ERR(key_col)) continue;
                key_col->len = (int64_t)grp_count;
                uint32_t gi = 0;
                for (uint32_t s = 0; s < n_slots; s++) {
                    if (da_count[s] == 0) continue;
                    int64_t offset = ((int64_t)s / da_key_stride[k]) % da_key_range[k];
                    int64_t key_val = da_key_min[k] + offset;
                    write_col_i64(ray_data(key_col), gi, key_val, src_col->type, key_col->attrs);
                    gi++;
                }
                ray_op_ext_t* key_ext = find_ext(g, ext->keys[k]->id);
                int64_t name_id = key_ext ? key_ext->sym : (int64_t)k;
                result = ray_table_add_col(result, name_id, key_col);
                ray_release(key_col);
            }

            /* Agg columns — compact sparse DA arrays into dense, then emit */
            size_t dense_total = (size_t)grp_count * n_aggs;
            ray_t *_h_dsum = NULL, *_h_dmin = NULL, *_h_dmax = NULL;
            ray_t *_h_dsq = NULL, *_h_dcnt = NULL;
            da_val_t* dense_sum     = da_sum     ? (da_val_t*)scratch_alloc(&_h_dsum, dense_total * sizeof(da_val_t)) : NULL;
            da_val_t* dense_min_val = da_min_val ? (da_val_t*)scratch_alloc(&_h_dmin, dense_total * sizeof(da_val_t)) : NULL;
            da_val_t* dense_max_val = da_max_val ? (da_val_t*)scratch_alloc(&_h_dmax, dense_total * sizeof(da_val_t)) : NULL;
            double*   dense_sumsq   = da_sumsq   ? (double*)scratch_alloc(&_h_dsq, dense_total * sizeof(double)) : NULL;
            int64_t*  dense_counts  = (int64_t*)scratch_alloc(&_h_dcnt, grp_count * sizeof(int64_t));

            uint32_t gi = 0;
            for (uint32_t s = 0; s < n_slots; s++) {
                if (da_count[s] == 0) continue;
                dense_counts[gi] = da_count[s];
                for (uint8_t a = 0; a < n_aggs; a++) {
                    size_t si = (size_t)s * n_aggs + a;
                    size_t di = (size_t)gi * n_aggs + a;
                    if (dense_sum)     dense_sum[di]     = da_sum[si];
                    if (dense_min_val) dense_min_val[di] = da_min_val[si];
                    if (dense_max_val) dense_max_val[di] = da_max_val[si];
                    if (dense_sumsq)   dense_sumsq[di]   = da_sumsq[si];
                }
                gi++;
            }

            emit_agg_columns(&result, g, ext, agg_vecs, grp_count, n_aggs,
                             (double*)dense_sum, (int64_t*)dense_sum,
                             (double*)dense_min_val, (double*)dense_max_val,
                             (int64_t*)dense_min_val, (int64_t*)dense_max_val,
                             dense_counts, agg_affine, dense_sumsq);

            scratch_free(_h_dsum); scratch_free(_h_dmin);
            scratch_free(_h_dmax);
            scratch_free(_h_dsq); scratch_free(_h_dcnt);

            da_accum_free(&accums[0]); scratch_free(accums_hdr);
            for (uint8_t a = 0; a < n_aggs; a++)
                if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
            for (uint8_t k = 0; k < n_keys; k++)
                if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
            if (match_idx_block) ray_release(match_idx_block);
            return result;
        }
    }

ht_path:;
    /* Compute which accumulator arrays the HT needs based on agg ops.
     * COUNT only reads group row's count field — no accumulator needed. */
    uint8_t ght_need = 0;
    for (uint8_t a = 0; a < n_aggs; a++) {
        uint16_t aop = ext->agg_ops[a];
        if (aop == OP_SUM || aop == OP_AVG || aop == OP_FIRST || aop == OP_LAST)
            ght_need |= GHT_NEED_SUM;
        if (aop == OP_STDDEV || aop == OP_STDDEV_POP || aop == OP_VAR || aop == OP_VAR_POP)
            { ght_need |= GHT_NEED_SUM; ght_need |= GHT_NEED_SUMSQ; }
        if (aop == OP_MIN) ght_need |= GHT_NEED_MIN;
        if (aop == OP_MAX) ght_need |= GHT_NEED_MAX;
    }

    /* RAY_STR keys still need the eval-level path (variable-width
     * with a pool).  RAY_GUID uses the wide-key row-indirection
     * support in the layout; see ght_layout_t.wide_key_mask. */
    for (uint8_t k = 0; k < n_keys; k++) {
        if (key_types[k] == RAY_STR) {
            for (uint8_t kk = 0; kk < n_keys; kk++)
                if (key_owned[kk] && key_vecs[kk]) ray_release(key_vecs[kk]);
            for (uint8_t a = 0; a < n_aggs; a++)
                if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
            if (match_idx_block) ray_release(match_idx_block);
            return ray_error("nyi", NULL);
        }
    }

    /* Compute row-layout: keys + agg values inline */
    ght_layout_t ght_layout = ght_compute_layout(n_keys, n_aggs, agg_vecs, ght_need, ext->agg_ops, key_types);

    /* Right-sized hash table: start small, rehash on load > 0.5 */
    uint32_t ht_cap = 256;
    {
        uint64_t target = (uint64_t)nrows < 65536 ? (uint64_t)nrows : 65536;
        if (target < 256) target = 256;
        while (ht_cap < target) ht_cap *= 2;
    }

    /* Parallel path: radix-partitioned group-by */
    ray_pool_t* pool = ray_pool_get();
    uint32_t n_total = pool ? ray_pool_total_workers(pool) : 1;

    group_ht_t single_ht;
    group_ht_t* final_ht = NULL;
    ray_t* result = NULL;

    ray_t* radix_bufs_hdr = NULL;
    radix_buf_t* radix_bufs = NULL;
    ray_t* part_hts_hdr = NULL;
    group_ht_t*  part_hts   = NULL;

    if (pool && nrows >= RAY_PARALLEL_THRESHOLD && n_total > 1) {
        size_t n_bufs = (size_t)n_total * RADIX_P;
        radix_bufs = (radix_buf_t*)scratch_calloc(&radix_bufs_hdr,
            n_bufs * sizeof(radix_buf_t));
        if (!radix_bufs) goto sequential_fallback;

        /* Pre-size each buffer: 1.5x expected, capped so total ≤ 2 GB.
         * Buffers grow on demand via radix_buf_push doubling. */
        uint32_t buf_init = (uint32_t)((uint64_t)nrows / (RADIX_P * n_total));
        if (buf_init < 64) buf_init = 64;
        buf_init = buf_init + buf_init / 2;  /* 1.5x headroom */
        uint16_t estride = ght_layout.entry_stride;
        {
            /* Cap: total pre-alloc ≤ 2 GB */
            size_t total_pre = (size_t)n_bufs * buf_init * estride;
            if (total_pre > (size_t)2 << 30) {
                buf_init = (uint32_t)(((size_t)2 << 30) / ((size_t)n_bufs * estride));
                if (buf_init < 64) buf_init = 64;
            }
        }
        for (size_t i = 0; i < n_bufs; i++) {
            radix_bufs[i].data = (char*)scratch_alloc(
                &radix_bufs[i]._hdr, (size_t)buf_init * estride);
            radix_bufs[i].count = 0;
            radix_bufs[i].cap = buf_init;
        }

        /* Compute per-key nullability — lets phase1 skip null checks on
         * key columns with no nulls (the common case). */
        uint8_t p1_nullable = 0;
        for (uint8_t k = 0; k < n_keys; k++) {
            if (!key_vecs[k]) continue;
            ray_t* src = (key_vecs[k]->attrs & RAY_ATTR_SLICE)
                         ? key_vecs[k]->slice_parent : key_vecs[k];
            if (src && (src->attrs & RAY_ATTR_HAS_NULLS))
                p1_nullable |= (uint8_t)(1u << k);
        }

        /* Phase 1: parallel hash + copy keys/agg values into fat entries */
        radix_phase1_ctx_t p1ctx = {
            .key_data      = key_data,
            .key_types     = key_types,
            .key_attrs     = key_attrs,
            .key_vecs      = key_vecs,
            .nullable_mask = p1_nullable,
            .agg_vecs      = agg_vecs,
            .n_workers     = n_total,
            .bufs          = radix_bufs,
            .layout        = ght_layout,
            .match_idx     = match_idx,
        };
        ray_pool_dispatch(pool, radix_phase1_fn, &p1ctx, n_scan);
        CHECK_CANCEL_GOTO(pool, cleanup);

        /* Check for OOM during phase 1 radix buffer growth */
        {
            bool phase1_oom = false;
            for (size_t i = 0; i < n_bufs; i++) {
                if (radix_bufs[i].oom) { phase1_oom = true; break; }
            }
            if (phase1_oom) {
                for (size_t i = 0; i < n_bufs; i++) scratch_free(radix_bufs[i]._hdr);
                scratch_free(radix_bufs_hdr);
                radix_bufs = NULL;
                goto sequential_fallback;
            }
        }

        /* Phase 2: parallel per-partition aggregation (no column access) */
        part_hts = (group_ht_t*)scratch_calloc(&part_hts_hdr,
            RADIX_P * sizeof(group_ht_t));
        if (!part_hts) {
            for (size_t i = 0; i < n_bufs; i++) scratch_free(radix_bufs[i]._hdr);
            scratch_free(radix_bufs_hdr);
            radix_bufs = NULL;
            goto sequential_fallback;
        }

        radix_phase2_ctx_t p2ctx = {
            .key_types   = key_types,
            .n_keys      = n_keys,
            .n_workers   = n_total,
            .bufs        = radix_bufs,
            .part_hts    = part_hts,
            .layout      = ght_layout,
            .key_data    = key_data,
        };
        ray_pool_dispatch_n(pool, radix_phase2_fn, &p2ctx, RADIX_P);
        CHECK_CANCEL_GOTO(pool, cleanup);

        /* Prefix offsets */
        uint32_t part_offsets[RADIX_P + 1];
        part_offsets[0] = 0;
        for (uint32_t p = 0; p < RADIX_P; p++)
            part_offsets[p + 1] = part_offsets[p] + part_hts[p].grp_count;
        uint32_t total_grps = part_offsets[RADIX_P];

        /* Build result directly from partition HTs */
        int64_t total_cols = n_keys + n_aggs;
        result = ray_table_new(total_cols);
        if (!result || RAY_IS_ERR(result)) goto cleanup;

        /* Pre-allocate key columns */
        ray_t* key_cols[n_keys];
        char* key_dsts[n_keys];
        int8_t key_out_types[n_keys];
        uint8_t key_esizes[n_keys];
        for (uint8_t k = 0; k < n_keys; k++) {
            ray_t* src_col = key_vecs[k];
            key_cols[k] = NULL;
            key_dsts[k] = NULL;
            key_out_types[k] = 0;
            key_esizes[k] = 0;
            if (!src_col) continue;
            uint8_t esz = ray_sym_elem_size(src_col->type, src_col->attrs);
            ray_t* new_col;
            if (src_col->type == RAY_SYM)
                new_col = ray_sym_vec_new(src_col->attrs & RAY_SYM_W_MASK, (int64_t)total_grps);
            else
                new_col = ray_vec_new(src_col->type, (int64_t)total_grps);
            if (!new_col || RAY_IS_ERR(new_col)) continue;
            new_col->len = (int64_t)total_grps;
            key_cols[k] = new_col;
            key_dsts[k] = (char*)ray_data(new_col);
            key_out_types[k] = src_col->type;
            key_esizes[k] = esz;
        }

        /* Pre-allocate agg result vectors */
        agg_out_t agg_outs[n_aggs];
        ray_t* agg_cols[n_aggs];
        for (uint8_t a = 0; a < n_aggs; a++) {
            uint16_t agg_op = ext->agg_ops[a];
            ray_t* agg_col = agg_vecs[a];
            bool is_f64 = agg_col && agg_col->type == RAY_F64;
            int8_t out_type;
            switch (agg_op) {
                case OP_AVG:
                case OP_STDDEV: case OP_STDDEV_POP:
                case OP_VAR: case OP_VAR_POP:
                    out_type = RAY_F64; break;
                case OP_COUNT: out_type = RAY_I64; break;
                case OP_SUM: case OP_PROD:
                    out_type = is_f64 ? RAY_F64 : RAY_I64; break;
                default:
                    out_type = agg_col ? agg_col->type : RAY_I64; break;
            }
            ray_t* new_col = ray_vec_new(out_type, (int64_t)total_grps);
            if (!new_col || RAY_IS_ERR(new_col)) {
                agg_cols[a] = NULL;
                memset(&agg_outs[a], 0, sizeof(agg_outs[a]));
                continue;
            }
            new_col->len = (int64_t)total_grps;
            agg_cols[a] = new_col;
            agg_outs[a] = (agg_out_t){
                .out_type = out_type, .src_f64 = is_f64,
                .agg_op = agg_op,
                .affine = agg_affine[a].enabled,
                .bias_f64 = agg_affine[a].bias_f64,
                .bias_i64 = agg_affine[a].bias_i64,
                .dst = ray_data(new_col),
                .vec = new_col,
            };
        }

        /* Pre-allocate nullmaps for agg result vectors (parallel safety) */
        bool nullmap_prep_ok[n_aggs];
        for (uint8_t a = 0; a < n_aggs; a++)
            nullmap_prep_ok[a] = agg_cols[a] && (grp_prepare_nullmap(agg_outs[a].vec) == RAY_OK);

        /* Pre-prepare nullmaps on output key columns for parallel null writes */
        for (uint8_t k = 0; k < n_keys; k++)
            if (key_cols[k]) grp_prepare_nullmap(key_cols[k]);

        /* Phase 3: parallel key gather + agg result building from inline rows */
        {
            radix_phase3_ctx_t p3ctx = {
                .part_hts     = part_hts,
                .part_offsets = part_offsets,
                .key_dsts     = key_dsts,
                .key_types    = key_out_types,
                .key_attrs    = key_attrs,
                .key_esizes   = key_esizes,
                .key_cols     = key_cols,
                .n_keys       = n_keys,
                .agg_outs     = agg_outs,
                .n_aggs       = n_aggs,
                .key_src_data = key_data,
            };
            ray_pool_dispatch_n(pool, radix_phase3_fn, &p3ctx, RADIX_P);
        }

        /* Fixup: if nullmap prep failed for any VAR/STDDEV agg, re-scan
         * hash tables sequentially to ensure all null bits were set */
        for (uint8_t a = 0; a < n_aggs; a++) {
            if (nullmap_prep_ok[a] || !agg_cols[a]) continue;
            uint16_t op = agg_outs[a].agg_op;
            if (op != OP_VAR && op != OP_VAR_POP &&
                op != OP_STDDEV && op != OP_STDDEV_POP) continue;
            for (uint32_t p = 0; p < RADIX_P; p++) {
                group_ht_t* ph = &part_hts[p];
                uint32_t gc = ph->grp_count;
                uint32_t off = part_offsets[p];
                uint16_t rs = ph->layout.row_stride;
                for (uint32_t gi = 0; gi < gc; gi++) {
                    const char* row = ph->rows + (size_t)gi * rs;
                    int64_t cnt = *(const int64_t*)(const void*)row;
                    bool insuf = (op == OP_VAR || op == OP_STDDEV) ? cnt <= 1 : cnt <= 0;
                    if (insuf) ray_vec_set_null(agg_outs[a].vec, off + gi, true);
                }
            }
        }

        /* Finalize null flags after parallel execution */
        for (uint8_t a = 0; a < n_aggs; a++) {
            if (!agg_cols[a]) continue;
            grp_finalize_nulls(agg_outs[a].vec);
        }
        for (uint8_t k = 0; k < n_keys; k++) {
            if (!key_cols[k]) continue;
            grp_finalize_nulls(key_cols[k]);
        }

        /* Add key columns to result */
        for (uint8_t k = 0; k < n_keys; k++) {
            if (!key_cols[k]) continue;
            ray_op_ext_t* key_ext = find_ext(g, ext->keys[k]->id);
            int64_t name_id = key_ext ? key_ext->sym : k;
            result = ray_table_add_col(result, name_id, key_cols[k]);
            ray_release(key_cols[k]);
        }

        /* Add agg columns to result */
        for (uint8_t a = 0; a < n_aggs; a++) {
            if (!agg_cols[a]) continue;
            uint16_t agg_op = ext->agg_ops[a];
            ray_op_ext_t* agg_ext = find_ext(g, ext->agg_ins[a]->id);
            int64_t name_id;
            if (agg_ext && agg_ext->base.opcode == OP_SCAN) {
                ray_t* name_atom = ray_sym_str(agg_ext->sym);
                const char* base = name_atom ? ray_str_ptr(name_atom) : NULL;
                size_t blen = base ? ray_str_len(name_atom) : 0;
                const char* sfx = "";
                size_t slen = 0;
                switch (agg_op) {
                    case OP_SUM:   sfx = "_sum";   slen = 4; break;
                    case OP_COUNT: sfx = "_count"; slen = 6; break;
                    case OP_AVG:   sfx = "_mean";  slen = 5; break;
                    case OP_MIN:   sfx = "_min";   slen = 4; break;
                    case OP_MAX:   sfx = "_max";   slen = 4; break;
                    case OP_FIRST: sfx = "_first"; slen = 6; break;
                    case OP_LAST:  sfx = "_last";  slen = 5; break;
                    case OP_STDDEV:     sfx = "_stddev";     slen = 7; break;
                    case OP_STDDEV_POP: sfx = "_stddev_pop"; slen = 11; break;
                    case OP_VAR:        sfx = "_var";        slen = 4; break;
                    case OP_VAR_POP:    sfx = "_var_pop";    slen = 8; break;
                }
                char buf[256];
                ray_t* name_dyn_hdr = NULL;
                char* nbp = buf;
                size_t nbc = sizeof(buf);
                if (base && blen + slen >= sizeof(buf)) {
                    nbp = (char*)scratch_alloc(&name_dyn_hdr, blen + slen + 1);
                    if (nbp) nbc = blen + slen + 1;
                    else { nbp = buf; nbc = sizeof(buf); }
                }
                if (base && blen + slen < nbc) {
                    memcpy(nbp, base, blen);
                    memcpy(nbp + blen, sfx, slen);
                    name_id = ray_sym_intern(nbp, blen + slen);
                } else {
                    name_id = agg_ext->sym;
                }
                scratch_free(name_dyn_hdr);
            } else {
                name_id = (int64_t)(n_keys + a);
            }
            result = ray_table_add_col(result, name_id, agg_cols[a]);
            ray_release(agg_cols[a]);
        }

        goto cleanup;
    }

sequential_fallback:;
    /* Sequential path using row-layout HT */
    if (!group_ht_init(&single_ht, ht_cap, &ght_layout)) {
        result = ray_error("oom", NULL);
        goto cleanup;
    }
    group_rows_range(&single_ht, key_data, key_types, key_attrs, key_vecs, agg_vecs,
                     0, n_scan, match_idx);
    final_ht = &single_ht;
    if (ray_interrupted()) { result = ray_error("cancel", "interrupted"); goto cleanup; }
    if (single_ht.oom) { result = ray_error("oom", NULL); goto cleanup; }

    /* Build result from sequential HT (inline row layout) */
    {
    uint32_t grp_count = final_ht->grp_count;
    const ght_layout_t* ly = &final_ht->layout;
    int64_t total_cols = n_keys + n_aggs;
    result = ray_table_new(total_cols);
    if (!result || RAY_IS_ERR(result)) goto cleanup;

    /* Key columns: read from inline group rows, narrow to original type.
     * Wide keys store a source row index in the HT slot; resolve it
     * through the original key column (key_data[k]) and copy bytes. */
    for (uint8_t k = 0; k < n_keys; k++) {
        ray_t* src_col = key_vecs[k];
        if (!src_col) continue;
        uint8_t esz = col_esz(src_col);
        int8_t kt = src_col->type;

        ray_t* new_col = col_vec_new(src_col, (int64_t)grp_count);
        if (!new_col || RAY_IS_ERR(new_col)) continue;
        new_col->len = (int64_t)grp_count;

        bool is_wide = (ly->wide_key_mask & (1u << k)) != 0;
        const char* src_base = is_wide ? (const char*)key_data[k] : NULL;

        for (uint32_t gi = 0; gi < grp_count; gi++) {
            const char* row = final_ht->rows + (size_t)gi * ly->row_stride;
            const int64_t* rkeys = (const int64_t*)(row + 8);
            int64_t kv = rkeys[k];
            int64_t null_mask = rkeys[n_keys];
            if (null_mask & (int64_t)(1u << k)) {
                ray_vec_set_null(new_col, (int64_t)gi, true);
                continue;
            }
            if (is_wide) {
                char* dst = (char*)ray_data(new_col) + (size_t)gi * esz;
                memcpy(dst, src_base + (size_t)kv * esz, esz);
            } else if (kt == RAY_F64) {
                char* dst = (char*)ray_data(new_col) + (size_t)gi * esz;
                memcpy(dst, &kv, 8);
            } else {
                write_col_i64(ray_data(new_col), gi, kv, kt, new_col->attrs);
            }
        }

        ray_op_ext_t* key_ext = find_ext(g, ext->keys[k]->id);
        int64_t name_id = key_ext ? key_ext->sym : k;
        result = ray_table_add_col(result, name_id, new_col);
        ray_release(new_col);
    }

    /* Agg columns from inline accumulators */
    for (uint8_t a = 0; a < n_aggs; a++) {
        uint16_t agg_op = ext->agg_ops[a];
        ray_t* agg_col = agg_vecs[a];
        bool is_f64 = agg_col && agg_col->type == RAY_F64;
        int8_t out_type;
        switch (agg_op) {
            case OP_AVG:
            case OP_STDDEV: case OP_STDDEV_POP:
            case OP_VAR: case OP_VAR_POP:
                out_type = RAY_F64; break;
            case OP_COUNT: out_type = RAY_I64; break;
            case OP_SUM: case OP_PROD:
                out_type = is_f64 ? RAY_F64 : RAY_I64; break;
            default:
                out_type = agg_col ? agg_col->type : RAY_I64; break;
        }
        ray_t* new_col = ray_vec_new(out_type, (int64_t)grp_count);
        if (!new_col || RAY_IS_ERR(new_col)) continue;
        new_col->len = (int64_t)grp_count;

        int8_t s = ly->agg_val_slot[a]; /* unified accum slot */
        for (uint32_t gi = 0; gi < grp_count; gi++) {
            const char* row = final_ht->rows + (size_t)gi * ly->row_stride;
            int64_t cnt = *(const int64_t*)(const void*)row;
            if (out_type == RAY_F64) {
                double v;
                switch (agg_op) {
                    case OP_SUM:
                        v = is_f64 ? ROW_RD_F64(row, ly->off_sum, s)
                                   : (double)ROW_RD_I64(row, ly->off_sum, s);
                        if (agg_affine[a].enabled) v += agg_affine[a].bias_f64 * cnt;
                        break;
                    case OP_AVG:
                        v = is_f64 ? ROW_RD_F64(row, ly->off_sum, s) / cnt
                                   : (double)ROW_RD_I64(row, ly->off_sum, s) / cnt;
                        if (agg_affine[a].enabled) v += agg_affine[a].bias_f64;
                        break;
                    case OP_MIN:
                        v = is_f64 ? ROW_RD_F64(row, ly->off_min, s)
                                   : (double)ROW_RD_I64(row, ly->off_min, s);
                        break;
                    case OP_MAX:
                        v = is_f64 ? ROW_RD_F64(row, ly->off_max, s)
                                   : (double)ROW_RD_I64(row, ly->off_max, s);
                        break;
                    case OP_FIRST: case OP_LAST:
                        v = is_f64 ? ROW_RD_F64(row, ly->off_sum, s)
                                   : (double)ROW_RD_I64(row, ly->off_sum, s);
                        break;
                    case OP_VAR: case OP_VAR_POP:
                    case OP_STDDEV: case OP_STDDEV_POP: {
                        bool insuf = (agg_op == OP_VAR || agg_op == OP_STDDEV) ? cnt <= 1 : cnt <= 0;
                        if (insuf) { v = 0.0; ray_vec_set_null(new_col, gi, true); break; }
                        double sum_val = is_f64 ? ROW_RD_F64(row, ly->off_sum, s)
                                                : (double)ROW_RD_I64(row, ly->off_sum, s);
                        double sq_val = ly->off_sumsq ? ROW_RD_F64(row, ly->off_sumsq, s) : 0.0;
                        double mean = sum_val / cnt;
                        double var_pop = sq_val / cnt - mean * mean;
                        if (var_pop < 0) var_pop = 0;
                        if (agg_op == OP_VAR_POP) v = var_pop;
                        else if (agg_op == OP_VAR) v = var_pop * cnt / (cnt - 1);
                        else if (agg_op == OP_STDDEV_POP) v = sqrt(var_pop);
                        else v = sqrt(var_pop * cnt / (cnt - 1));
                        break;
                    }
                    default: v = 0.0; break;
                }
                ((double*)ray_data(new_col))[gi] = v;
            } else {
                int64_t v;
                switch (agg_op) {
                    case OP_SUM:
                        v = ROW_RD_I64(row, ly->off_sum, s);
                        if (agg_affine[a].enabled) v += agg_affine[a].bias_i64 * cnt;
                        break;
                    case OP_COUNT: v = cnt; break;
                    case OP_MIN:   v = ROW_RD_I64(row, ly->off_min, s); break;
                    case OP_MAX:   v = ROW_RD_I64(row, ly->off_max, s); break;
                    case OP_FIRST: case OP_LAST: v = ROW_RD_I64(row, ly->off_sum, s); break;
                    default:       v = 0; break;
                }
                ((int64_t*)ray_data(new_col))[gi] = v;
            }
        }

        /* Generate unique column name */
        ray_op_ext_t* agg_ext = find_ext(g, ext->agg_ins[a]->id);
        int64_t name_id;
        if (agg_ext && agg_ext->base.opcode == OP_SCAN) {
            ray_t* name_atom = ray_sym_str(agg_ext->sym);
            const char* base = name_atom ? ray_str_ptr(name_atom) : NULL;
            size_t blen = base ? ray_str_len(name_atom) : 0;
            const char* sfx = "";
            size_t slen = 0;
            switch (agg_op) {
                case OP_SUM:   sfx = "_sum";   slen = 4; break;
                case OP_COUNT: sfx = "_count"; slen = 6; break;
                case OP_AVG:   sfx = "_mean";  slen = 5; break;
                case OP_MIN:   sfx = "_min";   slen = 4; break;
                case OP_MAX:   sfx = "_max";   slen = 4; break;
                case OP_FIRST: sfx = "_first"; slen = 6; break;
                case OP_LAST:  sfx = "_last";  slen = 5; break;
                case OP_STDDEV:     sfx = "_stddev";     slen = 7; break;
                case OP_STDDEV_POP: sfx = "_stddev_pop"; slen = 11; break;
                case OP_VAR:        sfx = "_var";        slen = 4; break;
                case OP_VAR_POP:    sfx = "_var_pop";    slen = 8; break;
            }
            char buf[256];
            if (base && blen + slen < sizeof(buf)) {
                memcpy(buf, base, blen);
                memcpy(buf + blen, sfx, slen);
                name_id = ray_sym_intern(buf, blen + slen);
            } else {
                name_id = agg_ext->sym;
            }
        } else {
            /* Expression agg input — synthetic name like "_e0_sum" */
            char nbuf[32];
            int np = 0;
            nbuf[np++] = '_'; nbuf[np++] = 'e';
            /* Multi-digit agg index */
            { uint8_t v = a; char dig[3]; int nd = 0;
              do { dig[nd++] = (char)('0' + v % 10); v /= 10; } while (v);
              while (nd--) nbuf[np++] = dig[nd]; }
            const char* nsfx = "";
            size_t nslen = 0;
            switch (agg_op) {
                case OP_SUM:   nsfx = "_sum";   nslen = 4; break;
                case OP_COUNT: nsfx = "_count"; nslen = 6; break;
                case OP_AVG:   nsfx = "_mean";  nslen = 5; break;
                case OP_MIN:   nsfx = "_min";   nslen = 4; break;
                case OP_MAX:   nsfx = "_max";   nslen = 4; break;
                case OP_FIRST: nsfx = "_first"; nslen = 6; break;
                case OP_LAST:  nsfx = "_last";  nslen = 5; break;
                case OP_STDDEV:     nsfx = "_stddev";     nslen = 7; break;
                case OP_STDDEV_POP: nsfx = "_stddev_pop"; nslen = 11; break;
                case OP_VAR:        nsfx = "_var";        nslen = 4; break;
                case OP_VAR_POP:    nsfx = "_var_pop";    nslen = 8; break;
            }
            memcpy(nbuf + np, nsfx, nslen);
            name_id = ray_sym_intern(nbuf, (size_t)np + nslen);
        }
        result = ray_table_add_col(result, name_id, new_col);
        ray_release(new_col);
    }
    }

cleanup:
    if (final_ht == &single_ht) {
        group_ht_free(&single_ht);
    }
    if (radix_bufs) {
        size_t n_bufs = (size_t)n_total * RADIX_P;
        for (size_t i = 0; i < n_bufs; i++) scratch_free(radix_bufs[i]._hdr);
        scratch_free(radix_bufs_hdr);
    }
    if (part_hts) {
        for (uint32_t p = 0; p < RADIX_P; p++) {
            if (part_hts[p].rows) group_ht_free(&part_hts[p]);
        }
        scratch_free(part_hts_hdr);
    }
    for (uint8_t a = 0; a < n_aggs; a++)
        if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
    for (uint8_t k = 0; k < n_keys; k++)
        if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
    if (match_idx_block) ray_release(match_idx_block);

    return result;
}

/* --------------------------------------------------------------------------
 * exec_group_per_partition — per-partition GROUP BY with merge
 *
 * Runs exec_group on each partition independently (zero-copy mmap segments),
 * then merges the small partial results via a second exec_group pass.
 *
 * Merge ops: SUM→SUM, COUNT→SUM, MIN→MIN, MAX→MAX, FIRST→FIRST, LAST→LAST.
 * AVG: decomposed into SUM+COUNT per partition, merged, then divided.
 * STDDEV/VAR: decomposed into SUM(x)+SUM(x²)+COUNT(x) per partition,
 *   merged with SUM, then final variance/stddev computed from merged totals.
 *
 * Returns NULL if any step fails (caller falls through to concat path).
 * -------------------------------------------------------------------------- */
static ray_t* __attribute__((noinline))
exec_group_per_partition(ray_t* parted_tbl, ray_op_ext_t* ext,
                         int32_t n_parts, const int64_t* key_syms,
                         const int64_t* agg_syms, int has_avg,
                         int has_stddev, int64_t group_limit) {

    uint8_t n_keys = ext->n_keys;
    uint8_t n_aggs = ext->n_aggs;

    /* Guard: fixed-size arrays below cap at 24 agg ops.
     * Each AVG adds 1 extra (COUNT), each STDDEV/VAR adds 2 (SUM_SQ + COUNT).
     * n_aggs + n_avg + 2*n_std must stay within 24. */
    if (n_aggs > 8 || n_keys > 8) return NULL;

    /* Identify MAPCOMMON vs PARTED keys.  MAPCOMMON keys are constant
     * within a partition, so they are excluded from per-partition GROUP BY
     * and reconstructed after concat. */
    uint8_t  n_mc_keys = 0;
    int64_t  mc_sym_ids[8];
    uint8_t  n_part_keys = 0;
    int64_t  pk_syms[8];       /* non-MAPCOMMON key sym IDs */

    for (uint8_t k = 0; k < n_keys; k++) {
        ray_t* pcol = ray_table_get_col(parted_tbl, key_syms[k]);
        if (pcol && pcol->type == RAY_MAPCOMMON) {
            mc_sym_ids[n_mc_keys++] = key_syms[k];
        } else {
            pk_syms[n_part_keys++] = key_syms[k];
        }
    }

    /* LIMIT pushdown: when all GROUP BY keys are MAPCOMMON (n_part_keys==0),
     * each partition produces exactly 1 group.  Limit the partition loop. */
    if (group_limit > 0 && n_part_keys == 0 && group_limit < n_parts)
        n_parts = (int32_t)group_limit;

    /* Decomposition: AVG(x) → SUM(x) + COUNT(x).
     * STDDEV/VAR(x) → SUM(x) + SUM(x²) + COUNT(x).
     * Build per-partition agg_ops with decomposed ops, then merge ops. */
    uint16_t part_ops[24];   /* per-partition agg ops */
    uint16_t merge_ops[24];  /* merge agg ops */
    uint8_t  avg_idx[8];     /* which original agg slots are AVG */
    uint8_t  std_idx[8];     /* which original agg slots are STDDEV/VAR */
    uint16_t std_orig_op[8]; /* original op for each std slot */
    uint8_t  n_avg = 0;
    uint8_t  n_std = 0;
    uint8_t  part_n_aggs = n_aggs;
    /* stddev_needs_sq[a]: index into part_ops for the SUM(x²) slot */
    uint8_t  std_sq_slot[8];
    uint8_t  std_cnt_slot[8];

    for (uint8_t a = 0; a < n_aggs; a++) {
        uint16_t aop = ext->agg_ops[a];
        if (aop == OP_AVG) {
            part_ops[a] = OP_SUM;     /* partition: compute SUM */
            avg_idx[n_avg++] = a;
        } else if (aop == OP_STDDEV || aop == OP_STDDEV_POP ||
                   aop == OP_VAR || aop == OP_VAR_POP) {
            part_ops[a] = OP_SUM;     /* partition: compute SUM(x) */
            std_orig_op[n_std] = aop;
            std_idx[n_std++] = a;
        } else {
            part_ops[a] = aop;
        }
    }
    /* Guard: total decomposed slots must fit */
    if (n_aggs + n_avg + 2 * n_std > 24) return NULL;

    /* Append SUM(x²) for each STDDEV/VAR slot */
    for (uint8_t i = 0; i < n_std; i++) {
        std_sq_slot[i] = part_n_aggs;
        part_ops[part_n_aggs++] = OP_SUM;  /* SUM(x²) */
    }
    /* Append COUNT for each AVG column */
    for (uint8_t i = 0; i < n_avg; i++)
        part_ops[part_n_aggs++] = OP_COUNT;
    /* Append COUNT for each STDDEV/VAR column */
    for (uint8_t i = 0; i < n_std; i++) {
        std_cnt_slot[i] = part_n_aggs;
        part_ops[part_n_aggs++] = OP_COUNT;
    }

    /* Merge ops: SUM→SUM, COUNT→SUM, MIN→MIN, MAX→MAX,
     * FIRST→FIRST, LAST→LAST, all appended slots → SUM */
    for (uint8_t a = 0; a < part_n_aggs; a++) {
        merge_ops[a] = part_ops[a];
        if (merge_ops[a] == OP_COUNT) merge_ops[a] = OP_SUM;
    }

    /* Agg input syms for the decomposed ops.
     * AVG's COUNT uses same input column as the AVG itself.
     * STDDEV's SUM(x²) and COUNT use same input column as the STDDEV. */
    int64_t part_agg_syms[24];
    /* Flag: slot needs x*x graph node (for SUM(x²)) */
    int part_needs_sq[24];
    memset(part_needs_sq, 0, sizeof(part_needs_sq));

    for (uint8_t a = 0; a < n_aggs; a++)
        part_agg_syms[a] = agg_syms[a];
    /* SUM(x²) slots for STDDEV/VAR */
    for (uint8_t i = 0; i < n_std; i++) {
        part_agg_syms[std_sq_slot[i]] = agg_syms[std_idx[i]];
        part_needs_sq[std_sq_slot[i]] = 1;
    }
    /* COUNT slots for AVG */
    for (uint8_t i = 0; i < n_avg; i++)
        part_agg_syms[n_aggs + n_std + i] = agg_syms[avg_idx[i]];
    /* COUNT slots for STDDEV/VAR */
    for (uint8_t i = 0; i < n_std; i++)
        part_agg_syms[std_cnt_slot[i]] = agg_syms[std_idx[i]];

    /* ---- Batched incremental merge ----
     * Process partitions in batches of MERGE_BATCH.  After each batch:
     *   Phase 1: exec_group each partition in batch → batch_partials[]
     *   Phase 2: concat (running + batch_partials + MAPCOMMON) → merge_tbl
     *   Phase 3: merge GROUP BY → new running
     * Bounds peak memory to O(MERGE_BATCH × groups_per_partition). */
#define MERGE_BATCH 8

    /* Capture agg column name IDs from first partition result */
    int64_t agg_name_ids[24];
    int agg_names_captured = 0;

    ray_t* running = NULL;
    ray_t* merge_tbl = NULL;      /* last merge table (for column name fixup) */

    for (int32_t batch_start = 0; batch_start < n_parts;
         batch_start += MERGE_BATCH) {

        int32_t batch_end = batch_start + MERGE_BATCH;
        if (batch_end > n_parts) batch_end = n_parts;
        int32_t batch_n = batch_end - batch_start;

        /* Phase 1: exec_group each partition in this batch */
        ray_t* bp[MERGE_BATCH];
        memset(bp, 0, sizeof(bp));

        for (int32_t bi = 0; bi < batch_n; bi++) {
            int32_t p = batch_start + bi;

            /* Collect unique agg input sym IDs (avoid duplicate columns) */
            int64_t unique_agg[24];
            int n_unique_agg = 0;
            for (uint8_t a = 0; a < part_n_aggs; a++) {
                int dup = 0;
                for (int j = 0; j < n_unique_agg; j++)
                    if (unique_agg[j] == part_agg_syms[a]) { dup = 1; break; }
                if (!dup) {
                    for (uint8_t k = 0; k < n_keys; k++)
                        if (key_syms[k] == part_agg_syms[a]) { dup = 1; break; }
                    if (!dup) unique_agg[n_unique_agg++] = part_agg_syms[a];
                }
            }

            ray_t* sub = ray_table_new((int64_t)(n_part_keys + n_unique_agg));
            if (!sub || RAY_IS_ERR(sub)) goto batch_fail;

            for (uint8_t k = 0; k < n_part_keys; k++) {
                ray_t* pcol = ray_table_get_col(parted_tbl, pk_syms[k]);
                if (!pcol || !RAY_IS_PARTED(pcol->type)) {
                    ray_release(sub); goto batch_fail;
                }
                ray_t* seg = ((ray_t**)ray_data(pcol))[p];
                if (!seg) { ray_release(sub); goto batch_fail; }
                ray_retain(seg);
                sub = ray_table_add_col(sub, pk_syms[k], seg);
                ray_release(seg);
            }
            for (int j = 0; j < n_unique_agg; j++) {
                ray_t* pcol = ray_table_get_col(parted_tbl, unique_agg[j]);
                if (!pcol || !RAY_IS_PARTED(pcol->type)) {
                    ray_release(sub); goto batch_fail;
                }
                ray_t* seg = ((ray_t**)ray_data(pcol))[p];
                if (!seg) { ray_release(sub); goto batch_fail; }
                ray_retain(seg);
                sub = ray_table_add_col(sub, unique_agg[j], seg);
                ray_release(seg);
            }

            ray_graph_t* pg = ray_graph_new(sub);
            if (!pg) { ray_release(sub); goto batch_fail; }

            ray_op_t* pkeys[8];
            for (uint8_t k = 0; k < n_part_keys; k++) {
                ray_t* sym_atom = ray_sym_str(pk_syms[k]);
                pkeys[k] = ray_scan(pg, ray_str_ptr(sym_atom));
            }
            ray_op_t* pagg_ins[24];
            for (uint8_t a = 0; a < part_n_aggs; a++) {
                ray_t* sym_atom = ray_sym_str(part_agg_syms[a]);
                pagg_ins[a] = ray_scan(pg, ray_str_ptr(sym_atom));
            }
            for (uint8_t j = 0; j < n_std; j++) {
                uint8_t sq = std_sq_slot[j];
                ray_op_t* x = pagg_ins[sq];
                pagg_ins[sq] = ray_mul(pg, x, x);
            }

            ray_op_t* proot = ray_group(pg, pkeys, n_part_keys,
                                       part_ops, pagg_ins, part_n_aggs);
            proot = ray_optimize(pg, proot);
            bp[bi] = ray_execute(pg, proot);
            ray_graph_free(pg);
            ray_release(sub);

            if (!bp[bi] || RAY_IS_ERR(bp[bi])) goto batch_fail;

            /* Capture agg column name IDs once (all partials share names) */
            if (!agg_names_captured) {
                for (uint8_t a = 0; a < part_n_aggs; a++)
                    agg_name_ids[a] = ray_table_col_name(
                        bp[bi], (int64_t)n_part_keys + a);
                agg_names_captured = 1;
            }
        }

        /* Phase 2: concat (running + batch_partials + MAPCOMMON) */
        int64_t mrows = running ? ray_table_nrows(running) : 0;
        for (int32_t i = 0; i < batch_n; i++)
            mrows += ray_table_nrows(bp[i]);

        if (merge_tbl) { ray_release(merge_tbl); merge_tbl = NULL; }
        merge_tbl = ray_table_new((int64_t)(n_keys + part_n_aggs));
        if (!merge_tbl || RAY_IS_ERR(merge_tbl)) {
            merge_tbl = NULL; goto batch_fail;
        }

        /* Key columns */
        for (uint8_t k = 0; k < n_keys; k++) {
            int is_mc = 0;
            for (uint8_t m = 0; m < n_mc_keys; m++)
                if (mc_sym_ids[m] == key_syms[k]) { is_mc = 1; break; }

            /* Type reference for column allocation */
            ray_t* tref = NULL;
            if (running) {
                tref = ray_table_get_col(running, key_syms[k]);
            } else if (is_mc) {
                ray_t* mc_col = ray_table_get_col(parted_tbl, key_syms[k]);
                tref = ((ray_t**)ray_data(mc_col))[0];
            } else {
                tref = ray_table_get_col(bp[0], key_syms[k]);
            }
            if (!tref) goto batch_fail;

            size_t esz = (size_t)col_esz(tref);
            ray_t* flat = col_vec_new(tref, mrows);
            if (!flat || RAY_IS_ERR(flat)) goto batch_fail;
            flat->len = mrows;
            char* out = (char*)ray_data(flat);
            int64_t off = 0;

            /* Copy from running result */
            if (running) {
                ray_t* rc = ray_table_get_col(running, key_syms[k]);
                if (rc && rc->len > 0) {
                    memcpy(out, ray_data(rc), (size_t)rc->len * esz);
                    off = rc->len;
                }
            }

            /* Copy from batch partials */
            for (int32_t i = 0; i < batch_n; i++) {
                int64_t pnrows = ray_table_nrows(bp[i]);
                if (is_mc) {
                    /* MAPCOMMON: replicate this partition's key value */
                    int32_t p = batch_start + i;
                    ray_t* mc_col = ray_table_get_col(parted_tbl, key_syms[k]);
                    ray_t* mc_kv = ((ray_t**)ray_data(mc_col))[0];
                    const char* kdata = (const char*)ray_data(mc_kv);
                    for (int64_t r = 0; r < pnrows; r++)
                        memcpy(out + (size_t)(off + r) * esz,
                               kdata + (size_t)p * esz, esz);
                    off += pnrows;
                } else {
                    ray_t* pc = ray_table_get_col(bp[i], key_syms[k]);
                    if (pc && pc->len > 0) {
                        memcpy(out + (size_t)off * esz,
                               ray_data(pc), (size_t)pc->len * esz);
                        off += pc->len;
                    }
                }
            }

            merge_tbl = ray_table_add_col(merge_tbl, key_syms[k], flat);
            ray_release(flat);
        }

        /* Agg columns */
        for (uint8_t a = 0; a < part_n_aggs; a++) {
            ray_t* tref = running
                ? ray_table_get_col_idx(running, (int64_t)n_keys + a)
                : ray_table_get_col_idx(bp[0], (int64_t)n_part_keys + a);
            if (!tref) goto batch_fail;

            size_t esz = (size_t)col_esz(tref);
            ray_t* flat = col_vec_new(tref, mrows);
            if (!flat || RAY_IS_ERR(flat)) goto batch_fail;
            flat->len = mrows;
            char* out = (char*)ray_data(flat);
            int64_t off = 0;

            if (running) {
                ray_t* rc = ray_table_get_col_idx(running, (int64_t)n_keys + a);
                if (rc && rc->len > 0) {
                    memcpy(out, ray_data(rc), (size_t)rc->len * esz);
                    off = rc->len;
                }
            }

            for (int32_t i = 0; i < batch_n; i++) {
                ray_t* pc = ray_table_get_col_idx(bp[i],
                                                 (int64_t)n_part_keys + a);
                if (pc && pc->len > 0) {
                    memcpy(out + (size_t)off * esz,
                           ray_data(pc), (size_t)pc->len * esz);
                    off += pc->len;
                }
            }

            merge_tbl = ray_table_add_col(merge_tbl, agg_name_ids[a], flat);
            ray_release(flat);
        }

        /* Free batch partials */
        for (int32_t i = 0; i < batch_n; i++) {
            ray_release(bp[i]);
            bp[i] = NULL;
        }

        /* Phase 3: merge GROUP BY */
        ray_graph_t* mg = ray_graph_new(merge_tbl);
        if (!mg) goto batch_fail;

        ray_op_t* mkeys[8];
        for (uint8_t k = 0; k < n_keys; k++) {
            ray_t* sym_atom = ray_sym_str(key_syms[k]);
            mkeys[k] = ray_scan(mg, ray_str_ptr(sym_atom));
        }

        ray_op_t* magg_ins[24];
        for (uint8_t a = 0; a < part_n_aggs; a++) {
            ray_t* agg_name = ray_sym_str(agg_name_ids[a]);
            magg_ins[a] = ray_scan(mg, ray_str_ptr(agg_name));
        }

        ray_op_t* mroot = ray_group(mg, mkeys, n_keys,
                                   merge_ops, magg_ins, part_n_aggs);
        mroot = ray_optimize(mg, mroot);
        ray_t* new_running = ray_execute(mg, mroot);
        ray_graph_free(mg);

        if (running) ray_release(running);
        running = new_running;

        if (!running || RAY_IS_ERR(running)) {
            ray_release(merge_tbl);
            return NULL;
        }

        /* Rename running's agg columns back to the original partial names.
         * Without this, each merge adds an extra suffix (e.g. v1_sum → v1_sum_sum). */
        for (uint8_t a = 0; a < part_n_aggs; a++)
            ray_table_set_col_name(running, (int64_t)n_keys + a, agg_name_ids[a]);

        continue;

batch_fail:
        for (int32_t i = 0; i < batch_n; i++)
            if (bp[i]) ray_release(bp[i]);
        if (running) ray_release(running);
        if (merge_tbl) ray_release(merge_tbl);
        return NULL;
    }

    ray_t* result = running;

    if (!result || RAY_IS_ERR(result)) {
        if (merge_tbl) ray_release(merge_tbl);
        return NULL;
    }

    int64_t rncols = ray_table_ncols(result);

    /* AVG/STDDEV post-processing: build trimmed table (n_keys + n_aggs cols),
     * computing final AVG = SUM/COUNT and STDDEV/VAR from SUM, SUM_SQ, COUNT. */
    if (has_avg || has_stddev) {
        ray_t* trimmed = ray_table_new((int64_t)(n_keys + n_aggs));
        if (!trimmed || RAY_IS_ERR(trimmed)) {
            ray_release(result);
            if (merge_tbl) ray_release(merge_tbl);
            return NULL;
        }

        for (int64_t c = 0; c < (int64_t)(n_keys + n_aggs) && c < rncols; c++) {
            int64_t nm = ray_table_col_name(result, c);

            /* Check if this agg column is an AVG or STDDEV/VAR slot */
            int is_avg_slot = 0, is_std_slot = 0;
            uint8_t avg_i = 0, std_i = 0;
            if (c >= n_keys) {
                uint8_t a = (uint8_t)(c - n_keys);
                for (uint8_t j = 0; j < n_avg; j++) {
                    if (avg_idx[j] == a) { is_avg_slot = 1; avg_i = j; break; }
                }
                for (uint8_t j = 0; j < n_std; j++) {
                    if (std_idx[j] == a) { is_std_slot = 1; std_i = j; break; }
                }
            }

            if (is_avg_slot) {
                /* AVG = SUM(x) / COUNT(x) */
                int64_t sum_ci = c;
                /* AVG COUNT slots: after n_aggs + n_std SUM_SQ slots */
                int64_t cnt_ci = (int64_t)n_keys + n_aggs + n_std + avg_i;
                ray_t* sum_col = ray_table_get_col_idx(result, sum_ci);
                ray_t* cnt_col = (cnt_ci < rncols) ? ray_table_get_col_idx(result, cnt_ci) : NULL;
                if (!sum_col || !cnt_col) {
                    if (sum_col) {
                        ray_retain(sum_col);
                        trimmed = ray_table_add_col(trimmed, nm, sum_col);
                        ray_release(sum_col);
                    }
                    continue;
                }

                int64_t nrows = sum_col->len;
                ray_t* avg_col = ray_vec_new(RAY_F64, nrows);
                if (!avg_col || RAY_IS_ERR(avg_col)) {
                    ray_release(trimmed); ray_release(result);
                    if (merge_tbl) ray_release(merge_tbl);
                    return NULL;
                }
                avg_col->len = nrows;

                double* out = (double*)ray_data(avg_col);
                if (sum_col->type == RAY_F64) {
                    const double* sv = (const double*)ray_data(sum_col);
                    const int64_t* cv = (const int64_t*)ray_data(cnt_col);
                    for (int64_t r = 0; r < nrows; r++)
                        out[r] = cv[r] > 0 ? sv[r] / (double)cv[r] : 0.0;
                } else {
                    const int64_t* sv = (const int64_t*)ray_data(sum_col);
                    const int64_t* cv = (const int64_t*)ray_data(cnt_col);
                    for (int64_t r = 0; r < nrows; r++)
                        out[r] = cv[r] > 0 ? (double)sv[r] / (double)cv[r] : 0.0;
                }
                trimmed = ray_table_add_col(trimmed, nm, avg_col);
                ray_release(avg_col);
            } else if (is_std_slot) {
                /* STDDEV/VAR from merged SUM(x), SUM(x²), COUNT(x):
                 * var_pop = SUM_SQ/N - (SUM/N)²
                 * var_samp = var_pop * N/(N-1)
                 * stddev_pop = sqrt(var_pop), stddev_samp = sqrt(var_samp) */
                int64_t sum_ci = c;
                int64_t sq_ci  = (int64_t)n_keys + std_sq_slot[std_i];
                int64_t cnt_ci = (int64_t)n_keys + std_cnt_slot[std_i];
                ray_t* sum_col = ray_table_get_col_idx(result, sum_ci);
                ray_t* sq_col  = (sq_ci < rncols) ? ray_table_get_col_idx(result, sq_ci) : NULL;
                ray_t* cnt_col = (cnt_ci < rncols) ? ray_table_get_col_idx(result, cnt_ci) : NULL;
                if (!sum_col || !sq_col || !cnt_col) {
                    if (sum_col) {
                        ray_retain(sum_col);
                        trimmed = ray_table_add_col(trimmed, nm, sum_col);
                        ray_release(sum_col);
                    }
                    continue;
                }

                int64_t nrows = sum_col->len;
                ray_t* out_col = ray_vec_new(RAY_F64, nrows);
                if (!out_col || RAY_IS_ERR(out_col)) {
                    ray_release(trimmed); ray_release(result);
                    if (merge_tbl) ray_release(merge_tbl);
                    return NULL;
                }
                out_col->len = nrows;
                double* out = (double*)ray_data(out_col);

                uint16_t orig_op = std_orig_op[std_i];
                /* SUM(x) is always F64 after merge (SUM produces F64 for F64 input,
                 * I64 for integer input; SUM(x²) via ray_mul always produces F64). */
                const double* sq = (const double*)ray_data(sq_col);
                const int64_t* cv = (const int64_t*)ray_data(cnt_col);
                if (sum_col->type == RAY_F64) {
                    const double* sv = (const double*)ray_data(sum_col);
                    for (int64_t r = 0; r < nrows; r++) {
                        double n = (double)cv[r];
                        if (n <= 0) { out[r] = 0.0; ray_vec_set_null(out_col, r, true); continue; }
                        double mean = sv[r] / n;
                        double var_pop = sq[r] / n - mean * mean;
                        if (var_pop < 0) var_pop = 0;
                        bool insuf = (orig_op == OP_VAR || orig_op == OP_STDDEV) && n <= 1;
                        if (insuf) { out[r] = 0.0; ray_vec_set_null(out_col, r, true); continue; }
                        if (orig_op == OP_VAR_POP)         out[r] = var_pop;
                        else if (orig_op == OP_VAR)         out[r] = var_pop * n / (n - 1);
                        else if (orig_op == OP_STDDEV_POP)  out[r] = sqrt(var_pop);
                        else /* OP_STDDEV */                out[r] = sqrt(var_pop * n / (n - 1));
                    }
                } else {
                    const int64_t* sv = (const int64_t*)ray_data(sum_col);
                    for (int64_t r = 0; r < nrows; r++) {
                        double n = (double)cv[r];
                        if (n <= 0) { out[r] = 0.0; ray_vec_set_null(out_col, r, true); continue; }
                        double mean = (double)sv[r] / n;
                        double var_pop = sq[r] / n - mean * mean;
                        if (var_pop < 0) var_pop = 0;
                        bool insuf = (orig_op == OP_VAR || orig_op == OP_STDDEV) && n <= 1;
                        if (insuf) { out[r] = 0.0; ray_vec_set_null(out_col, r, true); continue; }
                        if (orig_op == OP_VAR_POP)         out[r] = var_pop;
                        else if (orig_op == OP_VAR)         out[r] = var_pop * n / (n - 1);
                        else if (orig_op == OP_STDDEV_POP)  out[r] = sqrt(var_pop);
                        else /* OP_STDDEV */                out[r] = sqrt(var_pop * n / (n - 1));
                    }
                }
                trimmed = ray_table_add_col(trimmed, nm, out_col);
                ray_release(out_col);
            } else {
                ray_t* col = ray_table_get_col_idx(result, c);
                if (col) {
                    ray_retain(col);
                    trimmed = ray_table_add_col(trimmed, nm, col);
                    ray_release(col);
                }
            }
        }
        ray_release(result);
        result = trimmed;
        rncols = ray_table_ncols(result);
    }

    /* Agg column names already fixed by ray_table_set_col_name inside batch loop.
     * Apply final name fixup for the user-facing n_aggs columns (trim decomposed extras). */
    for (uint8_t a = 0; a < n_aggs && (int64_t)(n_keys + a) < rncols; a++)
        ray_table_set_col_name(result, (int64_t)n_keys + a, agg_name_ids[a]);

    if (merge_tbl) ray_release(merge_tbl);
    return result;
}

/* ══════════════════════════════════════════════════════════════════════
 * pivot_ingest_run — shared parallel hash-aggregate for pivot
 *
 * Mirrors the phase1+phase2 radix pipeline exec_group uses, leaving
 * the result in per-partition HTs with prefix offsets so the caller
 * can iterate grouped rows without knowing about the radix internals.
 * Falls back to a single sequential HT for tiny inputs or when no
 * pool is available — the caller iterates n_parts ∈ {1, RADIX_P}.
 * ══════════════════════════════════════════════════════════════════════ */

static void pivot_ingest_sequential(pivot_ingest_t* out, const ght_layout_t* ly,
                                     void** key_data, int8_t* key_types,
                                     uint8_t* key_attrs, ray_t** key_vecs,
                                     ray_t** agg_vecs, int64_t n_scan,
                                     group_ht_t* scratch_ht) {
    (void)key_data;
    out->part_hts = scratch_ht;
    out->n_parts = 1;
    out->row_stride = ly->row_stride;
    group_rows_range(scratch_ht, key_data, key_types, key_attrs, key_vecs,
                     agg_vecs, 0, n_scan, NULL);
    out->total_grps = scratch_ht->grp_count;
    out->part_offsets[0] = 0;
    out->part_offsets[1] = scratch_ht->grp_count;
    out->part_hts = scratch_ht;
}

bool pivot_ingest_run(pivot_ingest_t* out,
                      const ght_layout_t* ly,
                      void** key_data, int8_t* key_types, uint8_t* key_attrs,
                      ray_t** key_vecs, ray_t** agg_vecs,
                      int64_t n_scan) {
    memset(out, 0, sizeof(*out));
    out->row_stride = ly->row_stride;

    /* Allocate a small offsets buffer up front (RADIX_P+1 is the max). */
    out->part_offsets = (uint32_t*)scratch_alloc(&out->_offsets_hdr,
        (size_t)(RADIX_P + 1) * sizeof(uint32_t));
    if (!out->part_offsets) return false;

    uint8_t n_keys = ly->n_keys;

    ray_pool_t* pool = ray_pool_get();
    uint32_t n_total = pool ? ray_pool_total_workers(pool) : 1;
    bool parallel_ok = (pool && n_scan >= RAY_PARALLEL_THRESHOLD && n_total > 1);

    if (!parallel_ok) {
        /* Sequential single-HT path — allocate the HT in its own scratch
         * block and wire part_hts/n_parts immediately so every failure
         * below funnels through pivot_ingest_free for cleanup. */
        group_ht_t* seq = (group_ht_t*)scratch_calloc(&out->_part_hts_hdr,
            sizeof(group_ht_t));
        if (!seq) return false;
        out->part_hts = seq;
        out->n_parts = 1;
        uint32_t seq_cap = 1024;
        uint64_t target = (uint64_t)n_scan * 2;
        while ((uint64_t)seq_cap < target && seq_cap < (1u << 24)) seq_cap <<= 1;
        if (!group_ht_init(seq, seq_cap, ly)) return false;
        pivot_ingest_sequential(out, ly, key_data, key_types, key_attrs,
                                key_vecs, agg_vecs, n_scan, seq);
        /* Surface grow-path OOM from group_probe_entry so callers don't
         * silently see a truncated result. */
        if (seq->oom) return false;
        return true;
    }

    /* ═════ Parallel radix path ═════ */
    size_t n_bufs = (size_t)n_total * RADIX_P;
    out->_n_bufs = n_bufs;
    radix_buf_t* radix_bufs = (radix_buf_t*)scratch_calloc(&out->_radix_bufs_hdr,
        n_bufs * sizeof(radix_buf_t));
    if (!radix_bufs) return false;
    out->_radix_bufs = radix_bufs;

    uint32_t buf_init = (uint32_t)((uint64_t)n_scan / (RADIX_P * n_total));
    if (buf_init < 64) buf_init = 64;
    buf_init = buf_init + buf_init / 2;
    uint16_t estride = ly->entry_stride;
    {
        size_t total_pre = (size_t)n_bufs * buf_init * estride;
        if (total_pre > (size_t)2 << 30) {
            buf_init = (uint32_t)(((size_t)2 << 30) / ((size_t)n_bufs * estride));
            if (buf_init < 64) buf_init = 64;
        }
    }
    for (size_t i = 0; i < n_bufs; i++) {
        radix_bufs[i].data = (char*)scratch_alloc(&radix_bufs[i]._hdr,
            (size_t)buf_init * estride);
        radix_bufs[i].count = 0;
        radix_bufs[i].cap = buf_init;
    }

    uint8_t p1_nullable = 0;
    for (uint8_t k = 0; k < n_keys; k++) {
        if (!key_vecs[k]) continue;
        ray_t* src = (key_vecs[k]->attrs & RAY_ATTR_SLICE)
                     ? key_vecs[k]->slice_parent : key_vecs[k];
        if (src && (src->attrs & RAY_ATTR_HAS_NULLS))
            p1_nullable |= (uint8_t)(1u << k);
    }

    radix_phase1_ctx_t p1ctx = {
        .key_data      = key_data,
        .key_types     = key_types,
        .key_attrs     = key_attrs,
        .key_vecs      = key_vecs,
        .nullable_mask = p1_nullable,
        .agg_vecs      = agg_vecs,
        .n_workers     = n_total,
        .bufs          = radix_bufs,
        .layout        = *ly,
        .match_idx     = NULL,
    };
    ray_pool_dispatch(pool, radix_phase1_fn, &p1ctx, n_scan);
    if (ray_interrupted()) return true; /* caller checks ray_interrupted() */
    /* Sync point — phase1 drained all rows, so rows_done == n_scan. */
    ray_progress_update(NULL, "hash-partition", (uint64_t)n_scan, (uint64_t)n_scan);

    for (size_t i = 0; i < n_bufs; i++)
        if (radix_bufs[i].oom) return false;

    group_ht_t* part_hts = (group_ht_t*)scratch_calloc(&out->_part_hts_hdr,
        RADIX_P * sizeof(group_ht_t));
    if (!part_hts) return false;

    radix_phase2_ctx_t p2ctx = {
        .key_types = key_types,
        .n_keys    = n_keys,
        .n_workers = n_total,
        .bufs      = radix_bufs,
        .part_hts  = part_hts,
        .layout    = *ly,
        .key_data  = key_data,
    };
    ray_pool_dispatch_n(pool, radix_phase2_fn, &p2ctx, RADIX_P);
    out->part_hts = part_hts;
    out->n_parts = RADIX_P;
    if (ray_interrupted()) return true;
    /* Sync point — partitions materialized; show RADIX_P/RADIX_P. */
    ray_progress_update(NULL, "per-partition aggregate", RADIX_P, RADIX_P);

    /* OOM detection for the parallel path. Two distinct failure modes
     * must be caught here so callers never see a silently-truncated
     * result:
     *   (a) phase2 init failed — radix_phase2_fn `continue`s when
     *       group_ht_init_sized returns false, leaving the partition
     *       HT with NULL rows despite a non-zero buffer count. Every
     *       entry routed into that partition would be dropped.
     *   (b) grow-path OOM — group_probe_entry sets part_hts[p].oom
     *       on scratch_realloc failure and returns without inserting
     *       the key, silently truncating later groups. */
    for (uint32_t p = 0; p < RADIX_P; p++) {
        if (part_hts[p].oom) return false;
        if (part_hts[p].rows) continue;
        uint32_t pcount = 0;
        for (uint32_t w = 0; w < n_total; w++)
            pcount += radix_bufs[(size_t)w * RADIX_P + p].count;
        if (pcount) return false;
    }

    out->part_offsets[0] = 0;
    for (uint32_t p = 0; p < RADIX_P; p++)
        out->part_offsets[p + 1] = out->part_offsets[p] + part_hts[p].grp_count;
    out->total_grps = out->part_offsets[RADIX_P];
    return true;
}

void pivot_ingest_free(pivot_ingest_t* out) {
    if (!out) return;
    if (out->part_hts) {
        for (uint32_t p = 0; p < out->n_parts; p++) {
            if (out->part_hts[p].rows || out->part_hts[p].slots)
                group_ht_free(&out->part_hts[p]);
        }
        scratch_free(out->_part_hts_hdr);
    }
    if (out->_radix_bufs) {
        radix_buf_t* bufs = (radix_buf_t*)out->_radix_bufs;
        for (size_t i = 0; i < out->_n_bufs; i++) scratch_free(bufs[i]._hdr);
        scratch_free(out->_radix_bufs_hdr);
    }
    scratch_free(out->_offsets_hdr);
    memset(out, 0, sizeof(*out));
}
