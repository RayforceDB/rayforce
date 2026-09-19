/* src/ops/agg_engine.h — v2 composable aggregation engine entry (design §3). */
#ifndef RAY_OPS_AGG_ENGINE_H
#define RAY_OPS_AGG_ENGINE_H

#include <rayforce.h>
#include "ops/internal.h"   /* ray_graph_t, ray_op_t, ray_op_ext_t, find_ext */
#include "ops/agg_acc.h"    /* agg_vtable_t */

/* Test/feature knob: route OP_GROUP through the v2 engine when it can handle
 * the query (see agg_v2_can_handle). Enabled by default. */
extern bool ray_agg_engine_v2;

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
    AGG_V2_EMIT_FILTER,
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
    bool topn_native;               /* last v2 run selected the emit filter's top-N itself */
} agg_route_stats_t;
void agg_route_reset(void);
void agg_route_note_key_domain(void);
agg_route_stats_t agg_route_stats(void);
void agg_route_record(agg_route_t route);
void agg_route_reason(agg_v2_reason_t reason);

/* True iff the v2 engine fully supports this group node over this table.
 * Conservative: any uncertainty → false → caller uses the existing engine. */
bool agg_v2_can_handle(ray_graph_t* g, ray_op_t* op, ray_t* tbl);

/* True when v2 would group this node through a bounded dense plan (see the
 * definition for the strategy-prediction contract). */
bool agg_v2_dense_plan_available(ray_graph_t* g, ray_op_t* op, ray_t* tbl);

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
 * states + (slots ? slots[i] : i) * stride + off.  Nulls (and NaN) sink to
 * the far end of the keep direction (`desc`) so they never enter a top-N.
 * Returns false for an out_type without a scalar order (LIST, STR, ...). */
bool agg_group_values_f64(const agg_vtable_t* vt, const char* states,
                          size_t stride, size_t off, const int64_t* slots,
                          int64_t n, int64_t param, uint8_t desc, double* out);

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

/* Result column name for a plain-column-input aggregate: input column name
 * (ray_sym_str of in_sym) + per-op suffix (_sum/_count/_mean/_min/_max/...),
 * interned.  Falls back to in_sym on buffer overflow.  Behavior-identical to
 * emit_agg_columns' inline naming for the OP_SCAN input case. */
int64_t agg_result_col_name(int64_t in_sym, uint16_t agg_op);

#endif /* RAY_OPS_AGG_ENGINE_H */
