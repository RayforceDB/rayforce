/* src/ops/agg_engine.h — v2 composable aggregation engine entry (design §3). */
#ifndef RAY_OPS_AGG_ENGINE_H
#define RAY_OPS_AGG_ENGINE_H

#include <rayforce.h>
#include "ops/internal.h"   /* ray_graph_t, ray_op_t, ray_op_ext_t, find_ext */
#include "ops/agg_acc.h"    /* agg_vtable_t */
#include "table/sym.h"    /* ray_read_sym */

/* Test/feature knob: route OP_GROUP through the v2 engine when it can handle
 * the query (see agg_v2_can_handle). Enabled by default. */
extern bool ray_agg_engine_v2;

/* Test/feature knob: let an unordered grouped `take: N` find its N groups
 * before aggregating and fold only their rows (agg_first_n_run). On by default. */
extern bool ray_agg_first_n;

/* Test/feature knob: int/SYM-key parallel groups that are not dense pre-
 * aggregate into per-task top-bit tables (table stack) and scatter only
 * all-distinct tails; off = the former radix scatter of every row. */
extern bool ray_agg_tablestack;

/* Test knob: payload bytes one radix wave may hold when a top-N (`desc: AGG
 * take: N`) grouping is split into waves (agg_radix_wave_plan).  0 = derive
 * the bound from the heap's anonymous watermark; tests lower it to split
 * small inputs; negative = neither waves nor the per-partition top-N
 * reduction (the one-pass radix the reduction must reproduce). */
extern int64_t ray_agg_radix_wave_bytes;
/* Test knob: how the later waves of a split run get their records.  0 = by
 * bytes moved (agg_radix_spill_wanted), 1 = always re-scan the input,
 * 2 = always spill to a file when there is more than one wave; 3..6 spill
 * like 2 and inject a fault: 3 = every read-back fails, 4 = every write
 * fails, 5 = the pool is cancelled during the first read-back, 6 = the pool
 * is cancelled when a later wave is reduced. */
extern int ray_agg_radix_spill;

/* Unordered take: N first-N limits, shared by the pre-pass (agg_first_n_run)
 * and the parted group's first-key scan (group.c). */
#define AGG_FIRSTN_MAX        1024     /* largest take: N the pre-pass serves */
#define AGG_FIRSTN_PROBE_ROWS 65536    /* prefix budget for the serial scan, at least */

/* The group's transient-state budget: the dense compaction's per-task
 * bitmaps (agg_engine.c) and the null fills a ragged parted group builds
 * for one query (group.c, grp_synth_plain) stay within it. */
#define AGG_COMPACT_BITMAP_BUDGET ((size_t)32 << 20)

/* Read element `i` of integer/temporal/SYM data of `type` widened to int64;
 * a SYM element's width comes from `attrs` (a column's, or a range piece's). */
static inline int64_t agg_read_key_i64_raw(int8_t type, uint8_t attrs, const void* data, int64_t i) {
    switch (type) {
        case RAY_I64: case RAY_TIMESPAN: case RAY_TIMESTAMP: return ((const int64_t*)data)[i];
        case RAY_I32: case RAY_DATE: case RAY_TIME: return ((const int32_t*)data)[i];
        case RAY_I16: return ((const int16_t*)data)[i];
        case RAY_U8:  case RAY_BOOL: return ((const uint8_t*)data)[i];
        case RAY_SYM: return (int64_t)ray_read_sym(data, i, RAY_SYM, attrs);
        default: return 0;  /* gate guarantees only the above reach here */
    }
}
/* Read element `row` of an integer/temporal/SYM column widened to int64. */
static inline int64_t agg_read_key_i64(ray_t* col, const void* data, int64_t row) {
    return agg_read_key_i64_raw(col->type, col->attrs, data, row);
}
/* The key types agg_read_key_i64 reads. */
static inline bool agg_key_i64_type(int8_t t) {
    return t == RAY_I64 || t == RAY_TIMESPAN || t == RAY_TIMESTAMP || t == RAY_I32 || t == RAY_DATE || t == RAY_TIME ||
           t == RAY_I16 || t == RAY_U8 || t == RAY_BOOL || t == RAY_SYM;
}

/* Admission is pure: inspecting a plan must not change execution diagnostics. */
typedef enum {
    AGG_V2_ADMITTED,
    AGG_V2_SHAPE,
    AGG_V2_KEY_EXPRESSION,
    AGG_V2_KEY_TYPE,
    AGG_V2_AGG_EXPRESSION,
    AGG_V2_AGG_TYPE,
    AGG_V2_BUFFERED,
    AGG_V2_PARAMETER,
    AGG_V2_DISABLED,
    AGG_V2_EMIT_FILTER,     /* no longer produced: v2 owns the emit filter */
    AGG_V2_PARALLEL_WIDE,
} agg_v2_reason_t;

agg_v2_reason_t agg_v2_admission(ray_graph_t* g, ray_op_t* op, ray_t* tbl);

typedef enum {
    AGG_ROUTE_NONE,
    AGG_ROUTE_LEGACY,
    AGG_ROUTE_SLICES,
    AGG_ROUTE_PARTED,
    AGG_ROUTE_V2_SERIAL_DENSE,
    AGG_ROUTE_V2_SERIAL_HASH,
    AGG_ROUTE_V2_DENSE,
    AGG_ROUTE_V2_RADIX,
    AGG_ROUTE_V2_HASH,
    AGG_ROUTE_V2_SMALLHASH,
    AGG_ROUTE_V2_INDEXED,
    AGG_ROUTE_V2_TABLESTACK,
    AGG_ROUTE_V2_RUNS,
    AGG_ROUTE_COUNT,
} agg_route_t;

typedef enum {
    AGG_DENSE_NONE,
    AGG_DENSE_TASK_LOCAL,
    AGG_DENSE_PARTITIONED,
    AGG_DENSE_SHARED,
} agg_dense_strategy_t;

/* Per-calling-thread dispatch counts since reset, not a whole-query trace.
 * Nested/partitioned groups may record multiple routes. Incremented only at
 * dispatch boundaries, never inside worker row loops. A count records an
 * attempted dispatch (which may subsequently fail), not successful completion.
 * last_v2_reason describes the most recent legacy/v2 admission decision. */
typedef struct {
    uint64_t routes[AGG_ROUTE_COUNT];
    agg_v2_reason_t last_v2_reason;
    bool nullable_key;              /* last v2 run: non-SYM key may contain nulls */
    bool dense_plan_available;      /* last v2 run: bounded dense range exists */
    bool dense_worker_budget;       /* worker allocation or sampled traffic budget exceeded */
    agg_dense_strategy_t dense_strategy;
    uint64_t dense_local_slots;     /* allocated group-state slots, including partials */
    uint32_t dense_tasks;           /* local/partition tasks; worker count for shared updates */
    uint64_t key_domain_evals;      /* computed keys evaluated once per distinct symbol */
    uint64_t key_vocab_order;       /* ...of which put back into vocabulary order on the pool */
    uint64_t da_lazy_merges;        /* legacy dense-array groupings merged over lazily seeded sets */
    uint64_t da_lazy_skipped;       /* ...(worker, 64-slot chunk) pairs those merges skipped as never seeded */
    bool topn_native;               /* last v2 run selected the emit filter's top-N itself */
    int64_t topn_kept;              /* groups kept by that selection (ties included) */
    uint64_t first_n;               /* unordered take: N answered by the first-N pre-pass */
    uint32_t ts_tasks;              /* last table-stack run: phase-1 tasks */
    uint32_t ts_scatter_tasks;      /* ...of which switched to raw scatter */
    uint64_t ts_tables;             /* ...tables created across all tasks */
    uint32_t radix_waves;           /* last radix run: payload waves (1 = one pass) */
    uint64_t radix_compacted;       /* ...partitions reduced to their top-N candidates */
    uint64_t radix_spill_bytes;     /* ...bytes written to (and read back from) its spill file */
    uint64_t radix_spill_buf;       /* ...bytes of its write and read-back buffers */
    bool dense_count32;             /* last shared dense run: the slab held u32 counters (count only) */
    uint64_t dense_emit_slots;      /* last dense finish: slot entries its emit allocated */
    uint8_t radix_phase1;           /* last radix run's phase 1: AGG_RADIX_P1_* */
} agg_route_stats_t;
/* Phase 1 of a radix run: plain or staged two passes, the fused pass
 * staging whole records (sel mode), or the fused pass staging only the
 * sources it decodes (plain columns read in place by its scatter). */
enum { AGG_RADIX_P1_TWO_PASS = 1, AGG_RADIX_P1_FUSED_RECORDS, AGG_RADIX_P1_FUSED_COMPACT };
void agg_route_reset(void);
void agg_route_note_key_domain(void);
void agg_route_note_vocab_order(void);
void agg_route_note_da_lazy(uint64_t skipped_chunks);
agg_route_stats_t agg_route_stats(void);
void agg_route_record(agg_route_t route);
void agg_route_reason(agg_v2_reason_t reason);
/* Test hook (C tests only): the parallel int/SYM-key strategies run the
 * small hash (their allocation-failure fallback) in place of the table
 * stack and the radix scatter.  Disarmed by ray_test_hooks_off. */
void agg_test_force_smallhash(bool on);
/* Test hook: a plan wider than the row count is dropped, so the grouping
 * takes the radix / table-stack routes.  Disarmed by ray_test_hooks_off. */
void agg_test_no_wide_dense(bool on);
/* Test hook: dense plans over a selection that the row probe proved
 * impossible (agg_sel_bounds_impossible) since process start. */
uint64_t ray_agg_test_sel_probe_hits(void);

/* True iff the v2 engine fully supports this group node over this table.
 * Conservative: any uncertainty → false → caller uses the existing engine. */
bool agg_v2_can_handle(ray_graph_t* g, ray_op_t* op, ray_t* tbl);

/* Top-N keep decision shared by every strategy: keep[i] = 1 when group i
 * passes min_count_exclusive and (when top_count_take > 0) lies within the
 * top-N by value in the filter's direction, ties included (a superset of N;
 * the DAG's sort+take downstream finalizes order and limit).  Returns the
 * number kept.  vals may be NULL when n == 0. */
int64_t agg_topn_keep(const double* vals, int64_t n,
                      const ray_group_emit_filter_t* ef, uint8_t* keep);
/* The two halves of agg_topn_keep, for callers that reduce candidates in
 * parallel: the N-th value in the keep direction (false when every value is
 * kept), and the keep marking against a known threshold. */
bool agg_topn_threshold(const double* vals, int64_t n,
                        const ray_group_emit_filter_t* ef, double* thr);
int64_t agg_topn_mark(const double* vals, int64_t n, const ray_group_emit_filter_t* ef,
                      bool have_thr, double thr, uint8_t* keep);

/* Double view of aggregate `vt` for n groups: group i's state is at
 * states + (slots ? slots[i] : i) * stride + off.  Nulls (and NaN) map to
 * -INFINITY, where the sort places them (first ascending, last descending).
 * Returns false for an out_type without a scalar order (LIST, STR, ...). */
bool agg_group_values_f64(const agg_vtable_t* vt, const char* states,
                          size_t stride, size_t off, const int64_t* slots,
                          int64_t n, int64_t param, double* out);

/* Trim a finished group result to the emit filter's kept superset (row order
 * preserved; consumes `result`).  Used by routes that emit every group. */
/* True when the armed emit filter names this group node's slot (the
 * operation it names, count when unset): the one routing and trim test. */
bool agg_emit_filter_targets(const ray_group_emit_filter_t* ef, const ray_op_ext_t* ext);
ray_t* agg_emit_filter_trim(ray_t* result, uint32_t n_keys, uint32_t n_aggs,
                            const uint16_t* agg_ops, const ray_group_emit_filter_t* ef);

/* Precondition: agg_v2_can_handle(g, op, tbl) returned true.
 * `group_limit` is the HEAD(GROUP) row-limit HINT (0 = no limit): when
 * positive, an engine strategy may emit only the first `group_limit` groups in
 * first-seen order.  Advisory only — the caller trims the result regardless. */
ray_t* exec_group_v2(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                     int64_t group_limit);

/* Dense group assignment over the group's key columns, first-occurrence
 * order (key count unbounded since cut 3; the dense planner self-limits). */
typedef struct {
    uint32_t* gids;       /* len = nrows */
    int64_t*  first_row;  /* len = ngroups; row index where each group first appeared */
    int64_t   ngroups;
} agg_groups_t;

/* Multi-key grouping, key count unbounded. Uses native integer/SYM,
 * canonical float, byte/string, and structural LIST hash/equality. Assigns
 * gids incrementally on first sight → gid
 * order == first-occurrence order; first_row[gid] records the row where the
 * group first appeared. Returns 0 on success (caller releases out via
 * agg_groups_free()), -1 on allocation failure.
 * n_keys is uint32_t: unbounded key count.  Cut-3 lifted both admission gates
 * (the GROUP path and the keys-only DISTINCT path) and the fixed data[16]
 * inside became an exact carve, so any key count groups correctly. */
int agg_group_keys(ray_t** key_cols, uint32_t n_keys, int64_t nrows, agg_groups_t* out);
/* Exact wide-value distinct counts over an existing stable group index. */
ray_t* agg_count_distinct_indexed(ray_t* src, const int64_t* rows,
    const int64_t* offsets, const int64_t* counts, int64_t groups);
/* Large flat grouping: return first-occurrence keys and stable index vectors,
 * or NULL when the existing serial implementation should handle the input. */
ray_t* agg_group_indices(ray_t* source);

/* Release the buffers an agg_groups_t holds (buddy-backed, NOT libc malloc — so
 * callers must use this, not free()).  Idempotent; NULLs the pointers. */
void agg_groups_free(agg_groups_t* out);

/* Multi-key `select {by: {keys}}` with no aggregates → result table carrying the
 * first-of-group value of every `tbl` column (keys named by key_syms, then the
 * non-key columns), SYM columns adopting their source domain (no global
 * interning).  See agg_engine.c.  Precondition (caller-gated): keys are int/SYM
 * and every tbl column is fixed-width/SYM/STR/LIST.  Caller owns the table.
 * keep_syms (or NULL) lists the column syms a consumer references — non-key
 * columns NOT in it are dropped (projection pushdown); keys are always kept.
 * nk is uint32_t: unbounded key count (cut-3 lifted query.c's distinct gate;
 * agg_group_keys carves its key table, so any nk groups correctly). */
ray_t* agg_select_distinct(ray_t* tbl, ray_t** key_cols, const int64_t* key_syms,
                           uint32_t nk, int64_t nrows,
                           const int64_t* keep_syms, int keep_n);

/* Build a dense SoA per-group state array for one aggregate (vt), run a single
 * update_batch over val_col grouped by gids, and finalize each group into a
 * typed result column of vt->out_type, length ngroups. For COUNT, val_col is
 * NULL. Caller owns the returned column (ray_release). Returns a ray_error atom
 * on allocation failure. Single-threaded (Phase 1a). */
ray_t* agg_run_one(const agg_vtable_t* vt, ray_t* val_col,
                   const uint32_t* gids, int64_t nrows, int64_t ngroups,
                   int64_t kparam);
ray_t* agg_run_one_bin(const agg_vtable_t* vt, ray_t* x_col, ray_t* y_col,
                       const uint32_t* gids, int64_t nrows, int64_t ngroups,
                       int64_t kparam);

/* ── Dense grouping eligibility selector (compact-range int/SYM keys) ──
 * When dense applies, a group id is the packed key offset (O(1) direct index)
 * rather than a hash slot. */

typedef struct {
    bool     ok;
    uint32_t n_keys;        /* mirrors ext->n_keys' width; value stays 1..16 (dense self-limit) */
    bool nullable[16];
    int64_t nulls[16];
    int64_t  mins[16];      /* [16]: dense direct-index routing self-limits to <=16 keys (agg_dense_plan) */
    int64_t  ranges[16];    /* [16]: dense direct-index routing self-limits to <=16 keys (agg_dense_plan) */
    int64_t  strides[16];   /* [16]: dense self-limit <=16; composite packing: slot = sum_k (key_k - min_k)*strides[k] */
    int64_t  total_slots;   /* product of ranges */
    /* Compacted keys (composite plans whose raw range product overflowed):
     * remap[k][code - mins[k]] is the dense component of a code that occurs
     * in the input, inverse[k][component] the original code.  NULL for keys
     * that use their raw range.  Owned by the plan: agg_dense_plan_free. */
    int32_t* remap[16];
    int64_t* inverse[16];
    bool     compacted;     /* any remap set: hot loops select the remap form once */
    bool     wide;          /* one key whose range exceeds the row count: only the shared strategy may run it (agg_dense_wide_slots) */
} dense_plan_t;

/* Release a plan's compaction tables (no-op for raw-range plans). */
void agg_dense_plan_free(dense_plan_t* dp);

/* Decide if dense grouping applies to (key_cols, aggs).  Eligible iff:
 *  - every key type in {I64,I32,I16,U8,BOOL,DATE,TIME,TIMESTAMP,SYM} with a dedicated slot for nullable keys
 *  - product of per-key ranges is no larger than the contributing row count
 *    (so dense state is O(input), never controlled by a machine-size budget)
 * Does one min/max prescan over the key columns.  Sets out->ok accordingly.
 * n_keys is uint32_t (untruncated ext->n_keys): dense direct-index routing
 * self-limits to <=16 keys here (wider shapes are rejected to v2's unbounded
 * hash/radix path), so the fixed [16] mins/ranges/strides are only ever read
 * at <=16.  n_aggs is unused by dense eligibility (kept uint32_t so a widened
 * ext->n_aggs never narrows at this call). */
bool agg_dense_plan(ray_t** key_cols, uint32_t n_keys,
                    const agg_vtable_t** vts, uint32_t n_aggs,
                    int64_t nrows, dense_plan_t* out);

/* Cardinality estimates for a planner choosing between strategies before
 * any grouping runs (try_count_distinct_v2_rewrite), from one strided
 * sample of 32,768 rows over `cols` (every row below that) — over the
 * `nrows` row indices of `rows` when that is given (a selection's
 * survivors), else over rows 0..nrows.  When `rows` is itself a sample of
 * a larger population (the survivors of a predicate run over sampled rows),
 * `population` is that population's size — the estimates and the caps
 * scale to it — else 0.  A column's
 * distinct values in the sample are raised by the Chao1 estimator from the
 * values seen once and twice, so a key the sample cannot exhaust — a
 * million values, or one value on most rows and a million on the rest — is
 * estimated near its size, while a key of 45 values stays 45.  The
 * estimate is capped at the span of the sampled values — the codes of a
 * SYM key, fixed once interned, where the process-wide domain's count
 * would move the estimate with every symbol interned afterwards.
 * col_est[k] (when given) receives column k's estimate;
 * prefix_est the estimate for the tuple of the first n_prefix columns (the
 * groups a composite key forms, which the product of its columns' estimates
 * overstates when one column follows from another); prefix_max the rows of
 * the largest such group (its share of the sample over the input: a
 * per-group pass is serial over it); all_est the estimate
 * for the tuple of all n columns (the tuples a (key, value) grouping would
 * keep).  A tuple is capped at the product of its columns' caps and at
 * nrows; a sorted prefix, which a stride reads as all distinct, is read
 * through blocks of adjacent rows as well.  Estimates, never bounds: a
 * planner weighs them against thresholds with slack.  false — with the
 * outputs unset — when a column is not a dense key type (integer, temporal,
 * SYM), n is 0 or above 16, the input is empty, or memory runs out. */
bool agg_group_card_sample(ray_t** cols, uint32_t n, uint32_t n_prefix,
                           const int64_t* rows, int64_t nrows, int64_t population,
                           int64_t* col_est, int64_t* prefix_est, int64_t* prefix_max,
                           int64_t* all_est);

/* The rows the estimator samples (every row below that). */
#define AGG_EST_SAMPLE_ROWS (1 << 15)

/* The row the estimator samples from window i of the `sample` equal ones
 * the rows are cut into (nrows / sample rows each, fractional): an offset
 * into the window hashed from i by the splitmix64 finaliser.  A fixed
 * offset reads a key whose period divides the stride — five values
 * cycling under a stride of 305 — as one value.  An offset nearly linear
 * in i, the high bits of i times the golden ratio, keeps the sampled rows
 * an arithmetic progression and a key with a period near the stride at a
 * nearly fixed phase: a composite of (% i 1000) and (% i 1009) at 10 M
 * rows read as ten million tuples under it, one of 1000 and 1013 as a
 * tenth of its groups.  The finaliser's offsets make the collisions those
 * of a random sample.  Shared with a planner sampling rows for a predicate
 * (query.c). */
static inline int64_t agg_est_sample_row(int64_t i, int64_t sample, int64_t nrows) {
    /* window i of `sample` equal ones over the rows, [lo, hi): a whole
     * stride would leave the rows past sample * (nrows / sample) unread,
     * the last fifth of 41,943 rows under a sample of 32,768 */
    const int64_t lo = (int64_t)((double)i * (double)nrows / (double)sample);
    int64_t hi = (int64_t)((double)(i + 1) * (double)nrows / (double)sample);
    if (hi > nrows) hi = nrows;
    const int64_t w = hi - lo;
    if (w <= 1) return lo;
    uint64_t h = (uint64_t)i * 0x9E3779B97F4A7C15ULL;
    h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ULL;
    h = (h ^ (h >> 27)) * 0x94D049BB133111EBULL;
    h ^= h >> 31;
    return lo + (int64_t)(h % (uint64_t)w);
}

/* The product of the per-key estimates above for key_cols: the packed key
 * space a direct-index grouping would hold.  -1 when a key is not a dense
 * key type, the input is empty, or the product overflows. */
int64_t agg_group_card_estimate(ray_t** key_cols, uint32_t n_keys, int64_t nrows);

/* Result column name for a plain-column-input aggregate: input column name
 * (ray_sym_str of in_sym) + per-op suffix (_sum/_count/_mean/_min/_max/...),
 * interned.  Falls back to in_sym on buffer overflow.  Behavior-identical to
 * emit_agg_columns' inline naming for the OP_SCAN input case. */
int64_t agg_result_col_name(int64_t in_sym, uint16_t agg_op);

#endif /* RAY_OPS_AGG_ENGINE_H */
