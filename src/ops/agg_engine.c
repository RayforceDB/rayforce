/* src/ops/agg_engine.c — v2 aggregation engine. */
#include "ops/agg_engine.h"
#include "ops/agg_registry.h"
#include "ops/ops.h"
#include "ops/hash.h"     /* ray_hash_bytes — wide (STR) group-key hashing */
#include "ops/internal.h"  /* col_vec_new, col_esz */
#include "ops/rowsel.h"    /* ray_rowsel_meta, ray_rowsel_to_indices */
#include "lang/internal.h" /* sym_domain_rep */
#include "table/domain.h"
#include "table/sym.h"    /* ray_read_sym */
#include "core/platform.h" /* ray_cache_llc_bytes — replicated slab bound */
#include <stdlib.h>
#include <math.h>
#include <string.h>

/* Radix output address: high 32 bits partition, low 32 bits local group. */
typedef struct { int64_t idx; } agg_radix_order_t;

bool ray_agg_engine_v2 = true;   /* knob; default on */

static _Thread_local agg_route_stats_t route_stats;
void agg_route_reset(void) { memset(&route_stats, 0, sizeof(route_stats)); }
agg_route_stats_t agg_route_stats(void) { return route_stats; }
void agg_route_note_key_domain(void) { route_stats.key_domain_evals++; }
void agg_route_record(agg_route_t route) {
    static const char* const names[AGG_ROUTE_COUNT] = {
        "group: none", "group: legacy", "group: slices", "group: parted",
        "group: v2 serial dense", "group: v2 serial hash", "group: v2 dense",
        "group: v2 radix", "group: v2 hash", "group: v2 smallhash", "group: v2 indexed"
    };
    route_stats.routes[route]++;
    ray_profile_tick(names[route]);
}
void agg_route_reason(agg_v2_reason_t reason) {
    route_stats.last_v2_reason = reason;
    static const char* const names[] = {
        "group: v2 admitted", "group: v2 unsupported shape",
        "group: v2 key expression", "group: v2 unsupported key type",
        "group: v2 aggregate expression", "group: v2 unsupported aggregate type",
        "group: v2 buffered aggregate", "group: v2 unsupported parameter",
        "group: v2 disabled", "group: v2 emit filter", "group: parallel wide-key strategy"
    };
    if (reason != AGG_V2_ADMITTED) ray_profile_tick(names[reason]);
}

/* Read element `row` of an integer/temporal/SYM column widened to int64. */
static inline int64_t agg_read_key_i64(ray_t* col, const void* data, int64_t row);
/* Dense direct-index serial grouping; defined below agg_run_one. */
static int agg_group_keys_dense(ray_t** key_cols, int64_t nrows,
                                const dense_plan_t* dp, agg_groups_t* out);
static int agg_group_keys_parallel(ray_t** keys, uint32_t nkeys, int64_t rows,
                                    const dense_plan_t* dp, agg_groups_t* out);

static bool agg_cancelled(void) {
    ray_pool_t* pool = ray_pool_get();
    return pool && atomic_load_explicit(&pool->cancelled, memory_order_relaxed);
}

/* Row-index consumers share one grouping and one stable index layout. */
static bool agg_indexed_supported(uint16_t op, int8_t t) {
    bool number = t >= RAY_BOOL && t <= RAY_TIMESTAMP;
    bool scalar = number || t == RAY_GUID || t == RAY_SYM || t == RAY_STR;
    if (op == OP_FIRST || op == OP_LAST) return scalar || t == RAY_LIST;
    if (op == OP_MIN || op == OP_MAX) return t == RAY_GUID || t == RAY_STR || t == RAY_SYM;
    if (op == OP_MEDIAN || op == OP_QUANTILE) return number;
    if (op == OP_MODE || op == OP_TOP_N || op == OP_BOT_N) return scalar;
    return false;
}

agg_v2_reason_t agg_v2_admission(ray_graph_t* g, ray_op_t* op, ray_t* tbl) {
    if (!g || !op || !tbl) return AGG_V2_SHAPE;
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return AGG_V2_SHAPE;
    /* Unbounded keys (>=1): every per-run key buffer the v2 engine touches
     * (key_cols/key_syms, the radix scatter's per-worker key row, key_data,
     * agg_group_keys' data[]) is now an exact carve, and the key-count params
     * downstream are uint32_t — so there is no fixed [16]/[255] cap left to
     * protect.  ext->n_keys is uint32_t (widened, Task 1); dense direct-index
     * routing still self-limits to <=16 inside agg_dense_plan (see there). */
    if (ext->n_keys < 1) return AGG_V2_SHAPE;  /* need >=1 key */
    /* n_aggs == 0 (table-distinct: ray_group(keys, n_keys, NULL, NULL, 0)) is
     * now ADMITTED (cut-3): every strategy below — dense/smallhash/radix
     * parallel, and the serial dense/hash tail in exec_group_v2_run — sizes
     * its per-agg state from ext->n_aggs (agg_vo_init's block/vts/off carve,
     * the per-strategy output-column loops) with no agg assumed to exist, so
     * a 0-agg run just emits the grouped key columns with no agg columns
     * appended — exactly table-distinct's needed shape.  Before this, 0-agg
     * groups fell through to the legacy exec_group_run tail, whose
     * `n_keys > 8` guard (group.c) died `nyi` on any 9+-column
     * `(distinct t)` — a live bug, not a deliberate width cap; fixed here
     * instead of adding a parallel distinct-only route. */
    /* A pushed WHERE filter (g->selection set) is now handled by exec_group_v2's
     * compact-table prologue: it gathers the selected rows of the keys/agg-inputs
     * and runs the normal strategy dispatch on that compact table.  No bail. */

    /* Factorized-EXPAND result (synthetic _src key + _count weight column) needs
     * exec_group's dedicated factorized handling (COUNT/SUM(_count) weight by
     * _count). v2 would compute a plain count — defer the whole shape. */
    if (ext->n_keys == 1 && op_node(g, ext->keys[0]) && op_node(g, ext->keys[0])->opcode == OP_SCAN) {
        ray_op_ext_t* ke = find_ext(g, ext->keys[0]);
        if (ke && ke->sym == ray_sym_intern("_src", 4)) {
            ray_t* cnt = ray_table_get_col(tbl, ray_sym_intern("_count", 6));
            if (cnt && cnt->type == RAY_I64) return AGG_V2_SHAPE;
        }
    }

    /* every key must be a plain column scan of a supported type */
    for (uint32_t k = 0; k < ext->n_keys; k++) {
        ray_op_t* key = op_node(g, ext->keys[k]);
        if (!key || key->opcode != OP_SCAN) return AGG_V2_KEY_EXPRESSION;
        ray_op_ext_t* kext = find_ext(g, key->id);
        ray_t* kc = kext ? ray_table_get_col(tbl, kext->sym) : NULL;
        if (!kc) return AGG_V2_SHAPE;
        switch (kc->type) {
            case RAY_I64: case RAY_I32: case RAY_I16: case RAY_U8:
            case RAY_BOOL: case RAY_DATE: case RAY_TIME:
            case RAY_TIMESTAMP: case RAY_SYM:
            case RAY_F32: case RAY_F64: case RAY_GUID: case RAY_STR: case RAY_LIST: break;
            default: return AGG_V2_KEY_TYPE;
        }
    }

    /* The native float hash reducer avoids building row slices for small
     * parallel pools. At larger pools its replicated group states cost more
     * than the shared directory. Byte/structural keys always use full-key
     * grouping, and ordered/buffered consumers always share row slices. */
    ray_pool_t* pool = ray_pool_get();
    uint32_t workers = pool ? ray_pool_total_workers(pool) : 1;
    if (ext->n_keys <= 8 && workers >= 2 && workers <= 8 &&
            ray_table_nrows(tbl) >= RAY_PARALLEL_THRESHOLD) {
        bool floating = false, indexed = false;
        for (uint32_t k = 0; k < ext->n_keys; k++) {
            ray_op_ext_t* ke = find_ext(g, ext->keys[k]);
            ray_t* col = ray_table_get_col(tbl, ke->sym);
            if (col->type == RAY_F32 || col->type == RAY_F64) floating = true;
            if (col->type == RAY_STR || col->type == RAY_GUID || col->type == RAY_LIST) indexed = true;
        }
        for (uint32_t a = 0; a < ext->n_aggs; a++) {
            ray_op_ext_t* ie = find_ext(g, ext->agg_ins[a]);
            ray_t* col = ie ? ray_table_get_col(tbl, ie->sym) : NULL;
            if (col && agg_indexed_supported(ext->agg_ops[a], col->type)) indexed = true;
        }
        if (floating && !indexed) return AGG_V2_PARALLEL_WIDE;
    }

    /* every aggregate must be a registry-resolvable plain-column scan */
    for (uint32_t a = 0; a < ext->n_aggs; a++) {
        if (ext->agg_k && ext->agg_k[a]) {
            if (ext->agg_ops[a] != OP_TOP_N && ext->agg_ops[a] != OP_BOT_N && ext->agg_ops[a] != OP_QUANTILE) return AGG_V2_PARAMETER;
            if (ext->agg_ops[a] != OP_QUANTILE && ext->agg_k[a] < 1) return AGG_V2_PARAMETER;
            ray_op_t* in = op_node(g, ext->agg_ins[a]);
            if (!in || in->opcode != OP_SCAN) return AGG_V2_AGG_EXPRESSION;
            ray_op_ext_t* ie = find_ext(g, in->id);
            ray_t* ic = ie ? ray_table_get_col(tbl, ie->sym) : NULL;
            if (ic && agg_indexed_supported(ext->agg_ops[a], ic->type)) continue;
            const agg_vtable_t* vt = ic ? agg_resolve(ext->agg_ops[a], ic->type) : NULL;
            if (!vt) return AGG_V2_AGG_TYPE;
            if (vt->kind != ACC_STREAMING) return AGG_V2_BUFFERED;
            continue;  /* admitted */
        }
        if (ext->agg_ins2 && ext->agg_ins2[a] != RAY_OP_NONE) {
            if (!agg_is_binary_agg(ext->agg_ops[a])) return AGG_V2_SHAPE;
            ray_op_t* xin = op_node(g, ext->agg_ins[a]); ray_op_t* yin = op_node(g, ext->agg_ins2[a]);
            if (!xin || xin->opcode != OP_SCAN || !yin || yin->opcode != OP_SCAN) return AGG_V2_AGG_EXPRESSION;
            ray_op_ext_t* xe = find_ext(g, xin->id); ray_op_ext_t* ye = find_ext(g, yin->id);
            ray_t* xc = xe ? ray_table_get_col(tbl, xe->sym) : NULL;
            ray_t* yc = ye ? ray_table_get_col(tbl, ye->sym) : NULL;
            if (!xc || !yc) return AGG_V2_SHAPE;
            if (!agg_resolve(ext->agg_ops[a], xc->type)) return AGG_V2_AGG_TYPE;
            if (!agg_resolve(ext->agg_ops[a], yc->type)) return AGG_V2_AGG_TYPE;
            continue;  /* admitted */
        }
        if (ext->agg_ops[a] == OP_COUNT) {
            /* count: needs no typed input column */
            if (!agg_resolve(OP_COUNT, RAY_I64)) return AGG_V2_SHAPE;
            continue;
        }
        ray_op_t* in = op_node(g, ext->agg_ins[a]);
        if (!in || in->opcode != OP_SCAN) return AGG_V2_AGG_EXPRESSION;
        ray_op_ext_t* ie = find_ext(g, in->id);
        ray_t* ic = (ie) ? ray_table_get_col(tbl, ie->sym) : NULL;
        if (!ic) return AGG_V2_SHAPE;
        if (agg_indexed_supported(ext->agg_ops[a], ic->type)) continue;
        const agg_vtable_t* vt = agg_resolve(ext->agg_ops[a], ic->type);
        if (!vt) return AGG_V2_AGG_TYPE;
        if (vt->kind != ACC_STREAMING) return AGG_V2_BUFFERED;
    }
    return AGG_V2_ADMITTED;
}

bool agg_v2_can_handle(ray_graph_t* g, ray_op_t* op, ray_t* tbl) {
    return agg_v2_admission(g, op, tbl) == AGG_V2_ADMITTED;
}

/* True when the v2 engine would run this group through a bounded dense
 * (direct-index) plan over the whole table.  Callers use it to predict the
 * strategy class before committing: dense plans scale with the pool, while
 * the unbounded radix route still pays a large serial ordering/emission
 * tail on many-million-group inputs.  Costs one parallel min/max prescan
 * of the non-SYM keys; SYM keys resolve from their domain bounds. */
/* ── Dense grouping eligibility selector (mirrors group.c DA path) ────────
 * Decides whether the key tuple packs into a bounded direct-index slot space
 * (gid = sum_k (key_k - min_k)*strides[k]) so grouping can skip hashing.
 * Eligible iff: 1..16 integer/temporal/SYM keys, including reserved null slots,
 * and the product of per-key ranges is no larger than the input row
 * count (computed overflow-safely). Performs one min/max prescan per key column.
 * Aggregates may be ACC_STREAMING or ACC_BUFFERED (median/top-k): the dense
 * serial driver carries the per-group destroy lifecycle for buffered state. */
/* Nullable signed domains reserve the last slot; never span the sentinel. */
static int64_t agg_key_null(int8_t type) {
    switch (type) {
        case RAY_I16: return NULL_I16;
        case RAY_I32: case RAY_DATE: case RAY_TIME: return NULL_I32;
        default: return NULL_I64;
    }
}
/* Raw-range component: the hot loops use this whenever the plan has no
 * compacted key.  A per-row check of the remap pointer inside the row loop
 * defeats the compiler's pipelining of the multi-key packing (measured 12x
 * slower on a two-integer-key sum), so callers hoist `dp->compacted` out of
 * their loops and pick one of the two forms. */
static inline int64_t agg_dense_component_raw(const dense_plan_t* dp, uint32_t k, int64_t v) {
    return dp->nullable[k] && v == dp->nulls[k]
        ? dp->ranges[k] - 1 : v - dp->mins[k];
}
static inline int64_t agg_dense_component(const dense_plan_t* dp, uint32_t k, int64_t v) {
    if (dp->nullable[k] && v == dp->nulls[k]) return dp->ranges[k] - 1;
    int64_t c = v - dp->mins[k];
    return dp->remap[k] ? dp->remap[k][c] : c;
}
/* Original key code of a dense component (inverse of agg_dense_component). */
static inline int64_t agg_dense_code(const dense_plan_t* dp, uint32_t k, int64_t component) {
    if (dp->nullable[k] && component == dp->ranges[k] - 1) return dp->nulls[k];
    return dp->inverse[k] ? dp->inverse[k][component] : dp->mins[k] + component;
}

void agg_dense_plan_free(dense_plan_t* dp) {
    if (!dp) return;
    for (uint32_t k = 0; k < 16; k++) {
        ray_free_raw(dp->remap[k]); dp->remap[k] = NULL;
        ray_free_raw(dp->inverse[k]); dp->inverse[k] = NULL;
    }
    dp->compacted = false;
}
static bool agg_dense_range(dense_plan_t* dp, uint32_t k, int64_t mn, int64_t mx) {
    if (mx < mn) { dp->mins[k] = 0; dp->ranges[k] = 1; return dp->nullable[k]; }
    uint64_t span = (uint64_t)mx - (uint64_t)mn;
    if (span >= (uint64_t)INT64_MAX - (uint64_t)dp->nullable[k]) return false;
    dp->mins[k] = mn;
    dp->ranges[k] = (int64_t)span + 1 + dp->nullable[k];
    return true;
}

/* Hoist type and null handling out of range scans so native integer
 * reductions can vectorize, including nullable temporal keys. */
static void agg_key_bounds(ray_t* key, int64_t start, int64_t end, bool nullable,
                           int64_t null, int64_t* lo, int64_t* hi) {
    int64_t mn = INT64_MAX, mx = INT64_MIN;
    const void* data = ray_data(key);
    #define KEY_BOUNDS(T) do { \
        const T* p = data; \
        if (nullable) { \
            for (int64_t r = start; r < end; r++) { \
                int64_t v = (int64_t)p[r]; \
                int64_t low = v == null ? INT64_MAX : v; \
                int64_t high = v == null ? INT64_MIN : v; \
                if (low < mn) mn = low; \
                if (high > mx) mx = high; \
            } \
        } else { \
            for (int64_t r = start; r < end; r++) { \
                int64_t v = (int64_t)p[r]; \
                if (v < mn) mn = v; \
                if (v > mx) mx = v; \
            } \
        } \
    } while (0)
    switch (key->type) {
        case RAY_I64: case RAY_TIMESTAMP: KEY_BOUNDS(int64_t); break;
        case RAY_I32: case RAY_DATE: case RAY_TIME: KEY_BOUNDS(int32_t); break;
        case RAY_I16: KEY_BOUNDS(int16_t); break;
        case RAY_U8: case RAY_BOOL: KEY_BOUNDS(uint8_t); break;
        case RAY_SYM:
            switch (key->attrs & RAY_SYM_W_MASK) {
                case RAY_SYM_W8: KEY_BOUNDS(uint8_t); break;
                case RAY_SYM_W16: KEY_BOUNDS(uint16_t); break;
                case RAY_SYM_W32: KEY_BOUNDS(uint32_t); break;
                default: KEY_BOUNDS(int64_t); break;
            }
            break;
    }
    #undef KEY_BOUNDS
    *lo = mn; *hi = mx;
}

typedef struct {
    ray_t* key;
    bool nullable;
    int64_t null, rows;
    uint32_t tasks;
    int64_t* bounds;
} agg_key_bounds_ctx_t;
static void agg_key_bounds_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_key_bounds_ctx_t* c = raw;
    for (int64_t task = start; task < end; task++) {
        int64_t begin = c->rows / c->tasks * task;
        int64_t limit = task + 1 == c->tasks ? c->rows : c->rows / c->tasks * (task + 1);
        agg_key_bounds(c->key, begin, limit, c->nullable, c->null,
                       &c->bounds[task * 2], &c->bounds[task * 2 + 1]);
    }
}
static void agg_key_bounds_parallel(ray_t* key, int64_t rows, bool nullable,
                                    int64_t null, int64_t* lo, int64_t* hi) {
    ray_pool_t* pool = ray_pool_get();
    if (pool && rows >= RAY_PARALLEL_THRESHOLD) {
        uint32_t tasks = ray_pool_total_workers(pool);
        if (tasks > RAY_POOL_MAX_TASKS) tasks = RAY_POOL_MAX_TASKS;
        int64_t* bounds = ray_calloc_raw((size_t)tasks * 2 * sizeof(int64_t));
        if (bounds) {
            agg_key_bounds_ctx_t c = { key, nullable, null, rows, tasks, bounds };
            ray_pool_dispatch_n(pool, agg_key_bounds_fn, &c, tasks);
            *lo = INT64_MAX; *hi = INT64_MIN;
            for (uint32_t task = 0; task < tasks; task++) {
                if (bounds[task * 2] < *lo) *lo = bounds[task * 2];
                if (bounds[task * 2 + 1] > *hi) *hi = bounds[task * 2 + 1];
            }
            ray_free_raw(bounds);
            return;
        }
    }
    agg_key_bounds(key, 0, rows, nullable, null, lo, hi);
}

/* ── Key compaction for composite plans ──────────────────────────────────
 * A composite plan multiplies per-key ranges.  SYM columns of a shared
 * domain (one runtime domain across every column, or a splayed store's
 * shared symfile) interleave their codes with every other column's, so two
 * 100-value SYM keys can each span a 100k-code range and their raw product
 * (10^10 slots) rejects the plan although only 10^4 groups exist.  The
 * ranges of sparse integer keys multiply out the same way.  When the raw
 * product overflows, one parallel pass marks the codes each candidate key
 * actually uses; if the product of the used-code counts fits, the plan maps
 * each key through a code→component table (agg_dense_component) and emits
 * keys through its inverse (agg_dense_code).  Only keys whose remap table is
 * bounded (AGG_COMPACT_MAX_RANGE codes) are compacted. */
enum { AGG_COMPACT_MAX_RANGE = 1 << 22,   /* largest per-key remap table (codes) */
       AGG_COMPACT_TRY_SLOTS = 1 << 16 };  /* raw products above this try compaction */

typedef struct {
    ray_t** key_cols;
    const dense_plan_t* dp;
    const bool* candidate;
    uint32_t n_keys;
    int64_t rows;
    uint32_t tasks;
    uint64_t** task_bits;    /* [tasks][n_keys] private bitmaps, NULL when not a candidate */
} agg_compact_ctx_t;

static void agg_compact_mark_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_compact_ctx_t* c = raw;
    for (int64_t task = start; task < end; task++) {
        int64_t begin = c->rows / c->tasks * task;
        int64_t limit = task + 1 == c->tasks ? c->rows : c->rows / c->tasks * (task + 1);
        for (uint32_t k = 0; k < c->n_keys; k++) {
            if (!c->candidate[k]) continue;
            uint64_t* bits = c->task_bits[(size_t)task * c->n_keys + k];
            ray_t* kc = c->key_cols[k];
            const void* d = ray_data(kc);
            int64_t mn = c->dp->mins[k];
            bool nullable = c->dp->nullable[k];
            int64_t null = c->dp->nulls[k];
            for (int64_t r = begin; r < limit; r++) {
                int64_t v = agg_read_key_i64(kc, d, r);
                if (nullable && v == null) continue;
                uint64_t code = (uint64_t)(v - mn);
                bits[code >> 6] |= UINT64_C(1) << (code & 63);
            }
        }
    }
}

static bool agg_dense_plan_compact(ray_t** key_cols, uint32_t n_keys, int64_t nrows,
                                   int64_t dense_limit, dense_plan_t* out) {
    bool candidate[16];
    bool any = false;
    for (uint32_t k = 0; k < n_keys; k++) {
        int64_t raw = out->ranges[k] - out->nullable[k];
        candidate[k] = raw > 1 && raw <= AGG_COMPACT_MAX_RANGE;
        any |= candidate[k];
    }
    if (!any) return false;
    ray_pool_t* pool = ray_pool_get();
    uint32_t tasks = pool && nrows >= RAY_PARALLEL_THRESHOLD ? ray_pool_total_workers(pool) : 1;
    if ((int64_t)tasks > nrows) tasks = (uint32_t)nrows;
    size_t words_total = 0;
    for (uint32_t k = 0; k < n_keys; k++)
        if (candidate[k]) words_total += (size_t)((out->ranges[k] - out->nullable[k] + 63) / 64);
    uint64_t* bits = ray_calloc_raw(words_total * tasks * sizeof(uint64_t));
    uint64_t** task_bits = ray_calloc_raw((size_t)tasks * n_keys * sizeof(uint64_t*));
    if (!bits || !task_bits) { ray_free_raw(bits); ray_free_raw(task_bits); return false; }
    {
        uint64_t* cursor = bits;
        for (uint32_t t = 0; t < tasks; t++)
            for (uint32_t k = 0; k < n_keys; k++) {
                if (!candidate[k]) continue;
                task_bits[(size_t)t * n_keys + k] = cursor;
                cursor += (out->ranges[k] - out->nullable[k] + 63) / 64;
            }
    }
    agg_compact_ctx_t c = { .key_cols = key_cols, .dp = out, .candidate = candidate,
        .n_keys = n_keys, .rows = nrows, .tasks = tasks, .task_bits = task_bits };
    if (tasks > 1) ray_pool_dispatch_n(pool, agg_compact_mark_fn, &c, tasks);
    else agg_compact_mark_fn(&c, 0, 0, 1);
    /* Reduce task bitmaps into task 0, count used codes, re-check the product. */
    int64_t used[16];
    int64_t total = 1;
    bool fits = true;
    for (uint32_t k = 0; k < n_keys; k++) {
        int64_t rng = out->ranges[k];
        if (candidate[k]) {
            size_t words = (size_t)((rng - out->nullable[k] + 63) / 64);
            uint64_t* acc = task_bits[k];
            for (uint32_t t = 1; t < tasks; t++) {
                const uint64_t* tb = task_bits[(size_t)t * n_keys + k];
                for (size_t w = 0; w < words; w++) acc[w] |= tb[w];
            }
            int64_t n = 0;
            for (size_t w = 0; w < words; w++) n += __builtin_popcountll(acc[w]);
            used[k] = n;
            rng = n + out->nullable[k];
            if (rng <= 0) rng = 1;
        } else used[k] = rng;
        if (fits && total > dense_limit / rng) fits = false;
        if (fits) total *= rng;
    }
    if (!fits) { ray_free_raw(bits); ray_free_raw(task_bits); return false; }
    /* Build remap/inverse for every candidate key that actually shrank.  The
     * raw ranges are restored on any allocation failure so a plan that is
     * still accepted (the non-overflow entry) never pairs a shrunken range
     * with the raw slot computation. */
    int64_t raw_ranges[16];
    memcpy(raw_ranges, out->ranges, sizeof(raw_ranges));
    for (uint32_t k = 0; k < n_keys; k++) {
        if (!candidate[k]) continue;
        int64_t raw = raw_ranges[k] - out->nullable[k];
        if (used[k] >= raw) continue;    /* every code used: raw range is already dense */
        int32_t* remap = ray_alloc_raw((size_t)raw * sizeof(int32_t));
        int64_t* inverse = ray_alloc_raw((size_t)(used[k] > 0 ? used[k] : 1) * sizeof(int64_t));
        if (!remap || !inverse) {
            ray_free_raw(remap); ray_free_raw(inverse);
            ray_free_raw(bits); ray_free_raw(task_bits);
            agg_dense_plan_free(out);
            memcpy(out->ranges, raw_ranges, sizeof(raw_ranges));
            return false;
        }
        const uint64_t* acc = task_bits[k];
        int64_t next = 0;
        for (int64_t code = 0; code < raw; code++) {
            bool on = (acc[code >> 6] >> (code & 63)) & 1;
            remap[code] = on ? (int32_t)next : -1;
            if (on) inverse[next++] = out->mins[k] + code;
        }
        out->remap[k] = remap;
        out->inverse[k] = inverse;
        out->compacted = true;
        out->ranges[k] = used[k] + out->nullable[k];
        if (out->ranges[k] <= 0) out->ranges[k] = 1;
    }
    ray_free_raw(bits); ray_free_raw(task_bits);
    return true;
}

bool agg_dense_plan(ray_t** key_cols, uint32_t n_keys,
                    const agg_vtable_t** vts, uint32_t n_aggs,
                    int64_t nrows, dense_plan_t* out) {
    (void)vts; (void)n_aggs;   /* agg kind no longer gates dense eligibility */
    out->ok = false;
    out->n_keys = n_keys;
    out->compacted = false;
    memset(out->remap, 0, sizeof(out->remap));
    memset(out->inverse, 0, sizeof(out->inverse));
    /* Dense direct-index routing self-limits to <=16 keys: dense_plan_t's
     * mins/ranges/strides are fixed [16] (the direct-index packing bound).
     * n_keys is uint32_t (untruncated ext->n_keys), so a wider shape is
     * rejected HERE and takes v2's unbounded hash/radix path — never reads
     * those [16] arrays with a truncated count. */
    if (n_keys < 1 || n_keys > 16) return false;

    /* Per-key type check + min/max prescan. */
    for (uint32_t k = 0; k < n_keys; k++) {
        ray_t* kc = key_cols[k];
        switch (kc->type) {
            case RAY_I64: case RAY_I32: case RAY_I16: case RAY_U8:
            case RAY_BOOL: case RAY_DATE: case RAY_TIME:
            case RAY_TIMESTAMP: case RAY_SYM: break;
            default: return false;
        }
        out->nullable[k] = kc->type != RAY_SYM && ray_vec_may_have_nulls(kc);
        out->nulls[k] = agg_key_null(kc->type);
        if (nrows <= 0) return false;   /* empty → no min/max, max<min guard */

        /* Narrow SYM codes are bounded by their representation width. Use that
         * bound only when it is no larger than the input; otherwise prescan the
         * actual used range so dense state remains O(input). */
        if (kc->type == RAY_SYM) {
            switch (kc->attrs & RAY_SYM_W_MASK) {
                case RAY_SYM_W8:
                    if (nrows >= (int64_t)UINT8_MAX + 1) {
                        out->mins[k] = 0; out->ranges[k] = (int64_t)UINT8_MAX + 1; continue;
                    }
                    break;
                case RAY_SYM_W16:
                    if (nrows >= (int64_t)UINT16_MAX + 1) {
                        out->mins[k] = 0; out->ranges[k] = (int64_t)UINT16_MAX + 1; continue;
                    }
                    break;
                default: break;  /* W32/W64: try domain bounds, else prescan */
            }
            /* W32/W64 SYM codes are positions in [0, domain_count).  When the
             * domain is small enough to stay within the dense array budget (and
             * no larger than the row count), that range is known a priori — skip
             * the O(nrows) min/max prescan, using min=0 / range=count.  Larger
             * domains still scan: their actual used range may be far tighter
             * than the full count, and the array would otherwise blow the cap. */
            int64_t dc = ray_sym_domain_count(ray_sym_vec_domain(kc));
            if (dc > 0 && dc <= nrows) {
                out->mins[k] = 0; out->ranges[k] = dc; continue;
            }
        }

        int64_t mn, mx;
        agg_key_bounds_parallel(kc, nrows, out->nullable[k], out->nulls[k], &mn, &mx);
        if (!agg_dense_range(out, k, mn, mx)) return false;
    }

    /* Composite packing; keep dense state O(input). */
    int64_t total = 1;
    int64_t dense_limit = nrows < (int64_t)UINT32_MAX
        ? nrows : (int64_t)UINT32_MAX;
    bool overflow = false;
    for (uint32_t k = 0; k < n_keys && !overflow; k++) {
        int64_t rng = out->ranges[k];
        if (rng <= 0) return false;
        if (total > dense_limit / rng) overflow = true;
        else total *= rng;
    }
    if (overflow || (n_keys >= 2 && total > AGG_COMPACT_TRY_SLOTS)) {
        /* Raw ranges multiply out too far — or far enough past a
         * cache-resident slab that sparse keys would waste it (a W16 SYM key
         * reports its whole 65,536-code width): compact the keys to the codes
         * they use and retry the packing over the compacted ranges.  Keys
         * whose every code is used keep their raw range. */
        bool compacted_ok = n_keys >= 2 &&
            agg_dense_plan_compact(key_cols, n_keys, nrows, dense_limit, out);
        if (overflow && !compacted_ok) return false;
        total = 1;
        for (uint32_t k = 0; k < n_keys; k++) {
            int64_t rng = out->ranges[k];
            if (rng <= 0 || total > dense_limit / rng) { agg_dense_plan_free(out); return false; }
            total *= rng;
        }
    }
    total = 1;
    for (uint32_t k = 0; k < n_keys; k++) {
        out->strides[k] = total;
        total *= out->ranges[k];
    }

    out->total_slots = total;
    out->ok = true;
    return true;
}

/* Per-op result-column-name suffix, mirroring emit_agg_columns (group.c).
 * Returns "" / 0 for ops without a suffix. */
static const char* agg_name_suffix(uint16_t agg_op, size_t* slen_out) {
    const char* sfx = ""; size_t slen = 0;
    switch (agg_op) {
        case OP_SUM:   sfx = "_sum";   slen = 4; break;
        case OP_PROD:  sfx = "_prod";  slen = 5; break;
        case OP_ALL:   sfx = "_all";   slen = 4; break;
        case OP_ANY:   sfx = "_any";   slen = 4; break;
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
        case OP_MEDIAN:     sfx = "_median";     slen = 7; break;
        case OP_QUANTILE:   sfx = "_quantile";   slen = 9; break;
        case OP_MODE:       sfx = "_mode";       slen = 5; break;
        case OP_COV:        sfx = "_cov";        slen = 4; break;
        case OP_SCOV:       sfx = "_scov";       slen = 5; break;
        case OP_WSUM:       sfx = "_wsum";       slen = 5; break;
        case OP_WAVG:       sfx = "_wavg";       slen = 5; break;
        case OP_TOP_N:      sfx = "_top";        slen = 4; break;
        case OP_BOT_N:      sfx = "_bot";        slen = 4; break;
        default: break;
    }
    *slen_out = slen;
    return sfx;
}

/* Result column name for a plain-column-input aggregate: the input column's
 * name (for in_sym) plus the per-op suffix, interned.  On overflow, falls back
 * to the input sym itself — byte-identical to emit_agg_columns' inline copy. */
int64_t agg_result_col_name(int64_t in_sym, uint16_t agg_op) {
    ray_t* name_atom = ray_sym_str(in_sym);
    const char* base = name_atom ? ray_str_ptr(name_atom) : NULL;
    size_t blen = base ? ray_str_len(name_atom) : 0;
    size_t slen = 0;
    const char* sfx = agg_name_suffix(agg_op, &slen);
    char buf[256];
    if (base && blen + slen < sizeof(buf)) {
        memcpy(buf, base, blen);
        memcpy(buf + blen, sfx, slen);
        return ray_sym_intern(buf, blen + slen);
    }
    return in_sym;
}

typedef struct {
    const char* source;
    char* output;
    const int64_t* rows;
    size_t width;
    char null_value[16];
    _Atomic(bool) any_null;
} agg_gather_native_t;

static void agg_gather_native_run(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; agg_gather_native_t* c = raw;
    bool any_null = false;
    /* Constant byte widths let the compiler emit direct loads/stores. Workers
     * own disjoint payload slots and never mutate the output object header. */
    #define GATHER_NATIVE(WIDTH) \
        for (int64_t i = start; i < end; i++) { \
            int64_t row = c->rows[i]; \
            if (row < 0) { \
                memcpy(c->output + (size_t)i * (WIDTH), c->null_value, (WIDTH)); \
                any_null = true; \
            } else memcpy(c->output + (size_t)i * (WIDTH), \
                          c->source + (size_t)row * (WIDTH), (WIDTH)); \
        }
    assert(c->width > 0 && c->width <= sizeof(c->null_value));
    switch (c->width) {
        case 1: GATHER_NATIVE(1); break;
        case 2: GATHER_NATIVE(2); break;
        case 4: GATHER_NATIVE(4); break;
        case 8: GATHER_NATIVE(8); break;
        case 16: GATHER_NATIVE(16); break;
        default: GATHER_NATIVE(c->width); break;
    }
    #undef GATHER_NATIVE
    if (any_null) atomic_store_explicit(&c->any_null, true, memory_order_relaxed);
}

/* Build a result key column of src_col's type by gathering the first-row cell
 * of each group at native (type-exact) byte width.  For SYM, adopts the source
 * domain so the intern ids resolve correctly.  Caller owns the returned column. */
ray_t* ray_group_gather(ray_t* src_col, const int64_t* first_row, int64_t n) {
    if (src_col->type == RAY_LIST) {
        ray_t* out = ray_list_new(n);
        if (!out || RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* value = first_row[i] < 0 ? NULL : ray_list_get(src_col, first_row[i]);
            out = ray_list_append(out, value);
        }
        return out;
    }
    if (src_col->type == RAY_STR) {
        const char** ptrs = ray_alloc_raw((size_t)(n ? n : 1) * sizeof(char*));
        uint32_t* lens = ray_alloc_raw((size_t)(n ? n : 1) * sizeof(uint32_t));
        if (!ptrs || !lens) { ray_free_raw(ptrs); ray_free_raw(lens); return ray_error("oom", NULL); }
        for (int64_t i = 0; i < n; i++) {
            size_t len = 0;
            ptrs[i] = first_row[i] < 0 ? "" : ray_str_vec_get(src_col, first_row[i], &len);
            lens[i] = (uint32_t)len;
        }
        ray_t* out = ray_str_vec_from_parts(ptrs, lens, NULL, n);
        ray_free_raw(ptrs); ray_free_raw(lens);
        return out;
    }
    ray_t* out = col_vec_new(src_col, n);
    if (!out || RAY_IS_ERR(out)) return out;
    if (out->type == RAY_SYM)
        ray_sym_vec_adopt_domain(out, sym_domain_rep(src_col));
    out->len = n;
    agg_gather_native_t c = {.source = ray_data(src_col), .output = ray_data(out),
        .rows = first_row, .width = col_esz(src_col)};
    #define GATHER_NULL(TYPE, VALUE) do { TYPE value = (VALUE); memcpy(c.null_value, &value, sizeof(value)); } while (0)
    switch (src_col->type) {
        case RAY_F64: GATHER_NULL(double, NULL_F64); break;
        case RAY_F32: GATHER_NULL(float, NULL_F32); break;
        case RAY_I64: case RAY_TIMESTAMP: GATHER_NULL(int64_t, NULL_I64); break;
        case RAY_I32: case RAY_DATE: case RAY_TIME: GATHER_NULL(int32_t, NULL_I32); break;
        case RAY_I16: GATHER_NULL(int16_t, NULL_I16); break;
        default: break; /* SYM/GUID use zero payloads; BOOL/U8 have no null. */
    }
    #undef GATHER_NULL
    ray_pool_t* pool = ray_pool_get();
    if (ray_pool_par_dispatch_ok(pool, n, RAY_PARALLEL_THRESHOLD))
        ray_pool_dispatch(pool, agg_gather_native_run, &c, n);
    else agg_gather_native_run(&c, 0, 0, n);
    if (agg_cancelled()) { ray_release(out); return ray_error("cancel", NULL); }
    if (ray_vec_may_have_nulls(src_col) ||
            (atomic_load_explicit(&c.any_null, memory_order_relaxed) &&
             src_col->type != RAY_BOOL && src_col->type != RAY_U8))
        out->attrs |= RAY_ATTR_HAS_NULLS;
    return out;
}

/* ══════════════════════════════════════════════════════════════════════
 * CHUNKED SELECTION CONSUMPTION (selection-vector model)
 *
 * When a pushed WHERE filter is active, g->selection is a rowsel bitmap (see
 * src/ops/rowsel.h: per-segment NONE/ALL/MIX flags + morsel-local idx[] for MIX
 * segments).  Rather than materialize a full O(rows-passed) index array
 * (ray_rowsel_to_indices) + a full compact column (the old prologue), the
 * chunked strategies (task-local dense, radix, smallhash) consume the
 * selection IN PLACE: each worker walks its assigned SELECTED rows in fixed-size
 * chunks (AGG_SEL_CHUNK), decoding each chunk's ORIGINAL row indices into a
 * small reused stack buffer, gathering only that chunk's key/agg-input values
 * into small reused contiguous buffers, then feeding the existing dense-batch
 * kernel (update_batch) over the chunk.  No full index array, no full compact
 * column, no per-call large alloc.  This is the standard chunked add-chunk
 * approach (fixed vector size 2048): gather the chunk's selected rows into
 * reused vectors and sink the chunk — adapted to v2's dense-contiguous batch
 * kernels (which take a vector, not a vector+selection pair) by gathering per
 * chunk instead of slicing references.
 *
 * Representative-row indices (first_row) stay in ORIGINAL-row space: the decoded
 * row index is what gets recorded, so the result key columns gather correctly
 * (SYM domains preserved) and the unordered output contract is unchanged.
 *
 * Parallel partitioning: workers are dispatched over the SELECTED-row space
 * [0, n_sel).  A per-segment prefix sum of selected counts (seg_sel_prefix,
 * built once from seg_flags/seg_offsets — O(n_segs), cheap) lets each worker's
 * [sel_start, sel_end) range be mapped back to a starting segment + intra-
 * segment offset.  This gives roughly-equal selected counts per worker AND
 * fine-grained work-stealing balance (NONE segments cost nothing).
 * ══════════════════════════════════════════════════════════════════════ */

#include "core/pool.h"

#define AGG_SEL_CHUNK 2048   /* fixed selection-vector chunk size */

/* Build a prefix sum of per-segment selected counts: prefix[s] = number of
 * selected rows in segments [0, s).  prefix[n_segs] == total_pass.  Lets a
 * worker map a [sel_start, sel_end) range in selected-row space to the segment
 * where it begins.  Returned via a ray_alloc'd int64 block (caller releases). */
static ray_t* agg_sel_build_prefix(ray_t* sel) {
    ray_rowsel_t* m = ray_rowsel_meta(sel);
    uint32_t n_segs = m->n_segs;
    const uint8_t*  flags   = ray_rowsel_flags(sel);
    const uint32_t* offsets = ray_rowsel_offsets(sel);
    int64_t nrows = m->nrows;
    ray_t* block = ray_alloc((size_t)(n_segs + 1) * sizeof(int64_t));
    if (!block) return NULL;
    int64_t* prefix = (int64_t*)ray_data(block);
    int64_t cum = 0;
    for (uint32_t s = 0; s < n_segs; s++) {
        prefix[s] = cum;
        uint8_t f = flags[s];
        if (f == RAY_SEL_NONE) continue;
        if (f == RAY_SEL_ALL) {
            int64_t base = (int64_t)s * RAY_MORSEL_ELEMS;
            int64_t end  = base + RAY_MORSEL_ELEMS;
            if (end > nrows) end = nrows;
            cum += end - base;
        } else { /* MIX */
            cum += offsets[s + 1] - offsets[s];
        }
    }
    prefix[n_segs] = cum;
    return block;
}

/* Cursor over the selected rows in a contiguous slice [sel_start, sel_end) of
 * selected-row space.  Decodes the next chunk of up to AGG_SEL_CHUNK ORIGINAL
 * row indices into `rows`, returns the count (0 when exhausted). */
typedef struct {
    ray_t*          sel;
    const uint8_t*  flags;
    const uint32_t* offsets;
    const uint16_t* idx;
    const int64_t*  prefix;     /* per-segment selected-count prefix */
    int64_t         nrows;
    uint32_t        n_segs;
    /* iteration state */
    int64_t         remaining;  /* selected rows left in this worker's range */
    uint32_t        seg;        /* current segment */
    int64_t         seg_pos;    /* next intra-segment selected index to emit */
} agg_sel_cursor_t;

/* Position the cursor at the seg/offset where selected-row `sel_start` lives. */
static void agg_sel_cursor_init(agg_sel_cursor_t* c, ray_t* sel,
                                const int64_t* prefix,
                                int64_t sel_start, int64_t sel_end) {
    ray_rowsel_t* m = ray_rowsel_meta(sel);
    c->sel = sel;
    c->flags   = ray_rowsel_flags(sel);
    c->offsets = ray_rowsel_offsets(sel);
    c->idx     = ray_rowsel_idx(sel);
    c->prefix  = prefix;
    c->nrows   = m->nrows;
    c->n_segs  = m->n_segs;
    c->remaining = sel_end - sel_start;
    if (c->remaining <= 0) { c->seg = c->n_segs; c->seg_pos = 0; return; }
    /* Find the segment containing the sel_start'th selected row: largest s with
     * prefix[s] <= sel_start.  Linear-then-skip is fine (n_segs small), but the
     * prefix is monotonic so a binary search keeps init O(log n_segs). */
    uint32_t lo = 0, hi = c->n_segs;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (prefix[mid + 1] <= sel_start) lo = mid + 1;
        else hi = mid;
    }
    c->seg = lo;
    c->seg_pos = sel_start - prefix[lo];   /* intra-segment selected offset */
}

/* Decode the next chunk of original row indices into rows[] (cap AGG_SEL_CHUNK);
 * returns the number written (0 = done). */
static int64_t agg_sel_cursor_next(agg_sel_cursor_t* c, int64_t* rows) {
    int64_t n = 0;
    while (c->remaining > 0 && n < AGG_SEL_CHUNK && c->seg < c->n_segs) {
        uint32_t s = c->seg;
        uint8_t f = c->flags[s];
        if (f == RAY_SEL_NONE) { c->seg++; c->seg_pos = 0; continue; }
        int64_t base = (int64_t)s * RAY_MORSEL_ELEMS;
        int64_t seg_end = base + RAY_MORSEL_ELEMS;
        if (seg_end > c->nrows) seg_end = c->nrows;
        if (f == RAY_SEL_ALL) {
            int64_t seg_sel = seg_end - base;          /* every row selected */
            while (c->seg_pos < seg_sel && n < AGG_SEL_CHUNK && c->remaining > 0) {
                rows[n++] = base + c->seg_pos;
                c->seg_pos++; c->remaining--;
            }
            if (c->seg_pos >= seg_sel) { c->seg++; c->seg_pos = 0; }
        } else { /* MIX */
            const uint16_t* slice = c->idx + c->offsets[s];
            int64_t seg_sel = c->offsets[s + 1] - c->offsets[s];
            while (c->seg_pos < seg_sel && n < AGG_SEL_CHUNK && c->remaining > 0) {
                rows[n++] = base + slice[c->seg_pos];
                c->seg_pos++; c->remaining--;
            }
            if (c->seg_pos >= seg_sel) { c->seg++; c->seg_pos = 0; }
        }
    }
    return n;
}

/* Gather one chunk's value column (native esz) into a small dense buffer.
 * COUNT (val_data == NULL) needs no gather — returns without touching dst. */
static inline void agg_sel_gather_vals(void* dst, const void* src, uint8_t esz,
                                       const int64_t* rows, int64_t n) {
    if (!src || esz == 0) return;
    const char* s = (const char*)src;
    switch (esz) {
        case 8: {
            int64_t* dd = (int64_t*)dst; const int64_t* ss = (const int64_t*)s;
            for (int64_t i = 0; i < n; i++) dd[i] = ss[rows[i]];
            break;
        }
        case 4: {
            int32_t* dd = (int32_t*)dst; const int32_t* ss = (const int32_t*)s;
            for (int64_t i = 0; i < n; i++) dd[i] = ss[rows[i]];
            break;
        }
        case 2: {
            int16_t* dd = (int16_t*)dst; const int16_t* ss = (const int16_t*)s;
            for (int64_t i = 0; i < n; i++) dd[i] = ss[rows[i]];
            break;
        }
        case 1: {
            uint8_t* dd = (uint8_t*)dst; const uint8_t* ss = (const uint8_t*)s;
            for (int64_t i = 0; i < n; i++) dd[i] = ss[rows[i]];
            break;
        }
        default: {  /* general gather, correct for any element width */
            char* dd = (char*)dst;
            for (int64_t i = 0; i < n; i++) memcpy(dd + i * esz, s + rows[i] * esz, esz);
            break;
        }
    }
}

/* Per-dispatch descriptor tables, sized from ext->n_keys / ext->n_aggs.
 * One carve; 8-byte arrays first, then narrower.  Replaces the fixed
 * [16] blocks that used to cap the engine at 16 keys/aggs. */
typedef struct {
    ray_t*        hdr;
    const void**  key_data;
    const void**  val_data;  const void** val2_data;
    int64_t*      agg_syms;
    int8_t*       val_types; int8_t*  val2_types;
    bool*         val_hasnull; bool*  val2_hasnull;
    uint8_t*      val_esz;   uint8_t* val2_esz;
} agg_desc_t;

static bool agg_desc_init(agg_desc_t* d, ray_graph_t* g, ray_op_ext_t* ext,
                          ray_t* tbl, ray_t** key_cols) {
    uint32_t nk = ext->n_keys, na = ext->n_aggs;
    size_t n8 = (size_t)nk + 3u * na;              /* key_data,val_data,val2_data,agg_syms */
    size_t n1 = 6u * na;                           /* types/hasnull/esz pairs */
    d->key_data = (const void**)scratch_alloc(&d->hdr, n8 * 8 + n1);
    if (!d->key_data) return false;
    d->val_data   = d->key_data + nk;
    d->val2_data  = d->val_data + na;
    d->agg_syms   = (int64_t*)(d->val2_data + na);
    d->val_types  = (int8_t*)(d->agg_syms + na);
    d->val2_types = d->val_types + na;
    d->val_hasnull  = (bool*)(d->val2_types + na);
    d->val2_hasnull = d->val_hasnull + na;
    d->val_esz  = (uint8_t*)(d->val2_hasnull + na);
    d->val2_esz = d->val_esz + na;
    for (uint32_t k = 0; k < nk; k++) d->key_data[k] = ray_data(key_cols[k]);
    for (uint32_t a = 0; a < na; a++) {
        ray_op_ext_t* ie = find_ext(g, ext->agg_ins[a]);
        d->agg_syms[a] = ie->sym;
        ray_t* vc = (ext->agg_ops[a] != OP_COUNT) ? ray_table_get_col(tbl, ie->sym) : NULL;
        d->val_data[a]    = vc ? ray_data(vc) : NULL;
        d->val_types[a]   = vc ? vc->type : RAY_I64;
        d->val_hasnull[a] = vc ? ray_vec_may_have_nulls(vc) : false;
        d->val_esz[a]     = vc ? col_esz(vc) : 0;
        /* Binary agg (pearson): resolve the y-side column from agg_ins2[a]. */
        ray_t* vc2 = (ext->agg_ins2 && ext->agg_ins2[a] != RAY_OP_NONE)
            ? ray_table_get_col(tbl, find_ext(g, ext->agg_ins2[a])->sym) : NULL;
        d->val2_data[a]    = vc2 ? ray_data(vc2) : NULL;
        d->val2_types[a]   = vc2 ? vc2->type : RAY_I64;
        d->val2_hasnull[a] = vc2 ? ray_vec_may_have_nulls(vc2) : false;
        d->val2_esz[a]     = vc2 ? col_esz(vc2) : 0;
    }
    return true;
}

static void agg_desc_free(agg_desc_t* d) { scratch_free(d->hdr); d->hdr = NULL; }

/* Per-dispatch AoS layout tables (vtable ptr + state offset per agg), carved
 * from one buffer sized na*(sizeof(void*)+sizeof(size_t)).  Replaces the fixed
 * [16] vts/off blocks. */
typedef struct {
    ray_t*               hdr;
    const agg_vtable_t** vts;
    size_t*              off;
    size_t               block;
} agg_vo_t;

static bool agg_vo_init(agg_vo_t* vo, ray_graph_t* g, ray_op_ext_t* ext, ray_t* tbl) {
    uint32_t na = ext->n_aggs;
    vo->vts = (const agg_vtable_t**)scratch_alloc(&vo->hdr,
                (size_t)na * (sizeof(void*) + sizeof(size_t)));
    if (!vo->vts) return false;
    vo->off = (size_t*)(vo->vts + na);
    vo->block = 0;
    for (uint32_t a = 0; a < na; a++) {
        ray_op_ext_t* ie = find_ext(g, ext->agg_ins[a]);
        ray_t* vc = (ext->agg_ops[a] != OP_COUNT) ? ray_table_get_col(tbl, ie->sym) : NULL;
        int8_t in_type = vc ? vc->type : RAY_I64;
        vo->vts[a] = agg_resolve(ext->agg_ops[a], in_type);
        vo->off[a] = vo->block;
        vo->block += vo->vts[a]->state_size;
    }
    return true;
}

static void agg_vo_free(agg_vo_t* vo) { scratch_free(vo->hdr); vo->hdr = NULL; }

/* Per-agg value descriptors, shared by every strategy's parallel ctx.  Gathered
 * into a struct so the sel-mode chunk accumulator can be written once. */
typedef struct {
    uint32_t            n_aggs;
    const agg_vtable_t** vts;
    const size_t*       off;
    size_t              block;
    const void**        val_data;  const int8_t* val_types;  const bool* val_hasnull;  const uint8_t* val_esz;
    const void**        val2_data; const int8_t* val2_types; const bool* val2_hasnull; const uint8_t* val2_esz;
} agg_valdesc_t;

/* Reused per-worker scratch for sel-mode chunk gathering: one dense value buffer
 * per agg (sized AGG_SEL_CHUNK * esz), plus the y-side for binary aggs.  Lazily
 * allocated the first time a worker touches a non-COUNT agg. */
typedef struct {
    char** gv;      /* [n_aggs], carved */
    char** gy;
    ray_t* hdr;
    uint32_t n_aggs;
    int   oom;
} agg_sel_scratch_t;

/* Carve 2*n_aggs zeroed value-buffer pointers (gv[n_aggs] followed by gy[n_aggs]).
 * Returns false on OOM (leaves sc safe for agg_sel_scratch_free). */
static bool agg_sel_scratch_init(agg_sel_scratch_t* sc, uint32_t n_aggs) {
    sc->hdr = NULL; sc->gv = NULL; sc->gy = NULL; sc->n_aggs = 0; sc->oom = 0;
    sc->gv = (char**)scratch_calloc(&sc->hdr, (size_t)2u * n_aggs * sizeof(char*));
    if (!sc->gv) return false;
    sc->gy = sc->gv + n_aggs;
    sc->n_aggs = n_aggs;
    return true;
}

static void agg_sel_scratch_free(agg_sel_scratch_t* sc) {
    for (uint32_t a = 0; a < sc->n_aggs; a++) { ray_free_raw(sc->gv[a]); ray_free_raw(sc->gy[a]); }
    scratch_free(sc->hdr); sc->hdr = NULL; sc->gv = NULL; sc->gy = NULL; sc->n_aggs = 0;
}

/* Accumulate one decoded chunk (rows[], gid[] indexed [0,n)) into `states` for
 * every aggregate, gathering each agg's input values for this chunk's ORIGINAL
 * rows into the reused scratch buffers first.  `states` points at agg 0's slot 0
 * (i.e. states_base + off applied per agg by the kernel via off[a]).  Returns 0,
 * or -1 on scratch OOM (sets sc->oom). */
static int agg_sel_accum_chunk(const agg_valdesc_t* vd, agg_sel_scratch_t* sc,
                               char* states, const uint32_t* gid,
                               const int64_t* rows, int64_t n) {
    for (uint32_t a = 0; a < vd->n_aggs; a++) {
        const agg_vtable_t* vt = vd->vts[a];
        if (vt->update_batch2) {                            /* binary agg (pearson) */
            uint8_t ez = vd->val_esz[a], ez2 = vd->val2_esz[a];
            if (!sc->gv[a]) { sc->gv[a] = ray_alloc_raw((size_t)AGG_SEL_CHUNK * (ez ? ez : 1)); if (!sc->gv[a]) { sc->oom = 1; return -1; } }
            if (!sc->gy[a]) { sc->gy[a] = ray_alloc_raw((size_t)AGG_SEL_CHUNK * (ez2 ? ez2 : 1)); if (!sc->gy[a]) { sc->oom = 1; return -1; } }
            agg_sel_gather_vals(sc->gv[a], vd->val_data[a],  ez,  rows, n);
            agg_sel_gather_vals(sc->gy[a], vd->val2_data[a], ez2, rows, n);
            ray_valid_t vx = { sc->gv[a], vd->val_types[a],  vd->val_hasnull[a] };
            ray_valid_t vy = { sc->gy[a], vd->val2_types[a], vd->val2_hasnull[a] };
            vt->update_batch2(states + vd->off[a], vd->block, gid,
                              sc->gv[a], sc->gy[a], &vx, &vy, n, NULL);
        } else if (vd->val_data[a]) {                       /* unary agg over a column */
            uint8_t ez = vd->val_esz[a];
            if (!sc->gv[a]) { sc->gv[a] = ray_alloc_raw((size_t)AGG_SEL_CHUNK * (ez ? ez : 1)); if (!sc->gv[a]) { sc->oom = 1; return -1; } }
            agg_sel_gather_vals(sc->gv[a], vd->val_data[a], ez, rows, n);
            ray_valid_t valid = { sc->gv[a], vd->val_types[a], vd->val_hasnull[a] };
            vt->update_batch(states + vd->off[a], vd->block, gid, sc->gv[a], &valid, n, NULL);
        } else {                                            /* COUNT (no value gather) */
            ray_valid_t valid = { NULL, vd->val_types[a], false };
            vt->update_batch(states + vd->off[a], vd->block, gid, NULL, &valid, n, NULL);
        }
    }
    return 0;
}

/* Selection-aware dense plan: identical to agg_dense_plan but the per-key
 * min/max prescan walks only the SELECTED rows (via the cursor) instead of the
 * full table.  In sel mode the grouped keys are exactly the selected rows, so
 * their min/max defines the slot range — and the prescan cost is O(n_sel), not
 * O(nrows).  This is what lets a high-selectivity filter (e.g. 100 distinct SYM
 * keys among 100k of 10M rows) reach the dense direct-index path cheaply,
 * instead of scanning all 10M rows just to discover the range. */
/* n_keys is uint32_t (untruncated ext->n_keys): the >16 shapes are rejected by
 * the same dense direct-index bound as agg_dense_plan and take v2's unbounded
 * hash/radix path; the fixed [16] mins/ranges/strides are only read at <=16. */
typedef struct {
    ray_t** key_cols;
    const void** key_data;
    const bool* nullable;
    const int64_t* nulls;
    uint32_t n_keys;
    ray_t* sel;
    const int64_t* sel_prefix;
    int64_t n_sel;
    uint32_t tasks;
    int64_t* task_bounds;   /* [tasks][2][n_keys]: min row, max row per key */
} agg_sel_bounds_ctx_t;

/* One task's min/max over its slice of selected-row space. */
static void agg_sel_bounds_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_sel_bounds_ctx_t* c = raw;
    int64_t rows[AGG_SEL_CHUNK];
    for (int64_t task = start; task < end; task++) {
        int64_t* mins = c->task_bounds + (2 * (size_t)task) * c->n_keys;
        int64_t* maxs = mins + c->n_keys;
        for (uint32_t k = 0; k < c->n_keys; k++) { mins[k] = INT64_MAX; maxs[k] = INT64_MIN; }
        int64_t begin = c->n_sel / c->tasks * task;
        int64_t limit = task + 1 == c->tasks ? c->n_sel : c->n_sel / c->tasks * (task + 1);
        agg_sel_cursor_t cur;
        agg_sel_cursor_init(&cur, c->sel, c->sel_prefix, begin, limit);
        int64_t cn;
        while ((cn = agg_sel_cursor_next(&cur, rows)) > 0) {
            for (uint32_t k = 0; k < c->n_keys; k++) {
                ray_t* kc = c->key_cols[k]; const void* d = c->key_data[k];
                int64_t mn = mins[k], mx = maxs[k];
                for (int64_t i = 0; i < cn; i++) {
                    int64_t v = agg_read_key_i64(kc, d, rows[i]);
                    if (c->nullable[k] && v == c->nulls[k]) continue;
                    if (v < mn) mn = v;
                    if (v > mx) mx = v;
                }
                mins[k] = mn; maxs[k] = mx;
            }
        }
    }
}

static bool agg_dense_plan_sel(ray_t** key_cols, uint32_t n_keys, int64_t n_sel,
                               ray_t* sel, const int64_t* sel_prefix,
                               dense_plan_t* out) {
    out->ok = false;
    out->n_keys = n_keys;
    out->compacted = false;
    memset(out->remap, 0, sizeof(out->remap));
    memset(out->inverse, 0, sizeof(out->inverse));
    if (n_keys < 1 || n_keys > 16) return false;
    if (n_sel <= 0) return false;

    for (uint32_t k = 0; k < n_keys; k++) {
        ray_t* kc = key_cols[k];
        switch (kc->type) {
            case RAY_I64: case RAY_I32: case RAY_I16: case RAY_U8:
            case RAY_BOOL: case RAY_DATE: case RAY_TIME:
            case RAY_TIMESTAMP: case RAY_SYM: break;
            default: return false;
        }
        out->nullable[k] = kc->type != RAY_SYM && ray_vec_may_have_nulls(kc);
        out->nulls[k] = agg_key_null(kc->type);
        out->mins[k] = INT64_MAX; out->ranges[k] = 0;   /* sentinels; filled below */
    }

    /* One pass over the selected rows updating every key's min/max together.
     * The pass is split across the pool: a serial walk of a 10M-row selection
     * cost ~13 ms on every filtered group-by regardless of core count. */
    ray_t* pre_hdr;
    void* pre = scratch_alloc(&pre_hdr, 3u * (size_t)n_keys * 8);   /* mins,maxs,key_data */
    if (!pre) return false;
    int64_t* mins = (int64_t*)pre;
    int64_t* maxs = mins + n_keys;
    const void** key_data = (const void**)(maxs + n_keys);
    for (uint32_t k = 0; k < n_keys; k++) { mins[k] = INT64_MAX; maxs[k] = INT64_MIN; }
    for (uint32_t k = 0; k < n_keys; k++) key_data[k] = ray_data(key_cols[k]);

    ray_pool_t* pool = ray_pool_get();
    uint32_t tasks = pool && n_sel >= RAY_PARALLEL_THRESHOLD ? ray_pool_total_workers(pool) * 4 : 1;
    if (tasks > RAY_POOL_INIT_TASKS) tasks = RAY_POOL_INIT_TASKS;
    if ((int64_t)tasks > n_sel) tasks = (uint32_t)n_sel;
    agg_sel_bounds_ctx_t bc = { .key_cols = key_cols, .key_data = key_data,
        .nullable = out->nullable, .nulls = out->nulls, .n_keys = n_keys,
        .sel = sel, .sel_prefix = sel_prefix, .n_sel = n_sel, .tasks = tasks };
    ray_t* tb_hdr;
    bc.task_bounds = (int64_t*)scratch_alloc(&tb_hdr, 2u * (size_t)tasks * n_keys * sizeof(int64_t));
    if (!bc.task_bounds) { scratch_free(pre_hdr); return false; }
    if (tasks > 1) ray_pool_dispatch_n(pool, agg_sel_bounds_fn, &bc, tasks);
    else agg_sel_bounds_fn(&bc, 0, 0, 1);
    for (uint32_t t = 0; t < tasks; t++)
        for (uint32_t k = 0; k < n_keys; k++) {
            int64_t mn = bc.task_bounds[(2 * (size_t)t) * n_keys + k];
            int64_t mx = bc.task_bounds[(2 * (size_t)t + 1) * n_keys + k];
            if (mn < mins[k]) mins[k] = mn;
            if (mx > maxs[k]) maxs[k] = mx;
        }
    scratch_free(tb_hdr);
    for (uint32_t k = 0; k < n_keys; k++) {
        if (!agg_dense_range(out, k, mins[k], maxs[k])) { scratch_free(pre_hdr); return false; }
    }
    scratch_free(pre_hdr);

    int64_t total = 1;
    int64_t dense_limit = n_sel < (int64_t)UINT32_MAX
        ? n_sel : (int64_t)UINT32_MAX;
    for (uint32_t k = 0; k < n_keys; k++) {
        int64_t rng = out->ranges[k];
        if (rng <= 0) return false;
        if (total > dense_limit / rng) return false;
        out->strides[k] = total;
        total *= rng;
    }
    out->total_slots = total;
    out->ok = true;
    return true;
}

/* ══════════════════════════════════════════
 * Integer tuple hashing for radix and small-hash grouping
 * ══════════════════════════════════════════ */

/* Tuple FNV-1a hash over all keys at row r — identical to agg_group_keys.
 * n_keys is uint32_t: unbounded key count (carries the untruncated ext->n_keys
 * through every caller). */
static inline uint64_t agg_tuple_hash(ray_t** key_cols, const void** key_data,
                                      uint32_t n_keys, int64_t r) {
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t k = 0; k < n_keys; k++) {
        int64_t v = agg_read_key_i64(key_cols[k], key_data[k], r);
        h ^= (uint64_t)v; h *= 1099511628211ULL;
    }
    /* Avalanche (murmur3 fmix64).  FNV-1a leaves the LOW bits weakly mixed, and
     * the hash table indexes with `h & htmask` (low bits).  Keys that share low
     * bits then collapse into a few slots — catastrophic for structured keys
     * like time-bucketed values (e.g. an xbar stride divisible by a high power
     * of two): every key lands in the same handful of buckets and linear
     * probing degrades to O(n²).  Finalize so every output bit depends on every
     * input bit; correctness is unaffected (the eq-check resolves collisions),
     * only the slot distribution. */
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

/* All keys at row ra equal all keys at row rb?  n_keys is uint32_t (unbounded),
 * same contract as agg_tuple_hash above. */
static inline int agg_tuple_eq(ray_t** key_cols, const void** key_data,
                               uint32_t n_keys, int64_t ra, int64_t rb) {
    for (uint32_t k = 0; k < n_keys; k++)
        if (agg_read_key_i64(key_cols[k], key_data[k], ra) !=
            agg_read_key_i64(key_cols[k], key_data[k], rb))
            return 0;
    return 1;
}

/* Parallel dense streaming aggregation. Each logical task owns a local key
 * occupancy bitmap, group states, and first-row indices. Only occupied
 * states are initialized or merged. This fallback covers small domains and
 * selected/composite keys; capability-based strategies below avoid replication. */

/* Per-task dense slab. */
typedef struct {
    char*    states;     /* [total_slots * block] AoS, only occupied slots initialized */
    int64_t* first_row;  /* initialized only for occupied slots */
    uint64_t* occupied; /* one bit per initialized local state */
    int      oom;
    bool     ready;
    bool     eager;
    int64_t slots;
} agg_dense_local_t;

typedef struct {
    ray_t**             key_cols;
    const void**        key_data;
    uint32_t            n_keys;
    const dense_plan_t* dp;
    const agg_vtable_t** vts;
    const size_t*       off;
    size_t              block;
    uint32_t            n_aggs;
    const void**        val_data; const int8_t* val_types; const bool* val_hasnull; const uint8_t* val_esz;
    const void**        val2_data; const int8_t* val2_types; const bool* val2_hasnull; const uint8_t* val2_esz;
    agg_dense_local_t*  locals;   /* [nw] */
    /* Sel-mode (pushed WHERE filter): when sel != NULL, workers chunk-iterate
     * the SELECTED rows of [0,n_sel) instead of dense rows [start,end). */
    ray_t*              sel;
    const int64_t*      sel_prefix;
    agg_valdesc_t       vd;
    int64_t             task_rows;
    uint32_t            n_tasks;
} agg_dense_ctx_t;

/* Each task owns a slice of one query allocation. Only the bitmap is
 * cleared; states and first rows are initialized on first touch. */
static int agg_dense_local_init(agg_dense_local_t* loc, int64_t total_slots, size_t block) {
    loc->oom = 0;
    size_t state_bytes = (size_t)total_slots * block;
    size_t row_bytes = (size_t)total_slots * sizeof(int64_t);
    size_t bits_bytes = ((size_t)total_slots + 63) / 64 * sizeof(uint64_t);
    loc->first_row = (int64_t*)(loc->states + state_bytes);
    loc->occupied = (uint64_t*)(loc->states + state_bytes + row_bytes);
    memset(loc->occupied, 0, bits_bytes);
    loc->ready = true;
    return 0;
}

/* Run vt->destroy on EVERY slot's buffered agg state in a dense slab (states +
 * total_slots blocks).  Empty slots hold the init'd state (median/top-k init →
 * buf=NULL), so destroy on them is ray_free_raw(NULL) — safe.  No-op when no agg has a
 * destroy hook (all-streaming).  Idempotent per slab when paired with a ray_free_raw()
 * immediately after; callers must invoke exactly once per slab. */
static void agg_dense_slab_destroy_states(char* states, int64_t total_slots,
                                          const agg_vtable_t** vts, const size_t* off,
                                          size_t block, uint32_t n_aggs) {
    if (!states) return;
    for (uint32_t a = 0; a < n_aggs; a++)
        if (vts[a]->destroy)
            for (int64_t s = 0; s < total_slots; s++)
                vts[a]->destroy(states + (size_t)s * block + off[a]);
}

static void agg_dense_local_destroy_states(agg_dense_local_t* loc,
        const agg_vtable_t** vts, const size_t* off, size_t block, uint32_t n_aggs) {
    if (!loc->ready) return;
    for (uint32_t a = 0; a < n_aggs; a++)
        if (vts[a]->destroy)
            for (int64_t s = 0; s < loc->slots; s++)
                if (loc->eager || (loc->occupied[s / 64] & (UINT64_C(1) << (s % 64))))
                    vts[a]->destroy(loc->states + (size_t)s * block + off[a]);
}

typedef struct {
    agg_dense_local_t* locals;
    int64_t slots;
    const agg_vtable_t** vts;
    const size_t* off;
    size_t block;
    uint32_t n_aggs, nw;
    char* states;
    int64_t* first;
} agg_dense_merge_ctx_t;
static void agg_dense_merge_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; agg_dense_merge_ctx_t* c = raw;
    /* A single streaming state is already the complete result. Copy it
     * directly instead of initializing and merging an identical state. */
    bool copy = c->nw == 1;
    for (uint32_t a = 0; a < c->n_aggs && copy; a++) copy = c->vts[a]->destroy == NULL;
    if (copy) {
        const agg_dense_local_t* loc = &c->locals[0];
        for (int64_t s = start; s < end; s++) {
            if (loc->occupied[s / 64] & (UINT64_C(1) << (s % 64))) {
                c->first[s] = loc->first_row[s];
                memcpy(c->states + (size_t)s * c->block, loc->states + (size_t)s * c->block, c->block);
            } else {
                c->first[s] = INT64_MAX;
                for (uint32_t a = 0; a < c->n_aggs; a++)
                    c->vts[a]->init(c->states + (size_t)s * c->block + c->off[a]);
            }
        }
        return;
    }
    for (int64_t s = start; s < end; s++) {
        c->first[s] = INT64_MAX;
        for (uint32_t a = 0; a < c->n_aggs; a++)
            c->vts[a]->init(c->states + (size_t)s * c->block + c->off[a]);
    }
    for (uint32_t w = 0; w < c->nw; w++) {
        agg_dense_local_t* loc = &c->locals[w];
        for (int64_t word = start / 64; word <= (end - 1) / 64; word++) {
            uint64_t bits = loc->occupied[word];
            if (word == start / 64) bits &= UINT64_MAX << (start % 64);
            if (word == (end - 1) / 64 && end % 64) bits &= (UINT64_C(1) << (end % 64)) - 1;
            while (bits) {
                int64_t s = word * 64 + __builtin_ctzll(bits);
                bits &= bits - 1;
                if (loc->first_row[s] < c->first[s]) c->first[s] = loc->first_row[s];
                for (uint32_t a = 0; a < c->n_aggs; a++)
                    c->vts[a]->merge(c->states + (size_t)s * c->block + c->off[a],
                                    loc->states + (size_t)s * c->block + c->off[a], NULL);
            }
        }
    }
}
static inline bool agg_put_cell_value(ray_t* out, int64_t i, ray_t* cell);
/* Caller owns null metadata: parallel emitters reduce this flag after the
 * barrier, while serial emitters may set it immediately. LISTs retain their
 * separate ownership-aware path. */
static inline bool agg_finalize_value(const agg_vtable_t* vt, const void* state,
                                      ray_t* out, int64_t i, int64_t param) {
    if (vt->finalize_value)
        return vt->finalize_value(state, (char*)ray_data(out) + (size_t)i * col_esz(out));
    ray_t* cell = vt->finalize(state, NULL, param);
    bool is_null = agg_put_cell_value(out, i, cell);
    ray_release(cell);
    return is_null;
}

/* Threshold = N-th value in the keep direction, found by quickselect on a
 * copy: O(n) and exact, so ties at the threshold are kept.  Callers
 * parallelize the value fill; the selection itself runs on the caller. */
bool agg_topn_threshold(const double* vals, int64_t n,
                        const ray_group_emit_filter_t* ef, double* thr) {
    if (ef->top_count_take <= 0 || n <= ef->top_count_take) return false;
    ray_t* hdr = NULL;
    double* sv = (double*)scratch_alloc(&hdr, (size_t)n * sizeof(double));
    if (!sv) return false;
    memcpy(sv, vals, (size_t)n * sizeof(double));
    int64_t k = ef->desc ? (n - ef->top_count_take) : (ef->top_count_take - 1);
    int64_t lo = 0, hi = n - 1;
    while (lo < hi) {
        double pivot = sv[k];
        int64_t i = lo, j = hi;
        while (i <= j) {
            while (sv[i] < pivot) i++;
            while (sv[j] > pivot) j--;
            if (i <= j) { double t = sv[i]; sv[i] = sv[j]; sv[j] = t; i++; j--; }
        }
        if (k <= j) hi = j;
        else if (k >= i) lo = i;
        else break;
    }
    *thr = sv[k];
    scratch_free(hdr);
    return true;
}

int64_t agg_topn_mark(const double* vals, int64_t n, const ray_group_emit_filter_t* ef,
                      bool have_thr, double thr, uint8_t* keep) {
    int64_t kept = 0;
    for (int64_t i = 0; i < n; i++) {
        double v = vals[i];
        bool ok = !(ef->min_count_exclusive > 0 && !(v > (double)ef->min_count_exclusive));
        if (ok && have_thr && (ef->desc ? (v < thr) : (v > thr))) ok = false;
        keep[i] = (uint8_t)ok;
        kept += ok;
    }
    return kept;
}

int64_t agg_topn_keep(const double* vals, int64_t n,
                      const ray_group_emit_filter_t* ef, uint8_t* keep) {
    if (n <= 0) return 0;
    double thr = 0.0;
    bool have_thr = agg_topn_threshold(vals, n, ef, &thr);
    return agg_topn_mark(vals, n, ef, have_thr, thr, keep);
}

/* Bounded candidate heap: the N best values of a range in the keep
 * direction.  For desc a min-heap of the N largest; for asc a max-heap of
 * the N smallest.  Every partition contributes one such set, and the
 * threshold of their union equals the threshold of the whole. */
static void agg_topn_candidates(const double* vals, int64_t n, int64_t cap, uint8_t desc,
                                double* heap, int64_t* hn) {
    int64_t h = 0;
    #define CAND_WORSE(a, b) (desc ? ((a) < (b)) : ((a) > (b)))   /* a is worse than b */
    for (int64_t i = 0; i < n; i++) {
        double v = vals[i];
        if (h < cap) {
            int64_t j = h++;
            heap[j] = v;
            while (j > 0) {                       /* sift up: root is the worst kept */
                int64_t par = (j - 1) / 2;
                if (!CAND_WORSE(heap[j], heap[par])) break;
                double t = heap[par]; heap[par] = heap[j]; heap[j] = t; j = par;
            }
        } else if (CAND_WORSE(heap[0], v)) {
            heap[0] = v;
            int64_t j = 0;
            for (;;) {                            /* sift down */
                int64_t l = 2 * j + 1, r = l + 1, m = j;
                if (l < cap && CAND_WORSE(heap[l], heap[m])) m = l;
                if (r < cap && CAND_WORSE(heap[r], heap[m])) m = r;
                if (m == j) break;
                double t = heap[m]; heap[m] = heap[j]; heap[j] = t; j = m;
            }
        }
    }
    #undef CAND_WORSE
    *hn = h;
}

bool agg_group_values_f64(const agg_vtable_t* vt, const char* states,
                          size_t stride, size_t off, const int64_t* slots,
                          int64_t n, int64_t param, double* out) {
    int8_t t = vt->out_type;
    switch (t) {
        case RAY_F64: case RAY_F32: case RAY_I64: case RAY_TIMESTAMP:
        case RAY_I32: case RAY_DATE: case RAY_TIME: case RAY_I16:
        case RAY_U8: case RAY_BOOL: break;
        default: return false;
    }
    /* The sort downstream ranks nulls FIRST ascending and LAST descending
     * (sort_nulls_first is !desc), i.e. below every value either way: an
     * asc take keeps an all-null group at rank 1, a desc take drops it.
     * -INFINITY reproduces exactly that in agg_topn_mark for both directions. */
    const double null_sink = -INFINITY;
    ray_t* cell = ray_vec_new(t, 1);
    if (!cell || RAY_IS_ERR(cell)) { if (cell) ray_error_free(cell); return false; }
    cell->len = 1;
    for (int64_t i = 0; i < n; i++) {
        int64_t g = slots ? slots[i] : i;
        const void* state = states + (size_t)g * stride + off;
        bool is_null = agg_finalize_value(vt, state, cell, 0, param);
        double v;
        switch (t) {
            case RAY_F64: v = ((const double*)ray_data(cell))[0]; break;
            case RAY_F32: v = ((const float*)ray_data(cell))[0]; break;
            case RAY_I64: case RAY_TIMESTAMP: v = (double)((const int64_t*)ray_data(cell))[0]; break;
            case RAY_I32: case RAY_DATE: case RAY_TIME: v = (double)((const int32_t*)ray_data(cell))[0]; break;
            case RAY_I16: v = (double)((const int16_t*)ray_data(cell))[0]; break;
            default: v = (double)((const uint8_t*)ray_data(cell))[0]; break;
        }
        out[i] = is_null || v != v ? null_sink : v;
    }
    ray_release(cell);
    return true;
}

typedef struct {
    const agg_vtable_t* vt;
    ray_t* out;
    const char* states;
    const int64_t* slots;
    size_t block, off;
    int64_t param;
    _Atomic(bool) any_null;
} agg_dense_emit_ctx_t;
static void agg_dense_emit_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; agg_dense_emit_ctx_t* c = raw;
    bool any = false;
    for (int64_t i = start; i < end; i++) {
        const void* st = c->states + (size_t)(c->slots ? c->slots[i] : i) * c->block + c->off;
        any |= agg_finalize_value(c->vt, st, c->out, i, c->param);
    }
    if (any) atomic_store_explicit(&c->any_null, true, memory_order_relaxed);
}

/* Compute the dense slot for row r via mixed-radix packing.  `compacted`
 * is loop-invariant (dp->compacted); callers pass it from a local so the
 * raw form stays branch-free. */
static inline int64_t agg_dense_slot(const agg_dense_ctx_t* c, int64_t r, bool compacted) {
    int64_t slot = 0;
    if (compacted) {
        for (uint32_t k = 0; k < c->n_keys; k++)
            slot += agg_dense_component(c->dp, k, agg_read_key_i64(c->key_cols[k], c->key_data[k], r)) * c->dp->strides[k];
    } else {
        for (uint32_t k = 0; k < c->n_keys; k++)
            slot += agg_dense_component_raw(c->dp, k, agg_read_key_i64(c->key_cols[k], c->key_data[k], r)) * c->dp->strides[k];
    }
    return slot;
}

static inline bool agg_dense_first(agg_dense_local_t* loc, int64_t slot) {
    uint64_t bit = UINT64_C(1) << (slot % 64);
    uint64_t* word = &loc->occupied[slot / 64];
    if (*word & bit) return false;
    *word |= bit;
    return true;
}

/* Phase A: per-worker dense accumulate over chunk [start,end). */
static void agg_dense_phaseA_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    agg_dense_ctx_t* c = vctx;
    agg_dense_local_t* loc = &c->locals[wid];
    if (loc->oom) return;
    int64_t n = end - start;
    if (n <= 0) return;

    /* ── Sel mode: chunk-iterate the SELECTED rows in [start,end) (selected-row
     * space).  Decode each chunk's ORIGINAL rows, compute dense slots, gather
     * agg values per chunk into reused scratch, and batch-update.  No full
     * index array, no full compact column. */
    if (c->sel) {
        int64_t rows[AGG_SEL_CHUNK];
        uint32_t gid[AGG_SEL_CHUNK];
        agg_sel_scratch_t sc;
        if (!agg_sel_scratch_init(&sc, c->n_aggs)) { loc->oom = 1; return; }
        agg_sel_cursor_t cur;
        agg_sel_cursor_init(&cur, c->sel, c->sel_prefix, start, end);
        int64_t cn;
        const bool compacted = c->dp->compacted;
        while ((cn = agg_sel_cursor_next(&cur, rows)) > 0) {
            for (int64_t i = 0; i < cn; i++) {
                int64_t r = rows[i];
                int64_t slot = agg_dense_slot(c, r, compacted);   /* provably in [0,total_slots) */
                gid[i] = (uint32_t)slot;
                if (agg_dense_first(loc, slot)) {
                    for (uint32_t a = 0; a < c->n_aggs; a++)
                        c->vts[a]->init(loc->states + (size_t)slot * c->block + c->off[a]);
                    loc->first_row[slot] = r;
                }
                if (r < loc->first_row[slot]) loc->first_row[slot] = r;
            }
            if (agg_sel_accum_chunk(&c->vd, &sc, loc->states, gid, rows, cn) != 0) {
                loc->oom = 1; agg_sel_scratch_free(&sc); return;
            }
        }
        agg_sel_scratch_free(&sc);
        return;
    }

    uint32_t cgid[8192];

    /* Hoist the per-row key-type dispatch out of the hot loop.  agg_dense_slot
     * calls agg_read_key_i64 — a switch(col->type) — on every row, and for SYM
     * keys ray_read_sym adds a second per-row switch on the code width.  Both
     * are loop-invariant.  For the common single-key case, branch ONCE on
     * (type, width) and run a tight typed load loop the compiler can vectorize;
     * this is the dominant cost of a low-card group-by (a 7-group count was
     * ~70% here).  Multi-key keeps the generic composite path. */
    if (c->n_keys == 1) {
        const void* kd = c->key_data[0];
        int64_t  kmin = c->dp->mins[0];
        int64_t  kstride = c->dp->strides[0];
        bool nullable = c->dp->nullable[0];
        int64_t null = c->dp->nulls[0], null_slot = c->dp->ranges[0] - 1;
        int64_t* fr = loc->first_row;
        uint32_t* cg = cgid;
        #define DENSE_SLOT1_LAZY(LD)                                            \
            for (int64_t r = start; r < end; r++) {                        \
                int64_t v = (int64_t)(LD);                               \
                int64_t slot = nullable && v == null ? null_slot          \
                    : (v - kmin) * kstride;                               \
                cg[r - start] = (uint32_t)slot;                            \
                if (agg_dense_first(loc, slot)) {                              \
                    for (uint32_t a = 0; a < c->n_aggs; a++)               \
                        c->vts[a]->init(loc->states + (size_t)slot * c->block + c->off[a]); \
                    fr[slot] = r;                                        \
                }                                                        \
            }
        /* For domains whose initialization amortizes across task rows,
         * avoid bitmap bookkeeping in the source-row loop. */
        #define DENSE_SLOT1_EAGER(LD, SLOT)                                \
            for (int64_t r = start; r < end; r++) {                        \
                int64_t v = (int64_t)(LD);                                \
                int64_t slot = (SLOT);                                    \
                cg[r - start] = (uint32_t)slot;                           \
                if (r < fr[slot]) fr[slot] = r;                           \
            }
        #define DENSE_SLOT1(LD) do {                                      \
            if (loc->eager) {                                            \
                if (!nullable) { DENSE_SLOT1_EAGER(LD, v - kmin); }        \
                else { DENSE_SLOT1_EAGER(LD, v == null ? null_slot : v - kmin); } \
            } else { DENSE_SLOT1_LAZY(LD); }                              \
        } while (0)
        switch (c->key_cols[0]->type) {
            case RAY_I64: case RAY_TIMESTAMP: DENSE_SLOT1(((const int64_t*)kd)[r]); break;
            case RAY_I32: case RAY_DATE: case RAY_TIME: DENSE_SLOT1(((const int32_t*)kd)[r]); break;
            case RAY_I16: DENSE_SLOT1(((const int16_t*)kd)[r]); break;
            case RAY_U8: case RAY_BOOL: DENSE_SLOT1(((const uint8_t*)kd)[r]); break;
            case RAY_SYM:
                switch (c->key_cols[0]->attrs & RAY_SYM_W_MASK) {
                    case RAY_SYM_W8:  DENSE_SLOT1(((const uint8_t*)kd)[r]);  break;
                    case RAY_SYM_W16: DENSE_SLOT1(((const uint16_t*)kd)[r]); break;
                    case RAY_SYM_W32: DENSE_SLOT1(((const uint32_t*)kd)[r]); break;
                    default:          DENSE_SLOT1(((const int64_t*)kd)[r]);  break; /* W64 */
                }
                break;
            default:
                for (int64_t r = start; r < end; r++) {
                    int64_t slot = agg_dense_slot(c, r, false);   /* single key: never compacted */
                    cg[r - start] = (uint32_t)slot;
                    if (r < fr[slot]) fr[slot] = r;
                }
        }
        #undef DENSE_SLOT1
        #undef DENSE_SLOT1_LAZY
        #undef DENSE_SLOT1_EAGER
    } else {
        #define DENSE_SLOT_MULTI(COMPACTED)                                    \
            for (int64_t r = start; r < end; r++) {                          \
                int64_t slot = agg_dense_slot(c, r, COMPACTED);   /* provably in [0,total_slots) */ \
                cgid[r - start] = (uint32_t)slot;                            \
                if (agg_dense_first(loc, slot)) {                            \
                    for (uint32_t a = 0; a < c->n_aggs; a++)                 \
                        c->vts[a]->init(loc->states + (size_t)slot * c->block + c->off[a]); \
                    loc->first_row[slot] = r;                                \
                }                                                            \
            }
        if (c->dp->compacted) { DENSE_SLOT_MULTI(true); }
        else { DENSE_SLOT_MULTI(false); }
        #undef DENSE_SLOT_MULTI
    }

    for (uint32_t a = 0; a < c->n_aggs; a++) {
        if (c->vts[a]->update_batch2) {                 /* binary agg (pearson) */
            const void* vx = (const char*)c->val_data[a]  + (size_t)start * c->val_esz[a];
            const void* vy = (const char*)c->val2_data[a] + (size_t)start * c->val2_esz[a];
            ray_valid_t valid_x = { vx, c->val_types[a],  c->val_hasnull[a] };
            ray_valid_t valid_y = { vy, c->val2_types[a], c->val2_hasnull[a] };
            c->vts[a]->update_batch2(loc->states + c->off[a], c->block, cgid,
                                     vx, vy, &valid_x, &valid_y, n, NULL);
        } else {
            const void* vals = (c->val_data[a])
                ? (const void*)((const char*)c->val_data[a] + (size_t)start * c->val_esz[a])
                : NULL;
            ray_valid_t valid = { vals, c->val_types[a],
                                  c->val_data[a] ? c->val_hasnull[a] : false };
            c->vts[a]->update_batch(loc->states + c->off[a], c->block,
                                    cgid, vals, &valid, n, NULL);
        }
    }
}

/* One slab per logical task, independent of the physical worker executing it.
 * A large pool can run a memory-bounded number of dense tasks safely. */
static void agg_dense_task_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_dense_ctx_t* c = raw;
    int64_t chunk = c->task_rows / c->n_tasks;
    for (int64_t task = start; task < end; task++) {
        agg_dense_local_t* loc = &c->locals[task];
        /* Storage was allocated once by the coordinator. Initialize each
         * slab on its consuming task, without another pool dispatch. */
        agg_dense_local_init(loc, loc->slots, c->block);
        if (loc->eager) for (int64_t s = 0; s < loc->slots; s++) {
            loc->first_row[s] = INT64_MAX;
            for (uint32_t a = 0; a < c->n_aggs; a++)
                c->vts[a]->init(loc->states + (size_t)s * c->block + c->off[a]);
        }
        int64_t begin = chunk * task;
        int64_t limit = task + 1 == c->n_tasks ? c->task_rows : chunk * (task + 1);
        if (c->sel) agg_dense_phaseA_fn(c, (uint32_t)task, begin, limit);
        else for (int64_t row = begin; row < limit && !agg_cancelled(); row += 8192)
            agg_dense_phaseA_fn(c, (uint32_t)task, row, limit - row < 8192 ? limit : row + 8192);
        if (loc->eager) for (int64_t s = 0; s < loc->slots; s++)
            if (loc->first_row[s] != INT64_MAX)
                loc->occupied[s / 64] |= UINT64_C(1) << (s % 64);
    }
}

/* Parallel dense path.  Precondition: dp->ok, all aggs ACC_STREAMING, per-worker
 * budget already gated by the caller. */
typedef struct {
    const dense_plan_t* plan;
    const int64_t* occupied;
    int64_t part_slots;
    uint32_t bits;
    ray_t* out;
    uint32_t component;
} agg_dense_key_emit_t;
static void agg_dense_key_emit(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_dense_key_emit_t* c = raw;
    void* data = ray_data(c->out);
    for (int64_t r = start; r < end; r++) {
        int64_t index = c->occupied[r];
        int64_t slot = c->bits ? ((index % c->part_slots) << c->bits) + index / c->part_slots : index;
        uint32_t k = c->component;
        int64_t component = (slot / c->plan->strides[k]) % c->plan->ranges[k];
        write_col_i64(data, r, agg_dense_code(c->plan, k, component), c->out->type, c->out->attrs);
    }
}

/* Top-N selection over a slot list: tasks finalize the filter aggregate for
 * disjoint ranges of `slots`, keep a bounded candidate heap each, the union's
 * N-th value is the threshold, and a second pass marks the kept groups.
 * Returns the kept count, or -1 when the aggregate has no scalar order or
 * memory ran out (the caller then emits every group and the query trims). */
typedef struct {
    const agg_vtable_t* vt;
    const char* states;
    size_t block, off;
    const int64_t* slots;
    int64_t n, param;
    const ray_group_emit_filter_t* ef;
    double* vals;
    uint8_t* keep;
    uint32_t tasks;
    double* cand;
    int64_t* cand_n;
    int64_t cap;
    bool have_thr;
    double thr;
    int64_t* kept;
    _Atomic(int) fail;
} agg_slots_topn_ctx_t;

static void agg_slots_topn_range(const agg_slots_topn_ctx_t* c, int64_t task, int64_t* begin, int64_t* end) {
    *begin = c->n / c->tasks * task;
    *end = task + 1 == (int64_t)c->tasks ? c->n : c->n / c->tasks * (task + 1);
}

static void agg_slots_vals_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_slots_topn_ctx_t* c = raw;
    for (int64_t task = start; task < end; task++) {
        int64_t b, e;
        agg_slots_topn_range(c, task, &b, &e);
        if (!agg_group_values_f64(c->vt, c->states, c->block, c->off, c->slots + b,
                                  e - b, c->param, c->vals + b)) {
            atomic_store_explicit(&c->fail, 1, memory_order_relaxed);
            continue;
        }
        if (c->cap > 0)
            agg_topn_candidates(c->vals + b, e - b, c->cap, c->ef->desc,
                                c->cand + (size_t)task * c->cap, &c->cand_n[task]);
    }
}

static void agg_slots_mark_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_slots_topn_ctx_t* c = raw;
    for (int64_t task = start; task < end; task++) {
        int64_t b, e;
        agg_slots_topn_range(c, task, &b, &e);
        c->kept[task] = agg_topn_mark(c->vals + b, e - b, c->ef, c->have_thr, c->thr, c->keep + b);
    }
}

static int64_t agg_slots_topn_select(ray_pool_t* pool, const agg_vtable_t* vt,
        const char* states, size_t block, size_t off, const int64_t* slots, int64_t n,
        int64_t param, const ray_group_emit_filter_t* ef, uint8_t* keep) {
    if (n <= 0) return 0;
    uint32_t tasks = pool && n >= RAY_PARALLEL_THRESHOLD ? ray_pool_total_workers(pool) * 4 : 1;
    if (tasks > RAY_POOL_INIT_TASKS) tasks = RAY_POOL_INIT_TASKS;
    if ((int64_t)tasks > n) tasks = (uint32_t)n;
    int64_t cap = ef->top_count_take > 0 && n > ef->top_count_take ? ef->top_count_take : 0;
    double* vals = ray_alloc_raw((size_t)n * sizeof(double));
    double* cand = ray_alloc_raw((size_t)(cap > 0 ? cap * tasks : 1) * sizeof(double));
    int64_t* cand_n = ray_calloc_raw((size_t)tasks * 2 * sizeof(int64_t));
    int64_t kept = -1;
    if (vals && cand && cand_n) {
        agg_slots_topn_ctx_t c = { .vt = vt, .states = states, .block = block, .off = off,
            .slots = slots, .n = n, .param = param, .ef = ef, .vals = vals, .keep = keep,
            .tasks = tasks, .cand = cand, .cand_n = cand_n, .cap = cap, .kept = cand_n + tasks };
        atomic_init(&c.fail, 0);
        if (tasks > 1) ray_pool_dispatch_n(pool, agg_slots_vals_fn, &c, tasks);
        else agg_slots_vals_fn(&c, 0, 0, 1);
        if (!atomic_load_explicit(&c.fail, memory_order_relaxed)) {
            if (cap > 0) {
                int64_t nc = 0;
                for (uint32_t t = 0; t < tasks; t++) {
                    memmove(cand + nc, cand + (size_t)t * cap, (size_t)cand_n[t] * sizeof(double));
                    nc += cand_n[t];
                }
                c.have_thr = agg_topn_threshold(cand, nc, ef, &c.thr);
            }
            if (tasks > 1) ray_pool_dispatch_n(pool, agg_slots_mark_fn, &c, tasks);
            else agg_slots_mark_fn(&c, 0, 0, 1);
            kept = 0;
            for (uint32_t t = 0; t < tasks; t++) kept += c.kept[t];
        }
    }
    ray_free_raw(vals); ray_free_raw(cand); ray_free_raw(cand_n);
    return kept;
}

/* Shared output stage. Takes ownership of states/first, borrows the
 * prepared aggregate layout and descriptors. */
static ray_t* agg_dense_finish(ray_t** key_cols, int64_t* key_syms, ray_op_ext_t* ext,
        ray_pool_t* pool, const agg_vo_t* vo, const agg_desc_t* d,
        int64_t total_slots, char* gstates, int64_t* gfirst,
        const dense_plan_t* key_plan, int64_t key_part_slots, uint32_t key_bits,
        const ray_group_emit_filter_t* ef, int64_t group_limit) {
    uint32_t n_keys = ext->n_keys, n_aggs = ext->n_aggs;
    const agg_vtable_t** vts = vo->vts;
    const size_t* off = vo->off;
    size_t block = vo->block;
    const int64_t* agg_syms = d->agg_syms;
    /* ── Phase C: collect occupied slots in slot order, emit. ──
     * Group-by output order is UNSPECIFIED, so we emit in dense-slot order
     * (the natural build order) rather than sorting by first_row.  gfirst[s] is
     * still the gather index (any member row) for the key columns. */
    int64_t ng = 0;
    for (int64_t s = 0; s < total_slots; s++) if (gfirst[s] != INT64_MAX) ng++;
    ray_profile_tick("dense: freed slabs and counted groups");

    int64_t* occupied_slot     = ray_alloc_raw((size_t)(ng > 0 ? ng : 1) * sizeof(int64_t));
    int64_t* first_row_ordered = key_plan ? NULL : ray_alloc_raw((size_t)(ng > 0 ? ng : 1) * sizeof(int64_t));
    if (!occupied_slot || (!key_plan && !first_row_ordered)) {
        ray_free_raw(occupied_slot); ray_free_raw(first_row_ordered);
        agg_dense_slab_destroy_states(gstates, total_slots, vts, off, block, n_aggs);
        ray_free_raw(gstates); ray_free_raw(gfirst);
        return ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
    }
    { int64_t i = 0;
      for (int64_t s = 0; s < total_slots; s++)
          if (gfirst[s] != INT64_MAX) {
              if (first_row_ordered) first_row_ordered[i] = gfirst[s];
              occupied_slot[i] = s; i++;
          }
    }
    /* Bounded emit (HEAD(GROUP) hint): the N groups with the smallest first
     * row ARE the first N groups in first-seen order.  Select them with an
     * N-sized max-heap over the occupied slots and emit them ascending by
     * first row, byte-identical to trimming a first-seen-ordered result. */
    if (group_limit > 0 && first_row_ordered && ng > group_limit) {
        int64_t n_keep = group_limit;
        int64_t* hkey = ray_alloc_raw((size_t)n_keep * sizeof(int64_t));
        int64_t* hslot = ray_alloc_raw((size_t)n_keep * sizeof(int64_t));
        if (hkey && hslot) {
            int64_t hn = 0;
            for (int64_t i = 0; i < ng; i++) {
                int64_t first = first_row_ordered[i], slot = occupied_slot[i];
                if (hn < n_keep) {
                    int64_t j = hn++;
                    hkey[j] = first; hslot[j] = slot;
                    while (j > 0) {
                        int64_t par = (j - 1) / 2;
                        if (hkey[par] >= hkey[j]) break;
                        int64_t tk = hkey[par]; hkey[par] = hkey[j]; hkey[j] = tk;
                        int64_t ts = hslot[par]; hslot[par] = hslot[j]; hslot[j] = ts;
                        j = par;
                    }
                } else if (first < hkey[0]) {
                    hkey[0] = first; hslot[0] = slot;
                    int64_t j = 0;
                    for (;;) {
                        int64_t l = 2 * j + 1, rr = l + 1, m = j;
                        if (l < n_keep && hkey[l] > hkey[m]) m = l;
                        if (rr < n_keep && hkey[rr] > hkey[m]) m = rr;
                        if (m == j) break;
                        int64_t tk = hkey[m]; hkey[m] = hkey[j]; hkey[j] = tk;
                        int64_t ts = hslot[m]; hslot[m] = hslot[j]; hslot[j] = ts;
                        j = m;
                    }
                }
            }
            /* heap sort ascending by first row */
            for (int64_t end = hn - 1; end > 0; end--) {
                int64_t tk = hkey[0]; hkey[0] = hkey[end]; hkey[end] = tk;
                int64_t ts = hslot[0]; hslot[0] = hslot[end]; hslot[end] = ts;
                int64_t j = 0;
                for (;;) {
                    int64_t l = 2 * j + 1, rr = l + 1, m = j;
                    if (l < end && hkey[l] > hkey[m]) m = l;
                    if (rr < end && hkey[rr] > hkey[m]) m = rr;
                    if (m == j) break;
                    tk = hkey[m]; hkey[m] = hkey[j]; hkey[j] = tk;
                    ts = hslot[m]; hslot[m] = hslot[j]; hslot[j] = ts;
                    j = m;
                }
            }
            for (int64_t i = 0; i < hn; i++) { occupied_slot[i] = hslot[i]; first_row_ordered[i] = hkey[i]; }
            ng = hn;
        }
        ray_free_raw(hkey); ray_free_raw(hslot);
    }
    /* Top-N emit filter: keep only the groups the filter would keep, in the
     * same slot order.  Nothing below allocates for the dropped groups. */
    if (ef && ng > 0) {
        uint8_t* keep = ray_alloc_raw((size_t)ng);
        int64_t kept = keep ? agg_slots_topn_select(pool, vts[ef->agg_index], gstates, block,
                off[ef->agg_index], occupied_slot, ng,
                ext->agg_k ? ext->agg_k[ef->agg_index] : 0, ef, keep) : -1;
        if (kept >= 0) {
            int64_t w = 0;
            for (int64_t i = 0; i < ng; i++) if (keep[i]) {
                occupied_slot[w] = occupied_slot[i];
                if (first_row_ordered) first_row_ordered[w] = first_row_ordered[i];
                w++;
            }
            ng = kept;
            route_stats.topn_native = true;
        }
        ray_free_raw(keep);
        ray_profile_tick("dense: selected top-N groups");
    }

    ray_t* result = ray_table_new(n_keys + n_aggs);
    if (!result || RAY_IS_ERR(result)) {
        agg_dense_slab_destroy_states(gstates, total_slots, vts, off, block, n_aggs);
        ray_free_raw(occupied_slot); ray_free_raw(first_row_ordered); ray_free_raw(gstates); ray_free_raw(gfirst);
        return result ? result : ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
    }

    for (uint32_t k = 0; k < n_keys; k++) {
        ray_t* kc = key_plan ? col_vec_new(key_cols[k], ng)
                            : ray_group_gather(key_cols[k], first_row_ordered, ng);
        if (!kc || RAY_IS_ERR(kc)) {
            agg_dense_slab_destroy_states(gstates, total_slots, vts, off, block, n_aggs);
            ray_free_raw(occupied_slot); ray_free_raw(first_row_ordered); ray_free_raw(gstates); ray_free_raw(gfirst);
            ray_release(result); return kc ? kc : ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
        }
        if (key_plan) {
            kc->len = ng;
            if (ray_vec_may_have_nulls(key_cols[k])) kc->attrs |= RAY_ATTR_HAS_NULLS;
            if (kc->type == RAY_SYM) ray_sym_vec_adopt_domain(kc, sym_domain_rep(key_cols[k]));
            agg_dense_key_emit_t emit = {key_plan, occupied_slot, key_part_slots, key_bits, kc, k};
            ray_pool_dispatch(pool, agg_dense_key_emit, &emit, ng);
        }
        result = ray_table_add_col(result, key_syms[k], kc);
        ray_release(kc);
    }

    for (uint32_t a = 0; a < n_aggs; a++) {
        /* Buffered top_n/bot_n produce a LIST cell per group (a native vector);
         * median and all streaming aggs produce a scalar out_type cell. */
        bool is_list = (vts[a]->out_type == RAY_LIST);
        ray_t* out = is_list ? ray_list_new(ng)
                             : ray_vec_new(vts[a]->out_type, ng);
        if (!out || RAY_IS_ERR(out)) {
            agg_dense_slab_destroy_states(gstates, total_slots, vts, off, block, n_aggs);
            ray_free_raw(occupied_slot); ray_free_raw(first_row_ordered); ray_free_raw(gstates); ray_free_raw(gfirst);
            ray_release(result); return out ? out : ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
        }
        out->len = ng;
        int64_t kparam = (ext->agg_k ? ext->agg_k[a] : 0);
        if (!is_list) {
            agg_dense_emit_ctx_t emit = { .vt = vts[a], .out = out, .states = gstates,
                .slots = occupied_slot, .block = block, .off = off[a], .param = kparam, .any_null = false };
            ray_pool_dispatch(pool, agg_dense_emit_fn, &emit, ng);
            if (atomic_load_explicit(&emit.any_null, memory_order_relaxed)) out->attrs |= RAY_ATTR_HAS_NULLS;
        } else {
            for (int64_t i = 0; i < ng; i++) {
                ray_t* cell = vts[a]->finalize(gstates + (size_t)occupied_slot[i] * block + off[a], NULL, kparam);
                out = ray_list_set(out, i, cell);   /* retains cell */
                ray_release(cell);
            }
        }
        int64_t agg_name = agg_result_col_name(agg_syms[a], ext->agg_ops[a]);
        result = ray_table_add_col(result, agg_name, out);
        ray_release(out);
    }

    /* Finalize is done reading the global slab → destroy its buffered per-group
     * state exactly once before freeing the slab. */
    agg_dense_slab_destroy_states(gstates, total_slots, vts, off, block, n_aggs);
    ray_free_raw(occupied_slot); ray_free_raw(first_row_ordered); ray_free_raw(gstates); ray_free_raw(gfirst);
    if (result && !RAY_IS_ERR(result) && agg_cancelled()) { ray_release(result); return ray_error("cancel", NULL); }
    return result;
}

/* Key-owned partitions: each group is reduced exactly once. Source tasks
 * histogram/scatter row IDs, then independently scheduled key partitions
 * reduce those rows directly into disjoint output state. No state merge. */
typedef struct {
    uint32_t part;
    uint64_t begin, end;
    int64_t base;
} agg_dense_partition_task_t;

typedef struct {
    ray_t* key;
    ray_t** keys;
    const void** key_data;
    ray_t* selection_indices;
    const int64_t* selected_rows;
    const dense_plan_t* plan;
    agg_valdesc_t values;
    int64_t rows, part_slots;
    uint32_t sources, parts, bits, n_tasks;
    agg_dense_partition_task_t* tasks;
    uint64_t* counts;
    uint64_t* starts;
    uint32_t* gids;
    char* payload;
    size_t record_size;
    size_t* value_offsets;
    size_t* value2_offsets;
    char* states;
    int64_t* first;
    _Atomic(bool) failed;
} agg_dense_partition_ctx_t;

static void agg_dense_partition_count(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_dense_partition_ctx_t* c = raw;
    const void* data = ray_data(c->key);
    for (int64_t task = start; task < end; task++) {
        int64_t begin = c->rows / c->sources * task;
        int64_t limit = task + 1 == c->sources ? c->rows : c->rows / c->sources * (task + 1);
        uint64_t* count = c->counts + task * c->parts;
        if (c->plan->n_keys > 1) {
            #define PART_SLOT_MULTI(COMPONENT)                                 \
                for (int64_t r = begin; r < limit; r++) {                    \
                    int64_t source = c->selected_rows ? c->selected_rows[r] : r; \
                    int64_t slot = 0;                                        \
                    for (uint32_t k = 0; k < c->plan->n_keys; k++)           \
                        slot += COMPONENT(c->plan, k, agg_read_key_i64(c->keys[k], c->key_data[k], source)) * c->plan->strides[k]; \
                    c->gids[r] = (uint32_t)slot;                             \
                    count[slot & (c->parts - 1)]++;                          \
                }
            if (c->plan->compacted) { PART_SLOT_MULTI(agg_dense_component); }
            else { PART_SLOT_MULTI(agg_dense_component_raw); }
            #undef PART_SLOT_MULTI
            continue;
        }
        #define PART_COUNT(T) do { \
            const T* p = data; \
            for (int64_t r = begin; r < limit; r++) { \
                uint32_t slot = (uint32_t)agg_dense_component_raw(c->plan, 0, (int64_t)p[c->selected_rows ? c->selected_rows[r] : r]); \
                c->gids[r] = slot; \
                count[slot & (c->parts - 1)]++; \
            } \
        } while (0)
        switch (c->key->type) {
            case RAY_I64: case RAY_TIMESTAMP: PART_COUNT(int64_t); break;
            case RAY_I32: case RAY_DATE: case RAY_TIME: PART_COUNT(int32_t); break;
            case RAY_I16: PART_COUNT(int16_t); break;
            case RAY_U8: case RAY_BOOL: PART_COUNT(uint8_t); break;
            case RAY_SYM:
                switch (c->key->attrs & RAY_SYM_W_MASK) {
                    case RAY_SYM_W8: PART_COUNT(uint8_t); break;
                    case RAY_SYM_W16: PART_COUNT(uint16_t); break;
                    case RAY_SYM_W32: PART_COUNT(uint32_t); break;
                    default: PART_COUNT(int64_t); break;
                }
                break;
        }
        #undef PART_COUNT
    }
}

/* Resolve native widths outside the row loop. A runtime-sized memcpy per
 * value dominated scatter for eight-byte inputs and mixed aggregates. */
static void agg_dense_partition_field(agg_dense_partition_ctx_t* c, const void* data,
        uint8_t width, size_t field, const uint64_t* starts, int64_t begin, int64_t end) {
    uint64_t cursor[RAY_POOL_INIT_TASKS / 2];
    assert(c->parts <= RAY_POOL_INIT_TASKS / 2);
    memcpy(cursor, starts, c->parts * sizeof(*cursor));
    #define SCATTER_FIELD(W) do { \
        for (int64_t r = begin; r < end; r++) { \
            uint32_t part = c->gids[r] & (c->parts - 1); \
            memcpy(c->payload + cursor[part]++ * c->record_size + field, \
                   (const char*)data + (size_t)(c->selected_rows ? c->selected_rows[r] : r) * (W), (W)); \
        } \
    } while (0)
    switch (width) {
        case 1: SCATTER_FIELD(1); break;
        case 2: SCATTER_FIELD(2); break;
        case 4: SCATTER_FIELD(4); break;
        case 8: SCATTER_FIELD(8); break;
    }
    #undef SCATTER_FIELD
}

static void agg_dense_partition_scatter(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_dense_partition_ctx_t* c = raw;
    for (int64_t task = start; task < end; task++) {
        int64_t begin = c->rows / c->sources * task;
        int64_t limit = task + 1 == c->sources ? c->rows : c->rows / c->sources * (task + 1);
        const uint64_t* starts = c->counts + task * c->parts;
        uint64_t cursor[RAY_POOL_INIT_TASKS / 2];
        assert(c->parts <= RAY_POOL_INIT_TASKS / 2);
        memcpy(cursor, starts, c->parts * sizeof(*cursor));
        const agg_valdesc_t* vd = &c->values;
        if (vd->n_aggs == 1 && vd->val_data[0] && !vd->val2_data[0]) {
            #define SCATTER_UNARY(W) do { \
                for (int64_t r = begin; r < limit; r++) { \
                    uint32_t slot = c->gids[r]; \
                    uint32_t gid = slot >> c->bits, part = slot & (c->parts - 1); \
                    char* dst = c->payload + cursor[part]++ * (4 + (W)); \
                    memcpy(dst, &gid, 4); \
                    memcpy(dst + 4, (const char*)vd->val_data[0] + (size_t)(c->selected_rows ? c->selected_rows[r] : r) * (W), (W)); \
                } \
            } while (0)
            switch (vd->val_esz[0]) {
                case 1: SCATTER_UNARY(1); break;
                case 2: SCATTER_UNARY(2); break;
                case 4: SCATTER_UNARY(4); break;
                case 8: SCATTER_UNARY(8); break;
            }
            #undef SCATTER_UNARY
        } else {
            /* Complete nearby records before advancing through a source
             * range. Whole-column passes repeatedly read/write the same large
             * output buffer when several input fields share each record. */
            int64_t chunk_rows = (64 * 1024) / c->record_size;
            if (chunk_rows > AGG_SEL_CHUNK) chunk_rows = AGG_SEL_CHUNK;
            if (chunk_rows < 1) chunk_rows = 1;
            uint64_t chunk_starts[RAY_POOL_INIT_TASKS / 2];
            for (int64_t chunk = begin; chunk < limit; chunk += chunk_rows) {
                int64_t chunk_end = limit - chunk < chunk_rows ? limit : chunk + chunk_rows;
                memcpy(chunk_starts, cursor, c->parts * sizeof(*cursor));
                for (int64_t r = chunk; r < chunk_end; r++) {
                    uint32_t slot = c->gids[r], part = slot & (c->parts - 1), gid = slot >> c->bits;
                    char* dst = c->payload + cursor[part]++ * c->record_size;
                    memcpy(dst, &gid, 4);
                }
                size_t emitted = sizeof(uint32_t);
                for (uint32_t a = 0; a < vd->n_aggs; a++) {
                    if (vd->val_data[a] && c->value_offsets[a] >= emitted) {
                        agg_dense_partition_field(c, vd->val_data[a], vd->val_esz[a], c->value_offsets[a], chunk_starts, chunk, chunk_end);
                        emitted = c->value_offsets[a] + vd->val_esz[a];
                    }
                    if (vd->val2_data[a] && c->value2_offsets[a] >= emitted) {
                        agg_dense_partition_field(c, vd->val2_data[a], vd->val2_esz[a], c->value2_offsets[a], chunk_starts, chunk, chunk_end);
                        emitted = c->value2_offsets[a] + vd->val2_esz[a];
                    }
                }
            }
        }
    }
}

static void agg_dense_payload_gather(char* dst, const char* src, size_t stride, uint8_t width, int64_t n) {
    #define PAYLOAD_GATHER(W) for (int64_t i = 0; i < n; i++) \
        memcpy(dst + (size_t)i * (W), src + (size_t)i * stride, (W))
    switch (width) {
        case 1: PAYLOAD_GATHER(1); break;
        case 2: PAYLOAD_GATHER(2); break;
        case 4: PAYLOAD_GATHER(4); break;
        case 8: PAYLOAD_GATHER(8); break;
        default: for (int64_t i = 0; i < n; i++) memcpy(dst + (size_t)i * width, src + (size_t)i * stride, width);
    }
    #undef PAYLOAD_GATHER
}
static int agg_dense_payload_accum(const agg_dense_partition_ctx_t* c, agg_sel_scratch_t* sc,
        char* states, const uint32_t* gids, uint64_t start, int64_t n) {
    const agg_valdesc_t* vd = &c->values;
    const char* records = c->payload + start * c->record_size;
    for (uint32_t a = 0; a < vd->n_aggs; a++) {
        if (vd->val_data[a]) {
            if (!sc->gv[a]) sc->gv[a] = ray_alloc_raw((size_t)AGG_SEL_CHUNK * vd->val_esz[a]);
            if (!sc->gv[a]) return -1;
            agg_dense_payload_gather(sc->gv[a], records + c->value_offsets[a], c->record_size, vd->val_esz[a], n);
        }
        if (vd->val2_data[a]) {
            if (!sc->gy[a]) sc->gy[a] = ray_alloc_raw((size_t)AGG_SEL_CHUNK * vd->val2_esz[a]);
            if (!sc->gy[a]) return -1;
            agg_dense_payload_gather(sc->gy[a], records + c->value2_offsets[a], c->record_size, vd->val2_esz[a], n);
        }
        ray_valid_t vx = {sc->gv[a], vd->val_types[a], vd->val_data[a] && vd->val_hasnull[a]};
        ray_valid_t vy = {sc->gy[a], vd->val2_types[a], vd->val2_hasnull[a]};
        if (vd->vts[a]->update_batch2)
            vd->vts[a]->update_batch2(states + vd->off[a], vd->block, gids, sc->gv[a], sc->gy[a], &vx, &vy, n, NULL);
        else vd->vts[a]->update_batch(states + vd->off[a], vd->block, gids, sc->gv[a], &vx, n, NULL);
    }
    return 0;
}

static void agg_dense_partition_reduce(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_dense_partition_ctx_t* c = raw;
    const agg_valdesc_t* vd = &c->values;
    agg_sel_scratch_t scratch;
    if (!agg_sel_scratch_init(&scratch, vd->n_aggs)) {
        atomic_store_explicit(&c->failed, true, memory_order_relaxed);
        return;
    }
    uint32_t gids[AGG_SEL_CHUNK];
    for (int64_t task = start; task < end && !agg_cancelled(); task++) {
        const agg_dense_partition_task_t* work = &c->tasks[task];
        int64_t base = work->base;
        char* states = c->states + (size_t)base * vd->block;
        int64_t* first = c->first + base;
        for (int64_t slot = 0; slot < c->part_slots; slot++) {
            first[slot] = INT64_MAX;
            for (uint32_t a = 0; a < vd->n_aggs; a++)
                vd->vts[a]->init(states + (size_t)slot * vd->block + vd->off[a]);
        }
        for (uint64_t at = work->begin; at < work->end; at += AGG_SEL_CHUNK) {
            int64_t n = work->end - at < AGG_SEL_CHUNK ? work->end - at : AGG_SEL_CHUNK;
            for (int64_t i = 0; i < n; i++) {
                uint32_t gid;
                memcpy(&gid, c->payload + (at + i) * c->record_size, 4);
                gids[i] = gid;
                first[gid] = 0; /* key is reconstructed from the partition/slot */
            }
            if (agg_dense_payload_accum(c, &scratch, states, gids, at, n) != 0) {
                atomic_store_explicit(&c->failed, true, memory_order_relaxed);
                break;
            }
        }
    }
    agg_sel_scratch_free(&scratch);
}

static void agg_dense_partition_merge(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_dense_partition_ctx_t* c = raw;
    const agg_valdesc_t* vd = &c->values;
    for (int64_t part = start; part < end; part++) {
        int64_t dst = part * c->part_slots;
        for (uint32_t task = 0; task < c->n_tasks; task++) {
            const agg_dense_partition_task_t* work = &c->tasks[task];
            if (work->part != part || work->base == dst) continue;
            for (int64_t slot = 0; slot < c->part_slots; slot++) {
                int64_t src = work->base + slot, target = dst + slot;
                if (c->first[src] == INT64_MAX) continue;
                if (c->first[src] < c->first[target]) c->first[target] = c->first[src];
                for (uint32_t a = 0; a < vd->n_aggs; a++)
                    vd->vts[a]->merge(c->states + (size_t)target * vd->block + vd->off[a],
                                      c->states + (size_t)src * vd->block + vd->off[a], NULL);
            }
        }
    }
}

/* Pack each input column once, even when several unary/binary aggregates
 * consume it. Planning and execution use the same native-byte layout. */
static size_t agg_partition_record(const agg_desc_t* d, uint32_t n,
        size_t* x_offsets, size_t* y_offsets) {
    size_t bytes = sizeof(uint32_t);
    for (uint32_t f = 0; f < 2 * n; f++) {
        uint32_t a = f / 2;
        const void* data = f % 2 ? d->val2_data[a] : d->val_data[a];
        uint8_t width = f % 2 ? d->val2_esz[a] : d->val_esz[a];
        size_t offset = bytes;
        bool found = false;
        for (uint32_t prev = 0; data && prev < f; prev++) {
            uint32_t b = prev / 2;
            const void* other = prev % 2 ? d->val2_data[b] : d->val_data[b];
            uint8_t size = prev % 2 ? d->val2_esz[b] : d->val_esz[b];
            if (other == data && size == width) {
                if (x_offsets) offset = prev % 2 ? y_offsets[b] : x_offsets[b];
                found = true; break;
            }
        }
        if (data && !found) bytes += width;
        if (x_offsets) (f % 2 ? y_offsets : x_offsets)[a] = offset;
    }
    return bytes;
}

static uint32_t agg_dense_partition_parts(uint32_t sources, int64_t slots) {
    /* Leave room for skew subtasks; dispatch_n must never truncate work. */
    _Static_assert(RAY_POOL_INIT_TASKS >= 2 &&
        (RAY_POOL_INIT_TASKS & (RAY_POOL_INIT_TASKS - 1)) == 0,
        "dense partition cursor capacity must be a power of two");
    uint32_t parts = 1;
    while (parts < RAY_POOL_INIT_TASKS / 2 &&
           (parts < sources * 4 || (int64_t)parts * 32768 < slots)) parts *= 2;
    return parts;
}

static ray_t* agg_dense_partitioned(ray_t** key_cols, int64_t* key_syms, ray_op_ext_t* ext,
        ray_pool_t* pool, const dense_plan_t* plan, int64_t rows,
        const agg_vo_t* vo, const agg_desc_t* d, ray_t* selection,
        const ray_group_emit_filter_t* ef) {
    uint32_t workers = ray_pool_total_workers(pool);
    uint32_t sources = workers;
    if (sources > RAY_POOL_INIT_TASKS / 2) sources = RAY_POOL_INIT_TASKS / 2;
    uint32_t parts = agg_dense_partition_parts(workers, plan->total_slots);
    uint32_t bits = (uint32_t)__builtin_ctz(parts);
    uint32_t split_budget = sources * 4 < parts ? sources * 4 : parts;
    int64_t part_slots = ((plan->total_slots + parts - 1) / parts + 7) & ~INT64_C(7);
    int64_t slots = part_slots * parts;
    agg_dense_partition_ctx_t c = {
        .key = key_cols[0], .keys = key_cols, .key_data = d->key_data,
        .plan = plan, .rows = rows, .part_slots = part_slots,
        .sources = sources, .parts = parts, .bits = bits, .failed = false,
        .values = { .n_aggs = ext->n_aggs, .vts = vo->vts, .off = vo->off, .block = vo->block,
            .val_data = d->val_data, .val_types = d->val_types, .val_hasnull = d->val_hasnull, .val_esz = d->val_esz,
            .val2_data = d->val2_data, .val2_types = d->val2_types, .val2_hasnull = d->val2_hasnull, .val2_esz = d->val2_esz }
    };
    if (selection) {
        c.selection_indices = ray_rowsel_to_indices(selection);
        if (!c.selection_indices || RAY_IS_ERR(c.selection_indices)) goto failed;
        c.selected_rows = ray_data(c.selection_indices);
    }
    c.counts = ray_calloc_raw(((size_t)sources * parts + parts + 1) * sizeof(uint64_t));
    c.gids = ray_alloc_raw((size_t)rows * sizeof(uint32_t));
    c.value_offsets = ray_alloc_raw((size_t)ext->n_aggs * 2 * sizeof(size_t));
    if (!c.value_offsets) goto failed;
    c.value2_offsets = c.value_offsets + ext->n_aggs;
    c.record_size = agg_partition_record(d, ext->n_aggs, c.value_offsets, c.value2_offsets);
    c.payload = ray_alloc_raw((size_t)rows * c.record_size);
    c.tasks = ray_alloc_raw((size_t)(parts + split_budget) * sizeof(*c.tasks));
    if (!c.counts || !c.gids || !c.tasks || !c.payload) goto failed;
    c.starts = c.counts + (size_t)sources * parts;
    ray_pool_dispatch_n(pool, agg_dense_partition_count, &c, sources);
    if (agg_cancelled()) goto failed;
    uint64_t offset = 0;
    for (uint32_t part = 0; part < parts; part++) {
        c.starts[part] = offset;
        for (uint32_t task = 0; task < sources; task++) {
            size_t at = (size_t)task * parts + part;
            uint64_t n = c.counts[at]; c.counts[at] = offset; offset += n;
        }
    }
    c.starts[parts] = offset;
    uint64_t grain = ((uint64_t)rows + split_budget - 1) / split_budget;
    int64_t extra_slots = 0;
    for (uint32_t part = 0; part < parts; part++) {
        uint64_t begin = c.starts[part], end = c.starts[part + 1];
        /* Even empty partitions own initialized output slots. */
        do {
            uint64_t limit = end - begin < grain ? end : begin + grain;
            int64_t base = begin == c.starts[part] ? (int64_t)part * part_slots : slots + extra_slots;
            if (base >= slots) extra_slots += part_slots;
            c.tasks[c.n_tasks++] = (agg_dense_partition_task_t){part, begin, limit, base};
            begin = limit;
        } while (begin < end);
    }
    c.states = ray_alloc_raw((size_t)(slots + extra_slots) * vo->block);
    c.first = ray_alloc_raw((size_t)(slots + extra_slots) * sizeof(int64_t));
    if (!c.states || !c.first) goto failed;
    ray_profile_tick("dense partition: histogram");
    ray_pool_dispatch_n(pool, agg_dense_partition_scatter, &c, sources);
    if (agg_cancelled()) goto failed;
    ray_profile_tick("dense partition: scattered rows");
    ray_pool_dispatch_n(pool, agg_dense_partition_reduce, &c, c.n_tasks);
    if (agg_cancelled() || atomic_load_explicit(&c.failed, memory_order_relaxed)) goto failed;
    ray_profile_tick("dense partition: reduced groups");
    ray_pool_dispatch_n(pool, agg_dense_partition_merge, &c, parts);
    if (agg_cancelled()) goto failed;
    ray_profile_tick("dense partition: merged split partitions");
    ray_free_raw(c.counts); ray_free_raw(c.gids); ray_free_raw(c.tasks);
    ray_free_raw(c.payload); ray_free_raw(c.value_offsets); ray_release(c.selection_indices);
    route_stats.dense_strategy = AGG_DENSE_PARTITIONED;
    route_stats.dense_tasks = c.n_tasks;
    route_stats.dense_local_slots = slots + extra_slots;
    return agg_dense_finish(key_cols, key_syms, ext, pool, vo, d, slots, c.states, c.first, plan, part_slots, bits, ef, 0);
failed:
    ray_free_raw(c.counts); ray_free_raw(c.gids); ray_free_raw(c.tasks);
    ray_free_raw(c.payload); ray_free_raw(c.value_offsets); ray_release(c.selection_indices); ray_free_raw(c.states); ray_free_raw(c.first);
    return ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
}

typedef struct {
    ray_t* key;
    const dense_plan_t* plan;
    const agg_vo_t* layout;
    const agg_desc_t* desc;
    uint32_t n_aggs;
    char* states;
    _Atomic(uint64_t)* occupied;
    int64_t* first;
} agg_dense_shared_ctx_t;

static void agg_dense_shared_init(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_dense_shared_ctx_t* c = raw;
    const agg_vo_t* vo = c->layout;
    for (int64_t s = start; s < end; s++)
        for (uint32_t a = 0; a < c->n_aggs; a++)
            vo->vts[a]->init(c->states + (size_t)s * vo->block + vo->off[a]);
}

static void agg_dense_shared_rows(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_dense_shared_ctx_t* c = raw;
    const agg_vo_t* vo = c->layout;
    const agg_desc_t* d = c->desc;
    const void* data = ray_data(c->key);
    uint32_t gids[8192];
    for (int64_t begin = start; begin < end && !agg_cancelled(); begin += 8192) {
        int64_t n = end - begin < 8192 ? end - begin : 8192;
        #define SHARED_GIDS(T) do { \
            const T* p = data; \
            for (int64_t i = 0; i < n; i++) { \
                uint32_t slot = (uint32_t)agg_dense_component_raw(c->plan, 0, (int64_t)p[begin + i]); \
                gids[i] = slot; \
                uint64_t bit = UINT64_C(1) << (slot % 64); \
                _Atomic(uint64_t)* word = &c->occupied[slot / 64]; \
                if (!(atomic_load_explicit(word, memory_order_relaxed) & bit)) \
                    atomic_fetch_or_explicit(word, bit, memory_order_relaxed); \
            } \
        } while (0)
        switch (c->key->type) {
            case RAY_I64: case RAY_TIMESTAMP: SHARED_GIDS(int64_t); break;
            case RAY_I32: case RAY_DATE: case RAY_TIME: SHARED_GIDS(int32_t); break;
            case RAY_I16: SHARED_GIDS(int16_t); break;
            case RAY_U8: case RAY_BOOL: SHARED_GIDS(uint8_t); break;
            case RAY_SYM:
                switch (c->key->attrs & RAY_SYM_W_MASK) {
                    case RAY_SYM_W8: SHARED_GIDS(uint8_t); break;
                    case RAY_SYM_W16: SHARED_GIDS(uint16_t); break;
                    case RAY_SYM_W32: SHARED_GIDS(uint32_t); break;
                    default: SHARED_GIDS(int64_t); break;
                }
                break;
        }
        #undef SHARED_GIDS
        for (uint32_t a = 0; a < c->n_aggs; a++) {
            const void* vals = (const char*)d->val_data[a] + (size_t)begin * d->val_esz[a];
            ray_valid_t valid = {vals, d->val_types[a], d->val_hasnull[a]};
            vo->vts[a]->update_shared(c->states + vo->off[a], vo->block, gids, vals, &valid, n);
        }
    }
}

static void agg_dense_shared_occupied(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_dense_shared_ctx_t* c = raw;
    for (int64_t s = start; s < end; s++)
        c->first[s] = atomic_load_explicit(&c->occupied[s / 64], memory_order_relaxed)
            & (UINT64_C(1) << (s % 64)) ? 0 : INT64_MAX;
}

static ray_t* agg_dense_shared(ray_t** key_cols, int64_t* key_syms, ray_op_ext_t* ext,
        ray_pool_t* pool, const dense_plan_t* plan, int64_t rows,
        const agg_vo_t* vo, const agg_desc_t* d, const ray_group_emit_filter_t* ef) {
    int64_t slots = plan->total_slots;
    agg_dense_shared_ctx_t c = {.key = key_cols[0], .plan = plan, .layout = vo, .desc = d, .n_aggs = ext->n_aggs};
    c.states = ray_alloc_raw((size_t)slots * vo->block);
    c.first = ray_alloc_raw((size_t)slots * sizeof(int64_t));
    c.occupied = ray_alloc_raw(((size_t)slots + 63) / 64 * sizeof(*c.occupied));
    if (!c.states || !c.first || !c.occupied) goto failed;
    for (int64_t w = 0; w < (slots + 63) / 64; w++) atomic_init(&c.occupied[w], 0);
    ray_pool_dispatch(pool, agg_dense_shared_init, &c, slots);
    ray_profile_tick("dense shared: initialized states");
    ray_pool_dispatch(pool, agg_dense_shared_rows, &c, rows);
    if (agg_cancelled()) goto failed;
    ray_profile_tick("dense shared: reduced rows");
    ray_pool_dispatch(pool, agg_dense_shared_occupied, &c, slots);
    if (agg_cancelled()) goto failed;
    ray_profile_tick("dense shared: collected occupancy");
    ray_free_raw(c.occupied);
    route_stats.dense_strategy = AGG_DENSE_SHARED;
    route_stats.dense_tasks = ray_pool_total_workers(pool);
    route_stats.dense_local_slots = slots;
    return agg_dense_finish(key_cols, key_syms, ext, pool, vo, d, slots, c.states, c.first, plan, slots, 0, ef, 0);
failed:
    ray_free_raw(c.states); ray_free_raw(c.first); ray_free_raw(c.occupied);
    return ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
}

static ray_t* exec_group_v2_parallel_dense(
        ray_graph_t* g, ray_op_t* op, ray_t* tbl,
        ray_t** key_cols, int64_t* key_syms, ray_op_ext_t* ext, int64_t nrows,
        ray_pool_t* pool, const dense_plan_t* dp, uint32_t nw, agg_dense_strategy_t strategy,
        ray_t* sel, const int64_t* sel_prefix, int64_t n_sel,
        const ray_group_emit_filter_t* efp, int64_t group_limit) {
    uint32_t n_keys = ext->n_keys, n_aggs = ext->n_aggs;
    int64_t total_slots = dp->total_slots;

    /* Re-derive the AoS layout (same order as exec_group_v2). */
    agg_vo_t vo;
    if (!agg_vo_init(&vo, g, ext, tbl)) return ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
    const agg_vtable_t** vts = vo.vts; size_t* off = vo.off; size_t block = vo.block;

    agg_desc_t d;
    if (!agg_desc_init(&d, g, ext, tbl, key_cols)) { agg_vo_free(&vo); return ray_error(agg_cancelled() ? "cancel" : "oom", NULL); }
    const void** key_data = d.key_data;
    const void** val_data = d.val_data; const int8_t* val_types = d.val_types;
    const bool* val_hasnull = d.val_hasnull; const uint8_t* val_esz = d.val_esz;
    const void** val2_data = d.val2_data; const int8_t* val2_types = d.val2_types;
    const bool* val2_hasnull = d.val2_hasnull; const uint8_t* val2_esz = d.val2_esz;

    if (strategy == AGG_DENSE_SHARED) {
        ray_t* result = agg_dense_shared(key_cols, key_syms, ext, pool, dp, nrows, &vo, &d, efp);
        agg_vo_free(&vo); agg_desc_free(&d);
        return result;
    }
    if (strategy == AGG_DENSE_PARTITIONED) {
        ray_t* result = agg_dense_partitioned(key_cols, key_syms, ext, pool, dp, sel ? n_sel : nrows, &vo, &d, sel, efp);
        agg_vo_free(&vo); agg_desc_free(&d);
        return result;
    }
    route_stats.dense_strategy = AGG_DENSE_TASK_LOCAL;
    size_t metadata_bytes = ((size_t)nw * sizeof(agg_dense_local_t) + 63) & ~(size_t)63;
    size_t local_bytes = metadata_bytes + 63;
    for (uint32_t w = 0; w < nw; w++) {
        size_t slots = total_slots;
        size_t bytes = slots * (block + sizeof(int64_t)) + (slots + 63) / 64 * sizeof(uint64_t);
        local_bytes += (bytes + 63) & ~(size_t)63;
    }
    /* One query-owned allocation avoids concurrent allocator contention and
     * gives repeated queries one stable scratch size to reuse. Task slices are
     * cache-line aligned and written only by their owning task. */
    agg_dense_local_t* locals = ray_alloc_raw(local_bytes);
    if (!locals) { agg_vo_free(&vo); agg_desc_free(&d); return ray_error(agg_cancelled() ? "cancel" : "oom", NULL); }
    memset(locals, 0, metadata_bytes);
    char* cursor = (char*)(((uintptr_t)locals + metadata_bytes + 63) & ~(uintptr_t)63);
    for (uint32_t w = 0; w < nw; w++) {
        locals[w].slots = total_slots;
        /* Eager initialization amortizes when a task processes at least
         * two rows per possible slot. It preserves a tight row loop for
         * densely occupied domains; sparse worker slices remain lazy. */
        locals[w].eager = !sel && n_keys == 1 && total_slots <= nrows / nw / 2;
        if (locals[w].eager && total_slots > 1) {
            /* A clustered source range may visit only a small part of the
             * global domain. Sampling chooses initialization work only;
             * every actual row still initializes its state on the lazy path. */
            int64_t begin = nrows / nw * w;
            int64_t end = w + 1 == nw ? nrows : nrows / nw * (w + 1);
            int64_t low = total_slots, high = 0;
            for (int64_t sample = 0; sample < 64; sample++) {
                int64_t row = begin + (end - begin - 1) * sample / 63;
                int64_t slot = agg_dense_component(dp, 0, agg_read_key_i64(key_cols[0], key_data[0], row));
                if (slot < low) low = slot;
                if (slot > high) high = slot;
            }
            locals[w].eager = high - low + 1 >= total_slots - total_slots / 4;
        }
        size_t slots = locals[w].slots;
        size_t bytes = slots * (block + sizeof(int64_t)) + (slots + 63) / 64 * sizeof(uint64_t);
        locals[w].states = cursor;
        cursor += (bytes + 63) & ~(size_t)63;
    }
    agg_dense_merge_ctx_t merge_ctx = { .locals = locals, .slots = total_slots,
        .vts = vts, .off = off, .block = block, .n_aggs = n_aggs, .nw = nw };
    ray_profile_tick("dense: allocated slabs");

    agg_dense_ctx_t ctx = {
        .key_cols = key_cols, .key_data = key_data, .n_keys = n_keys, .dp = dp,
        .vts = vts, .off = off, .block = block, .n_aggs = n_aggs,
        .val_data = val_data, .val_types = val_types, .val_hasnull = val_hasnull, .val_esz = val_esz,
        .val2_data = val2_data, .val2_types = val2_types, .val2_hasnull = val2_hasnull, .val2_esz = val2_esz,
        .locals = locals,
        .task_rows = sel ? n_sel : nrows, .n_tasks = nw,
        .sel = sel, .sel_prefix = sel_prefix,
        .vd = { .n_aggs = n_aggs, .vts = vts, .off = off, .block = block,
                .val_data = val_data, .val_types = val_types, .val_hasnull = val_hasnull, .val_esz = val_esz,
                .val2_data = val2_data, .val2_types = val2_types, .val2_hasnull = val2_hasnull, .val2_esz = val2_esz },
    };
    /* Sel mode dispatches over the SELECTED-row space [0,n_sel); each worker
     * range maps to a segment span via sel_prefix.  Non-sel dispatches over rows. */
    ray_pool_dispatch_n(pool, agg_dense_task_fn, &ctx, nw);
    ray_profile_tick("dense: accumulated rows");

    for (uint32_t w = 0; w < nw; w++)
        if (locals[w].oom || agg_cancelled()) {
            for (uint32_t i = 0; i < nw; i++)
                agg_dense_local_destroy_states(&locals[i], vts, off, block, n_aggs);
            ray_free_raw(locals);
            agg_vo_free(&vo); agg_desc_free(&d);
            return ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
        }

    /* dispatch_n has joined every task. If cancellation skipped a task, the
     * check above returns before reading its bitmap; destroy also checks ready.
     * Cancellation after this point cannot uninitialize a completed slab. */
    /* ── Phase B: merge task slabs into a global slab in parallel ── */
    char*    gstates  = ray_alloc_raw((size_t)total_slots * block);
    int64_t* gfirst   = ray_alloc_raw((size_t)total_slots * sizeof(int64_t));
    if (!gstates || !gfirst) {
        ray_free_raw(gstates); ray_free_raw(gfirst);
        for (uint32_t i = 0; i < nw; i++)
            agg_dense_local_destroy_states(&locals[i], vts, off, block, n_aggs);
        ray_free_raw(locals);
        agg_vo_free(&vo); agg_desc_free(&d);
        return ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
    }
    merge_ctx.states = gstates; merge_ctx.first = gfirst;
    ray_pool_dispatch(pool, agg_dense_merge_fn, &merge_ctx, total_slots);
    ray_profile_tick("dense: merged slabs");
    if (agg_cancelled()) {
        for (uint32_t i = 0; i < nw; i++) {
            agg_dense_local_destroy_states(&locals[i], vts, off, block, n_aggs);
        }
        /* A cancelled merge may leave global slots uninitialized. Streaming
         * vtables own no separately allocated global state. */
        ray_free_raw(locals); ray_free_raw(gstates); ray_free_raw(gfirst);
        agg_vo_free(&vo); agg_desc_free(&d); return ray_error("cancel", NULL);
    }
    /* Worker buffered state has been merged into the global slab → its per-group
     * buffers are now redundant.  Destroy (free) them exactly once before
     * releasing the worker slabs.  No-op for all-streaming. */
    for (uint32_t i = 0; i < nw; i++)
        agg_dense_local_destroy_states(&locals[i], vts, off, block, n_aggs);
    ray_free_raw(locals);

    ray_t* result = agg_dense_finish(key_cols, key_syms, ext, pool, &vo, &d,
                                      total_slots, gstates, gfirst, NULL, 0, 0, efp, group_limit);
    agg_vo_free(&vo); agg_desc_free(&d);
    return result;
}

/* ══════════════════════════════════════════════════════════════════════
 * Parallel SMALL-HASH fallback for sparse int/SYM keys.
 *
 * This is the allocation-failure fallback for the deterministic radix route.
 * Its per-worker structures start at one group and grow from observed
 * cardinality, so a sparse, high-reduction input can still complete without a
 * sampled cardinality estimate or a cache/RAM-derived routing crossover.
 *
 * Streaming aggs only (selector gates ACC_BUFFERED → radix).  Per-worker hash:
 * open-addressing and growable on load-factor exceed; states + first_row grow
 * with the hash, never with N.
 * Phase A interleaves probe + accumulate in fixed CHUNK_ROWS chunks so the gid
 * buffer is chunk-sized, not nrows-sized.  Phase B merges per-worker hashes into
 * a global hash (≤ groups×nworkers entries — cheap).  Phase C emits in build
 * order (unspecified output order): gather key columns at the representative
 * row per group, finalize each group's streaming state.
 * ══════════════════════════════════════════════════════════════════════ */

/* Per-worker (and global merge) GROWABLE small-hash group table: open-addressing
 * hash on the tuple-hash → local gid; AoS per-group state blocks of `block`
 * bytes.  Sized to cardinality (not N) and grown by rehash on load-factor. */
typedef struct {
    int32_t* ht;          /* [htcap] slot -> local gid, -1 empty */
    int64_t  htcap;
    uint64_t htmask;
    int64_t* first_row;   /* [cap] representative (MIN) row idx per local group */
    char*    states;      /* [cap*block] AoS group state */
    int64_t  ng, cap;
    size_t   block;
    int      oom;
} agg_sh_t;

static int agg_sh_init(agg_sh_t* sh, int64_t cap, size_t block) {
    if (cap < 1) cap = 1;
    int64_t htcap = 2;
    while (htcap < cap * 2) htcap <<= 1;   /* <0.5 load at full cap */
    sh->ng = 0; sh->cap = cap; sh->block = block; sh->oom = 0;
    sh->htcap = htcap; sh->htmask = (uint64_t)htcap - 1;
    sh->ht        = ray_alloc_raw((size_t)htcap * sizeof(int32_t));
    sh->first_row = ray_alloc_raw((size_t)cap * sizeof(int64_t));
    sh->states    = ray_alloc_raw((size_t)cap * block);
    if (!sh->ht || !sh->first_row || !sh->states) { sh->oom = 1; return -1; }
    for (int64_t i = 0; i < htcap; i++) sh->ht[i] = -1;
    return 0;
}

static void agg_sh_destroy(agg_sh_t* sh) {
    ray_free_raw(sh->ht); ray_free_raw(sh->first_row); ray_free_raw(sh->states);
    sh->ht = NULL; sh->first_row = NULL; sh->states = NULL;
}

/* Grow capacity (states + first_row) when ng would exceed cap.  Doubles cap. */
static int agg_sh_grow_cap(agg_sh_t* sh) {
    int64_t nc = sh->cap * 2;
    int64_t* nfr = ray_realloc_raw(sh->first_row, (size_t)nc * sizeof(int64_t));
    if (!nfr) { sh->oom = 1; return -1; }
    sh->first_row = nfr;
    char* nst = ray_realloc_raw(sh->states, (size_t)nc * sh->block);
    if (!nst) { sh->oom = 1; return -1; }
    sh->states = nst;
    sh->cap = nc;
    return 0;
}

/* Rehash the hash table to a larger htcap (keeps load factor low).  The gid set
 * is unchanged; we recompute each group's slot from its representative row's
 * tuple hash.  n_keys is uint32_t (unbounded), same contract as
 * agg_tuple_hash. */
static int agg_sh_grow_ht(agg_sh_t* sh, ray_t** key_cols, const void** key_data,
                          uint32_t n_keys) {
    int64_t nc = sh->htcap * 2;
    int32_t* nht = ray_alloc_raw((size_t)nc * sizeof(int32_t));
    if (!nht) { sh->oom = 1; return -1; }
    uint64_t nmask = (uint64_t)nc - 1;
    for (int64_t i = 0; i < nc; i++) nht[i] = -1;
    for (int64_t g = 0; g < sh->ng; g++) {
        uint64_t h = agg_tuple_hash(key_cols, key_data, n_keys, sh->first_row[g]);
        uint64_t slot = h & nmask;
        while (nht[slot] >= 0) slot = (slot + 1) & nmask;
        nht[slot] = (int32_t)g;
    }
    ray_free_raw(sh->ht);
    sh->ht = nht; sh->htcap = nc; sh->htmask = nmask;
    return 0;
}

#define AGG_SH_CHUNK 4096
static ray_t* exec_group_v2_parallel_radix(
        ray_graph_t* g, ray_op_t* op, ray_t* tbl, int64_t nrows,
        ray_t** key_cols, int64_t* key_syms,
        const agg_vtable_t** vts, const size_t* off, size_t block,
        ray_t* sel, const int64_t* sel_prefix, int64_t n_sel,
        int64_t group_limit, const ray_group_emit_filter_t* efp);

typedef struct {
    ray_t**            key_cols;
    const void**       key_data;
    uint32_t           n_keys;
    const agg_vtable_t** vts;
    const size_t*      off;
    size_t             block;
    uint32_t           n_aggs;
    const void**       val_data; const int8_t* val_types; const bool* val_hasnull; const uint8_t* val_esz;
    const void**       val2_data; const int8_t* val2_types; const bool* val2_hasnull; const uint8_t* val2_esz;
    agg_sh_t*          locals;     /* [nw] */
    /* Sel-mode (pushed WHERE filter): chunk-iterate selected rows of [0,n_sel). */
    ray_t*             sel;
    const int64_t*     sel_prefix;
    agg_valdesc_t      vd;
} agg_sh_ctx_t;

/* Probe-or-insert key tuple at row r in sh; returns gid (>=0) or -1 on oom.
 * n_keys and n_aggs are both uint32_t (unbounded). */
static inline int32_t agg_sh_find_or_insert(
        agg_sh_t* sh, ray_t** key_cols, const void** key_data, uint32_t n_keys,
        const agg_vtable_t** vts, const size_t* off, uint32_t n_aggs, int64_t r) {
    uint64_t h = agg_tuple_hash(key_cols, key_data, n_keys, r);
    uint64_t slot = h & sh->htmask;
    for (;;) {
        int32_t gp = sh->ht[slot];
        if (gp < 0) {                       /* new group */
            if (sh->ng == sh->cap && agg_sh_grow_cap(sh) != 0) return -1;
            /* Keep hash load factor < 0.5: grow + rehash before insert. */
            if ((sh->ng + 1) * 2 > sh->htcap) {
                if (agg_sh_grow_ht(sh, key_cols, key_data, n_keys) != 0) return -1;
                slot = h & sh->htmask;
                while (sh->ht[slot] >= 0) slot = (slot + 1) & sh->htmask;
            }
            int32_t g = (int32_t)sh->ng;
            sh->ht[slot] = g;
            sh->first_row[g] = r;
            for (uint32_t a = 0; a < n_aggs; a++)
                vts[a]->init(sh->states + (size_t)g * sh->block + off[a]);
            sh->ng++;
            return g;
        }
        if (agg_tuple_eq(key_cols, key_data, n_keys, r, sh->first_row[gp])) {
            if (r < sh->first_row[gp]) sh->first_row[gp] = r;   /* MIN representative */
            return gp;
        }
        slot = (slot + 1) & sh->htmask;
    }
}

/* Phase A: per-worker small-hash group + accumulate over chunk [start,end),
 * processed in fixed-size sub-chunks so the gid buffer is O(CHUNK), not O(N). */
static void agg_sh_phaseA_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    agg_sh_ctx_t* c = (agg_sh_ctx_t*)vctx;
    agg_sh_t* sh = &c->locals[wid];
    if (sh->oom) return;
    uint32_t gid[AGG_SH_CHUNK];

    /* ── Sel mode: chunk-iterate the SELECTED rows in [start,end) (selected-row
     * space), decoding ORIGINAL rows, probing the per-worker hash, gathering
     * agg values per chunk into reused scratch.  No full index/compact alloc. */
    if (c->sel) {
        int64_t rows[AGG_SEL_CHUNK];
        uint32_t sgid[AGG_SEL_CHUNK];
        agg_sel_scratch_t sc;
        if (!agg_sel_scratch_init(&sc, c->n_aggs)) { sh->oom = 1; return; }
        agg_sel_cursor_t cur;
        agg_sel_cursor_init(&cur, c->sel, c->sel_prefix, start, end);
        int64_t cn;
        while ((cn = agg_sel_cursor_next(&cur, rows)) > 0) {
            for (int64_t i = 0; i < cn; i++) {
                int32_t g = agg_sh_find_or_insert(sh, c->key_cols, c->key_data, c->n_keys,
                                                  c->vts, c->off, c->n_aggs, rows[i]);
                if (g < 0) { sh->oom = 1; agg_sel_scratch_free(&sc); return; }
                sgid[i] = (uint32_t)g;
            }
            if (agg_sel_accum_chunk(&c->vd, &sc, sh->states, sgid, rows, cn) != 0) {
                sh->oom = 1; agg_sel_scratch_free(&sc); return;
            }
        }
        agg_sel_scratch_free(&sc);
        return;
    }

    for (int64_t cs = start; cs < end; cs += AGG_SH_CHUNK) {
        int64_t ce = cs + AGG_SH_CHUNK; if (ce > end) ce = end;
        int64_t n = ce - cs;
        for (int64_t r = cs; r < ce; r++) {
            int32_t g = agg_sh_find_or_insert(sh, c->key_cols, c->key_data, c->n_keys,
                                              c->vts, c->off, c->n_aggs, r);
            if (g < 0) { sh->oom = 1; return; }
            gid[r - cs] = (uint32_t)g;
        }
        for (uint32_t a = 0; a < c->n_aggs; a++) {
            if (c->vts[a]->update_batch2) {             /* binary agg (pearson) */
                const void* vx = (const char*)c->val_data[a]  + (size_t)cs * c->val_esz[a];
                const void* vy = (const char*)c->val2_data[a] + (size_t)cs * c->val2_esz[a];
                ray_valid_t valid_x = { vx, c->val_types[a],  c->val_hasnull[a] };
                ray_valid_t valid_y = { vy, c->val2_types[a], c->val2_hasnull[a] };
                c->vts[a]->update_batch2(sh->states + c->off[a], c->block, gid,
                                         vx, vy, &valid_x, &valid_y, n, NULL);
            } else {
                const void* vals = (c->val_data[a])
                    ? (const void*)((const char*)c->val_data[a] + (size_t)cs * c->val_esz[a])
                    : NULL;
                ray_valid_t valid = { vals, c->val_types[a],
                                      c->val_data[a] ? c->val_hasnull[a] : false };
                c->vts[a]->update_batch(sh->states + c->off[a], c->block,
                                        gid, vals, &valid, n, NULL);
            }
        }
    }
}

/* Parallel small-hash fallback.  Precondition: int/SYM keys and streaming
 * accumulators. Hashes start at one group and grow from observed cardinality. */
static ray_t* exec_group_v2_parallel_smallhash(
        ray_graph_t* g, ray_op_t* op, ray_t* tbl, int64_t nrows,
        ray_t** key_cols, int64_t* key_syms,
        const agg_vtable_t** vts, const size_t* off, size_t block,
        ray_t* sel, const int64_t* sel_prefix, int64_t n_sel) {
    agg_route_record(AGG_ROUTE_V2_SMALLHASH);
    ray_op_ext_t* ext = find_ext(g, op->id);
    uint32_t n_keys = ext->n_keys, n_aggs = ext->n_aggs;
    ray_pool_t* pool = ray_pool_get();
    uint32_t nw = ray_pool_total_workers(pool);

    agg_desc_t d;
    if (!agg_desc_init(&d, g, ext, tbl, key_cols)) return ray_error("oom", NULL);
    const void** key_data = d.key_data;
    const void** val_data = d.val_data; const int8_t* val_types = d.val_types;
    const bool* val_hasnull = d.val_hasnull; const uint8_t* val_esz = d.val_esz;
    const void** val2_data = d.val2_data; const int8_t* val2_types = d.val2_types;
    const bool* val2_hasnull = d.val2_hasnull; const uint8_t* val2_esz = d.val2_esz;
    const int64_t* agg_syms = d.agg_syms;

    agg_sh_t* locals = ray_calloc_raw((size_t)((size_t)nw) * (sizeof(agg_sh_t)));
    if (!locals) { agg_desc_free(&d); return ray_error("oom", NULL); }
    int alloc_oom = 0;
    for (uint32_t w = 0; w < nw; w++)
        if (agg_sh_init(&locals[w], 1, block) != 0) { alloc_oom = 1; break; }
    if (alloc_oom) {
        for (uint32_t w = 0; w < nw; w++) agg_sh_destroy(&locals[w]);
        ray_free_raw(locals);
        agg_desc_free(&d);
        return ray_error("oom", NULL);
    }

    agg_sh_ctx_t ctx = {
        .key_cols = key_cols, .key_data = key_data, .n_keys = n_keys,
        .vts = vts, .off = off, .block = block, .n_aggs = n_aggs,
        .val_data = val_data, .val_types = val_types,
        .val_hasnull = val_hasnull, .val_esz = val_esz,
        .val2_data = val2_data, .val2_types = val2_types,
        .val2_hasnull = val2_hasnull, .val2_esz = val2_esz,
        .locals = locals,
        .sel = sel, .sel_prefix = sel_prefix,
        .vd = { .n_aggs = n_aggs, .vts = vts, .off = off, .block = block,
                .val_data = val_data, .val_types = val_types, .val_hasnull = val_hasnull, .val_esz = val_esz,
                .val2_data = val2_data, .val2_types = val2_types, .val2_hasnull = val2_hasnull, .val2_esz = val2_esz },
    };
    ray_pool_dispatch(pool, agg_sh_phaseA_fn, &ctx, sel ? n_sel : nrows);

    for (uint32_t w = 0; w < nw; w++)
        if (locals[w].oom) {
            for (uint32_t i = 0; i < nw; i++) agg_sh_destroy(&locals[i]);
            ray_free_raw(locals);
            agg_desc_free(&d);
            return ray_error("oom", NULL);
        }

    /* ── Phase B: merge per-worker hashes into a global hash (serial). ──
     * Entries ≤ groups×nworkers, so size the global hash to that bound. */
    int64_t gcap = 0;
    for (uint32_t w = 0; w < nw; w++) gcap += locals[w].ng;
    agg_sh_t gt = {0};
    if (agg_sh_init(&gt, gcap, block) != 0) {
        agg_sh_destroy(&gt);
        for (uint32_t i = 0; i < nw; i++) agg_sh_destroy(&locals[i]);
        ray_free_raw(locals);
        agg_desc_free(&d);
        return ray_error("oom", NULL);
    }
    for (uint32_t w = 0; w < nw; w++) {
        agg_sh_t* loc = &locals[w];
        for (int64_t lg = 0; lg < loc->ng; lg++) {
            int64_t fr = loc->first_row[lg];
            int32_t gg = agg_sh_find_or_insert(&gt, key_cols, key_data, n_keys,
                                               vts, off, n_aggs, fr);
            if (gg < 0) {
                agg_sh_destroy(&gt);
                for (uint32_t i = 0; i < nw; i++) agg_sh_destroy(&locals[i]);
            ray_free_raw(locals);
                agg_desc_free(&d);
                return ray_error("oom", NULL);
            }
            for (uint32_t a = 0; a < n_aggs; a++)
                vts[a]->merge(gt.states + (size_t)gg * block + off[a],
                              loc->states + (size_t)lg * block + off[a], NULL);
        }
    }
    int64_t ng = gt.ng;
    for (uint32_t i = 0; i < nw; i++) agg_sh_destroy(&locals[i]);
    ray_free_raw(locals);

    /* ── Phase C: emit key + agg columns in build order (unspecified order). ──
     * gt.first_row[g] is the representative row for the scattered key gather —
     * trivially cheap for the few groups this path handles. */
    ray_t* result = ray_table_new(n_keys + n_aggs);
    if (!result || RAY_IS_ERR(result)) {
        agg_sh_destroy(&gt); agg_desc_free(&d);
        return result ? result : ray_error("oom", NULL);
    }
    for (uint32_t k = 0; k < n_keys; k++) {
        ray_t* kc = ray_group_gather(key_cols[k], gt.first_row, ng);
        if (!kc || RAY_IS_ERR(kc)) {
            agg_sh_destroy(&gt); agg_desc_free(&d); ray_release(result);
            return kc ? kc : ray_error("oom", NULL);
        }
        result = ray_table_add_col(result, key_syms[k], kc);
        ray_release(kc);
    }
    for (uint32_t a = 0; a < n_aggs; a++) {
        ray_t* out = ray_vec_new(vts[a]->out_type, ng);   /* streaming → never LIST */
        if (!out || RAY_IS_ERR(out)) {
            agg_sh_destroy(&gt); agg_desc_free(&d); ray_release(result);
            return out ? out : ray_error("oom", NULL);
        }
        out->len = ng;
        int64_t kparam = (ext->agg_k ? ext->agg_k[a] : 0);
        for (int64_t i = 0; i < ng; i++) {
            if (agg_finalize_value(vts[a], gt.states + (size_t)i * block + off[a], out, i, kparam))
                out->attrs |= RAY_ATTR_HAS_NULLS;
        }
        int64_t agg_name = agg_result_col_name(agg_syms[a], ext->agg_ops[a]);
        result = ray_table_add_col(result, agg_name, out);
        ray_release(out);
    }
    agg_sh_destroy(&gt);   /* streaming-only: no per-group destroy lifecycle */
    agg_desc_free(&d);
    return result;
}

/* ══════════════════════════════════════════════════════════════════════
 * Parallel RADIX group-by (high-card int/SYM keys, ACC_STREAMING aggs only).
 *
 * Partition rows by key-hash into a worker-derived power-of-two partition set:
 * every row
 * whose tuple-hash maps to partition p lands in p across ALL workers (same key
 * → same hash → same partition).  Partitions therefore hold DISJOINT key sets,
 * so each can be grouped+accumulated fully in parallel with NO cross-partition
 * merge — eliminating the serial Phase-B bottleneck of the hash path.
 *
 *   Phase 1 (parallel over rows): scatter each row's INDEX into a per-(worker,
 *            partition) growable int64 buffer keyed by the hash partition.
 *   Phase 2 (parallel over partitions): gather all workers' index buffers for
 *            partition p; build a small open-addressing hash over those rows
 *            (keys disjoint from other partitions); assign partition-local gids
 *            (first_row = MIN row), init each agg state on first key sight,
 *            batch-update each agg from a gathered value column.
 *   Phase 3: ngroups = Σ partition group counts; emit groups in build order
 *            (partition order, then partition-local group order — output order
 *            is unspecified, so no global sort), assembling {key cols, agg cols}.
 *            The per-group representative row (first_row) gathers the key cols.
 *
 * Streaming aggs only (selector guarantees it) → no buffered destroy lifecycle.
 * ══════════════════════════════════════════════════════════════════════ */

static uint32_t agg_radix_part_count(uint32_t nworkers, int64_t nrows) {
    uint32_t n = 1;
    uint64_t rows = nrows > 0 ? (uint64_t)nrows : 1;
    while ((n < nworkers ||
            (uint64_t)n < rows / n + (rows % n != 0)) &&
           n <= UINT32_MAX / 2)
        n <<= 1;
    return n;
}

/* Growable contiguous PAYLOAD buffer (per worker, per partition).  Phase 1
 * scatters one fixed-size record per row instead of a bare row index: the
 * record packs [n_keys×int64 widened keys][agg input value(s) at native esz]
 * [row_idx int64].  Phase 2 then groups+accumulates each partition by walking
 * its records SEQUENTIALLY — hash/equality compare the contiguous packed keys
 * and accumulation reads values from the contiguous record — trading a one-time
 * scatter cost for cache-friendly Phase-2 reads (the memory-bound hot spot was
 * agg_tuple_eq re-reading scattered key columns on every distinct insert). */
typedef struct { char* buf; uint32_t n, cap; } agg_pay_buf_t;  /* n = #records */

/* Reserve room for one more record of `rec` bytes; returns dest ptr or NULL. */
static char* agg_pay_reserve(agg_pay_buf_t* b, size_t rec, uint32_t cap0) {
    if (b->n == b->cap) {
        if (b->cap > UINT32_MAX / 2) return NULL;
        /* First allocation jumps straight to the caller's expected row count
         * (uniform-hash estimate).  Growing 1->2->4->... instead re-copies the
         * whole payload ~2x across tens of thousands of per-(worker,partition)
         * buffers — the memmove was 23% of ClickBench q17. */
        uint32_t nc = b->cap ? b->cap * 2 : (cap0 ? cap0 : 1);
        char* nb = ray_realloc_raw(b->buf, (size_t)nc * rec);
        if (!nb) return NULL;
        b->buf = nb; b->cap = nc;
    }
    return b->buf + (size_t)b->n++ * rec;
}

/* Per-partition result slot (filled by Phase 2). */
typedef struct {
    char*    states;     /* [ng * block] AoS group state (every group init'd) */
    int64_t* first_row;  /* [ng] MIN logical input position per local group */
    int64_t* keys;       /* [ng * n_keys] packed keys (build order) for the
                          * representative record of each group — Phase 3 emits
                          * the key columns by un-packing these SEQUENTIALLY
                          * (cache-friendly) instead of gathering scattered
                          * original-column rows at first_row[]. */
    int64_t  ng;
    int      oom;
} agg_radix_part_t;

/* Run vt->destroy on every init'd per-group buffered state across all
 * partitions' slabs, then free each partition's states/first_row.  Each
 * partition's `ng` groups are all init'd contiguously in [0, ng) (no sparse
 * slots), so destroy walks exactly the live states.  No-op destroy hook for
 * streaming accs.  Must be called exactly once per partition slab (paired with
 * the free here); buffered states must be finalized before this runs. */
static void agg_radix_parts_destroy(agg_radix_part_t* parts, uint32_t nparts,
                                    const agg_vtable_t** vts, const size_t* off,
                                    size_t block, uint32_t n_aggs) {
    for (uint32_t p = 0; p < nparts; p++) {
        char* states = parts[p].states;
        if (states)
            for (uint32_t a = 0; a < n_aggs; a++)
                if (vts[a]->destroy)
                    for (int64_t gg = 0; gg < parts[p].ng; gg++)
                        vts[a]->destroy(states + (size_t)gg * block + off[a]);
        ray_free_raw(parts[p].states);
        ray_free_raw(parts[p].first_row);
        ray_free_raw(parts[p].keys);
    }
}

/* Allocate an EMPTY result key column of src_col's type/width for the radix
 * path.  agg_key_emit_range below fills it from the per-partition packed `keys`
 * buffers in stable first-seen order; the two halves are split so one dispatch
 * can fill every key column at once.
 *
 * agg_read_key_i64 widened each key into int64 (sign-extend for signed types,
 * zero-extend for U8/BOOL/SYM intern ids); write_col_i64 is its exact inverse,
 * so the round-trip stores byte-identical payload to what ray_group_gather's
 * raw memcpy of the original column produced — for SYM it routes through
 * ray_write_sym at the matching width, and the domain is adopted from src_col
 * just like ray_group_gather.  Caller owns the returned column.
 * Keys are admitted only as fixed-width integer/temporal/SYM columns
 * (agg_v2_can_handle) — there is no STR/variable-width emit path here — but the
 * key COUNT is unbounded on the radix strategy (only dense direct-index routing
 * caps it at 16), so nothing downstream may size an array by a 16 assumption. */
static ray_t* agg_unpack_key_col_new(ray_t* src_col, int64_t n) {
    ray_t* out = col_vec_new(src_col, n);
    if (!out || RAY_IS_ERR(out)) return out;
    if (out->type == RAY_SYM)
        ray_sym_vec_adopt_domain(out, sym_domain_rep(src_col));
    out->len = n;
    if (src_col->attrs & RAY_ATTR_HAS_NULLS) out->attrs |= RAY_ATTR_HAS_NULLS;
    return out;
}

/* Un-pack context for the key emit: all n_keys destination columns at once, so
 * one dispatch covers the whole key side (mirrors agg_radix_fin_ctx_t, which
 * carries every agg's output column through one finalize dispatch). */
typedef struct {
    ray_t* const*            key_cols;   /* [n_keys] source columns (type/attrs) */
    ray_t* const*            outs;       /* [n_keys] pre-sized destinations */
    const agg_radix_part_t*  parts;
    const agg_radix_order_t* pairs;      /* [n], stable first-seen order */
    uint32_t                 n_keys;
} agg_key_emit_ctx_t;

/* Fill output rows [start,end) of EVERY key column from the packed per-group
 * keys.  Row i of every column is written by exactly one caller, so a parallel
 * dispatch over disjoint [start,end) ranges is race-free: write_col_i64 is a
 * pure payload store at index i (down to a 1-byte store for BOOL/U8/SYM_W8 —
 * still disjoint, there is no bit-packed representation here) and nothing in
 * this loop touches out->attrs or any other shared header field.  The nulls
 * flag is not derived per row at all: it is copied wholesale from src_col by
 * agg_unpack_key_col_new before the dispatch, so no attrs fold is needed. */
RAY_INLINE void agg_key_emit_range(const agg_key_emit_ctx_t* c,
                                   int64_t start, int64_t end) {
    uint32_t n_keys = c->n_keys;
    for (uint32_t k = 0; k < n_keys; k++) {
        ray_t* out = c->outs[k];
        void* dst = ray_data(out);
        int8_t type = c->key_cols[k]->type;
        uint8_t attrs = c->key_cols[k]->attrs;
        for (int64_t i = start; i < end; i++) {
            uint32_t p  = (uint32_t)(c->pairs[i].idx >> 32);
            uint32_t gg = (uint32_t)c->pairs[i].idx;
            write_col_i64(dst, i,
                c->parts[p].keys[(size_t)gg * n_keys + k], type, attrs);
        }
    }
}

static void agg_key_emit_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_key_emit_range((const agg_key_emit_ctx_t*)vctx, start, end);
}

typedef struct {
    ray_t**             key_cols;
    const void**        key_data;
    uint32_t            n_keys;
    const agg_vtable_t** vts;
    const size_t*       off;
    size_t              block;
    uint32_t            n_aggs;
    const void**        val_data; const int8_t* val_types; const bool* val_hasnull; const uint8_t* val_esz;
    const void**        val2_data; const int8_t* val2_types; const bool* val2_hasnull; const uint8_t* val2_esz;
    uint32_t            nw;
    uint32_t            n_parts;
    uint32_t            part_bits;  /* log2(n_parts): hash bits consumed by
                                     * partition selection; phase 2 shifts
                                     * them out before slot indexing */
    uint32_t            pay_prime;  /* expected rows per (worker,partition)
                                     * buffer — first-allocation capacity */
    agg_pay_buf_t*      bufs;       /* [nw * n_parts] payload records */
    agg_radix_part_t*   parts;      /* [n_parts] */
    int                 phase1_oom; /* set by any Phase-1 worker on push failure */
    /* Per-row payload record layout (bytes), computed once by the caller:
     *   [0 .. n_keys*8)         packed keys (each widened via agg_read_key_i64)
     *   [val_off[a] ..]         agg a's input value at native esz (if val_data[a])
     *   [val2_off[a] ..]        agg a's 2nd input (pearson) at native esz
     *   [row_off ..]            int64 logical input position */
    size_t              rec;        /* total record size, 8-aligned */
    const size_t*       val_off;    /* [n_aggs], carved by the caller */
    const size_t*       val2_off;   /* [n_aggs], carved by the caller */
    size_t              row_off;
    /* Stable group ordering records one logical input position per payload. */
    bool                needs_row;
    /* Sel-mode (pushed WHERE filter): when sel != NULL, scatter the SELECTED
     * rows of [0,n_sel) (decoded to ORIGINAL row indices) instead of [start,end).
     * The packed record stores the selected-row ordinal, while column reads use
     * the decoded original row. */
    ray_t*              sel;
    const int64_t*      sel_prefix;
    /* Per-WORKER key-staging row: [nw * n_keys] int64.  The scatter reads all
     * keys into its worker's slice to compute the partition hash, THEN copies
     * them into the reserved record (the record's partition is not known until
     * the hash is computed, so the keys can't be written straight to it).  One
     * carve for the whole dispatch (caller-owned) — never a per-row alloc. */
    int64_t*            kv_scratch;
} agg_radix_ctx_t;

/* Avalanche finalizer shared by the radix scatter (partition selection) and
 * phase 2 (partition-local slot index).  Both consume DISJOINT bit ranges of
 * the same per-row hash — partition from the low log2(n_parts) bits, slot
 * from the bits above them — so the raw FNV must be finalized (fmix64, same
 * rationale as agg_tuple_hash) and the two sides must compute IDENTICAL
 * values for the shift-out split to hold. */
static inline uint64_t agg_radix_fmix64(uint64_t h) {
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

/* Scatter one ORIGINAL row r's packed payload record into the worker's per-
 * partition buffer.  Returns 0 or -1 on push OOM (sets phase1_oom). */
static inline int agg_radix_scatter_one(agg_radix_ctx_t* c, agg_pay_buf_t* my,
                                        int64_t* kv, int64_t r,
                                        int64_t input_order) {
    uint32_t n_keys = c->n_keys, n_aggs = c->n_aggs;
    /* kv: this worker's key-staging slice (c->kv_scratch + wid*n_keys), sized
     * for any key count — no fixed [16] cap. */
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t k = 0; k < n_keys; k++) {
        int64_t v = agg_read_key_i64(c->key_cols[k], c->key_data[k], r);
        kv[k] = v;
        h ^= (uint64_t)v; h *= 1099511628211ULL;
    }
    h = agg_radix_fmix64(h);
    uint32_t p = (uint32_t)(h & (c->n_parts - 1));
    char* rec = agg_pay_reserve(&my[p], c->rec, c->pay_prime);
    if (!rec) { c->phase1_oom = 1; return -1; }
    int64_t* kdst = (int64_t*)rec;
    for (uint32_t k = 0; k < n_keys; k++) kdst[k] = kv[k];
    for (uint32_t a = 0; a < n_aggs; a++) {
        if (c->val_data[a]) {
            uint8_t ez = c->val_esz[a];
            memcpy(rec + c->val_off[a], (const char*)c->val_data[a] + (size_t)r * ez, ez);
        }
        if (c->val2_data[a]) {
            uint8_t ez2 = c->val2_esz[a];
            memcpy(rec + c->val2_off[a], (const char*)c->val2_data[a] + (size_t)r * ez2, ez2);
        }
    }
    if (c->needs_row)
        memcpy(rec + c->row_off, &input_order, sizeof(input_order));
    return 0;
}

/* Scatter this worker's [start,end) rows, staging each row's keys through kv.
 * static inline so each caller below inlines its own copy: when the stack
 * array is passed, kv's alloca provenance reaches the per-row kv[k]=v stage
 * unmerged and the compiler scalarizes/register-promotes it (parent codegen);
 * a heap slice stays compiler-opaque (may alias key_data/rec/bufs).
 * RAY_INLINE (always_inline) so both call sites below get their own copy —
 * an out-of-line body would merge the two provenances into one pointer param
 * and re-pessimize the stack path. */
RAY_INLINE void agg_radix_scatter_range(agg_radix_ctx_t* c, agg_pay_buf_t* my,
                                        int64_t* kv, int64_t start, int64_t end) {
    if (c->sel) {
        /* Chunk-decode this worker's slice of selected rows; scatter each. */
        int64_t rows[AGG_SEL_CHUNK];
        agg_sel_cursor_t cur;
        agg_sel_cursor_init(&cur, c->sel, c->sel_prefix, start, end);
        int64_t input_order = start;
        int64_t cn;
        while ((cn = agg_sel_cursor_next(&cur, rows)) > 0)
            for (int64_t i = 0; i < cn; i++)
                if (agg_radix_scatter_one(c, my, kv, rows[i],
                                          input_order++) != 0) return;
        return;
    }
    for (int64_t r = start; r < end; r++)
        if (agg_radix_scatter_one(c, my, kv, r, r) != 0) return;
}

/* Phase 1: scatter one packed payload record per row into per-(worker,
 * partition) contiguous buffers, keyed by the tuple-hash partition. */
static void agg_radix_scatter_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    agg_radix_ctx_t* c = (agg_radix_ctx_t*)vctx;
    agg_pay_buf_t* my = &c->bufs[(size_t)wid * c->n_parts];
    /* Key-staging row for this worker.  The common bounded case (n_keys <= 16)
     * stages through a STACK array: a compiler-opaque heap slice defeated the
     * scalarization/register-promotion of the per-row kv[k]=v stage (flat
     * retired instructions, IPC 0.71->0.58, scatter self-time tripled — see
     * task-9-rca.md).  Branch ONCE here (not per row) so each inlined copy of
     * agg_radix_scatter_range gets its argument's provenance unmerged: the
     * stack call restores parent codegen (and drops the secondary false-sharing
     * term); wide keys (>16) fall back to the per-worker heap slice. */
    if (c->n_keys <= 16) {
        int64_t kv_stk[16];
        agg_radix_scatter_range(c, my, kv_stk, start, end);
    } else {
        agg_radix_scatter_range(c, my, &c->kv_scratch[(size_t)wid * c->n_keys], start, end);
    }
}

/* Phase 2: group + accumulate one partition by walking its packed payload
 * records SEQUENTIALLY.  Keys here are disjoint from every other partition, so
 * a partition-local open-addressing hash suffices.  Hash/equality compare the
 * contiguous packed keys at the head of each record (no scattered key-column
 * re-reads), and each agg's dense value buffer is gathered straight from the
 * records in the same sequential pass.  Keys are disjoint from other
 * partitions so the partition-local open-addressing hash suffices. */
static void agg_radix_group_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_radix_ctx_t* c = (agg_radix_ctx_t*)vctx;
    uint32_t n_keys = c->n_keys, n_aggs = c->n_aggs;
    size_t key_bytes = (size_t)n_keys * 8;
    for (int64_t pi = start; pi < end; pi++) {
        uint32_t p = (uint32_t)pi;
        agg_radix_part_t* pr = &c->parts[p];

        /* Total rows hashing to p across all workers. */
        int64_t total = 0;
        for (uint32_t w = 0; w < c->nw; w++)
            total += c->bufs[(size_t)w * c->n_parts + p].n;
        if (total == 0) continue;

        /* Open-addressing hash sized to next_pow2(2*total). */
        int64_t htcap = 16;
        while (htcap < total * 2) htcap <<= 1;
        uint64_t htmask = (uint64_t)htcap - 1;
        int32_t* ht        = ray_alloc_raw((size_t)htcap * sizeof(int32_t));
        uint8_t* ht_salt   = ray_alloc_raw((size_t)htcap);  /* parallel salt fingerprint per slot */
        /* Group-state arrays sized to the distinct-group count (grow-on-
         * demand) instead of `total` rows. gid/gv/gy stay row-sized. */
        int64_t gcap = total < 256 ? total : 256;
        if (gcap < 16) gcap = 16;
        /* first_row is only populated when a row-dependent agg needs it. */
        int64_t* first_row = c->needs_row ? ray_alloc_raw((size_t)gcap * sizeof(int64_t)) : NULL;
        char*    states    = ray_alloc_raw((size_t)gcap * c->block);
        const int64_t** keyp = ray_alloc_raw((size_t)gcap * sizeof(int64_t*)); /* gid -> packed keys */
        uint32_t* gid      = ray_alloc_raw((size_t)total * sizeof(uint32_t));/* row i -> local gid */
        /* Persist each group's packed keys (build order) so Phase 3 emits the
         * key columns by sequential un-pack rather than scattered gather. */
        int64_t* gkeys     = ray_alloc_raw((size_t)gcap * (size_t)(n_keys ? n_keys : 1) * sizeof(int64_t));
        if (!ht || !ht_salt || (c->needs_row && !first_row) || !states || !keyp || !gid || !gkeys) {
            ray_free_raw(ht); ray_free_raw(ht_salt); ray_free_raw(first_row); ray_free_raw(states); ray_free_raw(keyp); ray_free_raw(gid); ray_free_raw(gkeys);
            pr->oom = 1; return;
        }
        for (int64_t i = 0; i < htcap; i++) ht[i] = -1;

        /* Per-agg dense value buffers, gathered contiguously from the records.
         * gv/gy are per-partition scratch (this worker only) — carved here, not
         * the shared descriptor, so per-invocation allocation is correct. */
        char** gv = ray_calloc_raw((size_t)(n_aggs ? n_aggs : 1) * sizeof(char*));
        char** gy = ray_calloc_raw((size_t)(n_aggs ? n_aggs : 1) * sizeof(char*));
        if (!gv || !gy) {
            ray_free_raw(gv); ray_free_raw(gy);
            ray_free_raw(ht); ray_free_raw(ht_salt); ray_free_raw(first_row); ray_free_raw(states); ray_free_raw(keyp); ray_free_raw(gid); ray_free_raw(gkeys);
            pr->oom = 1; return;
        }
        for (uint32_t a = 0; a < n_aggs; a++) {
            if (c->val_data[a]) {
                uint8_t ez = c->val_esz[a];
                gv[a] = ray_alloc_raw((size_t)total * (ez ? ez : 1));
                if (!gv[a]) goto oom;
            }
            if (c->val2_data[a]) {
                uint8_t ez2 = c->val2_esz[a];
                gy[a] = ray_alloc_raw((size_t)total * (ez2 ? ez2 : 1));
                if (!gy[a]) goto oom;
            }
        }

        int64_t ng = 0, ri = 0;
        for (uint32_t w = 0; w < c->nw; w++) {
            agg_pay_buf_t* b = &c->bufs[(size_t)w * c->n_parts + p];
            const char* rec = b->buf;
            for (uint32_t i = 0; i < b->n; i++, rec += c->rec) {
                const int64_t* keys = (const int64_t*)rec;
                int64_t r = 0;
                if (c->needs_row) memcpy(&r, rec + c->row_off, sizeof(r));
                /* Gather this row's agg values into the dense per-agg buffers
                 * (sequential record read → sequential dense write). */
                for (uint32_t a = 0; a < n_aggs; a++) {
                    if (gv[a]) { uint8_t ez = c->val_esz[a];
                        memcpy(gv[a] + (size_t)ri * ez, rec + c->val_off[a], ez); }
                    if (gy[a]) { uint8_t ez2 = c->val2_esz[a];
                        memcpy(gy[a] + (size_t)ri * ez2, rec + c->val2_off[a], ez2); }
                }
                /* Hash the contiguous packed keys (same FNV-1a as the scatter).
                 * The scatter consumed the LOW log2(n_parts) bits of this hash
                 * to pick the partition, so within this partition those bits
                 * are constant across every record — the slot index must come
                 * from the bits above them or the open-addressing table
                 * collapses to 2^(htbits - partbits) usable slots and probing
                 * degrades to O(ng^2) linear chains (ClickBench q17: 65% of
                 * the query inside this probe loop at 4096 partitions). */
                uint64_t h = 1469598103934665603ULL;
                for (uint32_t k = 0; k < n_keys; k++) { h ^= (uint64_t)keys[k]; h *= 1099511628211ULL; }
                h = agg_radix_fmix64(h);
                uint64_t slot = (h >> c->part_bits) & htmask;
                /* Salt = top 8 bits of the hash (independent of the low-bit slot
                 * index). Compared first on probe to skip ~255/256 full memcmps. */
                uint8_t salt = (uint8_t)(h >> 56);
                int32_t gg;
                for (;;) {
                    int32_t gp = ht[slot];
                    if (gp < 0) {                       /* new group */
                        if ((int64_t)ng >= gcap) {      /* grow group-state arrays (sized to distinct groups, not rows) */
                            int64_t ncap = gcap * 2;
                            /* Assign each realloc result back before the next so
                             * a mid-sequence failure leaves only valid pointers
                             * for the oom: cleanup (realloc frees the old block
                             * on success, so the pre-realloc pointer must never
                             * survive into cleanup). */
                            void* ns = ray_realloc_raw(states, (size_t)ncap * c->block);
                            if (!ns) goto oom;
                            states = ns;
                            void* nk = ray_realloc_raw((void*)keyp, (size_t)ncap * sizeof(int64_t*));
                            if (!nk) goto oom;
                            keyp = nk;
                            void* ngk = ray_realloc_raw(gkeys, (size_t)ncap * (size_t)(n_keys ? n_keys : 1) * sizeof(int64_t));
                            if (!ngk) goto oom;
                            gkeys = ngk;
                            if (c->needs_row) {
                                void* nf = ray_realloc_raw(first_row, (size_t)ncap * sizeof(int64_t));
                                if (!nf) goto oom;
                                first_row = nf;
                            }
                            gcap = ncap;
                        }
                        gg = (int32_t)ng;
                        ht[slot] = gg;
                        ht_salt[slot] = salt;
                        if (c->needs_row) first_row[gg] = r;
                        keyp[gg] = keys;
                        for (uint32_t k = 0; k < n_keys; k++)
                            gkeys[(size_t)gg * n_keys + k] = keys[k];
                        for (uint32_t a = 0; a < n_aggs; a++)
                            c->vts[a]->init(states + (size_t)gg * c->block + c->off[a]);
                        ng++;
                        break;
                    }
                    if (ht_salt[slot] == salt && memcmp(keys, keyp[gp], key_bytes) == 0) {
                        gg = gp;                                     /* salt-gated key compare */
                        if (c->needs_row && r < first_row[gg]) first_row[gg] = r;   /* MIN */
                        break;
                    }
                    slot = (slot + 1) & htmask;
                }
                gid[ri] = (uint32_t)gg;
                ri++;
            }
        }
        ray_free_raw(ht); ray_free_raw(ht_salt); ray_free_raw(keyp);

        /* Batch-accumulate each agg over the partition's dense value buffers. */
        for (uint32_t a = 0; a < n_aggs; a++) {
            if (c->vts[a]->update_batch2) {             /* binary agg (pearson) */
                ray_valid_t vx = { gv[a], c->val_types[a],  c->val_hasnull[a] };
                ray_valid_t vy = { gy[a], c->val2_types[a], c->val2_hasnull[a] };
                c->vts[a]->update_batch2(states + c->off[a], c->block, gid,
                                         gv[a], gy[a], &vx, &vy, total, NULL);
            } else if (c->val_data[a]) {                /* unary agg over a column */
                ray_valid_t valid = { gv[a], c->val_types[a], c->val_hasnull[a] };
                c->vts[a]->update_batch(states + c->off[a], c->block, gid, gv[a], &valid, total, NULL);
            } else {                                    /* COUNT (no value column) */
                ray_valid_t valid = { NULL, c->val_types[a], false };
                c->vts[a]->update_batch(states + c->off[a], c->block, gid, NULL, &valid, total, NULL);
            }
        }

        for (uint32_t a = 0; a < n_aggs; a++) { ray_free_raw(gv[a]); ray_free_raw(gy[a]); }
        ray_free_raw(gv); ray_free_raw(gy);
        ray_free_raw(gid);
        pr->states = states; pr->first_row = first_row; pr->keys = gkeys; pr->ng = ng;
        continue;
    oom:
        ray_free_raw(ht); ray_free_raw(ht_salt); ray_free_raw(first_row); ray_free_raw(states); ray_free_raw(keyp); ray_free_raw(gid); ray_free_raw(gkeys);
        for (uint32_t a = 0; a < n_aggs; a++) { ray_free_raw(gv[a]); ray_free_raw(gy[a]); }
        ray_free_raw(gv); ray_free_raw(gy);
        pr->oom = 1; return;
    }
}

/* ── Phase 3 parallel finalize/gather (high-group-count win) ──────────────
 * For huge ngroups (q10 ~10M), the serial per-group vt->finalize + cell write
 * dominates.  The emit order is fixed by `pairs` (build order — output order is
 * unspecified), so the output is ngroups rows: partition [0,ngroups) across
 * workers and have each worker finalize its DISJOINT slice into the pre-sized
 * output columns.
 *
 * Safety:
 *   - Each worker writes disjoint output element ranges → no payload contention.
 *   - Per-group buffered states are read (finalize) here; the buffered destroy
 *     stays SERIAL afterward (agg_radix_parts_destroy) → exactly-once, no race.
 *   - a serial emitter would OR RAY_ATTR_HAS_NULLS into the SHARED out->attrs (a
 *     racy read-modify-write).  So the worker writes the null sentinel directly
 *     (disjoint idx, safe) and records a per-worker "saw null" flag; the caller
 *     ORs RAY_ATTR_HAS_NULLS once, serially, after the parallel pass.
 *   - LIST out_type (top/bot) cells are NEW ray_t's whose finalize + ray_list_set
 *     COW/retain semantics are not confirmed race-free → LIST aggs are finalized
 *     SERIALLY by the caller (q10 and other scalar shapes get the full win). */
/* ---- Parallel phase-3 stable ordering -----------------------------------
 * The order map (pairs[input_count], -1-filled) is scattered into by
 * partition and then stream-compacted.  Serially this is an 80MB touch +
 * scatter + full scan per 10M-row query — a flat-scaling wall on high-card
 * groups (q17/q18).  Parallel version:
 *   scatter — dispatched over PARTITIONS: each group's first_row is globally
 *   unique (a row belongs to exactly one group, groups are disjoint across
 *   partitions), so writes are disjoint and race-free.  The serial code's
 *   per-write duplicate check moves to the aggregate `ordered == ng` check:
 *   a duplicate (corrupt state) overwrites one entry, the compact count
 *   comes up short, and the same error path fires.
 *   compact — two passes over fixed chunks: per-chunk non-empty counts, a
 *   serial prefix over the (few hundred) chunk counts, then in-place writes
 *   at each chunk's prefix offset.  In-place is forward-safe: chunk k's
 *   write region [prefix[k], prefix[k]+cnt[k]) never overlaps a LATER
 *   chunk's unread region (prefix[j+1] <= (j+1)*C <= k*C for j < k), and
 *   within a chunk dst <= src with ascending iteration. */
typedef struct {
    agg_radix_part_t*  parts;
    agg_radix_order_t* pairs;
    int64_t            input_count;
    _Atomic(int)       fail;
} agg_ord_scatter_ctx_t;

static void agg_ord_scatter_fn(void* vctx, uint32_t wid, int64_t start,
                               int64_t end) {
    (void)wid;
    agg_ord_scatter_ctx_t* c = (agg_ord_scatter_ctx_t*)vctx;
    if (atomic_load_explicit(&c->fail, memory_order_relaxed)) return;
    for (int64_t p = start; p < end; p++) {
        agg_radix_part_t* pr = &c->parts[p];
        for (int64_t gg = 0; gg < pr->ng; gg++) {
            int64_t first = pr->first_row[gg];
            if (first < 0 || first >= c->input_count) {
                atomic_store_explicit(&c->fail, 1, memory_order_relaxed);
                return;
            }
            c->pairs[first].idx = ((int64_t)p << 32) | (uint32_t)gg;
        }
    }
}

typedef struct {
    agg_radix_order_t* pairs;
    agg_radix_order_t* dst;        /* [ng] compacted output (out of place) */
    int64_t            input_count;
    int64_t            chunk;      /* elements per chunk */
    int64_t*           counts;     /* [n_chunks]: pass A out / pass B prefix in */
} agg_ord_compact_ctx_t;

static void agg_ord_count_fn(void* vctx, uint32_t wid, int64_t start,
                             int64_t end) {
    (void)wid;
    agg_ord_compact_ctx_t* c = (agg_ord_compact_ctx_t*)vctx;
    for (int64_t ch = start; ch < end; ch++) {
        int64_t lo = ch * c->chunk;
        int64_t hi = lo + c->chunk;
        if (hi > c->input_count) hi = c->input_count;
        int64_t n = 0;
        for (int64_t i = lo; i < hi; i++)
            n += (c->pairs[i].idx != -1);
        c->counts[ch] = n;
    }
}

static void agg_ord_compact_fn(void* vctx, uint32_t wid, int64_t start,
                               int64_t end) {
    (void)wid;
    agg_ord_compact_ctx_t* c = (agg_ord_compact_ctx_t*)vctx;
    for (int64_t ch = start; ch < end; ch++) {
        int64_t lo = ch * c->chunk;
        int64_t hi = lo + c->chunk;
        if (hi > c->input_count) hi = c->input_count;
        int64_t w = c->counts[ch];   /* prefix offset for this chunk */
        for (int64_t i = lo; i < hi; i++)
            if (c->pairs[i].idx != -1) c->dst[w++].idx = c->pairs[i].idx;
    }
}

typedef struct {
    agg_radix_part_t*    parts;
    const agg_radix_order_t* pairs;   /* [ng], stable first-seen order */
    const agg_vtable_t** vts;
    const size_t*        off;
    size_t               block;
    uint32_t             n_aggs;
    const int64_t*       agg_k;       /* [n_aggs] kparam, or NULL */
    ray_t**              outs;        /* [n_aggs] pre-sized scalar columns (LIST = NULL) */
    uint8_t*             saw_null;    /* [nw * n_aggs] per-worker null flag, 0/1 */
} agg_radix_fin_ctx_t;

/* Write a finalized scalar cell's payload at disjoint index i WITHOUT touching
 * the shared out->attrs flag; report null via the return value.  Byte-identical
 * to serial emission followed by ray_vec_set_null: a null cell stores the
 * type's NULL sentinel (overwriting cell->f64/i64), matching ray_vec_set_null's
 * payload write — only the RAY_ATTR_HAS_NULLS flag set is deferred to the
 * caller (set once serially) to avoid a racy shared read-modify-write. */
static inline bool agg_put_cell_value(ray_t* out, int64_t i, ray_t* cell) {
    bool is_null = RAY_ATOM_IS_NULL(cell);
    switch (out->type) {
        case RAY_BOOL: case RAY_U8: ((uint8_t*)ray_data(out))[i] = cell->u8; break;
        case RAY_I16: ((int16_t*)ray_data(out))[i] = is_null ? NULL_I16 : cell->i16; break;
        case RAY_I32: case RAY_DATE: case RAY_TIME:
            ((int32_t*)ray_data(out))[i] = is_null ? NULL_I32 : cell->i32; break;
        case RAY_F32: ((float*)ray_data(out))[i] = is_null ? NULL_F32 : (float)cell->f64; break;
        case RAY_F64: ((double*)ray_data(out))[i] = is_null ? NULL_F64 : cell->f64; break;
        default: ((int64_t*)ray_data(out))[i] = is_null ? NULL_I64 : cell->i64; break;
    }
    return is_null;
}

/* Phase 3 worker: finalize the scalar aggs for output rows [start,end). */
static void agg_radix_finalize_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    agg_radix_fin_ctx_t* c = (agg_radix_fin_ctx_t*)vctx;
    uint32_t n_aggs = c->n_aggs;
    for (uint32_t a = 0; a < n_aggs; a++) {
        ray_t* out = c->outs[a];
        if (!out) continue;                       /* LIST agg: emitted serially */
        int64_t kparam = c->agg_k ? c->agg_k[a] : 0;
        bool any_null = false;
        for (int64_t i = start; i < end; i++) {
            uint32_t p  = (uint32_t)(c->pairs[i].idx >> 32);
            uint32_t gg = (uint32_t)(c->pairs[i].idx & 0xffffffffu);
            const void* state = c->parts[p].states + (size_t)gg * c->block + c->off[a];
            any_null |= agg_finalize_value(c->vts[a], state, out, i, kparam);
        }
        if (any_null) c->saw_null[(size_t)wid * n_aggs + a] = 1;
    }
}

/* Heap sort of (keys, pairs) ascending by key, moving both arrays together.
 * Used for the small selected sets of the bounded and top-N emits. */
static void agg_sort_pairs_by_key(agg_radix_order_t* pairs, int64_t* keys, int64_t n) {
    #define PAIRS_SIFT_DOWN(START, END) do {                                   \
        int64_t i_ = (START);                                                  \
        for (;;) {                                                             \
            int64_t l = 2 * i_ + 1, r = l + 1, m = i_;                         \
            if (l < (END) && keys[l] > keys[m]) m = l;                         \
            if (r < (END) && keys[r] > keys[m]) m = r;                         \
            if (m == i_) break;                                                \
            int64_t tk = keys[m]; keys[m] = keys[i_]; keys[i_] = tk;           \
            int64_t tp = pairs[m].idx; pairs[m].idx = pairs[i_].idx; pairs[i_].idx = tp; \
            i_ = m;                                                            \
        }                                                                      \
    } while (0)
    for (int64_t start = n / 2 - 1; start >= 0; start--) PAIRS_SIFT_DOWN(start, n);
    for (int64_t end = n - 1; end > 0; end--) {
        int64_t tk = keys[0]; keys[0] = keys[end]; keys[end] = tk;
        int64_t tp = pairs[0].idx; pairs[0].idx = pairs[end].idx; pairs[end].idx = tp;
        PAIRS_SIFT_DOWN(0, end);
    }
    #undef PAIRS_SIFT_DOWN
}

/* Top-N selection for the emit filter: every partition finalizes the filter
 * aggregate into one double per group in parallel, the shared keep decision
 * picks the kept superset, and only those groups are emitted — in ascending
 * first-row order, so the emitted prefix is deterministic.  Replaces the
 * full path's input-sized order map, full emission and post-trim for
 * `desc: AGG take: N` shapes (a 10M-group three-key count spent 110 of its
 * 154 ms there).  `*rc`: 0 ok, 1 oom, 2 the aggregate has no scalar order
 * (caller keeps the full path and trims). */
typedef struct {
    const agg_radix_part_t* parts;
    const agg_vtable_t* vt;
    size_t off, block;
    int64_t param;
    const ray_group_emit_filter_t* ef;
    double* vals;
    uint8_t* keep;
    const int64_t* base;
    double* cand;          /* [n_parts * cap] per-partition candidate heaps */
    int64_t* cand_n;       /* [n_parts] */
    int64_t cap;
    bool have_thr;
    double thr;
    int64_t* kept;         /* [n_parts] */
    _Atomic(int) fail;
} agg_radix_vals_ctx_t;

/* Pass 1 per partition: finalize values, then the bounded candidate heap. */
static void agg_radix_vals_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_radix_vals_ctx_t* c = raw;
    for (int64_t p = start; p < end; p++) {
        double* v = c->vals + c->base[p];
        if (!agg_group_values_f64(c->vt, c->parts[p].states, c->block, c->off, NULL,
                                  c->parts[p].ng, c->param, v)) {
            atomic_store_explicit(&c->fail, 1, memory_order_relaxed);
            continue;
        }
        if (c->cap > 0)
            agg_topn_candidates(v, c->parts[p].ng, c->cap, c->ef->desc,
                                c->cand + (size_t)p * c->cap, &c->cand_n[p]);
    }
}

/* Pass 2 per partition: mark the kept groups against the global threshold. */
static void agg_radix_mark_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_radix_vals_ctx_t* c = raw;
    for (int64_t p = start; p < end; p++)
        c->kept[p] = agg_topn_mark(c->vals + c->base[p], c->parts[p].ng, c->ef,
                                   c->have_thr, c->thr, c->keep + c->base[p]);
}

static agg_radix_order_t* __attribute__((noinline))
agg_radix_select_topn(ray_pool_t* pool, const agg_radix_part_t* parts, uint32_t n_parts,
                      const agg_vtable_t* vt, size_t off, size_t block, int64_t param,
                      const ray_group_emit_filter_t* ef, int64_t* n_emit, int* rc) {
    *rc = 0;
    int64_t ng = 0;
    int64_t* base = ray_alloc_raw(((size_t)n_parts + 1) * sizeof(int64_t));
    if (!base) { *rc = 1; return NULL; }
    for (uint32_t p = 0; p < n_parts; p++) { base[p] = ng; ng += parts[p].ng; }
    base[n_parts] = ng;
    double* vals = ray_alloc_raw((size_t)(ng > 0 ? ng : 1) * sizeof(double));
    uint8_t* keep = ray_alloc_raw((size_t)(ng > 0 ? ng : 1));
    if (!vals || !keep) {
        ray_free_raw(base); ray_free_raw(vals); ray_free_raw(keep); *rc = 1; return NULL;
    }
    int64_t cap = ef->top_count_take > 0 && ng > ef->top_count_take ? ef->top_count_take : 0;
    double* cand = ray_alloc_raw((size_t)(cap > 0 ? cap * n_parts : 1) * sizeof(double));
    int64_t* cand_n = ray_calloc_raw((size_t)n_parts * 2 * sizeof(int64_t));
    if (!cand || !cand_n) {
        ray_free_raw(base); ray_free_raw(vals); ray_free_raw(keep);
        ray_free_raw(cand); ray_free_raw(cand_n); *rc = 1; return NULL;
    }
    agg_radix_vals_ctx_t c = { .parts = parts, .vt = vt, .off = off, .block = block,
        .param = param, .ef = ef, .vals = vals, .keep = keep, .base = base,
        .cand = cand, .cand_n = cand_n, .cap = cap, .kept = cand_n + n_parts };
    atomic_init(&c.fail, 0);
    if (pool) ray_pool_dispatch_n(pool, agg_radix_vals_fn, &c, n_parts);
    else agg_radix_vals_fn(&c, 0, 0, n_parts);
    if (atomic_load_explicit(&c.fail, memory_order_relaxed)) {
        ray_free_raw(base); ray_free_raw(vals); ray_free_raw(keep);
        ray_free_raw(cand); ray_free_raw(cand_n); *rc = 2; return NULL;
    }
    /* The union of every partition's N best contains the global N best, so
     * its N-th value is the global threshold: O(N * parts) serial work. */
    if (cap > 0) {
        int64_t nc = 0;
        for (uint32_t p = 0; p < n_parts; p++) {
            memmove(cand + nc, cand + (size_t)p * cap, (size_t)cand_n[p] * sizeof(double));
            nc += cand_n[p];
        }
        c.have_thr = agg_topn_threshold(cand, nc, ef, &c.thr);
    }
    if (pool) ray_pool_dispatch_n(pool, agg_radix_mark_fn, &c, n_parts);
    else agg_radix_mark_fn(&c, 0, 0, n_parts);
    int64_t kept = 0;
    for (uint32_t p = 0; p < n_parts; p++) kept += c.kept[p];
    ray_free_raw(cand); ray_free_raw(cand_n);
    agg_radix_order_t* sel = ray_alloc_raw((size_t)(kept > 0 ? kept : 1) * sizeof(agg_radix_order_t));
    int64_t* fr = ray_alloc_raw((size_t)(kept > 0 ? kept : 1) * sizeof(int64_t));
    if (!sel || !fr) {
        ray_free_raw(sel); ray_free_raw(fr);
        ray_free_raw(base); ray_free_raw(vals); ray_free_raw(keep); *rc = 1; return NULL;
    }
    int64_t k = 0;
    for (uint32_t p = 0; p < n_parts; p++)
        for (int64_t gg = 0; gg < parts[p].ng; gg++)
            if (keep[base[p] + gg]) {
                sel[k].idx = ((int64_t)p << 32) | (uint32_t)gg;
                fr[k] = parts[p].first_row[gg];
                k++;
            }
    agg_sort_pairs_by_key(sel, fr, k);
    ray_free_raw(fr); ray_free_raw(base); ray_free_raw(vals); ray_free_raw(keep);
    *n_emit = k;
    return sel;
}

/* Bounded first-seen selection for the HEAD(GROUP) limit hint: pick the
 * `n_emit` groups with the SMALLEST first_row across all partitions — which
 * ARE the first `n_emit` groups in first-seen order — and return them as the
 * usual (part << 32 | local gid) order array, ascending by first_row.
 *
 * Kept noinline so the (cold, hint-only) selection code neither inflates nor
 * perturbs the register allocation of exec_group_v2_parallel_radix's hot body
 * (mirrors group.c's noinline isolation of its per-partition path).
 * `*rc`: 0 ok, 1 oom, 2 corrupt first_row (caller raises the order error). */
static agg_radix_order_t* __attribute__((noinline))
agg_radix_select_first_n(const agg_radix_part_t* parts, uint32_t n_parts,
                         int64_t input_count, int64_t n_emit, int* rc) {
    *rc = 0;
    agg_radix_order_t* sel_pairs =
        ray_alloc_raw((size_t)n_emit * sizeof(agg_radix_order_t));
    ray_t* hk_hdr = NULL;
    int64_t* hkey = sel_pairs
        ? (int64_t*)scratch_alloc(&hk_hdr, (size_t)n_emit * sizeof(int64_t))
        : NULL;
    if (!sel_pairs || !hkey) {
        ray_free_raw(sel_pairs); scratch_free(hk_hdr);
        *rc = 1; return NULL;
    }
    /* Max-heap over first_row keeping the n_emit smallest. */
    int64_t hn = 0;
    bool ok = true;
    for (uint32_t p = 0; p < n_parts && ok; p++) {
        const int64_t* fr = parts[p].first_row;
        for (int64_t gg = 0; gg < parts[p].ng; gg++) {
            int64_t first = fr[gg];
            if (first < 0 || first >= input_count) { ok = false; break; }
            int64_t payload = ((int64_t)p << 32) | (uint32_t)gg;
            if (hn < n_emit) {
                int64_t i = hn++;
                hkey[i] = first; sel_pairs[i].idx = payload;
                while (i > 0) {                       /* sift up */
                    int64_t par = (i - 1) / 2;
                    if (hkey[par] >= hkey[i]) break;
                    int64_t tk = hkey[par]; hkey[par] = hkey[i]; hkey[i] = tk;
                    int64_t tp = sel_pairs[par].idx;
                    sel_pairs[par].idx = sel_pairs[i].idx; sel_pairs[i].idx = tp;
                    i = par;
                }
            } else if (first < hkey[0]) {
                hkey[0] = first; sel_pairs[0].idx = payload;
                int64_t i = 0;
                for (;;) {                            /* sift down */
                    int64_t l = 2 * i + 1, r = l + 1, m = i;
                    if (l < n_emit && hkey[l] > hkey[m]) m = l;
                    if (r < n_emit && hkey[r] > hkey[m]) m = r;
                    if (m == i) break;
                    int64_t tk = hkey[m]; hkey[m] = hkey[i]; hkey[i] = tk;
                    int64_t tp = sel_pairs[m].idx;
                    sel_pairs[m].idx = sel_pairs[i].idx; sel_pairs[i].idx = tp;
                    i = m;
                }
            }
        }
    }
    /* Sort the selected entries into ascending first_row order (the heap
     * above is already a max-heap, so the sort's heapify is a no-op). */
    if (ok) {
        agg_sort_pairs_by_key(sel_pairs, hkey, hn);
        ok = hn == n_emit;
    }
    scratch_free(hk_hdr);
    if (!ok) { ray_free_raw(sel_pairs); *rc = 2; return NULL; }
    return sel_pairs;
}

typedef struct { agg_radix_order_t* pairs; } agg_ord_fill_ctx_t;
static void agg_ord_fill_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_ord_fill_ctx_t* c = raw;
    memset(c->pairs + start, 0xFF, (size_t)(end - start) * sizeof(agg_radix_order_t));
}

static ray_t* exec_group_v2_parallel_radix(
        ray_graph_t* g, ray_op_t* op, ray_t* tbl, int64_t nrows,
        ray_t** key_cols, int64_t* key_syms,
        const agg_vtable_t** vts, const size_t* off, size_t block,
        ray_t* sel, const int64_t* sel_prefix, int64_t n_sel,
        int64_t group_limit, const ray_group_emit_filter_t* efp) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    uint32_t n_keys = ext->n_keys, n_aggs = ext->n_aggs;
    ray_pool_t* pool = ray_pool_get();
    uint32_t nw = ray_pool_total_workers(pool);

    agg_desc_t d;
    if (!agg_desc_init(&d, g, ext, tbl, key_cols)) return ray_error("oom", NULL);
    const void** key_data = d.key_data;
    const void** val_data = d.val_data; const int8_t* val_types = d.val_types;
    const bool* val_hasnull = d.val_hasnull; const uint8_t* val_esz = d.val_esz;
    const void** val2_data = d.val2_data; const int8_t* val2_types = d.val2_types;
    const bool* val2_hasnull = d.val2_hasnull; const uint8_t* val2_esz = d.val2_esz;
    const int64_t* agg_syms = d.agg_syms;

    /* Every group records its earliest source row so emitted order does not
     * depend on the number of radix partitions used by this execution. */
    bool needs_row = true;

    /* Per-row payload record layout: [packed keys][agg values][row_idx?].
     * val_off/val2_off are per-dispatch (filled once here, read by workers).
     * The same carve tails a per-worker key-staging block (nw*n_keys int64,
     * both 8-byte) so kv_scratch shares off_hdr's lifetime — one free, no extra
     * exit sites.  Only phases 1-2 read either; both freed once phase 2 joins. */
    ray_t* off_hdr;
    size_t* val_off = (size_t*)scratch_calloc(&off_hdr,
        2u * (size_t)n_aggs * sizeof(size_t) + (size_t)nw * (size_t)n_keys * sizeof(int64_t));
    if (!val_off) { agg_desc_free(&d); return ray_error("oom", NULL); }
    size_t* val2_off = val_off + n_aggs;
    int64_t* kv_scratch = (int64_t*)(val2_off + n_aggs);   /* [nw * n_keys] */
    size_t rec_cur = (size_t)n_keys * 8;
    for (uint32_t a = 0; a < n_aggs; a++) {
        if (val_data[a])  { val_off[a]  = rec_cur; rec_cur += val_esz[a]; }
        if (val2_data[a]) { val2_off[a] = rec_cur; rec_cur += val2_esz[a]; }
    }
    size_t row_off = rec_cur; if (needs_row) rec_cur += 8;
    size_t rec = (rec_cur + 7u) & ~(size_t)7u;   /* 8-align records */

    uint32_t n_parts = agg_radix_part_count(nw, sel ? n_sel : nrows);
    size_t nbuf = (size_t)nw * n_parts;
    agg_pay_buf_t*    bufs  = ray_calloc_raw((size_t)(nbuf) * (sizeof(agg_pay_buf_t)));
    agg_radix_part_t* parts = ray_calloc_raw((size_t)n_parts * sizeof(agg_radix_part_t));
    if (!bufs || !parts) {
        ray_free_raw(bufs); ray_free_raw(parts);
        scratch_free(off_hdr); agg_desc_free(&d);
        return exec_group_v2_parallel_smallhash(g, op, tbl, nrows,
                key_cols, key_syms, vts, off, block, sel, sel_prefix, n_sel);
    }

    agg_radix_ctx_t ctx = {
        .key_cols = key_cols, .key_data = key_data, .n_keys = n_keys,
        .vts = vts, .off = off, .block = block, .n_aggs = n_aggs,
        .val_data = val_data, .val_types = val_types, .val_hasnull = val_hasnull, .val_esz = val_esz,
        .val2_data = val2_data, .val2_types = val2_types, .val2_hasnull = val2_hasnull, .val2_esz = val2_esz,
        .nw = nw, .n_parts = n_parts,
        .part_bits = (uint32_t)__builtin_ctz(n_parts),
        /* Uniform-hash expectation with 25% slack; ≥8 so tiny buffers don't
         * immediately re-double. */
        .pay_prime = (uint32_t)((uint64_t)(sel ? n_sel : nrows)
                                / ((uint64_t)nw * n_parts) * 5 / 4 + 8),
        .bufs = bufs, .parts = parts, .phase1_oom = 0,
        .rec = rec, .row_off = row_off, .needs_row = needs_row,
        .sel = sel, .sel_prefix = sel_prefix,
        .val_off = val_off, .val2_off = val2_off,
        .kv_scratch = kv_scratch,
    };

    /* Phase 1: scatter.  Sel mode dispatches over selected-row space [0,n_sel);
     * each worker decodes its slice of selected rows to ORIGINAL indices. */
    ray_pool_dispatch(pool, agg_radix_scatter_fn, &ctx, sel ? n_sel : nrows);
    ray_profile_tick("radix: scattered rows");

    /* Phase 2: per-partition group+accumulate. */
    int oom = ctx.phase1_oom;
    if (!oom)
        ray_pool_dispatch_n(pool, agg_radix_group_fn, &ctx, n_parts);
    if (!oom)
        for (uint32_t p = 0; p < n_parts; p++)
            if (parts[p].oom) { oom = 1; break; }

    ray_profile_tick("radix: reduced partitions");

    /* Phases 1+2 done (both dispatches joined); val_off/val2_off no longer read. */
    scratch_free(off_hdr);

    if (oom) {
        agg_radix_parts_destroy(parts, n_parts, vts, off, block, n_aggs);
        for (size_t i = 0; i < nbuf; i++) ray_free_raw(bufs[i].buf);
        ray_free_raw(bufs); ray_free_raw(parts);
        agg_desc_free(&d);
        return exec_group_v2_parallel_smallhash(g, op, tbl, nrows,
                key_cols, key_syms, vts, off, block, sel, sel_prefix, n_sel);
    }

    /* Phase 3: restore stable first-seen order across partitions. */
    int64_t ng = 0;
    for (uint32_t p = 0; p < n_parts; p++) ng += parts[p].ng;

    /* Place each group directly at its first logical input position, then
     * compact those occupied positions in one linear pass.  This restores
     * first-seen order without a comparison sort or a routing threshold. */
    int64_t input_count = sel ? n_sel : nrows;

    /* Bounded emit under a HEAD(GROUP) limit hint: when only the first
     * `group_limit` groups are wanted, selecting them directly is O(ng) with a
     * `group_limit`-sized heap, versus the full path's O(input_count) order map
     * (an 80MB alloc + memset + scatter + compact on a 10M-row input) followed
     * by a full key-unpack and finalize of every group.  The N groups with the
     * SMALLEST first_row ARE the first N groups in first-seen order, so the
     * emitted prefix is byte-identical to trimming the full result to N. */
    /* Bounded emit under a HEAD(GROUP) limit hint: when only the first
     * `group_limit` groups are wanted, selecting them directly is O(ng) with a
     * `group_limit`-sized heap, versus the full path's O(input_count) order map
     * (an 80MB alloc + memset + scatter + compact on a 10M-row input) followed
     * by a full key-unpack and finalize of every group.  The N groups with the
     * SMALLEST first_row ARE the first N groups in first-seen order, so the
     * emitted prefix is byte-identical to trimming the full result to N. */
    int64_t n_emit = ng;
    agg_radix_order_t* pairs = NULL;
    if (efp && group_limit == 0) {
        int rc = 0;
        int64_t kept = 0;
        agg_radix_order_t* sel_pairs = agg_radix_select_topn(pool, parts, n_parts,
                vts[efp->agg_index], off[efp->agg_index], block,
                ext->agg_k ? ext->agg_k[efp->agg_index] : 0, efp, &kept, &rc);
        if (sel_pairs) {
            pairs = sel_pairs;
            n_emit = kept;
            route_stats.topn_native = true;
        } else if (rc == 1) {
            agg_radix_parts_destroy(parts, n_parts, vts, off, block, n_aggs);
            for (size_t i = 0; i < nbuf; i++) ray_free_raw(bufs[i].buf);
            ray_free_raw(bufs); ray_free_raw(parts);
            agg_desc_free(&d);
            return ray_error("oom", NULL);
        }
        /* rc == 2: no scalar order for this aggregate — full path below. */
    }
    if (pairs) {
        /* native top-N selected above */
    } else if (group_limit > 0 && ng > group_limit) {
        int rc = 0;
        n_emit = group_limit;
        agg_radix_order_t* sel_pairs = agg_radix_select_first_n(
                parts, n_parts, input_count, n_emit, &rc);
        if (!sel_pairs) {
            agg_radix_parts_destroy(parts, n_parts, vts, off, block, n_aggs);
            for (size_t i = 0; i < nbuf; i++) ray_free_raw(bufs[i].buf);
            ray_free_raw(bufs); ray_free_raw(parts);
            agg_desc_free(&d);
            return rc == 1 ? ray_error("oom", NULL)
                           : ray_error("group", "failed to order radix groups");
        }
        pairs = sel_pairs;
    } else {
    pairs = ray_alloc_raw(
        (size_t)(input_count > 0 ? input_count : 1) * sizeof(agg_radix_order_t));
    if (!pairs) {
        agg_radix_parts_destroy(parts, n_parts, vts, off, block, n_aggs);
        for (size_t i = 0; i < nbuf; i++) ray_free_raw(bufs[i].buf);
        ray_free_raw(bufs); ray_free_raw(parts);
        agg_desc_free(&d);
        return ray_error("oom", NULL);
    }
    /* -1 fill: on a 10M-row input this is an 80MB first touch of fresh
     * pages, so it runs across the pool (each task faults and fills its own
     * range); the serial memset it replaces cost ~20 ms of a 63 ms phase. */
    {
        agg_ord_fill_ctx_t fctx = { .pairs = pairs };
        if (pool && input_count >= RAY_PARALLEL_THRESHOLD)
            ray_pool_dispatch(pool, agg_ord_fill_fn, &fctx, input_count);
        else
            memset(pairs, 0xFF, (size_t)(input_count > 0 ? input_count : 1) * sizeof(agg_radix_order_t));
    }
    bool order_ok = true;
    int64_t ordered = 0;
    bool ord_parallel_done = false;
    /* Every stage below is dispatched by TASK COUNT (ray_pool_dispatch_n):
     * the element-grain dispatch used before produced a single task for a
     * 128-partition scatter and a 77-chunk compaction, so this "parallel"
     * ordering ran serially (63 ms of a 10M-group query on 28 threads).  The
     * compaction writes out of place into an ng-sized buffer, so chunks are
     * independent and the input-sized map is released right after. */
    const int64_t ORD_CHUNK = 1 << 17;
    const int64_t ORD_MAX_CHUNKS = RAY_POOL_MAX_TASKS / 4;
    if (pool && nw > 1 && input_count >= (1 << 20) &&
        (input_count + ORD_CHUNK - 1) / ORD_CHUNK <= ORD_MAX_CHUNKS) {
        int64_t n_chunks = (input_count + ORD_CHUNK - 1) / ORD_CHUNK;
        ray_t* ordcnt_hdr = NULL;
        int64_t* ord_counts = (int64_t*)scratch_alloc(&ordcnt_hdr,
            (size_t)n_chunks * sizeof(int64_t));
        agg_radix_order_t* compacted = ray_alloc_raw((size_t)(ng > 0 ? ng : 1) * sizeof(agg_radix_order_t));
        if (ord_counts && compacted) {
            agg_ord_scatter_ctx_t sctx = {
                .parts = parts, .pairs = pairs,
                .input_count = input_count, .fail = 0,
            };
            ray_pool_dispatch_n(pool, agg_ord_scatter_fn, &sctx, (uint32_t)n_parts);
            if (!atomic_load_explicit(&sctx.fail, memory_order_relaxed)) {
                agg_ord_compact_ctx_t cctx = {
                    .pairs = pairs, .dst = compacted, .input_count = input_count,
                    .chunk = ORD_CHUNK, .counts = ord_counts,
                };
                ray_pool_dispatch_n(pool, agg_ord_count_fn, &cctx, (uint32_t)n_chunks);
                /* Exclusive prefix (serial over a few hundred chunks). */
                int64_t run = 0;
                for (int64_t ch = 0; ch < n_chunks; ch++) {
                    int64_t n = ord_counts[ch];
                    ord_counts[ch] = run;
                    run += n;
                }
                ordered = run;
                order_ok = ordered == ng;
                if (order_ok) {
                    ray_pool_dispatch_n(pool, agg_ord_compact_fn, &cctx, (uint32_t)n_chunks);
                    ray_free_raw(pairs);
                    pairs = compacted;
                    compacted = NULL;
                }
                ord_parallel_done = true;
            } else {
                order_ok = false;
                ord_parallel_done = true;   /* bounds violation → error path */
            }
        }
        ray_free_raw(compacted);
        scratch_free(ordcnt_hdr);
    }
    if (!ord_parallel_done) {
        for (uint32_t p = 0; p < n_parts && order_ok; p++) {
            for (int64_t gg = 0; gg < parts[p].ng; gg++) {
                int64_t first = parts[p].first_row[gg];
                if (first < 0 || first >= input_count || pairs[first].idx != -1) {
                    order_ok = false;
                    break;
                }
                pairs[first].idx = ((int64_t)p << 32) | (uint32_t)gg;
            }
        }
        if (order_ok) {
            for (int64_t i = 0; i < input_count; i++)
                if (pairs[i].idx != -1) pairs[ordered++].idx = pairs[i].idx;
            order_ok = ordered == ng;
        }
    }
    if (!order_ok) {
        ray_free_raw(pairs);
        agg_radix_parts_destroy(parts, n_parts, vts, off, block, n_aggs);
        for (size_t i = 0; i < nbuf; i++) ray_free_raw(bufs[i].buf);
        ray_free_raw(bufs); ray_free_raw(parts);
        agg_desc_free(&d);
        return ray_error("group", "failed to order radix groups");
    }
    }   /* end full-order path */
    ray_profile_tick("radix: ordered groups");

    ray_t* result = ray_table_new(n_keys + n_aggs);
    if (!result || RAY_IS_ERR(result)) {
        ray_free_raw(pairs);
        agg_radix_parts_destroy(parts, n_parts, vts, off, block, n_aggs);
        for (size_t i = 0; i < nbuf; i++) ray_free_raw(bufs[i].buf);
        ray_free_raw(bufs); ray_free_raw(parts);
        agg_desc_free(&d);
        return result ? result : ray_error("oom", NULL);
    }

    /* Emit key columns by sequential un-pack from the contiguous per-partition
     * packed-key buffers (cache-friendly), NOT a scattered gather of the
     * original columns at first_row[].  Byte-identical to ray_group_gather
     * (see agg_unpack_key_col_new), incl. SYM payload + domain.
     *
     * All n_keys destinations are built BEFORE the fill so one dispatch covers
     * the whole key side; they enter the table only once every row is written.
     * n_keys is NOT bounded to 16 on this path (only dense direct-index routing
     * caps it, in agg_dense_plan), so the handle array is a scratch carve, not
     * a stack array — ASan caught the stack version overflowing on a 17-key
     * group-by (test/rfl/group/radix_key_emit_parallel.rfl). */
    ray_t*   kouts_hdr = NULL;
    ray_t**  kouts   = (ray_t**)scratch_calloc(&kouts_hdr, (size_t)n_keys * sizeof(ray_t*));
    ray_t*   kerr    = kouts ? NULL : ray_error("oom", NULL);
    uint32_t k_built = 0;
    for (; !kerr && k_built < n_keys; k_built++) {
        ray_t* kc = agg_unpack_key_col_new(key_cols[k_built], n_emit);
        if (!kc || RAY_IS_ERR(kc)) { kerr = kc ? kc : ray_error("oom", NULL); break; }
        kouts[k_built] = kc;
    }
    if (!kerr) {
        agg_key_emit_ctx_t kctx = {
            .key_cols = key_cols, .outs = kouts, .parts = parts,
            .pairs = pairs, .n_keys = n_keys,
        };
        /* Parallelize the un-pack over the OUTPUT rows on the same terms as the
         * phase-3 finalize below: worthwhile only when ngroups is large.  Every
         * bounded-emit shape (the HEAD(GROUP) limit hint — q17 emits 10 rows)
         * and every low-cardinality group-by stays on the identical serial
         * range call, so there is no dispatch overhead where there is no win. */
        if (ray_pool_par_dispatch_ok(pool, n_emit, RAY_PARALLEL_THRESHOLD)) {
            ray_pool_dispatch(pool, agg_key_emit_fn, &kctx, n_emit);
            /* A cancelled dispatch drains its tickets WITHOUT running fn, so the
             * key columns can be left partly uninitialized (for SYM that would
             * be an out-of-domain id).  Never hand that back — bail. */
            if (pool_cancelled(pool)) kerr = ray_error("cancel", NULL);
        } else {
            agg_key_emit_range(&kctx, 0, n_emit);
        }
    }
    if (kerr) {
        for (uint32_t k = 0; k < k_built; k++) ray_release(kouts[k]);
        scratch_free(kouts_hdr);
        ray_free_raw(pairs);
        agg_radix_parts_destroy(parts, n_parts, vts, off, block, n_aggs);
        for (size_t i = 0; i < nbuf; i++) ray_free_raw(bufs[i].buf);
        ray_free_raw(bufs); ray_free_raw(parts);
        agg_desc_free(&d);
        ray_release(result); return kerr;
    }
    for (uint32_t k = 0; k < n_keys; k++) {
        result = ray_table_add_col(result, key_syms[k], kouts[k]);
        ray_release(kouts[k]);
    }
    scratch_free(kouts_hdr);

    /* Pre-allocate every agg's output column up front so the parallel finalize
     * pass can write disjoint slices into all scalar columns at once.  LIST
     * (top/bot) columns are finalized serially below — outs[a] stays NULL for
     * them so the parallel worker skips them. */
    ray_t* outs_hdr;
    ray_t** outs = (ray_t**)scratch_calloc(&outs_hdr, (size_t)n_aggs * (sizeof(ray_t*) + sizeof(int64_t)));
    if (!outs) {
        ray_free_raw(pairs);
        agg_radix_parts_destroy(parts, n_parts, vts, off, block, n_aggs);
        for (size_t i = 0; i < nbuf; i++) ray_free_raw(bufs[i].buf);
        ray_free_raw(bufs); ray_free_raw(parts);
        agg_desc_free(&d);
        ray_release(result); return ray_error("oom", NULL);
    }
    int64_t* kparams = (int64_t*)(outs + n_aggs);
    for (uint32_t a = 0; a < n_aggs; a++) {
        /* Buffered top_n/bot_n produce a LIST cell per group (a native vector);
         * median and all streaming aggs produce a scalar out_type cell. */
        bool is_list = (vts[a]->out_type == RAY_LIST);
        ray_t* out = is_list ? ray_list_new(n_emit)
                             : ray_vec_new(vts[a]->out_type, n_emit);
        if (!out || RAY_IS_ERR(out)) {
            for (uint32_t b = 0; b < a; b++) ray_release(outs[b]);
            ray_free_raw(pairs);
            agg_radix_parts_destroy(parts, n_parts, vts, off, block, n_aggs);
            for (size_t i = 0; i < nbuf; i++) ray_free_raw(bufs[i].buf);
            ray_free_raw(bufs); ray_free_raw(parts);
            scratch_free(outs_hdr); agg_desc_free(&d);
            ray_release(result); return out ? out : ray_error("oom", NULL);
        }
        out->len = n_emit;
        outs[a] = out;
        kparams[a] = (ext->agg_k ? ext->agg_k[a] : 0);
    }

    /* Parallelize the scalar finalize over the output rows when ngroups is large
     * (the q10/high-card win).  Small ng (incl. every low-card shape) keeps the
     * trivial serial loop → zero dispatch overhead, no regression. */
    bool par_fin = (pool && n_emit >= RAY_PARALLEL_THRESHOLD);
    if (par_fin) {
        uint8_t* saw_null = ray_calloc_raw((size_t)((size_t)nw * (n_aggs ? n_aggs : 1)) * (1));
        ray_t* scalar_hdr = NULL;
        ray_t** scalar_outs = saw_null
            ? (ray_t**)scratch_alloc(&scalar_hdr, (size_t)n_aggs * sizeof(ray_t*)) : NULL;
        if (!saw_null || !scalar_outs) { par_fin = false; ray_free_raw(saw_null); scratch_free(scalar_hdr); }
        else {
            for (uint32_t a = 0; a < n_aggs; a++)
                scalar_outs[a] = (vts[a]->out_type == RAY_LIST) ? NULL : outs[a];
            agg_radix_fin_ctx_t fc = {
                .parts = parts, .pairs = pairs, .vts = vts, .off = off,
                .block = block, .n_aggs = n_aggs, .agg_k = kparams,
                .outs = scalar_outs, .saw_null = saw_null,
            };
            ray_pool_dispatch(pool, agg_radix_finalize_fn, &fc, n_emit);
            /* OR the deferred HAS_NULLS flag once, serially. */
            for (uint32_t a = 0; a < n_aggs; a++) {
                if (vts[a]->out_type == RAY_LIST) continue;
                for (uint32_t w = 0; w < nw; w++)
                    if (saw_null[(size_t)w * n_aggs + a]) {
                        outs[a]->attrs |= RAY_ATTR_HAS_NULLS;
                        break;
                    }
            }
            ray_free_raw(saw_null);
            scratch_free(scalar_hdr);
        }
    }

    for (uint32_t a = 0; a < n_aggs; a++) {
        bool is_list = (vts[a]->out_type == RAY_LIST);
        ray_t* out = outs[a];
        /* Serial finalize: LIST aggs always (ray_list_set COW/retain not
         * confirmed race-safe), and all aggs when the parallel pass was skipped. */
        if (is_list || !par_fin) {
            for (int64_t i = 0; i < n_emit; i++) {
                uint32_t p  = (uint32_t)(pairs[i].idx >> 32);
                uint32_t gg = (uint32_t)(pairs[i].idx & 0xffffffffu);
                const void* state = parts[p].states + (size_t)gg * block + off[a];
                if (is_list) {
                    ray_t* cell = vts[a]->finalize(state, NULL, kparams[a]);
                    out = ray_list_set(out, i, cell);   /* retains cell */
                    ray_release(cell);
                } else if (agg_finalize_value(vts[a], state, out, i, kparams[a])) {
                    out->attrs |= RAY_ATTR_HAS_NULLS;
                }
            }
            outs[a] = out;   /* ray_list_set may COW-realloc */
        }
        int64_t agg_name = agg_result_col_name(agg_syms[a], ext->agg_ops[a]);
        result = ray_table_add_col(result, agg_name, out);
        ray_release(out);
    }

    ray_free_raw(pairs);
    /* Finalize is done reading every partition's buffered group state → destroy
     * exactly once before freeing the partition slabs. */
    agg_radix_parts_destroy(parts, n_parts, vts, off, block, n_aggs);
    for (size_t i = 0; i < nbuf; i++) ray_free_raw(bufs[i].buf);
    ray_free_raw(bufs); ray_free_raw(parts);
    scratch_free(outs_hdr); agg_desc_free(&d);
    return result;
}

static ray_t* agg_build_compact(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                                int64_t* idx, int64_t n_sel);

/* Stable row slices shared by ordered and streaming consumers. Large group
 * directories use O(groups) counters; replicated small-directory counters
 * have a fixed memory budget independent of input size. */
typedef struct {
    const uint32_t* gids;
    int64_t nrows;
    uint32_t tasks, parts, bits, slices;
    int64_t slots, local_stride;
    int64_t* local;
    const int64_t* offsets;
    uint32_t part_slices[257];
    int64_t slice_begin[512], slice_end[512];
    int64_t* hist;
    int64_t* starts;
    uint64_t* packed;
    int64_t* counts;
    int64_t* cursor;
    int64_t* rows;
} agg_index_layout_t;
static void agg_index_hist(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; agg_index_layout_t* c = raw;
    for (int64_t task = start; task < end; task++) {
        int64_t* hist = c->hist + task * c->parts;
        int64_t begin = c->nrows / c->tasks * task;
        int64_t limit = task + 1 == c->tasks ? c->nrows : c->nrows / c->tasks * (task + 1);
        for (int64_t r = begin; r < limit; r++) hist[c->gids[r] >> c->bits]++;
    }
}
static void agg_index_pack(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; agg_index_layout_t* c = raw;
    for (int64_t task = start; task < end; task++) {
        int64_t* cursor = c->hist + task * c->parts;
        int64_t begin = c->nrows / c->tasks * task;
        int64_t limit = task + 1 == c->tasks ? c->nrows : c->nrows / c->tasks * (task + 1);
        for (int64_t r = begin; r < limit; r++)
            c->packed[cursor[c->gids[r] >> c->bits]++] = ((uint64_t)c->gids[r] << 32) | (uint32_t)r;
    }
}
static void agg_index_count(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; agg_index_layout_t* c = raw;
    for (int64_t task = start; task < end; task++) {
        int64_t* counts = c->local + task * c->local_stride;
        for (int64_t i = c->slice_begin[task]; i < c->slice_end[task]; i++)
            counts[(c->packed[i] >> 32) & (c->slots - 1)]++;
    }
}
static void agg_index_merge_counts(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; agg_index_layout_t* c = raw;
    for (int64_t g = start; g < end; g++) {
        uint32_t part = g >> c->bits;
        int64_t count = 0;
        for (uint32_t task = c->part_slices[part]; task < c->part_slices[part + 1]; task++)
            count += c->local[task * c->local_stride + (g & (c->slots - 1))];
        c->counts[g] = count;
    }
}
static void agg_index_slice_cursors(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; agg_index_layout_t* c = raw;
    for (int64_t g = start; g < end; g++) {
        uint32_t part = g >> c->bits;
        int64_t offset = c->offsets[g];
        for (uint32_t task = c->part_slices[part]; task < c->part_slices[part + 1]; task++) {
            int64_t* slot = &c->local[task * c->local_stride + (g & (c->slots - 1))];
            int64_t count = *slot; *slot = offset; offset += count;
        }
    }
}
static void agg_index_fill(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; agg_index_layout_t* c = raw;
    for (int64_t task = start; task < end; task++) {
        int64_t* cursor = c->local + task * c->local_stride;
        for (int64_t i = c->slice_begin[task]; i < c->slice_end[task]; i++) {
            uint64_t record = c->packed[i];
            c->rows[cursor[(record >> 32) & (c->slots - 1)]++] = (uint32_t)record;
        }
    }
}
/* For a small group directory, bounded task-local counters avoid packing
 * another row buffer. Source-task prefixes preserve order within each group. */
static void agg_index_direct_count(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; agg_index_layout_t* c = raw;
    for (int64_t task = start; task < end; task++) {
        int64_t* counts = c->local + task * c->slots;
        int64_t begin = c->nrows / c->tasks * task;
        int64_t limit = task + 1 == c->tasks ? c->nrows : c->nrows / c->tasks * (task + 1);
        for (int64_t r = begin; r < limit; r++) counts[c->gids[r]]++;
    }
}
static void agg_index_direct_fill(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; agg_index_layout_t* c = raw;
    for (int64_t task = start; task < end; task++) {
        int64_t* cursor = c->local + task * c->slots;
        int64_t begin = c->nrows / c->tasks * task;
        int64_t limit = task + 1 == c->tasks ? c->nrows : c->nrows / c->tasks * (task + 1);
        for (int64_t r = begin; r < limit; r++) c->rows[cursor[c->gids[r]]++] = r;
    }
}
static int agg_index_layout(const agg_groups_t* groups, int64_t nrows,
        int64_t* counts, int64_t* offsets, int64_t* cursor, int64_t* rows) {
    ray_pool_t* pool = ray_pool_get();
    int64_t ng = groups->ngroups;
    uint32_t direct_tasks = pool ? ray_pool_total_workers(pool) * 4 : 0;
    if (direct_tasks > RAY_POOL_INIT_TASKS) direct_tasks = RAY_POOL_INIT_TASKS;
    /* Cap replicated counters at 256 KiB, including cache-line padding. */
    int64_t direct_slots = ((ng + 7) & ~INT64_C(7)) + 8;
    if (ray_pool_par_dispatch_ok(pool, nrows, RAY_PARALLEL_THRESHOLD) && direct_slots > 0 &&
            direct_tasks && direct_slots <= 32768 / direct_tasks) {
        agg_index_layout_t direct = {.gids = groups->gids, .nrows = nrows,
            .tasks = direct_tasks, .slots = direct_slots, .rows = rows};
        direct.local = ray_calloc_raw((size_t)direct_tasks * direct_slots * sizeof(int64_t));
        if (!direct.local) return -1;
        ray_pool_dispatch_n(pool, agg_index_direct_count, &direct, direct_tasks);
        if (agg_cancelled()) { ray_free_raw(direct.local); return -1; }
        for (int64_t g = 0; g < ng; g++) {
            int64_t at = offsets[g];
            for (uint32_t task = 0; task < direct_tasks; task++) {
                int64_t* slot = direct.local + task * direct_slots + g;
                int64_t count = *slot; *slot = at; at += count;
            }
            counts[g] = at - offsets[g]; offsets[g + 1] = at;
        }
        ray_pool_dispatch_n(pool, agg_index_direct_fill, &direct, direct_tasks);
        ray_free_raw(direct.local);
        return agg_cancelled() ? -1 : 0;
    }
    /* Packed row addresses use 32 bits; retain full-width indices above it. */
    /* Histogram/packing adds two full row passes. With fewer than four
     * workers, the direct count/fill layout uses less time and memory. */
    if (nrows > UINT32_MAX || !ray_pool_par_dispatch_ok(pool, nrows, RAY_PARALLEL_THRESHOLD) ||
            ray_pool_total_workers(pool) < 4) {
        for (int64_t r = 0; r < nrows; r++) counts[groups->gids[r]]++;
        for (int64_t i = 0; i < ng; i++) offsets[i + 1] = offsets[i] + counts[i];
        memcpy(cursor, offsets, (size_t)ng * sizeof(int64_t));
        for (int64_t r = 0; r < nrows; r++) rows[cursor[groups->gids[r]]++] = r;
        return 0;
    }
    /* One source range per worker avoids scattering tiny adjacent ranges
     * into the same pages. Partition slices supply additional skew tasks. */
    uint32_t tasks = ray_pool_total_workers(pool);
    if (tasks > RAY_POOL_INIT_TASKS) tasks = RAY_POOL_INIT_TASKS;
    uint32_t parts = 1;
    while (parts < tasks && parts < 256) parts *= 2;
    /* Adjacent group ids own adjacent output ranges. Partition by the high
     * bits so different workers do not scatter interleaved groups into the
     * same output pages. Hot partitions still split into source-order slices. */
    uint32_t slots = 1;
    while (slots < (ng + parts - 1) / parts) slots *= 2;
    agg_index_layout_t c = {.gids = groups->gids, .nrows = nrows, .tasks = tasks,
        .parts = parts, .bits = (uint32_t)__builtin_ctz(slots), .slots = slots, .local_stride = slots + 8,
        .offsets = offsets, .counts = counts, .cursor = cursor, .rows = rows};
    c.hist = ray_calloc_raw(((size_t)tasks * parts + parts + 1) * sizeof(int64_t));
    c.packed = ray_alloc_raw((size_t)nrows * sizeof(uint64_t));
    if (!c.hist || !c.packed) { ray_free_raw(c.hist); ray_free_raw(c.packed); return -1; }
    c.starts = c.hist + (size_t)tasks * parts;
    ray_pool_dispatch_n(pool, agg_index_hist, &c, tasks);
    int64_t offset = 0;
    for (uint32_t part = 0; part < parts; part++) {
        c.starts[part] = offset;
        for (uint32_t task = 0; task < tasks; task++) {
            size_t at = (size_t)task * parts + part;
            int64_t count = c.hist[at]; c.hist[at] = offset; offset += count;
        }
    }
    c.starts[parts] = offset;
    ray_profile_tick("indexed slices: histogram");
    ray_pool_dispatch_n(pool, agg_index_pack, &c, tasks);
    ray_profile_tick("indexed slices: packed");
    int rc = -1;
    if (agg_cancelled()) goto done;
    /* Split hot partitions by rows. There are at most 2*parts slices, each
     * storing a power-of-two group range plus a cache-line gap. This uses
     * at most four group slabs plus bounded task padding.
     * Packed rows and slice prefixes both retain source order. */
    int64_t grain = (nrows + parts - 1) / parts;
    for (uint32_t part = 0; part < parts; part++) {
        c.part_slices[part] = c.slices;
        for (int64_t begin = c.starts[part]; begin < c.starts[part + 1];) {
            int64_t end = c.starts[part + 1] - begin < grain ? c.starts[part + 1] : begin + grain;
            c.slice_begin[c.slices] = begin; c.slice_end[c.slices++] = end;
            begin = end;
        }
    }
    c.part_slices[parts] = c.slices;
    c.local = ray_calloc_raw((size_t)c.slices * c.local_stride * sizeof(int64_t));
    if (!c.local) goto done;
    ray_pool_dispatch_n(pool, agg_index_count, &c, c.slices);
    ray_pool_dispatch(pool, agg_index_merge_counts, &c, ng);
    if (agg_cancelled()) goto done;
    ray_profile_tick("indexed slices: counted groups");
    for (int64_t i = 0; i < ng; i++) offsets[i + 1] = offsets[i] + counts[i];
    ray_pool_dispatch(pool, agg_index_slice_cursors, &c, ng);
    ray_pool_dispatch_n(pool, agg_index_fill, &c, c.slices);
    ray_profile_tick("indexed slices: filled");
    if (!agg_cancelled()) rc = 0;
done:
    ray_free_raw(c.local); ray_free_raw(c.hist); ray_free_raw(c.packed);
    return rc;
}

/* Streaming consumers of the shared index use row-balanced tasks. Only
 * groups crossing task boundaries need partial states (at most two per task),
 * so a dominant group can use every worker without replicated group slabs. */
typedef struct {
    const agg_vtable_t* vt;
    ray_t* x;
    ray_t* y;
    const int64_t* rows;
    const int64_t* offsets;
    int64_t ng, nrows;
    uint32_t tasks;
    char* states;
    int64_t* partial_groups;
    _Atomic(bool) failed;
} agg_index_stream_t;
static void agg_index_stream_init(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; agg_index_stream_t* c = raw;
    for (int64_t g = start; g < end; g++) c->vt->init(c->states + g * c->vt->state_size);
}
static void agg_index_stream_reduce(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; agg_index_stream_t* c = raw;
    uint8_t ex = c->x ? col_esz(c->x) : 0, ey = c->y ? col_esz(c->y) : 0;
    char* x = ex ? ray_alloc_raw((size_t)AGG_SEL_CHUNK * ex) : NULL;
    char* y = ey ? ray_alloc_raw((size_t)AGG_SEL_CHUNK * ey) : NULL;
    if ((ex && !x) || (ey && !y)) { atomic_store(&c->failed, true); goto done; }
    uint32_t gids[AGG_SEL_CHUNK] = {0};
    ray_valid_t vx = {x, c->x ? c->x->type : RAY_I64, c->x && ray_vec_may_have_nulls(c->x)};
    ray_valid_t vy = {y, c->y ? c->y->type : RAY_I64, c->y && ray_vec_may_have_nulls(c->y)};
    for (int64_t task = start; task < end; task++) {
        int64_t begin = c->nrows / c->tasks * task;
        int64_t limit = task + 1 == c->tasks ? c->nrows : c->nrows / c->tasks * (task + 1);
        int64_t lo = 0, hi = c->ng;
        while (lo < hi) { int64_t mid = lo + (hi - lo) / 2; if (c->offsets[mid + 1] <= begin) lo = mid + 1; else hi = mid; }
        int partial = 0;
        for (int64_t group = lo, at = begin; at < limit && !agg_cancelled(); group++) {
            int64_t stop = c->offsets[group + 1] < limit ? c->offsets[group + 1] : limit;
            bool whole = at == c->offsets[group] && stop == c->offsets[group + 1];
            int64_t slot = whole ? group : c->ng + 2 * task + partial;
            char* state = c->states + slot * c->vt->state_size;
            if (!whole) {
                c->partial_groups[2 * task + partial++] = group;
                c->vt->init(state);
            }
            while (at < stop) {
                int64_t n = stop - at < AGG_SEL_CHUNK ? stop - at : AGG_SEL_CHUNK;
                if (ex) agg_sel_gather_vals(x, ray_data(c->x), ex, c->rows + at, n);
                if (ey) agg_sel_gather_vals(y, ray_data(c->y), ey, c->rows + at, n);
                if (c->y) c->vt->update_batch2(state, c->vt->state_size, gids, x, y, &vx, &vy, n, NULL);
                else c->vt->update_batch(state, c->vt->state_size, gids, x, &vx, n, NULL);
                at += n;
            }
        }
    }
done:
    ray_free_raw(x); ray_free_raw(y);
}
static ray_t* agg_index_streaming(const agg_vtable_t* vt, ray_t* x, ray_t* y,
        const agg_groups_t* groups, const int64_t* rows, const int64_t* offsets,
        int64_t nrows, int64_t param) {
    ray_pool_t* pool = ray_pool_get();
    if (!ray_pool_par_dispatch_ok(pool, nrows, RAY_PARALLEL_THRESHOLD))
        return y ? agg_run_one_bin(vt, x, y, groups->gids, nrows, groups->ngroups, param)
                 : agg_run_one(vt, x, groups->gids, nrows, groups->ngroups, param);
    uint32_t tasks = ray_pool_total_workers(pool) * 4;
    if (tasks > RAY_POOL_INIT_TASKS) tasks = RAY_POOL_INIT_TASKS;
    agg_index_stream_t c = {.vt = vt, .x = x, .y = y, .rows = rows, .offsets = offsets,
        .ng = groups->ngroups, .nrows = nrows, .tasks = tasks, .failed = false};
    c.states = ray_alloc_raw((size_t)(c.ng + 2 * tasks) * vt->state_size);
    c.partial_groups = ray_alloc_raw((size_t)2 * tasks * sizeof(int64_t));
    ray_t* result = NULL;
    if (!c.states || !c.partial_groups) goto done;
    for (uint32_t t = 0; t < 2 * tasks; t++) c.partial_groups[t] = -1;
    ray_pool_dispatch(pool, agg_index_stream_init, &c, c.ng);
    ray_pool_dispatch_n(pool, agg_index_stream_reduce, &c, tasks);
    if (agg_cancelled() || atomic_load(&c.failed)) goto done;
    for (uint32_t t = 0; t < 2 * tasks; t++) if (c.partial_groups[t] >= 0)
        vt->merge(c.states + c.partial_groups[t] * vt->state_size,
                  c.states + (c.ng + t) * vt->state_size, NULL);
    result = ray_vec_new(vt->out_type, c.ng);
    if (!result || RAY_IS_ERR(result)) goto done;
    result->len = c.ng;
    agg_dense_emit_ctx_t emit = {.vt = vt, .out = result, .states = c.states,
        .block = vt->state_size, .param = param, .any_null = false};
    ray_pool_dispatch(pool, agg_dense_emit_fn, &emit, c.ng);
    if (atomic_load(&emit.any_null)) result->attrs |= RAY_ATTR_HAS_NULLS;
done:
    ray_free_raw(c.states); ray_free_raw(c.partial_groups);
    if (agg_cancelled()) { ray_release(result); return ray_error("cancel", NULL); }
    return result ? result : ray_error("oom", NULL);
}

typedef struct {
    ray_t* src;
    uint16_t kind;
    ray_group_sym_view_t symbols;
} agg_index_winners_t;
static int64_t agg_index_winner(void* raw, const int64_t* rows, int64_t count) {
    agg_index_winners_t* c = raw;
    const void* data = ray_data(c->src);
    if (c->kind == OP_FIRST || c->kind == OP_LAST) {
        bool last = c->kind == OP_LAST;
        if (!count || ray_interrupted()) return -1;
        if (!ray_vec_may_have_nulls(c->src) && c->src->type != RAY_SYM && c->src->type != RAY_STR)
            return rows[last ? count - 1 : 0];
#define INDEX_FIRST_VALID(TYPE, VALID) do { \
            const TYPE* values = data; \
            for (int64_t j = 0; j < count; j++) { \
                if ((j & 65535) == 0 && ray_interrupted()) return -1; \
                int64_t row = rows[last ? count - 1 - j : j]; \
                TYPE value = values[row]; \
                if (VALID) return row; \
            } \
            return -1; \
        } while (0)
        switch (c->src->type) {
            case RAY_I16: INDEX_FIRST_VALID(int16_t, value != NULL_I16);
            case RAY_I32: case RAY_DATE: case RAY_TIME:
                INDEX_FIRST_VALID(int32_t, value != NULL_I32);
            case RAY_I64: case RAY_TIMESTAMP:
                INDEX_FIRST_VALID(int64_t, value != NULL_I64);
            case RAY_F32: INDEX_FIRST_VALID(float, value == value);
            case RAY_F64: INDEX_FIRST_VALID(double, value == value);
            default: break;
        }
#undef INDEX_FIRST_VALID
    }
    int64_t best = -1;
    for (int64_t j = 0; j < count; j++) {
        if ((j & 65535) == 0 && ray_interrupted()) return -1;
        int64_t r = rows[c->kind == OP_LAST ? count - 1 - j : j];
        if (ray_vec_is_null(c->src, r)) continue;
        if (best < 0) best = r;
        if (c->kind == OP_FIRST || c->kind == OP_LAST) break;
        ray_t* x = ray_group_sym_read(&c->symbols, ray_sym_vec_domain(c->src), ray_read_sym(data, r, c->src->type, c->src->attrs));
        ray_t* y = ray_group_sym_read(&c->symbols, ray_sym_vec_domain(c->src), ray_read_sym(data, best, c->src->type, c->src->attrs));
        int cmp = ray_str_cmp(x, y);
        if (c->kind == OP_MIN ? cmp < 0 : cmp > 0) best = r;
    }
    return best;
}

typedef struct {
    const int64_t* rows;
    const int64_t* offsets;
    const int64_t* counts;
    ray_t** values;
    _Atomic(bool) failed;
} agg_index_vectors_t;
static void agg_index_vectors_run(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; agg_index_vectors_t* c = raw;
    for (int64_t g = start; g < end && !agg_cancelled(); g++) {
        ray_t* value = ray_vec_new(RAY_I64, c->counts[g]);
        if (!value || RAY_IS_ERR(value)) { ray_release(value); atomic_store(&c->failed, true); return; }
        value->len = c->counts[g];
        memcpy(ray_data(value), c->rows + c->offsets[g], (size_t)value->len * sizeof(int64_t));
        c->values[g] = value;
    }
}
ray_t* agg_group_indices(ray_t* source) {
    ray_pool_t* pool = ray_pool_get();
    int64_t n = source->len;
    if (n > INT32_MAX || !ray_pool_par_dispatch_ok(pool, n, RAY_PARALLEL_THRESHOLD) ||
            ray_pool_total_workers(pool) < 4) return NULL;
    switch (source->type) {
        case RAY_BOOL: case RAY_U8: case RAY_I16: case RAY_I32: case RAY_I64:
        case RAY_F32: case RAY_F64: case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP:
        case RAY_SYM: case RAY_GUID: case RAY_STR: case RAY_LIST: break;
        default: return NULL;
    }
    agg_groups_t groups = {0};
    dense_plan_t plan;
    bool dense = agg_dense_plan(&source, 1, NULL, 0, n, &plan);
    int rc = dense ? agg_group_keys_dense(&source, n, &plan, &groups)
                   : agg_group_keys(&source, 1, n, &groups);
    if (rc) return ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
    int64_t ng = groups.ngroups;
    int64_t* scratch = ray_calloc_raw((size_t)(n + 3 * ng + 1) * sizeof(int64_t));
    if (!scratch) { agg_groups_free(&groups); return ray_error("oom", NULL); }
    int64_t* offsets = scratch + ng;
    int64_t* cursor = offsets + ng + 1;
    int64_t* rows = cursor + ng;
    ray_t* keys = NULL;
    ray_t* values = NULL;
    ray_t* result = NULL;
    if (agg_index_layout(&groups, n, scratch, offsets, cursor, rows)) goto failed;
    keys = ray_group_gather(source, groups.first_row, ng);
    if (!keys || RAY_IS_ERR(keys)) goto failed;
    values = ray_list_new(ng);
    if (!values || RAY_IS_ERR(values)) goto failed;
    memset(ray_data(values), 0, (size_t)ng * sizeof(ray_t*));
    values->len = ng;
    agg_index_vectors_t emit = {rows, offsets, scratch, ray_data(values), false};
    ray_group_dispatch(agg_index_vectors_run, &emit, scratch, ng);
    if (agg_cancelled() || atomic_load(&emit.failed)) goto failed;
    result = ray_dict_new(keys, values);
    keys = values = NULL;
failed:
    ray_release(keys); ray_release(values); ray_free_raw(scratch); agg_groups_free(&groups);
    return result ? result : ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
}

/* O(rows + groups) shared layout. Holistic helpers allocate disjoint group
 * scratch; no worker owns a second full input or a second grouping. */
static ray_t* agg_indexed_run(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                             ray_t** keys, int64_t* key_syms, int64_t nrows) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    agg_groups_t groups = {0};
    dense_plan_t dp;
    bool dense = agg_dense_plan(keys, ext->n_keys, NULL, 0, nrows, &dp);
    route_stats.dense_plan_available = dense;
    int rc = dense ? agg_group_keys_dense(keys, nrows, &dp, &groups)
                   : agg_group_keys(keys, ext->n_keys, nrows, &groups);
    if (rc) return ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
    if (agg_cancelled()) { agg_groups_free(&groups); return ray_error("cancel", NULL); }
    int64_t ng = groups.ngroups;
    int64_t* counts = ray_calloc_raw((size_t)(ng + 1) * sizeof(int64_t));
    int64_t* offsets = ray_calloc_raw((size_t)(ng + 1) * sizeof(int64_t));
    int64_t* cursor = ray_calloc_raw((size_t)(ng + 1) * sizeof(int64_t));
    int64_t* rows = ray_alloc_raw((size_t)(nrows + 1) * sizeof(int64_t));
    ray_t* result = NULL;
    if (!counts || !offsets || !cursor || !rows) { result = ray_error("oom", NULL); goto done; }
    ray_profile_tick("indexed: grouped keys");
    if (agg_index_layout(&groups, nrows, counts, offsets, cursor, rows)) {
        result = ray_error(agg_cancelled() ? "cancel" : "oom", NULL); goto done;
    }
    ray_profile_tick("indexed: stable row slices");
    result = ray_table_new(ext->n_keys + ext->n_aggs);
    if (!result || RAY_IS_ERR(result)) goto done;
    for (uint32_t k = 0; k < ext->n_keys; k++) {
        ray_t* col = ray_group_gather(keys[k], groups.first_row, ng);
        if (!col || RAY_IS_ERR(col)) { ray_release(result); result = col; goto done; }
        if (col->type == RAY_F32 || col->type == RAY_F64) {
            for (int64_t i = 0; i < ng; i++) {
                if (col->type == RAY_F32) {
                    float* d = ray_data(col); if (d[i] == 0) memset(&d[i], 0, sizeof(float));
                } else {
                    double* d = ray_data(col); if (d[i] == 0) memset(&d[i], 0, sizeof(double));
                }
            }
        }
        result = ray_table_add_col(result, key_syms[k], col); ray_release(col);
        if (!result || RAY_IS_ERR(result)) goto done;
    }
    ray_profile_tick("indexed: emitted keys");
    for (uint32_t a = 0; a < ext->n_aggs; a++) {
        if (agg_cancelled()) { ray_release(result); result = ray_error("cancel", NULL); goto done; }
        uint16_t kind = ext->agg_ops[a];
        ray_op_ext_t* ie = find_ext(g, ext->agg_ins[a]);
        ray_t* src = ie ? ray_table_get_col(tbl, ie->sym) : NULL;
        int64_t param = ext->agg_k ? ext->agg_k[a] : 0;
        ray_t* col = NULL;
        if (kind == OP_MEDIAN) col = ray_median_per_group_buf(src, rows, offsets, counts, ng);
        else if (kind == OP_QUANTILE) {
            double q = 0.5; if (ext->agg_k) memcpy(&q, &param, sizeof(q));
            col = ray_quantile_per_group_buf(src, rows, offsets, counts, ng, q);
        } else if (kind == OP_MODE) col = ray_mode_per_group_buf(src, rows, offsets, counts, ng);
        else if (kind == OP_TOP_N || kind == OP_BOT_N)
            col = ray_topk_per_group_buf(src, param, kind == OP_TOP_N, rows, offsets, counts, ng);
        else if (agg_indexed_supported(kind, src ? src->type : RAY_I64)) {
            col = ray_wide_minmax_per_group_buf(src, kind, rows, offsets, counts, ng);
            if (!col) {
                /* Native first/last and lexical SYM extrema retain the source
                 * column's domain by gathering winning original row indices. */
                int64_t* winners = cursor;
                agg_index_winners_t work = {src, kind, {0}};
                if (src->type == RAY_SYM)
                    ray_sym_strings_borrow(&work.symbols.strings, &work.symbols.count);
                ray_group_winners(agg_index_winner, &work, rows, offsets, counts, ng, winners);
                if (agg_cancelled()) { ray_release(result); result = ray_error("cancel", NULL); goto done; }
                ray_profile_tick("indexed: selected winning rows");
                col = ray_group_gather(src, winners, ng);
                /* F32 reduction results follow the existing F64 contract. */
                if (col && !RAY_IS_ERR(col) && src->type == RAY_F32) {
                    ray_t* widened = ray_vec_new(RAY_F64, ng);
                    if (widened && !RAY_IS_ERR(widened)) {
                        widened->len = ng;
                        for (int64_t i = 0; i < ng; i++) ((double*)ray_data(widened))[i] = ((float*)ray_data(col))[i];
                        widened->attrs |= col->attrs & RAY_ATTR_HAS_NULLS;
                    }
                    ray_release(col); col = widened;
                }
            }
        } else if (ext->agg_ins2 && ext->agg_ins2[a] != RAY_OP_NONE) {
            ray_op_ext_t* ye = find_ext(g, ext->agg_ins2[a]);
            col = agg_index_streaming(agg_resolve(kind, src->type), src,
                ray_table_get_col(tbl, ye->sym), &groups, rows, offsets, nrows, param);
        } else if (kind == OP_COUNT) {
            col = ray_vec_new(RAY_I64, ng);
            if (col && !RAY_IS_ERR(col)) { col->len = ng; memcpy(ray_data(col), counts, (size_t)ng * sizeof(int64_t)); }
        } else col = agg_index_streaming(agg_resolve(kind, src ? src->type : RAY_I64),
                                        src, NULL, &groups, rows, offsets, nrows, param);
        if (!col || RAY_IS_ERR(col)) { ray_release(result); result = col; goto done; }
        ray_profile_tick("indexed: emitted aggregate");
        result = ray_table_add_col(result, agg_result_col_name(ie ? ie->sym : 0, kind), col);
        ray_release(col);
        if (!result || RAY_IS_ERR(result)) goto done;
    }
done:
    if (result && !RAY_IS_ERR(result) && agg_cancelled()) { ray_release(result); result = ray_error("cancel", NULL); }
    ray_free_raw(counts); ray_free_raw(offsets); ray_free_raw(cursor); ray_free_raw(rows);
    agg_groups_free(&groups);
    return result ? result : ray_error("oom", NULL);
}

/* Shared extrema help when repeated keys let workers skip state writes.
 * A deterministic, stratified sample exercises the registered capability with
 * its own state, so this decision needs no aggregate/type-specific predicates.
 * Predominantly changing states use owned partitions instead of contended CAS.
 * The sample is query-local and never supplies an answer to execution. */
static bool agg_shared_sample(ray_graph_t* g, ray_op_ext_t* ext, ray_t* tbl,
        ray_t** keys, const agg_vo_t* vo, int64_t rows) {
    if (!ext->n_aggs) return true;
    if (rows < 1024) return false;
    enum { SAMPLES = 1024, CAP = 2048 };
    agg_desc_t d;
    if (!agg_desc_init(&d, g, ext, tbl, keys)) return false;
    char* states = ray_alloc_raw((SAMPLES + 1) * vo->block);
    if (!states) { agg_desc_free(&d); return false; }
    char* previous = states + SAMPLES * vo->block;
    int16_t map[CAP]; int64_t values[SAMPLES];
    memset(map, -1, sizeof(map));
    uint32_t gid = 0;
    int groups = 0, unchanged = 0;
    int64_t step = rows / SAMPLES;
    for (int i = 0; i < SAMPLES; i++) {
        uint64_t jitter = ray_hash_i64(i);
        int64_t row = i * step + jitter % step;
        int64_t value = agg_read_key_i64(keys[0], ray_data(keys[0]), row);
        uint32_t at = ray_hash_i64(value) & (CAP - 1);
        while (map[at] >= 0 && values[map[at]] != value) at = (at + 1) & (CAP - 1);
        if (map[at] < 0) {
            map[at] = groups; values[groups] = value;
            for (uint32_t a = 0; a < ext->n_aggs; a++) vo->vts[a]->init(states + groups * vo->block + vo->off[a]);
            groups++;
        }
        char* state = states + map[at] * vo->block;
        memcpy(previous, state, vo->block);
        for (uint32_t a = 0; a < ext->n_aggs; a++) {
            const void* input = d.val_data[a] ? (const char*)d.val_data[a] + (size_t)row * d.val_esz[a] : NULL;
            ray_valid_t valid = {input, d.val_types[a], d.val_hasnull[a]};
            vo->vts[a]->update_shared(state + vo->off[a], vo->block, &gid, input, &valid, 1);
        }
        unchanged += memcmp(previous, state, vo->block) == 0;
    }
    ray_free_raw(states); agg_desc_free(&d);
    return unchanged >= SAMPLES / 4;
}

/* Core of exec_group_v2: resolution + strategy dispatch over (tbl, nrows).
 *
 * Selection handling.  `sel` is the pushed WHERE filter's rowsel (or NULL).
 * When set, the CHUNKED strategies (parallel dense / radix / smallhash) consume
 * it IN PLACE: they chunk-decode the selected ORIGINAL rows and gather per chunk
 * (no full index array, no compact column).  `nrows` stays the FULL table row
 * count (dense min/max prescan + hash sizing operate on the source columns; the
 * selected rows are a subset whose slots/keys are a subset of the full range),
 * while `n_sel` is the number of selected rows used for the parallel-vs-serial
 * decision and the chunked dispatch extent.  `sel_prefix` is the per-segment
 * selected-count prefix used to partition selected-row space across workers.
 *
 * The HASH-FALLBACK strategy (F64/STR keys) and the SERIAL path keep the
 * COMPACT-table approach: those shapes were not perf blockers and are rare/small
 * (serial only fires below RAY_PARALLEL_THRESHOLD selected rows), so we build a
 * compact table of the selected rows once and recurse with sel=NULL — the
 * unmodified strategy then runs over the compact table.  This is the documented
 * compact fallback the design permits for the non-chunked shapes. */
static ray_t* exec_group_v2_run_inner(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                                int64_t nrows, ray_t* sel,
                                const int64_t* sel_prefix, int64_t n_sel,
                                int64_t group_limit,
                                const ray_group_emit_filter_t* efp);

/* Trim a full group result to the emit filter's keep set (the same decision
 * the native selections make, over the finished aggregate column).  Row
 * order is preserved.  Consumes `result`; a failure keeps the full result,
 * which is still a valid answer for the sort+take downstream. */
ray_t* agg_emit_filter_trim(ray_t* result, uint32_t n_keys, uint32_t n_aggs,
                            const ray_group_emit_filter_t* ef) {
    if (!result || RAY_IS_ERR(result) || result->type != RAY_TABLE) return result;
    if (ef->agg_index >= n_aggs) return result;
    int64_t nrows = ray_table_nrows(result);
    if (nrows <= 0) return result;
    ray_t* vcol = ray_table_get_col_idx(result, (int64_t)n_keys + ef->agg_index);
    if (!vcol || (vcol->type != RAY_I64 && vcol->type != RAY_F64)) return result;
    double* vals = ray_alloc_raw((size_t)nrows * sizeof(double));
    uint8_t* keep = ray_alloc_raw((size_t)nrows);
    ray_t* idx = ray_vec_new(RAY_I64, nrows);
    if (!vals || !keep || !idx || RAY_IS_ERR(idx)) {
        ray_free_raw(vals); ray_free_raw(keep);
        if (idx && RAY_IS_ERR(idx)) ray_error_free(idx); else ray_release(idx);
        return result;
    }
    /* Same null placement as agg_group_values_f64: below every value. */
    if (vcol->type == RAY_F64) {
        const double* vf = (const double*)ray_data(vcol);
        for (int64_t r = 0; r < nrows; r++) vals[r] = vf[r] != vf[r] ? -INFINITY : vf[r];
    } else {
        const int64_t* vi = (const int64_t*)ray_data(vcol);
        for (int64_t r = 0; r < nrows; r++) vals[r] = vi[r] == NULL_I64 ? -INFINITY : (double)vi[r];
    }
    int64_t kept = agg_topn_keep(vals, nrows, ef, keep);
    int64_t* ix = (int64_t*)ray_data(idx);
    int64_t w = 0;
    for (int64_t r = 0; r < nrows; r++) if (keep[r]) ix[w++] = r;
    idx->len = w;
    ray_free_raw(vals); ray_free_raw(keep);
    if (kept == nrows) { ray_release(idx); return result; }
    ray_t* out = ray_at_fn(result, idx);
    ray_release(idx);
    if (!out || RAY_IS_ERR(out)) { if (out) ray_error_free(out); return result; }
    ray_release(result);
    return out;
}

/* Every v2 strategy returns through here: a route that could not select
 * the emit filter's top-N itself hands back its full result and is trimmed
 * to the kept superset, so callers see one contract regardless of route. */
static ray_t* exec_group_v2_run(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                                int64_t nrows, ray_t* sel,
                                const int64_t* sel_prefix, int64_t n_sel,
                                int64_t group_limit,
                                const ray_group_emit_filter_t* efp) {
    ray_t* r = exec_group_v2_run_inner(g, op, tbl, nrows, sel, sel_prefix, n_sel,
                                       group_limit, efp);
    if (efp && r && !RAY_IS_ERR(r) && !route_stats.topn_native) {
        ray_op_ext_t* ext = find_ext(g, op->id);
        if (ext) r = agg_emit_filter_trim(r, ext->n_keys, ext->n_aggs, efp);
    }
    return r;
}

static ray_t* exec_group_v2_run_inner(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                                int64_t nrows, ray_t* sel,
                                const int64_t* sel_prefix, int64_t n_sel,
                                int64_t group_limit,
                                const ray_group_emit_filter_t* efp) {
    agg_route_reason(AGG_V2_ADMITTED);
    route_stats.topn_native = false;
    route_stats.nullable_key = false;
    route_stats.dense_plan_available = false;
    route_stats.dense_worker_budget = false;
    route_stats.dense_tasks = 0;
    ray_op_ext_t* ext = find_ext(g, op->id);
    /* The top-N emit filter names an aggregate slot; an out-of-range slot or a
     * filter armed for a different node means "no filter" here (the caller's
     * sort+take still produces the final answer from the full result). */
    if (efp && (!efp->enabled || efp->agg_index >= ext->n_aggs)) efp = NULL;

    /* Exact-size carve for the per-key column pointers + syms (one block, both
     * 8-byte): unbounded key count, freed at every exit of this function
     * (parallel branches free it beside agg_vo_free; the serial tail frees it
     * after the key-emit loop; the compact-fallback macro frees it too). */
    ray_t* kc_hdr;
    ray_t** key_cols = (ray_t**)scratch_alloc(&kc_hdr,
        (size_t)ext->n_keys * (sizeof(ray_t*) + sizeof(int64_t)));
    if (!key_cols) return ray_error("oom", NULL);
    int64_t* key_syms = (int64_t*)(key_cols + ext->n_keys);
    for (uint32_t k = 0; k < ext->n_keys; k++) {
        ray_op_ext_t* kext = find_ext(g, ext->keys[k]);
        key_cols[k] = ray_table_get_col(tbl, kext->sym);
        key_syms[k] = kext->sym;
        if (key_cols[k]->type != RAY_SYM && ray_vec_may_have_nulls(key_cols[k]))
            route_stats.nullable_key = true;
    }

    bool indexed = false;
    for (uint32_t k = 0; k < ext->n_keys; k++) {
        int8_t t = key_cols[k]->type;
        if (t == RAY_F32 || t == RAY_F64 || t == RAY_GUID || t == RAY_STR || t == RAY_LIST) indexed = true;
    }
    for (uint32_t a = 0; a < ext->n_aggs; a++) {
        ray_op_ext_t* ie = find_ext(g, ext->agg_ins[a]);
        ray_t* col = ie ? ray_table_get_col(tbl, ie->sym) : NULL;
        if (col && agg_indexed_supported(ext->agg_ops[a], col->type)) indexed = true;
    }
    if (indexed) {
        if (sel) {
            ray_t* idx = ray_rowsel_to_indices(sel);
            if (!idx || RAY_IS_ERR(idx)) { scratch_free(kc_hdr); return idx ? idx : ray_error("oom", NULL); }
            ray_t* compact = agg_build_compact(g, op, tbl, ray_data(idx), n_sel);
            ray_t* result = compact && !RAY_IS_ERR(compact)
                ? exec_group_v2_run(g, op, compact, n_sel, NULL, NULL, 0, group_limit, efp) : compact;
            if (compact && !RAY_IS_ERR(compact)) ray_release(compact);
            ray_release(idx); scratch_free(kc_hdr); return result;
        }
        agg_route_record(AGG_ROUTE_V2_INDEXED);
        ray_t* result = agg_indexed_run(g, op, tbl, key_cols, key_syms, nrows);
        scratch_free(kc_hdr); return result;
    }

    /* Precompute AoS state layout for the admitted aggregates. */
    agg_vo_t vo;
    if (!agg_vo_init(&vo, g, ext, tbl)) { scratch_free(kc_hdr); return ray_error("oom", NULL); }
    const agg_vtable_t** vts = vo.vts; size_t* off = vo.off; size_t block = vo.block;

    /* Dense plan (low-card int/SYM keys + streaming aggs → direct index, no
     * hash).  Computed up front so both the parallel and serial dispatch can use
     * it.  In sel mode the min/max prescan walks only the SELECTED rows (O(n_sel),
     * not O(nrows)) — both correct (grouped keys ARE the selected rows) and cheap
     * for high-selectivity filters, and it can pull a sparse-but-low-card selected
     * key set into the dense path that the full-table range would have rejected. */
    dense_plan_t dp;
    bool dense = sel ? agg_dense_plan_sel(key_cols, ext->n_keys, n_sel, sel, sel_prefix, &dp)
                     : agg_dense_plan(key_cols, ext->n_keys, vts, ext->n_aggs, nrows, &dp);
    route_stats.dense_plan_available = dense;

    /* Compact-fallback helper for the non-chunked shapes (hash-fallback keys,
     * and the serial path): gather selected rows into a compact table once and
     * recurse with sel=NULL so the unmodified strategy runs over it. */
    #define AGG_RUN_COMPACT_FALLBACK()                                          \
        do {                                                                    \
            agg_vo_free(&vo);   /* not needed by the compact recursion */       \
            agg_dense_plan_free(&dp); scratch_free(kc_hdr);  /* key_cols dead: the recursion rebuilds them */ \
            ray_t* idxb = ray_rowsel_to_indices(sel);                           \
            if (!idxb) return ray_error("oom", NULL);                           \
            ray_t* compact = agg_build_compact(g, op, tbl, (int64_t*)ray_data(idxb), n_sel); \
            if (!compact || RAY_IS_ERR(compact)) {                              \
                ray_release(idxb);                                              \
                return compact ? compact : ray_error("oom", NULL);             \
            }                                                                   \
            ray_t* r = exec_group_v2_run(g, op, compact, n_sel, NULL, NULL, 0,   \
                                         group_limit, efp);                     \
            ray_release(compact); ray_release(idxb);                            \
            return r;                                                           \
        } while (0)

    /* Effective row count for the parallel-vs-serial decision: the selected
     * count when a filter is active, else the full table. */
    int64_t eff_n = sel ? n_sel : nrows;

    /* Buffered accumulators retain every contributing value per group.  Running
     * them through the parallel radix strategy overlaps those buffers with the
     * full scatter payload and can exhaust the heap on large inputs.  Keep
     * buffered shapes on the shared indexed driver; streaming shapes retain
     * the dense/radix strategies below. */
    bool all_streaming = true;
    for (uint32_t a = 0; a < ext->n_aggs && all_streaming; a++)
        if (vts[a]->kind != ACC_STREAMING) all_streaming = false;

    ray_pool_t* pool = ray_pool_get();
    if (pool && eff_n >= RAY_PARALLEL_THRESHOLD && all_streaming) {
        /* A parallel dense plan owns one slot slab per worker. Admit it only
         * when the aggregate slot count across all workers remains O(input);
         * this is data-derived and independent of cache or RAM size. */
        uint32_t dense_workers = ray_pool_total_workers(pool);
        double scatter_budget = (double)eff_n * 2 * (8.0 * (ext->n_keys + ext->n_aggs + 1));
        int64_t watermark = ray_heap_anon_watermark();
        double dense_budget = scatter_budget;
        if (dense_budget > (double)SIZE_MAX) dense_budget = (double)SIZE_MAX;
        if (watermark > 0 && dense_budget > (double)watermark / 4) dense_budget = (double)watermark / 4;
        double slab_bytes = dp.ok ? (double)dp.total_slots * (block + sizeof(int64_t) + 1) : 0;
        /* Replicated slabs must stay resident in the last-level cache.  Each
         * task updates random slots of its own slab, so once the slabs
         * together outgrow the LLC every update misses to DRAM and adding
         * tasks makes the query slower: 100k groups over 10M rows measured
         * 6 ms with 8 slabs (26 MB inside a 33 MB L3) and 21 ms with 28
         * slabs (92 MB) on the same 28-thread pool.  Bound the number of
         * replicated task slabs by three quarters of the LLC (the rest
         * streams the input); an unreported cache assumes 32 MB.  The
         * memory budgets below remain data-derived; this bound only limits
         * replication, and larger domains use partition ownership, which
         * scales with cores because every partition slab is L1-sized. */
        uint64_t llc_bytes = ray_cache_llc_bytes();
        double cache_budget = (llc_bytes ? (double)llc_bytes : 32.0 * 1048576.0) * 0.75;
        uint32_t cache_tasks = dense_workers;
        if (slab_bytes > 0 && slab_bytes * dense_workers > cache_budget) {
            double fit = cache_budget / slab_bytes;
            cache_tasks = fit >= 1 ? (uint32_t)fit : 0;
        }
        /* A bounded group emit selects first-seen groups. Dense slot order
         * cannot satisfy that contract; retain radix's bounded selection. */
        bool shared_plan = dp.ok && group_limit <= 0 && !sel && ext->n_keys == 1 && dp.total_slots >= 4096;
        for (uint32_t a = 0; a < ext->n_aggs && shared_plan; a++)
            shared_plan = vts[a]->update_shared != NULL;
        if (shared_plan && slab_bytes <= dense_budget && agg_shared_sample(g, ext, tbl, key_cols, &vo, nrows)) {
            route_stats.dense_worker_budget = false;
            agg_route_record(AGG_ROUTE_V2_DENSE);
            ray_t* result = exec_group_v2_parallel_dense(g, op, tbl, key_cols, key_syms, ext,
                nrows, pool, &dp, dense_workers, AGG_DENSE_SHARED, sel, sel_prefix, n_sel, efp, 0);
            agg_vo_free(&vo); agg_dense_plan_free(&dp); scratch_free(kc_hdr);
            return result;
        }
        /* Partition ownership amortizes scatter through concurrent reducers.
         * With one worker, direct task-local updates avoid that extra payload. */
        if (dp.ok && dense_workers > 1 && group_limit <= 0 && eff_n <= UINT32_MAX && dp.total_slots >= 4096) {
            uint32_t sources = dense_workers;
            if (sources > RAY_POOL_INIT_TASKS / 2) sources = RAY_POOL_INIT_TASKS / 2;
            uint32_t parts = agg_dense_partition_parts(dense_workers, dp.total_slots);
            uint32_t split_budget = sources * 4 < parts ? sources * 4 : parts;
            int64_t part_slots = ((dp.total_slots + parts - 1) / parts + 7) & ~INT64_C(7);
            /* Splitting adds at most one global slab. Shared input columns
             * occupy one field in the payload, including mixed binary uses. */
            agg_desc_t payload;
            double record_size = (double)SIZE_MAX;
            if (agg_desc_init(&payload, g, ext, tbl, key_cols)) {
                record_size = agg_partition_record(&payload, ext->n_aggs, NULL, NULL);
                agg_desc_free(&payload);
            }
            double bytes = 2.0 * part_slots * parts * (block + sizeof(int64_t))
                + (double)eff_n * (sizeof(uint32_t) + record_size + (sel ? sizeof(int64_t) : 0))
                + (double)(sources + 1) * parts * sizeof(uint64_t)
                + (double)(parts + split_budget) * sizeof(agg_dense_partition_task_t);
            /* Small per-worker slabs are cheaper than another full payload
             * pass. Prefer partition ownership only once replicated state
             * traffic exceeds its row traffic; large pools/ranges still use
             * bounded shared storage. */
            uint32_t local_tasks = cache_tasks < dense_workers ? cache_tasks : dense_workers;
            double local_traffic = local_tasks * slab_bytes;
            double partition_traffic = (double)eff_n * (sizeof(uint32_t) + record_size);
            /* Compare the complete partition allocation against radix's
             * payload plus its worst-case per-row group state. A payload-only
             * budget unnecessarily rejects dense high-cardinality domains,
             * although radix must allocate the same aggregate states too. */
            double partition_budget = scatter_budget + (double)eff_n * (block + sizeof(int64_t));
            if (partition_budget > (double)SIZE_MAX) partition_budget = (double)SIZE_MAX;
            if (watermark > 0 && partition_budget > (double)watermark / 4)
                partition_budget = (double)watermark / 4;
            /* Fewer than three cache-resident slabs cannot use the pool;
             * partition ownership keeps every core busy instead. */
            bool cache_starved = local_tasks < 3 && local_tasks < dense_workers;
            if (bytes <= partition_budget && (local_traffic > partition_traffic || cache_starved)) {
                route_stats.dense_worker_budget = false;
                agg_route_record(AGG_ROUTE_V2_DENSE);
                ray_t* result = exec_group_v2_parallel_dense(g, op, tbl, key_cols, key_syms, ext,
                    nrows, pool, &dp, dense_workers, AGG_DENSE_PARTITIONED, sel, sel_prefix, n_sel, efp, 0);
                agg_vo_free(&vo); agg_dense_plan_free(&dp); scratch_free(kc_hdr);
                return result;
            }
        }
        if (dp.ok && slab_bytes > 0) {
            double fit = (dense_budget - (double)eff_n * sizeof(uint32_t)) / slab_bytes - 1;
            if (fit < dense_workers) dense_workers = fit >= 2 ? (uint32_t)fit : 0;
            if (cache_tasks < dense_workers) dense_workers = cache_tasks >= 2 ? cache_tasks : 0;
        }
        /* A bounded group emit (unordered take: N) selects the first N groups
         * in first-seen order.  Task-local slabs keep a true first row per
         * slot, so their finish can select those groups; the shared and
         * partition strategies above keep no first rows and stay gated. */
        bool dense_par_ok = dp.ok && dense_workers > 0;
        /* Allocation size alone misses repeated wide-range worker updates.
         * Estimate touched slot traffic from evenly spaced key samples in each
         * worker-sized input range. Prefer radix when duplicated state traffic
         * exceeds one scatter payload. This affects strategy only, never values;
         * selections keep their existing exact selected-range planning. */
        if (dense_par_ok && !sel && ext->n_keys == 1) {
            const void* data = ray_data(key_cols[0]);
            double touched_slots = 0;
            for (uint32_t w = 0; w < dense_workers; w++) {
                int64_t start = nrows / dense_workers * w;
                int64_t end = w + 1 == dense_workers ? nrows : nrows / dense_workers * (w + 1);
                int64_t samples = end - start < 1024 ? end - start : 1024;
                int64_t lo = INT64_MAX, hi = INT64_MIN;
                for (int64_t i = 0; i < samples; i++) {
                    int64_t r = start + i * ((end - start - 1) / (samples > 1 ? samples - 1 : 1));
                    int64_t v = agg_read_key_i64(key_cols[0], data, r);
                    if (dp.nullable[0] && v == dp.nulls[0]) continue;
                    if (v < lo) lo = v;
                    if (v > hi) hi = v;
                }
                if (lo <= hi) touched_slots += (double)((uint64_t)hi - (uint64_t)lo) + 1;
            }
            if (touched_slots * (block + sizeof(int64_t) + 1) > scatter_budget / 2)
                dense_par_ok = false;
        }
        route_stats.dense_worker_budget = dp.ok && !dense_par_ok;
        if (route_stats.nullable_key) ray_profile_tick("group: nullable key");
        if (route_stats.dense_worker_budget) ray_profile_tick("group: dense worker budget exceeded");

        /* RADIX eligibility: every key an int/SYM type with no nulls (same
         * type-set check as agg_dense_plan).  Radix takes the high-card
         * remainder of streaming int-key queries (dense handles the low-card
         * head). Hash handles F64 / STR keys. */
        bool keys_intsym = true;
        for (uint32_t k = 0; k < ext->n_keys && keys_intsym; k++) {
            ray_t* kc = key_cols[k];
            switch (kc->type) {
                case RAY_I64: case RAY_I32: case RAY_I16: case RAY_U8:
                case RAY_BOOL: case RAY_DATE: case RAY_TIME:
                case RAY_TIMESTAMP: case RAY_SYM: break;
                default: keys_intsym = false;
            }
            /* Signed null sentinels are distinct canonical radix keys. */
        }

        if (dense_par_ok) {
            /* Small domains can afford more independent source tasks. This
             * lets work stealing absorb uneven worker throughput without
             * multiplying large state slabs or retaining prior query state. */
            uint32_t extra_tasks = dense_workers * 4;
            if (extra_tasks > RAY_POOL_INIT_TASKS) extra_tasks = RAY_POOL_INIT_TASKS;
            if (dense_workers > 1 && extra_tasks * slab_bytes <= scatter_budget / 8 &&
                    extra_tasks * slab_bytes <= cache_budget &&
                    (extra_tasks + 1.0) * slab_bytes + (double)eff_n * sizeof(uint32_t) <= dense_budget)
                dense_workers = extra_tasks;
            route_stats.dense_tasks = dense_workers;
            route_stats.dense_local_slots = 0;
            for (uint32_t w = 0; w < dense_workers; w++)
                route_stats.dense_local_slots += dp.total_slots;
            agg_route_record(AGG_ROUTE_V2_DENSE);
            ray_t* r = exec_group_v2_parallel_dense(g, op, tbl, key_cols, key_syms, ext, nrows, pool, &dp, dense_workers, AGG_DENSE_TASK_LOCAL,
                                                    sel, sel_prefix, n_sel, efp, group_limit);
            agg_vo_free(&vo); agg_dense_plan_free(&dp); scratch_free(kc_hdr); return r;
        }
        if (keys_intsym) {
            agg_route_record(AGG_ROUTE_V2_RADIX);
            /* Sparse ranges and excessive dense worker traffic use radix. */
            ray_t* r = exec_group_v2_parallel_radix(g, op, tbl, nrows,
                    key_cols, key_syms, vts, off, block, sel, sel_prefix, n_sel,
                    group_limit, efp);
            agg_vo_free(&vo); agg_dense_plan_free(&dp); scratch_free(kc_hdr); return r;
        }
        /* Hash fallback (F64 / STR keys): not a chunked strategy — compact. */
        if (sel) AGG_RUN_COMPACT_FALLBACK();
        agg_route_record(AGG_ROUTE_V2_INDEXED);
        { ray_t* r = agg_indexed_run(g, op, tbl, key_cols, key_syms, nrows);
          agg_vo_free(&vo); agg_dense_plan_free(&dp); scratch_free(kc_hdr); return r; }
    }

    /* Serial path does not consult the vts/off tables (per-agg vt is re-resolved
     * below), so release the layout carve before it runs. */
    agg_vo_free(&vo);

    /* Serial path: keep the compact fallback when a filter is active (selected
     * count is below the parallel threshold here → small; not a perf blocker). */
    if (sel) AGG_RUN_COMPACT_FALLBACK();

    agg_route_record(dense ? AGG_ROUTE_V2_SERIAL_DENSE : AGG_ROUTE_V2_SERIAL_HASH);
    agg_groups_t groups = {0};
    int grp_rc = dense ? agg_group_keys_dense(key_cols, nrows, &dp, &groups)
                       : agg_group_keys(key_cols, ext->n_keys, nrows, &groups);
    if (grp_rc != 0) { agg_dense_plan_free(&dp); scratch_free(kc_hdr); return ray_error("oom", NULL); }

    ray_t* result = ray_table_new(ext->n_keys + ext->n_aggs);
    if (!result || RAY_IS_ERR(result)) { agg_dense_plan_free(&dp); scratch_free(kc_hdr); agg_groups_free(&groups); return ray_error("oom", NULL); }

    for (uint32_t k = 0; k < ext->n_keys; k++) {
        ray_t* kc = ray_group_gather(key_cols[k], groups.first_row, groups.ngroups);
        if (!kc || RAY_IS_ERR(kc)) { agg_dense_plan_free(&dp); scratch_free(kc_hdr); agg_groups_free(&groups); ray_release(result); return kc ? kc : ray_error("oom", NULL); }
        result = ray_table_add_col(result, key_syms[k], kc);
        ray_release(kc);
    }
    agg_dense_plan_free(&dp); scratch_free(kc_hdr);   /* key_cols/key_syms done — agg loop below reads neither */

    for (uint32_t a = 0; a < ext->n_aggs; a++) {
        ray_op_ext_t* ie = find_ext(g, ext->agg_ins[a]);
        int64_t kparam = (ext->agg_k ? ext->agg_k[a] : 0);
        ray_t* col;
        if (ext->agg_ins2 && ext->agg_ins2[a] != RAY_OP_NONE) {       /* binary agg (pearson) */
            ray_op_ext_t* ye = find_ext(g, ext->agg_ins2[a]);
            ray_t* x_col = ray_table_get_col(tbl, ie->sym);
            ray_t* y_col = ray_table_get_col(tbl, ye->sym);
            const agg_vtable_t* vt = agg_resolve(ext->agg_ops[a], x_col->type);
            col = agg_run_one_bin(vt, x_col, y_col, groups.gids, nrows, groups.ngroups, kparam);
        } else {
            ray_t* val_col = (ext->agg_ops[a] != OP_COUNT) ? ray_table_get_col(tbl, ie->sym) : NULL;
            int8_t in_type = val_col ? val_col->type : RAY_I64;
            const agg_vtable_t* vt = agg_resolve(ext->agg_ops[a], in_type);
            col = agg_run_one(vt, val_col, groups.gids, nrows, groups.ngroups, kparam);
        }
        if (!col || RAY_IS_ERR(col)) { agg_groups_free(&groups); ray_release(result); return col ? col : ray_error("oom", NULL); }
        int64_t agg_name = agg_result_col_name(ie->sym, ext->agg_ops[a]);
        result = ray_table_add_col(result, agg_name, col);
        ray_release(col);
    }
    agg_groups_free(&groups);
    return result;
    #undef AGG_RUN_COMPACT_FALLBACK
}

/* Build a COMPACT table holding exactly the columns exec_group_v2_run reads —
 * every KEY column and every non-COUNT AGG-INPUT column (x and y for binary
 * aggs) — gathered at the surviving-row indices `idx` (length n_sel) under the
 * SAME column sym, so the table_get_col(sym) resolution in run + every strategy
 * works unchanged.  Distinct syms are gathered once (de-dup of columns shared
 * by multiple keys/aggs).  gather_by_idx produces a fresh column (no alias to
 * `tbl`'s buffers) preserving type / null bits / STR / GUID payload / SYM
 * domain.  Returns a new table (caller releases) or an error ray. */
static ray_t* agg_build_compact(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                                int64_t* idx, int64_t n_sel) {
    ray_op_ext_t* ext = find_ext(g, op->id);

    /* Collect the distinct syms this group needs from the source table.
     * want/n_want is a LOCAL de-dup counter/scratch, sized exactly to the
     * structural maximum before dedup: every key (ext->n_keys, admission-
     * bounded <=16 today) plus up to 2 input syms per agg (ext->n_aggs,
     * gated <=255 today via the compile-time UINT8_MAX check but NOT capped
     * at 16 — a WHERE-filtered group with >48 distinct key/agg syms used to
     * overflow the old fixed want[48] here; see width_matrix.rfl). */
    size_t want_cap = (size_t)ext->n_keys + 2 * (size_t)ext->n_aggs;
    ray_t* want_hdr = NULL;
    int64_t* want = (int64_t*)scratch_alloc(&want_hdr, want_cap * sizeof(int64_t));
    if (want_cap && !want) return ray_error("oom", NULL);
    uint32_t n_want = 0;
    for (uint32_t k = 0; k < ext->n_keys; k++) {
        int64_t s = find_ext(g, ext->keys[k])->sym;
        bool seen = false;
        for (uint32_t i = 0; i < n_want; i++) if (want[i] == s) { seen = true; break; }
        if (!seen) want[n_want++] = s;
    }
    for (uint32_t a = 0; a < ext->n_aggs; a++) {
        if (ext->agg_ops[a] == OP_COUNT && !(ext->agg_ins && ext->agg_ins[a] != RAY_OP_NONE))
            continue;                       /* COUNT needs no typed input */
        ray_op_t* ins[2] = { ext->agg_ins ? op_node(g, ext->agg_ins[a]) : NULL,
                             ext->agg_ins2 ? op_node(g, ext->agg_ins2[a]) : NULL };
        for (int j = 0; j < 2; j++) {
            if (!ins[j]) continue;
            int64_t s = find_ext(g, ins[j]->id)->sym;
            bool seen = false;
            for (uint32_t i = 0; i < n_want; i++) if (want[i] == s) { seen = true; break; }
            if (!seen) want[n_want++] = s;
        }
    }

    ray_t* compact = ray_table_new(n_want);
    if (!compact || RAY_IS_ERR(compact)) {
        scratch_free(want_hdr);
        return compact ? compact : ray_error("oom", NULL);
    }
    for (uint32_t i = 0; i < n_want; i++) {
        ray_t* src = ray_table_get_col(tbl, want[i]);
        if (!src) { ray_release(compact); scratch_free(want_hdr); return ray_error("nyi", NULL); }
        ray_t* gcol = gather_by_idx(src, idx, n_sel);   /* fresh, no alias */
        if (!gcol || RAY_IS_ERR(gcol)) {
            ray_release(compact); scratch_free(want_hdr);
            return gcol ? gcol : ray_error("oom", NULL);
        }
        compact = ray_table_add_col(compact, want[i], gcol);  /* retains gcol */
        ray_release(gcol);                                    /* drop our ref */
        if (!compact || RAY_IS_ERR(compact)) { scratch_free(want_hdr); return compact ? compact : ray_error("oom", NULL); }
    }
    scratch_free(want_hdr);
    return compact;
}

/* Public entry.  No active WHERE filter → run directly over the input table.
 *
 * With g->selection set (a pushed WHERE filter, rowsel form), build the per-
 * segment selected-count prefix once and hand the rowsel to exec_group_v2_run,
 * which threads it into the CHUNKED strategies (parallel dense / radix /
 * smallhash): they decode the selected ORIGINAL rows in fixed-size chunks and
 * gather only each chunk's key/agg values into small reused buffers, feeding the
 * dense-batch kernels — NO full index array (ray_rowsel_to_indices), NO full
 * compact column, NO per-call large alloc.  This is the chunked selection-
 * vector model (gather a fixed-size chunk's selected rows into reused vectors
 * and sink the chunk), adapted to v2's dense-contiguous batch kernels by
 * gathering per chunk rather than slicing references.
 *
 * The HASH-FALLBACK strategy (F64/STR keys) and the SERIAL path are NOT chunked
 * — those shapes were not perf blockers and are rare/small.  exec_group_v2_run
 * keeps the compact-table approach for them (gather selected rows once, recurse
 * with sel=NULL).  Representative (first_row) indices stay in ORIGINAL-row space
 * throughout so result key columns gather correctly (SYM domains preserved); the
 * output order is unspecified per the v2 contract. */
ray_t* exec_group_v2(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                     int64_t group_limit) {
    if (agg_cancelled()) return ray_error("cancel", NULL);
    /* The top-N emit filter (`desc: AGG take: N`) is read once here and
     * handed to every strategy: radix and the dense finishes select the kept
     * groups themselves; the other routes trim their full result. */
    ray_group_emit_filter_t ef = ray_group_emit_filter_get();
    const ray_group_emit_filter_t* efp = ef.enabled ? &ef : NULL;
    if (!g || !g->selection)
        return exec_group_v2_run(g, op, tbl, ray_table_nrows(tbl), NULL, NULL, 0,
                                 group_limit, efp);

    int64_t src_nrows = ray_table_nrows(tbl);
    ray_rowsel_t* sm = ray_rowsel_meta(g->selection);
    /* Defensive: a selection that doesn't cover this table's rows can't be
     * applied here — fall back to the unfiltered run (matches the scalar-agg
     * guard in group.c, which also only honors a selection when nrows match). */
    if (sm->nrows != src_nrows)
        return exec_group_v2_run(g, op, tbl, src_nrows, NULL, NULL, 0, group_limit, efp);

    int64_t n_sel = sm->total_pass;
    ray_t* prefix_block = agg_sel_build_prefix(g->selection);
    if (!prefix_block) return ray_error("oom", NULL);
    const int64_t* sel_prefix = (const int64_t*)ray_data(prefix_block);

    /* Hide the selection from the run so a downstream strategy can't double-
     * apply it; the rowsel is passed explicitly via the sel argument instead. */
    ray_t* saved_sel = g->selection;
    g->selection = NULL;
    ray_t* result = exec_group_v2_run(g, op, tbl, src_nrows, saved_sel, sel_prefix,
                                      n_sel, group_limit, efp);
    g->selection = saved_sel;

    ray_release(prefix_block);
    return result;
}

/* Read element `row` of an integer/temporal/SYM column widened to int64. */
static inline int64_t agg_read_key_i64(ray_t* col, const void* data, int64_t row) {
    switch (col->type) {
        case RAY_I64: case RAY_TIMESTAMP: return ((const int64_t*)data)[row];
        case RAY_I32: case RAY_DATE: case RAY_TIME: return ((const int32_t*)data)[row];
        case RAY_I16: return ((const int16_t*)data)[row];
        case RAY_U8:  case RAY_BOOL: return ((const uint8_t*)data)[row];
        case RAY_SYM: return (int64_t)ray_read_sym(data, row, col->type, col->attrs);
        default: return 0;  /* gate guarantees only the above reach here */
    }
}

ray_t* agg_run_one(const agg_vtable_t* vt, ray_t* val_col,
                   const uint32_t* gids, int64_t nrows, int64_t ngroups,
                   int64_t kparam) {
    char* states = ray_calloc_raw((size_t)((size_t)(ngroups > 0 ? ngroups : 1)) * (vt->state_size));
    if (!states) return ray_error("oom", NULL);
    for (int64_t gi = 0; gi < ngroups; gi++)
        vt->init(states + (size_t)gi * vt->state_size);

    ray_valid_t valid = { val_col ? ray_data(val_col) : NULL,
                          val_col ? val_col->type : RAY_I64,
                          val_col ? ray_vec_may_have_nulls(val_col) : false };
    const void* vals = val_col ? ray_data(val_col) : NULL;
    vt->update_batch(states, vt->state_size, gids, vals, &valid, nrows, NULL);

    bool is_list = (vt->out_type == RAY_LIST);
    ray_t* out = is_list ? ray_list_new(ngroups) : ray_vec_new(vt->out_type, ngroups);
    if (!out || RAY_IS_ERR(out)) {
        if (vt->destroy)
            for (int64_t gi = 0; gi < ngroups; gi++)
                vt->destroy(states + (size_t)gi * vt->state_size);
        ray_free_raw(states); return ray_error("oom", NULL);
    }
    out->len = ngroups;
    for (int64_t gi = 0; gi < ngroups; gi++) {
        const void* state = states + (size_t)gi * vt->state_size;
        if (is_list) {
            ray_t* cell = vt->finalize(state, NULL, kparam);
            out = ray_list_set(out, gi, cell);   /* retains cell */
            ray_release(cell);
        } else if (agg_finalize_value(vt, state, out, gi, kparam)) {
            out->attrs |= RAY_ATTR_HAS_NULLS;
        }
        if (vt->destroy) vt->destroy(states + (size_t)gi * vt->state_size);
    }
    ray_free_raw(states);
    return out;
}

/* Binary-aggregate (pearson) serial driver: mirrors agg_run_one but feeds two
 * value columns through vt->update_batch2.  A row contributes only when both x
 * and y are valid (the accumulator enforces this via valid_x/valid_y). */
ray_t* agg_run_one_bin(const agg_vtable_t* vt, ray_t* x_col, ray_t* y_col,
                       const uint32_t* gids, int64_t nrows, int64_t ngroups,
                       int64_t kparam) {
    char* states = ray_calloc_raw((size_t)((size_t)(ngroups > 0 ? ngroups : 1)) * (vt->state_size));
    if (!states) return ray_error("oom", NULL);
    for (int64_t gi = 0; gi < ngroups; gi++)
        vt->init(states + (size_t)gi * vt->state_size);

    ray_valid_t vx = { ray_data(x_col), x_col->type,
                       ray_vec_may_have_nulls(x_col) };
    ray_valid_t vy = { ray_data(y_col), y_col->type,
                       ray_vec_may_have_nulls(y_col) };
    vt->update_batch2(states, vt->state_size, gids,
                      ray_data(x_col), ray_data(y_col), &vx, &vy, nrows, NULL);

    ray_t* out = ray_vec_new(vt->out_type, ngroups);
    if (!out || RAY_IS_ERR(out)) {
        if (vt->destroy)
            for (int64_t gi = 0; gi < ngroups; gi++)
                vt->destroy(states + (size_t)gi * vt->state_size);
        ray_free_raw(states); return ray_error("oom", NULL);
    }
    out->len = ngroups;
    for (int64_t gi = 0; gi < ngroups; gi++) {
        if (agg_finalize_value(vt, states + (size_t)gi * vt->state_size, out, gi, kparam))
            out->attrs |= RAY_ATTR_HAS_NULLS;
        if (vt->destroy) vt->destroy(states + (size_t)gi * vt->state_size);
    }
    ray_free_raw(states);
    return out;
}

/* Direct-index grouping: gid = slot2gid[packed slot], assigned first-occurrence.
 * O(1) per row, no hashing. Precondition: dp->ok. Returns 0 / -1 (OOM).
 * Caller frees out->gids and out->first_row.  Fills the SAME agg_groups_t
 * contract as agg_group_keys (gids per row, first_row per group, ngroups in
 * first-occurrence order) so the downstream emit/assembler is unchanged. */
static int agg_group_keys_dense(ray_t** key_cols, int64_t nrows,
                                const dense_plan_t* dp, agg_groups_t* out) {
    /* Direct dense lookup needs one row pass; shared first-row IDs need four.
     * Small pools cannot amortize the extra traffic and barriers. */
    if (ray_pool_par_dispatch_ok(ray_pool_get(), nrows, RAY_PARALLEL_THRESHOLD) &&
            ray_pool_total_workers(ray_pool_get()) >= 4)
        return agg_group_keys_parallel(key_cols, dp->n_keys, nrows, dp, out);
    const void* data[16];   /* [16]: dense path is <=16-key by construction (agg_dense_plan
                             * rejects wider shapes to v2's hash/radix; dp->n_keys is that <=16) */
    for (uint32_t k = 0; k < dp->n_keys; k++) data[k] = ray_data(key_cols[k]);
    int32_t* slot2gid = ray_alloc_raw((size_t)dp->total_slots * sizeof(int32_t));
    out->gids      = ray_alloc_raw((size_t)(nrows > 0 ? nrows : 1) * sizeof(uint32_t));
    out->first_row = ray_alloc_raw((size_t)(nrows > 0 ? nrows : 1) * sizeof(int64_t));
    if (!slot2gid || !out->gids || !out->first_row) {
        ray_free_raw(slot2gid); ray_free_raw(out->gids); ray_free_raw(out->first_row);
        out->gids = NULL; out->first_row = NULL; return -1;
    }
    for (int64_t s = 0; s < dp->total_slots; s++) slot2gid[s] = -1;
    int64_t ngroups = 0;
    const bool compacted = dp->compacted;
    for (int64_t r = 0; r < nrows; r++) {
        int64_t slot = 0;
        if (compacted) {
            for (uint32_t k = 0; k < dp->n_keys; k++)
                slot += agg_dense_component(dp, k, agg_read_key_i64(key_cols[k], data[k], r)) * dp->strides[k];
        } else {
            for (uint32_t k = 0; k < dp->n_keys; k++)
                slot += agg_dense_component_raw(dp, k, agg_read_key_i64(key_cols[k], data[k], r)) * dp->strides[k];
        }
        /* slot is provably in [0,total_slots): each key in [min_k,max_k] so
         * (key-min) in [0,range_k), and the composite is a mixed-radix index
         * < total_slots (dp->ok from the same prescan). */
        int32_t gp = slot2gid[slot];
        if (gp < 0) {
            gp = (int32_t)ngroups;
            slot2gid[slot] = gp;
            out->first_row[ngroups] = r;
            ngroups++;
        }
        out->gids[r] = (uint32_t)gp;
    }
    out->ngroups = ngroups;
    ray_free_raw(slot2gid);
    return 0;
}

/* Per-key hash/eq that also handles WIDE (variable-length STR) keys: int/SYM
 * keys hash/compare their int64 cell; STR keys hash/compare their bytes.  The
 * per-key type branch is invariant across rows (predicted), so the all-int/SYM
 * path is unaffected. */
static uint64_t agg_float_key(ray_t* col, const void* data, int64_t r) {
    if (col->type == RAY_F32) {
        float v = ((const float*)data)[r];
        if (v != v && ray_vec_may_have_nulls(col)) return UINT64_C(0x7fc00000);
        if (v == 0) return 0;
        uint32_t bits; memcpy(&bits, &v, sizeof(bits)); return bits;
    }
    double v = ((const double*)data)[r];
    if (v != v && ray_vec_may_have_nulls(col)) return UINT64_C(0x7ff8000000000000);
    if (v == 0) return 0;
    uint64_t bits; memcpy(&bits, &v, sizeof(bits)); return bits;
}
/* Domain/owner-aware vector comparison for nested LIST keys. Raw descriptor
 * equality is unsuitable for narrow SYM vectors and separately pooled strings. */
static bool agg_list_key_eq(ray_t* a, ray_t* b, const ray_group_sym_view_t* view) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->type == RAY_LIST && b->type == RAY_LIST) {
        if (a->len != b->len) return false;
        for (int64_t i = 0; i < a->len; i++)
            if (!agg_list_key_eq(ray_list_get(a, i), ray_list_get(b, i), view)) return false;
        return true;
    }
    if (a->type == b->type && (a->type == RAY_SYM || a->type == RAY_STR)) {
        if (a->len != b->len) return false;
        for (int64_t i = 0; i < a->len; i++) {
            if (a->type == RAY_SYM) {
                ray_t* x = ray_group_sym_read(view, ray_sym_vec_domain(a), ray_read_sym(ray_data(a), i, a->type, a->attrs));
                ray_t* y = ray_group_sym_read(view, ray_sym_vec_domain(b), ray_read_sym(ray_data(b), i, b->type, b->attrs));
                if (ray_str_cmp(x, y)) return false;
            } else {
                size_t nx = 0, ny = 0;
                const char* x = ray_str_vec_get(a, i, &nx);
                const char* y = ray_str_vec_get(b, i, &ny);
                if (nx != ny || (nx && memcmp(x, y, nx))) return false;
            }
        }
        return true;
    }
    return atom_eq(a, b);
}

/* Match atom_eq's structural LIST / byte-exact typed-vector contract. */
static uint64_t agg_list_key_hash(ray_t* value, const ray_group_sym_view_t* view) {
    if (!value || ray_is_atom(value)) return ray_atom_hash(value);
    if (value->type == RAY_LIST) {
        uint64_t hash = ray_hash_i64(value->len);
        ray_t* const* children = ray_data(value);
        for (int64_t i = 0; i < value->len; i++) hash = ray_hash_combine(hash, agg_list_key_hash(children[i], view));
        return hash;
    }
    if (value->type == RAY_SYM || value->type == RAY_STR) {
        uint64_t hash = ray_hash_i64(value->type);
        for (int64_t i = 0; i < value->len; i++) {
            size_t len = 0; const char* str;
            if (value->type == RAY_SYM) {
                ray_t* atom = ray_group_sym_read(view, ray_sym_vec_domain(value), ray_read_sym(ray_data(value), i, value->type, value->attrs));
                str = ray_str_ptr(atom); len = ray_str_len(atom);
            } else str = ray_str_vec_get(value, i, &len);
            hash = ray_hash_combine(hash, ray_hash_bytes(str ? str : "", len));
        }
        return hash;
    }
    if (ray_is_vec(value)) {
        size_t bytes = (size_t)value->len * ray_elem_size(value->type);
        return ray_hash_combine(ray_hash_i64(value->type), ray_hash_bytes(ray_data(value), bytes));
    }
    return ray_atom_hash(value);
}
static inline uint64_t agg_key_hash_at(ray_t* col, const void* data, int64_t r, const ray_group_sym_view_t* view) {
    if (col->type == RAY_LIST) return agg_list_key_hash(((ray_t* const*)data)[r], view);
    if (col->type == RAY_F32 || col->type == RAY_F64) return ray_hash_i64((int64_t)agg_float_key(col, data, r));
    if (col->type == RAY_GUID) return ray_hash_bytes((const char*)data + (size_t)r * 16, 16);
    if (col->type == RAY_STR) {
        size_t len = 0;
        const char* s = ray_str_vec_get(col, r, &len);
        return ray_hash_bytes(s ? s : "", s ? len : 0);
    }
    return (uint64_t)agg_read_key_i64(col, data, r);
}
static inline int agg_key_eq_at(ray_t* col, const void* data, int64_t a, int64_t b, const ray_group_sym_view_t* view) {
    if (col->type == RAY_LIST) return agg_list_key_eq(((ray_t* const*)data)[a], ((ray_t* const*)data)[b], view);
    if (col->type == RAY_F32 || col->type == RAY_F64) return agg_float_key(col, data, a) == agg_float_key(col, data, b);
    if (col->type == RAY_GUID) return memcmp((const char*)data + (size_t)a * 16, (const char*)data + (size_t)b * 16, 16) == 0;
    if (col->type == RAY_STR) {
        size_t la = 0, lb = 0;
        const char* sa = ray_str_vec_get(col, a, &la);
        const char* sb = ray_str_vec_get(col, b, &lb);
        return la == lb && (la == 0 || memcmp(sa, sb, la) == 0);
    }
    return agg_read_key_i64(col, data, a) == agg_read_key_i64(col, data, b);
}

typedef struct {
    ray_t* source;
    const void* data;
    const int64_t* rows;
    const int64_t* offsets;
    const int64_t* counts;
    int64_t* output;
    _Atomic(bool) failed;
    ray_group_sym_view_t symbols;
} agg_distinct_indexed_t;
static void agg_distinct_indexed_run(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; agg_distinct_indexed_t* c = raw;
    int64_t largest = 0;
    for (int64_t g = start; g < end; g++) if (c->counts[g] > largest) largest = c->counts[g];
    uint64_t capacity = 16;
    while (capacity < (uint64_t)largest * 2) {
        if (capacity > SIZE_MAX / sizeof(int64_t) / 2) { atomic_store(&c->failed, true); return; }
        capacity *= 2;
    }
    int64_t* entries = ray_alloc_raw((size_t)capacity * sizeof(int64_t));
    if (!entries) { atomic_store(&c->failed, true); return; }
    for (int64_t g = start; g < end && !agg_cancelled(); g++) {
        uint64_t size = 16;
        while (size < (uint64_t)c->counts[g] * 2) size *= 2;
        memset(entries, 0, (size_t)size * sizeof(int64_t));
        int64_t distinct = 0;
        for (int64_t i = 0; i < c->counts[g]; i++) {
            int64_t row = c->rows[c->offsets[g] + i];
            uint64_t slot = agg_key_hash_at(c->source, c->data, row, &c->symbols) & (size - 1);
            while (entries[slot] && !agg_key_eq_at(c->source, c->data, row, entries[slot] - 1, &c->symbols))
                slot = (slot + 1) & (size - 1);
            if (!entries[slot]) { entries[slot] = row + 1; distinct++; }
        }
        c->output[g] = distinct;
    }
    ray_free_raw(entries);
}
ray_t* agg_count_distinct_indexed(ray_t* src, const int64_t* rows,
        const int64_t* offsets, const int64_t* counts, int64_t groups) {
    if (src->type != RAY_STR && src->type != RAY_GUID && src->type != RAY_LIST) return NULL;
    ray_t* out = ray_vec_new(RAY_I64, groups);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = groups;
    agg_distinct_indexed_t c = {src, ray_data(src), rows, offsets, counts, ray_data(out), false, {0}};
    if (src->type == RAY_LIST) ray_sym_strings_borrow(&c.symbols.strings, &c.symbols.count);
    ray_group_dispatch(agg_distinct_indexed_run, &c, counts, groups);
    if (agg_cancelled() || atomic_load(&c.failed)) {
        ray_release(out); return ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
    }
    return out;
}

/* One shared key directory, with earliest source row as the representative.
 * Concurrent insertion never changes a key's identity: competing rows compare
 * immutable input columns, then atomically lower its representative. Separate
 * barriers assign deterministic first-occurrence group IDs and remap rows.
 * Memory is O(rows + directory), independent of the number of workers. */
typedef struct {
    ray_t** keys;
    const void** data;
    uint32_t nkeys, tasks, init_tasks;
    int64_t rows, capacity, init_pages;
    const dense_plan_t* dense;
    void* first;
    bool narrow;
    uint8_t* unique;
    int64_t* offsets;
    agg_groups_t* out;
    ray_group_sym_view_t symbols;
} agg_key_build_t;

/* Coarse aligned ranges avoid concurrent first writes to common 2 MiB
 * huge pages. Small directories initialize inline; large ones retain
 * enough independent ranges to use the worker pool. */
enum { AGG_DIRECTORY_INIT_BYTES = 2 * 1024 * 1024 };
static void agg_key_directory_init(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; agg_key_build_t* c = raw;
#define KEY_DIRECTORY_INIT(TYPE, EMPTY) do { \
        _Atomic TYPE* first_slots = c->first; \
        uint64_t offset = (uintptr_t)c->first & (AGG_DIRECTORY_INIT_BYTES - 1); \
        for (int64_t task = start; task < end; task++) { \
            uint64_t lo = (uint64_t)(c->init_pages * task / c->init_tasks) * AGG_DIRECTORY_INIT_BYTES; \
            uint64_t hi = (uint64_t)(c->init_pages * (task + 1) / c->init_tasks) * AGG_DIRECTORY_INIT_BYTES; \
            int64_t begin = lo < offset ? 0 : (int64_t)((lo - offset) / sizeof(*first_slots)); \
            int64_t limit = (int64_t)((hi - offset) / sizeof(*first_slots)); \
            if (limit > c->capacity) limit = c->capacity; \
            for (int64_t i = begin; i < limit; i++) atomic_init(&first_slots[i], EMPTY); \
        } \
    } while (0)
    if (c->narrow) KEY_DIRECTORY_INIT(int32_t, INT32_MAX);
    else KEY_DIRECTORY_INIT(int64_t, INT64_MAX);
#undef KEY_DIRECTORY_INIT
}
static void agg_key_directory_insert(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; agg_key_build_t* c = raw;
#define KEY_DIRECTORY_INSERT(TYPE, EMPTY) do { \
        _Atomic TYPE* first_slots = c->first; \
        for (int64_t r = start; r < end; r++) { \
            uint64_t slot = 0; \
            if (c->dense) { \
                for (uint32_t k = 0; k < c->nkeys; k++) \
                    slot += agg_dense_component(c->dense, k, agg_read_key_i64(c->keys[k], c->data[k], r)) \
                        * c->dense->strides[k]; \
            } else { \
                uint64_t hash = UINT64_C(1469598103934665603); \
                for (uint32_t k = 0; k < c->nkeys; k++) { \
                    hash ^= agg_key_hash_at(c->keys[k], c->data[k], r, &c->symbols); \
                    hash *= UINT64_C(1099511628211); \
                } \
                slot = hash & (c->capacity - 1); \
            } \
            for (;;) { \
                TYPE first = atomic_load_explicit(&first_slots[slot], memory_order_relaxed); \
                if (!c->dense && first != EMPTY) { \
                    bool equal = true; \
                    for (uint32_t k = 0; k < c->nkeys && equal; k++) \
                        equal = agg_key_eq_at(c->keys[k], c->data[k], r, first, &c->symbols); \
                    if (!equal) { slot = (slot + 1) & (c->capacity - 1); continue; } \
                } \
                if (r < first && !atomic_compare_exchange_weak_explicit(&first_slots[slot], &first, (TYPE)r, \
                        memory_order_relaxed, memory_order_relaxed)) continue; \
                c->out->gids[r] = (uint32_t)slot; \
                break; \
            } \
        } \
    } while (0)
    if (c->narrow) KEY_DIRECTORY_INSERT(int32_t, INT32_MAX);
    else KEY_DIRECTORY_INSERT(int64_t, INT64_MAX);
#undef KEY_DIRECTORY_INSERT
}
static void agg_key_directory_count(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; agg_key_build_t* c = raw;
#define KEY_DIRECTORY_COUNT(TYPE, EMPTY) do { \
        _Atomic TYPE* first_slots = c->first; \
        for (int64_t task = start; task < end; task++) { \
            int64_t begin = c->rows / c->tasks * task; \
            int64_t limit = task + 1 == c->tasks ? c->rows : c->rows / c->tasks * (task + 1); \
            int64_t count = 0; \
            for (int64_t r = begin; r < limit; r++) { \
                bool first = atomic_load_explicit(&first_slots[c->out->gids[r]], memory_order_relaxed) == r; \
                c->unique[r] = first; \
                count += first; \
            } \
            c->offsets[task + 1] = count; \
        } \
    } while (0)
    if (c->narrow) KEY_DIRECTORY_COUNT(int32_t, INT32_MAX);
    else KEY_DIRECTORY_COUNT(int64_t, INT64_MAX);
#undef KEY_DIRECTORY_COUNT
}
static void agg_key_directory_compact(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; agg_key_build_t* c = raw;
#define KEY_DIRECTORY_COMPACT(TYPE, EMPTY) do { \
        _Atomic TYPE* first_slots = c->first; \
        for (int64_t task = start; task < end; task++) { \
            int64_t begin = c->rows / c->tasks * task; \
            int64_t limit = task + 1 == c->tasks ? c->rows : c->rows / c->tasks * (task + 1); \
            int64_t gid = c->offsets[task]; \
            for (int64_t r = begin; r < limit; r++) if (c->unique[r]) { \
                c->out->first_row[gid] = r; \
                atomic_store_explicit(&first_slots[c->out->gids[r]], (TYPE)gid++, memory_order_relaxed); \
            } \
        } \
    } while (0)
    if (c->narrow) KEY_DIRECTORY_COMPACT(int32_t, INT32_MAX);
    else KEY_DIRECTORY_COMPACT(int64_t, INT64_MAX);
#undef KEY_DIRECTORY_COMPACT
}
static void agg_key_directory_remap(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; agg_key_build_t* c = raw;
#define KEY_DIRECTORY_REMAP(TYPE, EMPTY) do { \
        _Atomic TYPE* first_slots = c->first; \
        for (int64_t r = start; r < end; r++) \
            c->out->gids[r] = (uint32_t)atomic_load_explicit(&first_slots[c->out->gids[r]], memory_order_relaxed); \
    } while (0)
    if (c->narrow) KEY_DIRECTORY_REMAP(int32_t, INT32_MAX);
    else KEY_DIRECTORY_REMAP(int64_t, INT64_MAX);
#undef KEY_DIRECTORY_REMAP
}
static int agg_group_keys_parallel(ray_t** keys, uint32_t nkeys, int64_t rows,
                                    const dense_plan_t* dp, agg_groups_t* out) {
    ray_pool_t* pool = ray_pool_get();
    uint32_t tasks = ray_pool_total_workers(pool) * 4;
    if (tasks > RAY_POOL_INIT_TASKS) tasks = RAY_POOL_INIT_TASKS;
    int64_t cap = dp ? dp->total_slots : 16;
    if (!dp) while (cap < rows * 2) cap *= 2;
    agg_key_build_t c = {.keys = keys, .nkeys = nkeys, .tasks = tasks, .rows = rows,
        .capacity = cap, .dense = dp, .out = out, .narrow = rows <= INT32_MAX};
    for (uint32_t k = 0; k < nkeys; k++) if (keys[k]->type == RAY_LIST) {
        ray_sym_strings_borrow(&c.symbols.strings, &c.symbols.count); break;
    }
    c.data = ray_alloc_raw((size_t)nkeys * sizeof(*c.data));
    /* Row and group ids below INT32_MAX leave its maximum value available
     * as the empty sentinel. Larger dense inputs retain full-width entries. */
    c.first = ray_alloc_raw((size_t)cap * (c.narrow ? sizeof(_Atomic(int32_t)) : sizeof(_Atomic(int64_t))));
    c.unique = ray_alloc_raw((size_t)rows);
    c.offsets = ray_calloc_raw((tasks + 1) * sizeof(*c.offsets));
    out->gids = ray_alloc_raw((size_t)rows * sizeof(*out->gids));
    out->first_row = NULL;
    int rc = -1;
    if (!c.data || !c.first || !c.unique || !c.offsets || !out->gids) goto done;
    for (uint32_t k = 0; k < nkeys; k++) c.data[k] = ray_data(keys[k]);
    ray_profile_tick("directory: allocated");
    size_t entry_size = c.narrow ? sizeof(_Atomic(int32_t)) : sizeof(_Atomic(int64_t));
    uint64_t init_bytes = ((uintptr_t)c.first & (AGG_DIRECTORY_INIT_BYTES - 1)) + (uint64_t)cap * entry_size;
    c.init_pages = (init_bytes + AGG_DIRECTORY_INIT_BYTES - 1) / AGG_DIRECTORY_INIT_BYTES;
    c.init_tasks = c.init_pages < tasks ? (uint32_t)c.init_pages : tasks;
    if (c.init_tasks == 1) agg_key_directory_init(&c, 0, 0, 1);
    else ray_pool_dispatch_n(pool, agg_key_directory_init, &c, c.init_tasks);
    ray_profile_tick("directory: initialized");
    ray_pool_dispatch(pool, agg_key_directory_insert, &c, rows);
    ray_profile_tick("directory: inserted keys");
    if (agg_cancelled()) goto done;
    ray_pool_dispatch_n(pool, agg_key_directory_count, &c, tasks);
    ray_profile_tick("directory: counted groups");
    if (agg_cancelled()) goto done;
    for (uint32_t t = 0; t < tasks; t++) c.offsets[t + 1] += c.offsets[t];
    out->ngroups = c.offsets[tasks];
    /* Representatives need one entry per group, not per input row. Delay
     * allocation until the prefix counts give the exact output capacity. */
    out->first_row = ray_alloc_raw((size_t)(out->ngroups ? out->ngroups : 1) * sizeof(*out->first_row));
    if (!out->first_row) goto done;
    ray_pool_dispatch_n(pool, agg_key_directory_compact, &c, tasks);
    ray_profile_tick("directory: compacted groups");
    ray_pool_dispatch(pool, agg_key_directory_remap, &c, rows);
    ray_profile_tick("directory: remapped rows");
    if (!agg_cancelled()) rc = 0;
done:
    ray_free_raw(c.data); ray_free_raw(c.first); ray_free_raw(c.unique); ray_free_raw(c.offsets);
    if (rc) agg_groups_free(out);
    return rc;
}

int agg_group_keys(ray_t** key_cols, uint32_t n_keys, int64_t nrows, agg_groups_t* out) {
    if (nrows < 0 || nrows > INT32_MAX) return -1;
    if (ray_pool_par_dispatch_ok(ray_pool_get(), nrows, RAY_PARALLEL_THRESHOLD))
        return agg_group_keys_parallel(key_cols, n_keys, nrows, NULL, out);
    /* Unbounded keys: cut-3 lifted both admission gates (the GROUP path and the
     * keys-only DISTINCT path via agg_select_distinct), so the key-data pointer
     * table is an exact carve, not a fixed [16]. */
    ray_t* data_hdr;
    const void** data = (const void**)scratch_alloc(&data_hdr, (size_t)n_keys * sizeof(void*));
    if (!data) return -1;
    for (uint32_t k = 0; k < n_keys; k++) data[k] = ray_data(key_cols[k]);

    ray_group_sym_view_t symbols = {0};
    for (uint32_t k = 0; k < n_keys; k++) if (key_cols[k]->type == RAY_LIST) {
        ray_sym_strings_borrow(&symbols.strings, &symbols.count); break;
    }

    /* hash table capacity: next pow2 >= 2*nrows, min 16 */
    int64_t cap = 16;
    while (cap < nrows * 2) cap <<= 1;
    uint64_t mask = (uint64_t)cap - 1;

    int32_t* ht_gid = ray_alloc_raw((size_t)cap * sizeof(int32_t));
    out->gids      = ray_alloc_raw((size_t)(nrows > 0 ? nrows : 1) * sizeof(uint32_t));
    out->first_row = ray_alloc_raw((size_t)(nrows > 0 ? nrows : 1) * sizeof(int64_t));
    if (!ht_gid || !out->gids || !out->first_row) {
        scratch_free(data_hdr);
        ray_free_raw(ht_gid); ray_free_raw(out->gids); ray_free_raw(out->first_row);
        out->gids = NULL; out->first_row = NULL; return -1;
    }
    for (int64_t i = 0; i < cap; i++) ht_gid[i] = -1;

    int64_t ngroups = 0;
    for (int64_t r = 0; r < nrows; r++) {
        uint64_t h = 1469598103934665603ULL;
        for (uint32_t k = 0; k < n_keys; k++) {
            h ^= agg_key_hash_at(key_cols[k], data[k], r, &symbols); h *= 1099511628211ULL;
        }
        uint64_t slot = h & mask;
        for (;;) {
            int32_t gptr = ht_gid[slot];
            if (gptr < 0) {                          /* empty slot → new group */
                ht_gid[slot] = (int32_t)ngroups;
                out->first_row[ngroups] = r;
                out->gids[r] = (uint32_t)ngroups;
                ngroups++;
                break;
            }
            int64_t fr = out->first_row[gptr];
            int eq = 1;
            for (uint32_t k = 0; k < n_keys; k++) {
                if (!agg_key_eq_at(key_cols[k], data[k], r, fr, &symbols)) { eq = 0; break; }
            }
            if (eq) { out->gids[r] = (uint32_t)gptr; break; }
            slot = (slot + 1) & mask;                /* linear probe */
        }
    }
    out->ngroups = ngroups;
    ray_free_raw(ht_gid);
    scratch_free(data_hdr);
    return 0;
}

void agg_groups_free(agg_groups_t* out) {
    if (!out) return;
    ray_free_raw(out->gids);      out->gids = NULL;
    ray_free_raw(out->first_row); out->first_row = NULL;
}

/* Gather one column's first-of-group values (first_row[gi]) by type: STR via the
 * string-vec path (null-preserving), LIST via retained borrows, everything else
 * (fixed-width + SYM) via ray_group_gather — which adopts a SYM column's source
 * domain, so SYM columns are NEVER interned into the global table.  Returns a new
 * column of n rows, or an error/NULL on failure. */
static ray_t* agg_gather_col_at(ray_t* sc, const int64_t* first_row, int64_t n) {
    if (sc->type == RAY_STR) {
        if (n == 0) return ray_str_vec_from_parts(NULL, NULL, NULL, 0);
        const char** ptrs = (const char**)ray_alloc_raw((size_t)n * sizeof(const char*));
        uint32_t*    lens = (uint32_t*)ray_alloc_raw((size_t)n * sizeof(uint32_t));
        if (!ptrs || !lens) {
            ray_free_raw(ptrs);
            ray_free_raw(lens);
            return ray_error("oom", NULL);
        }
        for (int64_t gi = 0; gi < n; gi++) {
            size_t sl = 0;
            const char* sp = ray_str_vec_get(sc, first_row[gi], &sl);
            ptrs[gi] = sp ? sp : "";
            lens[gi] = (uint32_t)(sp ? sl : 0);
        }
        ray_t* dst = ray_str_vec_from_parts(ptrs, lens, NULL, n);
        ray_free_raw(ptrs);
        ray_free_raw(lens);
        return dst;
    }
    if (sc->type == RAY_LIST) {
        ray_t* dst = ray_alloc((size_t)(n > 0 ? n : 1) * sizeof(ray_t*));
        if (!dst || RAY_IS_ERR(dst)) return dst;
        dst->type = RAY_LIST; dst->len = n;
        ray_t** dout = (ray_t**)ray_data(dst);
        ray_t** sitems = (ray_t**)ray_data(sc);
        for (int64_t gi = 0; gi < n; gi++) { dout[gi] = sitems[first_row[gi]]; ray_retain(dout[gi]); }
        return dst;
    }
    return ray_group_gather(sc, first_row, n);
}

/* Multi-key `select {by: {keys}}` with NO aggregates.  Group on each key
 * column's RAW cell values (positions for SYM — domain-local, NO global
 * interning of the vocabulary), then emit, per group, the FIRST-of-group value
 * of every column of `tbl`: the key columns (named by key_syms, in by: order)
 * followed by every non-key column (group-by semantics — first row of
 * the group).  Columns are gathered via agg_gather_col_at, which handles
 * STR/LIST and adopts SYM source domains, so NOTHING is interned globally.
 * Replaces the legacy path's per-cell runtime-id boxing.  Precondition (gated by
 * the caller): keys are int/SYM (agg_group_keys reads them as int64) and tbl
 * columns are fixed-width/SYM/STR/LIST (parted/mapcommon fall back to legacy).
 * Caller owns the returned table. */
ray_t* agg_select_distinct(ray_t* tbl, ray_t** key_cols, const int64_t* key_syms,
                           uint32_t nk, int64_t nrows,
                           const int64_t* keep_syms, int keep_n) {
    agg_groups_t groups = {0};
    if (agg_group_keys(key_cols, nk, nrows, &groups) != 0)
        return ray_error("oom", NULL);
    int64_t ncol = ray_table_ncols(tbl);
    ray_t* result = ray_table_new(ncol);
    if (!result || RAY_IS_ERR(result)) {
        agg_groups_free(&groups);
        return result ? result : ray_error("oom", NULL);
    }
    /* key columns first, named by key_syms (by: order) */
    for (uint32_t k = 0; k < nk; k++) {
        ray_t* kc = agg_gather_col_at(key_cols[k], groups.first_row, groups.ngroups);
        if (!kc || RAY_IS_ERR(kc)) {
            agg_groups_free(&groups); ray_release(result);
            return kc ? kc : ray_error("oom", NULL);
        }
        result = ray_table_add_col(result, key_syms[k], kc);
        ray_release(kc);
        if (RAY_IS_ERR(result)) { agg_groups_free(&groups); return result; }
    }
    /* then every NON-key column: first-of-group value (first_row = first occ) */
    for (int64_t c = 0; c < ncol; c++) {
        int64_t cn = ray_table_col_name(tbl, c);
        bool is_key = false;
        for (uint32_t k = 0; k < nk; k++) if (key_syms[k] == cn) { is_key = true; break; }
        if (is_key) continue;
        /* Projection pushdown: when the consumer published the columns it
         * references (keep_syms), drop the non-key columns it never reads —
         * keys are always carried.  keep_syms == NULL → carry all (default). */
        if (keep_syms) {
            bool req = false;
            for (int j = 0; j < keep_n; j++) if (keep_syms[j] == cn) { req = true; break; }
            if (!req) continue;
        }
        ray_t* sc = ray_table_get_col_idx(tbl, c);
        if (!sc) continue;
        ray_t* dc = agg_gather_col_at(sc, groups.first_row, groups.ngroups);
        if (!dc || RAY_IS_ERR(dc)) {
            agg_groups_free(&groups); ray_release(result);
            return dc ? dc : ray_error("oom", NULL);
        }
        result = ray_table_add_col(result, cn, dc);
        ray_release(dc);
        if (RAY_IS_ERR(result)) { agg_groups_free(&groups); return result; }
    }
    agg_groups_free(&groups);
    return result;
}
