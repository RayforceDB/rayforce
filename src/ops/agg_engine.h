/* src/ops/agg_engine.h — v2 composable aggregation engine entry (design §3). */
#ifndef RAY_OPS_AGG_ENGINE_H
#define RAY_OPS_AGG_ENGINE_H

#include <rayforce.h>
#include "ops/internal.h"   /* ray_graph_t, ray_op_t, ray_op_ext_t, find_ext */
#include "ops/agg_acc.h"    /* agg_vtable_t */

/* Test/feature knob: route OP_GROUP through the v2 engine when it can handle
 * the query (see agg_v2_can_handle). Default false → zero behavioral change. */
extern bool ray_agg_engine_v2;

/* True iff the v2 engine fully supports this group node over this table.
 * Conservative: any uncertainty → false → caller uses the existing engine. */
bool agg_v2_can_handle(ray_graph_t* g, ray_op_t* op, ray_t* tbl);

/* Precondition: agg_v2_can_handle(g, op, tbl) returned true. */
ray_t* exec_group_v2(ray_graph_t* g, ray_op_t* op, ray_t* tbl);

/* Dense group assignment over 1..16 key columns, first-occurrence order. */
typedef struct {
    uint32_t* gids;       /* len = nrows */
    int64_t*  first_row;  /* len = ngroups; row index where each group first appeared */
    int64_t   ngroups;
} agg_groups_t;

/* Multi-key grouping (1..16 keys). Reads each key as an int64 (intern id for
 * SYM) and hashes the tuple. Assigns gids incrementally on first sight → gid
 * order == first-occurrence order; first_row[gid] records the row where the
 * group first appeared. Returns 0 on success (caller releases out via
 * agg_groups_free()), -1 on allocation failure.
 * n_keys stays uint8_t: the interface admits up to 255 (agg_select_distinct's
 * own UINT8_MAX gate in query.c), but the LIVE bound today is 1..16 for both
 * callers — the GROUP path via agg_v2_can_handle's admission, and the
 * keys-only DISTINCT path via the by-dict's own 1..16 cap in query.c (~5203,
 * "by-dict must have 1..16 keys"), which dominates before nk ever reaches
 * agg_select_distinct. Cut-3 is the boundary where either gate may lift past
 * 16, at which point the fixed data[16] inside agg_group_keys must become a
 * carve. */
int agg_group_keys(ray_t** key_cols, uint8_t n_keys, int64_t nrows, agg_groups_t* out);

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
 * nk stays uint8_t: query.c's only caller gates `nk > UINT8_MAX` before the
 * call (its own MIGRATION GATE — falls through to the composite path beyond
 * 255 keys), so this parameter never sees a truncated value. */
ray_t* agg_select_distinct(ray_t* tbl, ray_t** key_cols, const int64_t* key_syms,
                           uint8_t nk, int64_t nrows,
                           const int64_t* keep_syms, int keep_n);

/* Build a dense SoA per-group state array for one aggregate (vt), run a single
 * update_batch over val_col grouped by gids, and finalize each group into a
 * typed result column of vt->out_type, length ngroups. For COUNT, val_col is
 * NULL. Caller owns the returned column (ray_release). Returns a ray_error atom
 * on allocation failure. Single-threaded (Phase 1a). */
ray_t* agg_run_one(const agg_vtable_t* vt, ray_t* val_col,
                   const uint32_t* gids, int64_t nrows, int64_t ngroups,
                   int64_t kparam);

/* ── Dense grouping eligibility selector (low-cardinality int/SYM keys) ──
 * Mirrors the old direct-array group path's cap.  When dense applies, a group
 * id is the packed key offset (O(1) direct index) rather than a hash slot. */
#define DENSE_MAX_SLOTS 262144   /* mirror the old DA path's cap */

typedef struct {
    bool     ok;
    uint32_t n_keys;        /* mirrors ext->n_keys' width; value stays 1..16 */
    int64_t  mins[16];      /* [16]: protected by agg_v2_can_handle's 1..16-key admission (cut-3 boundary) */
    int64_t  ranges[16];    /* [16]: protected by agg_v2_can_handle's 1..16-key admission (cut-3 boundary) */
    int64_t  strides[16];   /* [16]: protected by agg_v2_can_handle's 1..16-key admission (cut-3 boundary); composite packing: slot = sum_k (key_k - min_k)*strides[k] */
    int64_t  total_slots;   /* product of ranges */
} dense_plan_t;

/* Decide if dense grouping applies to (key_cols, aggs).  Eligible iff:
 *  - every key type in {I64,I32,I16,U8,BOOL,DATE,TIME,TIMESTAMP,SYM} and NOT HAS_NULLS
 *  - every agg vtable is ACC_STREAMING (no buffered median/top)
 *  - product of per-key ranges <= DENSE_MAX_SLOTS (no overflow)
 * Does one min/max prescan over the key columns.  Sets out->ok accordingly.
 * n_keys stays uint8_t: only ever called with ext->n_keys post agg_v2_can_handle's
 * 1..16 admission (dense direct-index routing bound; wider shapes take the hash
 * path) — see agg_v2_can_handle's comment.  n_aggs is unused by dense eligibility
 * (kept uint32_t so a widened ext->n_aggs never narrows at this call). */
bool agg_dense_plan(ray_t** key_cols, uint8_t n_keys,
                    const agg_vtable_t** vts, uint32_t n_aggs,
                    int64_t nrows, dense_plan_t* out);

/* Result column name for a plain-column-input aggregate: input column name
 * (ray_sym_str of in_sym) + per-op suffix (_sum/_count/_mean/_min/_max/...),
 * interned.  Falls back to in_sym on buffer overflow.  Behavior-identical to
 * emit_agg_columns' inline naming for the OP_SCAN input case. */
int64_t agg_result_col_name(int64_t in_sym, uint16_t agg_op);

#endif /* RAY_OPS_AGG_ENGINE_H */
